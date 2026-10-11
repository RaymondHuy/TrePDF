#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

namespace {
bool createHeaderSelectionPdf(const QString &path)
{
    const QByteArray content(
        "BT /F1 12 Tf 24 370 Td (Header text) Tj ET\n"
        "BT /F1 20 Tf 24 295 Td (Crossing text) Tj ET\n"
        "BT /F1 12 Tf 24 190 Td (Body text) Tj ET\n"
        "q 40 0 0 40 220 340 cm /I1 Do Q\n"
        "q 40 0 0 40 220 285 cm /I1 Do Q\n"
        "q 40 0 0 40 220 180 cm /I1 Do Q\n");
    return writePdfObjects(path, {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] "
        "/Resources << /Font << /F1 4 0 R >> /XObject << /I1 6 0 R >> >> "
        "/Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream",
        "<< /Type /XObject /Subtype /Image /Width 1 /Height 1 "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /ASCIIHexDecode "
        "/Length 7 >>\nstream\nff0000>\nendstream"
    });
}

QString selectionId(const QVariantMap &candidate)
{
    return candidate.value(QStringLiteral("objectId")).toString();
}

QRectF selectionFrame(const QVariantMap &candidate)
{
    return candidate.value(QStringLiteral("frameRect")).toRectF();
}

QVariantMap headerImageCandidate(const QVariantList &candidates)
{
    for (const QVariant &value : candidates) {
        const QVariantMap candidate = value.toMap();
        if (candidate.value(QStringLiteral("kind")).toString() == QLatin1String("image")
            && selectionFrame(candidate).bottom() <= 0.25)
            return candidate;
    }
    return {};
}

QQuickItem *visibleHeaderItem(QQuickItem *parent, const QString &name)
{
    if (parent->objectName() == name && parent->isVisible())
        return parent;
    for (QQuickItem *child : parent->childItems()) {
        if (auto *item = visibleHeaderItem(child, name))
            return item;
    }
    return nullptr;
}

QQuickItem *headerEditOverlay(QQuickItem *parent)
{
    if (parent->isVisible()
        && parent->metaObject()->indexOfMethod("selectRelativeCandidate(QVariant)") >= 0)
        return parent;
    for (QQuickItem *child : parent->childItems()) {
        if (auto *item = headerEditOverlay(child))
            return item;
    }
    return nullptr;
}
}

