#include "PDFSplitController.h"

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
#include <QRegularExpression>
#include <QRunnable>
#include <QSet>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTranslation>

#include <qpdf/qpdf-c.h>
#include <qpdf/qpdfjob-c.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
#include <stdio.h>
#elif defined(Q_OS_LINUX)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
constexpr int maximumPages = 10000;
constexpr int maximumOutputs = 1000;
constexpr qint64 maximumFileBytes = 256LL * 1024 * 1024;
constexpr qint64 maximumOutputBytes = 1024LL * 1024 * 1024;
constexpr int maximumAnnotations = 100000;
constexpr int maximumTreeNodes = 100000;
constexpr qsizetype maximumRangeLength = 65536;

QThreadPool &splitPool()
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

struct QPdfDocument {
    qpdf_data handle;
    explicit QPdfDocument(qpdf_data existing = nullptr) : handle(existing ? existing : qpdf_init())
    {
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
                   Destination, Ranges, Failed };

QString errorText(Error error)
{
    switch (error) {
    case Error::InvalidFile:
        //% "This file could not be read as a complete PDF. Choose the PDF again."
        return qtTrId("SplitPdfInvalidFile");
    case Error::Encrypted:
        //% "This PDF is password-protected. Choose a PDF without password protection to split it."
        return qtTrId("SplitPdfEncryptedFile");
    case Error::Unsupported:
        //% "This PDF contains forms, digital signatures, layers, or actions that cannot be preserved when splitting yet."
        return qtTrId("SplitPdfUnsupportedFile");
    case Error::Limits:
        //% "Split supports up to 10,000 source pages, 1,000 output files, a 256 MB source, and 1 GB of output in total."
        return qtTrId("SplitPdfLimitExceeded");
    case Error::SourceChanged:
        //% "The PDF changed after it was selected. Choose it again before splitting."
        return qtTrId("SplitPdfSourceChanged");
    case Error::Destination:
        //% "Choose an existing writable folder. TrePDF will create a new folder inside it without replacing your files."
        return qtTrId("SplitPdfInvalidDestination");
    case Error::Ranges:
        //% "Enter valid page groups, such as 1-3; 4-6; 7,9. Use each page only once and stay within the PDF's page count."
        return qtTrId("SplitPdfInvalidRanges");
    default:
        //% "Could not split this PDF. Your original file has not been changed and no output folder was created."
        return qtTrId("SplitPdfFailed");
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
    // Source annotation identities are converted to per-page indices before
    // qpdf copies pages. Its import can otherwise leave /IRT or /Parent aimed
    // at a duplicate foreign annotation instead of the retained page entry.
    struct Relations { int popup = -1; int parent = -1; int reply = -1; };
    QList<QList<Relations>> annotationRelations;
};

quint64 objectIdentity(qpdf_data pdf, qpdf_oh object)
{
    return (quint64(quint32(qpdf_oh_get_object_id(pdf, object))) << 32)
        | quint32(qpdf_oh_get_generation(pdf, object));
}

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
        || has(pdf, root, "/AcroForm"))
        return Error::Unsupported;
    // Page extraction starts with an empty catalog. Outlines, catalog attachments
    // and structure tags are not copied; the split dialog discloses this limit.
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
        QHash<quint64, int> annotationIndices;
        for (int index = 0; index < count; ++index) {
            if (cancelled())
                return Error::None;
            const auto annotation = qpdf_oh_get_array_item(pdf, annots, index);
            if (!qpdf_oh_is_indirect(pdf, annotation))
                continue;
            const quint64 identity = objectIdentity(pdf, annotation);
            if (annotationIndices.contains(identity))
                return Error::Unsupported;
            annotationIndices.insert(identity, index);
        }
        QList<PdfInfo::Relations> pageRelations;
        for (int index = 0; index < count; ++index) {
            const qpdf_oh annotation = qpdf_oh_get_array_item(pdf, annots, index);
            if (!qpdf_oh_is_dictionary(pdf, annotation) || has(pdf, annotation, "/AA"))
                return Error::Unsupported;
            PdfInfo::Relations relations;
            for (const auto &relation : {qMakePair("/Popup", &relations.popup),
                                        qMakePair("/Parent", &relations.parent),
                                        qMakePair("/IRT", &relations.reply)}) {
                if (!has(pdf, annotation, relation.first))
                    continue;
                const auto target = qpdf_oh_get_key(pdf, annotation, relation.first);
                const quint64 identity = objectIdentity(pdf, target);
                if (!qpdf_oh_is_indirect(pdf, target) || !annotationIndices.contains(identity)
                    || annotationIndices.value(identity) == index)
                    return Error::Unsupported;
                *relation.second = annotationIndices.value(identity);
            }
            pageRelations.append(relations);
            const qpdf_oh subtype = qpdf_oh_get_key(pdf, annotation, "/Subtype");
            if (qpdf_oh_is_name_and_equals(pdf, subtype, "/Widget")
                || qpdf_oh_is_name_and_equals(pdf, subtype, "/RichMedia")
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
        info.annotationRelations.append(pageRelations);
        if (qpdf_has_error(pdf))
            return Error::InvalidFile;
        qpdf_oh_release_all(pdf);
    }
    return Error::None;
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

// Groups are bounded before expansion; a source page may appear only once in
// the complete batch. Sort within each group to retain source reading order.
Error pageGroups(int pages, int mode, int perFile, const QString &ranges,
                 QList<QList<int>> &groups)
{
    if (pages < 1 || pages > maximumPages)
        return Error::InvalidFile;
    if (mode == 0 || mode == 1) {
        const int count = mode == 0 ? 1 : perFile;
        if (count < 1 || count > maximumPages)
            return Error::Ranges;
        if ((pages + count - 1) / count > maximumOutputs)
            return Error::Limits;
        for (int start = 0; start < pages; start += count) {
            QList<int> group;
            for (int page = start; page < std::min(pages, start + count); ++page)
                group.append(page);
            groups.append(group);
        }
        return Error::None;
    }
    if (mode != 2 || ranges.isEmpty() || ranges.size() > maximumRangeLength)
        return Error::Ranges;
    const QStringList expressions = ranges.split(QLatin1Char(';'));
    if (expressions.size() > maximumOutputs)
        return Error::Limits;
    static const QRegularExpression rangeExpression(QStringLiteral("^([0-9]+)(?:\\s*-\\s*([0-9]+))?$"));
    QSet<int> used;
    for (const QString &expression : expressions) {
        QList<int> group;
        const QStringList terms = expression.split(QLatin1Char(','));
        if (terms.size() > maximumPages)
            return Error::Limits;
        for (const QString &term : terms) {
            const auto match = rangeExpression.match(term.trimmed());
            if (!match.hasMatch())
                return Error::Ranges;
            bool firstOk = false;
            bool lastOk = true;
            const int first = match.captured(1).toInt(&firstOk);
            const int last = match.captured(2).isEmpty() ? first : match.captured(2).toInt(&lastOk);
            if (!firstOk || !lastOk || first < 1 || last < first || last > pages)
                return Error::Ranges;
            for (int page = first - 1; page < last; ++page) {
                if (used.contains(page))
                    return Error::Ranges;
                used.insert(page);
                group.append(page);
            }
        }
        std::sort(group.begin(), group.end());
        groups.append(group);
    }
    return groups.isEmpty() ? Error::Ranges : Error::None;
}

// Restore links against the new page tree rather than trusting qpdf's copied
// references. Destinations outside this output are disabled, preserving the
// annotation itself and its popup/reply relationships and visible appearance.
bool fixInternalLinks(qpdf_data pdf, const PdfInfo &info, const QList<int> &pages,
                      const std::function<bool()> &cancelled)
{
    if (qpdf_get_num_pages(pdf) != pages.size())
        return false;
    QHash<int, int> outputIndex;
    for (qsizetype index = 0; index < pages.size(); ++index)
        outputIndex.insert(pages.at(index), int(index));
    QHash<int, QHash<int, Link>> linksByPage;
    for (const auto &link : info.links)
        linksByPage[link.page].insert(link.annotation, link);
    for (qsizetype index = 0; index < pages.size(); ++index) {
        if (cancelled())
            return false;
        const int sourcePage = pages.at(index);
        const auto page = qpdf_get_page_n(pdf, size_t(index));
        const auto annotations = qpdf_oh_get_key(pdf, page, "/Annots");
        const int count = qpdf_oh_is_array(pdf, annotations)
            ? qpdf_oh_get_array_n_items(pdf, annotations) : 0;
        if (count != info.annotationCounts.at(sourcePage))
            return false;
        if (count == 0) {
            qpdf_oh_release_all(pdf);
            continue;
        }
        const auto kept = qpdf_oh_new_array(pdf);
        const auto links = linksByPage.value(sourcePage);
        for (int annotIndex = 0; annotIndex < count; ++annotIndex) {
            if (cancelled())
                return false;
            const auto annotation = qpdf_oh_get_array_item(pdf, annotations, annotIndex);
            const auto &relations = info.annotationRelations.at(sourcePage).at(annotIndex);
            for (const auto &relation : {qMakePair("/Popup", relations.popup),
                                        qMakePair("/Parent", relations.parent),
                                        qMakePair("/IRT", relations.reply)}) {
                if (relation.second >= 0)
                    qpdf_oh_replace_key(pdf, annotation, relation.first,
                                       qpdf_oh_get_array_item(pdf, annotations, relation.second));
            }
            const auto found = links.constFind(annotIndex);
            if (found != links.cend()) {
                if (!outputIndex.contains(found->destination.page)) {
                    qpdf_oh_remove_key(pdf, annotation, found->action ? "/A" : "/Dest");
                    if (has(pdf, annotation, "/P"))
                        qpdf_oh_replace_key(pdf, annotation, "/P", page);
                    qpdf_oh_append_item(pdf, kept, annotation);
                    continue;
                }
                const auto destination = qpdf_oh_new_array(pdf);
                qpdf_oh_append_item(pdf, destination,
                    qpdf_get_page_n(pdf, size_t(outputIndex.value(found->destination.page))));
                for (const auto &parameter : found->destination.parameters)
                    qpdf_oh_append_item(pdf, destination, qpdf_oh_parse(pdf, parameter.constData()));
                if (found->action) {
                    const auto action = qpdf_oh_get_key(pdf, annotation, "/A");
                    if (!qpdf_oh_is_dictionary(pdf, action))
                        return false;
                    qpdf_oh_replace_key(pdf, action, "/D", destination);
                } else {
                    qpdf_oh_replace_key(pdf, annotation, "/Dest", destination);
                }
            }
            // An annotation's optional owner reference must point at this output.
            if (has(pdf, annotation, "/P"))
                qpdf_oh_replace_key(pdf, annotation, "/P", page);
            qpdf_oh_append_item(pdf, kept, annotation);
        }
        qpdf_oh_replace_key(pdf, page, "/Annots", kept);
        if (qpdf_has_error(pdf))
            return false;
        qpdf_oh_release_all(pdf);
    }
    return !qpdf_has_error(pdf);
}

// qpdf's custom save sink enforces the batch byte budget while bytes are
// produced, avoiding an oversized temporary output before a post-write check.
struct OutputSink {
    QFile *file = nullptr;
    qint64 *total = nullptr;
    const std::function<bool()> *cancelled = nullptr;
    bool exceeded = false;
    static int write(const char *data, size_t length, void *opaque)
    {
        auto &sink = *static_cast<OutputSink *>(opaque);
        if ((*sink.cancelled)())
            return 1;
        if (length > size_t(maximumOutputBytes - *sink.total)) {
            sink.exceeded = true;
            return 1;
        }
        size_t offset = 0;
        while (offset < length) {
            if ((*sink.cancelled)())
                return 1;
            const qint64 chunk = qint64(std::min<size_t>(length - offset, 256 * 1024));
            if (sink.file->write(data + offset, chunk) != chunk)
                return 1;
            *sink.total += chunk;
            offset += size_t(chunk);
        }
        return 0;
    }
};

// Directory publication must not replace even an empty directory created by
// another process. Qt's exists()+rename() cannot provide that guarantee on
// POSIX; use each supported desktop OS's atomic no-replace primitive.
bool publishDirectory(const QString &staging, const QString &destination)
{
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    return ::renamex_np(QFile::encodeName(staging).constData(),
                        QFile::encodeName(destination).constData(), RENAME_EXCL) == 0;
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2)
    return ::syscall(SYS_renameat2, AT_FDCWD, QFile::encodeName(staging).constData(),
                     AT_FDCWD, QFile::encodeName(destination).constData(), RENAME_NOREPLACE) == 0;
#elif defined(Q_OS_WIN)
    return ::MoveFileExW(reinterpret_cast<LPCWSTR>(staging.utf16()),
                         reinterpret_cast<LPCWSTR>(destination.utf16()), MOVEFILE_WRITE_THROUGH);
#else
    Q_UNUSED(staging);
    Q_UNUSED(destination);
    return false;
#endif
}

QString outputBaseName(const QString &path)
{
    // Keep output names portable across filesystems and comfortably below
    // component limits even when the original filename uses multibyte text.
    QString base = QFileInfo(path).completeBaseName().left(60);
    base.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f<>:\"/\\\\|?*]")), QStringLiteral("_"));
    while (base.endsWith(QLatin1Char('.')) || base.endsWith(QLatin1Char(' ')))
        base.chop(1);
    return base.isEmpty() ? QStringLiteral("PDF") : base;
}
} // namespace

