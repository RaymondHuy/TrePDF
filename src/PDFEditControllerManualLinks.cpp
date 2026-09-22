#include "PDFEditController.h"
#include "PDFDocument.h"
#include "PDFFormController.h"

#include <QCoreApplication>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QUuid>
#include <QtTranslation>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
QThreadPool &manualLinkPool()
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

bool autoRegion(const QString &id, int &index, int &region)
{
    const QStringList parts = id.split(QLatin1Char(':'));
    if (parts.size() != 3 || parts.at(0) != QLatin1String("auto"))
        return false;
    bool indexValid = false;
    bool regionValid = false;
    index = parts.at(1).toInt(&indexValid);
    region = parts.at(2).toInt(&regionValid);
    return indexValid && regionValid && index >= 0 && region >= 0;
}
}

bool PDFEditController::beginLinkEditing()
{
    if (!m_document || !m_document->isLoaded() || m_saving || m_webLinkCancellation)
        return false;
    if (!documentAllowsAnnotations()) {
        //% "This PDF does not permit changing annotations."
        emit statusMessage(qtTrId("PdfAnnotationsNotPermitted"));
        return false;
    }
    finishSelectedFrameResize();
    finishSelectedTextEdit();
    setTool(LinkTool);
    if (m_manualSourceLinksReady || m_manualLinkCancellation)
        return true;
    const QString path = m_document->renderFilePath();
    const QByteArray password = m_document->documentPassword();
    const int revision = m_document->renderRevision();
    const QPointer<PDFDocument> document = m_document;
    const QPointer<PDFEditController> controller(this);
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_manualLinkCancellation = cancellation;
    auto *work = QRunnable::create([controller, document, path, password, revision, cancellation] {
        auto result = PDFWebLinks::scanEditableLinks(path, cancellation, password);
        if (cancellation->load(std::memory_order_relaxed))
            return;
        QMetaObject::invokeMethod(QCoreApplication::instance(),
            [controller, document, path, revision, cancellation,
             result = std::move(result)]() mutable {
                if (!controller || controller->m_manualLinkCancellation != cancellation
                    || cancellation->load(std::memory_order_relaxed))
                    return;
                controller->m_manualLinkCancellation.reset();
                emit controller->editableLinksChanged();
                if (!document || controller->m_document != document
                    || document->renderRevision() != revision
                    || document->renderFilePath() != path)
                    return;
                if (result.failed) {
                    //% "The PDF links could not be loaded. Select Add or edit a link to try again."
                    emit controller->statusMessage(qtTrId("EditableLinkScanFailed"));
                    return;
                }
                controller->m_sourceManualLinks = std::move(result.links);
                controller->m_manualSourceLinksReady = true;
                controller->invalidateWebLinks();
                if (result.limited) {
                    //% "Some links are read-only or exceed the scan limits. Drag a rectangle to create a link."
                    emit controller->statusMessage(qtTrId("EditableLinkScanLimited"));
                } else {
                    //% "Drag a rectangle to create a link. Double-click an existing link to edit it."
                    emit controller->statusMessage(qtTrId("LinkEditingHint"));
                }
            }, Qt::QueuedConnection);
    });
    if (!manualLinkPool().tryStart(work)) {
        delete work;
        m_manualLinkCancellation.reset();
        //% "Another link scan is finishing. Please try again."
        emit statusMessage(qtTrId("WebLinkScannerBusy"));
        return false;
    }
    emit editableLinksChanged();
    return true;
}

