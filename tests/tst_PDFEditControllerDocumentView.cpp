#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::exposesExcelExportTabAndDraftGuard()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("export-ui.pdf"));
    QVERIFY(createFontStylePdf(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    window->setProperty("visible", false);
    auto *model = window->findChild<PDFTabModel *>();
    QVERIFY(model);
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(tabs);
    QVERIFY(tabs->setProperty("currentIndex", 5));
    QTRY_COMPARE(session->editController()->tool(), PDFEditController::BrowseTool);
    QObject *button = nullptr;
    QTRY_VERIFY((button = window->findChild<QObject *>(QStringLiteral("exportExcelButton"))));
    QVERIFY(button->property("enabled").toBool());
    QVERIFY(session->editController()->addBlankPage());
    QTRY_VERIFY(!button->property("enabled").toBool());
    const auto target = QUrl::fromLocalFile(m_temporaryDirectory.filePath(QStringLiteral("guard.xlsx")));
    QVERIFY(!model->requestExcelExport(session, target));
    QVERIFY(!session->excelExportController()->busy());
    session->editController()->discardDrafts();
    QTRY_VERIFY(button->property("enabled").toBool());
    QVERIFY(window->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::defaultsCommentTabToCommentSelectTool()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-tab-default-select.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

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
    PDFDocumentSession *session = tabModel->open(
        QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);

    QObject *ribbonTabs = mainWindow->findChild<QObject *>(
        QStringLiteral("ribbonTabs"));
    QVERIFY(ribbonTabs);

    PDFEditController *controller = session->editController();
    QVERIFY(ribbonTabs->setProperty("currentIndex", 1));
    QTRY_COMPARE_WITH_TIMEOUT(
        controller->tool(),
        PDFEditController::SelectTool,
        1000);

    QVERIFY(ribbonTabs->setProperty("currentIndex", 2));
    QTRY_COMPARE_WITH_TIMEOUT(
        controller->tool(),
        PDFEditController::CommentSelectTool,
        1000);

    QObject *selectButton = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            selectButton = mainWindow->findChild<QObject *>(
                QStringLiteral("commentSelectButton"));
            return selectButton != nullptr;
        })(),
        1000);
    QVERIFY(selectButton->property("primary").toBool());

    QVERIFY(mainWindow->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::appliesPageDisplayActionsToActiveTab()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The Page Display integration test needs a Qt Quick screen.");
    }

    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("page-display-ui-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("page-display-ui-second.pdf"));
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

    QObject *pageDisplayControl = nullptr;
    QObject *pageDisplayPopup = nullptr;
    QObject *singlePageItem = nullptr;
    QObject *continuousItem = nullptr;
    QObject *facingItem = nullptr;
    QObject *continuousFacingItem = nullptr;
    QObject *separateCoverItem = nullptr;
    QObject *splitItem = nullptr;
    QObject *splitMenuButton = nullptr;
    QObject *splitModeMenu = nullptr;
    QObject *verticalSplitItem = nullptr;
    QObject *horizontalSplitItem = nullptr;
    QObject *previousPageButton = nullptr;
    QObject *nextPageButton = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            pageDisplayControl = mainWindow->findChild<QObject *>(
                QStringLiteral("pageDisplayControl"));
            pageDisplayPopup = mainWindow->findChild<QObject *>(
                QStringLiteral("pageDisplayPopup"));
            singlePageItem = mainWindow->findChild<QObject *>(
                QStringLiteral("singlePageDisplayItem"));
            continuousItem = mainWindow->findChild<QObject *>(
                QStringLiteral("continuousPageDisplayItem"));
            facingItem = mainWindow->findChild<QObject *>(
                QStringLiteral("facingPageDisplayItem"));
            continuousFacingItem = mainWindow->findChild<QObject *>(
                QStringLiteral("continuousFacingPageDisplayItem"));
            separateCoverItem = mainWindow->findChild<QObject *>(
                QStringLiteral("separateCoverPageDisplayItem"));
            splitItem = mainWindow->findChild<QObject *>(
                QStringLiteral("splitPageDisplayItem"));
            splitMenuButton = mainWindow->findChild<QObject *>(
                QStringLiteral("splitPageDisplayMenuButton"));
            splitModeMenu = mainWindow->findChild<QObject *>(
                QStringLiteral("splitPageDisplayModeMenu"));
            verticalSplitItem = mainWindow->findChild<QObject *>(
                QStringLiteral("verticalSplitPageDisplayItem"));
            horizontalSplitItem = mainWindow->findChild<QObject *>(
                QStringLiteral("horizontalSplitPageDisplayItem"));
            previousPageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("previousViewPageButton"));
            nextPageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("nextViewPageButton"));
            return pageDisplayControl
                && pageDisplayPopup
                && singlePageItem
                && continuousItem
                && facingItem
                && continuousFacingItem
                && separateCoverItem
                && splitItem
                && splitMenuButton
                && splitModeMenu
                && verticalSplitItem
                && horizontalSplitItem
                && previousPageButton
                && nextPageButton;
        })(),
        3000);
    QVERIFY(!mainWindow->findChild<QObject *>(
        QStringLiteral("spreadsheetSplitPageDisplayItem")));

    const auto click = [](QObject *item) {
        return QMetaObject::invokeMethod(
            item,
            "clicked",
            Qt::DirectConnection);
    };
    const auto trigger = [](QObject *item) {
        return QMetaObject::invokeMethod(
            item,
            "triggered",
            Qt::DirectConnection);
    };
    const auto polishSplitLayout = [](QObject *loader) {
        auto *loaderItem = qobject_cast<QQuickItem *>(loader);
        QQuickItem *layoutItem = loaderItem
            ? loaderItem->parentItem()
            : nullptr;
        if (!layoutItem) {
            return false;
        }
        layoutItem->ensurePolished();
        return true;
    };

    QCOMPARE(
        pageDisplayControl->property("pageDisplayMode").toInt(),
        static_cast<int>(PDFDocumentSession::ContinuousMode));
    QVERIFY(continuousItem->property("checked").toBool());
    QVERIFY(!singlePageItem->property("checked").toBool());
    QVERIFY(pageDisplayControl->property("menuIndicatorVisible").toBool());
    QVERIFY(click(pageDisplayControl));
    QTRY_VERIFY_WITH_TIMEOUT(
        pageDisplayPopup->property("visible").toBool(),
        1000);
    QVERIFY(click(splitMenuButton));
    QTRY_VERIFY_WITH_TIMEOUT(
        splitModeMenu->property("visible").toBool(),
        1000);
    QVERIFY(QMetaObject::invokeMethod(
        splitModeMenu,
        "close",
        Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        pageDisplayPopup,
        "close",
        Qt::DirectConnection));
    QVERIFY(!separateCoverItem->property("enabled").toBool());
    QVERIFY(click(separateCoverItem));
    QVERIFY(!firstSession->separateCoverPage());

    QVERIFY(click(facingItem));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->pageDisplayMode(),
        PDFDocumentSession::FacingMode,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        facingItem->property("checked").toBool(),
        1000);
    QObject *pagedPageList = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            QObject *initialPrimaryView = mainWindow->findChild<QObject *>(
                QStringLiteral("primaryDocumentView"));
            pagedPageList = initialPrimaryView
                ? initialPrimaryView->findChild<QObject *>(
                    QStringLiteral("documentPageList"))
                : nullptr;
            return pagedPageList
                && pagedPageList->property("count").toInt() == 1;
        })(),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        separateCoverItem->property("enabled").toBool(),
        1000);

    QVERIFY(click(nextPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 2, 1000);
    QVERIFY(click(previousPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 0, 1000);

    QVERIFY(click(separateCoverItem));
    QTRY_VERIFY_WITH_TIMEOUT(firstSession->separateCoverPage(), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        separateCoverItem->property("checked").toBool(),
        1000);
    QVERIFY(click(nextPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 1, 1000);
    QVERIFY(click(previousPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 0, 1000);

    QVERIFY(click(splitItem));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->splitViewMode(),
        PDFDocumentSession::VerticalSplit,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        splitItem->property("checked").toBool(),
        1000);

    QObject *primaryView = nullptr;
    QObject *secondaryView = nullptr;
    QObject *secondaryViewLoader = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            primaryView = mainWindow->findChild<QObject *>(
                QStringLiteral("primaryDocumentView"));
            secondaryView = mainWindow->findChild<QObject *>(
                QStringLiteral("secondaryDocumentView"));
            secondaryViewLoader = mainWindow->findChild<QObject *>(
                QStringLiteral("secondaryDocumentViewLoader"));
            return primaryView
                && secondaryView
                && secondaryViewLoader
                && polishSplitLayout(secondaryViewLoader)
                && secondaryViewLoader->property("x").toReal()
                    > primaryView->property("x").toReal();
        })(),
        3000);

    QVERIFY(trigger(horizontalSplitItem));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->splitViewMode(),
        PDFDocumentSession::HorizontalSplit,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        polishSplitLayout(secondaryViewLoader)
            && secondaryViewLoader->property("y").toReal()
            > primaryView->property("y").toReal(),
        3000);

    PDFDocumentSession *secondSession = tabModel->open(
        QUrl::fromLocalFile(secondPath));
    QVERIFY(secondSession);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageDisplayControl->property("pageDisplayMode").toInt(),
        static_cast<int>(PDFDocumentSession::ContinuousMode),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !pageDisplayControl->property("separateCoverPage").toBool(),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !pageDisplayControl->property("splitViewEnabled").toBool(),
        1000);

    secondSession->setZoom(1.6);
    QVERIFY(click(singlePageItem));
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->pageDisplayMode(),
        PDFDocumentSession::SinglePageMode,
        1000);
    QCOMPARE(secondSession->zoom(), 1.6);
    QVERIFY(!separateCoverItem->property("enabled").toBool());

    secondSession->setSplitScrollPosition(20.0, 40.0);
    QVERIFY(click(splitItem));
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->splitViewMode(),
        PDFDocumentSession::VerticalSplit,
        1000);
    QCOMPARE(secondSession->splitScrollX(), 20.0);
    QCOMPARE(secondSession->splitScrollY(), 40.0);
    QVERIFY(click(splitItem));
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->splitViewMode(),
        PDFDocumentSession::NoSplit,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !splitItem->property("checked").toBool(),
        1000);
    QVERIFY(firstSession->splitViewEnabled());

    tabModel->setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageDisplayControl->property("pageDisplayMode").toInt(),
        static_cast<int>(PDFDocumentSession::FacingMode),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        pageDisplayControl->property("separateCoverPage").toBool(),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        pageDisplayControl->property("splitViewEnabled").toBool(),
        1000);
    QCOMPARE(firstSession->splitViewMode(), PDFDocumentSession::HorizontalSplit);

    QVERIFY(click(continuousItem));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->pageDisplayMode(),
        PDFDocumentSession::ContinuousMode,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !separateCoverItem->property("enabled").toBool(),
        1000);
    QVERIFY(click(separateCoverItem));
    QVERIFY(firstSession->separateCoverPage());

    QVERIFY(click(continuousFacingItem));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->pageDisplayMode(),
        PDFDocumentSession::ContinuousFacingMode,
        1000);
    QVERIFY(click(splitItem));
    QTRY_VERIFY_WITH_TIMEOUT(!firstSession->splitViewEnabled(), 1000);

    QVERIFY(mainWindow->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::supportsHorizontalScrollingAcrossPageDisplayModes()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The horizontal scrolling integration test needs a Qt Quick screen.");
    }

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("horizontal-scrolling-ui.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

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
    PDFDocumentSession *session = tabModel->open(
        QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);

    QObject *workspace = nullptr;
    QObject *primaryView = nullptr;
    QObject *pageList = nullptr;
    QObject *horizontalScrollBar = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            workspace = mainWindow->findChild<QObject *>(
                QStringLiteral("documentWorkspace"));
            primaryView = mainWindow->findChild<QObject *>(
                QStringLiteral("primaryDocumentView"));
            pageList = primaryView
                ? primaryView->findChild<QObject *>(
                    QStringLiteral("documentPageList"))
                : nullptr;
            horizontalScrollBar = primaryView
                ? primaryView->findChild<QObject *>(
                    QStringLiteral("documentHorizontalScrollBar"))
                : nullptr;
            return workspace && primaryView && pageList && horizontalScrollBar;
        })(),
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        workspace->property("initialViewReady").toBool(),
        3000);

    const auto setPosition = [primaryView](qreal x, qreal y) {
        return QMetaObject::invokeMethod(
            primaryView,
            "setViewportPosition",
            Qt::DirectConnection,
            Q_ARG(QVariant, x),
            Q_ARG(QVariant, y));
    };
    session->setZoom(3.0);
    const QVector<PDFDocumentSession::PageDisplayMode> modes{
        PDFDocumentSession::SinglePageMode,
        PDFDocumentSession::ContinuousMode,
        PDFDocumentSession::FacingMode,
        PDFDocumentSession::ContinuousFacingMode,
    };
    for (const PDFDocumentSession::PageDisplayMode mode : modes) {
        const int previousNavigationGeneration
            = primaryView->property("navigationGeneration").toInt();
        session->setPageDisplayMode(mode);
        QTRY_VERIFY_WITH_TIMEOUT(
            primaryView->property("navigationGeneration").toInt()
                >= previousNavigationGeneration + 2,
            2000);
        QTRY_COMPARE_WITH_TIMEOUT(
            primaryView->property("pendingNavigationPage").toInt(),
            -1,
            2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            pageList->property("contentWidth").toReal()
                > pageList->property("width").toReal() + 1.0,
            2000);

        const qreal maximumX
            = pageList->property("contentWidth").toReal()
            - pageList->property("width").toReal();
        const qreal targetX = maximumX * 0.5;
        QVERIFY(setPosition(targetX, 0.0));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(pageList->property("contentX").toReal() - targetX) < 1.0,
            1000);
        QVERIFY(horizontalScrollBar->property("size").toReal() < 0.999);
        auto *thumb = qvariant_cast<QQuickItem *>(
            horizontalScrollBar->property("contentItem"));
        QVERIFY(thumb);
        QTRY_VERIFY_WITH_TIMEOUT(thumb->opacity() > 0.5, 1000);

        // An attached scrollbar must move the same content as programmatic pan.
        const qreal barTargetX = maximumX * 0.25;
        QVERIFY(horizontalScrollBar->setProperty(
            "position",
            barTargetX / pageList->property("contentWidth").toReal()));
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(pageList->property("contentX").toReal() - barTargetX) < 1.0,
            1000);
        if (mode == PDFDocumentSession::SinglePageMode
            || mode == PDFDocumentSession::FacingMode) {
            const int pageBefore = session->document()->currentPage();
            QVariant handled;
            QVERIFY(QMetaObject::invokeMethod(
                primaryView,
                "scrollPagedBy",
                Qt::DirectConnection,
                Q_RETURN_ARG(QVariant, handled),
                Q_ARG(QVariant, 48.0),
                Q_ARG(QVariant, 0.0)));
            QVERIFY(handled.toBool());
            QVERIFY(pageList->property("contentX").toReal() > barTargetX);
            QCOMPARE(session->document()->currentPage(), pageBefore);
        }

        QVERIFY(setPosition(maximumX + 500.0, 0.0));
        QCOMPARE(pageList->property("contentX").toReal(), maximumX);
        session->setZoom(0.1);
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(pageList->property("contentWidth").toReal()
                 - pageList->property("width").toReal()) < 1.0,
            2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            qAbs(pageList->property("contentX").toReal()) < 1.0,
            1000);
        QTRY_VERIFY_WITH_TIMEOUT(thumb->opacity() < 0.01, 1000);
        session->setZoom(3.0);
    }

    const qreal unrotatedWidth = pageList->property("contentWidth").toReal();
    session->setViewRotation(90);
    QTRY_VERIFY_WITH_TIMEOUT(
        pageList->property("contentWidth").toReal() > unrotatedWidth,
        2000);
    QVERIFY(setPosition(120.0, 0.0));
    QCOMPARE(pageList->property("contentX").toReal(), 120.0);

    QVERIFY(mainWindow->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::keepsHorizontalScrollingIndependentAcrossSplitPanes()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The split horizontal scrolling test needs a Qt Quick screen.");
    }

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("split-horizontal-scrolling-ui.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));
    const QString themePath = QFINDTESTDATA("../src/Components/AppTheme.qml");
    const QString mainPath = QFINDTESTDATA("../src/Main.qml");
    QVERIFY(!themePath.isEmpty());
    QVERIFY(!mainPath.isEmpty());
    QVERIFY(registerQmlTestTypes(themePath));

    QQmlEngine engine;
    QQmlComponent mainComponent(&engine, QUrl::fromLocalFile(mainPath));
    std::unique_ptr<QObject> mainWindow(mainComponent.create());
    QVERIFY2(mainWindow, qPrintable(mainComponent.errorString()));
    QVERIFY(mainWindow->setProperty("visible", false));
    auto *tabModel = mainWindow->findChild<PDFTabModel *>();
    QVERIFY(tabModel);
    PDFDocumentSession *session = tabModel->open(QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);

    QObject *workspace = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            workspace = mainWindow->findChild<QObject *>(
                QStringLiteral("documentWorkspace"));
            return workspace && workspace->property("initialViewReady").toBool();
        })(),
        3000);
    session->setZoom(3.0);
    session->setPageDisplayMode(PDFDocumentSession::FacingMode);
    auto *primaryView = workspace->findChild<QObject *>(
        QStringLiteral("primaryDocumentView"));
    QVERIFY(primaryView);
    auto *primaryList = primaryView->findChild<QObject *>(
        QStringLiteral("documentPageList"));
    QVERIFY(primaryList);

    const auto setSplitMode = [workspace](PDFDocumentSession::SplitViewMode mode) {
        return QMetaObject::invokeMethod(
            workspace, "setSplitViewMode", Qt::DirectConnection,
            Q_ARG(QVariant, static_cast<int>(mode)));
    };
    const auto setPosition = [](QObject *view, qreal x) {
        return QMetaObject::invokeMethod(
            view, "setViewportPosition", Qt::DirectConnection,
            Q_ARG(QVariant, x), Q_ARG(QVariant, 0.0));
    };
    const QVector<PDFDocumentSession::SplitViewMode> splitModes{
        PDFDocumentSession::VerticalSplit,
        PDFDocumentSession::HorizontalSplit,
    };
    for (const auto mode : splitModes) {
        QVERIFY(setSplitMode(mode));
        QObject *secondaryView = nullptr;
        QObject *secondaryList = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            ([&]() {
                auto *loader = workspace->findChild<QQuickItem *>(
                    QStringLiteral("secondaryDocumentViewLoader"));
                if (loader && loader->parentItem())
                    loader->parentItem()->ensurePolished();
                secondaryView = workspace->findChild<QObject *>(
                    QStringLiteral("secondaryDocumentView"));
                secondaryList = secondaryView
                    ? secondaryView->findChild<QObject *>(
                        QStringLiteral("documentPageList"))
                    : nullptr;
                return secondaryList
                    && secondaryList->property("contentWidth").toReal()
                        > secondaryList->property("width").toReal() + 160.0;
            })(),
            3000);
        QCoreApplication::processEvents();
        QTRY_COMPARE_WITH_TIMEOUT(
            primaryView->property("pendingNavigationPage").toInt(), -1, 2000);
        QVERIFY(setPosition(primaryView, 80.0));
        QVERIFY(setPosition(secondaryView, 160.0));
        QTRY_COMPARE_WITH_TIMEOUT(primaryList->property("contentX").toReal(), 80.0, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(secondaryList->property("contentX").toReal(), 160.0, 1000);

        QVERIFY(setSplitMode(PDFDocumentSession::NoSplit));
        QCOMPARE(session->secondaryViewState()->scrollX(), 160.0);
        QTRY_VERIFY_WITH_TIMEOUT(
            !workspace->findChild<QObject *>(QStringLiteral("secondaryDocumentView")),
            1000);
        QVERIFY(setSplitMode(mode));
        QTRY_VERIFY_WITH_TIMEOUT(
            ([&]() {
                secondaryView = workspace->findChild<QObject *>(
                    QStringLiteral("secondaryDocumentView"));
                return secondaryView
                    && qAbs(secondaryView->property("contentX").toReal() - 160.0) < 1.0;
            })(),
            3000);
        QCOMPARE(primaryList->property("contentX").toReal(), 80.0);
        QVERIFY(setSplitMode(PDFDocumentSession::NoSplit));
    }

    QVERIFY(mainWindow->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::controlsReaderStatusBarForActiveTab()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The reader status bar integration test needs a Qt Quick screen.");
    }

    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("reader-status-bar-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("reader-status-bar-second.pdf"));
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

    QObject *controlBar = nullptr;
    QObject *firstPageButton = nullptr;
    QObject *previousPageButton = nullptr;
    QObject *pageField = nullptr;
    QObject *pageCountLabel = nullptr;
    QObject *nextPageButton = nullptr;
    QObject *lastPageButton = nullptr;
    QObject *previousViewButton = nullptr;
    QObject *nextViewButton = nullptr;
    QObject *singlePageButton = nullptr;
    QObject *continuousPageButton = nullptr;
    QObject *facingPageButton = nullptr;
    QObject *continuousFacingPageButton = nullptr;
    QObject *zoomOutButton = nullptr;
    QObject *zoomSlider = nullptr;
    QObject *zoomInButton = nullptr;
    QObject *zoomComboBox = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            controlBar = mainWindow->findChild<QObject *>(
                QStringLiteral("readerControlBar"));
            firstPageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerFirstPageButton"));
            previousPageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerPreviousPageButton"));
            pageField = mainWindow->findChild<QObject *>(
                QStringLiteral("readerPageField"));
            pageCountLabel = mainWindow->findChild<QObject *>(
                QStringLiteral("readerPageCountLabel"));
            nextPageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerNextPageButton"));
            lastPageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerLastPageButton"));
            previousViewButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerPreviousViewButton"));
            nextViewButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerNextViewButton"));
            singlePageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerSinglePageButton"));
            continuousPageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerContinuousPageButton"));
            facingPageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerFacingPageButton"));
            continuousFacingPageButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerContinuousFacingPageButton"));
            zoomOutButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerZoomOutButton"));
            zoomSlider = mainWindow->findChild<QObject *>(
                QStringLiteral("readerZoomSlider"));
            zoomInButton = mainWindow->findChild<QObject *>(
                QStringLiteral("readerZoomInButton"));
            zoomComboBox = mainWindow->findChild<QObject *>(
                QStringLiteral("readerZoomComboBox"));
            return controlBar
                && firstPageButton
                && previousPageButton
                && pageField
                && pageCountLabel
                && nextPageButton
                && lastPageButton
                && previousViewButton
                && nextViewButton
                && singlePageButton
                && continuousPageButton
                && facingPageButton
                && continuousFacingPageButton
                && zoomOutButton
                && zoomSlider
                && zoomInButton
                && zoomComboBox;
        })(),
        3000);

    const auto click = [](QObject *item) {
        return QMetaObject::invokeMethod(
            item,
            "clicked",
            Qt::DirectConnection);
    };

    QCOMPARE(controlBar->property("pageCount").toInt(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageField->property("text").toString(),
        QStringLiteral("1"),
        1000);
    QVERIFY(pageCountLabel->property("text").toString().contains(
        QStringLiteral("3")));
    QVERIFY(!firstPageButton->property("enabled").toBool());
    QVERIFY(!previousPageButton->property("enabled").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(
        controlBar->property("viewReady").toBool(),
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        nextPageButton->property("enabled").toBool(),
        1000);
    QVERIFY(lastPageButton->property("enabled").toBool());
    QVERIFY(!previousViewButton->property("enabled").toBool());
    QVERIFY(!nextViewButton->property("enabled").toBool());
    QVERIFY(singlePageButton->property("checkable").toBool());
    QVERIFY(continuousPageButton->property("checkable").toBool());
    QVERIFY(facingPageButton->property("checkable").toBool());
    QVERIFY(continuousFacingPageButton->property("checkable").toBool());
    QVERIFY(singlePageButton->property("autoExclusive").toBool());
    QVERIFY(continuousPageButton->property("autoExclusive").toBool());
    QVERIFY(facingPageButton->property("autoExclusive").toBool());
    QVERIFY(continuousFacingPageButton->property("autoExclusive").toBool());
    QVERIFY(continuousPageButton->property("selected").toBool());
    QVERIFY(continuousPageButton->property("checked").toBool());

    QVERIFY(click(nextPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageField->property("text").toString(),
        QStringLiteral("2"),
        1000);
    QVERIFY(firstPageButton->property("enabled").toBool());
    QVERIFY(previousPageButton->property("enabled").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(
        previousViewButton->property("enabled").toBool(),
        1000);
    QVERIFY(!nextViewButton->property("enabled").toBool());

    QVERIFY(click(previousViewButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        nextViewButton->property("enabled").toBool(),
        1000);
    QVERIFY(click(nextViewButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        previousViewButton->property("enabled").toBool(),
        1000);
    QVERIFY(!nextViewButton->property("enabled").toBool());

    firstSession->document()->setCurrentPage(2);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(
        !controlBar->property("_observationPending").toBool(),
        1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageField->property("text").toString(),
        QStringLiteral("3"),
        1000);
    QVERIFY(click(previousViewButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        nextViewButton->property("enabled").toBool(),
        1000);

    QVERIFY(click(lastPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageField->property("text").toString(),
        QStringLiteral("3"),
        1000);
    QVERIFY(!nextPageButton->property("enabled").toBool());
    QVERIFY(!lastPageButton->property("enabled").toBool());

    QVERIFY(click(firstPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 0, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageField->property("text").toString(),
        QStringLiteral("1"),
        1000);
    QVERIFY(!firstPageButton->property("enabled").toBool());
    QVERIFY(!previousPageButton->property("enabled").toBool());

    QVERIFY(pageField->setProperty("text", QStringLiteral("2")));
    QVERIFY(QMetaObject::invokeMethod(
        pageField,
        "accepted",
        Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageField->property("text").toString(),
        QStringLiteral("2"),
        1000);
    QVERIFY(click(firstPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 0, 1000);

    QVERIFY(click(lastPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 2, 1000);

    QVERIFY(click(singlePageButton));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->pageDisplayMode(),
        PDFDocumentSession::SinglePageMode,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        singlePageButton->property("selected").toBool(),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        singlePageButton->property("checked").toBool(),
        1000);

    QVERIFY(click(continuousFacingPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->pageDisplayMode(),
        PDFDocumentSession::ContinuousFacingMode,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        continuousFacingPageButton->property("selected").toBool(),
        1000);

    QVERIFY(click(facingPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->pageDisplayMode(),
        PDFDocumentSession::FacingMode,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        facingPageButton->property("selected").toBool(),
        1000);

    const qreal initialZoom = firstSession->zoom();
    const qreal initialSliderValue = zoomSlider->property("value").toReal();
    const QString initialZoomText
        = zoomComboBox->property("displayText").toString();
    QVERIFY(!initialZoomText.isEmpty());
    QVERIFY(click(zoomInButton));
    QTRY_VERIFY_WITH_TIMEOUT(firstSession->zoom() > initialZoom, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        zoomSlider->property("value").toReal() > initialSliderValue,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        zoomComboBox->property("displayText").toString() != initialZoomText,
        1000);

    const qreal increasedZoom = firstSession->zoom();
    QVERIFY(click(zoomOutButton));
    QTRY_VERIFY_WITH_TIMEOUT(firstSession->zoom() < increasedZoom, 1000);
    QVERIFY(click(zoomInButton));
    QTRY_VERIFY_WITH_TIMEOUT(firstSession->zoom() > initialZoom, 1000);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(
        !controlBar->property("_observationPending").toBool(),
        1000);
    const qreal firstZoom = firstSession->zoom();
    const qreal firstSliderValue = zoomSlider->property("value").toReal();
    const QString firstZoomText
        = zoomComboBox->property("displayText").toString();

    const qreal externalZoom = firstZoom > 4.5
        ? firstZoom - 0.37
        : firstZoom + 0.37;
    firstSession->setZoom(externalZoom);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(
        !controlBar->property("_observationPending").toBool(),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(firstSession->zoom() - externalZoom) < 0.0001,
        1000);
    QVERIFY(click(previousViewButton));
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(firstSession->zoom() - firstZoom) < 0.0001,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        nextViewButton->property("enabled").toBool(),
        1000);
    QVERIFY(click(nextViewButton));
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(firstSession->zoom() - externalZoom) < 0.0001,
        1000);
    QVERIFY(click(previousViewButton));
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(firstSession->zoom() - firstZoom) < 0.0001,
        1000);

    PDFDocumentSession *secondSession = tabModel->open(
        QUrl::fromLocalFile(secondPath));
    QVERIFY(secondSession);
    QVERIFY(secondSession != firstSession);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageField->property("text").toString(),
        QStringLiteral("1"),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        continuousPageButton->property("selected").toBool(),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        nextPageButton->property("enabled").toBool(),
        3000);

    QVERIFY(click(nextPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(secondSession->document()->currentPage(), 1, 1000);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(
        !controlBar->property("_observationPending").toBool(),
        1000);
    QVERIFY(click(singlePageButton));
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->pageDisplayMode(),
        PDFDocumentSession::SinglePageMode,
        1000);
    const qreal secondInitialZoom = secondSession->zoom();
    QVERIFY(click(zoomOutButton));
    QTRY_VERIFY_WITH_TIMEOUT(secondSession->zoom() < secondInitialZoom, 1000);
    const qreal secondZoom = secondSession->zoom();
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(
        !controlBar->property("_observationPending").toBool(),
        1000);

    tabModel->setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(firstSession->document()->currentPage(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageField->property("text").toString(),
        QStringLiteral("3"),
        1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->pageDisplayMode(),
        PDFDocumentSession::FacingMode,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        facingPageButton->property("selected").toBool(),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(firstSession->zoom() - firstZoom) < 0.0001,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(zoomSlider->property("value").toReal() - firstSliderValue)
            < 0.0001,
        1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        zoomComboBox->property("displayText").toString(),
        firstZoomText,
        1000);

    tabModel->setCurrentIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(secondSession->document()->currentPage(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        pageField->property("text").toString(),
        QStringLiteral("2"),
        1000);
    QTRY_COMPARE_WITH_TIMEOUT(
        secondSession->pageDisplayMode(),
        PDFDocumentSession::SinglePageMode,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        singlePageButton->property("selected").toBool(),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(secondSession->zoom() - secondZoom) < 0.0001,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        previousViewButton->property("enabled").toBool(),
        1000);
    QVERIFY(click(previousViewButton));
    QTRY_COMPARE_WITH_TIMEOUT(secondSession->document()->currentPage(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(secondSession->zoom() - secondInitialZoom) < 0.0001,
        1000);
    QVERIFY(click(previousViewButton));
    QTRY_COMPARE_WITH_TIMEOUT(secondSession->document()->currentPage(), 0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        nextViewButton->property("enabled").toBool(),
        1000);
    QVERIFY(click(nextViewButton));
    QTRY_COMPARE_WITH_TIMEOUT(secondSession->document()->currentPage(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(secondSession->zoom() - secondInitialZoom) < 0.0001,
        1000);
    QVERIFY(click(nextViewButton));
    QTRY_VERIFY_WITH_TIMEOUT(
        qAbs(secondSession->zoom() - secondZoom) < 0.0001,
        1000);

    tabModel->setCurrentIndex(0);
    QObject *splitItem = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            splitItem = mainWindow->findChild<QObject *>(
                QStringLiteral("splitPageDisplayItem"));
            return splitItem != nullptr;
        })(),
        1000);
    QVERIFY(click(splitItem));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->splitViewMode(),
        PDFDocumentSession::VerticalSplit,
        1000);

    QObject *secondaryView = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            secondaryView = mainWindow->findChild<QObject *>(
                QStringLiteral("secondaryDocumentView"));
            return secondaryView != nullptr;
        })(),
        3000);
    QVERIFY(QMetaObject::invokeMethod(
        secondaryView,
        "activated",
        Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(
        controlBar->property("navigationPage").toInt(),
        2,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        firstPageButton->property("enabled").toBool(),
        1000);

    QVERIFY(click(firstPageButton));
    QTRY_COMPARE_WITH_TIMEOUT(
        firstSession->secondaryViewState()->currentPage(),
        0,
        1000);
    QCOMPARE(firstSession->document()->currentPage(), 2);

    const qreal primaryZoomBefore = firstSession->zoom();
    const qreal secondaryZoomBefore
        = firstSession->secondaryViewState()->zoom();
    QVERIFY(click(zoomOutButton));
    QTRY_VERIFY_WITH_TIMEOUT(
        firstSession->secondaryViewState()->zoom() < secondaryZoomBefore,
        1000);
    QCOMPARE(firstSession->zoom(), primaryZoomBefore);

    QVERIFY(tabModel->closeSession(firstSession));
    PDFDocumentSession *reopenedSession = tabModel->open(
        QUrl::fromLocalFile(firstPath));
    QVERIFY(reopenedSession);
    QVERIFY(reopenedSession != firstSession);
    QTRY_VERIFY_WITH_TIMEOUT(
        controlBar->property("viewReady").toBool(),
        3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        nextPageButton->property("enabled").toBool(),
        1000);
    QVERIFY(!previousViewButton->property("enabled").toBool());
    QVERIFY(!nextViewButton->property("enabled").toBool());

    QVERIFY(controlBar->setProperty("pageCount", 2000));
    const QString localizedPage = QLocale().toString(1000);
    QVERIFY(localizedPage != QStringLiteral("1000"));
    QVERIFY(pageField->setProperty("text", localizedPage));
    QVERIFY(pageField->property("acceptableInput").toBool());
    QSignalSpy pageRequestedSpy(controlBar, SIGNAL(pageRequested(int)));
    QVERIFY(pageRequestedSpy.isValid());
    QVERIFY(QMetaObject::invokeMethod(
        pageField,
        "accepted",
        Qt::DirectConnection));
    QCOMPARE(pageRequestedSpy.count(), 1);
    QCOMPARE(pageRequestedSpy.takeFirst().at(0).toInt(), 999);

    QVERIFY(mainWindow->setProperty("allowWindowClose", true));
}