struct PDFSplitController::Work {
    bool probe = false;
    Entry entry;
    QList<QList<int>> groups;
    QString parentFolder;
    std::atomic_bool cancelled{false};
    QMutex commitMutex;
    bool committed = false;
};

struct PDFSplitController::Result {
    bool succeeded = false;
    Error error = Error::Failed;
    Entry entry;
    QUrl folder;
    QList<QUrl> outputs;
};

PDFSplitController::PDFSplitController(QObject *parent) : QObject(parent)
{
    connect(qApp, &QCoreApplication::aboutToQuit, this, &PDFSplitController::cancel);
}
PDFSplitController::~PDFSplitController() { cancel(); }
QUrl PDFSplitController::source() const { return QUrl::fromLocalFile(m_source.path); }
QString PDFSplitController::fileName() const { return QFileInfo(m_source.path).fileName(); }
int PDFSplitController::pageCount() const { return m_source.pageCount; }
bool PDFSplitController::busy() const { return bool(m_work); }
QString PDFSplitController::statusText() const { return m_statusText; }
qreal PDFSplitController::progress() const { return m_progress; }
int PDFSplitController::mode() const { return m_mode; }
int PDFSplitController::pagesPerFile() const { return m_pagesPerFile; }
QString PDFSplitController::pageRanges() const { return m_pageRanges; }
int PDFSplitController::outputCount() const { return m_outputCount; }
bool PDFSplitController::canSplit() const { return !busy() && outputCount() > 0; }