bool PDFEditController::resolveManualLink(int page, const QString &id,
                                          PDFWebLinks::ManualLink &link) const
{
    if (!m_document || !m_document->isLoaded() || id.isEmpty())
        return false;
    const int sourcePage = m_document->sourcePageIndex(page);
    if (sourcePage < 0)
        return false;
    for (const auto &draft : m_manualLinks) {
        if (draft.id == id) {
            if (draft.sourcePage != sourcePage || draft.deleted)
                return false;
            link = draft;
            return true;
        }
    }
    for (const auto &source : m_sourceManualLinks) {
        if (source.id != id || source.sourcePage != sourcePage)
            continue;
        const bool removed = std::any_of(m_removedWebLinks.cbegin(), m_removedWebLinks.cend(),
            [&](const auto &item) {
                return item.sourcePage == sourcePage && item.annotationIndex == source.annotationIndex;
            });
        if (removed)
            return false;
        link = source;
        return true;
    }
    int index = -1;
    int region = -1;
    if (!autoRegion(id, index, region) || index >= m_autoWebLinks.size())
        return false;
    const auto &automatic = m_autoWebLinks.at(index);
    if (automatic.sourcePage != sourcePage || region >= automatic.rects.size())
        return false;
    link = {};
    link.id = id;
    link.sourcePage = sourcePage;
    link.rect = automatic.rects.at(region);
    link.url = automatic.url;
    return true;
}

QVariantMap PDFEditController::linkProperties(int page, const QString &id) const
{
    PDFWebLinks::ManualLink link;
    if (!resolveManualLink(page, id, link))
        return {};
    const bool automatic = id.startsWith(QLatin1String("auto:"));
    const bool draft = automatic || std::any_of(m_manualLinks.cbegin(), m_manualLinks.cend(),
        [&](const auto &item) { return item.id == id; });
    return manualLinkProperties(link, draft, automatic);
}

QVariantMap PDFEditController::manualLinkProperties(const PDFWebLinks::ManualLink &link,
                                                   bool draft, bool automatic) const
{
    return {
        {QStringLiteral("id"), link.id},
        {QStringLiteral("rect"), link.rect},
        {QStringLiteral("url"), link.url},
        {QStringLiteral("targetPage"), link.targetSourcePage >= 0
             ? m_document->displayPageIndexForSourcePage(link.targetSourcePage) : -1},
        {QStringLiteral("visible"), link.visible},
        {QStringLiteral("color"), link.color},
        {QStringLiteral("width"), link.width},
        {QStringLiteral("lineStyle"), link.lineStyle},
        {QStringLiteral("highlight"), link.highlight},
        {QStringLiteral("editable"), link.editable},
        {QStringLiteral("draft"), draft},
        {QStringLiteral("automatic"), automatic},
        {QStringLiteral("revision"), QVariant::fromValue(m_webLinksRevision)}
    };
}

QVariantList PDFEditController::editableLinksForPage(int page) const
{
    QVariantList items;
    if (!m_document || !m_document->isLoaded())
        return items;
    const int sourcePage = m_document->sourcePageIndex(page);
    if (sourcePage < 0)
        return items;
    QHash<QString, const PDFWebLinks::ManualLink *> drafts;
    QSet<int> removed;
    for (const auto &link : m_manualLinks)
        if (link.sourcePage == sourcePage)
            drafts.insert(link.id, &link);
    for (const auto &link : m_removedWebLinks)
        if (link.sourcePage == sourcePage)
            removed.insert(link.annotationIndex);
    const auto append = [&](const PDFWebLinks::ManualLink &link, bool draft, bool automatic) {
        if (!link.deleted && items.size() < PDFWebLinks::MaximumAnnotationsPerPage)
            items.append(manualLinkProperties(link, draft, automatic));
    };
    for (const auto &source : m_sourceManualLinks) {
        if (source.sourcePage != sourcePage || removed.contains(source.annotationIndex))
            continue;
        const auto draft = drafts.constFind(source.id);
        if (draft == drafts.cend())
            append(source, false, false);
        else
            append(**draft, true, false);
    }
    for (const auto &draft : m_manualLinks)
        if (draft.sourcePage == sourcePage && draft.annotationIndex < 0)
            append(draft, true, false);
    for (qsizetype index = 0; index < m_autoWebLinks.size(); ++index) {
        const auto &automatic = m_autoWebLinks.at(index);
        if (automatic.sourcePage != sourcePage)
            continue;
        for (qsizetype region = 0; region < automatic.rects.size(); ++region) {
            PDFWebLinks::ManualLink link;
            link.id = QStringLiteral("auto:%1:%2").arg(index).arg(region);
            link.sourcePage = sourcePage;
            link.url = automatic.url;
            link.rect = automatic.rects.at(region);
            append(link, true, true);
        }
    }
    return items;
}

