#include "PDFDocument.h"
#include "PDFExcelExportController.h"
#include "PDFTableExtractor.h"
#include "PDFTableLayout.h"
#include "PDFTableSymbolMapping.h"
#include "PDFiumSynchronization.h"
#include "SpreadsheetWorkbook.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_transformpage.h>

#include <fcntl.h>
#include <unistd.h>

namespace {
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
struct PdfWriter : FPDF_FILEWRITE {
    QFile *file;
    explicit PdfWriter(QFile &output) : file(&output)
    {
        version = 1;
        WriteBlock = [](FPDF_FILEWRITE *self, const void *data, unsigned long size) -> int {
            auto *writer = static_cast<PdfWriter *>(self);
            return writer->file->write(static_cast<const char *>(data), size) == qint64(size);
        };
    }
};
bool createPdf(const QString &path, bool table = true, int rotation = 0,
               bool filled = false, bool compound = false, bool segmented = false,
               bool mergedHeader = false)
{
    PDFiumProcessLock lock;
    FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
    FPDF_PAGE page = FPDFPage_New(doc, 0, 400, 400);
    FPDF_PAGEOBJECT compoundPath = nullptr;
    constexpr double halfWidth = 0.48;
    const auto line = [&](double x1, double y1, double x2, double y2) {
        FPDF_PAGEOBJECT object;
        if (filled) {
            const bool vertical = x1 == x2;
            const float left = float(x1 - (vertical ? halfWidth : 0));
            const float bottom = float(y1 - (vertical ? 0 : halfWidth));
            const float right = float(x2 + (vertical ? halfWidth : 0));
            const float top = float(y2 + (vertical ? 0 : halfWidth));
            if (compound) {
                if (!compoundPath)
                    compoundPath = FPDFPageObj_CreateNewPath(left, bottom);
                else
                    FPDFPath_MoveTo(compoundPath, left, bottom);
                object = compoundPath;
                FPDFPath_LineTo(object, right, bottom);
                FPDFPath_LineTo(object, right, top);
                FPDFPath_LineTo(object, left, top);
                FPDFPath_Close(object);
            } else {
                object = FPDFPageObj_CreateNewRect(left, bottom, right - left, top - bottom);
            }
            FPDFPageObj_SetFillColor(object, 0, 0, 0, 255);
            FPDFPath_SetDrawMode(object, FPDF_FILLMODE_WINDING, false);
        } else {
            object = FPDFPageObj_CreateNewPath(float(x1), float(y1));
            FPDFPath_LineTo(object, float(x2), float(y2));
            FPDFPageObj_SetStrokeColor(object, 0, 0, 0, 255);
            FPDFPageObj_SetStrokeWidth(object, 0.5f);
            FPDFPath_SetDrawMode(object, FPDF_FILLMODE_NONE, true);
        }
        if (!compound)
            FPDFPage_InsertObject(page, object);
    };
    if (table) {
        for (int i = 0; i < 3; ++i) {
            line(40 + i * 140, 160 - halfWidth, 40 + i * 140,
                 mergedHeader && i == 1 ? 250 : 340 + halfWidth);
            if (segmented) {
                // Painted horizontal edges meet the sides of the vertical
                // rectangles, leaving a gap between their centerlines.
                for (int column = 0; column < 2; ++column)
                    line(40 + column * 140 + halfWidth, 160 + i * 90,
                         40 + (column + 1) * 140 - halfWidth, 160 + i * 90);
            } else {
                line(40, 160 + i * 90, 320, 160 + i * 90);
            }
        }
    }
    if (compoundPath)
        FPDFPage_InsertObject(page, compoundPath);
    const auto text = [&](const QString &value, float x, float y) {
        FPDF_PAGEOBJECT object = FPDFPageObj_NewTextObj(doc, "Helvetica", 12);
        FPDFText_SetText(object, reinterpret_cast<FPDF_WIDESTRING>(value.utf16()));
        FPDFPageObj_Transform(object, 1, 0, 0, 1, x, y);
        FPDFPage_InsertObject(page, object);
    };
    text(QStringLiteral("Item"), 50, 310);
    text(QStringLiteral("Code"), 190, 310);
    text(QStringLiteral("Pencil"), 50, 210);
    text(QStringLiteral("0012"), 190, 210);
    FPDFPage_SetRotation(page, rotation);
    const bool generated = FPDFPage_GenerateContent(page);
    QFile output(path);
    bool saved = false;
    if (output.open(QIODevice::WriteOnly)) {
        PdfWriter writer(output);
        saved = FPDF_SaveAsCopy(doc, &writer, FPDF_NO_INCREMENTAL);
    }
    FPDF_ClosePage(page);
    FPDF_CloseDocument(doc);
    return generated && saved;
}
QByteArray zipEntry(const QString &path, const QString &entry)
{
    QProcess unzip;
    unzip.start(QStandardPaths::findExecutable(QStringLiteral("unzip")),
                {QStringLiteral("-p"), path, entry});
    if (!unzip.waitForFinished(10000) || unzip.exitCode() != 0)
        return {};
    return unzip.readAllStandardOutput();
}
QMap<QString, QString> cellValues(const QByteArray &xml, const QByteArray &sharedXml = {})
{
    QStringList sharedStrings;
    QXmlStreamReader shared(sharedXml);
    while (!shared.atEnd()) {
        shared.readNext();
        if (shared.isStartElement() && shared.name() == QLatin1String("si"))
            sharedStrings.append(QString());
        else if (shared.isStartElement() && shared.name() == QLatin1String("t")
                 && !sharedStrings.isEmpty())
            sharedStrings.last() += shared.readElementText();
    }
    QMap<QString, QString> values;
    QXmlStreamReader reader(xml);
    QString cell;
    bool sharedString = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("c")) {
            cell = reader.attributes().value(QLatin1String("r")).toString();
            sharedString = reader.attributes().value(QLatin1String("t")) == QLatin1String("s");
            values[cell] = QString();
        } else if (reader.isStartElement() && reader.name() == QLatin1String("t")) {
            values[cell] += reader.readElementText();
        } else if (sharedString && reader.isStartElement() && reader.name() == QLatin1String("v")) {
            values[cell] = sharedStrings.value(reader.readElementText().toInt());
        }
    }
    return values;
}
QVector<PDFTableLayout::Rule> grid(double x = 0, double y = 0)
{
    QVector<PDFTableLayout::Rule> rules;
    for (int i = 0; i < 3; ++i) {
        rules.append({{x + i * 100, y}, {x + i * 100, y + 80}});
        rules.append({{x, y + i * 40}, {x + 200, y + i * 40}});
    }
    return rules;
}
PDFTableLayout::Glyph glyph(QString text, double x, double y, int index)
{
    return {text, QRectF(x, y, 20, 10), QPointF(x, y + 9), true, true, index};
}
int descriptorCount()
{
    int count = 0;
    for (int fd = 0; fd < 4096; ++fd)
        if (fcntl(fd, F_GETFD) != -1) ++count;
    return count;
}
}

