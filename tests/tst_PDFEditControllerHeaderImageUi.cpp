#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QPointer>

using namespace PDFEditControllerTestSupport;

namespace {
QObject *headerImageWorkspace(QObject *window)
{
    return window->property("activeWorkspaceItem").value<QObject *>();
}

QObject *headerImageChild(QObject *window, const QString &name)
{
    auto *workspace = headerImageWorkspace(window);
    return workspace ? workspace->findChild<QObject *>(name) : nullptr;
}

QQuickItem *headerImageItem(QQuickItem *parent, const QString &name)
{
    if (parent->objectName() == name && parent->isVisible())
        return parent;
    for (auto *child : parent->childItems()) {
        if (auto *found = headerImageItem(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *headerImageItem(QObject *object, const QString &name)
{
    auto *workspace = qobject_cast<QQuickItem *>(headerImageWorkspace(object));
    return workspace ? headerImageItem(workspace, name) : nullptr;
}

QRectF headerImageFrame(QObject *placement)
{
    return {placement->property("xPoints").toReal(), placement->property("yPoints").toReal(),
        placement->property("widthPoints").toReal(), placement->property("heightPoints").toReal()};
}

bool createHeaderImageFixture(const QString &path)
{
    QImage image(120, 60, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#1768aa")));
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, 30, 60), QColor(QStringLiteral("#f56a21")));
    painter.end();
    return image.save(path);
}

// Keep the real picker ownership path while avoiding native file-browser interaction.
bool acceptHeaderImage(QObject *window, const QUrl &source)
{
    auto *placement = headerImageChild(window, QStringLiteral("headerImagePlacement"));
    auto *dialog = window->findChild<QObject *>(QStringLiteral("headerImageFileDialog"));
    if (!placement || !dialog || !placement->property("choosing").toBool())
        return false;
    if (!QMetaObject::invokeMethod(window, "insertSelectedHeaderImage", Q_ARG(QVariant, source)))
        return false;
    QMetaObject::invokeMethod(dialog, "close");
    return placement->property("active").toBool();
}

bool clickHeaderImagePosition(QObject *object)
{
    auto *placement = headerImageChild(object, QStringLiteral("headerImagePlacement"));
    auto *window = qobject_cast<QQuickWindow *>(object);
    if (!placement || !window || !window->isExposed())
        return false;
    if (placement->property("choosing").toBool())
        return true;
    if (!placement->property("armed").toBool())
        return false;
    auto *overlay = headerImageItem(object, QStringLiteral("headerImageOverlay"));
    if (!overlay || overlay->width() <= 0 || overlay->height() <= 0)
        return false;
    const qreal zoom = overlay->property("zoom").toReal();
    const QPoint point = overlay->mapToScene(QPointF(40 * zoom, 40 * zoom)).toPoint();
    if (!QRect(QPoint(), window->size()).contains(point))
        return false;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
    return placement->property("choosing").toBool();
}

bool beginHeaderImage(QObject *window, const QUrl &source)
{
    if (!QMetaObject::invokeMethod(window, "requestHeaderImage"))
        return false;
    auto *placement = headerImageChild(window, QStringLiteral("headerImagePlacement"));
    if (!placement || !placement->property("armed").toBool())
        return false;
    if (!QMetaObject::invokeMethod(placement, "place", Q_ARG(QVariant, 0),
            Q_ARG(QVariant, 40.0), Q_ARG(QVariant, 40.0)))
        return false;
    return acceptHeaderImage(window, source);
}
} // namespace

void PDFEditControllerTest::addsHeaderImageThroughMainWindow()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-image-main.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("header-image-main.png"));
    QVERIFY(createThreePageTextPdf(path));
    QVERIFY(createHeaderImageFixture(imagePath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QVERIFY(window->setProperty("visible", true));
    QVERIFY(window->setProperty("allowWindowClose", true));
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(model && tabs);
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QObject *addButton = nullptr;
    QTRY_VERIFY((addButton = window->findChild<QObject *>(QStringLiteral("headerAddImageButton"))));
    QVERIFY(!addButton->property("enabled").toBool());
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    auto *controller = session->editController();
    QTRY_VERIFY(addButton->property("enabled").toBool());
    QTRY_VERIFY(headerImageWorkspace(window.get()));
    QVERIFY(QMetaObject::invokeMethod(addButton, "clicked"));
    auto *placement = headerImageChild(window.get(), QStringLiteral("headerImagePlacement"));
    QVERIFY(placement && placement->property("armed").toBool());
    QSignalSpy pickerRequests(placement, SIGNAL(fileRequested()));
    QVERIFY(pickerRequests.isValid());
    QVERIFY(!headerImageChild(window.get(), QStringLiteral("headerImagePanel")));
    QTRY_VERIFY(clickHeaderImagePosition(window.get()));
    QCOMPARE(pickerRequests.count(), 1);
    QVERIFY(!placement->property("active").toBool());
    QVERIFY(!controller->hasDrafts());
    QVERIFY(acceptHeaderImage(window.get(), QUrl::fromLocalFile(imagePath)));
    QPointer<QObject> panel;
    QTRY_VERIFY((panel = headerImageChild(window.get(), QStringLiteral("headerImagePanel"))));
    QQuickItem *preview = nullptr;
    QTRY_VERIFY((preview = headerImageItem(window.get(), QStringLiteral("headerImagePreview"))));
    QTRY_COMPARE(preview->property("status").toInt(), 1);
    QVERIFY(!controller->hasDrafts());
    auto *allPages = panel->findChild<QObject *>(QStringLiteral("headerImageAllPagesSelector"));
    auto *custom = panel->findChild<QObject *>(QStringLiteral("headerImageCustomPagesSelector"));
    auto *range = panel->findChild<QObject *>(QStringLiteral("headerImagePageRangeInput"));
    auto *apply = panel->findChild<QObject *>(QStringLiteral("headerImageApplyButton"));
    QVERIFY(allPages && custom && range && apply);
    QVERIFY(allPages->property("checked").toBool());
    const QRectF originalFrame = headerImageFrame(placement);
    QVERIFY(custom->setProperty("checked", true));
    QVERIFY(!apply->property("enabled").toBool());
    QVERIFY(range->setProperty("text", QStringLiteral("0, 4")));
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QVERIFY(panel && placement->property("active").toBool());
    QVERIFY(!placement->property("errorText").toString().isEmpty());
    QCOMPARE(headerImageFrame(placement), originalFrame);
    QVERIFY(!controller->hasDrafts());
    QVERIFY(range->setProperty("text", QStringLiteral("1, 3")));
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(panel.isNull());
    QVERIFY(!placement->property("active").toBool());
    QCOMPARE(controller->draftsForPage(0).size(), 1);
    QVERIFY(controller->draftsForPage(1).isEmpty());
    QCOMPARE(controller->draftsForPage(2).size(), 1);
    QCOMPARE(controller->draftsForPage(0).constFirst().toMap().value(QStringLiteral("kind")).toString(),
        QStringLiteral("addedImage"));
    controller->undo();
    QVERIFY(!controller->hasDrafts());
    controller->redo();
    QCOMPARE(controller->draftsForPage(0).size(), 1);
    QCOMPARE(controller->draftsForPage(2).size(), 1);
    controller->discardDrafts();
}

void PDFEditControllerTest::transformsHeaderImagePreviewWithinHeader()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-image-transform.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("header-image-transform.png"));
    QVERIFY(createThreePageTextPdf(path));
    QVERIFY(createHeaderImageFixture(imagePath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QVERIFY(window->setProperty("visible", true));
    QVERIFY(window->setProperty("allowWindowClose", true));
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(model && tabs);
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_VERIFY(headerImageWorkspace(window.get()));
    QVERIFY(beginHeaderImage(window.get(), QUrl::fromLocalFile(imagePath)));
    auto *placement = headerImageChild(window.get(), QStringLiteral("headerImagePlacement"));
    QPointer<QObject> panel;
    QTRY_VERIFY((panel = headerImageChild(window.get(), QStringLiteral("headerImagePanel"))));
    for (int handle = 0; handle < 8; ++handle)
        QTRY_VERIFY(headerImageItem(window.get(), QStringLiteral("headerImageResizeHandle%1").arg(handle)));
    const QRectF initial = headerImageFrame(placement);
    QVERIFY(initial.width() > 0 && initial.height() > 0);
    QVERIFY(qAbs(initial.width() / initial.height() - 2) < 0.001);
    QVERIFY(placement->property("aspectRatioLocked").toBool());
    QVERIFY(QMetaObject::invokeMethod(placement, "resizeFrame", Q_ARG(QVariant, initial),
        Q_ARG(QVariant, 1), Q_ARG(QVariant, 1), Q_ARG(QVariant, 1000.0), Q_ARG(QVariant, 1000.0)));
    const QRectF expanded = headerImageFrame(placement);
    QVERIFY(expanded.width() > initial.width());
    QVERIFY(qAbs(expanded.width() / expanded.height() - 2) < 0.001);
    QVERIFY(expanded.right() <= session->document()->pageSize(0).width() + 0.001);
    QVERIFY(expanded.bottom() <= 100.001);
    QVERIFY(QMetaObject::invokeMethod(placement, "updateFrame", Q_ARG(QVariant, -100.0),
        Q_ARG(QVariant, 500.0), Q_ARG(QVariant, 60.0), Q_ARG(QVariant, 30.0)));
    QCOMPARE(headerImageFrame(placement), QRectF(0, 70, 60, 30));

    auto *rotateRight = panel->findChild<QObject *>(QStringLiteral("headerImageRotateRightButton"));
    auto *rotateLeft = panel->findChild<QObject *>(QStringLiteral("headerImageRotateLeftButton"));
    auto *lock = panel->findChild<QObject *>(QStringLiteral("headerImageAspectRatioLock"));
    QVERIFY(rotateRight && rotateLeft && lock);
    QVERIFY(rotateRight->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(rotateRight, "clicked"));
    QCOMPARE(placement->property("quarterTurns").toInt(), 1);
    QCOMPARE(headerImageFrame(placement).size(), QSizeF(30, 60));
    QVERIFY(headerImageFrame(placement).bottom() <= 100);
    auto *preview = headerImageItem(window.get(), QStringLiteral("headerImagePreview"));
    QVERIFY(preview);
    QCOMPARE(preview->property("rotation").toReal(), 90.0);
    QVERIFY(QMetaObject::invokeMethod(rotateLeft, "clicked"));
    QCOMPARE(placement->property("quarterTurns").toInt(), 0);
    QCOMPARE(headerImageFrame(placement).size(), QSizeF(60, 30));
    QVERIFY(lock->setProperty("checked", false));
    QVERIFY(!placement->property("aspectRatioLocked").toBool());
    const QRectF beforeUnlockedResize = headerImageFrame(placement);
    QVERIFY(QMetaObject::invokeMethod(placement, "resizeFrame", Q_ARG(QVariant, beforeUnlockedResize),
        Q_ARG(QVariant, 1), Q_ARG(QVariant, 0), Q_ARG(QVariant, 50.0), Q_ARG(QVariant, 0.0)));
    QCOMPARE(headerImageFrame(placement).height(), beforeUnlockedResize.height());
    QCOMPARE(headerImageFrame(placement).width(), 110.0);
    QVERIFY(!rotateRight->property("enabled").toBool());
    QVariant rotated;
    QVERIFY(QMetaObject::invokeMethod(placement, "rotate", Q_RETURN_ARG(QVariant, rotated), Q_ARG(QVariant, 1)));
    QVERIFY(!rotated.toBool());
    QCOMPARE(placement->property("quarterTurns").toInt(), 0);
    QVERIFY(!session->editController()->hasDrafts());
    QVERIFY(QMetaObject::invokeMethod(placement, "cancel"));
    QTRY_VERIFY(panel.isNull());
}

void PDFEditControllerTest::cancelsHeaderImageAcrossUiChanges()
{
    const QString firstPath = m_temporaryDirectory.filePath(QStringLiteral("header-image-cancel-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(QStringLiteral("header-image-cancel-second.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("header-image-cancel.png"));
    QVERIFY(createThreePageTextPdf(firstPath));
    QVERIFY(createThreePageTextPdf(secondPath));
    QVERIFY(createHeaderImageFixture(imagePath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QVERIFY(window->setProperty("visible", true));
    QVERIFY(window->setProperty("allowWindowClose", true));
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(model && tabs);
    auto *first = model->open(QUrl::fromLocalFile(firstPath));
    auto *second = model->open(QUrl::fromLocalFile(secondPath));
    QVERIFY(first && second);
    auto *controller = first->editController();
    for (const QString &action : {QStringLiteral("cancel"), QStringLiteral("ribbon"),
            QStringLiteral("session"), QStringLiteral("history"), QStringLiteral("structure"),
            QStringLiteral("save"), QStringLiteral("textTool")}) {
        model->setCurrentIndex(0);
        QVERIFY(tabs->setProperty("currentIndex", 7));
        QTRY_VERIFY(headerImageWorkspace(window.get()));
        QVERIFY2(beginHeaderImage(window.get(), QUrl::fromLocalFile(imagePath)), qPrintable(action));
        QPointer<QObject> placement = headerImageChild(window.get(), QStringLiteral("headerImagePlacement"));
        QPointer<QObject> panel;
        QTRY_VERIFY((panel = headerImageChild(window.get(), QStringLiteral("headerImagePanel"))));
        QVERIFY(!controller->hasDrafts());
        if (action == QLatin1String("cancel")) {
            auto *cancel = panel->findChild<QObject *>(QStringLiteral("headerImageCancelButton"));
            QVERIFY(cancel && QMetaObject::invokeMethod(cancel, "clicked"));
        } else if (action == QLatin1String("ribbon")) {
            QVERIFY(tabs->setProperty("currentIndex", 8));
            QCOMPARE(tabs->property("currentIndex").toInt(), 7);
            QVERIFY(panel && placement->property("active").toBool());
            auto *notice = window->findChild<QObject *>(QStringLiteral("headerEntryNoticeLoader"));
            QVERIFY(notice && notice->property("active").toBool());
            auto *understood = headerImageItem(qobject_cast<QQuickWindow *>(window.get())->contentItem(),
                QStringLiteral("headerEntryUnderstoodButton"));
            QVERIFY(understood && QMetaObject::invokeMethod(understood, "clicked"));
            auto *cancel = panel->findChild<QObject *>(QStringLiteral("headerImageCancelButton"));
            QVERIFY(cancel && QMetaObject::invokeMethod(cancel, "clicked"));
            QVERIFY(tabs->setProperty("currentIndex", 8));
            QCOMPARE(tabs->property("currentIndex").toInt(), 8);
        } else if (action == QLatin1String("session")) {
            model->setCurrentIndex(1);
        } else if (action == QLatin1String("history")) {
            QVERIFY(controller->addHeaderText(QStringLiteral("Other draft"), QStringLiteral("1")));
        } else if (action == QLatin1String("structure")) {
            QVERIFY(controller->addBlankPage() >= 0);
        } else if (action == QLatin1String("textTool")) {
            QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
        } else {
            QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSave"));
        }
        QTRY_VERIFY2(panel.isNull(), qPrintable(action));
        QTRY_VERIFY(!placement || !placement->property("active").toBool());
        if (action == QLatin1String("textTool")) {
            // Leaving Image keeps its all-pages placement as unsaved drafts.
            for (int page = 0; page < 3; ++page) {
                const auto drafts = controller->draftsForPage(page);
                QCOMPARE(drafts.size(), 1);
                QCOMPARE(drafts.constFirst().toMap().value(QStringLiteral("kind")).toString(),
                    QStringLiteral("addedImage"));
            }
        }
        QVERIFY(!controller->addHeaderImage(QStringLiteral("1"), 0, 0, 20, 20));
        // Save submits the pending placement and completes asynchronously.
        if (action == QLatin1String("save"))
            QTRY_VERIFY_WITH_TIMEOUT(!controller->saving() && !controller->hasDrafts(), 10000);
        if (controller->hasDrafts())
            controller->discardDrafts();
        QVERIFY(!controller->hasDrafts());
        QVERIFY(!second->editController()->hasDrafts());
    }

    // Closing the picker cancels the armed placement without showing a properties panel.
    model->setCurrentIndex(0);
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderImage"));
    auto *placement = headerImageChild(window.get(), QStringLiteral("headerImagePlacement"));
    QVERIFY(placement);
    QVERIFY(QMetaObject::invokeMethod(placement, "place", Q_ARG(QVariant, 0),
        Q_ARG(QVariant, 40.0), Q_ARG(QVariant, 40.0)));
    QVERIFY(placement->property("choosing").toBool());
    auto *dialog = window->findChild<QObject *>(QStringLiteral("headerImageFileDialog"));
    QVERIFY(dialog && QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(!placement->property("choosing").toBool());
    QVERIFY(!placement->property("active").toBool());
    QVERIFY(!headerImageChild(window.get(), QStringLiteral("headerImagePanel")));
    QVERIFY(!controller->hasDrafts());
}

void PDFEditControllerTest::retainsHeaderImageAcrossPageDelegatesAndZoom()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-image-delegates.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("header-image-delegates.png"));
    QVERIFY(createThreePageTextPdf(path));
    QVERIFY(createHeaderImageFixture(imagePath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QVERIFY(window->setProperty("visible", true));
    QVERIFY(window->setProperty("allowWindowClose", true));
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(model && tabs);
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QObject *workspace = nullptr;
    QTRY_VERIFY((workspace = headerImageWorkspace(window.get())));
    QTRY_VERIFY(workspace->property("initialViewReady").toBool());
    auto *view = workspace->findChild<QObject *>(QStringLiteral("primaryDocumentView"));
    QVERIFY(view);
    QSignalSpy navigationSettled(view, SIGNAL(navigationSettled(int)));
    QVERIFY(navigationSettled.isValid());
    QVERIFY(QMetaObject::invokeMethod(workspace, "setPageDisplayMode",
        Q_ARG(QVariant, int(PDFDocumentSession::SinglePageMode))));
    // Mode changes queue a relayout around the old page; let it settle before navigating.
    QTRY_VERIFY(navigationSettled.count() > 0);
    QTRY_COMPARE(view->property("pendingNavigationPage").toInt(), -1);
    QVERIFY(beginHeaderImage(window.get(), QUrl::fromLocalFile(imagePath)));
    auto *placement = headerImageChild(window.get(), QStringLiteral("headerImagePlacement"));
    const QRectF originalFrame = headerImageFrame(placement);
    const QString originalPreview = placement->property("previewSource").toString();
    QPointer<QQuickItem> oldPreview;
    QTRY_VERIFY((oldPreview = headerImageItem(window.get(), QStringLiteral("headerImagePreview"))));
    QVERIFY(QMetaObject::invokeMethod(workspace, "navigateToPage", Q_ARG(QVariant, 2)));
    QTRY_COMPARE(session->document()->currentPage(), 2);
    QTRY_VERIFY(oldPreview.isNull());
    QVERIFY(placement->property("active").toBool());
    QCOMPARE(headerImageFrame(placement), originalFrame);
    QCOMPARE(placement->property("previewSource").toString(), originalPreview);
    QTRY_VERIFY(!headerImageItem(window.get(), QStringLiteral("headerImagePreview")));
    QVERIFY(QMetaObject::invokeMethod(workspace, "setZoom", Q_ARG(QVariant, 1.75)));
    QTRY_COMPARE(session->zoom(), 1.75);
    QVERIFY(QMetaObject::invokeMethod(workspace, "navigateToPage", Q_ARG(QVariant, 0)));
    QTRY_COMPARE(session->document()->currentPage(), 0);
    QQuickItem *preview = nullptr;
    QTRY_VERIFY((preview = headerImageItem(window.get(), QStringLiteral("headerImagePreview"))));
    QTRY_COMPARE(preview->property("status").toInt(), 1);
    QCOMPARE(headerImageFrame(placement), originalFrame);
    QCOMPARE(placement->property("previewSource").toString(), originalPreview);
    QVERIFY(qAbs(preview->width() - originalFrame.width() * 1.75) < 0.01);
    QVERIFY(qAbs(preview->height() - originalFrame.height() * 1.75) < 0.01);
    QVERIFY(!session->editController()->hasDrafts());
    QVERIFY(QMetaObject::invokeMethod(placement, "submit", Q_ARG(QVariant, QStringLiteral("all"))));
    for (int page = 0; page < 3; ++page)
        QCOMPARE(session->editController()->draftsForPage(page).size(), 1);
    session->editController()->undo();
    QVERIFY(!session->editController()->hasDrafts());
}
