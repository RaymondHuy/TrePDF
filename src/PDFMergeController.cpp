#include "PDFMergeController.h"

#include "PDFAtomicSaveFile.h"
#include "PDFiumDocument.h"
#include "PDFiumSynchronization.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRunnable>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTranslation>

#include <qpdf/qpdf-c.h>
#include <qpdf/qpdfjob-c.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>

namespace {
constexpr int maximumFiles = 100;
constexpr int maximumPages = 10000;
constexpr qint64 maximumFileBytes = 256LL * 1024 * 1024;
constexpr qint64 maximumBatchBytes = 1024LL * 1024 * 1024;
constexpr int maximumAnnotations = 100000;
constexpr int maximumTreeNodes = 100000;

QThreadPool &mergePool()
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

bool sameFile(const QString &left, const QString &right)
{
    if (QFileInfo(left).absoluteFilePath() == QFileInfo(right).absoluteFilePath())
        return true;
    const auto a = std::filesystem::u8path(left.toUtf8().constData());
    const auto b = std::filesystem::u8path(right.toUtf8().constData());
    std::error_code error;
    return std::filesystem::equivalent(a, b, error) && !error;
}

struct QPdfDocument {
    qpdf_data handle = qpdf_init();
    explicit QPdfDocument(qpdf_data existing = nullptr)
    {
        if (existing) {
            qpdf_cleanup(&handle);
            handle = existing;
        }
        if (handle) {
            qpdf_silence_errors(handle);
            qpdf_set_suppress_warnings(handle, QPDF_TRUE);
            qpdf_set_attempt_recovery(handle, QPDF_FALSE);
        }
    }
    ~QPdfDocument()
    {
        if (handle)
            qpdf_get_error(handle);
        qpdf_cleanup(&handle);
    }
    Q_DISABLE_COPY_MOVE(QPdfDocument)
};

struct QPdfJob {
    qpdfjob_handle handle = qpdfjob_init();
    qpdflogger_handle logger = qpdflogger_create();
    QPdfJob()
    {
        qpdflogger_set_info(logger, qpdf_log_dest_discard, nullptr, nullptr);
        qpdflogger_set_warn(logger, qpdf_log_dest_discard, nullptr, nullptr);
        qpdflogger_set_error(logger, qpdf_log_dest_discard, nullptr, nullptr);
        qpdfjob_set_logger(handle, logger);
    }
    ~QPdfJob()
    {
        qpdfjob_cleanup(&handle);
        qpdflogger_cleanup(&logger);
    }
};

enum class Error { None, InvalidFile, Encrypted, Unsupported, Limits, SourceChanged,
                   Destination, Failed };

QString errorText(Error error, const QString &name)
{
    switch (error) {
    case Error::InvalidFile:
        //% "Could not read %1 as a complete PDF. Check the file and add it again."
        return qtTrId("MergePdfInvalidFile").arg(name);
    case Error::Encrypted:
        //% "%1 is password-protected. Add a PDF without password protection to merge it."
        return qtTrId("MergePdfEncryptedFile").arg(name);
    case Error::Unsupported:
        //% "%1 uses digital signatures, XFA forms, layers, or actions that cannot be preserved when merging yet."
        return qtTrId("MergePdfUnsupportedFile").arg(name);
    case Error::Limits:
        //% "Merge supports up to 100 PDFs, 10,000 pages, 256 MB per file, and 1 GB in total."
        return qtTrId("MergePdfLimitExceeded");
    case Error::SourceChanged:
        //% "%1 changed after it was added. Remove it and add it again before merging."
        return qtTrId("MergePdfSourceChanged").arg(name);
    case Error::Destination:
        //% "Choose a writable PDF destination different from every input file."
        return qtTrId("MergePdfInvalidDestination");
    default:
        //% "Could not merge these PDFs. Your original files and destination have not been changed."
        return qtTrId("MergePdfFailed");
    }
}

bool has(qpdf_data pdf, qpdf_oh object, const char *key)
{
    return qpdf_oh_is_dictionary(pdf, object) && qpdf_oh_has_key(pdf, object, key)
        && !qpdf_oh_is_null(pdf, qpdf_oh_get_key(pdf, object, key));
}

QByteArray objectString(qpdf_data pdf, qpdf_oh object)
{
    if (qpdf_oh_is_name(pdf, object))
        return QByteArray(qpdf_oh_get_name(pdf, object)).mid(1);
    if (!qpdf_oh_is_string(pdf, object))
        return {};
    size_t length = 0;
    const char *bytes = qpdf_oh_get_binary_string_value(pdf, object, &length);
    if (length > 4096)
        return {};
    return QByteArray(bytes, static_cast<qsizetype>(length));
}

// Keep the original destination's view parameters, replacing only its page.
struct Destination {
    int page = -1;
    QList<QByteArray> parameters;
};
struct Link {
    int page = -1;
    int annotation = -1;
    bool action = false;
    Destination destination;
};
struct PdfInfo {
    int pageCount = 0;
    QList<int> annotationCounts;
    QList<Link> links;
};

bool destinationFromObject(qpdf_data pdf, qpdf_oh object, Destination &destination)
{
    if (qpdf_oh_is_dictionary(pdf, object))
        object = qpdf_oh_get_key(pdf, object, "/D");
    if (!qpdf_oh_is_array(pdf, object))
        return false;
    const int count = qpdf_oh_get_array_n_items(pdf, object);
    if (count < 2 || count > 8)
        return false;
    const qpdf_oh page = qpdf_oh_get_array_item(pdf, object, 0);
    if (!qpdf_oh_is_indirect(pdf, page)
        || !qpdf_oh_is_dictionary(pdf, page))
        return false;
    destination.page = qpdf_find_page_by_oh(pdf, page);
    if (destination.page < 0)
        return false;
    for (int index = 1; index < count; ++index) {
        const qpdf_oh value = qpdf_oh_get_array_item(pdf, object, index);
        if (qpdf_oh_is_indirect(pdf, value)
            || !(qpdf_oh_is_name(pdf, value) || qpdf_oh_is_null(pdf, value)
                 || (qpdf_oh_is_number(pdf, value)
                     && std::isfinite(qpdf_oh_get_numeric_value(pdf, value)))))
            return false;
        const QByteArray serialized(qpdf_oh_unparse(pdf, value));
        if (serialized.size() > 128)
            return false;
        destination.parameters.append(serialized);
    }
    return !qpdf_has_error(pdf);
}

bool readNamedDestinations(qpdf_data pdf, QHash<QByteArray, Destination> &destinations,
                           const std::function<bool()> &cancelled)
{
    const qpdf_oh root = qpdf_get_root(pdf);
    const qpdf_oh legacy = qpdf_oh_get_key(pdf, root, "/Dests");
    QList<QByteArray> keys;
    if (qpdf_oh_is_dictionary(pdf, legacy)) {
        qpdf_oh_begin_dict_key_iter(pdf, legacy);
        while (qpdf_oh_dict_more_keys(pdf)) {
            if (keys.size() >= maximumTreeNodes || cancelled())
                return false;
            keys.append(qpdf_oh_dict_next_key(pdf));
        }
        for (const auto &key : keys) {
            Destination destination;
            if (key.size() > 4097
                || !destinationFromObject(pdf, qpdf_oh_get_key(pdf, legacy, key.constData()),
                                           destination))
                return false;
            destinations.insert(key.mid(1), destination);
        }
    }
    const qpdf_oh names = qpdf_oh_get_key(pdf, root, "/Names");
    const qpdf_oh tree = qpdf_oh_get_key_if_dict(pdf, names, "/Dests");
    if (qpdf_oh_is_null(pdf, tree))
        return !qpdf_has_error(pdf);
    QList<QPair<qpdf_oh, int>> pending{{tree, 0}};
    int visited = 0;
    while (!pending.isEmpty()) {
        const auto node = pending.takeLast();
        if (++visited > maximumTreeNodes || node.second > 32 || cancelled()
            || !qpdf_oh_is_dictionary(pdf, node.first))
            return false;
        const qpdf_oh values = qpdf_oh_get_key(pdf, node.first, "/Names");
        if (qpdf_oh_is_array(pdf, values)) {
            const int count = qpdf_oh_get_array_n_items(pdf, values);
            if (count % 2 != 0 || count > maximumTreeNodes * 2)
                return false;
            for (int index = 0; index < count; index += 2) {
                Destination destination;
                const QByteArray key = objectString(pdf, qpdf_oh_get_array_item(pdf, values, index));
                if (key.isEmpty() || destinations.size() >= maximumTreeNodes
                    || destinations.contains(key)
                    || !destinationFromObject(pdf, qpdf_oh_get_array_item(pdf, values, index + 1),
                                               destination))
                    return false;
                destinations.insert(key, destination);
            }
        }
        const qpdf_oh kids = qpdf_oh_get_key(pdf, node.first, "/Kids");
        if (qpdf_oh_is_array(pdf, kids)) {
            const int count = qpdf_oh_get_array_n_items(pdf, kids);
            if (count > maximumTreeNodes - visited - pending.size())
                return false;
            for (int index = 0; index < count; ++index)
                pending.append({qpdf_oh_get_array_item(pdf, kids, index), node.second + 1});
        }
    }
    return !qpdf_has_error(pdf);
}

bool safeFields(qpdf_data pdf, qpdf_oh acroform, const std::function<bool()> &cancelled)
{
    if (has(pdf, acroform, "/XFA") || has(pdf, acroform, "/CO"))
        return false;
    QList<QPair<qpdf_oh, int>> pending;
    const qpdf_oh fields = qpdf_oh_get_key_if_dict(pdf, acroform, "/Fields");
    if (qpdf_oh_is_array(pdf, fields))
        pending.append({fields, 0});
    int visited = 0;
    while (!pending.isEmpty()) {
        const auto node = pending.takeLast();
        const int count = qpdf_oh_get_array_n_items(pdf, node.first);
        if (count > maximumTreeNodes - visited || node.second > 32 || cancelled())
            return false;
        visited += count;
        for (int index = 0; index < count; ++index) {
            const qpdf_oh field = qpdf_oh_get_array_item(pdf, node.first, index);
            if (!qpdf_oh_is_dictionary(pdf, field) || has(pdf, field, "/AA")
                || has(pdf, field, "/A")
                || qpdf_oh_is_name_and_equals(pdf, qpdf_oh_get_key(pdf, field, "/FT"), "/Sig"))
                return false;
            const qpdf_oh kids = qpdf_oh_get_key(pdf, field, "/Kids");
            if (qpdf_oh_is_array(pdf, kids))
                pending.append({kids, node.second + 1});
        }
    }
    return !qpdf_has_error(pdf);
}

Error inspectPdf(const QString &path, PdfInfo &info, const std::function<bool()> &cancelled)
{
    QPdfDocument document;
    const auto pdf = document.handle;
    if (!pdf)
        return Error::Failed;
    const QByteArray encoded = path.toUtf8();
    if (qpdf_read(pdf, encoded.constData(), "") != QPDF_SUCCESS) {
        const qpdf_error error = qpdf_get_error(pdf);
        return error && qpdf_get_error_code(pdf, error) == qpdf_e_password
            ? Error::Encrypted : Error::InvalidFile;
    }
    if (qpdf_is_encrypted(pdf))
        return Error::Encrypted;
    info.pageCount = qpdf_get_num_pages(pdf);
    if (info.pageCount < 1)
        return Error::InvalidFile;
    if (info.pageCount > maximumPages)
        return Error::Limits;
    const qpdf_oh root = qpdf_get_root(pdf);
    const qpdf_oh names = qpdf_oh_get_key(pdf, root, "/Names");
    const qpdf_oh openAction = qpdf_oh_get_key(pdf, root, "/OpenAction");
    if (has(pdf, root, "/OCProperties") || has(pdf, root, "/Perms")
        || has(pdf, root, "/AA") || has(pdf, root, "/Collection")
        || (qpdf_oh_is_dictionary(pdf, openAction)
            && !qpdf_oh_is_name_and_equals(pdf, qpdf_oh_get_key(pdf, openAction, "/S"), "/GoTo"))
        || has(pdf, names, "/JavaScript")
        || !safeFields(pdf, qpdf_oh_get_key(pdf, root, "/AcroForm"), cancelled))
        return Error::Unsupported;
    // Outlines, attachments and structure tags are intentionally not copied from
    // a single arbitrary input. The merge dialog discloses that limitation.
    QHash<QByteArray, Destination> destinations;
    if (!readNamedDestinations(pdf, destinations, cancelled))
        return Error::Unsupported;
    qpdf_oh_release_all(pdf);
    int annotations = 0;
    for (int page = 0; page < info.pageCount; ++page) {
        if (cancelled())
            return Error::None;
        const qpdf_oh pageObject = qpdf_get_page_n(pdf, static_cast<size_t>(page));
        if (has(pdf, pageObject, "/AA"))
            return Error::Unsupported;
        const qpdf_oh annots = qpdf_oh_get_key(pdf, pageObject, "/Annots");
        const int count = qpdf_oh_is_array(pdf, annots) ? qpdf_oh_get_array_n_items(pdf, annots) : 0;
        if (count > maximumAnnotations - annotations)
            return Error::Limits;
        annotations += count;
        info.annotationCounts.append(count);
        for (int index = 0; index < count; ++index) {
            const qpdf_oh annotation = qpdf_oh_get_array_item(pdf, annots, index);
            if (!qpdf_oh_is_dictionary(pdf, annotation) || has(pdf, annotation, "/AA"))
                return Error::Unsupported;
            const qpdf_oh subtype = qpdf_oh_get_key(pdf, annotation, "/Subtype");
            if (qpdf_oh_is_name_and_equals(pdf, subtype, "/RichMedia")
                || qpdf_oh_is_name_and_equals(pdf, subtype, "/Screen")
                || qpdf_oh_is_name_and_equals(pdf, subtype, "/Movie"))
                return Error::Unsupported;
            const qpdf_oh action = qpdf_oh_get_key(pdf, annotation, "/A");
            const bool hasAction = qpdf_oh_is_dictionary(pdf, action);
            const qpdf_oh type = qpdf_oh_get_key_if_dict(pdf, action, "/S");
            const bool goTo = qpdf_oh_is_name_and_equals(pdf, type, "/GoTo");
            const bool uri = qpdf_oh_is_name_and_equals(pdf, type, "/URI");
            if (hasAction && (has(pdf, action, "/Next")
                             || !(goTo || uri)))
                return Error::Unsupported;
            if (uri) {
                // Relative URI and remote-file destinations depend on the
                // original catalog/base directory and cannot move unchanged.
                const qpdf_oh value = qpdf_oh_get_key(pdf, action, "/URI");
                if (!qpdf_oh_is_string(pdf, value))
                    return Error::Unsupported;
                const QUrl url = QUrl::fromEncoded(objectString(pdf, value), QUrl::StrictMode);
                if (!url.isValid() || url.isRelative() || url.scheme().isEmpty())
                    return Error::Unsupported;
            }
            const bool direct = has(pdf, annotation, "/Dest");
            if (direct && hasAction)
                return Error::Unsupported;
            if (!direct && !goTo)
                continue;
            Link link;
            link.page = page;
            link.annotation = index;
            link.action = goTo;
            const qpdf_oh destination = goTo ? qpdf_oh_get_key(pdf, action, "/D")
                                            : qpdf_oh_get_key(pdf, annotation, "/Dest");
            const QByteArray named = objectString(pdf, destination);
            if (!named.isEmpty()) {
                if (!destinations.contains(named))
                    return Error::Unsupported;
                link.destination = destinations.value(named);
            } else if (!destinationFromObject(pdf, destination, link.destination)) {
                return Error::Unsupported;
            }
            info.links.append(link);
        }
        if (qpdf_has_error(pdf))
            return Error::InvalidFile;
        qpdf_oh_release_all(pdf);
    }
    return Error::None;
}

bool fixInternalLinks(qpdf_data pdf, const QList<PdfInfo> &inputs,
                      const std::function<bool()> &cancelled)
{
    int offset = 0;
    for (const PdfInfo &info : inputs) {
        for (int page = 0; page < info.pageCount; ++page) {
            if (cancelled())
                return false;
            const qpdf_oh outputPage = qpdf_get_page_n(pdf, static_cast<size_t>(offset + page));
            const qpdf_oh annotations = qpdf_oh_get_key(pdf, outputPage, "/Annots");
            const int count = qpdf_oh_is_array(pdf, annotations)
                ? qpdf_oh_get_array_n_items(pdf, annotations) : 0;
            if (count != info.annotationCounts.at(page))
                return false;
            qpdf_oh_release_all(pdf);
        }
        for (const Link &link : info.links) {
            if (cancelled())
                return false;
            const qpdf_oh page = qpdf_get_page_n(pdf, static_cast<size_t>(offset + link.page));
            const qpdf_oh annotations = qpdf_oh_get_key(pdf, page, "/Annots");
            const qpdf_oh annotation = qpdf_oh_get_array_item(pdf, annotations, link.annotation);
            const qpdf_oh destination = qpdf_oh_new_array(pdf);
            qpdf_oh_append_item(pdf, destination,
                qpdf_get_page_n(pdf, static_cast<size_t>(offset + link.destination.page)));
            for (const auto &parameter : link.destination.parameters)
                qpdf_oh_append_item(pdf, destination, qpdf_oh_parse(pdf, parameter.constData()));
            if (link.action) {
                const qpdf_oh action = qpdf_oh_get_key(pdf, annotation, "/A");
                if (!qpdf_oh_is_dictionary(pdf, action))
                    return false;
                qpdf_oh_replace_key(pdf, action, "/D", destination);
            } else {
                qpdf_oh_replace_key(pdf, annotation, "/Dest", destination);
            }
            if (qpdf_has_error(pdf))
                return false;
            qpdf_oh_release_all(pdf);
        }
        offset += info.pageCount;
    }
    return qpdf_get_num_pages(pdf) == offset && !qpdf_has_error(pdf);
}

QByteArray fingerprintFile(const QString &path, const std::function<bool()> &cancelled,
                           QFile *snapshot = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maximumFileBytes)
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytes = 0;
    while (!file.atEnd()) {
        if (cancelled())
            return {};
        const QByteArray chunk = file.read(256 * 1024);
        bytes += chunk.size();
        if (chunk.isEmpty() || bytes > maximumFileBytes
            || (snapshot && snapshot->write(chunk) != chunk.size()))
            return {};
        hash.addData(chunk);
    }
    return file.error() == QFileDevice::NoError ? hash.result() : QByteArray();
}
} // namespace

