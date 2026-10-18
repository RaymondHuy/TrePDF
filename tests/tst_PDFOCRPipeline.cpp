#include "PDFOCRPipeline.h"
#include "OCRLanguageManager.h"
#include "PDFiumDocument.h"
#include "PDFiumSynchronization.h"

#include <QFile>
#include <QDir>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>
#include <qpdf/qpdf-c.h>

namespace {

struct Writer : FPDF_FILEWRITE {
    QFile *file;
    explicit Writer(QFile &output) : file(&output)
    {
        version = 1;
        WriteBlock = [](FPDF_FILEWRITE *self, const void *data, unsigned long size) -> int {
            return static_cast<Writer *>(self)->file->write(static_cast<const char *>(data), size)
                == qint64(size);
        };
    }
};

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool save(FPDF_DOCUMENT document, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    Writer writer(file);
    return FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
}

QImage scanImage(bool vietnamese)
{
    QImage image(1600, 640, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(64);
    painter.setFont(font);
    painter.setPen(Qt::black);
    painter.drawText(100, 180, QStringLiteral("SEARCHABLE DOCUMENT"));
    painter.drawText(100, 300, vietnamese ? QStringLiteral("Tiếng Việt có dấu")
                                         : QStringLiteral("Invoice number 12345"));
    painter.end();
    return image;
}

// A raster-only source whose visible orientation is always upright. Varying
// intrinsic rotation and CropBox tests inverse geometry, independently of OCR's
// orientation-detection support (v1 intentionally does not enable OSD).
bool createPdf(const QString &path, int rotation = 0, bool crop = false,
               bool vietnamese = false, bool sourceText = false, int pages = 1,
               bool huge = false, bool blank = false)
{
    const QImage image = scanImage(vietnamese);
    PDFiumProcessLock lock;
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document)
        return false;
    bool ok = true;
    for (int index = 0; index < pages && ok; ++index) {
        const double width = huge ? 10000 : (rotation % 2 ? 240 : 600);
        const double height = huge ? 10000 : (rotation % 2 ? 600 : 240);
        FPDF_PAGE page = FPDFPage_New(document, index, width + (crop ? 100 : 0),
                                    height + (crop ? 100 : 0));
        if (!page) { ok = false; break; }
        if (crop)
            FPDFPage_SetCropBox(page, 50, 50, float(width + 50), float(height + 50));
        FPDFPage_SetRotation(page, rotation);
        if (!huge && !blank) {
            FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(image.width(), image.height(), FPDFBitmap_BGRx,
                const_cast<uchar *>(image.constBits()), int(image.bytesPerLine()));
            FPDF_PAGEOBJECT object = FPDFPageObj_NewImageObj(document);
            double ox = 0, oy = 0, rx = 0, ry = 0, tx = 0, ty = 0;
            ok = bitmap && object
                && FPDF_DeviceToPage(page, 0, 0, 600, 240, 0, 0, 240, &ox, &oy)
                && FPDF_DeviceToPage(page, 0, 0, 600, 240, 0, 600, 240, &rx, &ry)
                && FPDF_DeviceToPage(page, 0, 0, 600, 240, 0, 0, 0, &tx, &ty)
                && FPDFImageObj_SetBitmap(nullptr, 0, object, bitmap)
                && FPDFImageObj_SetMatrix(object, rx - ox, ry - oy, tx - ox, ty - oy, ox, oy);
            if (ok)
                FPDFPage_InsertObject(page, object);
            else if (object)
                FPDFPageObj_Destroy(object);
            if (sourceText && ok) {
                FPDF_PAGEOBJECT text = FPDFPageObj_NewTextObj(document, "Helvetica", 10);
                const QString content = QStringLiteral("Existing text must remain");
                ok = text && FPDFText_SetText(text, reinterpret_cast<FPDF_WIDESTRING>(content.utf16()));
                if (ok) {
                    FPDFPageObj_Transform(text, 1, 0, 0, 1, 20, 20);
                    FPDFPage_InsertObject(page, text);
                } else if (text) {
                    FPDFPageObj_Destroy(text);
                }
            }
            if (ok)
                ok = FPDFPage_GenerateContent(page);
            if (bitmap)
                FPDFBitmap_Destroy(bitmap);
        }
        FPDF_ANNOTATION annotation = FPDFPage_CreateAnnot(page, FPDF_ANNOT_TEXT);
        if (annotation) {
            const FS_RECTF rect{2, 18, 18, 2};
            const QString contents = QStringLiteral("Preserve this annotation");
            ok = ok && FPDFAnnot_SetRect(annotation, &rect)
                && FPDFAnnot_SetStringValue(annotation, "Contents",
                    reinterpret_cast<FPDF_WIDESTRING>(contents.utf16()));
            FPDFPage_CloseAnnot(annotation);
        } else {
            ok = false;
        }
        FPDF_ClosePage(page);
    }
    ok = ok && save(document, path);
    FPDF_CloseDocument(document);
    return ok;
}

struct Snapshot {
    QString text;
    QImage image;
    QRectF firstWord;
    int annotations = 0;
    int pageCount = 0;
};

Snapshot inspect(const QString &path, int index = 0)
{
    Snapshot result;
    PDFiumProcessLock lock;
    PDFiumDocument document = PDFiumDocument::open(path);
    if (!document)
        return result;
    result.pageCount = FPDF_GetPageCount(document);
    FPDF_PAGE page = FPDF_LoadPage(document, index);
    if (!page)
        return result;
    FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
    if (text) {
        const int count = FPDFText_CountChars(text);
        QVector<unsigned short> buffer(count + 1);
        const int copied = FPDFText_GetText(text, 0, count, buffer.data());
        if (copied > 0)
            result.text = QString::fromUtf16(reinterpret_cast<const char16_t *>(buffer.data()), copied - 1);
        for (int i = 0; i < count && i < 10; ++i) {
            double left = 0, right = 0, bottom = 0, top = 0;
            if (FPDFText_GetCharBox(text, i, &left, &right, &bottom, &top)) {
                int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                FPDF_PageToDevice(page, 0, 0, 1200, 480, 0, left, top, &x1, &y1);
                FPDF_PageToDevice(page, 0, 0, 1200, 480, 0, right, bottom, &x2, &y2);
                const QRectF rect = QRectF(QPointF(x1, y1), QPointF(x2, y2)).normalized();
                result.firstWord = result.firstWord.isNull() ? rect : result.firstWord.united(rect);
            }
        }
        FPDFText_ClosePage(text);
    }
    result.annotations = FPDFPage_GetAnnotCount(page);
    result.image = QImage(1200, 480, QImage::Format_RGB32);
    result.image.fill(Qt::white);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(1200, 480, FPDFBitmap_BGRx,
        result.image.bits(), int(result.image.bytesPerLine()));
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, 1200, 480, 0, FPDF_ANNOT);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
    return result;
}

