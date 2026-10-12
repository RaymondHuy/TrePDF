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
    // The workspace and page delegates load asynchronously. DocumentView also
    // has beginSelectedEditing(), but no page property or live editor yet.
    if (parent->isVisible() && parent->property("page").isValid()
        && parent->property("page").toInt() == 0
        && parent->metaObject()->indexOfProperty("editingObjectId") >= 0
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
    {
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(path)));
        PDFEditController controller;
        controller.setDocument(&document);
        QVERIFY(controller.addHeaderText(QStringLiteral("Original"), QStringLiteral("all"), 24, 20, 180));
        QVERIFY(controller.save());
    }
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
    QTRY_VERIFY(controller.headerSelectionOnly());
    const auto savedCandidate = [&](const QString &text) {
        return candidateForText(waitForCandidates(controller, 0), text)
            .value(QStringLiteral("objectId")).toString();
    };
    QString anchor = savedCandidate(QStringLiteral("Original"));
    QVERIFY(!anchor.isEmpty());
    QVERIFY(controller.selectCandidate(0, anchor));
    QTRY_VERIFY(quickWindow->isExposed());
    QObject *workspace = nullptr;
    QTRY_VERIFY((workspace = window->property("activeWorkspaceItem").value<QObject *>()));
    QTRY_VERIFY(workspace->property("initialViewReady").toBool());

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
    QVERIFY(!item("headerSyncApplyButton"));
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
    auto *save = item("headerSaveButton");
    QVERIFY(customPages && range && save);
    QVERIFY(customPages->setProperty("checked", true));
    QTRY_VERIFY(save->isEnabled());
    QSignalSpy saved(model, &PDFTabModel::sessionSaveFinished);
    const auto fileBytes = [&]() {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const QByteArray originalBytes = fileBytes();
    QVERIFY(!originalBytes.isEmpty());

    // Empty or invalid custom ranges must keep the pending edit and never write
    // the source file. In particular, empty custom input must not mean all pages.
    QVERIFY(controller.textEditActive());
    for (const QString &invalidRange : {QString(), QStringLiteral("4")}) {
        QVERIFY(range->setProperty("text", invalidRange));
        QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
        QTest::qWait(150);
        QCOMPARE(saved.count(), 0);
        QVERIFY(!controller.saving());
        QCOMPARE(fileBytes(), originalBytes);
        QCOMPARE(range->property("text").toString(), invalidRange);
        QVERIFY(controller.hasDrafts() || controller.textEditActive());
    }
    QVERIFY(range->setProperty("text", QStringLiteral("2")));
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.first().at(1).toBool());
    QVERIFY(!controller.textEditActive());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(normalizedPageText(path, 0), QStringLiteral("Changed header"));
    QCOMPARE(normalizedPageText(path, 1), QStringLiteral("Changed header"));
    QCOMPARE(normalizedPageText(path, 2), QStringLiteral("Original"));
    QCOMPARE(tabs->property("currentIndex").toInt(), 7);

    // Save commits a pending font-size field and applies it to all matching pages.
    anchor = savedCandidate(QStringLiteral("Changed header"));
    QVERIFY(!anchor.isEmpty());
    QVERIFY(controller.selectCandidate(0, anchor));
    QTRY_VERIFY((enable = item("headerSyncEnabled")));
    QTRY_VERIFY(enable->property("checked").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    auto *allPages = item("headerSyncAllPages");
    QVERIFY(allPages);
    QVERIFY(allPages->setProperty("checked", true));
    QQuickItem *fontSizeBox = nullptr;
    QTRY_VERIFY((fontSizeBox = syncFontSizeBox(quickWindow->contentItem())));
    QVERIFY(QMetaObject::invokeMethod(fontSizeBox, "beginSizeEdit"));
    QVERIFY(fontSizeBox->setProperty("editText", QStringLiteral("14")));
    QVERIFY(workspace->property("hasPendingPanelEdits").toBool());
    QTRY_VERIFY((save = item("headerSaveButton")) && save->isEnabled());
    saved.clear();
    // A save requested during the asynchronous match scan must wait and retain
    // the pending property input until that match set can be applied.
    controller.findSimilarHeaders();
    QVERIFY(controller.headerSyncLoading());
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.first().at(1).toBool());
    QTRY_VERIFY(!workspace->property("hasPendingPanelEdits").toBool());
    QVERIFY(!controller.hasDrafts());
    {
        PDFDocument reopened;
        QVERIFY(reopened.open(QUrl::fromLocalFile(path)));
        PDFEditController reopenedController;
        reopenedController.setDocument(&reopened);
        reopenedController.setTool(PDFEditController::SelectTool);
        for (int page = 0; page < 3; ++page) {
            QCOMPARE(normalizedPageText(path, page), QStringLiteral("Changed header"));
            const auto candidate = candidateForText(waitForCandidates(reopenedController, page),
                QStringLiteral("Changed header"));
            QVERIFY(!candidate.isEmpty());
            QVERIFY(reopenedController.selectCandidate(page,
                candidate.value(QStringLiteral("objectId")).toString()));
            QVERIFY(qAbs(reopenedController.selectedFontSize() - 14.0) < 0.01);
        }
    }

    // Clicking blank space after moving a header must retain the pending
    // synchronization snapshot for Save, even though no object remains selected.
    anchor = savedCandidate(QStringLiteral("Changed header"));
    QVERIFY(!anchor.isEmpty());
    QVERIFY(controller.selectCandidate(0, anchor));
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    allPages = item("headerSyncAllPages");
    QVERIFY(allPages);
    QVERIFY(allPages->setProperty("checked", true));
    const QRectF movedFrame = controller.selectedFrameRect().translated(12.0 / 300, 3.0 / 400);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();
    controller.clearSelection();
    QVERIFY(controller.selectedObjectId().isEmpty());
    QVERIFY(controller.headerSyncCanApply());
    QTRY_VERIFY((save = item("headerSaveButton")) && save->isEnabled());
    saved.clear();
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.first().at(1).toBool());
    QVERIFY(!controller.hasDrafts());
    for (int page = 0; page < 3; ++page) {
        const auto candidate = candidateForText(waitForCandidates(controller, page),
            QStringLiteral("Changed header"));
        QVERIFY(!candidate.isEmpty());
        const QRectF frame = candidate.value(QStringLiteral("frameRect")).toRectF();
        QVERIFY(qAbs(frame.x() - movedFrame.x()) * 300 < 0.1);
        QVERIFY(qAbs(frame.y() - movedFrame.y()) * 400 < 0.1);
    }

    // Opting out saves only the local inline edit, even with a custom range set.
    anchor = savedCandidate(QStringLiteral("Changed header"));
    QVERIFY(!anchor.isEmpty());
    QVERIFY(controller.selectCandidate(0, anchor));
    QTRY_VERIFY((enable = item("headerSyncEnabled")));
    QTRY_VERIFY(enable->property("checked").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    customPages = item("headerSyncSelectedPages");
    range = item("headerSyncPageRange");
    QVERIFY(customPages && range);
    QVERIFY(customPages->setProperty("checked", true));
    QVERIFY(range->setProperty("text", QString()));
    QVERIFY(click(enable));
    QTRY_VERIFY(!enable->property("checked").toBool());
    QTRY_VERIFY((overlay = syncEditOverlay(quickWindow->contentItem())));
    QVERIFY(QMetaObject::invokeMethod(overlay, "beginSelectedEditing"));
    QTRY_VERIFY(controller.textEditActive());
    QTRY_VERIFY((editor = syncTextEditor(overlay)));
    QVERIFY(editor->setProperty("text", QStringLiteral("Local header")));
    QTRY_VERIFY((save = item("headerSaveButton")) && save->isEnabled());
    saved.clear();
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.first().at(1).toBool());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(normalizedPageText(path, 0), QStringLiteral("Local header"));
    QCOMPARE(normalizedPageText(path, 1), QStringLiteral("Changed header"));
    QCOMPARE(normalizedPageText(path, 2), QStringLiteral("Changed header"));

    // Save retains the synchronization snapshot after deleting its selected anchor.
    anchor = savedCandidate(QStringLiteral("Local header"));
    QVERIFY(!anchor.isEmpty());
    QVERIFY(controller.selectCandidate(0, anchor));
    QTRY_VERIFY((enable = item("headerSyncEnabled")));
    QTRY_VERIFY(enable->property("checked").toBool());
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    allPages = item("headerSyncAllPages");
    QVERIFY(allPages);
    QVERIFY(allPages->setProperty("checked", true));
    controller.deleteSelectedObject();
    QVERIFY(controller.selectedObjectId().isEmpty());
    QTRY_VERIFY(item("headerSyncPanel"));
    QTRY_VERIFY((save = item("headerSaveButton")) && save->isEnabled());
    saved.clear();
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.first().at(1).toBool());
    QVERIFY(!controller.hasDrafts());
    for (int page = 0; page < 3; ++page)
        QVERIFY(normalizedPageText(path, page).isEmpty());
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
    // Child items can exist while the asynchronous workspace is still
    // incubating. Only the ready workspace participates in Main's edit flush.
    QObject *workspace = nullptr;
    QTRY_VERIFY((workspace = window->property("activeWorkspaceItem").value<QObject *>()));
    QTRY_VERIFY(workspace->property("initialViewReady").toBool());
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
        QTRY_VERIFY2(noticeLoader->property("active").toBool(),
            qPrintable(QStringLiteral("tab %1, current %2, selected %3, editing %4, drafts %5, text %6")
                .arg(index).arg(tabs->property("currentIndex").toInt())
                .arg(controller.selectedObjectId()).arg(controller.textEditActive())
                .arg(controller.hasDrafts()).arg(controller.selectedText())));
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
    QObject *workspace = nullptr;
    QTRY_VERIFY((workspace = window->property("activeWorkspaceItem").value<QObject *>()));
    QTRY_VERIFY(workspace->property("initialViewReady").toBool());
    const auto item = [&](const char *name) {
        return syncVisualItem(quickWindow->contentItem(), QString::fromLatin1(name));
    };
    QObject *placement = nullptr;
    QTRY_VERIFY((placement = window->findChild<QObject *>(QStringLiteral("headerTextPlacement"))));
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
