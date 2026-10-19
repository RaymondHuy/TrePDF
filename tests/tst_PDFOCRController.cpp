#include "PDFDocument.h"
#include "PDFOCRController.h"
#include "PDFOCRPipeline.h"
#include "OCRLanguageManager.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <chrono>
#include <thread>

namespace {
// This suite exercises dispatch/commit boundaries without tying cancellation
// races to recognition speed. Actual OCR and PDF validity have pipeline tests.
std::atomic_bool entered{false};
std::atomic_bool releaseWorker{true};
std::atomic_int activeWorkers{0};
bool failRecognition = false;
bool noNewText = false;
QVector<int> capturedPages;
QString capturedLanguage;
QHash<QString, QString> capturedModels;
const QByteArray outputBytes("%PDF-1.7\ncontroller-test-output\n");

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeBlankPDF(const QString &path, int pageCount = 5)
{
    QVector<QByteArray> objects;
    objects.append("<< /Type /Catalog /Pages 2 0 R >>");
    QByteArray kids;
    for (int page = 0; page < pageCount; ++page)
        kids += QByteArray::number(page + 3) + " 0 R ";
    objects.append("<< /Type /Pages /Count " + QByteArray::number(pageCount)
                   + " /Kids [" + kids + "] >>");
    for (int page = 0; page < pageCount; ++page)
        objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << >> >>");
    QByteArray pdf("%PDF-1.4\n");
    QVector<qint64> offsets{0};
    for (int index = 0; index < objects.size(); ++index) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(index + 1) + " 0 obj\n" + objects.at(index) + "\nendobj\n";
    }
    const qint64 xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (int index = 1; index < offsets.size(); ++index)
        pdf += QByteArray::number(offsets.at(index)).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1)
        + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return writeBytes(path, pdf);
}
}

namespace PDFOCRPipeline {
Result run(const Request &request, const QString &stagedOutputPath,
           const std::function<void(int)> &progress)
{
    ++activeWorkers;
    struct Exit { ~Exit() { --activeWorkers; } } exit;
    capturedPages = request.sourcePages;
    capturedLanguage = request.language;
    capturedModels = request.modelPaths;
    entered.store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!releaseWorker.load() && !request.cancellation->load()
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Result result;
    if (request.cancellation->load()) {
        result.outcome = Result::Cancelled;
        return result;
    }
    if (!releaseWorker.load() || failRecognition) {
        result.error = QStringLiteral("Intentional pipeline failure.");
        return result;
    }
    if (noNewText) {
        result.outcome = Result::NoText;
        result.skippedPages = request.sourcePages.size();
        return result;
    }
    for (int page = 0; page < request.sourcePages.size(); ++page)
        progress(page + 1);
    if (!writeBytes(stagedOutputPath, outputBytes))
        return result;
    result.recognizedPages = request.sourcePages.size();
    result.outcome = Result::Succeeded;
    return result;
}
}

