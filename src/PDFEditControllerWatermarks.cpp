#include "PDFEditController.h"
#include "PDFMemoryTelemetry.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QRunnable>
#include <QRegularExpression>
#include <QThreadPool>
#include <QUuid>
#include <QtTranslation>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
QThreadPool &watermarkPool()
{
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

QString imageUrl(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

QVariantMap settingsFor(const PDFWatermarks::Spec &spec)
{
    return {{QStringLiteral("id"), spec.id},
        {QStringLiteral("imageMode"), spec.imageMode},
        {QStringLiteral("text"), spec.text},
        {QStringLiteral("fontFamily"), spec.fontFamily},
        {QStringLiteral("fontSize"), spec.fontSize},
        {QStringLiteral("color"), spec.color.name(QColor::HexRgb)},
        {QStringLiteral("opacity"), spec.opacity},
        {QStringLiteral("rotation"), spec.rotation},
        {QStringLiteral("scale"), spec.scale},
        {QStringLiteral("x"), spec.x}, {QStringLiteral("y"), spec.y},
        {QStringLiteral("behind"), spec.behind}};
}

bool sameSpec(const PDFWatermarks::Spec &left, const PDFWatermarks::Spec &right)
{
    return settingsFor(left) == settingsFor(right) && left.image == right.image;
}

bool parsePages(const QString &text, const PDFDocument &document, QSet<int> &pages)
{
    const int count = document.pageCount();
    if (count < 1 || count > PDFWatermarks::MaximumPages || text.size() > 4096)
        return false;
    const auto add = [&](int page) {
        const int source = document.sourcePageIndex(page - 1);
        if (source < 0)
            return false;
        pages.insert(source);
        return true;
    };
    if (text.trimmed().isEmpty() || text.trimmed().compare(QLatin1String("all"), Qt::CaseInsensitive) == 0) {
        for (int page = 1; page <= count; ++page)
            if (!add(page))
                return false;
        return true;
    }
    static const QRegularExpression range(QStringLiteral("^\\s*([0-9]+)\\s*(?:-\\s*([0-9]+)\\s*)?$"));
    const QStringList entries = text.split(QLatin1Char(','));
    for (const QString &entry : entries) {
        const auto match = range.match(entry);
        if (!match.hasMatch())
            return false;
        bool firstOk = false, lastOk = true;
        const int first = match.captured(1).toInt(&firstOk);
        const int last = match.captured(2).isEmpty() ? first : match.captured(2).toInt(&lastOk);
        if (!firstOk || !lastOk || first < 1 || first > last || last > count)
            return false;
        for (int page = first; page <= last; ++page)
            if (!add(page))
                return false;
    }
    return !pages.isEmpty();
}
}

void PDFEditController::resetWatermarks()
{
    if (m_watermarkCancellation)
        m_watermarkCancellation->store(true, std::memory_order_relaxed);
    m_watermarkCancellation.reset();
    m_watermarksReady = false;
    m_sourceWatermarks.clear();
    m_watermarkChanges.clear();
    m_watermarkPreviewPages.clear();
    m_watermarkDialogImage = {};
    m_watermarkImageToken.clear();
    emit watermarksChanged();
}

void PDFEditController::loadWatermarks()
{
    if (!m_document || !m_document->isLoaded() || m_saving || m_watermarksReady || m_watermarkCancellation)
        return;
    const QString path = m_document->renderFilePath();
    const QByteArray password = m_document->documentPassword();
    const int revision = m_document->renderRevision();
    const QPointer<PDFDocument> document(m_document);
    const QPointer<PDFEditController> controller(this);
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_watermarkCancellation = cancellation;
    auto *work = QRunnable::create([path, password, revision, document, controller, cancellation] {
        auto result = PDFWatermarks::scan(path, password, cancellation);
        if (cancellation->load(std::memory_order_relaxed))
            return;
        QMetaObject::invokeMethod(QCoreApplication::instance(),
            [path, revision, document, controller, cancellation, result = std::move(result)]() mutable {
                if (!controller || controller->m_watermarkCancellation != cancellation)
                    return;
                controller->m_watermarkCancellation.reset();
                if (!document || controller->m_document != document
                    || document->renderRevision() != revision || document->renderFilePath() != path) {
                    emit controller->watermarksChanged();
                    return;
                }
                if (!result.succeeded) {
                    //% "Watermarks could not be read safely. Reopen the PDF and try again."
                    emit controller->statusMessage(qtTrId("WatermarkScanFailed"));
                } else {
                    controller->m_sourceWatermarks = std::move(result.groups);
                    controller->m_watermarksReady = true;
                }
                emit controller->watermarksChanged();
            }, Qt::QueuedConnection);
    });
    if (!watermarkPool().tryStart(work)) {
        delete work;
        m_watermarkCancellation.reset();
        //% "Another watermark scan is finishing. Please try again."
        emit statusMessage(qtTrId("WatermarkScannerBusy"));
    }
    emit watermarksChanged();
}

QVector<PDFWatermarks::Group> PDFEditController::effectiveWatermarks() const
{
    auto groups = m_sourceWatermarks;
    for (const auto &change : m_watermarkChanges) {
        auto found = std::find_if(groups.begin(), groups.end(), [&](const auto &group) {
            return group.spec.id == change.spec.id;
        });
        if (found != groups.end())
            groups.erase(found);
        if (!change.removed)
            groups.append({change.spec, change.pages, change.originals});
    }
    return groups;
}

QVariantList PDFEditController::watermarks() const
{
    QVariantList result;
    if (!m_document)
        return result;
    for (const auto &group : effectiveWatermarks()) {
        auto value = settingsFor(group.spec);
        QList<int> pageNumbers;
        for (int sourcePage : group.pages) {
            const int page = m_document->displayPageIndexForSourcePage(sourcePage);
            if (page >= 0)
                pageNumbers.append(page + 1);
        }
        if (pageNumbers.isEmpty())
            continue;
        std::sort(pageNumbers.begin(), pageNumbers.end());
        QStringList ranges;
        for (qsizetype start = 0; start < pageNumbers.size();) {
            qsizetype end = start;
            while (end + 1 < pageNumbers.size() && pageNumbers[end + 1] == pageNumbers[end] + 1)
                ++end;
            ranges.append(end == start ? QString::number(pageNumbers[start])
                : QStringLiteral("%1-%2").arg(pageNumbers[start]).arg(pageNumbers[end]));
            start = end + 1;
        }
        value.insert(QStringLiteral("pages"), ranges.join(QLatin1Char(',')));
        //% "Image watermark"
        value.insert(QStringLiteral("label"), group.spec.imageMode
            ? qtTrId("ImageWatermarkLabel") : group.spec.text.left(60));
        result.append(value);
    }
    return result;
}

bool PDFEditController::canChangeWatermarks()
{
    if (!m_document || !m_document->isLoaded() || m_saving || !m_watermarksReady)
        return false;
    if (!m_document->m_allowsContentEditing) {
        //% "This PDF does not permit changing watermarks."
        emit statusMessage(qtTrId("WatermarkEditingNotPermitted"));
        return false;
    }
    finishSelectedFrameResize();
    finishSelectedTextEdit();
    if (!m_drafts.isEmpty() || !m_pendingTextMarkupCreations.isEmpty()
        || !m_pageInsertions.isEmpty() || !m_deletedSourcePages.isEmpty()) {
        //% "Save or discard other page edits before changing watermarks."
        emit statusMessage(qtTrId("SaveBeforeWatermark"));
        return false;
    }
    return true;
}

bool PDFEditController::prepareWatermarkSpec(const QVariantMap &settings, const QString &id,
                                            PDFWatermarks::Spec &spec) const
{
    if (!id.isEmpty()) {
        bool found = false;
        for (const auto &group : effectiveWatermarks()) {
            if (group.spec.id == id) {
                spec = group.spec;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    spec.id = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
    spec.imageMode = settings.value(QStringLiteral("imageMode"), spec.imageMode).toBool();
    spec.text = settings.value(QStringLiteral("text"), spec.text).toString();
    spec.fontFamily = settings.value(QStringLiteral("fontFamily"), spec.fontFamily).toString();
    const auto number = [&](const char *key, qreal fallback) {
        bool ok = false;
        const qreal value = settings.value(QLatin1String(key), fallback).toDouble(&ok);
        return ok ? value : std::numeric_limits<qreal>::quiet_NaN();
    };
    spec.fontSize = number("fontSize", spec.fontSize);
    spec.opacity = number("opacity", spec.opacity);
    spec.rotation = number("rotation", spec.rotation);
    spec.scale = number("scale", spec.scale);
    spec.x = number("x", spec.x);
    spec.y = number("y", spec.y);
    spec.behind = settings.value(QStringLiteral("behind"), spec.behind).toBool();
    spec.color = QColor(settings.value(QStringLiteral("color"), spec.color.name()).toString());
    const QString token = settings.value(QStringLiteral("imageToken")).toString();
    if (spec.imageMode && !token.isEmpty()) {
        if (token != m_watermarkImageToken)
            return false;
        spec.image = m_watermarkDialogImage;
    }
    QString error;
    if (!spec.imageMode) {
        spec.image = {};
        if (!PDFWatermarks::prepareText(spec, error))
            return false;
    }
    return PDFWatermarks::validSpec(spec);
}

bool PDFEditController::applyWatermark(const QVariantMap &settings, const QString &id)
{
    if (!canChangeWatermarks())
        return false;
    PDFWatermarks::Spec spec;
    QSet<int> pages;
    if (!prepareWatermarkSpec(settings, id, spec)) {
        //% "Check the watermark text, image, font and appearance settings."
        emit statusMessage(qtTrId("InvalidWatermarkSettings"));
        return false;
    }
    if (!parsePages(settings.value(QStringLiteral("pages"), QStringLiteral("all")).toString(), *m_document, pages)) {
        //% "Enter existing page numbers or ranges, for example 1-3,5. Save newly inserted pages first."
        emit statusMessage(qtTrId("InvalidWatermarkPages"));
        return false;
    }
    const auto groups = effectiveWatermarks();
    if (id.isEmpty() && groups.size() >= PDFWatermarks::MaximumGroups) {
        //% "A PDF can contain at most 32 managed watermarks."
        emit statusMessage(qtTrId("WatermarkGroupLimit"));
        return false;
    }
    for (const auto &group : groups)
        if (group.spec.id == id && group.pages == pages && sameSpec(group.spec, spec))
            return true;
    qint64 imageBytes = spec.image.sizeInBytes();
    for (const auto &group : groups)
        if (group.spec.id != id)
            imageBytes += group.spec.image.sizeInBytes();
    if (imageBytes > 64 * 1024 * 1024) {
        //% "The watermark images exceed the 64 MiB document limit."
        emit statusMessage(qtTrId("WatermarkImageBudget"));
        return false;
    }
    PDFWatermarks::Change change;
    change.spec = std::move(spec);
    change.pages = std::move(pages);
    for (const auto &group : m_sourceWatermarks)
        if (group.spec.id == id)
            change.originals = group.fingerprints;
    auto changes = m_watermarkChanges;
    changes.erase(std::remove_if(changes.begin(), changes.end(), [&](const auto &previous) {
        return previous.spec.id == change.spec.id;
    }), changes.end());
    bool matchesSource = false;
    for (const auto &group : m_sourceWatermarks)
        if (group.spec.id == id && group.pages == change.pages && sameSpec(group.spec, change.spec))
            matchesSource = true;
    if (!matchesSource)
        changes.append(std::move(change));
    clearSelection();
    setTool(BrowseTool);
    commitWatermarkChanges(std::move(changes));
    //% "Watermark changes are ready. Save the PDF to apply them."
    emit statusMessage(qtTrId("WatermarkChangesReady"));
    return true;
}

bool PDFEditController::removeWatermark(const QString &id)
{
    if (!canChangeWatermarks() || id.isEmpty())
        return false;
    const auto groups = effectiveWatermarks();
    const auto group = std::find_if(groups.cbegin(), groups.cend(), [&](const auto &value) { return value.spec.id == id; });
    if (group == groups.cend())
        return false;
    auto changes = m_watermarkChanges;
    changes.erase(std::remove_if(changes.begin(), changes.end(), [&](const auto &change) { return change.spec.id == id; }), changes.end());
    for (const auto &source : m_sourceWatermarks) {
        if (source.spec.id == id) {
            PDFWatermarks::Change removal;
            removal.spec = source.spec;
            removal.originals = source.fingerprints;
            removal.removed = true;
            changes.append(std::move(removal));
            break;
        }
    }
    clearSelection();
    setTool(BrowseTool);
    commitWatermarkChanges(std::move(changes));
    //% "Watermark changes are ready. Save the PDF to apply them."
    emit statusMessage(qtTrId("WatermarkChangesReady"));
    return true;
}

void PDFEditController::commitWatermarkChanges(QVector<PDFWatermarks::Change> changes)
{
    Command command;
    command.kind = Command::Kind::Watermarks;
    command.watermarksBefore = m_watermarkChanges;
    command.watermarksAfter = std::move(changes);
    m_watermarkChanges = command.watermarksAfter;
    m_undoStack.append(std::move(command));
    m_redoStack.clear();
    invalidateWatermarks();
    emit historyStateChanged();
}

void PDFEditController::invalidateWatermarks()
{
    QSet<int> pages;
    if (m_document) {
        for (const auto &change : m_watermarkChanges) {
            QSet<int> sources = change.pages;
            for (auto it = change.originals.cbegin(); it != change.originals.cend(); ++it)
                sources.insert(it.key());
            for (int source : sources) {
                const int page = m_document->displayPageIndexForSourcePage(source);
                if (page >= 0)
                    pages.insert(page);
            }
        }
        const QSet<int> affected = pages | m_watermarkPreviewPages;
        m_watermarkPreviewPages = pages;
        for (int page : affected)
            if (page < m_document->pageCount())
                markDraftStateChanged(page, true, false);
    }
    emit watermarksChanged();
}

QVector<PDFWatermarks::PageChange> PDFEditController::watermarkChangesForPage(int page) const
{
    QVector<PDFWatermarks::PageChange> result;
    if (!m_document)
        return result;
    const int source = m_document->sourcePageIndex(page);
    if (source < 0)
        return result;
    for (const auto &change : m_watermarkChanges) {
        const bool add = !change.removed && change.pages.contains(source);
        if (add || change.originals.contains(source))
            result.append({change.spec, change.originals.value(source), add});
    }
    return result;
}

QVariantMap PDFEditController::loadWatermarkImage(const QUrl &url)
{
    if (!url.isLocalFile() || !m_document || !m_document->isLoaded() || m_saving)
        return {};
    const QFileInfo file(url.toLocalFile());
    QImageReader reader(file.absoluteFilePath());
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!file.isFile() || file.size() > 64 * 1024 * 1024 || size.isEmpty()
        || size.width() > 8192 || size.height() > 8192
        || qint64(size.width()) * size.height() > 4 * 1024 * 1024) {
        //% "Choose an image smaller than 64 MiB and 4 megapixels."
        emit statusMessage(qtTrId("WatermarkImageTooLarge"));
        return {};
    }
    QImage image = reader.read().convertToFormat(QImage::Format_ARGB32);
    if (image.isNull() || image.sizeInBytes() > 16 * 1024 * 1024) {
        //% "The watermark image could not be loaded."
        emit statusMessage(qtTrId("WatermarkImageLoadFailed"));
        return {};
    }
    m_watermarkDialogImage = std::move(image);
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    PDFMemoryTelemetry::updateEditController(this);
#endif
    m_watermarkImageToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return {{QStringLiteral("token"), m_watermarkImageToken},
        {QStringLiteral("preview"), imageUrl(m_watermarkDialogImage.scaled(360, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation))}};
}

QString PDFEditController::watermarkPreview(const QVariantMap &settings, const QString &id)
{
    PDFWatermarks::Spec spec;
    if (!m_document || !prepareWatermarkSpec(settings, id, spec))
        return {};
    const QSizeF page = m_document->pageSize(m_document->currentPage());
    if (page.isEmpty())
        return {};
    QSize pixels = page.toSize();
    pixels.scale(360, 440, Qt::KeepAspectRatio);
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.scale(pixels.width() / page.width(), pixels.height() / page.height());
    PDFWatermarks::paint(painter, spec, page);
    painter.end();
    return imageUrl(image);
}
