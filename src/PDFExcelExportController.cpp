#include "PDFExcelExportController.h"

#include "PDFDocument.h"
#include "PDFTableExtractor.h"
#include "PDFiumSynchronization.h"
#include "PDFiumDocument.h"
#include "SpreadsheetWorkbook.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRunnable>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTranslation>

#include <atomic>

namespace {
QThreadPool &exportPool()
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

struct ExportDocument {
    PDFiumDocument handle;
    ~ExportDocument()
    {
        if (handle) {
            PDFiumProcessLock lock;
            handle.reset();
        }
    }
};
}

struct PDFExcelExportController::Work {
    QString sourcePath;
    QByteArray password;
    QUrl target;
    QVector<int> sourcePages;
    int revision = 0;
    qint64 sourceSize = 0;
    QDateTime sourceModified;
    QString sheetNamePattern;
    std::shared_ptr<std::atomic_bool> cancellation = std::make_shared<std::atomic_bool>(false);
    // Cancellation and the final atomic rename cannot pass each other unnoticed.
    QMutex commitMutex;
    bool committed = false;
};

struct PDFExcelExportController::Result {
    enum Outcome { Succeeded, Cancelled, NoTables, Failed } outcome = Failed;
    int tableCount = 0;
    QString error;
    QString userError;
};

PDFExcelExportController::PDFExcelExportController(QObject *parent)
    : QObject(parent)
{
    connect(qApp, &QCoreApplication::aboutToQuit, this, &PDFExcelExportController::cancel);
}

PDFExcelExportController::~PDFExcelExportController()
{
    cancel();
}

bool PDFExcelExportController::busy() const { return bool(m_work); }
int PDFExcelExportController::completedPages() const { return m_completedPages; }
int PDFExcelExportController::totalPages() const { return m_totalPages; }
QString PDFExcelExportController::statusText() const { return m_statusText; }

void PDFExcelExportController::setDocument(PDFDocument *document)
{
    if (m_document == document)
        return;
    cancel();
    if (m_document)
        disconnect(m_document, nullptr, this, nullptr);
    m_document = document;
    if (m_document) {
        connect(m_document, &PDFDocument::sourceChanged, this, &PDFExcelExportController::cancel);
        connect(m_document, &PDFDocument::pageStructureChanged, this, &PDFExcelExportController::cancel);
        connect(m_document, &PDFDocument::loadedChanged, this, &PDFExcelExportController::cancel);
        connect(m_document, &QObject::destroyed, this, &PDFExcelExportController::cancel);
    }
}

void PDFExcelExportController::setExportGuard(std::function<bool()> guard)
{
    m_exportGuard = std::move(guard);
}

bool PDFExcelExportController::reject(const QString &message)
{
    m_statusText = message;
    emit changed();
    emit statusMessage(message);
    return false;
}

bool PDFExcelExportController::exportTo(const QUrl &target)
{
    if (busy())
        return false;
    if (!m_document || !m_document->isLoaded()) {
        //% "Open a PDF before exporting."
        return reject(qtTrId("ExcelExportNeedsDocument"));
    }
    if (m_exportGuard && !m_exportGuard()) {
        //% "Save your changes before exporting to Excel."
        return reject(qtTrId("ExcelExportNeedsSavedDocument"));
    }
    const QFileInfo destination(target.toLocalFile());
    const QFileInfo source(m_document->renderFilePath());
    if (!target.isLocalFile() || destination.fileName().isEmpty()
        || destination.suffix().compare(QStringLiteral("xlsx"), Qt::CaseInsensitive) != 0
        || (!destination.canonicalFilePath().isEmpty()
            && destination.canonicalFilePath() == source.canonicalFilePath())) {
        //% "Choose an Excel (.xlsx) destination different from the source PDF."
        return reject(qtTrId("ExcelExportInvalidDestination"));
    }
    auto work = std::make_shared<Work>();
    work->sourcePath = source.absoluteFilePath();
    work->password = m_document->documentPassword();
    work->target = QUrl::fromLocalFile(destination.absoluteFilePath());
    work->revision = m_document->renderRevision();
    work->sourceSize = source.size();
    work->sourceModified = source.lastModified();
    //% "Page %1 Table %2"
    work->sheetNamePattern = qtTrId("ExcelExportSheetName");
    // ID-based translations return the ID if no catalog is installed. Sheet
    // names must still be unique in headless/embedded use without a translator.
    if (!work->sheetNamePattern.contains(QStringLiteral("%1"))
        || !work->sheetNamePattern.contains(QStringLiteral("%2")))
        work->sheetNamePattern = QStringLiteral("Page %1 Table %2");
    for (int page = 0; page < m_document->pageCount(); ++page)
        work->sourcePages.append(m_document->sourcePageIndex(page));
    m_work = work;
    m_completedPages = 0;
    m_totalPages = work->sourcePages.size();
    //% "Looking for tables…"
    m_statusText = qtTrId("ExcelExportScanning");
    const QPointer<PDFExcelExportController> controller(this);
    auto *runnable = QRunnable::create([controller, work]() {
        const auto progress = [controller, work](int completed) {
            QMetaObject::invokeMethod(qApp, [controller, work, completed]() {
                if (controller && controller->m_work == work
                    && !work->cancellation->load()) {
                    if (!controller->m_document
                        || controller->m_document->renderRevision() != work->revision
                        || controller->m_document->renderFilePath() != work->sourcePath) {
                        controller->cancel();
                        return;
                    }
                    controller->m_completedPages = completed;
                    emit controller->changed();
                }
            }, Qt::QueuedConnection);
        };
        const Result result = performExport(work, progress);
        QMetaObject::invokeMethod(qApp, [controller, work, result]() {
            if (controller)
                controller->complete(work, result);
        }, Qt::QueuedConnection);
    });
    // A single active export bounds decoder/workbook memory; no obsolete queue.
    if (!exportPool().tryStart(runnable)) {
        delete runnable;
        m_work.reset();
        //% "Another Excel export is running. Try again when it finishes."
        return reject(qtTrId("ExcelExportAlreadyRunning"));
    }
    emit changed();
    return true;
}