bool PDFEditController::validateManualLink(int page, const QRectF &rect,
                                          const QVariantMap &properties,
                                          PDFWebLinks::ManualLink &link) const
{
    if (!m_document || !m_document->isLoaded() || m_saving || m_tool != LinkTool
        || loadingEditableLinks() || m_webLinkCancellation || !documentAllowsAnnotations()
        || !std::isfinite(rect.x()) || !std::isfinite(rect.y())
        || !std::isfinite(rect.width()) || !std::isfinite(rect.height())
        || rect.isEmpty() || rect.left() < 0 || rect.top() < 0
        || rect.right() > 1 || rect.bottom() > 1)
        return false;
    const int sourcePage = m_document->sourcePageIndex(page);
    if (sourcePage < 0)
        return false;
    const QSizeF size = m_document->pageSize(page);
    if (rect.width() * size.width() < 1.0 || rect.height() * size.height() < 1.0)
        return false;
    const int targetPage = properties.value(QStringLiteral("targetPage"), -1).toInt();
    if (targetPage < -1 || targetPage >= m_document->pageCount())
        return false;
    link.sourcePage = sourcePage;
    link.rect = rect;
    link.targetSourcePage = targetPage >= 0 ? m_document->sourcePageIndex(targetPage) : -1;
    if (targetPage >= 0 && link.targetSourcePage < 0)
        return false;
    link.url.clear();
    if (targetPage < 0) {
        QString url = properties.value(QStringLiteral("url")).toString().trimmed();
        if (url.startsWith(QLatin1String("www."), Qt::CaseInsensitive))
            url.prepend(QStringLiteral("https://"));
        link.url = PDFWebLinks::validWebUrl(url);
        if (link.url.isEmpty())
            return false;
    }
    link.visible = properties.value(QStringLiteral("visible"), false).toBool();
    link.color = properties.value(QStringLiteral("color"), QColor(Qt::black)).value<QColor>();
    link.width = properties.value(QStringLiteral("width"), 1.0).toDouble();
    link.lineStyle = properties.value(QStringLiteral("lineStyle"), 0).toInt();
    link.highlight = properties.value(QStringLiteral("highlight"), 1).toInt();
    return link.color.isValid() && link.color.alpha() == 255
        && std::isfinite(link.width) && link.width >= 0.5 && link.width <= 12
        && link.lineStyle >= 0 && link.lineStyle <= 2
        && link.highlight >= 0 && link.highlight <= 3;
}

PDFEditController::Command PDFEditController::captureLinkCommand() const
{
    Command command;
    command.kind = Command::Kind::WebLinks;
    command.webLinksBefore = command.webLinksAfter = m_autoWebLinks;
    command.removedWebLinksBefore = command.removedWebLinksAfter = m_removedWebLinks;
    command.manualLinksBefore = command.manualLinksAfter = m_manualLinks;
    return command;
}

void PDFEditController::commitLinkCommand(Command command)
{
    command.webLinksAfter = m_autoWebLinks;
    command.removedWebLinksAfter = m_removedWebLinks;
    command.manualLinksAfter = m_manualLinks;
    if (command.webLinksBefore == command.webLinksAfter
        && command.removedWebLinksBefore == command.removedWebLinksAfter
        && command.manualLinksBefore == command.manualLinksAfter)
        return;
    m_undoStack.append(std::move(command));
    m_redoStack.clear();
    invalidateWebLinks();
    emit historyStateChanged();
}

