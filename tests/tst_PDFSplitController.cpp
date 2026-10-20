#include "PDFDocument.h"
#include "PDFSplitController.h"
#include "PDFiumSynchronization.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>
#include <qpdf/qpdf-c.h>

namespace {
struct PdfHandle {
    qpdf_data value = qpdf_init();
    ~PdfHandle() { qpdf_cleanup(&value); }
};

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray stream(const QByteArray &data, const QByteArray &dictionary = {})
{
    return "<< /Length " + QByteArray::number(data.size()) + " " + dictionary
        + " >>\nstream\n" + data + "\nendstream";
}
bool writeObjects(const QString &path, const QList<QByteArray> &objects)
{
    QByteArray pdf("%PDF-1.7\n");
    QList<qsizetype> offsets;
    for (qsizetype i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const auto offset : offsets)
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1)
        + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return writeFile(path, pdf);
}

// Five native vector/text pages with distinct geometry and rotation. Page 1
// contains a note, direct and named internal links, and an absolute URI.
bool createPdf(const QString &path, const QByteArray &label = "Page",
               const QByteArray &catalogExtra = {}, const QByteArray &annotationExtra = {})
{
    QList<QByteArray> objects;
    objects << "<< /Type /Catalog /Pages 2 0 R /Names << /Dests << /Names [(third) [7 0 R /XYZ 12 34 1.5]] >> >> "
                   + catalogExtra + " >>"
            << "<< /Type /Pages /Count 5 /Kids [3 0 R 5 0 R 7 0 R 9 0 R 11 0 R] >>";
    for (int page = 0; page < 5; ++page) {
        const int id = 3 + 2 * page;
        const QByteArray contents = "BT /F1 12 Tf 20 200 Td (" + label + " "
            + QByteArray::number(page + 1) + ") Tj ET\n0.1 0.4 0.7 RG 20 20 80 40 re S\n"
            + (page == 0 ? QByteArray("q 20 0 0 20 20 100 cm /Im1 Do Q\n") : QByteArray());
        objects << "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "
            + QByteArray::number(300 + page * 10) + " 400] /CropBox [10 15 280 380] /Rotate "
            + QByteArray::number((page % 4) * 90) + " /Resources << /Font << /F1 13 0 R >> "
            + "/XObject << /Im1 18 0 R >> >> /Contents " + QByteArray::number(id + 1) + " 0 R "
            + (page == 0 ? QByteArray("/Annots [14 0 R 15 0 R 16 0 R 17 0 R] ") : QByteArray()) + ">>"
            << stream(contents);
    }
    objects << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
            << "<< /Type /Annot /Subtype /Text /P 3 0 R /Rect [20 50 40 70] /Contents (Keep note) /F 4 >>"
            << "<< /Type /Annot /Subtype /Link /P 3 0 R /Rect [20 80 100 95] /Dest [5 0 R /Fit] >>"
            << "<< /Type /Annot /Subtype /Link /P 3 0 R /Rect [20 110 100 125] /A << /S /GoTo /D (third) >> >>"
            << "<< /Type /Annot /Subtype /Link /P 3 0 R /Rect [20 140 100 155] /A << /S /URI /URI (https://example.com/) >> "
                + annotationExtra + " >>"
            << stream(QByteArray::fromHex("137bc9"),
                "/Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 8");
    return writeObjects(path, objects);
}