void PDFEditControllerTest::filtersHeaderSelectionToRegion()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-selection.pdf"));
    QVERIFY(createHeaderSelectionPdf(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantList all = waitForCandidates(controller, 0);
    QCOMPARE(all.size(), 6);
    const QVariantMap header = candidateForText(all, QStringLiteral("Header text"));
    const QVariantMap body = candidateForText(all, QStringLiteral("Body text"));
    const QVariantMap crossing = candidateForText(all, QStringLiteral("Crossing text"));
    QVERIFY(!header.isEmpty() && !body.isEmpty() && !crossing.isEmpty());
    QVERIFY(selectionFrame(crossing).top() < 0.25);
    QVERIFY(selectionFrame(crossing).bottom() > 0.25);
    QVERIFY(controller.selectCandidate(0, selectionId(body)));
    controller.setHeaderSelectionOnly(true);
    QVERIFY(controller.headerSelectionOnly());
    QVERIFY(controller.selectedObjectId().isEmpty());
    QCOMPARE(controller.candidatesForPage(0).size(), 2);
    for (const QVariant &value : all) {
        const QVariantMap candidate = value.toMap();
        const QRectF frame = selectionFrame(candidate);
        const bool allowed = frame.bottom() <= 0.25;
        const QPointF center = frame.center();
        QCOMPARE(controller.candidateAllowedInSelection(0, selectionId(candidate)), allowed);
        QCOMPARE(!controller.candidateAt(0, center.x(), center.y()).isEmpty(), allowed);
        QCOMPARE(controller.selectCandidate(0, selectionId(candidate)), allowed);
    }
    // The crossing image begins within the guide, but its whole frame does not fit.
    QVERIFY(controller.candidateAt(0, 0.8, 0.20).isEmpty());
    QVERIFY(controller.candidateAt(0, 0.8, 0.2501).isEmpty());
    QVERIFY(!controller.hasDrafts());
    controller.setHeaderSelectionOnly(false);
    QCOMPARE(controller.candidatesForPage(0).size(), all.size());
    QVERIFY(controller.selectCandidate(0, selectionId(body)));
    QVERIFY(controller.selectCandidate(0, selectionId(crossing)));
}

void PDFEditControllerTest::headerSelectionFollowsDraftGeometryAndHistory()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-selection-history.pdf"));
    QVERIFY(createHeaderSelectionPdf(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Draft header"), QStringLiteral("1"), 24, 65, 100));
    const QVariantMap header = controller.draftsForPage(0).constFirst().toMap();
    const QVariantMap body = controller.addText(0, 0.1, 0.65);
    QVERIFY(!body.isEmpty());
    controller.updateSelectedTextPreview(QStringLiteral("Draft body"));
    controller.finishSelectedTextEdit();
    QImage pixels(80, 40, QImage::Format_RGB32);
    pixels.fill(Qt::blue);
    const QVariantMap image = controller.addImageData(0, 0.5, 0.65, pixels, 40);
    QVERIFY(!image.isEmpty());
    const QString imageId = selectionId(image);
    const QRectF original = selectionFrame(image);
    controller.setTool(PDFEditController::SelectTool);
    QVERIFY(controller.selectCandidate(0, imageId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.5, 0.16, original.width(), original.height()));
    controller.finishSelectedFrameResize();
    controller.setHeaderSelectionOnly(true);
    QVERIFY(controller.selectCandidate(0, selectionId(header)));
    QVERIFY(!controller.selectCandidate(0, selectionId(body)));
    QVERIFY(controller.selectCandidate(0, imageId));
    QCOMPARE(selectionId(controller.candidateAt(0, 0.55, 0.18)), imageId);
    controller.undo(); // Restoring the body position must not restore its selection.
    QVERIFY(controller.selectedObjectId().isEmpty());
    QVERIFY(!controller.selectCandidate(0, imageId));
    controller.redo();
    QVERIFY(controller.selectCandidate(0, imageId));
    QVERIFY(controller.selectedFrameRect().bottom() <= 0.25);
    controller.setHeaderSelectionOnly(false);
    QVERIFY(controller.selectCandidate(0, selectionId(body)));
    controller.setHeaderSelectionOnly(true);
    QVERIFY(controller.selectedObjectId().isEmpty());
}

void PDFEditControllerTest::constrainsHeaderSelectionGeometry()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-selection-geometry.pdf"));
    QVERIFY(createHeaderSelectionPdf(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QImage pixels(80, 40, QImage::Format_RGB32);
    pixels.fill(Qt::blue);
    const QVariantMap image = controller.addImageData(0, 0.4, 0.1, pixels, 40);
    QVERIFY(!image.isEmpty());
    const QRectF original = selectionFrame(image);
    controller.setTool(PDFEditController::SelectTool);
    controller.setHeaderSelectionOnly(true);
    QVERIFY(controller.selectCandidate(0, selectionId(image)));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(original.translated(0.0, 0.7));
    controller.finishSelectedFrameResize();
    const QRectF moved = controller.selectedFrameRect();
    QVERIFY(qAbs(moved.bottom() - 0.25) < 0.0001);
    QCOMPARE(moved.size(), original.size());
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), original);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), moved);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.4, 0.05, 0.3, 0.6));
    controller.finishSelectedFrameResize();
    QVERIFY(controller.selectedFrameRect().bottom() <= 0.25001);
    QVERIFY(controller.selectedContentBounds().bottom() <= 0.25001);
    controller.setHeaderSelectionOnly(false);
    const QRectF before = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(before.translated(0.0, 0.4));
    controller.finishSelectedFrameResize();
    QVERIFY(controller.selectedFrameRect().top() >= 0.4);
    // A wide, short image fits the header, but its quarter turn cannot fit.
    QImage widePixels(150, 20, QImage::Format_RGB32);
    widePixels.fill(Qt::green);
    const QVariantMap wide = controller.addImageData(0, 0.1, 0.1, widePixels, 150);
    QVERIFY(!wide.isEmpty());
    controller.setHeaderSelectionOnly(true);
    QVERIFY(controller.selectCandidate(0, selectionId(wide)));
    QVERIFY(!controller.selectedImageRotatable());
    const QRectF wideFrame = controller.selectedFrameRect();
    controller.rotateSelectedImage(1);
    QCOMPARE(controller.selectedFrameRect(), wideFrame);
}

