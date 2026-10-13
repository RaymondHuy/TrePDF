#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFFontAliases.h"

#include <QPointer>
#include <QClipboard>

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

QVector<QColor> headerEditorColors(QObject *editor)
{
    auto *quickDocument = editor->property("textDocument").value<QQuickTextDocument *>();
    if (!quickDocument || !quickDocument->textDocument())
        return {};
    auto *document = quickDocument->textDocument();
    QVector<QColor> result;
    for (int index = 0; index < document->toPlainText().size(); ++index) {
        QTextCursor cursor(document);
        cursor.setPosition(index);
        cursor.setPosition(index + 1, QTextCursor::KeepAnchor);
        const QBrush brush = cursor.charFormat().foreground();
        result.append(brush.style() == Qt::NoBrush ? QColor(Qt::black) : brush.color());
    }
    return result;
}

QStringList headerEditorFamilies(QObject *editor)
{
    auto *quickDocument = editor->property("textDocument").value<QQuickTextDocument *>();
    if (!quickDocument || !quickDocument->textDocument())
        return {};
    auto *document = quickDocument->textDocument();
    QStringList result;
    for (int index = 0; index < document->toPlainText().size(); ++index) {
        QTextCursor cursor(document);
        cursor.setPosition(index);
        cursor.setPosition(index + 1, QTextCursor::KeepAnchor);
        const QStringList families = cursor.charFormat().fontFamilies().toStringList();
        result.append(families.isEmpty() ? document->defaultFont().family() : families.constFirst());
    }
    return result;
}

QStringList headerUiDraftFamilies(const QVariantMap &draft)
{
    QStringList result(draft.value(QStringLiteral("text")).toString().size(), QString());
    for (const QVariant &value : draft.value(QStringLiteral("characterSpans")).toList()) {
        const QVariantMap span = value.toMap();
        const int start = span.value(QStringLiteral("start")).toInt();
        const int end = start + span.value(QStringLiteral("length")).toInt();
        if (start < 0 || end > result.size())
            return {};
        for (int index = start; index < end; ++index)
            result[index] = PDFFontAliases::semanticFamily(span.value(QStringLiteral("fontFamily")).toString());
    }
    return result;
}