void PDFSplitController::optionsChanged()
{
    m_progress = 0;
    m_statusText.clear();
    m_outputCount = 0;
    if (pageCount() > 0) {
        QList<QList<int>> groups;
        const Error error = pageGroups(pageCount(), m_mode, m_pagesPerFile, m_pageRanges, groups);
        if (error != Error::None)
            m_statusText = errorText(error);
        else
            m_outputCount = int(groups.size());
    }
    emit changed();
}
void PDFSplitController::setMode(int value)
{
    if (busy() || value == m_mode || value < 0 || value > 2)
        return;
    m_mode = value;
    optionsChanged();
}
void PDFSplitController::setPagesPerFile(int value)
{
    if (busy() || value == m_pagesPerFile)
        return;
    m_pagesPerFile = value;
    optionsChanged();
}
void PDFSplitController::setPageRanges(const QString &value)
{
    if (busy() || value == m_pageRanges)
        return;
    // Preserve an invalid oversized sentinel rather than silently splitting a
    // truncated expression. QML also limits editable input to this bound.
    m_pageRanges = value.left(maximumRangeLength + 1);
    optionsChanged();
}

bool PDFSplitController::reject(const QString &message)
{
    m_statusText = message;
    emit changed();
    emit statusMessage(message);
    return false;
}

