#include "PDFOpenTypeFontWriter.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QScopeGuard>
#include <qpdf/qpdf-c.h>
#include <zlib.h>

namespace {
struct Document {
    qpdf_data handle = qpdf_init();
    ~Document()
    {
        if (handle)
            qpdf_get_error(handle);
        qpdf_cleanup(&handle);
    }
};

// Read only bounded, standard PDFium font streams. Decompression writes into a
// fixed-size output buffer, so a source PDF cannot supply a decompression bomb.
QByteArray fontBytes(qpdf_data pdf, qpdf_oh stream, qsizetype maximum, qint64 &budget)
{
    const auto dictionary = qpdf_oh_get_dict(pdf, stream);
    const auto length = qpdf_oh_get_key(pdf, dictionary, "/Length");
    if (!qpdf_oh_is_integer(pdf, length))
        return {};
    const auto size = qpdf_oh_get_int_value(pdf, length);
    if (size <= 0 || size > maximum + 65536 || size > budget)
        return {};
    budget -= size;
    const auto filter = qpdf_oh_get_key(pdf, dictionary, "/Filter");
    const bool compressed = qpdf_oh_is_name_and_equals(pdf, filter, "/FlateDecode");
    if ((!qpdf_oh_is_null(pdf, filter) && !compressed)
        || !qpdf_oh_is_null(pdf, qpdf_oh_get_key(pdf, dictionary, "/DecodeParms")))
        return {};
    unsigned char *raw = nullptr;
    size_t rawSize = 0;
    const auto release = qScopeGuard([&]() { qpdf_oh_free_buffer(&raw); });
    // qpdf decrypts raw streams: their bytes may be shorter than the encrypted
    // /Length because of the encryption IV and padding.
    if (qpdf_oh_get_stream_data(pdf, stream, qpdf_dl_none, nullptr, &raw, &rawSize)
            != QPDF_SUCCESS || rawSize == 0 || rawSize > size_t(size))
        return {};
    if (!compressed)
        return rawSize <= size_t(maximum)
            ? QByteArray(reinterpret_cast<const char *>(raw), qsizetype(rawSize)) : QByteArray();
    // Charge the full attempted output, including failed decompression, to keep
    // cumulative CPU and allocation work bounded across all font descriptors.
    if (maximum > budget)
        return {};
    budget -= maximum;
    QByteArray decoded(maximum, Qt::Uninitialized);
    uLongf decodedSize = uLongf(maximum);
    if (uncompress(reinterpret_cast<Bytef *>(decoded.data()), &decodedSize,
                   raw, uLong(rawSize)) != Z_OK)
        return {};
    decoded.resize(qsizetype(decodedSize));
    return decoded;
}
}

bool PDFOpenTypeFontWriter::finalize(const QString &sourcePath, const QString &targetPath,
                                    const QList<QByteArray> &fonts, const QByteArray &password) try
{
    if (fonts.isEmpty() || fonts.size() > 128
        || QFileInfo(sourcePath).absoluteFilePath() == QFileInfo(targetPath).absoluteFilePath())
        return false;
    QSet<QByteArray> expected;
    qsizetype maximum = 0;
    qint64 bytes = 0;
    for (const auto &font : fonts) {
        bytes += font.size();
        if (!font.startsWith("OTTO") || font.size() > 16 * 1024 * 1024
            || bytes > 64 * 1024 * 1024)
            return false;
        maximum = qMax(maximum, font.size());
        expected.insert(QCryptographicHash::hash(font, QCryptographicHash::Sha256));
    }
    Document document;
    const auto pdf = document.handle;
    if (!pdf)
        return false;
    qpdf_silence_errors(pdf);
    qpdf_set_suppress_warnings(pdf, QPDF_TRUE);
    qpdf_set_attempt_recovery(pdf, QPDF_FALSE);
    if (qpdf_read(pdf, QFile::encodeName(sourcePath).constData(), password.constData()) != QPDF_SUCCESS)
        return false;
    const auto size = qpdf_oh_get_key(pdf, qpdf_get_trailer(pdf), "/Size");
    if (!qpdf_oh_is_integer(pdf, size))
        return false;
    const auto count = qpdf_oh_get_int_value(pdf, size);
    if (count <= 0 || count > 1000000)
        return false;
    QSet<QByteArray> found;
    qint64 scanBudget = 128 * 1024 * 1024;
    // FPDF_NO_INCREMENTAL serialization gives all in-use objects generation 0.
    // Each iteration releases C wrapper handles; the objects remain document-owned.
    for (int id = 1; id < count; ++id) {
        const auto release = qScopeGuard([&]() { qpdf_oh_release_all(pdf); });
        const auto descriptor = qpdf_get_object_by_id(pdf, id, 0);
        if (!qpdf_oh_is_dictionary_of_type(pdf, descriptor, "/FontDescriptor", nullptr))
            continue;
        const auto stream = qpdf_oh_get_key(pdf, descriptor, "/FontFile");
        if (!qpdf_oh_is_stream(pdf, stream))
            continue;
        const auto data = fontBytes(pdf, stream, maximum, scanBudget);
        if (!data.startsWith("OTTO"))
            continue;
        const auto digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
        if (!expected.contains(digest))
            continue;
        if (qpdf_oh_has_key(pdf, descriptor, "/FontFile2")
            || qpdf_oh_has_key(pdf, descriptor, "/FontFile3"))
            return false;
        // ISO 32000-1 9.7.4.2: for a name-keyed CFF program, CIDs are GIDs.
        // Keep PDFium's Identity-H, widths and ToUnicode; only fix the container.
        qpdf_oh_replace_key(pdf, qpdf_oh_get_dict(pdf, stream), "/Subtype",
                            qpdf_oh_new_name(pdf, "/OpenType"));
        qpdf_oh_replace_key(pdf, descriptor, "/FontFile3", stream);
        qpdf_oh_remove_key(pdf, descriptor, "/FontFile");
        found.insert(digest);
    }
    if (found != expected || qpdf_has_error(pdf))
        return false;
    if (qpdf_init_write(pdf, QFile::encodeName(targetPath).constData()) != QPDF_SUCCESS)
        return false;
    qpdf_set_preserve_encryption(pdf, QPDF_TRUE);
    qpdf_set_minimum_pdf_version(pdf, "1.6");
    qpdf_set_object_stream_mode(pdf, qpdf_o_preserve);
    qpdf_set_stream_data_mode(pdf, qpdf_s_preserve);
    if (qpdf_write(pdf) != QPDF_SUCCESS || qpdf_has_error(pdf)) {
        QFile::remove(targetPath);
        return false;
    }
    return true;
}
catch (...) {
    QFile::remove(targetPath);
    return false;
}