PDFOCRPipeline::Request request(const QString &source, const QString &language = QStringLiteral("eng"))
{
    PDFOCRPipeline::Request value;
    value.sourcePath = source;
    value.sourcePages = {0};
    value.language = language;
    value.tessdataPath = QString::fromUtf8(TREPDF_OCR_TESSDATA_DIR);
    value.cancellation = std::make_shared<std::atomic_bool>(false);
    return value;
}

struct QPdf {
    qpdf_data handle = qpdf_init();
    QPdf() { qpdf_silence_errors(handle); qpdf_set_suppress_warnings(handle, QPDF_TRUE); }
    ~QPdf() { qpdf_get_error(handle); qpdf_cleanup(&handle); }
};

bool decorate(const QString &source, const QString &output, bool encrypted)
{
    QPdf document;
    const QByteArray inputPath = QFile::encodeName(source), outputPath = QFile::encodeName(output);
    if (qpdf_read(document.handle, inputPath.constData(), "") != QPDF_SUCCESS)
        return false;
    const auto page = qpdf_get_page_n(document.handle, 0);
    const auto field = qpdf_make_indirect_object(document.handle, qpdf_oh_parse(document.handle,
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (RetainedField) /V (Original value)"
        " /Rect [20 5 150 20] /F 4 >>"));
    const auto annots = qpdf_oh_get_key(document.handle, page, "/Annots");
    qpdf_oh_append_item(document.handle, annots, field);
    const auto fields = qpdf_oh_new_array(document.handle);
    qpdf_oh_append_item(document.handle, fields, field);
    const auto form = qpdf_oh_new_dictionary(document.handle);
    qpdf_oh_replace_key(document.handle, form, "/Fields", fields);
    qpdf_oh_replace_key(document.handle, qpdf_get_root(document.handle), "/AcroForm", form);
    const auto info = qpdf_oh_new_dictionary(document.handle);
    qpdf_oh_replace_key(document.handle, info, "/Title",
                        qpdf_oh_new_unicode_string(document.handle, "Tài liệu gốc"));
    qpdf_oh_replace_key(document.handle, qpdf_get_trailer(document.handle), "/Info",
                        qpdf_make_indirect_object(document.handle, info));
    if (qpdf_init_write(document.handle, outputPath.constData()) != QPDF_SUCCESS)
        return false;
    if (encrypted) {
        qpdf_set_r6_encryption_parameters2(document.handle, "secret", "secret", QPDF_TRUE,
            QPDF_TRUE, QPDF_TRUE, QPDF_TRUE, QPDF_TRUE, QPDF_TRUE, qpdf_r3p_full, QPDF_TRUE);
    }
    qpdf_set_stream_data_mode(document.handle, qpdf_s_preserve);
    return !qpdf_has_error(document.handle) && qpdf_write(document.handle) == QPDF_SUCCESS;
}

