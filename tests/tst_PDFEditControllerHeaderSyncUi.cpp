#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QPointer>

using namespace PDFEditControllerTestSupport;

namespace {
QQuickItem *syncVisualItem(QQuickItem *parent, const QString &name)
{
    if (parent->objectName() == name && parent->isVisible())
        return parent;
    for (QQuickItem *child : parent->childItems()) {
        if (auto *item = syncVisualItem(child, name))
            return item;
    }
    return nullptr;
}

QQuickItem *syncEditOverlay(QQuickItem *parent)
{
    if (parent->isVisible() && parent->property("page").toInt() == 0
        && parent->metaObject()->indexOfMethod("beginSelectedEditing()") >= 0)
        return parent;
    for (QQuickItem *child : parent->childItems()) {
        if (auto *item = syncEditOverlay(child))
            return item;
    }
    return nullptr;
}

QQuickItem *syncTextEditor(QQuickItem *parent)
{
    if (parent->isVisible()
        && parent->metaObject()->indexOfProperty("textDocument") >= 0)
        return parent;
    for (QQuickItem *child : parent->childItems()) {
        if (auto *item = syncTextEditor(child))
            return item;
    }
    return nullptr;
}

QQuickItem *syncFontSizeBox(QQuickItem *parent)
{
    if (parent->isVisible()
        && parent->metaObject()->indexOfMethod("beginSizeEdit()") >= 0)
        return parent;
    for (QQuickItem *child : parent->childItems()) {
        if (auto *item = syncFontSizeBox(child))
            return item;
    }
    return nullptr;
}

QString syncUiText(const PDFEditController &controller, int page)
{
    const auto drafts = controller.draftsForPage(page);
    return drafts.isEmpty() ? QString()
        : drafts.first().toMap().value(QStringLiteral("text")).toString();
}
}