void PDFEditControllerTest::deletesHeaderSelectionWithUndoAndSave()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-selection-delete.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-selection-saved.pdf"));
    QVERIFY(createHeaderSelectionPdf(source));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantList all = waitForCandidates(controller, 0);
    const QVariantMap header = candidateForText(all, QStringLiteral("Header text"));
    const QVariantMap image = headerImageCandidate(all);
    QVERIFY(!header.isEmpty() && !image.isEmpty());
    controller.setHeaderSelectionOnly(true);
    QVERIFY(controller.selectCandidate(0, selectionId(header)));
    controller.deleteSelectedObject();
    QVERIFY(!controller.selectCandidate(0, selectionId(header)));
    controller.undo();
    QVERIFY(controller.selectCandidate(0, selectionId(header)));
    controller.redo();
    QVERIFY(controller.selectCandidate(0, selectionId(image)));
    controller.deleteSelectedObject();
    controller.undo();
    QVERIFY(controller.selectCandidate(0, selectionId(image)));
    controller.redo();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    const QString saved = normalizedPageText(target, 0);
    QVERIFY(!saved.contains(QStringLiteral("Header text")));
    QVERIFY(saved.contains(QStringLiteral("Body text")));
    QVERIFY(saved.contains(QStringLiteral("Crossing text")));
    QCOMPARE(inspectImages(target).count, 2);
    QVERIFY(normalizedPageText(source, 0).contains(QStringLiteral("Header text")));
    QCOMPARE(inspectImages(source).count, 3);
}

void PDFEditControllerTest::appliesFontFamilyThroughEditAndHeaderPanels_data()
{
    QTest::addColumn<int>("tab");
    QTest::newRow("edit") << 1;
    QTest::newRow("header") << 7;
}

void PDFEditControllerTest::appliesFontFamilyThroughEditAndHeaderPanels()
{
    QFETCH(int, tab);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("font-family-panels.pdf"));
    QVERIFY(createHeaderSelectionPdf(path));
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
    auto &controller = *session->editController();
    const QVariantMap header = candidateForText(waitForCandidates(controller, 0), QStringLiteral("Header text"));
    QVERIFY(!header.isEmpty());
    QVERIFY(tabs->setProperty("currentIndex", tab));
    QTRY_COMPARE(controller.tool(), PDFEditController::SelectTool);
    QVERIFY(controller.selectCandidate(0, selectionId(header)));
    const QString originalFamily = controller.selectedFontFamily();
    const QString originalText = controller.selectedText();
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    const QString family = QStringLiteral("Times New Roman");
    QVERIFY(family != originalFamily);
    QObject *panel = nullptr;
    const auto findPanel = [&]() {
        for (QObject *item : window->findChildren<QObject *>()) {
            if (item->metaObject()->indexOfSignal("fontFamilyChangeRequested(QString)") >= 0)
                return item;
        }
        return static_cast<QObject *>(nullptr);
    };
    QTRY_VERIFY((panel = findPanel()));
    // Choosing a font before opening the editor starts typing at the end.
    QVERIFY(QMetaObject::invokeMethod(panel, "fontFamilyChangeRequested", Q_ARG(QString, family)));
    QTRY_VERIFY(controller.textEditActive());
    QCOMPARE(controller.selectedFontFamily(), family);
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QVERIFY(!controller.hasDrafts());
    auto *overlay = headerEditOverlay(quickWindow->contentItem());
    QVERIFY(overlay);
    QQuickItem *editor = nullptr;
    for (QObject *child : overlay->findChildren<QObject *>()) {
        if (child->metaObject()->indexOfProperty("textDocument") >= 0) {
            editor = qobject_cast<QQuickItem *>(child);
            break;
        }
    }
    QVERIFY(editor);
    QTRY_VERIFY(editor->hasActiveFocus());
    QCOMPARE(editor->property("cursorPosition").toInt(), originalText.size());
    QTest::keyClick(quickWindow, Qt::Key_X);
    QTRY_COMPARE(controller.selectedText(), originalText + QLatin1Char('x'));
    QCOMPARE(controller.selectedCharacterSpans().size(), 2);
    QCOMPARE(controller.selectedCharacterSpans().constFirst().toMap().value(QStringLiteral("fontFamily")).toString(), originalFamily);
    QCOMPARE(controller.selectedCharacterSpans().constLast().toMap().value(QStringLiteral("fontFamily")).toString(), family);
    // The same panel changes only a selected range while editing.
    QVERIFY(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 1), Q_ARG(int, 3)));
    QTRY_COMPARE(controller.selectedFontFamily(), originalFamily);
    QVERIFY(QMetaObject::invokeMethod(panel, "fontFamilyChangeRequested", Q_ARG(QString, family)));
    const QVariantList spans = controller.selectedCharacterSpans();
    QCOMPARE(spans.size(), 4);
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("fontFamily")).toString(), originalFamily);
    QCOMPARE(spans.at(1).toMap().value(QStringLiteral("fontFamily")).toString(), family);
    QCOMPARE(spans.at(1).toMap().value(QStringLiteral("start")).toInt(), 1);
    QCOMPARE(spans.at(1).toMap().value(QStringLiteral("length")).toInt(), 2);
    QCOMPARE(spans.at(2).toMap().value(QStringLiteral("fontFamily")).toString(), originalFamily);
    QVERIFY(QMetaObject::invokeMethod(overlay, "commitEditing"));
    QVERIFY(controller.canUndo());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(controller.selectedCharacterSpans(), spans);
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("font-family-panels-saved.pdf"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QVERIFY(normalizedPageText(saved, 0).contains(originalText + QLatin1Char('x')));
}