bool PDFSplitController::setSource(const QUrl &url)
{
    if (busy())
        return false;
    const QFileInfo info(url.toLocalFile());
    if (!url.isLocalFile() || url.toLocalFile().contains(QChar::Null)
        || !info.isFile() || !info.isReadable() || info.size() <= 0
        || info.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) != 0)
        return reject(errorText(Error::InvalidFile));
    if (info.size() > maximumFileBytes)
        return reject(errorText(Error::Limits));
    auto work = std::make_shared<Work>();
    work->probe = true;
    work->entry = {info.canonicalFilePath(), 0, info.size(), info.lastModified(), {}};
    m_source = work->entry;
    m_progress = 0;
    m_outputCount = 0;
    //% "Reading PDF…"
    m_statusText = qtTrId("SplitPdfReading");
    return start(work);
}

void PDFSplitController::clear()
{
    if (busy()) {
        m_clearWhenFinished = true;
        cancel();
        return;
    }
    m_source = {};
    m_progress = 0;
    m_outputCount = 0;
    m_statusText.clear();
    emit changed();
}

bool PDFSplitController::splitTo(const QUrl &parentFolder)
{
    if (busy())
        return false;
    auto work = std::make_shared<Work>();
    const Error error = pageGroups(pageCount(), m_mode, m_pagesPerFile, m_pageRanges, work->groups);
    if (error != Error::None)
        return reject(errorText(error));
    const QFileInfo parent(parentFolder.toLocalFile());
    if (!parentFolder.isLocalFile() || parentFolder.toLocalFile().contains(QChar::Null)
        || !parent.isDir() || !parent.isWritable() || parent.isSymLink())
        return reject(errorText(Error::Destination));
    work->entry = m_source;
    work->parentFolder = parent.canonicalFilePath();
    m_progress = 0;
    //% "Splitting PDF…"
    m_statusText = qtTrId("SplitPdfWorking");
    return start(work);
}

