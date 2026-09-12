#include "SpreadsheetWorkbook.h"

#include <QFile>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#endif

namespace {
QByteArray zipEntry(const QString &path, const QString &entry)
{
    QProcess unzip;
    unzip.start(QStandardPaths::findExecutable(QStringLiteral("unzip")),
                {QStringLiteral("-p"), path, entry});
    if (!unzip.waitForFinished(10000) || unzip.exitCode() != 0)
        return {};
    return unzip.readAllStandardOutput();
}

QStringList sharedStrings(const QByteArray &xml)
{
    QStringList strings;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("t"))
            strings.append(reader.readElementText());
    }
    return strings;
}

int descriptorCount()
{
    int count = 0;
#ifdef Q_OS_UNIX
    for (int fd = 0; fd < 4096; ++fd)
        if (fcntl(fd, F_GETFD) != -1)
            ++count;
#endif
    return count;
}

SpreadsheetSheet blankSheet()
{
    SpreadsheetSheet sheet;
    sheet.name = QStringLiteral("Bảng dữ liệu");
    sheet.columnWidths = {15, 15, 15, 15};
    sheet.rows.resize(3);
    return sheet;
}
}

class SpreadsheetWorkbookTest final : public QObject
{
    Q_OBJECT

private slots:
    void writesMergedRectanglesAndLiteralText()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("unzip")).isEmpty())
            QSKIP("unzip is required to inspect workbook XML.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("Bảng.xlsx"));
        auto workbook = createSpreadsheetWorkbook();
        QString error;
        QVERIFY2(workbook->open(path, error), qPrintable(error));
        SpreadsheetSheet sheet = blankSheet();
        sheet.rows[0].cells = {{0, QStringLiteral("=SUM(1,2)")},
                               {1, QString()},
                               {2, QStringLiteral("Tiếng Việt 😀\nDòng 2")}};
        sheet.rows[1].cells = {{0, QString()}, {1, QString()},
                               {2, QStringLiteral("0012")}, {3, QStringLiteral("+2")}};
        sheet.rows[2].cells = {{0, QString()}, {1, QString()},
                               {2, QStringLiteral("@name")}, {3, QStringLiteral("-3")}};
        sheet.mergedRanges = {{0, 0, 1, 1}, {0, 2, 0, 3}, {2, 0, 2, 1}};
        QVERIFY2(workbook->writeSheet(sheet, error), qPrintable(error));
        QVERIFY2(workbook->finish(error), qPrintable(error));
        const QByteArray xml = zipEntry(path, QStringLiteral("xl/worksheets/sheet1.xml"));
        QVERIFY(!xml.isEmpty());
        const QStringList strings = sharedStrings(zipEntry(path, QStringLiteral("xl/sharedStrings.xml")));
        QXmlStreamReader reader(xml);
        QString cell;
        QString type;
        QMap<QString, QString> values;
        QStringList merges;
        while (!reader.atEnd()) {
            reader.readNext();
            if (!reader.isStartElement())
                continue;
            QVERIFY(reader.name() != QLatin1String("f"));
            if (reader.name() == QLatin1String("c")) {
                cell = reader.attributes().value(QLatin1String("r")).toString();
                type = reader.attributes().value(QLatin1String("t")).toString();
                values[cell] = QString();
            } else if (reader.name() == QLatin1String("v")) {
                QCOMPARE(type, QStringLiteral("s"));
                bool ok = false;
                const int index = reader.readElementText().toInt(&ok);
                QVERIFY(ok && index >= 0 && index < strings.size());
                values[cell] = strings.at(index);
            } else if (reader.name() == QLatin1String("mergeCell")) {
                merges.append(reader.attributes().value(QLatin1String("ref")).toString());
            }
        }
        QVERIFY(!reader.hasError());
        QCOMPARE(merges, QStringList({QStringLiteral("A1:B2"), QStringLiteral("C1:D1"), QStringLiteral("A3:B3")}));
        QCOMPARE(values.value(QStringLiteral("A1")), QStringLiteral("=SUM(1,2)"));
        QCOMPARE(values.value(QStringLiteral("C1")), QStringLiteral("Tiếng Việt 😀\nDòng 2"));
        QCOMPARE(values.value(QStringLiteral("C2")), QStringLiteral("0012"));
        QCOMPARE(values.value(QStringLiteral("D2")), QStringLiteral("+2"));
        QCOMPARE(values.value(QStringLiteral("C3")), QStringLiteral("@name"));
        QCOMPARE(values.value(QStringLiteral("D3")), QStringLiteral("-3"));
        QVERIFY(values.contains(QStringLiteral("B2")));
        QVERIFY(values.value(QStringLiteral("B2")).isEmpty());
        QVERIFY(values.contains(QStringLiteral("A3")));
        QVERIFY(values.value(QStringLiteral("A3")).isEmpty());
    }

    void rejectsInvalidOrOverlappingMerges()
    {
        const QVector<QVector<SpreadsheetMerge>> invalid = {
            {{-1, 0, 1, 1}}, {{0, -1, 1, 1}}, {{0, 0, 3, 1}},
            {{0, 0, 1, SpreadsheetWorkbook::MaximumColumns}},
            {{0, 0, 0, 0}}, {{1, 0, 0, 1}}, {{0, 1, 1, 0}},
            {{0, 0, 1, 1}, {1, 1, 2, 2}}
        };
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (const auto &ranges : invalid) {
            QString error;
            const QString path = directory.filePath(QStringLiteral("invalid.xlsx"));
            auto workbook = createSpreadsheetWorkbook();
            QVERIFY(workbook->open(path, error));
            SpreadsheetSheet sheet = blankSheet();
            sheet.mergedRanges = ranges;
            QVERIFY(!workbook->writeSheet(sheet, error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!QFile::exists(path));
            QVERIFY(!workbook->finish(error));
        }
    }

    void rejectsCoveredTextWithoutSilentlyDiscardingIt()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (const QString &text : {QStringLiteral("hidden value"), QStringLiteral(" ")}) {
            QString error;
            auto workbook = createSpreadsheetWorkbook();
            QVERIFY(workbook->open(directory.filePath(QStringLiteral("covered.xlsx")), error));
            SpreadsheetSheet sheet = blankSheet();
            sheet.rows[1].cells = {{1, text}};
            sheet.mergedRanges = {{0, 0, 1, 1}};
            QVERIFY(!workbook->writeSheet(sheet, error));
            QVERIFY(error.contains(QStringLiteral("hide nonempty")));
        }
    }

    void boundsMergedCellsAndTextBeforeAllocatingWorksheet()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString error;
        {
            auto workbook = createSpreadsheetWorkbook();
            QVERIFY(workbook->open(directory.filePath(QStringLiteral("large-merge.xlsx")), error));
            SpreadsheetSheet sheet = blankSheet();
            sheet.rows.resize(100);
            sheet.mergedRanges = {{0, 0, 99, 2999}};
            QVERIFY(!workbook->writeSheet(sheet, error));
            QVERIFY(error.contains(QStringLiteral("memory limit")));
        }
        {
            auto workbook = createSpreadsheetWorkbook();
            QVERIFY(workbook->open(directory.filePath(QStringLiteral("large-text.xlsx")), error));
            SpreadsheetSheet sheet = blankSheet();
            const QString text(SpreadsheetWorkbook::MaximumCellTextLength, QLatin1Char('x'));
            for (int column = 0; column < 1025; ++column)
                sheet.rows[0].cells.append({column, text});
            QVERIFY(!workbook->writeSheet(sheet, error));
            QVERIFY(error.contains(QStringLiteral("memory limit")));
        }
    }

    void discardsMergedWorkbooksAndFailedFinalizationWithoutDescriptorLeaks()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const int descriptorsBefore = descriptorCount();
        for (int attempt = 0; attempt < 24; ++attempt) {
            QString error;
            auto workbook = createSpreadsheetWorkbook();
            const QString path = directory.filePath(QStringLiteral("cancelled.xlsx"));
            QVERIFY(workbook->open(path, error));
            SpreadsheetSheet sheet = blankSheet();
            sheet.rows[0].cells = {{0, QStringLiteral("Việt Nam")}};
            sheet.mergedRanges = {{0, 0, 1, 1}};
            QVERIFY(workbook->writeSheet(sheet, error));
            workbook.reset();
            QVERIFY(!QFile::exists(path));
        }
        {
            QString error;
            auto workbook = createSpreadsheetWorkbook();
            // A directory cannot be replaced by workbook_close's file writer.
            QVERIFY(workbook->open(directory.path(), error));
            SpreadsheetSheet sheet = blankSheet();
            sheet.mergedRanges = {{0, 0, 1, 1}};
            QVERIFY(workbook->writeSheet(sheet, error));
            QVERIFY(!workbook->finish(error));
        }
        QCOMPARE(descriptorCount(), descriptorsBefore);
    }
};

QTEST_GUILESS_MAIN(SpreadsheetWorkbookTest)
#include "tst_SpreadsheetWorkbook.moc"
