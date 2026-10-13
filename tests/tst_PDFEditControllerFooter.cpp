#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QClipboard>

using namespace PDFEditControllerTestSupport;

namespace {
bool footerPages(const QString &path)
{
    return writePdfObjects(path, {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 600] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>"});
}

QString footerId(const QVariantMap &candidate)
{
    return candidate.value(QStringLiteral("objectId")).toString();
}

QQuickItem *footerItem(QQuickItem *parent, const QString &name)
{
    if (parent->objectName() == name && parent->isVisible())
        return parent;
    for (auto *child : parent->childItems()) {
        if (auto *found = footerItem(child, name))
            return found;
    }
    return nullptr;
}
}

void PDFEditControllerTest::footerTextPersistsAndSyncsAcrossPageSizes()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("footer-text.pdf"));
    QVERIFY(footerPages(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Header stays"), QStringLiteral("all"), 24, 20, 200));
    controller.setFooterMode(true);
    QVERIFY(controller.addHeaderText(QStringLiteral("Footer text"), QStringLiteral("all"), 24, 20, 200));
    for (int page = 0; page < 3; ++page) {
        const auto drafts = controller.draftsForPage(page);
        QVariantMap footer;
        for (const auto &draft : drafts) {
            if (draft.toMap().value(QStringLiteral("text")).toString() == QStringLiteral("Footer text"))
                footer = draft.toMap();
        }
        QVERIFY(!footer.isEmpty());
        const QRectF frame = footer.value(QStringLiteral("frameRect")).toRectF();
        const QSizeF size = document.pageSize(page);
        QVERIFY(qAbs(frame.top() * size.height() - (size.height() - 80)) < 0.01);
    }
    controller.undo();
    for (int page = 0; page < 3; ++page) {
        const auto drafts = controller.draftsForPage(page);
        QCOMPARE(drafts.size(), 1);
        QCOMPARE(drafts.first().toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Header stays"));
    }
    controller.redo();
    QVERIFY(controller.save());
    controller.setHeaderSelectionOnly(true);
    controller.setTool(PDFEditController::SelectTool);
    const auto candidate = candidateForText(waitForCandidates(controller, 0), QStringLiteral("Footer text"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.candidateAt(0, 0.1, 0.05).isEmpty());
    QVERIFY(controller.selectCandidate(0, footerId(candidate)));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 3);
    controller.commitSelectedText(QStringLiteral("Changed footer"));
    QVERIFY2(controller.applyHeaderChanges(), qPrintable(controller.headerSyncError()));
    QVERIFY(controller.save());
    for (int page = 0; page < 3; ++page) {
        const QString text = normalizedPageText(path, page);
        QVERIFY(text.contains(QStringLiteral("Header stays")));
        QVERIFY(text.contains(QStringLiteral("Changed footer")));
        QVERIFY(!text.contains(QStringLiteral("Footer text")));
        const auto saved = candidateForText(waitForCandidates(controller, page), QStringLiteral("Changed footer"));
        QVERIFY(!saved.isEmpty());
        const QRectF frame = saved.value(QStringLiteral("frameRect")).toRectF();
        QVERIFY(frame.top() >= controller.selectionTopRatio(page) - 0.001);
        QVERIFY(frame.bottom() <= 1.001);
    }
}

void PDFEditControllerTest::footerNumbersKeepValuesAfterSync()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("footer-numbers.pdf"));
    QVERIFY(footerPages(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    controller.setFooterMode(true);
    QVERIFY(controller.addHeaderPageNumbers(1, 3, 1, 2, 24, 25, 80));
    QVERIFY(controller.save());
    controller.setHeaderSelectionOnly(true);
    controller.setTool(PDFEditController::SelectTool);
    const auto candidate = candidateForText(waitForCandidates(controller, 0), QStringLiteral("i"));
    QVERIFY(controller.selectCandidate(0, footerId(candidate)));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 3);
    controller.setSelectedFontSize(18);
    QVERIFY2(controller.applyHeaderChanges(), qPrintable(controller.headerSyncError()));
    QVERIFY(controller.save());
    const QStringList expected{QStringLiteral("i"), QStringLiteral("1"), QStringLiteral("2")};
    for (int page = 0; page < 3; ++page)
        QCOMPARE(normalizedPageText(path, page).trimmed(), expected.at(page));
}

void PDFEditControllerTest::footerImagesCopyAndConstrainGeometry()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("footer-images.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("footer-image.png"));
    QVERIFY(footerPages(path));
    QImage pixels(40, 20, QImage::Format_ARGB32);
    pixels.fill(Qt::blue);
    QVERIFY(pixels.save(imagePath));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    controller.setFooterMode(true);
    controller.setHeaderSelectionOnly(true);
    controller.setTool(PDFEditController::SelectTool);
    QVERIFY(!controller.prepareHeaderImage(QUrl::fromLocalFile(imagePath)).isEmpty());
    QVERIFY(!controller.addHeaderImage(QStringLiteral("all"), 24, 90, 60, 30));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.addHeaderImage(QStringLiteral("all"), 24, 20, 60, 30));
    QCOMPARE(controller.draftsForPage(1).size(), 1);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.addImageData(0, 54.0 / 300.0, 335.0 / 400.0, pixels, 60, 30).isEmpty());
    QVERIFY(controller.save());
    const auto candidate = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(controller.selectCandidate(0, footerId(candidate)));
    auto *clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard);
    clipboard->clear();
    const auto cleanup = qScopeGuard([clipboard] { clipboard->clear(); });
    QVERIFY(controller.copySelectedImage());
    QTRY_VERIFY_WITH_TIMEOUT(controller.clipboardHasImage(), 10000);
    QVERIFY(!controller.pasteImageFromClipboard(1).isEmpty());
    QRectF pasted = controller.selectedFrameRect();
    QVERIFY(qAbs(pasted.x() * 400 - 24) < 0.01);
    QVERIFY(qAbs(pasted.y() * 600 - 520) < 0.01);
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 2);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(pasted.translated(0, -1));
    controller.finishSelectedFrameResize();
    QVERIFY(qAbs(controller.selectedFrameRect().top() - 500.0 / 600.0) < 0.001);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), pasted);
    QVERIFY(controller.save());
    const auto saved = candidateForKind(waitForCandidates(controller, 1), QStringLiteral("image"));
    QVERIFY(controller.selectCandidate(1, footerId(saved)));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 2);
}

