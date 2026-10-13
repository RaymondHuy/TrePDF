#include "PDFDocument.h"
#include "PDFMergeController.h"
#include "PDFiumSynchronization.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <qpdf/qpdf-c.h>

#include <set>

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

// Duplicate field names must remain independent. Pages also contain native
// text, an image, a comment, an internal destination and an external URL.
bool createPdf(const QString &path, const QByteArray &label)
{
    const QByteArray contents = "BT /F1 12 Tf 20 350 Td (" + label
        + " first) Tj ET\nq 24 0 0 24 20 250 cm /Im1 Do Q\n";
    const QByteArray second = "BT /F1 12 Tf 20 250 Td (" + label + " second) Tj ET\n";
    return writeObjects(path, {
        "<< /Type /Catalog /Pages 2 0 R /AcroForm 11 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] "
        "/Resources << /Font << /F1 5 0 R >> /XObject << /Im1 10 0 R >> >> "
        "/Contents 6 0 R /Annots [8 0 R 9 0 R 12 0 R 13 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] /Rotate 90 "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        stream(contents),
        stream(second),
        "<< /Type /Annot /Subtype /Text /Rect [20 200 40 220] /P 3 0 R "
        "/Contents (" + label + " comment) /F 4 >>",
        "<< /Type /Annot /Subtype /Link /Rect [20 160 100 180] /P 3 0 R "
        "/Border [0 0 0] /Dest [4 0 R /Fit] >>",
        stream(QByteArray::fromHex("137bc9"),
               "/Type /XObject /Subtype /Image /Width 1 /Height 1 "
               "/ColorSpace /DeviceRGB /BitsPerComponent 8"),
        "<< /Fields [12 0 R] /DR << /Font << /Helv 5 0 R >> >> /DA (/Helv 12 Tf 0 g) >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (shared-field) /V ("
            + label + " value) /Rect [20 100 160 130] /P 3 0 R /F 4 "
            "/DA (/Helv 12 Tf 0 g) /AP << /N 14 0 R >> >>",
        "<< /Type /Annot /Subtype /Link /Rect [20 50 100 80] /P 3 0 R "
        "/A << /S /URI /URI (https://example.com/) >> /Border [0 0 0] >>",
        stream("BT /Helv 12 Tf 2 10 Td (" + label + " value) Tj ET\n",
               "/Type /XObject /Subtype /Form /BBox [0 0 140 30] "
               "/Resources << /Font << /Helv 5 0 R >> >>")
    });
}

bool encryptPdf(const QString &source, const QString &target)
{
    PdfHandle pdf;
    if (qpdf_read(pdf.value, QFile::encodeName(source).constData(), nullptr) & QPDF_ERRORS)
        return false;
    if (qpdf_init_write(pdf.value, QFile::encodeName(target).constData()) & QPDF_ERRORS)
        return false;
    qpdf_set_r6_encryption_parameters2(pdf.value, "secret", "owner", true, true, true,
                                     true, true, true, qpdf_r3p_full, true);
    return !(qpdf_write(pdf.value) & QPDF_ERRORS);
}

bool createLinkPdf(const QString &path, const QByteArray &action, const QByteArray &catalog = {})
{
    return writeObjects(path, {
        "<< /Type /Catalog /Pages 2 0 R " + catalog + " >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] "
        "/Resources << >> /Annots [5 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
        "<< /Type /Annot /Subtype /Link /Rect [20 50 100 80] /P 3 0 R "
        "/A " + action + " /Border [0 0 0] >>"
    });
}

struct PageSnapshot {
    QSizeF size;
    int rotation;
    QString text;
    int imageCount = 0;
};

