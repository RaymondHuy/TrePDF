#include "PDFPasswordWriter.h"

#include <QFile>
#include <QFileInfo>

#include <qpdf/qpdf-c.h>

namespace {

struct QPdfDocument {
    qpdf_data handle = qpdf_init();
    ~QPdfDocument()
    {
        if (handle)
            qpdf_get_error(handle); // Consume errors so cleanup does not print them.
        qpdf_cleanup(&handle);
    }
};

} // namespace

bool PDFPasswordWriter::encryptFile(const QString &sourcePath,
                                    const QString &stagingPath,
                                    const QByteArray &password,
                                    const QByteArray &sourcePassword) try
{
    if (password.isEmpty() || password.size() > 127 || password.contains('\0')
        || QFileInfo(sourcePath).absoluteFilePath()
            == QFileInfo(stagingPath).absoluteFilePath()) {
        return false;
    }

    QPdfDocument document;
    if (!document.handle)
        return false;
    qpdf_silence_errors(document.handle);
    qpdf_set_suppress_warnings(document.handle, QPDF_TRUE);
    qpdf_set_attempt_recovery(document.handle, QPDF_FALSE);
    const QByteArray source = QFile::encodeName(sourcePath);
    const QByteArray target = QFile::encodeName(stagingPath);
    if (qpdf_read(document.handle, source.constData(), sourcePassword.constData())
        != QPDF_SUCCESS) {
        return false;
    }
    // Replacing the owner password with the opening password is safe only when
    // the source has no user restrictions. Never silently grant owner rights on
    // a restricted document, even if the caller opened it as its owner.
    if (qpdf_is_encrypted(document.handle)
        && (!qpdf_allow_accessibility(document.handle)
            || !qpdf_allow_extract_all(document.handle)
            || !qpdf_allow_print_high_res(document.handle)
            || !qpdf_allow_modify_all(document.handle))) {
        return false;
    }
    if (qpdf_init_write(document.handle, target.constData()) != QPDF_SUCCESS)
        return false;

    // This feature controls opening only: the same password owns the document,
    // and no printing, extraction, annotation or editing restrictions are added.
    qpdf_set_r6_encryption_parameters2(
        document.handle, password.constData(), password.constData(),
        QPDF_TRUE, QPDF_TRUE, QPDF_TRUE, QPDF_TRUE, QPDF_TRUE, QPDF_TRUE,
        qpdf_r3p_full, QPDF_TRUE);
    qpdf_set_object_stream_mode(document.handle, qpdf_o_preserve);
    qpdf_set_stream_data_mode(document.handle, qpdf_s_preserve);
    if (qpdf_has_error(document.handle)
        || qpdf_write(document.handle) != QPDF_SUCCESS) {
        QFile::remove(stagingPath);
        return false;
    }
    return true;
}
catch (...)
{
    // Some qpdf C configuration calls can throw (for example, crypto setup).
    // Keep the save transactional and never expose backend diagnostics/passwords.
    QFile::remove(stagingPath);
    return false;
}