void PDFEditControllerTest::footerToolsAndSaveGuardsThroughMainWindow()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*Binding loop.*")));
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("footer-main.pdf"));
    QVERIFY(footerPages(path));
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
    QVERIFY(tabs->setProperty("currentIndex", 8));
    QTRY_VERIFY(controller.footerMode() && controller.headerSelectionOnly());
    QTRY_COMPARE(controller.tool(), PDFEditController::SelectTool);
    QObject *workspace = nullptr;
    QTRY_VERIFY((workspace = window->property("activeWorkspaceItem").value<QObject *>()));
    QTRY_VERIFY(workspace->property("initialViewReady").toBool());
    QVERIFY(QMetaObject::invokeMethod(window.get(), "setZoom", Q_ARG(QVariant, 1.0)));
    QQuickItem *guide = nullptr;
    QTRY_VERIFY((guide = footerItem(quickWindow->contentItem(), QStringLiteral("headerAreaOverlay"))));
    QVERIFY(qAbs(guide->y() + guide->height() - guide->parentItem()->height()) < 0.01);
    QVERIFY(guide->y() > 0);
    for (const QString &name : {QStringLiteral("footerAddTextButton"),
             QStringLiteral("footerAddImageButton"), QStringLiteral("footerPageNumbersButton")}) {
        auto *button = window->findChild<QObject *>(name);
        QVERIFY(button);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY(button->property("primary").toBool());
        auto *select = window->findChild<QObject *>(QStringLiteral("footerSelectButton"));
        QVERIFY(select);
        QVERIFY(QMetaObject::invokeMethod(select, "clicked"));
        QTRY_VERIFY(select->property("primary").toBool());
    }
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
    QQuickItem *area = nullptr;
    QTRY_VERIFY((area = footerItem(quickWindow->contentItem(), QStringLiteral("headerTextPlacementArea"))));
    const qreal zoom = area->parentItem()->property("zoom").toReal();
    QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier,
        area->mapToScene(QPointF(24 * zoom, 20 * zoom)).toPoint());
    QQuickItem *editor = nullptr;
    QTRY_VERIFY((editor = footerItem(quickWindow->contentItem(), QStringLiteral("headerInlineTextInput"))));
    QVERIFY(editor->setProperty("text", QStringLiteral("Footer UI")));
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_COMPARE(tabs->property("currentIndex").toInt(), 8);
    auto *noticeLoader = window->findChild<QObject *>(QStringLiteral("headerEntryNoticeLoader"));
    QVERIFY(noticeLoader && noticeLoader->property("footerMode").toBool());
    QObject *save = nullptr;
    QTRY_VERIFY((save = window->findChild<QObject *>(QStringLiteral("headerEntrySaveButton"))));
    QVERIFY(QMetaObject::invokeMethod(save, "clicked"));
    QTRY_VERIFY_WITH_TIMEOUT(normalizedPageText(path, 0).contains(QStringLiteral("Footer UI")), 15000);
    QTRY_VERIFY(!controller.saving() && !controller.hasDrafts());
    for (int page = 0; page < 3; ++page)
        QVERIFY(normalizedPageText(path, page).contains(QStringLiteral("Footer UI")));
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_VERIFY(!controller.footerMode() && controller.headerSelectionOnly());
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QTRY_VERIFY(!controller.headerSelectionOnly());
    QVERIFY(!controller.addText(0, 0.2, 0.5).isEmpty());
    controller.commitSelectedText(QStringLiteral("Unsaved body"));
    QVERIFY(tabs->setProperty("currentIndex", 8));
    QTRY_COMPARE(tabs->property("currentIndex").toInt(), 1);
    QVERIFY(noticeLoader->property("footerMode").toBool());
    controller.discardDrafts();
}