QVector<QColor> headerUiDraftColors(const QVariantMap &draft)
{
    QVector<QColor> result(draft.value(QStringLiteral("text")).toString().size());
    for (const QVariant &value : draft.value(QStringLiteral("characterSpans")).toList()) {
        const QVariantMap span = value.toMap();
        const int start = span.value(QStringLiteral("start")).toInt();
        const int end = start + span.value(QStringLiteral("length")).toInt();
        if (start < 0 || end > result.size())
            return {};
        for (int index = start; index < end; ++index)
            result[index] = span.value(QStringLiteral("color")).value<QColor>();
    }
    return result;
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
    const QString baseFamily = placement->property("fontFamily").toString();
    QVERIFY(text->setProperty("text", QStringLiteral("ABC")));
    QVERIFY(text->setProperty("cursorPosition", 3));
    QVERIFY(QMetaObject::invokeMethod(placement, "setFontFamily",
        Q_ARG(QVariant, QVariant(QStringLiteral("Times New Roman")))));
    QCOMPARE(placement->property("fontFamily").toString(), QStringLiteral("Times New Roman"));
    QCOMPARE(family->property("displayText").toString(), QStringLiteral("Times New Roman"));
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    QSignalSpy framePresented(quickWindow, &QQuickWindow::frameSwapped);
    quickWindow->requestUpdate();
    QVERIFY(framePresented.wait());
    for (const QString &name : {QStringLiteral("headerFontBoldButton"),
                               QStringLiteral("headerFontItalicButton"),
                               QStringLiteral("headerFontUnderlineButton")}) {
        auto *button = panel->findChild<QQuickItem *>(name);
        QVERIFY(button);
        QTest::mouseClick(qobject_cast<QQuickWindow *>(window.get()), Qt::LeftButton,
            Qt::NoModifier, button->mapToScene(QPointF(button->width() / 2,
                button->height() / 2)).toPoint());
        QTRY_VERIFY(button->property("checked").toBool());
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
    QCOMPARE(headerEditorColors(text), QVector<QColor>(3, QColor(Qt::black)));
    QTRY_VERIFY(text->hasActiveFocus());
    QCOMPARE(text->property("cursorPosition").toInt(), 3);
    QTest::keyClick(qobject_cast<QQuickWindow *>(window.get()), Qt::Key_X);
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("ABCx"));
    const QVector<QColor> expectedColors{Qt::black, Qt::black, Qt::black,
        QColor(QStringLiteral("#1a4d99"))};
    QTRY_COMPARE(headerEditorColors(text), expectedColors);
    // Leave a size edit pending: Apply must commit it before creating drafts.
    QVERIFY(size->setProperty("editText", QStringLiteral("18")));
    QVERIFY(size->setProperty("pending", true));
    const QFont previewFont = text->property("font").value<QFont>();
    QVERIFY(previewFont.bold() && previewFont.italic() && previewFont.underline());
    QCOMPARE(headerEditorColors(text), expectedColors);
    QVERIFY(!controller->hasDrafts());
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(panel.isNull());
    for (int page = 0; page < 3; ++page) {
        const auto drafts = controller->draftsForPage(page);
        QCOMPARE(drafts.size(), 1);
        const QVariantMap draft = drafts.constFirst().toMap();
        QCOMPARE(headerUiDraftFamilies(draft),
            (QStringList{baseFamily, baseFamily, baseFamily, QStringLiteral("Times New Roman")}));
        QVERIFY(draft.value(QStringLiteral("fontBold")).toBool());
        QVERIFY(draft.value(QStringLiteral("fontItalic")).toBool());
        QVERIFY(draft.value(QStringLiteral("fontUnderline")).toBool());
        QCOMPARE(headerUiDraftColors(draft), expectedColors);
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

void PDFEditControllerTest::appliesHeaderColorsOnlyToTypedCharacters()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-caret-color-ui.pdf"));
    QVERIFY(createThreePageTextPdf(path));
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
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_VERIFY(window->findChild<QObject *>(QStringLiteral("documentWorkspace")));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
    QTRY_VERIFY(placeHeader(window.get()));
    auto *placement = window->findChild<QObject *>(QStringLiteral("headerTextPlacement"));
    auto *text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"));
    QVERIFY(placement && text);

    // A choice before the first key affects the new characters only.
    QVERIFY(QMetaObject::invokeMethod(placement, "setTextColor",
        Q_ARG(QVariant, QColor(Qt::blue))));
    QTRY_VERIFY(text->hasActiveFocus());
    for (int index = 0; index < 3; ++index)
        QTest::keyClick(quickWindow, Qt::Key_A);
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("aaa"));
    QTRY_COMPARE(headerEditorColors(text), QVector<QColor>(3, QColor(Qt::blue)));
    QVERIFY(!session->editController()->hasDrafts());

    // Repeated characters make prefix/suffix-only insertion detection ambiguous.
    QVERIFY(text->setProperty("cursorPosition", 1));
    QVERIFY(QMetaObject::invokeMethod(placement, "setTextColor",
        Q_ARG(QVariant, QColor(Qt::red))));
    QCOMPARE(headerEditorColors(text), QVector<QColor>(3, QColor(Qt::blue)));
    QTRY_VERIFY(text->hasActiveFocus());
    QTest::keyClick(quickWindow, Qt::Key_A);
    QVector<QColor> expected{Qt::blue, Qt::red, Qt::blue, Qt::blue};
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("aaaa"));
    QTRY_COMPARE(headerEditorColors(text), expected);
    QCOMPARE(text->property("cursorPosition").toInt(), 2);

    // Selection replacement may leave the plain text unchanged, but is still a new edit.
    QVERIFY(QMetaObject::invokeMethod(text, "select", Q_ARG(int, 1), Q_ARG(int, 2)));
    QVERIFY(QMetaObject::invokeMethod(placement, "setTextColor",
        Q_ARG(QVariant, QColor(Qt::green))));
    QCOMPARE(headerEditorColors(text), expected);
    QTRY_VERIFY(text->hasActiveFocus());
    QCOMPARE(text->property("selectionStart").toInt(), 1);
    QCOMPARE(text->property("selectionEnd").toInt(), 2);
    QTest::keyClick(quickWindow, Qt::Key_A);
    expected[1] = QColor(Qt::green);
    QTRY_COMPARE(headerEditorColors(text), expected);
    QCOMPARE(text->property("text").toString(), QStringLiteral("aaaa"));
    QCOMPARE(text->property("cursorPosition").toInt(), 2);

    // Color and text must share the native undo command; redo must restore the caret.
    QTest::keySequence(quickWindow, QKeySequence(QKeySequence::Undo));
    expected[1] = QColor(Qt::red);
    QTRY_COMPARE(headerEditorColors(text), expected);
    QTest::keySequence(quickWindow, QKeySequence(QKeySequence::Redo));
    expected[1] = QColor(Qt::green);
    QTRY_COMPARE(headerEditorColors(text), expected);
    QTRY_COMPARE(text->property("cursorPosition").toInt(), 2);

    // Moving away from a chosen typing color resumes the color at the new caret.
    QVERIFY(QMetaObject::invokeMethod(placement, "setTextColor",
        Q_ARG(QVariant, QColor(Qt::magenta))));
    QCOMPARE(headerEditorColors(text), expected);
    QVERIFY(text->setProperty("cursorPosition", 4));
    QTRY_COMPARE(placement->property("textColor").value<QColor>(), QColor(Qt::blue));
    QTest::keyClick(quickWindow, Qt::Key_B);
    expected.append(QColor(Qt::blue));
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("aaaab"));
    QTRY_COMPARE(headerEditorColors(text), expected);

    // Global font controls must not flatten the per-character foreground colors.
    QVERIFY(QMetaObject::invokeMethod(placement, "setFontFamily",
        Q_ARG(QVariant, QStringLiteral("Times New Roman"))));
    QVERIFY(QMetaObject::invokeMethod(placement, "setFontSize", Q_ARG(QVariant, 16)));
    for (const char *method : {"setFontBold", "setFontItalic", "setFontUnderline"})
        QVERIFY(QMetaObject::invokeMethod(placement, method, Q_ARG(QVariant, true)));
    QTRY_VERIFY(text->property("font").value<QFont>().bold());
    QCOMPARE(headerEditorColors(text), expected);
    QVERIFY(QMetaObject::invokeMethod(placement, "submit", Q_ARG(QVariant, QStringLiteral("all"))));
    QVERIFY2(!placement->property("active").toBool(),
        qPrintable(placement->property("errorText").toString()));
    for (int page = 0; page < 3; ++page) {
        const QVariantList drafts = session->editController()->draftsForPage(page);
        QCOMPARE(drafts.size(), 1);
        QCOMPARE(headerUiDraftColors(drafts.constFirst().toMap()), expected);
    }
    session->editController()->undo();
    QVERIFY(!session->editController()->hasDrafts());

    // Trimming an over-limit paste must not split a surrogate or separate its color undo.
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
    QTRY_VERIFY(placeHeader(window.get()));
    QTRY_VERIFY((text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"))));
    QTRY_VERIFY(text->hasActiveFocus());
    QTest::keyClick(quickWindow, Qt::Key_X);
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("x"));
    QVERIFY(QMetaObject::invokeMethod(placement, "setTextColor",
        Q_ARG(QVariant, QColor(Qt::blue))));
    QTRY_VERIFY(text->hasActiveFocus());
    QGuiApplication::clipboard()->setText(QString(698, QLatin1Char('a'))
        + QString::fromUtf8("\xf0\x9f\x98\x80") + QLatin1Char('z'));
    QTest::keySequence(quickWindow, QKeySequence(QKeySequence::Paste));
    const QString boundedText = QLatin1Char('x') + QString(698, QLatin1Char('a'));
    QTRY_COMPARE(text->property("text").toString(), boundedText);
    QVector<QColor> boundedColors(699, QColor(Qt::blue));
    boundedColors[0] = QColor(Qt::black);
    QTRY_COMPARE(headerEditorColors(text), boundedColors);
    QTest::keySequence(quickWindow, QKeySequence(QKeySequence::Undo));
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("x"));
    QCOMPARE(headerEditorColors(text), QVector<QColor>{QColor(Qt::black)});
    QTest::keySequence(quickWindow, QKeySequence(QKeySequence::Redo));
    QTRY_COMPARE(text->property("text").toString(), boundedText);
    QTRY_COMPARE(headerEditorColors(text), boundedColors);
    QVERIFY(QMetaObject::invokeMethod(placement, "cancel"));
}