class PDFOCRControllerTest : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        QTRY_COMPARE_WITH_TIMEOUT(activeWorkers.load(), 0, 12000);
        entered.store(false);
        releaseWorker.store(true);
        failRecognition = false;
        noNewText = false;
        capturedPages.clear();
        capturedLanguage.clear();
        capturedModels.clear();
    }

    void requiresLoadedSavedDocument()
    {
        QTemporaryDir directory;
        const QUrl target = QUrl::fromLocalFile(directory.filePath("out.pdf"));
        PDFOCRController controller;
        QVERIFY(controller.available());
        QVERIFY(!controller.recognizeTo(target, QStringLiteral("eng")));
        QVERIFY(!controller.busy());
        const QString source = directory.filePath("in.pdf");
        QVERIFY(writeBlankPDF(source));
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        controller.setDocument(&document);
        controller.setRecognitionGuard([]() { return false; });
        QVERIFY(!controller.recognizeTo(target, QStringLiteral("eng")));
        QVERIFY(!entered.load());
        QVERIFY(!QFile::exists(target.toLocalFile()));
    }

    void validatesLanguagesRangesAndDestination_data()
    {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("range");
        QTest::newRow("unknown-language") << QStringLiteral("zzz") << QString();
        QTest::newRow("empty-language") << QString() << QString();
        QTest::newRow("traversal-language") << QStringLiteral("../eng") << QString();
        QTest::newRow("duplicate-language") << QStringLiteral("eng+eng") << QString();
        QTest::newRow("empty-language-part") << QStringLiteral("eng++vie") << QString();
        QTest::newRow("too-many-languages") << QStringLiteral("eng+vie+fra+tha+hrv") << QString();
        QTest::newRow("zero") << QStringLiteral("eng") << QStringLiteral("0");
        QTest::newRow("negative") << QStringLiteral("eng") << QStringLiteral("-1");
        QTest::newRow("backwards") << QStringLiteral("eng") << QStringLiteral("3-1");
        QTest::newRow("outside-document") << QStringLiteral("eng") << QStringLiteral("1-6");
        QTest::newRow("trailing-comma") << QStringLiteral("eng") << QStringLiteral("1,");
        QTest::newRow("partial-range") << QStringLiteral("eng") << QStringLiteral("1-");
        QTest::newRow("numeric-overflow") << QStringLiteral("eng") << QStringLiteral("999999999999999999");
        QTest::newRow("too-long") << QStringLiteral("eng") << QString(4097, QLatin1Char('1'));
    }

    void validatesLanguagesRangesAndDestination()
    {
        QFETCH(QString, language);
        QFETCH(QString, range);
        QTemporaryDir directory;
        const QString source = directory.filePath("in.pdf");
        QVERIFY(writeBlankPDF(source));
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        PDFOCRController controller;
        controller.setDocument(&document);
        QVERIFY(!controller.recognizeTo(QUrl::fromLocalFile(directory.filePath("out.pdf")), language, range));
        QVERIFY(!controller.busy());
        QVERIFY(!entered.load());
    }

    void refusesSourceAndOpenDestination()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath("in.pdf");
        QVERIFY(writeBlankPDF(source));
        const QByteArray original = readBytes(source);
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        PDFOCRController controller;
        controller.setDocument(&document);
        QVERIFY(!controller.recognizeTo(QUrl::fromLocalFile(source), QStringLiteral("eng")));
        QVERIFY(!controller.recognizeTo(QUrl::fromLocalFile(directory.filePath("out.txt")), QStringLiteral("eng")));
        controller.setDestinationGuard([](const QUrl &) { return false; });
        QVERIFY(!controller.recognizeTo(QUrl::fromLocalFile(directory.filePath("out.pdf")), QStringLiteral("eng")));
        QCOMPARE(readBytes(source), original);
        QVERIFY(!entered.load());
    }

    void commitsCopyAndDeduplicatesPages()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath("in.pdf");
        const QString target = directory.filePath("out.pdf");
        QVERIFY(writeBlankPDF(source));
        QVERIFY(writeBytes(target, "previous output"));
        const QByteArray original = readBytes(source);
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        PDFOCRController controller;
        controller.setDocument(&document);
        QSignalSpy finished(&controller, &PDFOCRController::finished);
        QVERIFY(controller.recognizeTo(QUrl::fromLocalFile(target), QStringLiteral("eng+vie"),
                                       QStringLiteral("5, 2 - 3, 2, 1")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 12000);
        QVERIFY(finished.first().at(0).toBool());
        QCOMPARE(capturedPages, QVector<int>({0, 1, 2, 4}));
        QCOMPARE(capturedLanguage, QStringLiteral("eng+vie"));
        QCOMPARE(capturedModels.size(), 2);
        QCOMPARE(capturedModels.value(QStringLiteral("eng")), OCRLanguageManager::installedModelPath(QStringLiteral("eng")));
        QCOMPARE(capturedModels.value(QStringLiteral("vie")), OCRLanguageManager::installedModelPath(QStringLiteral("vie")));
        QCOMPARE(controller.completedPages(), 4);
        QCOMPARE(controller.totalPages(), 4);
        QCOMPARE(controller.recognizedPages(), 4);
        QVERIFY(!controller.busy());
        QCOMPARE(readBytes(target), outputBytes);
        QCOMPARE(readBytes(source), original);
    }

    void preservesLanguagePriorityOrder()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath("in.pdf");
        QVERIFY(writeBlankPDF(source));
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        PDFOCRController controller;
        controller.setDocument(&document);
        QSignalSpy finished(&controller, &PDFOCRController::finished);
        QVERIFY(controller.recognizeTo(QUrl::fromLocalFile(directory.filePath("out.pdf")),
                                       QStringLiteral("vie+eng"), QStringLiteral("1")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 12000);
        QVERIFY(finished.first().at(0).toBool());
        QCOMPARE(capturedLanguage, QStringLiteral("vie+eng"));
        QCOMPARE(capturedModels.size(), 2);
        for (const QString &path : std::as_const(capturedModels))
            QVERIFY(QFileInfo(path).isAbsolute());
    }

    void cancellationPreservesExistingDestinationAndBoundsWorkers()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath("in.pdf");
        const QString target = directory.filePath("out.pdf");
        QVERIFY(writeBlankPDF(source));
        QVERIFY(writeBytes(target, "previous output"));
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        PDFOCRController controller;
        PDFOCRController second;
        controller.setDocument(&document);
        second.setDocument(&document);
        QSignalSpy finished(&controller, &PDFOCRController::finished);
        releaseWorker.store(false);
        QVERIFY(controller.recognizeTo(QUrl::fromLocalFile(target), QStringLiteral("vie")));
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 12000);
        QVERIFY(!second.recognizeTo(QUrl::fromLocalFile(directory.filePath("other.pdf")), QStringLiteral("eng")));
        QVERIFY(!second.busy());
        QCOMPARE(activeWorkers.load(), 1);
        controller.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 12000);
        QVERIFY(!finished.first().at(0).toBool());
        QCOMPARE(readBytes(target), QByteArray("previous output"));
    }

    void sourceChangesPreventCommit()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath("in.pdf");
        const QString target = directory.filePath("out.pdf");
        QVERIFY(writeBlankPDF(source));
        QVERIFY(writeBytes(target, "previous output"));
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        PDFOCRController controller;
        controller.setDocument(&document);
        QSignalSpy finished(&controller, &PDFOCRController::finished);
        releaseWorker.store(false);
        QVERIFY(controller.recognizeTo(QUrl::fromLocalFile(target), QStringLiteral("eng")));
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 12000);
        QFile sourceFile(source);
        QVERIFY(sourceFile.open(QIODevice::Append));
        QVERIFY(sourceFile.write("\n% external change\n") > 0);
        sourceFile.close();
        releaseWorker.store(true);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 12000);
        QVERIFY(!finished.first().at(0).toBool());
        QCOMPARE(readBytes(target), QByteArray("previous output"));
    }

    void closingDocumentCancelsWork()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath("in.pdf");
        const QString target = directory.filePath("out.pdf");
        QVERIFY(writeBlankPDF(source));
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        PDFOCRController controller;
        controller.setDocument(&document);
        QSignalSpy finished(&controller, &PDFOCRController::finished);
        releaseWorker.store(false);
        QVERIFY(controller.recognizeTo(QUrl::fromLocalFile(target), QStringLiteral("eng")));
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 12000);
        document.close();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 12000);
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(!QFile::exists(target));
    }

    void pipelineFailureAndNoTextPreserveDestination_data()
    {
        QTest::addColumn<bool>("emptyResult");
        QTest::newRow("pipeline-failure") << false;
        QTest::newRow("already-has-text") << true;
    }

    void pipelineFailureAndNoTextPreserveDestination()
    {
        QFETCH(bool, emptyResult);
        QTemporaryDir directory;
        const QString source = directory.filePath("in.pdf");
        const QString target = directory.filePath("out.pdf");
        QVERIFY(writeBlankPDF(source));
        QVERIFY(writeBytes(target, "previous output"));
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        PDFOCRController controller;
        controller.setDocument(&document);
        QSignalSpy finished(&controller, &PDFOCRController::finished);
        noNewText = emptyResult;
        failRecognition = !emptyResult;
        QVERIFY(controller.recognizeTo(QUrl::fromLocalFile(target), QStringLiteral("eng")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 12000);
        QVERIFY(!finished.first().at(0).toBool());
        QCOMPARE(readBytes(target), QByteArray("previous output"));
        QCOMPARE(controller.skippedPages(), emptyResult ? 5 : 0);
    }
};

QTEST_GUILESS_MAIN(PDFOCRControllerTest)
#include "tst_PDFOCRController.moc"