bool PDFSplitController::start(const std::shared_ptr<Work> &work)
{
    m_work = work;
    m_clearWhenFinished = false;
    const QPointer<PDFSplitController> controller(this);
    auto *runnable = QRunnable::create([controller, work] {
        Result result;
        const auto report = [controller, work](qreal value) {
            QMetaObject::invokeMethod(qApp, [controller, work, value] {
                if (controller && controller->m_work == work && !work->cancelled.load()) {
                    controller->m_progress = value;
                    emit controller->changed();
                }
            }, Qt::QueuedConnection);
        };
        try {
            result = perform(work, report);
        } catch (...) {
            // Malformed input or an allocation failure must not escape a worker.
        }
        QMetaObject::invokeMethod(qApp, [controller, work, result] {
            if (controller)
                controller->complete(work, result);
        }, Qt::QueuedConnection);
    });
    if (!splitPool().tryStart(runnable)) {
        delete runnable;
        m_work.reset();
        //% "Another PDF split is finishing. Please try again in a moment."
        return reject(qtTrId("SplitPdfAlreadyRunning"));
    }
    emit changed();
    return true;
}

void PDFSplitController::cancel()
{
    if (!m_work)
        return;
    QMutexLocker lock(&m_work->commitMutex);
    if (!m_work->committed)
        m_work->cancelled.store(true);
}