struct PageSnapshot {
    QSizeF size;
    int rotation = 0;
    QString text;
    int images = 0;
    int paths = 0;
};
QList<PageSnapshot> inspectPages(const QString &path)
{
    PDFiumProcessLock lock;
    auto document = FPDF_LoadDocument(QFile::encodeName(path).constData(), nullptr);
    if (!document)
        return {};
    QList<PageSnapshot> result;
    for (int index = 0; index < FPDF_GetPageCount(document); ++index) {
        auto page = FPDF_LoadPage(document, index);
        if (!page)
            break;
        PageSnapshot snapshot;
        snapshot.size = {FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)};
        snapshot.rotation = FPDFPage_GetRotation(page);
        auto textPage = FPDFText_LoadPage(page);
        if (textPage) {
            QVector<unsigned short> text(FPDFText_CountChars(textPage) + 1);
            const int copied = FPDFText_GetText(textPage, 0, int(text.size()) - 1, text.data());
            if (copied > 0)
                snapshot.text = QString::fromUtf16(reinterpret_cast<const char16_t *>(text.constData()), copied - 1);
            FPDFText_ClosePage(textPage);
        }
        for (int object = 0; object < FPDFPage_CountObjects(page); ++object) {
            const int type = FPDFPageObj_GetType(FPDFPage_GetObject(page, object));
            snapshot.images += type == FPDF_PAGEOBJ_IMAGE;
            snapshot.paths += type == FPDF_PAGEOBJ_PATH;
        }
        result.append(snapshot);
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(document);
    return result;
}
QStringList directoryEntries(const QString &path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}
bool createManyPages(const QString &path, int pages)
{
    QByteArray kids;
    QList<QByteArray> objects{QByteArray("<< /Type /Catalog /Pages 2 0 R >>"), {}};
    for (int index = 0; index < pages; ++index) {
        kids += QByteArray::number(index + 3) + " 0 R ";
        objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>");
    }
    objects[1] = "<< /Type /Pages /Count " + QByteArray::number(pages) + " /Kids [" + kids + "] >>";
    return writeObjects(path, objects);
}
}