void PDFEditControllerTest::selectsHeaderObjectsThroughMainWindow()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("header-selection-ui.pdf"));
    const QString otherPath = m_temporaryDirectory.filePath(QStringLiteral("header-selection-other.pdf"));
    QVERIFY(createHeaderSelectionPdf(path));
    QVERIFY(createHeaderSelectionPdf(otherPath));
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
    const QVariantList all = waitForCandidates(controller, 0);
    const QVariantMap header = candidateForText(all, QStringLiteral("Header text"));
    const QVariantMap body = candidateForText(all, QStringLiteral("Body text"));
    QVERIFY(!header.isEmpty() && !body.isEmpty());
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_VERIFY(controller.headerSelectionOnly());
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    QObject *select = nullptr;
    QTRY_VERIFY((select = window->findChild<QObject *>(QStringLiteral("headerSelectButton"))));
    QVERIFY(select->property("primary").toBool());
    QQuickItem *guide = nullptr;
    QTRY_VERIFY((guide = visibleHeaderItem(quickWindow->contentItem(), QStringLiteral("headerAreaOverlay"))));
    QSignalSpy frame(quickWindow, &QQuickWindow::frameSwapped);
    quickWindow->requestUpdate();
    QTRY_VERIFY(!frame.isEmpty());
    const auto clickCandidate = [&](const QVariantMap &candidate) {
        const QPointF point = selectionFrame(candidate).center();
        QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier,
            guide->mapToScene(QPointF(point.x() * guide->width(),
                point.y() * 4.0 * guide->height())).toPoint());
    };
    clickCandidate(body);
    QVERIFY(controller.selectedObjectId().isEmpty());
    clickCandidate(header);
    QTRY_COMPARE(controller.selectedObjectId(), selectionId(header));
    // Keyboard traversal uses the same filtered list as pointer selection.
    QQuickItem *overlay = headerEditOverlay(quickWindow->contentItem());
    QVERIFY(overlay);
    controller.clearSelection();
    QSet<QString> traversed;
    for (int index = 0; index < all.size() + 1; ++index) {
        QVariant selected;
        QVERIFY(QMetaObject::invokeMethod(overlay, "selectRelativeCandidate",
            Q_RETURN_ARG(QVariant, selected), Q_ARG(QVariant, 1)));
        if (!selected.toBool())
            break;
        QVERIFY(controller.selectedFrameRect().bottom() <= 0.25);
        traversed.insert(controller.selectedObjectId());
    }
    QCOMPARE(traversed.size(), 2);
    for (const char *request : {"requestHeaderText", "requestHeaderImage", "requestHeaderPageNumbers"}) {
        QVERIFY(QMetaObject::invokeMethod(window.get(), request));
        QCOMPARE(controller.tool(), PDFEditController::BrowseTool);
        QVERIFY(controller.selectedObjectId().isEmpty());
        QVERIFY(!select->property("primary").toBool());
        QVERIFY(QMetaObject::invokeMethod(select, "clicked"));
        QCOMPARE(controller.tool(), PDFEditController::SelectTool);
        QVERIFY(select->property("primary").toBool());
    }
    QVERIFY(controller.selectCandidate(0, selectionId(header)));
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QTRY_VERIFY(!controller.headerSelectionOnly());
    QCOMPARE(controller.selectedObjectId(), selectionId(header));
    QVERIFY(controller.selectCandidate(0, selectionId(body)));
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QTRY_VERIFY(controller.headerSelectionOnly());
    QVERIFY(controller.selectedObjectId().isEmpty());
    auto *other = model->open(QUrl::fromLocalFile(otherPath));
    QVERIFY(other && other != session);
    QTRY_VERIFY(other->editController()->headerSelectionOnly());
    QVERIFY(!controller.headerSelectionOnly());
    QCOMPARE(other->editController()->tool(), PDFEditController::SelectTool);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!other->editController()->hasDrafts());
}