struct PDFMergeController::Work {
    bool probe = false;
    QList<Entry> entries;
    int existingPages = 0;
    qint64 existingBytes = 0;
    QUrl target;
    bool targetExisted = false;
    qint64 targetSize = 0;
    QDateTime targetModified;
    std::atomic_bool cancelled{false};
    QMutex commitMutex;
    bool committed = false;
};

struct PDFMergeController::Result {
    bool succeeded = false;
    Error error = Error::Failed;
    QString fileName;
    QList<Entry> entries;
};

PDFMergeController::PDFMergeController(QObject *parent) : QObject(parent)
{
    connect(qApp, &QCoreApplication::aboutToQuit, this, &PDFMergeController::cancel);
}

PDFMergeController::~PDFMergeController() { cancel(); }
bool PDFMergeController::busy() const { return bool(m_work); }
bool PDFMergeController::canMerge() const { return !busy() && m_files.size() >= 2; }
QString PDFMergeController::statusText() const { return m_statusText; }
int PDFMergeController::totalPages() const
{
    int count = 0;
    for (const auto &entry : m_files)
        count += entry.pageCount;
    return count;
}

QVariantList PDFMergeController::files() const
{
    QVariantList result;
    for (const auto &entry : m_files) {
        result.append(QVariantMap{{QStringLiteral("url"), QUrl::fromLocalFile(entry.path)},
            {QStringLiteral("name"), QFileInfo(entry.path).fileName()},
            {QStringLiteral("path"), entry.path}, {QStringLiteral("pageCount"), entry.pageCount}});
    }
    return result;
}

