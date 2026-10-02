#include "PDFImageFormResources.h"

#include <QFile>
#include <QFileInfo>
#include <QList>
#include <qpdf/qpdf-c.h>

namespace {
struct Document {
    qpdf_data handle = qpdf_init();
    ~Document()
    {
        if (handle)
            qpdf_get_error(handle);
        qpdf_cleanup(&handle);
    }
    bool open(const QString &path, const QByteArray &password)
    {
        if (!handle)
            return false;
        qpdf_silence_errors(handle);
        qpdf_set_suppress_warnings(handle, QPDF_TRUE);
        qpdf_set_attempt_recovery(handle, QPDF_FALSE);
        return qpdf_read(handle, QFile::encodeName(path).constData(), password.constData())
            == QPDF_SUCCESS;
    }
};

bool dictionaryKeys(qpdf_data pdf, qpdf_oh dictionary, QList<QByteArray> &keys, int &budget)
{
    if (!qpdf_oh_is_dictionary(pdf, dictionary))
        return false;
    qpdf_oh_begin_dict_key_iter(pdf, dictionary);
    while (qpdf_oh_dict_more_keys(pdf)) {
        if (--budget < 0)
            return false;
        const char *key = qpdf_oh_dict_next_key(pdf);
        if (!key)
            return false;
        keys.append(QByteArray(key));
    }
    return !qpdf_has_error(pdf);
}

qpdf_oh resourcesForPage(qpdf_data pdf, qpdf_oh page)
{
    // Resources may be inherited from the page tree, but not merged between
    // ancestors. Stop at the first non-null dictionary, as PDF readers do.
    for (int depth = 0; depth < 64 && qpdf_oh_is_dictionary(pdf, page); ++depth) {
        const auto resources = qpdf_oh_get_key(pdf, page, "/Resources");
        if (!qpdf_oh_is_null(pdf, resources))
            return resources;
        page = qpdf_oh_get_key(pdf, page, "/Parent");
    }
    return qpdf_oh_new_null(pdf);
}

qpdf_oh copyDictionary(qpdf_data pdf, qpdf_oh dictionary, int &budget)
{
    const auto copy = qpdf_oh_new_dictionary(pdf);
    if (qpdf_oh_is_null(pdf, dictionary))
        return copy;
    QList<QByteArray> keys;
    if (!dictionaryKeys(pdf, dictionary, keys, budget))
        return 0;
    for (const auto &key : keys)
        qpdf_oh_replace_key(pdf, copy, key.constData(),
                            qpdf_oh_get_key(pdf, dictionary, key.constData()));
    return copy;
}
} // namespace

bool PDFImageFormResources::restoreMissing(
    const QString &sourcePath, const QString &stagingPath, const QString &outputPath,
    const QMap<int, int> &pageMap, const QByteArray &sourcePassword,
    const QByteArray &stagingPassword, bool &changed) try
{
    changed = false;
    if (pageMap.isEmpty())
        return true;
    if (pageMap.size() > 4096 || outputPath.isEmpty()
        || QFileInfo(outputPath).absoluteFilePath() == QFileInfo(sourcePath).absoluteFilePath()
        || QFileInfo(outputPath).absoluteFilePath() == QFileInfo(stagingPath).absoluteFilePath()) {
        return false;
    }
    Document source;
    Document output;
    if (!source.open(sourcePath, sourcePassword) || !output.open(stagingPath, stagingPassword))
        return false;
    const auto src = source.handle;
    const auto dst = output.handle;
    const int sourcePages = qpdf_get_num_pages(src);
    const int outputPages = qpdf_get_num_pages(dst);
    int budget = 65536;
    for (auto entry = pageMap.cbegin(); entry != pageMap.cend(); ++entry) {
        if (entry.key() < 0 || entry.key() >= outputPages
            || entry.value() < 0 || entry.value() >= sourcePages)
            return false;
        const auto sourcePage = qpdf_get_page_n(src, entry.value());
        const auto outputPage = qpdf_get_page_n(dst, entry.key());
        const auto original = resourcesForPage(src, sourcePage);
        const auto current = resourcesForPage(dst, outputPage);
        QList<QByteArray> categories;
        if (!dictionaryKeys(src, original, categories, budget))
            return false;
        const auto restored = copyDictionary(dst, current, budget);
        if (!restored)
            return false;
        bool pageChanged = false;
        for (const auto &category : categories) {
            const auto originalCategory = qpdf_oh_get_key(src, original, category.constData());
            if (!qpdf_oh_is_dictionary(src, originalCategory))
                continue;
            const auto currentCategory = qpdf_oh_get_key(dst, restored, category.constData());
            const auto merged = copyDictionary(dst, currentCategory, budget);
            QList<QByteArray> names;
            if (!merged || !dictionaryKeys(src, originalCategory, names, budget))
                return false;
            bool categoryChanged = false;
            for (const auto &name : names) {
                if (qpdf_oh_has_key(dst, merged, name.constData()))
                    continue;
                auto value = qpdf_oh_get_key(src, originalCategory, name.constData());
                if (!qpdf_oh_is_indirect(src, value))
                    value = qpdf_make_indirect_object(src, value);
                const auto copied = qpdf_oh_copy_foreign_object(dst, src, value);
                if (!copied || qpdf_has_error(src) || qpdf_has_error(dst))
                    return false;
                qpdf_oh_replace_key(dst, merged, name.constData(), copied);
                categoryChanged = true;
            }
            if (categoryChanged) {
                qpdf_oh_replace_key(dst, restored, category.constData(), merged);
                pageChanged = true;
            }
        }
        if (pageChanged) {
            // Own both dictionary levels so inherited/shared resource tables
            // on unrelated pages are never modified as a side effect.
            qpdf_oh_replace_key(dst, outputPage, "/Resources", restored);
            changed = true;
        }
        if (qpdf_has_error(src) || qpdf_has_error(dst))
            return false;
        qpdf_oh_release_all(src);
        qpdf_oh_release_all(dst);
    }
    if (!changed)
        return true;
    if (qpdf_init_write(dst, QFile::encodeName(outputPath).constData()) != QPDF_SUCCESS)
        return false;
    qpdf_set_preserve_encryption(dst, QPDF_TRUE);
    qpdf_set_object_stream_mode(dst, qpdf_o_preserve);
    qpdf_set_stream_data_mode(dst, qpdf_s_preserve);
    return qpdf_write(dst) == QPDF_SUCCESS && !qpdf_has_error(dst);
}
catch (...) {
    return false;
}