PDFSplitController::Result PDFSplitController::perform(
    const std::shared_ptr<Work> &work, const std::function<void(qreal)> &report)
{
    Result result;
    const std::function<bool()> cancelled = [&] { return work->cancelled.load(); };
    // Source snapshot is separate from the output directory: it must never
    // become a stray source copy in the user's completed split folder.
    QTemporaryDir sourceStaging;
    if (cancelled() || !sourceStaging.isValid())
        return result;
    const QFileInfo before(work->entry.path);
    if (before.size() != work->entry.size || before.lastModified() != work->entry.modified) {
        result.error = Error::SourceChanged;
        return result;
    }
    const QString snapshotPath = sourceStaging.filePath(QStringLiteral("source.pdf"));
    QFile snapshot(snapshotPath);
    if (!snapshot.open(QIODevice::WriteOnly))
        return result;
    const QByteArray digest = fingerprintFile(work->entry.path, cancelled, &snapshot);
    const bool flushed = snapshot.flush();
    snapshot.close();
    const QFileInfo after(work->entry.path);
    if (!flushed || digest.isEmpty() || before.size() != after.size()
        || before.lastModified() != after.lastModified()
        || (!work->probe && digest != work->entry.fingerprint)) {
        result.error = Error::SourceChanged;
        return result;
    }
    if (cancelled())
        return result;
    PdfInfo info;
    result.error = inspectPdf(snapshotPath, info, cancelled);
    if (result.error != Error::None || cancelled())
        return result;
    result.error = Error::Failed;
    result.entry = work->entry;
    result.entry.pageCount = info.pageCount;
    result.entry.fingerprint = digest;
    if (work->probe) {
        result.succeeded = true;
        return result;
    }
    if (info.pageCount != work->entry.pageCount) {
        result.error = Error::SourceChanged;
        return result;
    }
    const QFileInfo parentBefore(work->parentFolder);
    if (!parentBefore.isDir() || parentBefore.isSymLink()
        || parentBefore.canonicalFilePath() != work->parentFolder) {
        result.error = Error::Destination;
        return result;
    }
    QTemporaryDir staging(QDir(work->parentFolder).filePath(QStringLiteral(".trepdf-split-XXXXXX")));
    if (!staging.isValid()) {
        result.error = Error::Destination;
        return result;
    }
    const QString base = outputBaseName(work->entry.path);
    QStringList outputNames;
    qint64 totalOutput = 0;
    for (qsizetype index = 0; index < work->groups.size(); ++index) {
        if (cancelled())
            return result;
        const auto &pages = work->groups.at(index);
        QStringList numbers;
        for (const int page : pages)
            numbers.append(QString::number(page + 1));
        const QString name = QStringLiteral("%1-part-%2.pdf").arg(base).arg(index + 1, 4, 10, QLatin1Char('0'));
        const QString outputPath = staging.filePath(name);
        QFile outputFile(outputPath);
        if (!outputFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            result.error = Error::Destination;
            return result;
        }
        OutputSink sink{&outputFile, &totalOutput, &cancelled, false};
        {
            QPdfJob job;
            qpdflogger_set_save(job.logger, qpdf_log_dest_custom, &OutputSink::write, &sink, false);
            const QList<QByteArray> arguments{"trepdf-split", "--empty", "--suppress-recovery",
                "--stream-data=preserve", "--decode-level=none", "--pages", snapshotPath.toUtf8(),
                numbers.join(QLatin1Char(',')).toLatin1(), "--", "-"};
            QList<const char *> argv;
            for (const auto &argument : arguments)
                argv.append(argument.constData());
            argv.append(nullptr);
            if (qpdfjob_initialize_from_argv(job.handle, argv.constData()) != 0)
                return result;
            qpdf_data output = qpdfjob_create_qpdf(job.handle);
            if (!output)
                return result;
            QPdfDocument document(output);
            if (!fixInternalLinks(output, info, pages, cancelled) || cancelled())
                return result;
            if (qpdfjob_write_qpdf(job.handle, output) != 0) {
                result.error = sink.exceeded ? Error::Limits : Error::Failed;
                return result;
            }
        }
        if (!outputFile.flush())
            return result;
        outputFile.close();
        if (cancelled())
            return result;
        {
            PDFiumProcessLock lock;
            if (cancelled())
                return result;
            auto document = PDFiumDocument::open(outputPath);
            if (!document || FPDF_GetPageCount(document) != pages.size())
                return result;
        }
        outputNames.append(name);
        report(qreal(index + 1) / qreal(work->groups.size()) * 0.95);
    }
    if (fingerprintFile(work->entry.path, cancelled) != digest) {
        result.error = Error::SourceChanged;
        return result;
    }
    QMutexLocker lock(&work->commitMutex);
    if (cancelled())
        return result;
    const QFileInfo sourceAfter(work->entry.path);
    const QFileInfo parentAfter(work->parentFolder);
    if (sourceAfter.size() != work->entry.size || sourceAfter.lastModified() != work->entry.modified) {
        result.error = Error::SourceChanged;
        return result;
    }
    if (!parentAfter.isDir() || parentAfter.isSymLink()
        || parentAfter.canonicalFilePath() != work->parentFolder) {
        result.error = Error::Destination;
        return result;
    }
    // A bounded search allows safe repeated exports. Atomic no-replace remains
    // authoritative if another process creates the candidate between checks.
    for (int suffix = 0; suffix < 10000; ++suffix) {
        const QString folderName = base + QStringLiteral("-split")
            + (suffix == 0 ? QString() : QStringLiteral("-%1").arg(suffix));
        const QString target = QDir(work->parentFolder).filePath(folderName);
        const QFileInfo candidate(target);
        if (candidate.exists() || candidate.isSymLink())
            continue;
        if (publishDirectory(staging.path(), target)) {
            staging.setAutoRemove(false);
            work->committed = true;
            result.folder = QUrl::fromLocalFile(target);
            for (const auto &name : outputNames)
                result.outputs.append(QUrl::fromLocalFile(QDir(target).filePath(name)));
            result.succeeded = true;
            return result;
        }
        const QFileInfo raced(target);
        if (!raced.exists() && !raced.isSymLink())
            break;
    }
    result.error = Error::Destination;
    return result;
}