bool PDFMergeController::reject(const QString &message)
{
    m_statusText = message;
    emit changed();
    emit statusMessage(message);
    return false;
}

bool PDFMergeController::addFiles(const QList<QUrl> &urls)
{
    if (busy() || urls.isEmpty())
        return false;
    if (urls.size() > maximumFiles - m_files.size())
        return reject(errorText(Error::Limits, {}));
    auto work = std::make_shared<Work>();
    work->probe = true;
    work->existingPages = totalPages();
    for (const Entry &entry : m_files)
        work->existingBytes += entry.size;
    qint64 bytes = work->existingBytes;
    for (const QUrl &url : urls) {
        const QFileInfo info(url.toLocalFile());
        if (!url.isLocalFile() || !info.isFile() || !info.isReadable()
            || info.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) != 0
            || info.size() <= 0)
            return reject(errorText(Error::InvalidFile, info.fileName()));
        bool duplicate = false;
        for (const auto &entry : m_files)
            duplicate |= sameFile(entry.path, info.absoluteFilePath());
        for (const auto &entry : work->entries)
            duplicate |= sameFile(entry.path, info.absoluteFilePath());
        if (duplicate)
            continue;
        bytes += info.size();
        if (info.size() > maximumFileBytes || bytes > maximumBatchBytes)
            return reject(errorText(Error::Limits, {}));
        work->entries.append({info.canonicalFilePath(), 0, info.size(), info.lastModified(), {}});
    }
    if (work->entries.isEmpty()) {
        //% "These PDFs are already in the list."
        return reject(qtTrId("MergePdfAlreadyAdded"));
    }
    //% "Reading PDF files…"
    m_statusText = qtTrId("MergePdfReading");
    return start(work);
}