void PDFEditControllerTest::retainsHeaderColorsAcrossPageDelegatesAndZoom()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-color-delegates.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
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
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QObject *workspace = nullptr;
    QTRY_VERIFY((workspace = window->findChild<QObject *>(QStringLiteral("documentWorkspace"))));
    QTRY_VERIFY(workspace->property("initialViewReady").toBool());
    auto *view = workspace->findChild<QObject *>(QStringLiteral("primaryDocumentView"));
    QVERIFY(view);
    QSignalSpy navigationSettled(view, SIGNAL(navigationSettled(int)));
    QVERIFY(navigationSettled.isValid());
    QVERIFY(QMetaObject::invokeMethod(workspace, "setPageDisplayMode",
        Q_ARG(QVariant, int(PDFDocumentSession::SinglePageMode))));
    QTRY_VERIFY(navigationSettled.count() > 0);
    QTRY_COMPARE(view->property("pendingNavigationPage").toInt(), -1);
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
    QTRY_VERIFY(placeHeader(window.get()));
    auto *placement = window->findChild<QObject *>(QStringLiteral("headerTextPlacement"));
    QPointer<QQuickItem> oldEditor;
    QTRY_VERIFY((oldEditor = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"))));
    QVERIFY(placement);
    const QString baseFamily = placement->property("fontFamily").toString();
    const QString otherFamily = QStringLiteral("Times New Roman");
    QTRY_VERIFY(oldEditor->hasActiveFocus());
    QTest::keyClick(quickWindow, Qt::Key_A);
    QVERIFY(QMetaObject::invokeMethod(placement, "setTextColor",
        Q_ARG(QVariant, QColor(Qt::red))));
    QTRY_VERIFY(oldEditor->hasActiveFocus());
    QVERIFY(QMetaObject::invokeMethod(placement, "setFontFamily",
        Q_ARG(QVariant, otherFamily)));
    QTest::keyClick(quickWindow, Qt::Key_B);
    QTRY_COMPARE(headerEditorColors(oldEditor), (QVector<QColor>{Qt::black, Qt::red}));
    QVERIFY(oldEditor->setProperty("cursorPosition", 1));
    QVERIFY(QMetaObject::invokeMethod(placement, "setTextColor",
        Q_ARG(QVariant, QColor(Qt::blue))));
    QVERIFY(QMetaObject::invokeMethod(placement, "setFontFamily",
        Q_ARG(QVariant, otherFamily)));
    QCOMPARE(headerEditorFamilies(oldEditor), (QStringList{baseFamily, otherFamily}));
    const QVariantList originalColors = placement->property("colors").toList();
    QCOMPARE(originalColors.size(), 2);
    QVERIFY(QMetaObject::invokeMethod(workspace, "navigateToPage", Q_ARG(QVariant, 2)));
    QTRY_COMPARE(session->document()->currentPage(), 2);
    QTRY_VERIFY(oldEditor.isNull());
    QVERIFY(placement->property("active").toBool());
    QCOMPARE(placement->property("text").toString(), QStringLiteral("ab"));
    QCOMPARE(placement->property("colors").toList(), originalColors);
    QCOMPARE(placement->property("cursorPosition").toInt(), 1);
    QCOMPARE(placement->property("textColor").value<QColor>(), QColor(Qt::blue));
    QVERIFY(QMetaObject::invokeMethod(workspace, "setZoom", Q_ARG(QVariant, 1.75)));
    QTRY_COMPARE(session->zoom(), 1.75);
    QVERIFY(QMetaObject::invokeMethod(workspace, "navigateToPage", Q_ARG(QVariant, 0)));
    QTRY_COMPARE(session->document()->currentPage(), 0);
    QQuickItem *text = nullptr;
    QTRY_VERIFY((text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"))));
    QTRY_VERIFY(text->hasActiveFocus());
    QCOMPARE(text->property("cursorPosition").toInt(), 1);
    QCOMPARE(headerEditorColors(text), (QVector<QColor>{Qt::black, Qt::red}));
    QCOMPARE(headerEditorFamilies(text), (QStringList{baseFamily, otherFamily}));
    QCOMPARE(placement->property("fontFamily").toString(), otherFamily);
    QTest::keyClick(quickWindow, Qt::Key_X);
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("axb"));
    const QVector<QColor> expected{Qt::black, Qt::blue, Qt::red};
    QTRY_COMPARE(headerEditorColors(text), expected);
    QTRY_COMPARE(headerEditorFamilies(text), (QStringList{baseFamily, otherFamily, otherFamily}));
    QVERIFY(QMetaObject::invokeMethod(placement, "submit", Q_ARG(QVariant, QStringLiteral("1,3"))));
    QVERIFY2(!placement->property("active").toBool(),
        qPrintable(placement->property("errorText").toString()));
    for (int page : {0, 2}) {
        const QVariantList drafts = session->editController()->draftsForPage(page);
        QCOMPARE(drafts.size(), 1);
        QCOMPARE(headerUiDraftColors(drafts.constFirst().toMap()), expected);
    }
    QVERIFY(session->editController()->draftsForPage(1).isEmpty());
    session->editController()->undo();
    QVERIFY(!session->editController()->hasDrafts());
}

void PDFEditControllerTest::appliesHeaderFontsAtCaretAndSelection()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-font-caret.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-font-caret-saved.pdf"));
    QVERIFY(createThreePageTextPdf(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
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
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_VERIFY(window->findChild<QObject *>(QStringLiteral("documentWorkspace")));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
    QTRY_VERIFY(placeHeader(window.get()));
    auto *placement = window->findChild<QObject *>(QStringLiteral("headerTextPlacement"));
    auto *text = headerVisualItem(window.get(), QStringLiteral("headerInlineTextInput"));
    QVERIFY(placement && text);
    const QString baseFamily = placement->property("fontFamily").toString();
    const QString otherFamily = QStringLiteral("Times New Roman");
    QVERIFY(baseFamily != otherFamily);
    QTRY_VERIFY(text->hasActiveFocus());
    for (int index = 0; index < 3; ++index)
        QTest::keyClick(quickWindow, Qt::Key_A);
    QTRY_COMPARE(headerEditorFamilies(text), QStringList(3, baseFamily));
    QVERIFY(text->setProperty("cursorPosition", 1));
    QVERIFY(QMetaObject::invokeMethod(placement, "setFontFamily", Q_ARG(QVariant, otherFamily)));
    QCOMPARE(headerEditorFamilies(text), QStringList(3, baseFamily));
    QVERIFY(!session->editController()->hasDrafts());
    QTRY_VERIFY(text->hasActiveFocus());
    QTest::keyClick(quickWindow, Qt::Key_A);
    QStringList expected{baseFamily, otherFamily, baseFamily, baseFamily};
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("aaaa"));
    QTRY_COMPARE(headerEditorFamilies(text), expected);
    QTest::keySequence(quickWindow, QKeySequence(QKeySequence::Undo));
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("aaa"));
    QTRY_COMPARE(headerEditorFamilies(text), QStringList(3, baseFamily));
    QTest::keySequence(quickWindow, QKeySequence(QKeySequence::Redo));
    QTRY_COMPARE(headerEditorFamilies(text), expected);

    // Changing a selected range changes exactly that range and has its own undo.
    QVERIFY(QMetaObject::invokeMethod(text, "select", Q_ARG(int, 2), Q_ARG(int, 3)));
    QVERIFY(QMetaObject::invokeMethod(placement, "setFontFamily", Q_ARG(QVariant, otherFamily)));
    expected[2] = otherFamily;
    QTRY_COMPARE(headerEditorFamilies(text), expected);
    QTRY_VERIFY(text->hasActiveFocus());
    QTest::keySequence(quickWindow, QKeySequence(QKeySequence::Undo));
    expected[2] = baseFamily;
    QTRY_COMPARE(headerEditorFamilies(text), expected);
    QTest::keySequence(quickWindow, QKeySequence(QKeySequence::Redo));
    expected[2] = otherFamily;
    QTRY_COMPARE(headerEditorFamilies(text), expected);

    // Caret movement abandons an unused choice and follows the adjacent character.
    QVERIFY(QMetaObject::invokeMethod(text, "deselect"));
    QVERIFY(text->setProperty("cursorPosition", 4));
    QTRY_COMPARE(placement->property("fontFamily").toString(), baseFamily);
    QTest::keyClick(quickWindow, Qt::Key_B);
    expected.append(baseFamily);
    QTRY_COMPARE(headerEditorFamilies(text), expected);
    QTRY_COMPARE(text->property("text").toString(), QStringLiteral("aaaab"));
    QVERIFY(QMetaObject::invokeMethod(placement, "submit", Q_ARG(QVariant, QStringLiteral("all"))));
    QVERIFY2(!placement->property("active").toBool(), qPrintable(placement->property("errorText").toString()));
    auto *controller = session->editController();
    for (int page = 0; page < 3; ++page)
        QCOMPARE(headerUiDraftFamilies(controller->draftsForPage(page).constFirst().toMap()), expected);
    controller->undo();
    QVERIFY(!controller->hasDrafts());
    controller->redo();
    for (int page = 0; page < 3; ++page)
        QCOMPARE(headerUiDraftFamilies(controller->draftsForPage(page).constFirst().toMap()), expected);
    QVERIFY(controller->saveAs(QUrl::fromLocalFile(target)));
    const auto characters = textCharacterRangeInfo(target, QStringLiteral("aaaab"));
    QCOMPARE(characters.size(), expected.size());
    for (int index = 0; index < expected.size(); ++index)
        QCOMPARE(characters.at(index).font.familyName, expected.at(index));
    PDFDocument reopened;
    QVERIFY(reopened.open(QUrl::fromLocalFile(target)));
    PDFEditController reopenedController;
    reopenedController.setDocument(&reopened);
    for (int page = 0; page < 3; ++page) {
        const QVariantList candidates = waitForCandidates(reopenedController, page);
        bool found = false;
        for (const QVariant &value : candidates) {
            const QVariantMap candidate = value.toMap();
            if (candidate.value(QStringLiteral("text")).toString() != QStringLiteral("aaaab"))
                continue;
            QCOMPARE(headerUiDraftFamilies(candidate), expected);
            found = true;
        }
        QVERIFY(found);
    }
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
            QCOMPARE(tabs->property("currentIndex").toInt(), 7);
            QVERIFY(panel && text->property("text").toString() == QStringLiteral("Never applied"));
            auto *notice = window->findChild<QObject *>(QStringLiteral("headerEntryNoticeLoader"));
            QVERIFY(notice && notice->property("active").toBool());
            auto *understood = headerVisualItem(window.get(), QStringLiteral("headerEntryUnderstoodButton"));
            QVERIFY(understood && QMetaObject::invokeMethod(understood, "clicked"));
            auto *cancel = panel->findChild<QObject *>(QStringLiteral("headerTextCancelButton"));
            QVERIFY(cancel && QMetaObject::invokeMethod(cancel, "clicked"));
            QVERIFY(tabs->setProperty("currentIndex", 8));
            QCOMPARE(tabs->property("currentIndex").toInt(), 8);
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
        // Save applies pending text; the other transitions cancel it.
        if (action == QLatin1String("save"))
            QTRY_VERIFY_WITH_TIMEOUT(!controller->saving() && !controller->hasDrafts(), 10000);
        if (controller->hasDrafts())
            controller->discardDrafts();
        QVERIFY(!controller->hasDrafts());
        QVERIFY(!second->editController()->hasDrafts());
        for (int page = 0; page < 3; ++page) {
            const QString savedText = normalizedPageText(firstPath, page);
            QVERIFY2(savedText.contains(QStringLiteral("Never applied"))
                         == (action == QLatin1String("save")), qPrintable(savedText));
        }
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
