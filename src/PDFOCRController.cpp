#include "PDFOCRController.h"

#include "PDFDocument.h"
#include "PDFOCRPipeline.h"
#include "OCRLanguageManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QRunnable>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTranslation>

#include <algorithm>
#include <atomic>

namespace {
constexpr int maximumSelectedPages = PDFOCRPipeline::maximumPages;
constexpr int maximumRangeCharacters = 4096;

QThreadPool &ocrPool()
{
    static QThreadPool pool;
    static const bool configured = []() {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

QString fileIdentity(const QFileInfo &info)
{
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

bool parsePages(const QString &input, int pageCount, QVector<int> &pages)
{
    if (pageCount <= 0 || input.size() > maximumRangeCharacters)
        return false;
    const QString range = input.trimmed();
    if (range.isEmpty()) {
        if (pageCount > maximumSelectedPages)
            return false;
        pages.reserve(pageCount);
        for (int page = 0; page < pageCount; ++page)
            pages.append(page);
        return true;
    }
    static const QRegularExpression itemPattern(
        QStringLiteral("^([1-9][0-9]*)\\s*(?:-\\s*([1-9][0-9]*))?$"));
    QSet<int> selected;
    const QStringList items = range.split(QLatin1Char(','), Qt::KeepEmptyParts);
    for (const QString &item : items) {
        const auto match = itemPattern.match(item.trimmed());
        if (!match.hasMatch())
            return false;
        bool firstValid = false;
        const int first = match.captured(1).toInt(&firstValid);
        bool lastValid = true;
        const int last = match.captured(2).isEmpty()
            ? first : match.captured(2).toInt(&lastValid);
        if (!firstValid || !lastValid || first > last || last > pageCount
            || last - first >= maximumSelectedPages)
            return false;
        for (int page = first - 1; page < last; ++page) {
            selected.insert(page);
            if (selected.size() > maximumSelectedPages)
                return false;
        }
    }
    pages = QVector<int>(selected.cbegin(), selected.cend());
    std::sort(pages.begin(), pages.end());
    return !pages.isEmpty();
}
}

struct PDFOCRController::Work {
    PDFOCRPipeline::Request request;
    QString documentPath;
    QString sourceIdentity;
    QUrl target;
    int revision = 0;
    qint64 sourceSize = 0;
    QDateTime sourceModified;
    // Cancellation and atomic rename share a lock. After committed becomes
    // true, cancellation cannot report that an already-published PDF vanished.
    QMutex commitMutex;
    bool committed = false;
};

struct PDFOCRController::Result {
    PDFOCRPipeline::Result pipeline;
    QString userError;
    bool committed = false;
};

PDFOCRController::PDFOCRController(QObject *parent) : QObject(parent)
{
    connect(qApp, &QCoreApplication::aboutToQuit, this, &PDFOCRController::cancel);
}

PDFOCRController::~PDFOCRController() { cancel(); }

bool PDFOCRController::available() const
{
#if defined(TREPDF_HAS_OCR) && TREPDF_HAS_OCR
    return true;
#else
    return false;
#endif
}

bool PDFOCRController::busy() const { return bool(m_work); }
int PDFOCRController::completedPages() const { return m_completedPages; }
int PDFOCRController::totalPages() const { return m_totalPages; }
int PDFOCRController::recognizedPages() const { return m_recognizedPages; }
int PDFOCRController::skippedPages() const { return m_skippedPages; }
int PDFOCRController::emptyPages() const { return m_emptyPages; }
QString PDFOCRController::statusText() const { return m_statusText; }
QUrl PDFOCRController::target() const { return m_work ? m_work->target : QUrl(); }

void PDFOCRController::setDocument(PDFDocument *document)
{
    if (m_document == document)
        return;
    cancel();
    if (m_document)
        disconnect(m_document, nullptr, this, nullptr);
    m_document = document;
    if (m_document) {
        connect(m_document, &PDFDocument::sourceChanged, this, &PDFOCRController::cancel);
        connect(m_document, &PDFDocument::pageStructureChanged, this, &PDFOCRController::cancel);
        connect(m_document, &PDFDocument::loadedChanged, this, &PDFOCRController::cancel);
        connect(m_document, &QObject::destroyed, this, &PDFOCRController::cancel);
    }
}

void PDFOCRController::setRecognitionGuard(std::function<bool()> guard)
{
    m_recognitionGuard = std::move(guard);
}

void PDFOCRController::setDestinationGuard(std::function<bool(const QUrl &)> guard)
{
    m_destinationGuard = std::move(guard);
}

bool PDFOCRController::reject(const QString &message)
{
    m_statusText = message;
    emit changed();
    emit statusMessage(message);
    return false;
}

bool PDFOCRController::recognizeTo(const QUrl &target, const QString &language,
                                 const QString &pageRange)
{
    if (busy())
        return false;
    if (!available()) {
        //% "OCR is unavailable in this build."
        return reject(qtTrId("OCRUnavailable"));
    }
    if (!m_document || !m_document->isLoaded()) {
        //% "Open a PDF before recognizing text."
        return reject(qtTrId("OCRNeedsDocument"));
    }
    if (m_recognitionGuard && !m_recognitionGuard()) {
        //% "Save your changes before recognizing text."
        return reject(qtTrId("OCRNeedsSavedDocument"));
    }
    // Copy permission is snapshotted by PDFDocument. The worker additionally
    // revalidates modification permission on its privately opened source.
    if (!m_document->allowsTextCopy()) {
        //% "This PDF does not allow text recognition."
        return reject(qtTrId("OCRPermissionDenied"));
    }
    if (!OCRLanguageManager::validSelection(language)) {
        //% "Choose between one and four different OCR languages."
        return reject(qtTrId("OCRInvalidLanguage"));
    }
    const QFileInfo source(m_document->renderFilePath());
    const QFileInfo destination(target.toLocalFile());
    if (!target.isLocalFile() || destination.fileName().isEmpty()
        || destination.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) != 0
        || destination.isDir() || fileIdentity(source) == fileIdentity(destination)) {
        //% "Choose a PDF destination different from the source file."
        return reject(qtTrId("OCRInvalidDestination"));
    }
    if (m_destinationGuard && !m_destinationGuard(target)) {
        //% "Choose a destination that is not used by an open PDF."
        return reject(qtTrId("OCRDestinationInUse"));
    }
    QVector<int> pages;
    if (!parsePages(pageRange, m_document->pageCount(), pages)) {
        //% "Enter valid page numbers or ranges, such as 1, 3-5. Select at most 1,000 pages."
        return reject(qtTrId("OCRInvalidPageRange"));
    }
    // The output preserves the complete saved document. A transient page map
    // would make a selected display page refer to different saved content.
    if (m_document->pageCount() != m_document->sourcePageCount()) {
        //% "Save your changes before recognizing text."
        return reject(qtTrId("OCRNeedsSavedDocument"));
    }
    for (int page = 0; page < m_document->pageCount(); ++page) {
        if (m_document->sourcePageIndex(page) != page) {
            //% "Save your changes before recognizing text."
            return reject(qtTrId("OCRNeedsSavedDocument"));
        }
    }
    auto work = std::make_shared<Work>();
    work->request.sourcePath = source.absoluteFilePath();
    work->request.password = m_document->documentPassword();
    work->request.sourcePages = pages;
    work->request.language = language;
    work->request.tessdataPath = OCRLanguageManager::bundledDirectory();
    for (const QString &code : language.split(QLatin1Char('+'))) {
        const QString path = OCRLanguageManager::installedModelPath(code);
        if (path.isEmpty()) {
            //% "The selected language files are not ready. Open Languages and download them before starting OCR."
            return reject(qtTrId("OCRLanguagesNotReady"));
        }
        work->request.modelPaths.insert(code, path);
    }
    work->request.cancellation = std::make_shared<std::atomic_bool>(false);
    work->documentPath = m_document->renderFilePath();
    work->sourceIdentity = fileIdentity(source);
    work->target = QUrl::fromLocalFile(destination.absoluteFilePath());
    work->revision = m_document->renderRevision();
    work->sourceSize = source.size();
    work->sourceModified = source.lastModified();
    m_work = work;
    m_completedPages = 0;
    m_totalPages = pages.size();
    m_recognizedPages = m_skippedPages = m_emptyPages = 0;
    //% "Recognizing text…"
    m_statusText = qtTrId("OCRRecognizing");
    const QPointer<PDFOCRController> controller(this);
    auto *runnable = QRunnable::create([controller, work]() {
        const auto progress = [controller, work](int completed) {
            QMetaObject::invokeMethod(qApp, [controller, work, completed]() {
                if (!controller || controller->m_work != work
                    || work->request.cancellation->load())
                    return;
                if (!controller->m_document
                    || controller->m_document->renderRevision() != work->revision
                    || controller->m_document->renderFilePath() != work->documentPath) {
                    controller->cancel();
                    return;
                }
                controller->m_completedPages = qBound(0, completed, controller->m_totalPages);
                emit controller->changed();
            }, Qt::QueuedConnection);
        };
        const Result result = performRecognition(work, progress);
        QMetaObject::invokeMethod(qApp, [controller, work, result]() {
            if (controller)
                controller->complete(work, result);
        }, Qt::QueuedConnection);
    });
    // A single global worker caps recognition/decoder memory across all tabs;
    // tryStart rejects another request instead of accumulating obsolete jobs.
    if (!ocrPool().tryStart(runnable)) {
        delete runnable;
        m_work.reset();
        //% "Another OCR job is running. Try again when it finishes."
        return reject(qtTrId("OCRAlreadyRunning"));
    }
    emit changed();
    return true;
}

void PDFOCRController::cancel()
{
    if (!m_work)
        return;
    QMutexLocker lock(&m_work->commitMutex);
    if (!m_work->committed)
        m_work->request.cancellation->store(true);
}

PDFOCRController::Result PDFOCRController::performRecognition(
    const std::shared_ptr<Work> &work, const std::function<void(int)> &progress)
{
    Result result;
    const auto cancelled = [&]() { return work->request.cancellation->load(); };
    const auto sourceUnchanged = [&]() {
        const QFileInfo source(work->request.sourcePath);
        return source.exists() && source.size() == work->sourceSize
            && source.lastModified() == work->sourceModified
            && fileIdentity(source) == work->sourceIdentity;
    };
    if (cancelled()) {
        result.pipeline.outcome = PDFOCRPipeline::Result::Cancelled;
        return result;
    }
    QTemporaryDir staging;
    if (!staging.isValid() || !sourceUnchanged()) {
        result.pipeline.error = QStringLiteral("Source changed or temporary directory is unavailable.");
        return result;
    }
    const QString stagedPath = staging.filePath(QStringLiteral("recognized.pdf"));
    result.pipeline = PDFOCRPipeline::run(work->request, stagedPath, progress);
    if (result.pipeline.outcome != PDFOCRPipeline::Result::Succeeded) {
        // The pipeline exposes only translated, bounded diagnostic messages;
        // filesystem/backend details from this controller remain internal.
        result.userError = result.pipeline.error;
        return result;
    }
    QFile input(stagedPath);
    QSaveFile output(work->target.toLocalFile());
    output.setDirectWriteFallback(false);
    // Convert success to failure until the destination has been atomically
    // committed. A staged PDF alone must never emit a successful completion.
    result.pipeline.outcome = PDFOCRPipeline::Result::Failed;
    if (!input.open(QIODevice::ReadOnly) || input.size() <= 0
        || !output.open(QIODevice::WriteOnly)) {
        result.pipeline.error = QStringLiteral("Cannot read staged PDF or write destination.");
        return result;
    }
    while (!input.atEnd()) {
        if (cancelled()) {
            result.pipeline.outcome = PDFOCRPipeline::Result::Cancelled;
            return result;
        }
        const QByteArray chunk = input.read(256 * 1024);
        if (chunk.isEmpty() || output.write(chunk) != chunk.size()) {
            result.pipeline.error = QStringLiteral("Cannot copy the completed PDF to destination.");
            return result;
        }
    }
    QMutexLocker lock(&work->commitMutex);
    if (cancelled()) {
        result.pipeline.outcome = PDFOCRPipeline::Result::Cancelled;
        return result;
    }
    const QFileInfo destination(work->target.toLocalFile());
    if (!sourceUnchanged() || fileIdentity(destination) == work->sourceIdentity) {
        result.pipeline.error = QStringLiteral("Source or destination changed during OCR.");
        return result;
    }
    if (!output.commit()) {
        result.pipeline.error = output.errorString();
        return result;
    }
    work->committed = true;
    result.committed = true;
    result.pipeline.outcome = PDFOCRPipeline::Result::Succeeded;
    return result;
}

void PDFOCRController::complete(const std::shared_ptr<Work> &work, const Result &result)
{
    if (m_work != work)
        return;
    m_recognizedPages = result.pipeline.recognizedPages;
    m_skippedPages = result.pipeline.skippedPages;
    m_emptyPages = result.pipeline.emptyPages;
    if (result.committed) {
        m_completedPages = m_totalPages;
        //% "OCR complete: %1 pages recognized, %2 pages already had text, %3 pages had no text detected."
        const QString pattern = qtTrId("OCRSucceeded");
        m_statusText = pattern.contains(QStringLiteral("%1"))
            ? pattern.arg(m_recognizedPages).arg(m_skippedPages).arg(m_emptyPages) : pattern;
    } else if (work->request.cancellation->load()
               || result.pipeline.outcome == PDFOCRPipeline::Result::Cancelled) {
        //% "OCR cancelled. No output file was changed."
        m_statusText = qtTrId("OCRCancelled");
    } else if (result.pipeline.outcome == PDFOCRPipeline::Result::NoText) {
        //% "No new text was recognized. Pages with existing text were skipped; no output file was changed."
        m_statusText = qtTrId("OCRNoText");
    } else if (!result.userError.isEmpty()) {
        m_statusText = result.userError;
    } else {
        qWarning().noquote() << "OCR failed:" << result.pipeline.error;
        //% "Could not create the OCR PDF. Check the document, language data and destination, then try again."
        m_statusText = qtTrId("OCRFailed");
    }
    m_work.reset();
    emit changed();
    emit statusMessage(m_statusText);
    emit finished(result.committed, work->target);
}
