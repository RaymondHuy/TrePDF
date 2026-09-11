#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::keepsViewSearchStatePerTab()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("view-search-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("view-search-second.pdf"));
    QVERIFY(createThreePageTextPdf(firstPath));
    QVERIFY(createThreePageTextPdf(secondPath));

    const QString themePath = QFINDTESTDATA(
        "../src/Components/AppTheme.qml");
    const QString mainPath = QFINDTESTDATA("../src/Main.qml");
    QVERIFY(!themePath.isEmpty());
    QVERIFY(!mainPath.isEmpty());
    QVERIFY(registerQmlTestTypes(themePath));

    QQmlEngine engine;
    QQmlComponent mainComponent(
        &engine,
        QUrl::fromLocalFile(mainPath));
    std::unique_ptr<QObject> mainWindow(mainComponent.create());
    QVERIFY2(mainWindow, qPrintable(mainComponent.errorString()));
    QVERIFY(mainWindow->setProperty("visible", false));

    auto *tabModel = mainWindow->findChild<PDFTabModel *>();
    QVERIFY(tabModel);
    PDFDocumentSession *firstSession = tabModel->open(
        QUrl::fromLocalFile(firstPath));
    QVERIFY(firstSession);
    firstSession->searchController()->search(
        QStringLiteral("First fixture page"));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->searchController()->state(),
        PDFSearchController::Ready,
        5000);

    QObject *searchField = nullptr;
    QObject *previousButton = nullptr;
    QObject *nextButton = nullptr;
    QObject *clearButton = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            searchField = mainWindow->findChild<QObject *>(
                QStringLiteral("documentSearchField"));
            previousButton = mainWindow->findChild<QObject *>(
                QStringLiteral("previousSearchResultButton"));
            nextButton = mainWindow->findChild<QObject *>(
                QStringLiteral("nextSearchResultButton"));
            clearButton = mainWindow->findChild<QObject *>(
                QStringLiteral("clearSearchButton"));
            return searchField && previousButton && nextButton && clearButton;
        })(),
        2000);
    QTRY_COMPARE_WITH_TIMEOUT(
        searchField->property("text").toString(),
        QStringLiteral("First fixture page"),
        1000);

    PDFDocumentSession *secondSession = tabModel->open(
        QUrl::fromLocalFile(secondPath));
    QVERIFY(secondSession);
    QTRY_VERIFY_WITH_TIMEOUT(
        searchField->property("text").toString().isEmpty(),
        1000);

    secondSession->searchController()->search(
        QStringLiteral("Third fixture page"));
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->searchController()->state(),
        PDFSearchController::Ready,
        5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        searchField->property("text").toString(),
        QStringLiteral("Third fixture page"),
        1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->document()->currentPage(),
        2,
        2000);

    tabModel->setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(
        searchField->property("text").toString(),
        QStringLiteral("First fixture page"),
        1000);
    QCOMPARE(firstSession->searchController()->resultCount(), 1);
    QCOMPARE(secondSession->searchController()->resultCount(), 1);

    tabModel->setCurrentIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(
        searchField->property("text").toString(),
        QStringLiteral("Third fixture page"),
        1000);
    QVERIFY(QMetaObject::invokeMethod(
        clearButton,
        "click",
        Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->searchController()->state(),
        PDFSearchController::Idle,
        1000);
    QVERIFY(secondSession->searchController()->query().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(
        searchField->property("text").toString().isEmpty(),
        1000);
    QCOMPARE(
        firstSession->searchController()->query(),
        QStringLiteral("First fixture page"));

    firstSession->editController()->setTool(
        PDFEditController::StickyNoteTool);
    QVERIFY(!firstSession->editController()
                 ->addStickyNote(0, QPointF(0.2, 0.2))
                 .isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->searchController()->state(),
        PDFSearchController::Idle,
        1000);
    QVERIFY(firstSession->searchController()->query().isEmpty());

    QObject *ribbonTabs = mainWindow->findChild<QObject *>(
        QStringLiteral("ribbonTabs"));
    QVERIFY(ribbonTabs);
    secondSession->document()->setCurrentPage(0);
    QVERIFY(ribbonTabs->setProperty("currentIndex", 2));
    secondSession->searchController()->search(
        QStringLiteral("Third fixture page"));
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->searchController()->state(),
        PDFSearchController::Ready,
        5000);
    QVERIFY(secondSession->document()->currentPage() != 2);

    QVERIFY(ribbonTabs->setProperty("currentIndex", 0));
    QCOMPARE(secondSession->searchController()->nextResult(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->document()->currentPage(),
        2,
        2000);

    QVERIFY(mainWindow->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::searchesDocumentTextAcrossPages()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("document-search-source.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFSearchController controller;
    controller.setDocument(&document);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

    QSignalSpy activatedSpy(
        &controller,
        &PDFSearchController::resultActivated);
    controller.search(QStringLiteral("FIXTURE PAGE"));
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.state(),
        PDFSearchController::Ready,
        5000);
    QCOMPARE(controller.query(), QStringLiteral("FIXTURE PAGE"));
    QCOMPARE(controller.resultCount(), 3);
    QCOMPARE(controller.currentResultIndex(), 0);
    QVERIFY(!activatedSpy.isEmpty());
    QCOMPARE(activatedSpy.constLast().at(0).toInt(), 0);
    QCOMPARE(activatedSpy.constLast().size(), 4);
    QVERIFY(activatedSpy.constLast().at(1).toReal() >= 0.0);
    QVERIFY(activatedSpy.constLast().at(1).toReal() <= 1.0);
    QVERIFY(activatedSpy.constLast().at(2).toReal() >= 0.0);
    QVERIFY(activatedSpy.constLast().at(2).toReal() <= 1.0);
    QCOMPARE(
        activatedSpy.constLast().at(3).toULongLong(),
        controller.resultsRevision());

    const QVariantList firstPageHighlights
        = controller.highlightsForPage(0);
    QVERIFY(!firstPageHighlights.isEmpty());
    const QVariantMap firstHighlight = firstPageHighlights.constFirst().toMap();
    QVERIFY(firstHighlight.value(QStringLiteral("current")).toBool());
    const QRectF firstRect = rectFromCoordinateMap(firstHighlight);
    QVERIFY(firstRect.left() >= 0.0);
    QVERIFY(firstRect.top() >= 0.0);
    QVERIFY(firstRect.right() <= 1.0);
    QVERIFY(firstRect.bottom() <= 1.0);
    QVERIFY(!firstRect.isEmpty());

    QCOMPARE(controller.nextResult(), 1);
    QCOMPARE(controller.currentResultIndex(), 1);
    QVERIFY(!controller.highlightsForPage(1).isEmpty());
    QCOMPARE(controller.previousResult(), 0);
    QCOMPARE(controller.previousResult(), 2);
    QCOMPARE(controller.currentResultIndex(), 2);

    controller.search(QStringLiteral("i"));
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.state(),
        PDFSearchController::Ready,
        5000);
    QCOMPARE(controller.resultCount(), 5);
    QCOMPARE(controller.currentResultIndex(), 0);
    const quint64 previousResultsRevision = controller.resultsRevision();
    QCOMPARE(controller.nextResult(), 0);
    QCOMPARE(controller.currentResultIndex(), 1);
    QVERIFY(controller.resultsRevision() > previousResultsRevision);
    QCOMPARE(activatedSpy.constLast().at(0).toInt(), 0);
    int currentHighlightCount = 0;
    for (const QVariant &highlight : controller.highlightsForPage(0)) {
        if (highlight.toMap().value(QStringLiteral("current")).toBool()) {
            ++currentHighlightCount;
        }
    }
    QCOMPARE(currentHighlightCount, 1);

    controller.search(QStringLiteral("missing phrase"));
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.state(),
        PDFSearchController::Ready,
        5000);
    QCOMPARE(controller.resultCount(), 0);
    QCOMPARE(controller.currentResultIndex(), -1);
    QVERIFY(controller.highlightsForPage(0).isEmpty());

    controller.clearSearch();
    QCOMPARE(controller.state(), PDFSearchController::Idle);
    QVERIFY(controller.query().isEmpty());

    controller.scheduleSearch(QStringLiteral("First fixture page"));
    QCOMPARE(controller.state(), PDFSearchController::Searching);
    QCOMPARE(controller.query(), QStringLiteral("First fixture page"));
    QCOMPARE(controller.resultCount(), 0);
    controller.scheduleSearch(QStringLiteral("Third fixture page"));
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.state(),
        PDFSearchController::Ready,
        5000);
    QCOMPARE(controller.query(), QStringLiteral("Third fixture page"));
    QCOMPARE(controller.resultCount(), 1);
    QCOMPARE(controller.currentResultIndex(), 0);

    QString surrogateBoundaryQuery(255, QLatin1Char('a'));
    surrogateBoundaryQuery.append(QString::fromUtf8("😀tail"));
    controller.prepareSearchText(surrogateBoundaryQuery);
    QCOMPARE(controller.query().size(), 255);
    QVERIFY(!controller.query().back().isHighSurrogate());
    controller.clearSearch();
}

void PDFEditControllerTest::replacesStaleDocumentSearch()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("document-search-replacement.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFSearchController controller;
    controller.setDocument(&document);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

    controller.search(QStringLiteral("First fixture page"));
    controller.search(QStringLiteral("Third fixture page"));
    QTRY_VERIFY_WITH_TIMEOUT(
        controller.state() == PDFSearchController::Ready
            && controller.query() == QStringLiteral("Third fixture page"),
        5000);
    QCOMPARE(controller.resultCount(), 1);
    QCOMPARE(controller.currentResultIndex(), 0);
    QVERIFY(controller.highlightsForPage(0).isEmpty());
    QVERIFY(!controller.highlightsForPage(2).isEmpty());

    document.close();
    QCOMPARE(controller.state(), PDFSearchController::Idle);
    QCOMPARE(controller.resultCount(), 0);
    QVERIFY(controller.query().isEmpty());
}