void PDFMergeController::moveFile(int from, int to)
{
    if (busy() || from < 0 || from >= m_files.size() || to < 0 || to >= m_files.size() || from == to)
        return;
    m_files.move(from, to);
    m_statusText.clear();
    emit changed();
}

void PDFMergeController::removeFile(int index)
{
    if (busy() || index < 0 || index >= m_files.size())
        return;
    m_files.removeAt(index);
    m_statusText.clear();
    emit changed();
}

void PDFMergeController::clear()
{
    if (busy()) {
        cancel();
        return;
    }
    m_files.clear();
    m_statusText.clear();
    emit changed();
}

bool PDFMergeController::mergeTo(const QUrl &target)
{
    if (busy())
        return false;
    if (!canMerge()) {
        //% "Add at least two PDFs to merge."
        return reject(qtTrId("MergePdfNeedsFiles"));
    }
    const QFileInfo info(target.toLocalFile());
    if (!target.isLocalFile() || info.fileName().isEmpty() || info.isSymLink()
        || target.toLocalFile().contains(QChar::Null)
        || info.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) != 0
        || !info.dir().exists() || (info.exists() && !info.isFile()))
        return reject(errorText(Error::Destination, {}));
    for (const Entry &entry : m_files) {
        if (sameFile(entry.path, info.absoluteFilePath()))
            return reject(errorText(Error::Destination, {}));
    }
    auto work = std::make_shared<Work>();
    work->entries = m_files;
    work->target = QUrl::fromLocalFile(info.absoluteFilePath());
    work->targetExisted = info.exists();
    work->targetSize = info.size();
    work->targetModified = info.lastModified();
    //% "Merging PDF files…"
    m_statusText = qtTrId("MergePdfWorking");
    return start(work);
}