QList<PageSnapshot> inspectPages(const QString &path)
{
    PDFiumProcessLock lock;
    const auto document = FPDF_LoadDocument(QFile::encodeName(path).constData(), nullptr);
    if (!document)
        return {};
    QList<PageSnapshot> result;
    for (int i = 0; i < FPDF_GetPageCount(document); ++i) {
        const auto page = FPDF_LoadPage(document, i);
        if (!page)
            break;
        PageSnapshot snapshot{QSizeF(FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)),
                              FPDFPage_GetRotation(page), {}, 0};
        const auto text = FPDFText_LoadPage(page);
        if (text) {
            const int count = FPDFText_CountChars(text);
            QVector<unsigned short> utf16(count + 1);
            const int copied = FPDFText_GetText(text, 0, count, utf16.data());
            if (copied > 0)
                snapshot.text = QString::fromUtf16(
                    reinterpret_cast<const char16_t *>(utf16.constData()), copied - 1);
            FPDFText_ClosePage(text);
        }
        for (int j = 0; j < FPDFPage_CountObjects(page); ++j) {
            if (FPDFPageObj_GetType(FPDFPage_GetObject(page, j)) == FPDF_PAGEOBJ_IMAGE)
                ++snapshot.imageCount;
        }
        result.append(snapshot);
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(document);
    return result;
}
}

class PDFMergeTest : public QObject
{
    Q_OBJECT
    PDFDocument m_runtime;

private slots:
    void mergesOrderedNativePagesAndPreservesSources()
    {
        QTemporaryDir dir;
        const QString alpha = dir.filePath("alpha.pdf");
        const QString beta = dir.filePath("beta.pdf");
        const QString target = dir.filePath("merged.pdf");
        QVERIFY(createPdf(alpha, "Alpha"));
        QVERIFY(createPdf(beta, "Beta"));
        const auto alphaBytes = readFile(alpha);
        const auto betaBytes = readFile(beta);
        const auto expected = inspectPages(beta) + inspectPages(alpha);
        PDFMergeController controller;
        QVERIFY(controller.addFiles({QUrl::fromLocalFile(alpha), QUrl::fromLocalFile(beta)}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.files().size(), 2);
        QCOMPARE(controller.totalPages(), 4);
        QVERIFY(controller.canMerge());
        controller.moveFile(1, 0);
        QCOMPARE(controller.files()[0].toMap().value("name").toString(), QString("beta.pdf"));
        QSignalSpy finished(&controller, &PDFMergeController::finished);
        QSignalSpy completed(&controller, &PDFMergeController::completed);
        QVERIFY(controller.mergeTo(QUrl::fromLocalFile(target)));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QVERIFY2(finished[0][0].toBool(), qPrintable(controller.statusText()));
        QCOMPARE(completed.size(), 1);
        QCOMPARE(completed[0][0].toUrl(), QUrl::fromLocalFile(target));
        const auto actual = inspectPages(target);
        QCOMPARE(actual.size(), expected.size());
        for (qsizetype i = 0; i < actual.size(); ++i) {
            QCOMPARE(actual[i].text, expected[i].text);
            QCOMPARE(actual[i].size, expected[i].size);
            QCOMPARE(actual[i].rotation, expected[i].rotation);
            QCOMPARE(actual[i].imageCount, expected[i].imageCount);
        }
        QCOMPARE(readFile(alpha), alphaBytes);
        QCOMPARE(readFile(beta), betaBytes);

        PdfHandle merged;
        const auto pdf = merged.value;
        QVERIFY(!(qpdf_read(pdf, QFile::encodeName(target).constData(), nullptr) & QPDF_ERRORS));
        QCOMPARE(qpdf_get_num_pages(pdf), 4);
        const auto key = [pdf](qpdf_oh object, const char *name) {
            return qpdf_oh_get_key(pdf, object, name);
        };
        const auto item = [pdf](qpdf_oh object, int index) {
            return qpdf_oh_get_array_item(pdf, object, index);
        };
        const auto value = [pdf](qpdf_oh object) {
            return QString::fromUtf8(qpdf_oh_get_utf8_value(pdf, object));
        };
        for (int i : {0, 2}) {
            const auto page = qpdf_get_page_n(pdf, size_t(i));
            const auto annotations = key(page, "/Annots");
            QCOMPARE(qpdf_oh_get_array_n_items(pdf, annotations), 4);
            const auto note = item(annotations, 0);
            QCOMPARE(value(key(note, "/Contents")),
                     (i == 0 ? QString("Beta comment") : QString("Alpha comment")));
            const auto destination = key(item(annotations, 1), "/Dest");
            QVERIFY(qpdf_oh_is_array(pdf, destination));
            QCOMPARE(qpdf_find_page_by_oh(pdf, item(destination, 0)), i + 1);
            const auto uri = key(key(item(annotations, 3), "/A"), "/URI");
            QCOMPARE(value(uri), QString("https://example.com/"));
            const auto image = key(key(key(page, "/Resources"), "/XObject"), "/Im1");
            QVERIFY(qpdf_oh_is_stream(pdf, image));
            unsigned char *pixels = nullptr;
            size_t length = 0;
            const auto status = qpdf_oh_get_stream_data(pdf, image, qpdf_dl_generalized,
                                                       nullptr, &pixels, &length);
            const QByteArray decoded(reinterpret_cast<const char *>(pixels), qsizetype(length));
            qpdf_oh_free_buffer(&pixels);
            QVERIFY(!(status & QPDF_ERRORS));
            QCOMPARE(decoded, QByteArray::fromHex("137bc9"));
        }
        const auto fields = key(key(qpdf_get_root(pdf), "/AcroForm"), "/Fields");
        QCOMPARE(qpdf_oh_get_array_n_items(pdf, fields), 2);
        std::set<QString> names;
        std::set<QString> values;
        for (int i = 0; i < 2; ++i) {
            const auto field = item(fields, i);
            names.insert(value(key(field, "/T")));
            values.insert(value(key(field, "/V")));
        }
        QCOMPARE(names.size(), size_t(2));
        QCOMPARE(values, (std::set<QString>{"Alpha value", "Beta value"}));
    }

    void listSupportsRemovalReorderingAndReset()
    {
        QTemporaryDir dir;
        QList<QUrl> urls;
        for (const auto &name : {"one", "two", "three"}) {
            const auto path = dir.filePath(QString::fromLatin1(name) + ".pdf");
            QVERIFY(createPdf(path, name));
            urls.append(QUrl::fromLocalFile(path));
        }
        PDFMergeController controller;
        QVERIFY(!controller.canMerge());
        QVERIFY(controller.addFiles(urls));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.totalPages(), 6);
        controller.moveFile(2, 0);
        controller.removeFile(1);
        QCOMPARE(controller.files().size(), 2);
        QCOMPARE(controller.files()[0].toMap().value("name").toString(), QString("three.pdf"));
        QCOMPARE(controller.files()[1].toMap().value("name").toString(), QString("two.pdf"));
        QCOMPARE(controller.totalPages(), 4);
        const auto beforeInvalidMoves = controller.files();
        controller.moveFile(-1, 0);
        controller.moveFile(0, 8);
        controller.removeFile(-1);
        QCOMPARE(controller.files(), beforeInvalidMoves);
        controller.removeFile(0);
        QVERIFY(!controller.canMerge());
        controller.clear();
        QVERIFY(controller.files().isEmpty());
        QCOMPARE(controller.totalPages(), 0);
    }