class PDFExcelExportTest : public QObject {
    Q_OBJECT
private slots:
    void layoutMultipleTablesAndBlankCells()
    {
        auto rules = grid();
        rules += grid(0, 150);
        QVector<PDFTableLayout::Glyph> glyphs = {
            glyph(QStringLiteral("Mã"), 10, 10, 0),
            glyph(QStringLiteral("0012"), 110, 10, 1),
            glyph(QStringLiteral("Hàng"), 10, 50, 2),
            glyph(QStringLiteral("A"), 10, 160, 3),
            glyph(QStringLiteral("B"), 110, 160, 4),
            glyph(QStringLiteral("Ngoài bảng"), 300, 350, 5)};
        QVector<SpreadsheetSheet> tables;
        QString error;
        QVERIFY2(PDFTableLayout::extract(rules, glyphs, tables, {}, error), qPrintable(error));
        QCOMPARE(tables.size(), 2);
        QCOMPARE(tables[0].rows.size(), 2);
        QCOMPARE(tables[0].rows[0].cells[0].text, QStringLiteral("Mã"));
        QCOMPARE(tables[0].rows[0].cells[1].text, QStringLiteral("0012"));
        QCOMPARE(tables[0].rows[1].cells[1].text, QString());
    }
    void layoutRejectsIncompleteBordersCancellationAndLimits()
    {
        auto rules = grid();
        rules[2].start.setY(20); // dangling divider ends in the middle of an edge
        QVector<SpreadsheetSheet> tables;
        QString error;
        QVERIFY(!PDFTableLayout::extract(rules, {}, tables, {}, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(tables.isEmpty());
        auto cancelled = std::make_shared<std::atomic_bool>(true);
        QVERIFY(!PDFTableLayout::extract(grid(), {}, tables, cancelled, error));
        rules.resize(PDFTableLayout::maximumRules + 1);
        QVERIFY(!PDFTableLayout::extract(rules, {}, tables, {}, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(PDFTableLayout::extract({}, {}, tables, {}, error));
        QVERIFY(tables.isEmpty());
    }
    void layoutPreservesRectangularMergedCells()
    {
        auto rules = grid();
        rules[2].start.setY(40); // first row spans both columns
        QVector<SpreadsheetSheet> tables;
        QString error;
        auto heading = glyph(QStringLiteral("Merged heading"), 90, 10, 0);
        heading.bounds.setWidth(70); // deliberately crosses the absent divider
        QVERIFY2(PDFTableLayout::extract(rules,
            {heading, glyph(QStringLiteral("0012"), 110, 50, 1)}, tables, {}, error), qPrintable(error));
        QCOMPARE(tables.size(), 1);
        QCOMPARE(tables[0].mergedRanges.size(), 1);
        QCOMPARE(tables[0].mergedRanges[0].firstRow, 0);
        QCOMPARE(tables[0].mergedRanges[0].lastColumn, 1);
        QCOMPARE(tables[0].rows[0].cells[0].text, QStringLiteral("Merged heading"));
        QVERIFY(tables[0].rows[0].cells[1].text.isEmpty());
        QCOMPARE(tables[0].rows[1].cells[1].text, QStringLiteral("0012"));

        rules = grid();
        rules[3].start.setX(100); // first column spans both rows
        tables.clear();
        auto multiline = glyph(QStringLiteral("Vertical heading"), 10, 35, 0);
        QVERIFY(PDFTableLayout::extract(rules,
            {multiline, glyph(QStringLiteral("Value"), 110, 50, 1)}, tables, {}, error));
        QCOMPARE(tables[0].mergedRanges[0].firstColumn, 0);
        QCOMPARE(tables[0].mergedRanges[0].lastRow, 1);
        QCOMPARE(tables[0].rows[0].cells[0].text, QStringLiteral("Vertical heading"));
        QVERIFY(tables[0].rows[1].cells[0].text.isEmpty());
    }
    void layoutRejectsOpenPerimetersAndNonrectangularMerges()
    {
        QVector<SpreadsheetSheet> tables;
        QString error;
        auto rules = grid();
        rules[0].start.setY(40); // open outer perimeter
        QVERIFY(!PDFTableLayout::extract(rules, {}, tables, {}, error));
        QVERIFY(tables.isEmpty());
        rules = grid();
        rules[2].start.setY(40);
        rules[3].start.setX(100); // three cells form an L, not a rectangle
        QVERIFY(!PDFTableLayout::extract(rules, {}, tables, {}, error));
        QVERIFY(tables.isEmpty());
    }
    void layoutAlignsSlightlyShiftedDividersAcrossRows()
    {
        auto rules = grid();
        rules[2].end.setY(40);
        rules.append({{100.9, 40}, {100.9, 80}});
        QVector<SpreadsheetSheet> tables;
        QString error;
        QVERIFY2(PDFTableLayout::extract(rules,
            {glyph(QStringLiteral("Heading"), 10, 10, 0),
             glyph(QStringLiteral("0012"), 110, 50, 1)}, tables, {}, error), qPrintable(error));
        QCOMPARE(tables.size(), 1);
        QCOMPARE(tables[0].columnWidths.size(), 2);
        QVERIFY(tables[0].mergedRanges.isEmpty());
        QCOMPARE(tables[0].rows[1].cells[1].text, QStringLiteral("0012"));
    }
    void layoutJoinsPaintedJunctionsButRejectsMissingBorders()
    {
        QVector<PDFTableLayout::Rule> rules;
        for (int i = 0; i < 3; ++i) {
            rules.append({{i * 100.0, 0}, {i * 100.0, 80}, 2.4});
            rules.append({{1.2, i * 40.0}, {98.8, i * 40.0}, 2.4});
            rules.append({{101.2, i * 40.0}, {198.8, i * 40.0}, 2.4});
        }
        const QVector<PDFTableLayout::Glyph> glyphs = {
            glyph(QStringLiteral("Item"), 10, 10, 0),
            glyph(QStringLiteral("0012"), 110, 50, 1)};
        QVector<SpreadsheetSheet> tables;
        QString error;
        QVERIFY2(PDFTableLayout::extract(rules, glyphs, tables, {}, error), qPrintable(error));
        QCOMPARE(tables.size(), 1);
        QCOMPARE(tables[0].rows.size(), 2);
        QCOMPARE(tables[0].rows[1].cells[1].text, QStringLiteral("0012"));

        // A real gap away from a perpendicular painted line must stay a gap.
        rules[4].end.setX(49);
        rules.append({{51, 40}, {98.8, 40}, 2.4});
        tables.clear();
        QVERIFY(!PDFTableLayout::extract(rules, glyphs, tables, {}, error));
        QVERIFY(tables.isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void layoutMultilineAndAmbiguousText()
    {
        QVector<PDFTableLayout::Glyph> glyphs = {
            glyph(QStringLiteral("Line2"), 10, 24, 1),
            glyph(QStringLiteral("Line1"), 10, 9, 0),
            glyph(QStringLiteral("Value"), 110, 10, 2)};
        QVector<SpreadsheetSheet> tables;
        QString error;
        QVERIFY(PDFTableLayout::extract(grid(), glyphs, tables, {}, error));
        QCOMPARE(tables[0].rows[0].cells[0].text, QStringLiteral("Line1\nLine2"));
        tables.clear();
        glyphs[0].bounds.moveLeft(90);
        QVERIFY(!PDFTableLayout::extract(grid(), glyphs, tables, {}, error));
        QVERIFY(tables.isEmpty());
        glyphs[0] = glyph(QStringLiteral("A"), 10, 10, 0);
        glyphs[0].upright = false;
        QVERIFY(!PDFTableLayout::extract(grid(), glyphs, tables, {}, error));
    }
    void layoutKeepsInlineFootnotesAndSeparateTextLines()
    {
        auto body = glyph(QStringLiteral("Information"), 10, 10, 0);
        PDFTableLayout::Glyph marker{QStringLiteral("2"), QRectF(32, 5, 4, 6),
                                    QPointF(32, 11), true, true, 1};
        QVector<SpreadsheetSheet> tables;
        QString error;
        QVERIFY(PDFTableLayout::extract(grid(),
            {marker, body, glyph(QStringLiteral("Next line"), 10, 28, 2),
             glyph(QStringLiteral("Value"), 110, 10, 3)}, tables, {}, error));
        QCOMPARE(tables[0].rows[0].cells[0].text, QStringLiteral("Information2\nNext line"));
        tables.clear();
        marker.bounds.moveLeft(5);
        marker.origin.setX(5); // a separate small heading must not be appended
        QVERIFY(PDFTableLayout::extract(grid(),
            {marker, body, glyph(QStringLiteral("Value"), 110, 10, 3)}, tables, {}, error));
        QCOMPARE(tables[0].rows[0].cells[0].text, QStringLiteral("2\nInformation"));
    }
    void symbolMappingRequiresKnownFontAndCodepoint()
    {
        using PDFTableSymbolMapping::unicodeSymbol;
        QCOMPARE(unicodeSymbol(0xf0fc, "Wingdings"), U'\u2713');
        QCOMPARE(unicodeSymbol(0xf0fd, "ABCDEF+Wingdings-Regular"), U'\u2612');
        for (const auto name : {"Arial", "Wingdings2", "ABCDE+Wingdings", "abcdef+Wingdings"})
            QCOMPARE(unicodeSymbol(0xf0fc, name), char32_t(0xf0fc));
        QCOMPARE(unicodeSymbol(0xf0fb, "Wingdings"), char32_t(0xf0fb));
    }
    void workbookPreservesLiteralUnicodeAndBlankCells()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("unzip")).isEmpty())
            QSKIP("unzip is required for XLSX inspection");
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("Việt.xlsx"));
        auto workbook = createSpreadsheetWorkbook();
        QString error;
        QVERIFY2(workbook->open(path, error), qPrintable(error));
        SpreadsheetSheet sheet;
        sheet.name = QStringLiteral("Bảng 1");
        sheet.columnWidths = {18, 12};
        sheet.rows = {{{{0, QStringLiteral("Tiếng Việt")}, {1, QStringLiteral("=1+2")}}},
                      {{{0, QStringLiteral("0012")}, {1, QString()}}}};
        QVERIFY2(workbook->writeSheet(sheet, error), qPrintable(error));
        sheet.name = QStringLiteral("Bảng 2");
        QVERIFY(workbook->writeSheet(sheet, error));
        QVERIFY2(workbook->finish(error), qPrintable(error));
        const QByteArray xml = zipEntry(path, QStringLiteral("xl/worksheets/sheet1.xml"));
        QVERIFY(!xml.isEmpty());
        const auto values = cellValues(xml, zipEntry(path, QStringLiteral("xl/sharedStrings.xml")));
        QCOMPARE(values.value(QStringLiteral("A1")), QStringLiteral("Tiếng Việt"));
        QCOMPARE(values.value(QStringLiteral("B1")), QStringLiteral("=1+2"));
        QCOMPARE(values.value(QStringLiteral("A2")), QStringLiteral("0012"));
        QVERIFY(values.contains(QStringLiteral("B2")));
        QVERIFY(!xml.contains("<f>"));
        QVERIFY(!zipEntry(path, QStringLiteral("xl/worksheets/sheet2.xml")).isEmpty());
    }
    void workbookAbandonAndValidationReleaseResources()
    {
        QTemporaryDir dir;
        SpreadsheetSheet sheet;
        sheet.name = QStringLiteral("Table");
        sheet.rows = {{{{0, QStringLiteral("a")}}}, {{{0, QStringLiteral("b")}}}};
        const int before = descriptorCount();
        for (int i = 0; i < 30; ++i) {
            const QString path = dir.filePath(QStringLiteral("test.xlsx"));
            auto workbook = createSpreadsheetWorkbook();
            QString error;
            QVERIFY(workbook->open(path, error));
            QVERIFY(workbook->writeSheet(sheet, error));
            if (i % 2 == 0) {
                QVERIFY(!workbook->writeSheet(sheet, error)); // duplicate name aborts
                QVERIFY(!error.isEmpty());
            }
        }
        QCOMPARE(descriptorCount(), before);
        QVERIFY(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void extractorReadsActualPdf_data()
    {
        QTest::addColumn<bool>("filled");
        QTest::addColumn<bool>("compound");
        QTest::addColumn<bool>("segmented");
        QTest::newRow("stroked") << false << false << false;
        QTest::newRow("filled-rectangles") << true << false << false;
        QTest::newRow("compound-filled-rectangles") << true << true << false;
        QTest::newRow("painted-junctions") << true << false << true;
        QTest::newRow("compound-painted-junctions") << true << true << true;
    }
    void extractorReadsActualPdf()
    {
        QFETCH(bool, filled);
        QFETCH(bool, compound);
        QFETCH(bool, segmented);
        PDFDocument runtime;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("table.pdf"));
        QVERIFY(createPdf(path, true, 0, filled, compound, segmented));
        PDFiumProcessLock lock;
        FPDF_DOCUMENT doc = FPDF_LoadDocument(QFile::encodeName(path).constData(), nullptr);
        QVERIFY(doc);
        QVector<SpreadsheetSheet> tables;
        QString error;
        const bool ok = PDFTableExtractor::extractPage(doc, 0, tables, {}, error);
        FPDF_CloseDocument(doc);
        QVERIFY2(ok, qPrintable(error));
        QCOMPARE(tables.size(), 1);
        QCOMPARE(tables[0].rows[0].cells[0].text, QStringLiteral("Item"));
        QCOMPARE(tables[0].rows[1].cells[1].text, QStringLiteral("0012"));
    }
    void exportsRealPdfAsynchronously()
    {
        PDFDocument doc;
        QTemporaryDir dir;
        const QString pdf = dir.filePath(QStringLiteral("table.pdf"));
        const QString xlsx = dir.filePath(QStringLiteral("table.xlsx"));
        QVERIFY(createPdf(pdf, true, 0, true, true, false, true));
        QVERIFY(doc.open(QUrl::fromLocalFile(pdf)));
        PDFExcelExportController controller;
        controller.setDocument(&doc);
        QSignalSpy done(&controller, &PDFExcelExportController::finished);
        QVERIFY(controller.exportTo(QUrl::fromLocalFile(xlsx)));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(done[0][0].toBool());
        QVERIFY(!controller.busy());
        QVERIFY(readFile(xlsx).startsWith("PK"));
        if (!QStandardPaths::findExecutable(QStringLiteral("unzip")).isEmpty()) {
            const auto xml = zipEntry(xlsx, QStringLiteral("xl/worksheets/sheet1.xml"));
            const auto values = cellValues(xml, zipEntry(xlsx, QStringLiteral("xl/sharedStrings.xml")));
            QCOMPARE(values.value(QStringLiteral("A1")), QStringLiteral("Item Code"));
            QCOMPARE(values.value(QStringLiteral("B2")), QStringLiteral("0012"));
            QVERIFY(xml.contains("<mergeCell ref=\"A1:B1\"/>"));
        }
    }
    void cancellationAndNoTablesPreserveExistingDestination()
    {
        PDFDocument doc;
        QTemporaryDir dir;
        const QString pdf = dir.filePath(QStringLiteral("prose.pdf"));
        const QString xlsx = dir.filePath(QStringLiteral("existing.xlsx"));
        QVERIFY(createPdf(pdf, false));
        QVERIFY(doc.open(QUrl::fromLocalFile(pdf)));
        QVERIFY(writeFile(xlsx, "original workbook bytes"));
        PDFExcelExportController controller;
        controller.setDocument(&doc);
        QSignalSpy done(&controller, &PDFExcelExportController::finished);
        QVERIFY(controller.exportTo(QUrl::fromLocalFile(xlsx)));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(!done[0][0].toBool());
        QCOMPARE(readFile(xlsx), QByteArray("original workbook bytes"));
        {
            PDFiumProcessLock lock;
            QVERIFY(controller.exportTo(QUrl::fromLocalFile(xlsx)));
            controller.cancel();
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 10000);
        QVERIFY(!done[1][0].toBool());
        QCOMPARE(readFile(xlsx), QByteArray("original workbook bytes"));
    }
    void validatesDestinationAndGuard()
    {
        PDFDocument doc;
        QTemporaryDir dir;
        const QString pdf = dir.filePath(QStringLiteral("table.pdf"));
        QVERIFY(createPdf(pdf));
        QVERIFY(doc.open(QUrl::fromLocalFile(pdf)));
        PDFExcelExportController controller;
        controller.setDocument(&doc);
        QVERIFY(!controller.exportTo(QUrl::fromLocalFile(pdf)));
        QVERIFY(!controller.exportTo(QUrl(QStringLiteral("https://example.com/a.xlsx"))));
        const QString alias = dir.filePath(QStringLiteral("alias.xlsx"));
        QVERIFY(QFile::link(pdf, alias));
        QVERIFY(!controller.exportTo(QUrl::fromLocalFile(alias)));
        controller.setExportGuard([] { return false; });
        QVERIFY(!controller.exportTo(QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.xlsx")))));
        QVERIFY(!controller.busy());
    }
    void rejectsConcurrentExportAndCancelsOnDocumentChange()
    {
        PDFDocument doc;
        QTemporaryDir dir;
        const QString pdf = dir.filePath(QStringLiteral("table.pdf"));
        QVERIFY(createPdf(pdf));
        QVERIFY(doc.open(QUrl::fromLocalFile(pdf)));
        const QString target = dir.filePath(QStringLiteral("existing.xlsx"));
        QVERIFY(writeFile(target, "untouched"));
        PDFExcelExportController first;
        PDFExcelExportController second;
        first.setDocument(&doc);
        second.setDocument(&doc);
        QSignalSpy done(&first, &PDFExcelExportController::finished);
        {
            PDFiumProcessLock lock;
            QVERIFY(first.exportTo(QUrl::fromLocalFile(target)));
            QVERIFY(!second.exportTo(QUrl::fromLocalFile(target)));
            first.setDocument(nullptr);
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(!done[0][0].toBool());
        QCOMPARE(readFile(target), QByteArray("untouched"));
    }
    void controllerDestructionCancelsPendingWorker()
    {
        PDFDocument doc;
        QTemporaryDir dir;
        const QString pdf = dir.filePath(QStringLiteral("table.pdf"));
        QVERIFY(createPdf(pdf));
        QVERIFY(doc.open(QUrl::fromLocalFile(pdf)));
        const QString cancelledTarget = dir.filePath(QStringLiteral("cancelled.xlsx"));
        QVERIFY(writeFile(cancelledTarget, "untouched"));
        auto first = std::make_unique<PDFExcelExportController>();
        first->setDocument(&doc);
        {
            PDFiumProcessLock lock;
            QVERIFY(first->exportTo(QUrl::fromLocalFile(cancelledTarget)));
            first.reset();
        }
        PDFExcelExportController next;
        next.setDocument(&doc);
        QSignalSpy done(&next, &PDFExcelExportController::finished);
        const auto nextTarget = QUrl::fromLocalFile(dir.filePath(QStringLiteral("next.xlsx")));
        bool started = false;
        QTRY_VERIFY_WITH_TIMEOUT(started || (started = next.exportTo(nextTarget)), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(done[0][0].toBool());
        QCOMPARE(readFile(cancelledTarget), QByteArray("untouched"));
    }
    void destinationFailurePreservesSource()
    {
        PDFDocument doc;
        QTemporaryDir dir;
        const QString pdf = dir.filePath(QStringLiteral("table.pdf"));
        QVERIFY(createPdf(pdf));
        const QByteArray original = readFile(pdf);
        QVERIFY(doc.open(QUrl::fromLocalFile(pdf)));
        PDFExcelExportController controller;
        controller.setDocument(&doc);
        QSignalSpy done(&controller, &PDFExcelExportController::finished);
        QVERIFY(controller.exportTo(QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing/out.xlsx")))));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(!done[0][0].toBool());
        QCOMPARE(readFile(pdf), original);
    }
};
QTEST_MAIN(PDFExcelExportTest)
#include "tst_PDFExcelExport.moc"