void PDFSplitController::complete(const std::shared_ptr<Work> &work, const Result &result)
{
    if (m_work != work)
        return;
    const bool succeeded = result.succeeded && (!work->cancelled.load() || work->committed);
    if (succeeded && work->probe) {
        m_source = result.entry;
        m_statusText.clear();
    } else if (succeeded) {
        m_progress = 1;
        //% "Saved %1 PDF files in %2."
        m_statusText = qtTrId("SplitPdfSucceeded").arg(result.outputs.size()).arg(result.folder.toLocalFile());
    } else if (work->cancelled.load()) {
        //% "PDF split cancelled. Your original file has not been changed."
        m_statusText = qtTrId("SplitPdfCancelled");
    } else {
        m_statusText = errorText(result.error);
    }
    const bool clearRequested = m_clearWhenFinished;
    m_clearWhenFinished = false;
    m_work.reset();
    if (clearRequested) {
        m_source = {};
        m_progress = 0;
        m_outputCount = 0;
        m_statusText.clear();
    } else if (succeeded && work->probe) {
        QList<QList<int>> groups;
        const Error error = pageGroups(pageCount(), m_mode, m_pagesPerFile, m_pageRanges, groups);
        if (error != Error::None)
            m_statusText = errorText(error);
        else
            m_outputCount = int(groups.size());
    }
    emit changed();
    if (!m_statusText.isEmpty())
        emit statusMessage(m_statusText);
    if (!work->probe) {
        if (succeeded)
            emit completed(result.folder, result.outputs);
        emit finished(succeeded, result.folder);
    }
}