    void rejectsMalformedEncryptedAndRemoteInput()
    {
        QTemporaryDir dir;
        const auto source = dir.filePath("good.pdf");
        const auto malformed = dir.filePath("broken.pdf");
        const auto encrypted = dir.filePath("encrypted.pdf");
        QVERIFY(createPdf(source, "Source"));
        QVERIFY(writeFile(malformed, "this is not a PDF"));
        QVERIFY(encryptPdf(source, encrypted));
        PDFMergeController controller;
        QVERIFY(controller.addFiles({QUrl::fromLocalFile(source)}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QCOMPARE(controller.files().size(), 1);
        for (const auto &bad : {QUrl::fromLocalFile(malformed), QUrl::fromLocalFile(encrypted),
                               QUrl("https://example.com/remote.pdf")}) {
            controller.addFiles({bad});
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
            QCOMPARE(controller.files().size(), 1);
            QVERIFY(!controller.statusText().isEmpty());
        }
    }

    void rejectsSourceDestinationAndSymlink()
    {
        QTemporaryDir dir;
        const auto first = dir.filePath("first.pdf");
        const auto second = dir.filePath("second.pdf");
        QVERIFY(createPdf(first, "First"));
        QVERIFY(createPdf(second, "Second"));
        const auto original = readFile(first);
        PDFMergeController controller;
        QVERIFY(controller.addFiles({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(!controller.mergeTo(QUrl::fromLocalFile(first)));
        QVERIFY(!controller.mergeTo(QUrl("https://example.com/out.pdf")));
        const auto alias = dir.filePath("alias.pdf");
        QVERIFY(QFile::link(first, alias));
        QVERIFY(!controller.mergeTo(QUrl::fromLocalFile(alias)));
        QVERIFY(!controller.busy());
        QCOMPARE(readFile(first), original);
        QVERIFY(QFileInfo(alias).isSymLink());
    }

    void handlesDestinationContext_data()
    {
        QTest::addColumn<QByteArray>("action");
        QTest::addColumn<QByteArray>("catalog");
        QTest::addColumn<bool>("supported");
        QTest::newRow("named-goto")
            << QByteArray("<< /S /GoTo /D (landing) >>")
            << QByteArray("/Names << /Dests << /Names [(landing) [4 0 R /XYZ 12 34 1.5]] >> >>")
            << true;
        QTest::newRow("relative-remote-goto")
            << QByteArray("<< /S /GoToR /F (adjacent.pdf) /D [0 /Fit] >>")
            << QByteArray()
            << false;
        QTest::newRow("relative-uri-with-base")
            << QByteArray("<< /S /URI /URI (chapter.pdf) >>")
            << QByteArray("/URI << /Base (https://example.com/manual/) >>")
            << false;
        QTest::newRow("relative-uri-without-base")
            << QByteArray("<< /S /URI /URI (chapter.pdf) >>")
            << QByteArray()
            << false;
    }

    void handlesDestinationContext()
    {
        QFETCH(QByteArray, action);
        QFETCH(QByteArray, catalog);
        QFETCH(bool, supported);
        QTemporaryDir dir;
        const auto first = dir.filePath("first.pdf");
        const auto linked = dir.filePath("linked.pdf");
        const auto target = dir.filePath("existing.pdf");
        QVERIFY(createPdf(first, "First"));
        QVERIFY(createLinkPdf(linked, action, catalog));
        QVERIFY(writeFile(target, "existing destination"));
        const auto original = readFile(linked);
        PDFMergeController controller;
        QVERIFY(controller.addFiles({QUrl::fromLocalFile(first)}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        const auto priorFiles = controller.files();
        QVERIFY(controller.addFiles({QUrl::fromLocalFile(linked)}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        if (!supported) {
            QCOMPARE(controller.files(), priorFiles);
            QVERIFY(!controller.statusText().isEmpty());
            QCOMPARE(readFile(target), QByteArray("existing destination"));
            QCOMPARE(readFile(linked), original);
            return;
        }
        QVERIFY2(controller.canMerge(), qPrintable(controller.statusText()));
        QSignalSpy done(&controller, &PDFMergeController::finished);
        QVERIFY(controller.mergeTo(QUrl::fromLocalFile(target)));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY2(done[0][0].toBool(), qPrintable(controller.statusText()));
        PdfHandle merged;
        const auto pdf = merged.value;
        QVERIFY(!(qpdf_read(pdf, QFile::encodeName(target).constData(), nullptr) & QPDF_ERRORS));
        QCOMPARE(qpdf_get_num_pages(pdf), 4);
        const auto page = qpdf_get_page_n(pdf, 2);
        const auto annotations = qpdf_oh_get_key(pdf, page, "/Annots");
        const auto link = qpdf_oh_get_array_item(pdf, annotations, 0);
        const auto mergedAction = qpdf_oh_get_key(pdf, link, "/A");
        const auto destination = qpdf_oh_get_key(pdf, mergedAction, "/D");
        QVERIFY(qpdf_oh_is_array(pdf, destination));
        QCOMPARE(qpdf_find_page_by_oh(pdf, qpdf_oh_get_array_item(pdf, destination, 0)), 3);
        QVERIFY(qpdf_oh_is_name_and_equals(pdf, qpdf_oh_get_array_item(pdf, destination, 1), "/XYZ"));
        QCOMPARE(qpdf_oh_get_numeric_value(pdf, qpdf_oh_get_array_item(pdf, destination, 2)), 12.0);
        QCOMPARE(qpdf_oh_get_numeric_value(pdf, qpdf_oh_get_array_item(pdf, destination, 3)), 34.0);
        QCOMPARE(qpdf_oh_get_numeric_value(pdf, qpdf_oh_get_array_item(pdf, destination, 4)), 1.5);
        QCOMPARE(readFile(linked), original);
    }

    void sourceChangedAfterSelectionPreservesDestination()
    {
        QTemporaryDir dir;
        const auto first = dir.filePath("first.pdf");
        const auto second = dir.filePath("second.pdf");
        const auto target = dir.filePath("existing.pdf");
        QVERIFY(createPdf(first, "First"));
        QVERIFY(createPdf(second, "Second"));
        QVERIFY(writeFile(target, "existing destination bytes"));
        PDFMergeController controller;
        QVERIFY(controller.addFiles({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        const auto modified = QFileInfo(second).lastModified();
        const auto size = QFileInfo(second).size();
        QVERIFY(createPdf(second, "Edited")); // Same byte length and timestamp: detect by content.
        QFile changed(second);
        QVERIFY(changed.open(QIODevice::ReadWrite));
        QVERIFY(changed.setFileTime(modified, QFileDevice::FileModificationTime));
        changed.close();
        QCOMPARE(QFileInfo(second).size(), size);
        QSignalSpy done(&controller, &PDFMergeController::finished);
        if (controller.mergeTo(QUrl::fromLocalFile(target))) {
            QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
            QVERIFY(!done[0][0].toBool());
        }
        QVERIFY(!controller.statusText().isEmpty());
        QCOMPARE(readFile(target), QByteArray("existing destination bytes"));
        QCOMPARE(controller.files().size(), 2);
    }

    void cancellationRetainsDestinationAndSelection()
    {
        QTemporaryDir dir;
        const auto first = dir.filePath("first.pdf");
        const auto second = dir.filePath("second.pdf");
        const auto target = dir.filePath("existing.pdf");
        QVERIFY(createPdf(first, "First"));
        QVERIFY(createPdf(second, "Second"));
        QVERIFY(writeFile(target, "untouched destination"));
        PDFMergeController controller;
        QVERIFY(controller.addFiles({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        const auto files = controller.files();
        QSignalSpy done(&controller, &PDFMergeController::finished);
        QSignalSpy completed(&controller, &PDFMergeController::completed);
        QVERIFY(controller.mergeTo(QUrl::fromLocalFile(target)));
        QVERIFY(controller.busy());
        QVERIFY(!controller.mergeTo(QUrl::fromLocalFile(dir.filePath("another.pdf"))));
        QVERIFY(!controller.addFiles({QUrl::fromLocalFile(first)}));
        controller.removeFile(0);
        controller.moveFile(0, 1);
        QCOMPARE(controller.files(), files);
        controller.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(!done[0][0].toBool());
        QVERIFY(completed.isEmpty());
        QCOMPARE(readFile(target), QByteArray("untouched destination"));
        QCOMPARE(controller.files(), files);
    }

    void mergesExternalPdfWithoutModifyingIt()
    {
        const auto source = qEnvironmentVariable("TREPDF_TEST_MERGE_PDF");
        if (source.isEmpty())
            QSKIP("Set TREPDF_TEST_MERGE_PDF to validate an additional local PDF.");
        QTemporaryDir dir;
        const auto other = dir.filePath("extra.pdf");
        const auto target = dir.filePath("merged.pdf");
        QVERIFY(createPdf(other, "Extra"));
        const auto original = readFile(source);
        QVERIFY(!original.isEmpty());
        const auto expected = inspectPages(source);
        QVERIFY(!expected.isEmpty());
        PDFMergeController controller;
        QVERIFY(controller.addFiles({QUrl::fromLocalFile(source), QUrl::fromLocalFile(other)}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 20000);
        QVERIFY2(controller.canMerge(), qPrintable(controller.statusText()));
        QSignalSpy done(&controller, &PDFMergeController::finished);
        QVERIFY(controller.mergeTo(QUrl::fromLocalFile(target)));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 20000);
        QVERIFY2(done[0][0].toBool(), qPrintable(controller.statusText()));
        const auto actual = inspectPages(target);
        QCOMPARE(actual.size(), expected.size() + 2);
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QCOMPARE(actual[i].text, expected[i].text);
            QCOMPARE(actual[i].size, expected[i].size);
            QCOMPARE(actual[i].rotation, expected[i].rotation);
            QCOMPARE(actual[i].imageCount, expected[i].imageCount);
        }
        QCOMPARE(readFile(source), original);
    }
};

QTEST_MAIN(PDFMergeTest)
#include "tst_PDFMerge.moc"