void PDFExcelExportController::cancel()
{
    if (!m_work)
        return;
    QMutexLocker lock(&m_work->commitMutex);
    if (!m_work->committed)
        m_work->cancellation->store(true);
}

PDFExcelExportController::Result PDFExcelExportController::performExport(
    const std::shared_ptr<Work> &work, const std::function<void(int)> &progress)
{
    Result result;
    const auto cancelled = [&]() { return work->cancellation->load(); };
    const auto sourceUnchanged = [&]() {
        const QFileInfo source(work->sourcePath);
        return source.exists() && source.size() == work->sourceSize
            && source.lastModified() == work->sourceModified;
    };
    if (cancelled()) {
        result.outcome = Result::Cancelled;
        return result;
    }
    QTemporaryDir staging;
    if (!staging.isValid() || !sourceUnchanged())
        return result;
    ExportDocument document;
    {
        PDFiumProcessLock lock;
        if (!cancelled())
            document.handle = PDFiumDocument::open(work->sourcePath, work->password);
    }
    if (!document.handle)
        return result;
    const QString workbookPath = staging.filePath(QStringLiteral("tables.xlsx"));
    auto workbook = createSpreadsheetWorkbook();
    if (!workbook || !workbook->open(workbookPath, result.error))
        return result;
    for (int page = 0; page < work->sourcePages.size(); ++page) {
        if (cancelled()) {
            result.outcome = Result::Cancelled;
            return result;
        }
        QVector<SpreadsheetSheet> tables;
        if (work->sourcePages.at(page) >= 0) {
            PDFiumProcessLock lock;
            if (cancelled()) {
                result.outcome = Result::Cancelled;
                return result;
            }
            if (!PDFTableExtractor::extractPage(document.handle, work->sourcePages.at(page),
                                               tables, work->cancellation, result.error)) {
                result.userError = result.error;
                return result;
            }
        }
        for (int table = 0; table < tables.size(); ++table) {
            if (cancelled()) {
                result.outcome = Result::Cancelled;
                return result;
            }
            if (++result.tableCount > 1000) {
                result.error = QStringLiteral("Workbook exceeds the 1000-table export limit.");
                return result;
            }
            tables[table].name = work->sheetNamePattern.arg(page + 1).arg(table + 1);
            if (!workbook->writeSheet(tables.at(table), result.error))
                return result;
        }
        progress(page + 1);
    }
    if (cancelled()) {
        result.outcome = Result::Cancelled;
        return result;
    }
    if (result.tableCount == 0) {
        result.outcome = Result::NoTables;
        return result;
    }
    if (!workbook->finish(result.error) || cancelled())
        return result;
    QFile input(workbookPath);
    QSaveFile output(work->target.toLocalFile());
    output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        result.error = output.errorString();
        return result;
    }
    while (!input.atEnd()) {
        if (cancelled()) {
            result.outcome = Result::Cancelled;
            return result;
        }
        const QByteArray chunk = input.read(256 * 1024);
        if (chunk.isEmpty() || output.write(chunk) != chunk.size()) {
            result.error = output.errorString();
            return result;
        }
    }
    QMutexLocker lock(&work->commitMutex);
    if (cancelled()) {
        result.outcome = Result::Cancelled;
        return result;
    }
    if (!sourceUnchanged() || !output.commit()) {
        result.error = output.errorString();
        return result;
    }
    work->committed = true;
    result.outcome = Result::Succeeded;
    return result;
}

void PDFExcelExportController::complete(const std::shared_ptr<Work> &work, const Result &result)
{
    if (m_work != work)
        return;
    const bool succeeded = result.outcome == Result::Succeeded;
    if (succeeded) {
        //% "Exported %1 tables to %2."
        const QString pattern = qtTrId("ExcelExportSucceeded");
        m_statusText = pattern.contains(QStringLiteral("%1"))
            ? pattern.arg(result.tableCount).arg(QFileInfo(work->target.toLocalFile()).fileName())
            : pattern;
    } else if (work->cancellation->load() || result.outcome == Result::Cancelled) {
        //% "Excel export cancelled."
        m_statusText = qtTrId("ExcelExportCancelled");
    } else if (result.outcome == Result::NoTables) {
        //% "No supported tables found. Tables need a text layer and a complete cell grid."
        m_statusText = qtTrId("ExcelExportNoTables");
    } else if (!result.userError.isEmpty()) {
        m_statusText = result.userError;
    } else {
        qWarning().noquote() << "Excel export failed:" << result.error;
        //% "Could not export the tables. Check the table layout and destination, then try again."
        m_statusText = qtTrId("ExcelExportFailed");
    }
    m_work.reset();
    emit changed();
    emit statusMessage(m_statusText);
    emit finished(succeeded, work->target);
}