void PDFEditControllerTest::syncsHeadersThroughMainWindow()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*Binding loop.*")));
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-sync-ui.pdf"));
    QVERIFY(writePdfObjects(path, {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>"}));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QVERIFY(window->setProperty("visible", true));
    QVERIFY(window->setProperty("allowWindowClose", true));
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(quickWindow && model && tabs);
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    auto &controller = *session->editController();
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QVERIFY(controller.addHeaderText(QStringLiteral("Original"), QStringLiteral("all"), 24, 20, 180));
    const QString anchor = controller.draftsForPage(0).first().toMap()
        .value(QStringLiteral("objectId")).toString();
    QTRY_VERIFY(controller.headerSelectionOnly());
    QVERIFY(controller.selectCandidate(0, anchor));
    QTRY_VERIFY(quickWindow->isExposed());

    const auto item = [&](const char *name) {
        return syncVisualItem(quickWindow->contentItem(), QString::fromLatin1(name));
    };
    const auto click = [&](QQuickItem *target) {
        // Loading/collapsing the panel changes its layout on the next scene-graph frame.
        QSignalSpy frame(quickWindow, &QQuickWindow::frameSwapped);
        quickWindow->requestUpdate();
        if (!frame.wait(2000))
            return false;
        QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier,
            target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
        return true;
    };
    QQuickItem *enable = nullptr;
    QTRY_VERIFY((enable = item("headerSyncEnabled")));
    QTRY_VERIFY(enable->property("checked").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    QCOMPARE(controller.headerSyncMatchPages(), QStringLiteral("1-3"));
    // Opting out stays off when the properties panel is recreated.
    QVERIFY(click(enable));
    QTRY_VERIFY(!enable->property("checked").toBool());
    QCOMPARE(controller.headerSyncMatchCount(), 0);
    QVERIFY(!controller.headerSyncLoading());
    session->setPanelCollapsed(true);
    QTRY_VERIFY(!item("headerSyncEnabled"));
    session->setPanelCollapsed(false);
    QTRY_VERIFY((enable = item("headerSyncEnabled")));
    QVERIFY(!enable->property("checked").toBool());
    QVERIFY(click(enable));
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    auto *propertiesColumn = item("selectionPropertiesColumn");
    QVERIFY(propertiesColumn);
    QTRY_COMPARE(propertiesColumn->width(), 264.0);
    QVERIFY(propertiesColumn->mapToScene(QPointF()).x() > quickWindow->width() / 2);

    auto *customPages = item("headerSyncSelectedPages");
    auto *range = item("headerSyncPageRange");
    QVERIFY(customPages && range);
    QVERIFY(customPages->setProperty("checked", true));
    QVERIFY(range->setProperty("text", QStringLiteral("2")));
    // Collapsing must retain the match set and the user's restricted page range.
    QPointer<QObject> panel = window->findChild<QObject *>(QStringLiteral("headerSyncPanel"));
    QVERIFY(panel);
    session->setPanelCollapsed(true);
    QTRY_VERIFY(panel.isNull());
    QTRY_COMPARE(propertiesColumn->width(), 42.0);
    session->setPanelCollapsed(false);
    QTRY_VERIFY((enable = item("headerSyncEnabled")));
    QVERIFY(enable->property("checked").toBool());
    QTRY_COMPARE(propertiesColumn->width(), 264.0);
    customPages = item("headerSyncSelectedPages");
    range = item("headerSyncPageRange");
    QVERIFY(customPages && range);
    QVERIFY(customPages->property("checked").toBool());
    QCOMPARE(range->property("text").toString(), QStringLiteral("2"));

    QQuickItem *overlay = nullptr;
    QTRY_VERIFY((overlay = syncEditOverlay(quickWindow->contentItem())));
    QVERIFY(QMetaObject::invokeMethod(overlay, "beginSelectedEditing"));
    QTRY_VERIFY(controller.textEditActive());
    QQuickItem *editor = nullptr;
    QTRY_VERIFY((editor = syncTextEditor(overlay)));
    QVERIFY(editor->setProperty("text", QStringLiteral("Changed header")));
    customPages = item("headerSyncSelectedPages");
    range = item("headerSyncPageRange");
    auto *apply = item("headerSyncApplyButton");
    QVERIFY(customPages && range && apply);
    QVERIFY(customPages->setProperty("checked", true));
    QVERIFY(range->setProperty("text", QStringLiteral("2")));
    QTRY_VERIFY(apply->isEnabled());
    // Invoke the actual panel action while the inline editor still owns uncommitted text.
    QVERIFY(controller.textEditActive());
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(!controller.textEditActive());
    QCOMPARE(syncUiText(controller, 0), QStringLiteral("Changed header"));
    QCOMPARE(syncUiText(controller, 1), QStringLiteral("Changed header"));
    QCOMPARE(syncUiText(controller, 2), QStringLiteral("Original"));
    controller.undo();
    for (int page = 0; page < 3; ++page)
        QCOMPARE(syncUiText(controller, page), QStringLiteral("Original"));

    // Apply also commits a pending font-size field before batching the edit.
    QVERIFY(controller.selectCandidate(0, anchor));
    QTRY_VERIFY((enable = item("headerSyncEnabled")));
    QTRY_VERIFY(enable->property("checked").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    QQuickItem *fontSizeBox = nullptr;
    QTRY_VERIFY((fontSizeBox = syncFontSizeBox(quickWindow->contentItem())));
    const qreal originalSize = controller.selectedFontSize();
    QVERIFY(QMetaObject::invokeMethod(fontSizeBox, "beginSizeEdit"));
    QVERIFY(fontSizeBox->setProperty("editText", QStringLiteral("14")));
    auto *workspace = window->findChild<QObject *>(QStringLiteral("documentWorkspace"));
    QVERIFY(workspace);
    QVERIFY(workspace->property("hasPendingPanelEdits").toBool());
    QTRY_VERIFY((apply = item("headerSyncApplyButton")) && apply->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(!workspace->property("hasPendingPanelEdits").toBool());
    QCOMPARE(controller.selectedFontSize(), 14.0);
    for (int page = 0; page < 3; ++page) {
        const auto draft = controller.draftsForPage(page).first().toMap();
        const auto spans = draft.value(QStringLiteral("characterSpans")).toList();
        QVERIFY(!spans.isEmpty());
        QCOMPARE(spans.first().toMap().value(QStringLiteral("fontSizeRatio")).toReal() * 400, 14.0);
    }
    controller.undo();
    QCOMPARE(controller.selectedFontSize(), originalSize);

    // The synchronization panel survives deleting its anchor and still applies/undoes as a group.
    QVERIFY(controller.selectCandidate(0, anchor));
    QTRY_VERIFY((enable = item("headerSyncEnabled")));
    QTRY_VERIFY(enable->property("checked").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    controller.deleteSelectedObject();
    QVERIFY(controller.selectedObjectId().isEmpty());
    QTRY_VERIFY((apply = item("headerSyncApplyButton")) && apply->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    for (int page = 0; page < 3; ++page)
        QVERIFY(controller.draftsForPage(page).isEmpty());
    controller.undo();
    for (int page = 0; page < 3; ++page)
        QCOMPARE(syncUiText(controller, page), QStringLiteral("Original"));
    QVERIFY(controller.selectCandidate(0, anchor));
    QTRY_VERIFY((enable = item("headerSyncEnabled")));
    QTRY_VERIFY(enable->property("checked").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    controller.discardDrafts();
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QTRY_VERIFY(!controller.headerSyncAvailable());
    QCOMPARE(controller.headerSyncMatchCount(), 0);
    QTRY_VERIFY(!item("headerSyncPanel"));
}

void PDFEditControllerTest::guardsHeaderEntryUntilChangesAreSavedOrUndone()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*Binding loop.*")));
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-entry-guard.pdf"));
    const QString otherPath = m_temporaryDirectory.filePath(QStringLiteral("header-entry-other.pdf"));
    QVERIFY(createFontStylePdf(path));
    QVERIFY(createFontStylePdf(otherPath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    window->setProperty("visible", true);
    window->setProperty("allowWindowClose", true);
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    auto *ribbon = window->findChild<QObject *>(QStringLiteral("appRibbon"));
    auto *noticeLoader = window->findChild<QObject *>(QStringLiteral("headerEntryNoticeLoader"));
    QVERIFY(quickWindow && model && tabs && ribbon && noticeLoader);
    QTRY_VERIFY(quickWindow->isExposed());
    const auto item = [&](const char *name) {
        return syncVisualItem(quickWindow->contentItem(), QString::fromLatin1(name));
    };
    const auto click = [&](QQuickItem *target) {
        if (!target)
            return false;
        QSignalSpy frame(quickWindow, &QQuickWindow::frameSwapped);
        quickWindow->requestUpdate();
        if (!frame.wait(2000))
            return false;
        QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier,
            target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint());
        return true;
    };
    const auto dismissNotice = [&]() {
        auto *button = item("headerEntryUnderstoodButton");
        return button && QMetaObject::invokeMethod(button, "clicked");
    };

    // No document and a clean document both allow Header entry.
    QVERIFY(click(item("headerTabButton")));
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(!noticeLoader->property("active").toBool());
    QVERIFY(item("headerSaveButton") && !item("headerSaveButton")->isEnabled());
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    auto &controller = *session->editController();
    QTRY_VERIFY(controller.headerSelectionOnly());
    QVERIFY(tabs->setProperty("currentIndex", 1));
    const auto candidate = candidateForText(waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QQuickItem *overlay = nullptr;
    QTRY_VERIFY((overlay = syncEditOverlay(quickWindow->contentItem())));
    QVERIFY(QMetaObject::invokeMethod(overlay, "beginSelectedEditing"));
    QTRY_VERIFY(controller.textEditActive());
    QVERIFY(click(item("headerTabButton")));
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!noticeLoader->property("active").toBool());

    // Pending inline input must be committed before checking; rejected entry never
    // activates Header selection or its guides, and Undo retains the original text.
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QTRY_VERIFY((overlay = syncEditOverlay(quickWindow->contentItem())));
    QVERIFY(QMetaObject::invokeMethod(overlay, "beginSelectedEditing"));
    QTRY_VERIFY(controller.textEditActive());
    QQuickItem *editor = nullptr;
    QTRY_VERIFY((editor = syncTextEditor(overlay)));
    QVERIFY(editor->setProperty("text", QStringLiteral("Pending inline change")));
    QSignalSpy headerActivated(ribbon, SIGNAL(headerTabActiveChanged()));
    QVERIFY(click(item("headerTabButton")));
    QTRY_VERIFY(noticeLoader->property("active").toBool());
    QCOMPARE(tabs->property("currentIndex").toInt(), 1);
    QCOMPARE(headerActivated.count(), 0);
    QVERIFY(!item("headerTabButton")->property("checked").toBool());
    QVERIFY(!ribbon->property("headerTabActive").toBool());
    QVERIFY(!controller.headerSelectionOnly());
    QVERIFY(!controller.textEditActive());
    QCOMPARE(syncUiText(controller, 0), QStringLiteral("Pending inline change"));
    QVERIFY(dismissNotice());
    QTRY_VERIFY(!noticeLoader->property("active").toBool());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(click(item("headerTabButton")));
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(controller.headerSelectionOnly());

    // Editing inside Header remains possible; leaving requires Save or Undo.
    QVERIFY(controller.addHeaderText(QStringLiteral("Saved header"), QStringLiteral("all"), 24, 20, 180));
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(!noticeLoader->property("active").toBool());
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QTRY_VERIFY(noticeLoader->property("active").toBool());
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(noticeLoader->property("leavingHeader").toBool());
    auto *save = item("headerEntrySaveButton");
    QVERIFY(save);
    QSignalSpy saved(model, &PDFTabModel::sessionSaveFinished);
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.first().at(1).toBool());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!noticeLoader->property("active").toBool());
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QCOMPARE(tabs->property("currentIndex").toInt(), 1);
    QVERIFY(click(item("headerTabButton")));
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);

    // Non-content drafts participate too: forms and pending password changes.
    QVERIFY(tabs->setProperty("currentIndex", 3));
    QVERIFY(!session->formDesignerController()->addTextField(0, 0.2, 0.5, 0.3, 0.1).isEmpty());
    QVERIFY(click(item("headerTabButton")));
    QCOMPARE(tabs->property("currentIndex").toInt(), 3);
    QVERIFY(noticeLoader->property("active").toBool());
    QVERIFY(dismissNotice());
    QTRY_VERIFY(!noticeLoader->property("active").toBool());
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(tabs->setProperty("currentIndex", 5));
    QVERIFY(controller.setOpenPassword(QStringLiteral("test-only-password")));
    QVERIFY(click(item("headerTabButton")));
    QCOMPARE(tabs->property("currentIndex").toInt(), 5);
    QVERIFY(noticeLoader->property("active").toBool());
    QVERIFY(dismissNotice());
    QTRY_VERIFY(!noticeLoader->property("active").toBool());

    // A dirty background document does not block a clean active document. Switching
    // back to the dirty document while Header is active must not bypass the guard.
    auto *other = model->open(QUrl::fromLocalFile(otherPath));
    QVERIFY(other && other != session);
    QVERIFY(click(item("headerTabButton")));
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(!noticeLoader->property("active").toBool());
    model->setCurrentIndex(model->indexOf(session));
    QTRY_COMPARE(tabs->property("currentIndex").toInt(), 0);
    QTRY_VERIFY(noticeLoader->property("active").toBool());
    QVERIFY(!controller.headerSelectionOnly());
    QVERIFY(controller.passwordChangePending());
    QVERIFY(dismissNotice());
    controller.discardDrafts();
}

void PDFEditControllerTest::guardsHeaderExitUntilChangesAreSavedOrUndone()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*Binding loop.*")));
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-exit-guard.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    {
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(path)));
        PDFEditController controller;
        controller.setDocument(&document);
        QVERIFY(controller.addHeaderText(QStringLiteral("Saved header"), QStringLiteral("all"), 24, 20, 150));
        QVERIFY(controller.save());
    }
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    window->setProperty("visible", true);
    window->setProperty("allowWindowClose", true);
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    auto *ribbon = window->findChild<QObject *>(QStringLiteral("appRibbon"));
    auto *noticeLoader = window->findChild<QObject *>(QStringLiteral("headerEntryNoticeLoader"));
    QVERIFY(quickWindow && model && tabs && ribbon && noticeLoader);
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    auto &controller = *session->editController();
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_VERIFY(controller.headerSelectionOnly());
    QTRY_VERIFY(quickWindow->isExposed());
    const auto candidate = candidateForText(waitForCandidates(controller, 0), QStringLiteral("Saved header"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QQuickItem *overlay = nullptr;
    QTRY_VERIFY((overlay = syncEditOverlay(quickWindow->contentItem())));
    QVERIFY(QMetaObject::invokeMethod(overlay, "beginSelectedEditing"));
    QTRY_VERIFY(controller.textEditActive());
    QQuickItem *editor = nullptr;
    QTRY_VERIFY((editor = syncTextEditor(overlay)));
    QVERIFY(editor->setProperty("text", QStringLiteral("Pending header edit")));
    QSignalSpy headerActivated(ribbon, SIGNAL(headerTabActiveChanged()));
    const auto item = [&](const char *name) {
        return syncVisualItem(quickWindow->contentItem(), QString::fromLatin1(name));
    };
    const auto dismissNotice = [&]() {
        auto *button = item("headerEntryUnderstoodButton");
        return button && QMetaObject::invokeMethod(button, "clicked");
    };
    // Every ribbon tab uses the same guard, including Footer and programmatic changes.
    for (int index = 0; index < 9; ++index) {
        if (index == 7)
            continue;
        QVERIFY(tabs->setProperty("currentIndex", index));
        QTRY_VERIFY(noticeLoader->property("active").toBool());
        QCOMPARE(tabs->property("currentIndex").toInt(), 7);
        QVERIFY(noticeLoader->property("leavingHeader").toBool());
        QVERIFY(ribbon->property("headerTabActive").toBool());
        QVERIFY(item("headerTabButton")->property("checked").toBool());
        QVERIFY(controller.headerSelectionOnly());
        QCOMPARE(controller.tool(), PDFEditController::SelectTool);
        QCOMPARE(syncUiText(controller, 0), QStringLiteral("Pending header edit"));
        QVERIFY(dismissNotice());
        QTRY_VERIFY(!noticeLoader->property("active").toBool());
    }
    QCOMPARE(headerActivated.count(), 0);
    QVERIFY(!controller.textEditActive());
    // Find is the command used by Cmd/Ctrl+F and must not bypass the guard.
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestFind"));
    QTRY_VERIFY(noticeLoader->property("active").toBool());
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(!ribbon->property("searchFocusPending").toBool());
    QVERIFY(dismissNotice());
    QTRY_VERIFY(!noticeLoader->property("active").toBool());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QCOMPARE(tabs->property("currentIndex").toInt(), 1);
    QVERIFY(!controller.headerSelectionOnly());
    QVERIFY(!noticeLoader->property("active").toBool());
}

void PDFEditControllerTest::savesPendingHeaderPlacementBeforeLeaving()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*Binding loop.*")));
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-exit-placement.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    window->setProperty("visible", true);
    window->setProperty("allowWindowClose", true);
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    auto *ribbon = window->findChild<QObject *>(QStringLiteral("appRibbon"));
    auto *noticeLoader = window->findChild<QObject *>(QStringLiteral("headerEntryNoticeLoader"));
    QVERIFY(quickWindow && model && tabs && ribbon && noticeLoader);
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    auto &controller = *session->editController();
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_VERIFY(quickWindow->isExposed());
    const auto item = [&](const char *name) {
        return syncVisualItem(quickWindow->contentItem(), QString::fromLatin1(name));
    };
    auto *placement = window->findChild<QObject *>(QStringLiteral("headerTextPlacement"));
    QVERIFY(placement);
    QQuickItem *headerSave = nullptr;
    QTRY_VERIFY((headerSave = item("headerSaveButton")));
    QVERIFY(!headerSave->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(placement, "start"));
    QVERIFY(QMetaObject::invokeMethod(placement, "place", Q_ARG(QVariant, 0),
        Q_ARG(QVariant, 24), Q_ARG(QVariant, 20)));
    QQuickItem *text = nullptr;
    QTRY_VERIFY((text = item("headerInlineTextInput")));
    QVERIFY(text->setProperty("text", QStringLiteral("Only page two")));
    QTRY_VERIFY(headerSave->isEnabled());
    auto *custom = item("headerCustomPagesSelector");
    auto *range = item("headerPageRangeInput");
    QVERIFY(custom && range);
    QVERIFY(custom->setProperty("checked", true));
    QVERIFY(range->setProperty("text", QStringLiteral("99")));
    QVERIFY(!controller.hasDrafts());
    QPointer<QObject> panel = window->findChild<QObject *>(QStringLiteral("headerTextPanel"));
    QVERIFY(panel);
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QTRY_VERIFY(noticeLoader->property("active").toBool());
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(placement->property("active").toBool());
    QCOMPARE(placement->property("text").toString(), QStringLiteral("Only page two"));
    QVERIFY(panel);
    QCOMPARE(range->property("text").toString(), QStringLiteral("99"));
    auto *save = item("headerEntrySaveButton");
    QVERIFY(save);
    QSignalSpy saved(model, &PDFTabModel::sessionSaveFinished);
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_VERIFY(!noticeLoader->property("active").toBool());
    // Invalid settings retain the pending content and do not start saving.
    QVERIFY(placement->property("active").toBool());
    QVERIFY(!placement->property("errorText").toString().isEmpty());
    QCOMPARE(placement->property("text").toString(), QStringLiteral("Only page two"));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.saving());
    QCOMPARE(saved.count(), 0);
    QVERIFY(range->setProperty("text", QStringLiteral("2")));
    // The Find shortcut must retain unapplied placement too.
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestFind"));
    QTRY_VERIFY(noticeLoader->property("active").toBool());
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    QVERIFY(!ribbon->property("searchFocusPending").toBool());
    QVERIFY(placement->property("active").toBool());
    QCOMPARE(placement->property("text").toString(), QStringLiteral("Only page two"));
    auto *understood = item("headerEntryUnderstoodButton");
    QVERIFY(understood && QMetaObject::invokeMethod(understood, "clicked"));
    QTRY_VERIFY(!noticeLoader->property("active").toBool());
    QVERIFY(headerSave->isEnabled());
    QSignalSpy frame(quickWindow, &QQuickWindow::frameSwapped);
    quickWindow->requestUpdate();
    QVERIFY(frame.wait(2000));
    QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier,
        headerSave->mapToScene(QPointF(headerSave->width() / 2, headerSave->height() / 2)).toPoint());
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.first().at(1).toBool());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!placement->property("active").toBool());
    QTRY_VERIFY(!headerSave->isEnabled());
    QTRY_VERIFY(panel.isNull());
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);
    for (int page = 0; page < 3; ++page) {
        const auto match = candidateForText(waitForCandidates(controller, page), QStringLiteral("Only page two"));
        QCOMPARE(!match.isEmpty(), page == 1);
    }
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QCOMPARE(tabs->property("currentIndex").toInt(), 1);
    QVERIFY(!noticeLoader->property("active").toBool());
}