struct Structure {
    QList<QByteArray> contentStreams;
    QByteArray fieldValue;
    QByteArray title;
    bool encrypted = false;
};

Structure structure(const QString &path, const QByteArray &password = {})
{
    Structure result;
    QPdf document;
    const QByteArray encodedPath = QFile::encodeName(path);
    if (qpdf_read(document.handle, encodedPath.constData(), password.constData()) != QPDF_SUCCESS)
        return result;
    result.encrypted = qpdf_is_encrypted(document.handle);
    const auto page = qpdf_get_page_n(document.handle, 0);
    const auto contents = qpdf_oh_get_key(document.handle, page, "/Contents");
    const int count = qpdf_oh_is_array(document.handle, contents)
        ? qpdf_oh_get_array_n_items(document.handle, contents) : 1;
    for (int i = 0; i < count; ++i) {
        const auto stream = qpdf_oh_is_array(document.handle, contents)
            ? qpdf_oh_get_array_item(document.handle, contents, i) : contents;
        unsigned char *bytes = nullptr;
        size_t length = 0;
        if (qpdf_oh_get_stream_data(document.handle, stream, qpdf_dl_none, nullptr,
                                    &bytes, &length) == QPDF_SUCCESS) {
            result.contentStreams.append(QByteArray(reinterpret_cast<char *>(bytes), qsizetype(length)));
        }
        qpdf_oh_free_buffer(&bytes);
    }
    const auto form = qpdf_oh_get_key(document.handle, qpdf_get_root(document.handle), "/AcroForm");
    const auto fields = qpdf_oh_get_key(document.handle, form, "/Fields");
    const auto field = qpdf_oh_get_array_item(document.handle, fields, 0);
    result.fieldValue = qpdf_oh_get_utf8_value(document.handle, qpdf_oh_get_key(document.handle, field, "/V"));
    const auto info = qpdf_oh_get_key(document.handle, qpdf_get_trailer(document.handle), "/Info");
    result.title = qpdf_oh_get_utf8_value(document.handle, qpdf_oh_get_key(document.handle, info, "/Title"));
    return result;
}

} // namespace

class PDFOCRPipelineTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        PDFiumProcessLock lock;
        FPDF_InitLibrary();
    }

    void cleanupTestCase()
    {
        PDFiumProcessLock lock;
        FPDF_DestroyLibrary();
    }

    void actualOcrPreservesVisualsAndGeometry_data()
    {
        QTest::addColumn<int>("rotation");
        QTest::addColumn<bool>("crop");
        QTest::addColumn<bool>("vietnamese");
        QTest::newRow("english") << 0 << false << false;
        QTest::newRow("vietnamese") << 0 << false << true;
        QTest::newRow("crop") << 0 << true << false;
        QTest::newRow("rotation90-crop") << 1 << true << false;
        QTest::newRow("rotation180") << 2 << false << false;
        QTest::newRow("rotation270-crop") << 3 << true << false;
    }

    void actualOcrPreservesVisualsAndGeometry()
    {
        QFETCH(int, rotation);
        QFETCH(bool, crop);
        QFETCH(bool, vietnamese);
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("source.pdf"));
        const QString output = directory.filePath(QStringLiteral("searchable.pdf"));
        QVERIFY(createPdf(source, rotation, crop, vietnamese));
        const QByteArray original = read(source);
        const Snapshot before = inspect(source);
        QVERIFY(before.text.trimmed().isEmpty());
        int completed = 0;
        const auto result = PDFOCRPipeline::run(request(source, vietnamese ? QStringLiteral("eng+vie")
                                                                         : QStringLiteral("eng")),
            output, [&](int value) { completed = value; QVERIFY(!pdfiumProcessLockOwnedByCurrentThread()); });
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::Succeeded);
        QCOMPARE(result.recognizedPages, 1);
        QCOMPARE(completed, 1);
        QCOMPARE(read(source), original);
        const Snapshot after = inspect(output);
        QVERIFY2(after.text.contains(QStringLiteral("SEARCHABLE DOCUMENT")), qPrintable(after.text));
        if (vietnamese)
            QVERIFY2(after.text.contains(QStringLiteral("Tiếng Việt có")), qPrintable(after.text));
        else
            QVERIFY2(after.text.contains(QStringLiteral("12345")), qPrintable(after.text));
        QCOMPARE(after.image, before.image);
        QCOMPARE(after.annotations, before.annotations);
        QCOMPARE(after.pageCount, 1);
        // The scanned first line begins at x=75 and spans y~=90..140 in this
        // 1200x480 inspection raster. Glyphless font metrics need not match the
        // printed ascenders exactly, but the selectable line must overlap it.
        QVERIFY2(after.firstWord.left() > 60 && after.firstWord.left() < 90,
                 qPrintable(QString::number(after.firstWord.left())));
        QVERIFY(after.firstWord.top() < 140 && after.firstWord.bottom() > 90);
    }

    void skipsExistingAndRepeatedOcr()
    {
        QTemporaryDir directory;
        const QString existing = directory.filePath(QStringLiteral("existing.pdf"));
        const QString output = directory.filePath(QStringLiteral("output.pdf"));
        QVERIFY(createPdf(existing, 0, false, false, true));
        auto result = PDFOCRPipeline::run(request(existing), output);
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::NoText);
        QCOMPARE(result.skippedPages, 1);
        QVERIFY(!QFile::exists(output));
        const QString scan = directory.filePath(QStringLiteral("scan.pdf"));
        QVERIFY(createPdf(scan));
        result = PDFOCRPipeline::run(request(scan), output);
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::Succeeded);
        const QString second = directory.filePath(QStringLiteral("second.pdf"));
        result = PDFOCRPipeline::run(request(output), second);
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::NoText);
        QCOMPARE(result.skippedPages, 1);
        QVERIFY(!QFile::exists(second));
    }

    void selectedPagesAndCancellationAreTransactional()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("multi.pdf"));
        const QString output = directory.filePath(QStringLiteral("output.pdf"));
        QVERIFY(createPdf(source, 0, false, false, false, 3));
        auto input = request(source);
        input.sourcePages = {1};
        auto result = PDFOCRPipeline::run(input, output);
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::Succeeded);
        QCOMPARE(inspect(output).pageCount, 3);
        QVERIFY(inspect(output, 0).text.trimmed().isEmpty());
        QVERIFY(inspect(output, 1).text.contains(QStringLiteral("SEARCHABLE")));
        QVERIFY(inspect(output, 2).text.trimmed().isEmpty());
        input.sourcePages = {0, 1, 2};
        const QString cancelledOutput = directory.filePath(QStringLiteral("cancelled.pdf"));
        result = PDFOCRPipeline::run(input, cancelledOutput, [&](int completed) {
            if (completed == 1)
                input.cancellation->store(true);
        });
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::Cancelled);
        QCOMPARE(result.recognizedPages, 1);
        QVERIFY(!QFile::exists(cancelledOutput));
        result = PDFOCRPipeline::run(input, cancelledOutput);
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::Cancelled);
        QVERIFY(!QFile::exists(cancelledOutput));
    }

    void preservesFormsStreamsMetadataAndEncryption_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::newRow("plain") << false;
        QTest::newRow("password-protected") << true;
    }

    void preservesFormsStreamsMetadataAndEncryption()
    {
        QFETCH(bool, encrypted);
        QTemporaryDir directory;
        const QString raster = directory.filePath(QStringLiteral("raster.pdf"));
        const QString source = directory.filePath(QStringLiteral("Tài liệu gốc.pdf"));
        const QString output = directory.filePath(QStringLiteral("Tài liệu OCR.pdf"));
        QVERIFY(createPdf(raster));
        QVERIFY(decorate(raster, source, encrypted));
        auto input = request(source);
        if (encrypted)
            input.password = "secret";
        const auto before = structure(source, input.password);
        QVERIFY(!before.contentStreams.isEmpty());
        QCOMPARE(before.fieldValue, QByteArray("Original value"));
        const auto result = PDFOCRPipeline::run(input, output);
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::Succeeded);
        const auto after = structure(output, input.password);
        QCOMPARE(after.encrypted, encrypted);
        QCOMPARE(after.title, before.title);
        QCOMPARE(after.fieldValue, before.fieldValue);
        QVERIFY(after.contentStreams.size() > before.contentStreams.size());
        QCOMPARE(after.contentStreams.sliced(after.contentStreams.size() - before.contentStreams.size()),
                 before.contentStreams);
        PDFiumProcessLock lock;
        auto document = PDFiumDocument::open(output, input.password);
        QVERIFY(document);
        FPDF_PAGE page = FPDF_LoadPage(document, 0);
        QVERIFY(page);
        QCOMPARE(FPDFPage_GetAnnotCount(page), 2);
        FPDF_ClosePage(page);
        document.reset();
        if (encrypted) {
            document = PDFiumDocument::open(output);
            QVERIFY(!document);
        }
    }

    void rejectsInvalidRequestsAndOversizedPages()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("source.pdf"));
        const QString output = directory.filePath(QStringLiteral("output.pdf"));
        QVERIFY(createPdf(source));
        const QByteArray original = read(source);
        auto input = request(source);
        QCOMPARE(PDFOCRPipeline::run(input, source).outcome, PDFOCRPipeline::Result::Failed);
        QCOMPARE(read(source), original);
        for (const QVector<int> &pages : {QVector<int>{}, QVector<int>{-1}, QVector<int>{1}, QVector<int>{0, 0}}) {
            input.sourcePages = pages;
            QCOMPARE(PDFOCRPipeline::run(input, output).outcome, PDFOCRPipeline::Result::Failed);
            QVERIFY(!QFile::exists(output));
        }
        input = request(source, QStringLiteral("../eng"));
        QCOMPARE(PDFOCRPipeline::run(input, output).outcome, PDFOCRPipeline::Result::Failed);
        input = request(source);
        input.tessdataPath = directory.filePath(QStringLiteral("missing-models"));
        QCOMPARE(PDFOCRPipeline::run(input, output).outcome, PDFOCRPipeline::Result::Failed);
        const QString huge = directory.filePath(QStringLiteral("huge.pdf"));
        QVERIFY(createPdf(huge, 0, false, false, false, 1, true));
        QCOMPARE(PDFOCRPipeline::run(request(huge), output).outcome, PDFOCRPipeline::Result::Failed);
        QVERIFY(!QFile::exists(output));
        const QString blank = directory.filePath(QStringLiteral("blank.pdf"));
        QVERIFY(createPdf(blank, 0, false, false, false, 1, false, true));
        const auto result = PDFOCRPipeline::run(request(blank), output);
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::NoText);
        QCOMPARE(result.emptyPages, 1);
        QVERIFY(!QFile::exists(output));
    }

    void rejectsUnsafeLanguageSelections_data()
    {
        QTest::addColumn<QString>("language");
        QTest::newRow("empty") << QString();
        QTest::newRow("unknown") << QStringLiteral("zzz");
        QTest::newRow("traversal") << QStringLiteral("../eng");
        QTest::newRow("absolute-path") << QStringLiteral("/eng");
        QTest::newRow("duplicate") << QStringLiteral("vie+eng+vie");
        QTest::newRow("empty-component") << QStringLiteral("eng++vie");
        QTest::newRow("over-limit") << QStringLiteral("eng+vie+fra+tha+hrv");
    }

    void rejectsUnsafeLanguageSelections()
    {
        QFETCH(QString, language);
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("source.pdf"));
        const QString output = directory.filePath(QStringLiteral("output.pdf"));
        QVERIFY(createPdf(source));
        const QByteArray original = read(source);
        QCOMPARE(PDFOCRPipeline::run(request(source, language), output).outcome,
                 PDFOCRPipeline::Result::Failed);
        QVERIFY(!QFile::exists(output));
        QCOMPARE(read(source), original);
    }

    void snapshotsModelsBeforeRecognitionAndPreservesLanguageOrder()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("source.pdf"));
        const QString output = directory.filePath(QStringLiteral("output.pdf"));
        QVERIFY(createPdf(source, 0, false, true, false, 2));
        auto input = request(source, QStringLiteral("vie+eng"));
        input.sourcePages = {0, 1};
        const QDir bundled(input.tessdataPath);
        for (const QString &code : {QStringLiteral("eng"), QStringLiteral("vie")}) {
            const QString filename = code + QStringLiteral(".traineddata");
            const QString temporaryModel = directory.filePath(filename);
            QVERIFY(QFile::copy(bundled.filePath(filename), temporaryModel));
            input.modelPaths.insert(code, temporaryModel);
        }
        const auto result = PDFOCRPipeline::run(input, output, [&](int completed) {
            if (completed == 1) {
                // Removing the source cache after initialization must not alter
                // the second page or the renderer's private language data.
                for (const QString &path : std::as_const(input.modelPaths))
                    QVERIFY(QFile::remove(path));
            }
        });
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::Succeeded);
        QCOMPARE(result.recognizedPages, 2);
        QVERIFY(inspect(output, 1).text.contains(QStringLiteral("Tiếng Việt có")));
    }

    void missingOrCorruptModelsDoNotWriteOutput_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char *mode : {"missing", "truncated", "corrupt", "symlink", "incomplete-map", "extra-map", "bad-font"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void missingOrCorruptModelsDoNotWriteOutput()
    {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("source.pdf"));
        const QString output = directory.filePath(QStringLiteral("output.pdf"));
        QVERIFY(createPdf(source));
        const QByteArray original = read(source);
        auto input = request(source);
        const QDir bundled(input.tessdataPath);
        const QString model = directory.filePath(QStringLiteral("eng.traineddata"));
        input.modelPaths.insert(QStringLiteral("eng"), model);
        if (mode != QStringLiteral("missing") && mode != QStringLiteral("symlink")) {
            QVERIFY(QFile::copy(bundled.filePath(QStringLiteral("eng.traineddata")), model));
            QFile writable(model);
            QVERIFY(writable.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        }
        if (mode == QStringLiteral("truncated")) {
            QFile file(model);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.resize(8));
        } else if (mode == QStringLiteral("corrupt")) {
            QFile file(model);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QByteArray byte = file.read(1);
            QCOMPARE(byte.size(), 1);
            byte[0] = char(byte[0] ^ 0xff);
            QVERIFY(file.seek(0));
            QCOMPARE(file.write(byte), 1);
        } else if (mode == QStringLiteral("symlink")) {
            if (!QFile::link(bundled.filePath(QStringLiteral("eng.traineddata")), model))
                QSKIP("Symbolic links are unavailable on this platform.");
        } else if (mode == QStringLiteral("incomplete-map")) {
            input.language = QStringLiteral("eng+vie");
        } else if (mode == QStringLiteral("extra-map")) {
            input.modelPaths.insert(QStringLiteral("vie"), bundled.filePath(QStringLiteral("vie.traineddata")));
        } else if (mode == QStringLiteral("bad-font")) {
            input.tessdataPath = directory.path();
            QFile font(directory.filePath(QStringLiteral("pdf.ttf")));
            QVERIFY(font.open(QIODevice::WriteOnly));
            QCOMPARE(font.write(QByteArray(572, 'x')), 572);
        }
        QCOMPARE(PDFOCRPipeline::run(input, output).outcome, PDFOCRPipeline::Result::Failed);
        QVERIFY(!QFile::exists(output));
        QCOMPARE(read(source), original);

        // A destination that already exists is rejected before any staging
        // cleanup; even failed model validation must preserve these bytes.
        QFile existing(output);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        const QByteArray previous("existing destination");
        QCOMPARE(existing.write(previous), previous.size());
        existing.close();
        QCOMPARE(PDFOCRPipeline::run(input, output).outcome, PDFOCRPipeline::Result::Failed);
        QCOMPARE(read(output), previous);
    }

    void recognizesWithDownloadedLanguageFixture()
    {
        // Optional local fixture supplied by release/feature validation. Never
        // download models from a test or alter the user's installed cache.
        const QString fixture = qEnvironmentVariable("TREPDF_TEST_FRA_MODEL");
        if (fixture.isEmpty())
            QSKIP("Set TREPDF_TEST_FRA_MODEL to a pinned French model to exercise downloaded models.");
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("source.pdf"));
        const QString output = directory.filePath(QStringLiteral("output.pdf"));
        QVERIFY(createPdf(source));
        auto input = request(source, QStringLiteral("fra+eng"));
        input.modelPaths.insert(QStringLiteral("fra"), fixture);
        input.modelPaths.insert(QStringLiteral("eng"),
                               QDir(input.tessdataPath).filePath(QStringLiteral("eng.traineddata")));
        const auto result = PDFOCRPipeline::run(input, output);
        QCOMPARE(result.outcome, PDFOCRPipeline::Result::Succeeded);
        QVERIFY(inspect(output).text.contains(QStringLiteral("SEARCHABLE DOCUMENT")));
    }
};

QTEST_MAIN(PDFOCRPipelineTest)
#include "tst_PDFOCRPipeline.moc"
