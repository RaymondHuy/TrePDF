#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QPointer>

using namespace PDFEditControllerTestSupport;

namespace {
QQuickItem *headerVisualItem(QQuickItem *parent, const QString &name)
{
    if (parent->objectName() == name && parent->isVisible())
        return parent;
    for (auto *child : parent->childItems()) {
        if (auto *found = headerVisualItem(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem *headerVisualItem(QObject *window, const QString &name)
{
    auto *quickWindow = qobject_cast<QQuickWindow *>(window);
    return quickWindow ? headerVisualItem(quickWindow->contentItem(), name) : nullptr;
}

bool placeHeader(QObject *window)
{
    auto *placement = window->findChild<QObject *>(QStringLiteral("headerTextPlacement"));
    auto *quickWindow = qobject_cast<QQuickWindow *>(window);
    if (!placement || !quickWindow || !quickWindow->isExposed())
        return false;
    if (placement->property("active").toBool())
        return true;
    if (!placement->property("armed").toBool()
        || window->findChild<QObject *>(QStringLiteral("headerTextPanel")))
        return false;
    auto *overlay = headerVisualItem(window, QStringLiteral("headerTextOverlay"));
    if (!overlay || overlay->width() <= 0 || overlay->height() <= 0)
        return false;
    const qreal zoom = overlay->property("zoom").toReal();
    const QPoint position = overlay->mapToScene(QPointF(24 * zoom, 16 * zoom)).toPoint();
    if (!QRect(QPoint(), quickWindow->size()).contains(position))
        return false;
    QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier, position);
    return placement->property("active").toBool();
}
}

void PDFEditControllerTest::addsHeaderTextThroughMainWindow()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-text-main.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QVERIFY(window->setProperty("visible", true));
    QVERIFY(window->setProperty("allowWindowClose", true));
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(model && tabs);
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QObject *addButton = nullptr;
    QTRY_VERIFY((addButton = window->findChild<QObject *>(QStringLiteral("headerAddTextButton"))));
    QVERIFY(!addButton->property("enabled").toBool());

    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    auto *controller = session->editController();
    QTRY_VERIFY(window->findChild<QObject *>(QStringLiteral("documentWorkspace")));
    QTRY_VERIFY(addButton->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(addButton, "clicked"));
    QTRY_VERIFY(placeHeader(window.get()));
    QPointer<QObject> panel;
    QTRY_VERIFY((panel = window->findChild<QObject *>(QStringLiteral("headerTextPanel"))));
    QObject *text = nullptr;
    QTRY_VERIFY((text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"))));
    auto *allPages = panel->findChild<QObject *>(QStringLiteral("headerAllPagesSelector"));
    auto *apply = panel->findChild<QObject *>(QStringLiteral("headerTextApplyButton"));
    QVERIFY(text && allPages && apply);
    QVERIFY(allPages->property("checked").toBool());
    QVERIFY(!apply->property("enabled").toBool());
    QVERIFY(text->setProperty("text", QStringLiteral("All-page heading")));
    QVERIFY(apply->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(addButton, "clicked"));
    QCOMPARE(window->findChild<QObject *>(QStringLiteral("headerTextPanel")), panel.data());
    QCOMPARE(text->property("text").toString(), QStringLiteral("All-page heading"));
    QVERIFY(!controller->hasDrafts());
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(panel.isNull());
    for (int page = 0; page < 3; ++page)
        QCOMPARE(controller->draftsForPage(page).size(), 1);
    controller->undo();
    QVERIFY(!controller->hasDrafts());

    QVERIFY(QMetaObject::invokeMethod(addButton, "clicked"));
    QTRY_VERIFY(placeHeader(window.get()));
    QTRY_VERIFY((panel = window->findChild<QObject *>(QStringLiteral("headerTextPanel"))));
    QTRY_VERIFY((text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"))));
    auto *customPages = panel->findChild<QObject *>(QStringLiteral("headerCustomPagesSelector"));
    auto *range = panel->findChild<QObject *>(QStringLiteral("headerPageRangeInput"));
    apply = panel->findChild<QObject *>(QStringLiteral("headerTextApplyButton"));
    QVERIFY(text && customPages && range && apply);
    // Keep this heading on one line even with the compact frame on the 240 pt page.
    QVERIFY(text->setProperty("text", QStringLiteral("Selected")));
    QVERIFY(customPages->setProperty("checked", true));
    QVERIFY(range->setProperty("text", QStringLiteral("1, 3")));
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(panel.isNull());
    QCOMPARE(controller->draftsForPage(0).size(), 1);
    QVERIFY(controller->draftsForPage(1).isEmpty());
    QCOMPARE(controller->draftsForPage(2).size(), 1);

    // Save commits the applied batch, never the text still being composed on the page.
    QVERIFY(QMetaObject::invokeMethod(addButton, "clicked"));
    QTRY_VERIFY(placeHeader(window.get()));
    QTRY_VERIFY((panel = window->findChild<QObject *>(QStringLiteral("headerTextPanel"))));
    QTRY_VERIFY((text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"))));
    QVERIFY(text);
    QVERIFY(text->setProperty("text", QStringLiteral("Unapplied heading")));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSave"));
    QTRY_VERIFY(panel.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(!controller->saving() && !controller->hasDrafts(), 10000);
    for (int page = 0; page < 3; ++page) {
        const QString savedText = pageText(path, page);
        QCOMPARE(savedText.contains(QStringLiteral("Selected")), page != 1);
        QVERIFY(!savedText.contains(QStringLiteral("Unapplied heading")));
        QVERIFY(!savedText.contains(QStringLiteral("All-page heading")));
    }
}

void PDFEditControllerTest::formatsHeaderTextThroughMainWindow()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-format-ui.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QVERIFY(window->setProperty("visible", true));
    QVERIFY(window->setProperty("allowWindowClose", true));
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(model && tabs);
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    auto *controller = session->editController();
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_VERIFY(window->findChild<QObject *>(QStringLiteral("documentWorkspace")));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
    QTRY_VERIFY(placeHeader(window.get()));
    QPointer<QObject> panel;
    QTRY_VERIFY((panel = window->findChild<QObject *>(QStringLiteral("headerTextPanel"))));
    auto *placement = window->findChild<QObject *>(QStringLiteral("headerTextPlacement"));
    auto *text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"));
    auto *family = panel->findChild<QObject *>(QStringLiteral("headerFontFamilyComboBox"));
    auto *size = panel->findChild<QObject *>(QStringLiteral("headerFontSizeBox"));
    auto *color = panel->findChild<QObject *>(QStringLiteral("headerFontColorButton"));
    auto *apply = panel->findChild<QObject *>(QStringLiteral("headerTextApplyButton"));
    QVERIFY(placement && text && family && size && color && apply);
    QVERIFY(text->setProperty("text", QStringLiteral("ABC")));
    QVERIFY(QMetaObject::invokeMethod(placement, "setFontFamily",
        Q_ARG(QVariant, QVariant(QStringLiteral("Times New Roman")))));
    QCOMPARE(placement->property("fontFamily").toString(), QStringLiteral("Times New Roman"));
    QCOMPARE(family->property("displayText").toString(), QStringLiteral("Times New Roman"));
    for (const QString &name : {QStringLiteral("headerFontBoldButton"),
                               QStringLiteral("headerFontItalicButton"),
                               QStringLiteral("headerFontUnderlineButton")}) {
        auto *button = panel->findChild<QQuickItem *>(name);
        QVERIFY(button);
        QTest::mouseClick(qobject_cast<QQuickWindow *>(window.get()), Qt::LeftButton,
            Qt::NoModifier, button->mapToScene(QPointF(button->width() / 2,
                button->height() / 2)).toPoint());
        QVERIFY(button->property("checked").toBool());
    }
    QVERIFY(QMetaObject::invokeMethod(color, "clicked"));
    QPointer<QObject> colorDialog;
    QTRY_VERIFY((colorDialog = panel->findChild<QObject *>(QStringLiteral("headerFontColorDialog"))));
    QTRY_VERIFY(colorDialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(colorDialog, "reject"));
    QTRY_VERIFY(colorDialog.isNull());
    QVERIFY(QMetaObject::invokeMethod(color, "clicked"));
    QTRY_VERIFY((colorDialog = panel->findChild<QObject *>(QStringLiteral("headerFontColorDialog"))));
    QTRY_VERIFY(colorDialog->property("visible").toBool());
    QVERIFY(colorDialog->setProperty("selectedColor", QColor(QStringLiteral("#1a4d99"))));
    QVERIFY(QMetaObject::invokeMethod(colorDialog, "accept"));
    QTRY_VERIFY(colorDialog.isNull());
    // Leave a size edit pending: Apply must commit it before creating drafts.
    QVERIFY(size->setProperty("editText", QStringLiteral("18")));
    QVERIFY(size->setProperty("pending", true));
    const QFont previewFont = text->property("font").value<QFont>();
    QVERIFY(previewFont.bold() && previewFont.italic() && previewFont.underline());
    QCOMPARE(text->property("color").value<QColor>(), QColor(QStringLiteral("#1a4d99")));
    QVERIFY(!controller->hasDrafts());
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(panel.isNull());
    for (int page = 0; page < 3; ++page) {
        const auto drafts = controller->draftsForPage(page);
        QCOMPARE(drafts.size(), 1);
        const QVariantMap draft = drafts.constFirst().toMap();
        QCOMPARE(draft.value(QStringLiteral("fontFamily")).toString(), QStringLiteral("Times New Roman"));
        QVERIFY(draft.value(QStringLiteral("fontBold")).toBool());
        QVERIFY(draft.value(QStringLiteral("fontItalic")).toBool());
        QVERIFY(draft.value(QStringLiteral("fontUnderline")).toBool());
        QCOMPARE(draft.value(QStringLiteral("color")).value<QColor>(), QColor(QStringLiteral("#1a4d99")));
        QVERIFY(qAbs(draft.value(QStringLiteral("fontSizeRatio")).toReal()
            * session->document()->pageSize(page).height() - 18.0) < 0.001);
    }
    controller->undo();
    QVERIFY(!controller->hasDrafts());
    controller->redo();
    QVERIFY(controller->hasDrafts());
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
    QTRY_VERIFY(placeHeader(window.get()));
    QCOMPARE(placement->property("fontSize").toReal(), 12.0);
    QVERIFY(!placement->property("fontBold").toBool());
    QCOMPARE(placement->property("textColor").value<QColor>(), QColor(Qt::black));
    QVERIFY(QMetaObject::invokeMethod(placement, "cancel"));
    controller->discardDrafts();
}

void PDFEditControllerTest::cancelsHeaderTextPanelAcrossUiChanges()
{
    const QString firstPath = m_temporaryDirectory.filePath(QStringLiteral("header-cancel-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(QStringLiteral("header-cancel-second.pdf"));
    QVERIFY(createThreePageTextPdf(firstPath));
    QVERIFY(createThreePageTextPdf(secondPath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
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
                                  QStringLiteral("session"), QStringLiteral("history"),
                                  QStringLiteral("structure"), QStringLiteral("discard"),
                                  QStringLiteral("save")}) {
        model->setCurrentIndex(0);
        QVERIFY(tabs->setProperty("currentIndex", 7));
        QTRY_VERIFY(window->findChild<QObject *>(QStringLiteral("documentWorkspace")));
        if (action == QLatin1String("discard"))
            QVERIFY(controller->addHeaderText(QStringLiteral("Existing draft"), QStringLiteral("all")));
        QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
        QTRY_VERIFY(placeHeader(window.get()));
        QPointer<QObject> panel;
        QTRY_VERIFY((panel = window->findChild<QObject *>(QStringLiteral("headerTextPanel"))));
        QObject *text = nullptr;
        QTRY_VERIFY((text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"))));
        QVERIFY(text);
        QCOMPARE(text->property("text").toString(), QString());
        QVERIFY(text->setProperty("text", QStringLiteral("Never applied")));
        if (action == QLatin1String("cancel")) {
            auto *cancel = panel->findChild<QObject *>(QStringLiteral("headerTextCancelButton"));
            QVERIFY(cancel);
            QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        } else if (action == QLatin1String("ribbon")) {
            QVERIFY(tabs->setProperty("currentIndex", 8));
        } else if (action == QLatin1String("session")) {
            model->setCurrentIndex(1);
        } else if (action == QLatin1String("history")) {
            QVERIFY(controller->addHeaderText(QStringLiteral("Other draft"), QStringLiteral("1")));
        } else if (action == QLatin1String("structure")) {
            QVERIFY(controller->addBlankPage());
        } else if (action == QLatin1String("discard")) {
            controller->discardDrafts();
        } else {
            QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSave"));
        }
        QTRY_VERIFY2(panel.isNull(), qPrintable(action));
        if (controller->hasDrafts())
            controller->discardDrafts();
        QVERIFY(!controller->hasDrafts());
        QVERIFY(!second->editController()->hasDrafts());
        for (int page = 0; page < 3; ++page)
            QVERIFY(!pageText(firstPath, page).contains(QStringLiteral("Never applied")));
    }
}

void PDFEditControllerTest::validatesHeaderTextPanelWithoutLosingInput()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-text-validation-ui.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
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
    QTRY_VERIFY(window->findChild<QObject *>(QStringLiteral("documentWorkspace")));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
    QTRY_VERIFY(placeHeader(window.get()));
    QPointer<QObject> panel;
    QTRY_VERIFY((panel = window->findChild<QObject *>(QStringLiteral("headerTextPanel"))));
    QObject *text = nullptr;
    QTRY_VERIFY((text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"))));
    auto *customPages = panel->findChild<QObject *>(QStringLiteral("headerCustomPagesSelector"));
    auto *range = panel->findChild<QObject *>(QStringLiteral("headerPageRangeInput"));
    auto *apply = panel->findChild<QObject *>(QStringLiteral("headerTextApplyButton"));
    QVERIFY(text && customPages && range && apply);
    QVERIFY(text->setProperty("text", QString(701, QLatin1Char('A'))));
    QCOMPARE(text->property("text").toString().size(), 700);
    QVERIFY(text->setProperty("text", QStringLiteral("Keep this heading")));
    QVERIFY(customPages->setProperty("checked", true));
    QVERIFY(!apply->property("enabled").toBool());
    QVERIFY(range->setProperty("text", QStringLiteral("0, 4")));
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QVERIFY(panel);
    auto *placement = window->findChild<QObject *>(QStringLiteral("headerTextPlacement"));
    QVERIFY(placement);
    QVERIFY(!placement->property("errorText").toString().isEmpty());
    QCOMPARE(text->property("text").toString(), QStringLiteral("Keep this heading"));
    QCOMPARE(range->property("text").toString(), QStringLiteral("0, 4"));
    QVERIFY(!session->editController()->hasDrafts());
    QVERIFY(range->setProperty("text", QStringLiteral("2-3")));
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(panel.isNull());
    QVERIFY(session->editController()->draftsForPage(0).isEmpty());
    QCOMPARE(session->editController()->draftsForPage(1).size(), 1);
    QCOMPARE(session->editController()->draftsForPage(2).size(), 1);
    session->editController()->discardDrafts();
}