void PDFEditControllerTest::retainsMarginPlacementAcrossFocusAndToolChanges_data()
{
    QTest::addColumn<bool>("footer");
    QTest::addColumn<bool>("image");
    QTest::newRow("header-text") << false << false;
    QTest::newRow("header-image") << false << true;
    QTest::newRow("footer-text") << true << false;
    QTest::newRow("footer-image") << true << true;
}

void PDFEditControllerTest::retainsMarginPlacementAcrossFocusAndToolChanges()
{
    QFETCH(bool, footer);
    QFETCH(bool, image);
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*Binding loop.*")));
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("margin-focus-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("margin-focus.png"));
    QVERIFY(footerPages(path));
    QFile sourceFile(path);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray savedSource = sourceFile.readAll();
    sourceFile.close();
    QImage pixels(60, 30, QImage::Format_ARGB32);
    pixels.fill(Qt::blue);
    QVERIFY(pixels.save(imagePath));
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
    QVERIFY(tabs->setProperty("currentIndex", footer ? 8 : 7));
    QObject *workspace = nullptr;
    QTRY_VERIFY((workspace = window->property("activeWorkspaceItem").value<QObject *>()));
    QTRY_VERIFY(workspace->property("initialViewReady").toBool());
    QVERIFY(QMetaObject::invokeMethod(window.get(), "setZoom", Q_ARG(QVariant, 1.0)));
    const char *request = image ? "requestHeaderImage" : "requestHeaderText";
    const QString placementName = image ? QStringLiteral("headerImagePlacement")
                                        : QStringLiteral("headerTextPlacement");
    const QString panelName = image ? QStringLiteral("headerImagePanel")
                                    : QStringLiteral("headerTextPanel");
    const QString expectedText = QStringLiteral("Kept margin text");
    const auto beginPlacement = [&]() {
        if (!QMetaObject::invokeMethod(window.get(), request))
            return false;
        auto *placement = workspace->findChild<QObject *>(placementName);
        if (!placement || !QMetaObject::invokeMethod(placement, "place", Q_ARG(QVariant, 0),
                Q_ARG(QVariant, 24.0), Q_ARG(QVariant, 20.0)))
            return false;
        if (!image)
            return placement->property("active").toBool();
        auto *picker = window->findChild<QObject *>(QStringLiteral("headerImageFileDialog"));
        if (!picker || !QMetaObject::invokeMethod(window.get(), "insertSelectedHeaderImage",
                Q_ARG(QVariant, QUrl::fromLocalFile(imagePath))))
            return false;
        QMetaObject::invokeMethod(picker, "close");
        return placement->property("active").toBool();
    };
    QVERIFY(beginPlacement());
    auto *placement = workspace->findChild<QObject *>(placementName);
    QPointer<QObject> panel;
    QTRY_VERIFY((panel = workspace->findChild<QObject *>(panelName)));
    QQuickItem *editor = nullptr;
    if (!image) {
        QTRY_VERIFY((editor = footerItem(quickWindow->contentItem(), QStringLiteral("headerInlineTextInput"))));
        QVERIFY(editor->setProperty("text", expectedText));
    }
    auto *custom = panel->findChild<QObject *>(image ? QStringLiteral("headerImageCustomPagesSelector")
                                                    : QStringLiteral("headerCustomPagesSelector"));
    auto *range = panel->findChild<QObject *>(image ? QStringLiteral("headerImagePageRangeInput")
                                                   : QStringLiteral("headerPageRangeInput"));
    QVERIFY(custom && range);
    QVERIFY(custom->setProperty("checked", true));
    QVERIFY(range->setProperty("text", QStringLiteral("0, 4")));
    const QString preview = placement->property("previewSource").toString();
    const qreal x = placement->property("xPoints").toReal();
    const qreal y = placement->property("yPoints").toReal();

    // A real click in the page body and focus leaving the editor must not discard input.
    QQuickItem *guide = nullptr;
    QTRY_VERIFY((guide = footerItem(quickWindow->contentItem(), QStringLiteral("headerAreaOverlay"))));
    quickWindow->contentItem()->forceActiveFocus();
    QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier,
        guide->parentItem()->mapToScene(QPointF(150, 200)).toPoint());
    QVERIFY(placement->property("active").toBool());
    QVERIFY(panel);
    QVERIFY(!controller.hasDrafts());
    if (!image)
        QCOMPARE(editor->property("text").toString(), expectedText);
    else
        QCOMPARE(placement->property("previewSource").toString(), preview);
    QCOMPARE(placement->property("xPoints").toReal(), x);
    QCOMPARE(placement->property("yPoints").toReal(), y);
    QVERIFY(QMetaObject::invokeMethod(window.get(), request));
    QVERIFY(placement->property("active").toBool());

    // Invalid page ranges keep the current tool and editable placement intact.
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderSelect"));
    QVERIFY(placement->property("active").toBool());
    QVERIFY(panel);
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.tool(), PDFEditController::BrowseTool);
    QCOMPARE(range->property("text").toString(), QStringLiteral("0, 4"));
    QVERIFY(!placement->property("errorText").toString().isEmpty());

    // A valid tool change retains the object as an ordinary unsaved draft on its chosen pages.
    QVERIFY(range->setProperty("text", QStringLiteral("1, 3")));
    QVERIFY(QMetaObject::invokeMethod(window.get(), image ? "requestHeaderText" : "requestHeaderImage"));
    QTRY_VERIFY(panel.isNull());
    QVERIFY(!placement->property("active").toBool());
    QVERIFY(controller.hasDrafts());
    for (int page = 0; page < 3; ++page) {
        const auto drafts = controller.draftsForPage(page);
        QCOMPARE(drafts.size(), page == 1 ? 0 : 1);
        if (drafts.isEmpty())
            continue;
        const QVariantMap draft = drafts.constFirst().toMap();
        QCOMPARE(draft.value(QStringLiteral("kind")).toString(),
            image ? QStringLiteral("addedImage") : QStringLiteral("addedText"));
        if (!image)
            QCOMPARE(draft.value(QStringLiteral("text")).toString(), expectedText);
        const QRectF frame = draft.value(QStringLiteral("frameRect")).toRectF();
        const QSizeF size = session->document()->pageSize(page);
        QVERIFY(qAbs(frame.left() * size.width() - x) < 0.01);
        QVERIFY(qAbs(frame.top() * size.height() - (footer ? size.height() - 100 + y : y)) < 0.01);
    }
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderSelect"));
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    QCOMPARE(sourceFile.readAll(), savedSource);
    sourceFile.close();
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSave"));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.saving() && !controller.hasDrafts(), 15000);
    for (int page = 0; page < 3; ++page) {
        if (image) {
            const auto saved = candidateForKind(waitForCandidates(controller, page), QStringLiteral("image"));
            QCOMPARE(!saved.isEmpty(), page != 1);
        } else {
            QCOMPARE(normalizedPageText(path, page).contains(expectedText), page != 1);
        }
    }

    // Explicit Cancel still abandons a newly placed object without creating a draft.
    QVERIFY(beginPlacement());
    QTRY_VERIFY((panel = workspace->findChild<QObject *>(panelName)));
    if (!image) {
        QTRY_VERIFY((editor = footerItem(quickWindow->contentItem(), QStringLiteral("headerInlineTextInput"))));
        QVERIFY(editor->setProperty("text", QStringLiteral("Cancelled text")));
    }
    auto *cancel = panel->findChild<QObject *>(image ? QStringLiteral("headerImageCancelButton")
                                                    : QStringLiteral("headerTextCancelButton"));
    QVERIFY(cancel && QMetaObject::invokeMethod(cancel, "clicked"));
    QTRY_VERIFY(panel.isNull());
    QVERIFY(!placement->property("active").toBool());
    QVERIFY(!controller.hasDrafts());
}