class PDFSplitControllerTest : public QObject
{
    Q_OBJECT
    PDFDocument m_runtime;

private slots:
    void splitModesPreserveNativePages_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("perFile");
        QTest::addColumn<QString>("ranges");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("each-page") << 0 << 1 << QString() << QStringList{"1", "2", "3", "4", "5"};
        QTest::newRow("every-two") << 1 << 2 << QString() << QStringList{"1,2", "3,4", "5"};
        QTest::newRow("custom-source-order") << 2 << 1 << QString("5,3; 2,1") << QStringList{"3,5", "1,2"};
        QTest::newRow("whole-file") << 1 << 99 << QString() << QStringList{"1,2,3,4,5"};
    }
    void splitModesPreserveNativePages()
    {
        QFETCH(int, mode);
        QFETCH(int, perFile);
        QFETCH(QString, ranges);
        QFETCH(QStringList, expected);
        QTemporaryDir dir;
        const QString source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        const auto original = readFile(source);
        const auto originals = inspectPages(source);
        QCOMPARE(originals.size(), 5);
        PDFSplitController controller;
        controller.setMode(mode);
        controller.setPagesPerFile(perFile);
        controller.setPageRanges(ranges);
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.pageCount(), 5);
        QCOMPARE(controller.outputCount(), expected.size());
        QVERIFY(controller.canSplit());
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QSignalSpy finished(&controller, &PDFSplitController::finished);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        QCOMPARE(completed.size(), 1);
        QCOMPARE(controller.progress(), 1.0);
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        QCOMPARE(outputs.size(), expected.size());
        QCOMPARE(completed[0][0].toUrl().toLocalFile(), QFileInfo(dir.filePath("source-split")).canonicalFilePath());
        for (int file = 0; file < outputs.size(); ++file) {
            const auto actual = inspectPages(outputs[file].toLocalFile());
            const auto expectedPages = expected[file].split(',');
            QCOMPARE(actual.size(), expectedPages.size());
            for (int page = 0; page < actual.size(); ++page) {
                const auto &wanted = originals[expectedPages[page].toInt() - 1];
                QCOMPARE(actual[page].text, wanted.text);
                QCOMPARE(actual[page].size, wanted.size);
                QCOMPARE(actual[page].rotation, wanted.rotation);
                QCOMPARE(actual[page].images, wanted.images);
                QCOMPARE(actual[page].paths, wanted.paths);
            }
        }
        QCOMPARE(readFile(source), original);
        QCOMPARE(directoryEntries(dir.path()), (QStringList{"source-split", "source.pdf"}));
    }

    void remapsKeptLinksAndRemovesExcludedTargets()
    {
        QTemporaryDir dir;
        const QString source = dir.filePath("links.pdf");
        QVERIFY(createPdf(source));
        PDFSplitController controller;
        controller.setMode(2);
        controller.setPageRanges("1,3");
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        const auto outputs = qvariant_cast<QList<QUrl>>(completed[0][1]);
        PdfHandle document;
        auto pdf = document.value;
        QCOMPARE(qpdf_read(pdf, outputs[0].toLocalFile().toUtf8().constData(), ""), QPDF_SUCCESS);
        const auto page = qpdf_get_page_n(pdf, 0);
        const auto annotations = qpdf_oh_get_key(pdf, page, "/Annots");
        QCOMPARE(qpdf_oh_get_array_n_items(pdf, annotations), 4); // note, disabled link, kept named link, URI
        const auto note = qpdf_oh_get_array_item(pdf, annotations, 0);
        QCOMPARE(QByteArray(qpdf_oh_get_string_value(pdf, qpdf_oh_get_key(pdf, note, "/Contents"))), QByteArray("Keep note"));
        const auto disabled = qpdf_oh_get_array_item(pdf, annotations, 1);
        QVERIFY(!qpdf_oh_has_key(pdf, disabled, "/Dest"));
        QVERIFY(!qpdf_oh_has_key(pdf, disabled, "/A"));
        const auto link = qpdf_oh_get_array_item(pdf, annotations, 2);
        const auto action = qpdf_oh_get_key(pdf, link, "/A");
        const auto destination = qpdf_oh_get_key(pdf, action, "/D");
        QCOMPARE(qpdf_find_page_by_oh(pdf, qpdf_oh_get_array_item(pdf, destination, 0)), 1);
        QCOMPARE(QByteArray(qpdf_oh_get_name(pdf, qpdf_oh_get_array_item(pdf, destination, 1))), QByteArray("/XYZ"));
        QCOMPARE(qpdf_oh_get_numeric_value(pdf, qpdf_oh_get_array_item(pdf, destination, 2)), 12.0);
        QCOMPARE(qpdf_oh_get_numeric_value(pdf, qpdf_oh_get_array_item(pdf, destination, 3)), 34.0);
        QCOMPARE(qpdf_oh_get_numeric_value(pdf, qpdf_oh_get_array_item(pdf, destination, 4)), 1.5);
        QCOMPARE(qpdf_find_page_by_oh(pdf, qpdf_oh_get_key(pdf, link, "/P")), 0);
        const auto uri = qpdf_oh_get_key(pdf, qpdf_oh_get_key(pdf,
            qpdf_oh_get_array_item(pdf, annotations, 3), "/A"), "/URI");
        QCOMPARE(QByteArray(qpdf_oh_get_string_value(pdf, uri)), QByteArray("https://example.com/"));
    }

    void keepsCommentAndPopupWhenDestinationIsExcluded()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("comment.pdf");
        QVERIFY(writeObjects(source, {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> /Annots [5 0 R 6 0 R 7 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
            "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /Contents (Keep this comment) /P 3 0 R /Popup 6 0 R /A << /S /GoTo /D [4 0 R /Fit] >> >>",
            "<< /Type /Annot /Subtype /Popup /Rect [40 40 140 140] /Parent 5 0 R /P 3 0 R >>",
            "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /IRT 5 0 R /Contents (Reply) /P 3 0 R >>"
        }));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy done(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        const auto outputs = qvariant_cast<QList<QUrl>>(done[0][1]);
        PdfHandle document;
        auto pdf = document.value;
        QCOMPARE(qpdf_read(pdf, outputs[0].toLocalFile().toUtf8().constData(), ""), QPDF_SUCCESS);
        const auto annotations = qpdf_oh_get_key(pdf, qpdf_get_page_n(pdf, 0), "/Annots");
        QCOMPARE(qpdf_oh_get_array_n_items(pdf, annotations), 3);
        const auto comment = qpdf_oh_get_array_item(pdf, annotations, 0);
        const auto popup = qpdf_oh_get_array_item(pdf, annotations, 1);
        const auto reply = qpdf_oh_get_array_item(pdf, annotations, 2);
        QVERIFY(!qpdf_oh_has_key(pdf, comment, "/A"));
        QCOMPARE(QByteArray(qpdf_oh_get_string_value(pdf, qpdf_oh_get_key(pdf, comment, "/Contents"))), QByteArray("Keep this comment"));
        QCOMPARE(qpdf_oh_get_object_id(pdf, qpdf_oh_get_key(pdf, popup, "/Parent")), qpdf_oh_get_object_id(pdf, comment));
        QCOMPARE(qpdf_oh_get_object_id(pdf, qpdf_oh_get_key(pdf, reply, "/IRT")), qpdf_oh_get_object_id(pdf, comment));
    }

    void enforcesPageOutputAndInputLimits()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("many.pdf");
        QVERIFY(createManyPages(source, 1001));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.pageCount(), 1001);
        QVERIFY(!controller.canSplit());
        QCOMPARE(controller.outputCount(), 0);
        controller.setMode(1);
        controller.setPagesPerFile(2);
        QVERIFY(controller.canSplit());
        QCOMPARE(controller.outputCount(), 501);
        controller.setPagesPerFile(0);
        QVERIFY(!controller.canSplit());
        controller.setPagesPerFile(10001);
        QVERIFY(!controller.canSplit());
        controller.setMode(2);
        controller.setPageRanges(QString(65537, QLatin1Char('1')));
        QVERIFY(!controller.canSplit());
        const auto excessive = dir.filePath("excessive.pdf");
        QVERIFY(createManyPages(excessive, 10001));
        QVERIFY(controller.setSource(QUrl::fromLocalFile(excessive)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.pageCount(), 0);
        QFile oversized(dir.filePath("oversized.pdf"));
        QVERIFY(oversized.open(QIODevice::WriteOnly));
        QVERIFY(oversized.resize(256LL * 1024 * 1024 + 1));
        oversized.close();
        QVERIFY(!controller.setSource(QUrl::fromLocalFile(oversized.fileName())));
    }

    void rejectsCrossPageAnnotationRelationships()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("cross-page-reply.pdf");
        QVERIFY(writeObjects(source, {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> /Annots [5 0 R] >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> /Annots [6 0 R] >>",
            "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /Contents (Comment on page one) /P 3 0 R >>",
            "<< /Type /Annot /Subtype /Text /Rect [10 10 30 30] /Contents (Reply on page two) /IRT 5 0 R /P 4 0 R >>"
        }));
        const auto original = readFile(source);
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.canSplit());
        QCOMPARE(controller.pageCount(), 0);
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QCOMPARE(readFile(source), original);
        QCOMPARE(directoryEntries(dir.path()), QStringList{"cross-page-reply.pdf"});
    }

    void cancellationAfterFirstOutputRemovesWholeBatch()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("many.pdf");
        QVERIFY(createManyPages(source, 300));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        bool cancelledAfterProgress = false;
        connect(&controller, &PDFSplitController::changed, this, [&] {
            if (controller.busy() && controller.progress() > 0 && !cancelledAfterProgress) {
                cancelledAfterProgress = true;
                controller.cancel();
            }
        });
        QSignalSpy done(&controller, &PDFSplitController::finished);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(cancelledAfterProgress);
        QVERIFY(!done[0][0].toBool());
        QCOMPARE(directoryEntries(dir.path()), QStringList{"many.pdf"});
    }

    void rejectsInvalidRanges_data()
    {
        QTest::addColumn<QString>("ranges");
        for (const auto &value : QStringList{"", " ", "0", "6", "3-1", "1-6", "1,1", "1-3;3-5",
                "1;;2", "1;", ";1", "1,", "1--2", "1.5", "1 2", "all", "9999999999999999999"})
            QTest::newRow(qPrintable(value.isEmpty() ? QString("empty") : value)) << value;
    }
    void rejectsInvalidRanges()
    {
        QFETCH(QString, ranges);
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        controller.setMode(2);
        controller.setPageRanges(ranges);
        QVERIFY(!controller.canSplit());
        QCOMPARE(controller.outputCount(), 0);
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QCOMPARE(directoryEntries(dir.path()), QStringList{"source.pdf"});
    }

    void createsUniqueOutputWithoutReplacingFiles()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        QVERIFY(QDir(dir.path()).mkdir("source-split"));
        QVERIFY(writeFile(dir.filePath("source-split/keep.txt"), "keep"));
        QVERIFY(writeFile(dir.filePath("source-split-1"), "existing file"));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy done(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QCOMPARE(done[0][0].toUrl().toLocalFile(), QFileInfo(dir.filePath("source-split-2")).canonicalFilePath());
        QCOMPARE(readFile(dir.filePath("source-split/keep.txt")), QByteArray("keep"));
        QCOMPARE(readFile(dir.filePath("source-split-1")), QByteArray("existing file"));
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 10000);
        QCOMPARE(done[1][0].toUrl().toLocalFile(), QFileInfo(dir.filePath("source-split-3")).canonicalFilePath());
    }

    void rejectsUnsupportedStructures_data()
    {
        QTest::addColumn<QByteArray>("catalog");
        QTest::addColumn<QByteArray>("annotation");
        QTest::newRow("AcroForm") << QByteArray("/AcroForm << /Fields [] >>") << QByteArray();
        QTest::newRow("XFA") << QByteArray("/AcroForm << /XFA (data) >>") << QByteArray();
        QTest::newRow("signature") << QByteArray("/Perms << /DocMDP 1 0 R >>") << QByteArray();
        QTest::newRow("layers") << QByteArray("/OCProperties << >>") << QByteArray();
        QTest::newRow("open-javascript") << QByteArray("/OpenAction << /S /JavaScript /JS (app.alert) >>") << QByteArray();
        QTest::newRow("annot-actions") << QByteArray() << QByteArray("/AA << /E << /S /JavaScript /JS (alert) >> >>");
        QTest::newRow("orphan-widget") << QByteArray() << QByteArray("/Subtype /Widget /FT /Tx");
    }
    void rejectsUnsupportedStructures()
    {
        QFETCH(QByteArray, catalog);
        QFETCH(QByteArray, annotation);
        QTemporaryDir dir;
        const auto source = dir.filePath("unsupported.pdf");
        QVERIFY(createPdf(source, "Page", catalog, annotation));
        const auto original = readFile(source);
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.canSplit());
        QCOMPARE(controller.pageCount(), 0);
        QVERIFY(!controller.statusText().isEmpty());
        QCOMPARE(readFile(source), original);
        QCOMPARE(directoryEntries(dir.path()), QStringList{"unsupported.pdf"});
    }

    void rejectsBrokenEncryptedRemoteAndDestination()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        const auto broken = dir.filePath("broken.pdf");
        const auto encrypted = dir.filePath("encrypted.pdf");
        QVERIFY(createPdf(source));
        QVERIFY(writeFile(broken, "not a PDF"));
        {
            // qpdf retains the output filename while configuring encryption
            // and writing. Both byte buffers outlive the entire qpdf handle.
            const QByteArray sourcePath = source.toUtf8();
            const QByteArray encryptedPath = encrypted.toUtf8();
            PdfHandle pdf;
            QCOMPARE(qpdf_read(pdf.value, sourcePath.constData(), ""), QPDF_SUCCESS);
            QCOMPARE(qpdf_init_write(pdf.value, encryptedPath.constData()), QPDF_SUCCESS);
            qpdf_set_r6_encryption_parameters2(pdf.value, "secret", "owner", true, true, true,
                true, true, true, qpdf_r3p_full, true);
            QCOMPARE(qpdf_write(pdf.value), QPDF_SUCCESS);
        }
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        for (const QString &path : {broken, encrypted}) {
            QVERIFY(controller.setSource(QUrl::fromLocalFile(path)));
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QVERIFY(!controller.canSplit());
            QCOMPARE(controller.fileName(), QFileInfo(path).fileName());
        }
        QVERIFY(!controller.setSource(QUrl("https://example.com/file.pdf")));
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(source)));
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.filePath("missing"))));
        QVERIFY(!controller.splitTo(QUrl("https://example.com/")));
        const auto alias = dir.filePath("alias");
        QVERIFY(QFile::link(dir.path(), alias));
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(alias)));
    }

    void sourceChangedWithSameSizeAndTimestampIsRejected()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source, "AAAA"));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        const auto before = QFileInfo(source);
        const auto modified = before.lastModified();
        const auto size = before.size();
        QVERIFY(createPdf(source, "BBBB"));
        QFile changed(source);
        QVERIFY(changed.open(QIODevice::ReadWrite));
        QVERIFY(changed.setFileTime(modified, QFileDevice::FileModificationTime));
        changed.close();
        QCOMPARE(QFileInfo(source).size(), size);
        QSignalSpy done(&controller, &PDFSplitController::finished);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(!done[0][0].toBool());
        QCOMPARE(directoryEntries(dir.path()), QStringList{"source.pdf"});
    }

    void cancellationCleansStagingAndKeepsSelection()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy done(&controller, &PDFSplitController::finished);
        QSignalSpy completed(&controller, &PDFSplitController::completed);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        QVERIFY(!controller.setSource(QUrl::fromLocalFile(source)));
        QVERIFY(!controller.splitTo(QUrl::fromLocalFile(dir.path())));
        controller.setMode(2);
        QCOMPARE(controller.mode(), 0);
        controller.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(!done[0][0].toBool());
        QVERIFY(completed.isEmpty());
        QCOMPARE(controller.pageCount(), 5);
        QCOMPARE(directoryEntries(dir.path()), QStringList{"source.pdf"});
        QVERIFY(controller.canSplit());
    }

    void clearAndDestructionDuringWork()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("source.pdf");
        QVERIFY(createPdf(source));
        PDFSplitController controller;
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        controller.clear();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.pageCount(), 0);
        QVERIFY(controller.fileName().isEmpty());
        QVERIFY(controller.setSource(QUrl::fromLocalFile(source)));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QSignalSpy done(&controller, &PDFSplitController::finished);
        QVERIFY(controller.splitTo(QUrl::fromLocalFile(dir.path())));
        controller.clear();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QCOMPARE(controller.pageCount(), 0);
        {
            auto worker = std::make_unique<PDFSplitController>();
            QVERIFY(worker->setSource(QUrl::fromLocalFile(source)));
            QTRY_VERIFY_WITH_TIMEOUT(!worker->busy(), 10000);
            QVERIFY(worker->splitTo(QUrl::fromLocalFile(dir.path())));
        }
        // A new inspection is accepted once the destroyed owner's worker exits.
        bool accepted = false;
        QTRY_VERIFY_WITH_TIMEOUT(accepted || (accepted = controller.setSource(QUrl::fromLocalFile(source))), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(directoryEntries(dir.path()), QStringList{"source.pdf"});
    }
};

QTEST_MAIN(PDFSplitControllerTest)
#include "tst_PDFSplitController.moc"