bool PDFMergeController::start(const std::shared_ptr<Work> &work)
{
    m_work = work;
    const QPointer<PDFMergeController> controller(this);
    auto *runnable = QRunnable::create([controller, work] {
        Result result;
        try {
            result = perform(work);
        } catch (...) {
            // qpdf configuration and malformed PDF input must never escape a worker.
        }
        QMetaObject::invokeMethod(qApp, [controller, work, result] {
            if (controller)
                controller->complete(work, result);
        }, Qt::QueuedConnection);
    });
    if (!mergePool().tryStart(runnable)) {
        delete runnable;
        m_work.reset();
        //% "Another PDF merge is finishing. Please try again in a moment."
        return reject(qtTrId("MergePdfAlreadyRunning"));
    }
    emit changed();
    return true;
}

void PDFMergeController::cancel()
{
    if (!m_work)
        return;
    QMutexLocker lock(&m_work->commitMutex);
    if (!m_work->committed)
        m_work->cancelled.store(true);
}

PDFMergeController::Result PDFMergeController::perform(const std::shared_ptr<Work> &work)
{
    Result result;
    const auto cancelled = [&] { return work->cancelled.load(); };
    QTemporaryDir staging;
    if (cancelled() || !staging.isValid())
        return result;
    int pages = work->existingPages;
    int annotationCount = 0;
    QList<QString> snapshots;
    QList<PdfInfo> inputs;
    for (Entry entry : work->entries) {
        result.fileName = QFileInfo(entry.path).fileName();
        const QFileInfo before(entry.path);
        if (before.size() != entry.size || before.lastModified() != entry.modified) {
            result.error = Error::SourceChanged;
            return result;
        }
        const QString path = staging.filePath(QStringLiteral("input-%1.pdf").arg(snapshots.size()));
        QFile snapshot(path);
        if (!snapshot.open(QIODevice::WriteOnly))
            return result;
        const QByteArray digest = fingerprintFile(entry.path, cancelled, &snapshot);
        snapshot.close();
        const QFileInfo after(entry.path);
        if (digest.isEmpty() || before.size() != after.size()
            || before.lastModified() != after.lastModified()
            || (!work->probe && digest != entry.fingerprint)) {
            result.error = Error::SourceChanged;
            return result;
        }
        if (cancelled())
            return result;
        PdfInfo info;
        result.error = inspectPdf(path, info, cancelled);
        if (result.error != Error::None || cancelled())
            return result;
        for (const int count : info.annotationCounts) {
            if (count > maximumAnnotations - annotationCount) {
                result.error = Error::Limits;
                return result;
            }
            annotationCount += count;
        }
        pages += info.pageCount;
        if (pages > maximumPages) {
            result.error = Error::Limits;
            return result;
        }
        entry.pageCount = info.pageCount;
        entry.fingerprint = digest;
        result.entries.append(entry);
        snapshots.append(path);
        inputs.append(info);
    }
    if (work->probe) {
        result.succeeded = !cancelled();
        return result;
    }
    const QString outputPath = staging.filePath(QStringLiteral("merged.pdf"));
    QList<QByteArray> arguments{"trepdf-merge", "--empty", "--suppress-recovery",
        "--stream-data=preserve", "--decode-level=none", "--pages"};
    for (const auto &path : snapshots) {
        arguments.append(path.toUtf8());
        arguments.append("1-z");
    }
    arguments.append("--");
    arguments.append(outputPath.toUtf8());
    QList<const char *> argv;
    for (const auto &argument : arguments)
        argv.append(argument.constData());
    argv.append(nullptr);
    {
        QPdfJob job;
        if (cancelled() || qpdfjob_initialize_from_argv(job.handle, argv.constData()) != 0)
            return result;
        qpdf_data output = qpdfjob_create_qpdf(job.handle);
        if (!output)
            return result;
        QPdfDocument document(output);
        if (!fixInternalLinks(output, inputs, cancelled) || cancelled()
            || qpdfjob_write_qpdf(job.handle, output) != 0)
            return result;
    }
    if (cancelled())
        return result;
    // Reopen with the application's renderer before committing; the private
    // document and all of its PDFium calls stay under the process-wide lock.
    {
        PDFiumProcessLock lock;
        if (cancelled())
            return result;
        auto document = PDFiumDocument::open(outputPath);
        if (!document || FPDF_GetPageCount(document) != pages)
            return result;
    }
    for (const Entry &entry : work->entries) {
        if (fingerprintFile(entry.path, cancelled) != entry.fingerprint) {
            result.error = Error::SourceChanged;
            result.fileName = QFileInfo(entry.path).fileName();
            return result;
        }
    }
    const QString target = work->target.toLocalFile();
    const auto validTarget = [&] {
        const QFileInfo info(target);
        if (info.isSymLink() || info.exists() != work->targetExisted
            || (info.exists() && (!info.isFile() || info.size() != work->targetSize
                || info.lastModified() != work->targetModified)))
            return false;
        for (const Entry &entry : work->entries) {
            if (sameFile(entry.path, target))
                return false;
        }
        return true;
    };
    if (!validTarget()) {
        result.error = Error::Destination;
        return result;
    }
    QFile input(outputPath);
    PDFAtomicSaveFile destination(target);
    destination.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
        result.error = Error::Destination;
        return result;
    }
    while (!input.atEnd()) {
        if (cancelled())
            return result;
        const QByteArray chunk = input.read(256 * 1024);
        if (chunk.isEmpty() || destination.write(chunk) != chunk.size())
            return result;
    }
    QMutexLocker lock(&work->commitMutex);
    if (cancelled())
        return result;
    for (const Entry &entry : work->entries) {
        const QFileInfo source(entry.path);
        if (source.size() != entry.size || source.lastModified() != entry.modified) {
            result.error = Error::SourceChanged;
            result.fileName = source.fileName();
            return result;
        }
    }
    if (!validTarget() || !destination.commit()) {
        result.error = Error::Destination;
        return result;
    }
    work->committed = true;
    result.succeeded = true;
    return result;
}

void PDFMergeController::complete(const std::shared_ptr<Work> &work, const Result &result)
{
    if (m_work != work)
        return;
    const bool succeeded = result.succeeded && (!work->cancelled.load() || work->committed);
    if (succeeded && work->probe) {
        m_files.append(result.entries);
        m_statusText.clear();
    } else if (succeeded) {
        //% "Merged PDF saved to %1."
        m_statusText = qtTrId("MergePdfSucceeded").arg(QFileInfo(work->target.toLocalFile()).fileName());
    } else if (work->cancelled.load()) {
        //% "PDF merge cancelled. Your files have not been changed."
        m_statusText = qtTrId("MergePdfCancelled");
    } else {
        m_statusText = errorText(result.error, result.fileName);
    }
    m_work.reset();
    emit changed();
    if (!m_statusText.isEmpty())
        emit statusMessage(m_statusText);
    if (!work->probe) {
        if (succeeded)
            emit completed(work->target);
        emit finished(succeeded, work->target);
    }
}