bool PDFEditController::removeAutoLinkRegion(const QString &id)
{
    int index = -1;
    int region = -1;
    if (!autoRegion(id, index, region) || index >= m_autoWebLinks.size()
        || region >= m_autoWebLinks.at(index).rects.size())
        return false;
    m_autoWebLinks[index].rects.removeAt(region);
    if (m_autoWebLinks.at(index).rects.isEmpty())
        m_autoWebLinks.removeAt(index);
    return true;
}

QString PDFEditController::addLink(int page, const QRectF &rect, const QVariantMap &properties)
{
    PDFWebLinks::ManualLink link;
    if (m_manualLinks.size() + m_autoWebLinks.size() >= PDFWebLinks::MaximumLinks
        || !validateManualLink(page, rect, properties, link))
        return {};
    link.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Command command = captureLinkCommand();
    m_manualLinks.append(link);
    commitLinkCommand(std::move(command));
    return link.id;
}

bool PDFEditController::updateLink(int page, const QString &id, const QRectF &rect,
                                   const QVariantMap &properties)
{
    PDFWebLinks::ManualLink original;
    if (!resolveManualLink(page, id, original) || !original.editable)
        return false;
    PDFWebLinks::ManualLink changed = original;
    if (!validateManualLink(page, rect, properties, changed))
        return false;
    if (changed == original)
        return true;
    Command command = captureLinkCommand();
    if (removeAutoLinkRegion(id))
        changed.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto existing = std::find_if(m_manualLinks.begin(), m_manualLinks.end(),
        [&](const auto &item) { return item.id == id; });
    const auto source = std::find_if(m_sourceManualLinks.cbegin(), m_sourceManualLinks.cend(),
        [&](const auto &item) { return item.id == id; });
    if (source != m_sourceManualLinks.cend() && changed == *source) {
        if (existing != m_manualLinks.end())
            m_manualLinks.erase(existing);
    } else if (existing != m_manualLinks.end()) {
        *existing = changed;
    } else {
        if (m_manualLinks.size() >= PDFWebLinks::MaximumLinks) {
            m_autoWebLinks = command.webLinksBefore;
            return false;
        }
        m_manualLinks.append(changed);
    }
    commitLinkCommand(std::move(command));
    return true;
}

bool PDFEditController::deleteLink(int page, const QString &id)
{
    PDFWebLinks::ManualLink link;
    if (m_saving || m_tool != LinkTool || loadingEditableLinks() || m_webLinkCancellation
        || !documentAllowsAnnotations() || !resolveManualLink(page, id, link) || !link.editable)
        return false;
    Command command = captureLinkCommand();
    if (!removeAutoLinkRegion(id)) {
        auto existing = std::find_if(m_manualLinks.begin(), m_manualLinks.end(),
            [&](const auto &item) { return item.id == id; });
        if (link.annotationIndex < 0) {
            if (existing == m_manualLinks.end())
                return false;
            m_manualLinks.erase(existing);
        } else {
            link.deleted = true;
            if (existing == m_manualLinks.end())
                m_manualLinks.append(link);
            else
                *existing = link;
        }
    }
    commitLinkCommand(std::move(command));
    return true;
}

bool PDFEditController::activateManualLink(int page, const QString &id, quint64 revision)
{
    PDFWebLinks::ManualLink link;
    if (m_saving || m_tool != BrowseTool || revision != m_webLinksRevision
        || !resolveManualLink(page, id, link) || !m_formController
        || m_formController->document() != m_document)
        return false;
    if (link.targetSourcePage >= 0) {
        const int target = m_document->displayPageIndexForSourcePage(link.targetSourcePage);
        if (target < 0)
            return false;
        emit m_formController->pageNavigationRequested(target);
        m_document->setCurrentPage(target);
        return true;
    }
    const quint64 request = m_formController->requestExternalWebLink(link.url);
    if (!request)
        return false;
    m_pendingWebLinkRequest = request;
    return true;
}
