#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::rendersAddedImageSnapshot()
{
    QImage source(8, 8, QImage::Format_ARGB32);
    source.fill(QColor(QStringLiteral("#2b5fd8")));
    for (int y = 0; y < source.height() / 2; ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor(QStringLiteral("#d86f4b")));
        }
    }

    DraftImageItem item;
    item.setWidth(32);
    item.setHeight(32);
    item.setSourceImage(QVariant::fromValue(source));
    source.fill(Qt::green);

    QImage rendered(32, 32, QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    item.paint(&painter);
    painter.end();

    const QColor topPixel = rendered.pixelColor(16, 4);
    const QColor bottomPixel = rendered.pixelColor(16, 28);
    QVERIFY(topPixel.red() > topPixel.blue());
    QVERIFY(bottomPixel.blue() > bottomPixel.red());
}

void PDFEditControllerTest::addsImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-image-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-image-preview.png"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#4b8fd8")));
    QVERIFY(image.save(imagePath, "PNG"));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::AddImageTool);

    const QPointF insertionPoint(0.2, 0.65);
    const QVariantMap added = controller.addImage(
        0,
        insertionPoint.x(),
        insertionPoint.y(),
        QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    QCOMPARE(added.value(QStringLiteral("kind")).toString(), QStringLiteral("addedImage"));
    QCOMPARE(added.value(QStringLiteral("added")).toBool(), true);
    QCOMPARE(added.value(QStringLiteral("imagePixelWidth")).toInt(), 80);
    QCOMPARE(added.value(QStringLiteral("imagePixelHeight")).toInt(), 40);
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    const QString objectId = added.value(QStringLiteral("objectId")).toString();
    const QRectF frame = added.value(QStringLiteral("frameRect")).toRectF();
    QVERIFY(frame.contains(insertionPoint));
    QVERIFY(frame.left() >= 0.0);
    QVERIFY(frame.top() >= 0.0);
    QVERIFY(frame.right() <= 1.0);
    QVERIFY(frame.bottom() <= 1.0);
    const QSizeF pageSize = document.pageSize(0);
    const qreal displayedAspect = frame.width() * pageSize.width()
        / (frame.height() * pageSize.height());
    QVERIFY(qAbs(displayedAspect - 2.0) < 0.001);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.textEditActive());
    QCOMPARE(
        controller.candidateAt(0, frame.center().x(), frame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);

    controller.undo();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());
    const QVariantList afterUndo = waitForCandidates(controller, 0);
    QVERIFY(std::none_of(
        afterUndo.cbegin(),
        afterUndo.cend(),
        [&objectId](const QVariant &value) {
            return value.toMap().value(QStringLiteral("objectId")).toString()
                == objectId;
        }));

    controller.redo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    const QVariantList afterRedo = waitForCandidates(controller, 0);
    const auto restored = std::find_if(
        afterRedo.cbegin(),
        afterRedo.cend(),
        [&objectId](const QVariant &value) {
            return value.toMap().value(QStringLiteral("objectId")).toString()
                == objectId;
        });
    QVERIFY(restored != afterRedo.cend());
    QCOMPARE(
        restored->toMap().value(QStringLiteral("frameRect")).toRectF(),
        frame);

    QVERIFY(controller.selectCandidate(0, objectId));
    const QRectF resizedFrame(
        frame.x() + 0.01,
        frame.y() + 0.01,
        frame.width() * 0.8,
        frame.height() * 0.8);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
    controller.finishSelectedFrameResize();
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
}

void PDFEditControllerTest::movesAddedImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-added-image-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-added-image.png"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#4b8fd8")));
    QVERIFY(image.save(imagePath, "PNG"));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap added = controller.addImage(
        0,
        0.2,
        0.55,
        QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    const QString objectId = added.value(QStringLiteral("objectId")).toString();
    const QRectF originalFrame = added.value(QStringLiteral("frameRect")).toRectF();
    const qreal movedX = originalFrame.right() + 0.03;
    QVERIFY(movedX + originalFrame.width() < 1.0);
    const QRectF movedFrame(
        movedX,
        originalFrame.y() + 0.08,
        originalFrame.width(),
        originalFrame.height());

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();

    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.selectedContentBounds(), movedFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);
    QCOMPARE(
        controller.candidateAt(0, movedFrame.center().x(), movedFrame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);
    QVERIFY(controller.candidateAt(
        0,
        originalFrame.center().x(),
        originalFrame.center().y()).value(QStringLiteral("objectId")).toString()
        != objectId);

    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
}

void PDFEditControllerTest::movesTopLevelImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-image-source.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    const QString objectId = imageCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF originalFrame = imageCandidate
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const qreal movedX = qMax<qreal>(0.0, originalFrame.x() - 0.3);
    const qreal movedY = qMin<qreal>(
        1.0 - originalFrame.height(),
        originalFrame.y() + 0.12);
    const QRectF requestedFrame(
        movedX,
        movedY,
        originalFrame.width(),
        originalFrame.height());
    const QRectF movedFrame(
        movedX,
        movedY,
        originalFrame.width(),
        originalFrame.height());
    QVERIFY(movedFrame != originalFrame);

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    QVERIFY(controller.textEditActive());
    controller.updateSelectedFrameRect(requestedFrame);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.selectedContentBounds(), movedFrame);
    controller.finishSelectedFrameResize();

    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 0);
    const QVector<PDFEditController::DraftObjectMove> moves
        = controller.draftedObjectMovesForPage(0);
    QCOMPARE(moves.size(), 1);
    QCOMPARE(moves.constFirst().kind, PDFEditController::DraftObjectMove::Kind::Image);
    QCOMPARE(moves.constFirst().sourceRect, originalFrame);
    QCOMPARE(moves.constFirst().frameRect, movedFrame);
    QCOMPARE(moves.constFirst().objectPath.size(), 1);
    QCOMPARE(
        controller.candidateAt(0, movedFrame.center().x(), movedFrame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);
    QVERIFY(controller.candidateAt(
        0,
        originalFrame.center().x(),
        originalFrame.center().y()).value(QStringLiteral("objectId")).toString()
        != objectId);

    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);

    controller.redo();
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 1);
}

void PDFEditControllerTest::resizesTopLevelImageWithHistoryAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("resize-image-history.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    const QString objectId = candidate.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectCandidate(0, objectId));
    QVERIFY(controller.selectedImageResizable());
    const QRectF originalFrame = controller.selectedFrameRect();
    const QRectF resizedFrame(0.2, 0.3,
                             originalFrame.width() * 1.5,
                             originalFrame.height() * 0.7);

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedSourceRect(), originalFrame);
    controller.cancelSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    // Returning to the starting frame is a no-op even after live previews.
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.updateSelectedFrameRect(originalFrame);
    controller.finishSelectedFrameResize();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.2, 0.3, 0.2, 0.2));
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
    const auto transforms = controller.draftedObjectMovesForPage(0);
    QCOMPARE(transforms.size(), 1);
    QCOMPARE(transforms.constFirst().kind, PDFEditController::DraftObjectMove::Kind::Image);
    QCOMPARE(transforms.constFirst().sourceRect, originalFrame);
    QCOMPARE(transforms.constFirst().frameRect, resizedFrame);
    QCOMPARE(controller.candidateAt(0, resizedFrame.center().x(), resizedFrame.center().y())
                 .value(QStringLiteral("objectId")).toString(), objectId);

    // Cancel a second gesture without deleting the first committed resize.
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.1, 0.1, 0.3, 0.3));
    controller.cancelSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QVERIFY(controller.hasDrafts());

    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(controller.draftedObjectMovesForPage(0).isEmpty());
}

void PDFEditControllerTest::keepsClippedSourceImagesMoveOnly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("clipped-image-resize.pdf"));
    const QByteArray content("q 110 120 90 40 re W n 120 0 0 60 100 100 cm /Im0 Do Q\n");
    const QByteArray pixels = QByteArray::fromHex("ff000000ff000000ffffffff");
    QVERIFY(writePdfObjects(sourcePath, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] "
                   "/Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n") + content + QByteArray("endstream"),
        QByteArray("<< /Type /XObject /Subtype /Image /Width 2 /Height 2 "
                   "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 12 >>\nstream\n")
            + pixels + QByteArray("\nendstream"),
    }));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(candidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(!candidate.value(QStringLiteral("imageResizable")).toBool());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!controller.selectedImageResizable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(
        sourceFrame.topLeft(), sourceFrame.size() * 0.5));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());

    const QRectF movedFrame = sourceFrame.translated(0.1, 0.1);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QVERIFY(controller.hasDrafts());
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());
    controller.deleteSelectedObject();
    QVERIFY(controller.hasDrafts());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::keepsOffPageSourceImagesMoveOnly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("off-page-image-resize.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath, {120, 0, 0, 60, -20, 620}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(candidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(!candidate.value(QStringLiteral("imageResizable")).toBool());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!controller.selectedImageResizable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(sourceFrame.topLeft(), sourceFrame.size() * 0.5));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());
    const QRectF movedFrame = sourceFrame.translated(0.1, 0.1);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.deleteSelectedObject();
    QVERIFY(controller.hasDrafts());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::keepsSingularSourceImagesMoveOnly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("singular-image-move.pdf"));
    const QString outputPath = sourcePath + QStringLiteral(".copy.pdf");
    // A rank-one image still has a nonempty axis-aligned selection frame.
    // Translation must remain available even though resizing is unsupported.
    QVERIFY(createTopLevelImagePdf(sourcePath, {120, 120, 60, 60, 100, 100}));
    const ImageSaveInspection original = inspectImages(sourcePath);
    QVERIFY(original.matrixReadable);
    QVERIFY(original.pixelsReadable);
    QCOMPARE(original.count, 1);
    QCOMPARE(original.matrix.a * original.matrix.d
        - original.matrix.b * original.matrix.c, 0.0F);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedEditable());
    QVERIFY(!controller.selectedImageResizable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    QVERIFY(!sourceFrame.isEmpty());
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(
        sourceFrame.topLeft(), sourceFrame.size() * 0.5));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());

    const QPointF delta(0.1, 0.1);
    const QRectF movedFrame = sourceFrame.translated(delta);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());

    const ImageSaveInspection saved = inspectImages(outputPath);
    QVERIFY(saved.matrixReadable);
    QVERIFY(saved.pixelsReadable);
    QCOMPARE(saved.count, original.count);
    QCOMPARE(saved.firstTopLevelIndex, original.firstTopLevelIndex);
    QCOMPARE(saved.pixelWidth, original.pixelWidth);
    QCOMPARE(saved.pixelHeight, original.pixelHeight);
    QCOMPARE(saved.pixels, original.pixels);
    QCOMPARE(saved.matrix.a, original.matrix.a);
    QCOMPARE(saved.matrix.b, original.matrix.b);
    QCOMPARE(saved.matrix.c, original.matrix.c);
    QCOMPARE(saved.matrix.d, original.matrix.d);
    QVERIFY(qAbs(saved.matrix.e - original.matrix.e - delta.x() * 595.0) < 0.001);
    QVERIFY(qAbs(saved.matrix.f - original.matrix.f + delta.y() * 842.0) < 0.001);
}

void PDFEditControllerTest::resizesSourceImageThroughQmlHandles()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The source image handle test needs a Qt Quick render screen.");
    }
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("image-qml-resize.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.imageAspectRatioLocked());
    const QRectF original = controller.selectedFrameRect();
    const qreal normalizedAspect = original.width() / original.height();

    const QString themePath = QFINDTESTDATA("../src/Components/AppTheme.qml");
    const QString overlayPath = QFINDTESTDATA("../src/Components/EditOverlay.qml");
    QVERIFY(registerQmlTestTypes(themePath));
    QQuickWindow window;
    window.resize(595, 842);
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(overlayPath));
    const QVariantMap properties{
        {QStringLiteral("page"), 0},
        {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), true},
        {QStringLiteral("draftTextureReady"), true},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), 1.0},
        {QStringLiteral("width"), 595.0},
        {QStringLiteral("height"), 842.0}};
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties(properties));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    auto *overlayItem = qobject_cast<QQuickItem *>(overlay.get());
    QVERIFY(overlayItem);
    overlayItem->setParentItem(window.contentItem());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

    // Locate real delegates from their directional data; do not duplicate the
    // QML resize algorithm or replace the controller with a mock.
    const auto findHandle = [&](auto &&self, QQuickItem *item,
                                int horizontal, int vertical) -> QQuickItem * {
        const QVariant raw = item->property("modelData");
        const QVariantMap data = raw.metaType() == QMetaType::fromType<QJSValue>()
            ? raw.value<QJSValue>().toVariant().toMap() : raw.toMap();
        if (data.contains(QStringLiteral("horizontal"))
            && data.value(QStringLiteral("horizontal")).toInt() == horizontal
            && data.value(QStringLiteral("vertical")).toInt() == vertical) {
            for (QQuickItem *child : item->childItems()) {
                if (child->metaObject()->indexOfProperty("acceptedButtons") >= 0) {
                    return child;
                }
            }
        }
        for (QQuickItem *child : item->childItems()) {
            if (QQuickItem *found = self(self, child, horizontal, vertical)) {
                return found;
            }
        }
        return nullptr;
    };
    const auto handle = [&](int horizontal, int vertical) {
        return findHandle(findHandle, overlayItem, horizontal, vertical);
    };
    const auto centerInWindow = [](QQuickItem *item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    };
    for (const QPoint direction : {
             QPoint(-1, -1), QPoint(0, -1), QPoint(1, -1), QPoint(1, 0),
             QPoint(1, 1), QPoint(0, 1), QPoint(-1, 1), QPoint(-1, 0)}) {
        QQuickItem *mouse = handle(direction.x(), direction.y());
        QVERIFY(mouse);
        QVERIFY(mouse->isVisible());
        const QPoint start = centerInWindow(mouse);
        const QPoint end = start + QPoint(direction.x() * 30, direction.y() * 20);
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&window, end);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
        QVERIFY(!controller.textEditActive());
        const QRectF resized = controller.selectedFrameRect();
        QVERIFY(resized.width() > original.width());
        QVERIFY(qAbs(resized.width() / resized.height() - normalizedAspect) < 0.000001);
        if (direction.x() < 0)
            QVERIFY(qAbs(resized.right() - original.right()) < 0.000001);
        else if (direction.x() > 0)
            QVERIFY(qAbs(resized.left() - original.left()) < 0.000001);
        else
            QVERIFY(qAbs(resized.center().x() - original.center().x()) < 0.000001);
        if (direction.y() < 0)
            QVERIFY(qAbs(resized.bottom() - original.bottom()) < 0.000001);
        else if (direction.y() > 0)
            QVERIFY(qAbs(resized.top() - original.top()) < 0.000001);
        else
            QVERIFY(qAbs(resized.center().y() - original.center().y()) < 0.000001);
        controller.undo();
        QCOMPARE(controller.selectedFrameRect(), original);
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
    }

    QQuickItem *rightHandle = handle(1, 0);
    QVERIFY(rightHandle);
    rightHandle->forceActiveFocus();
    QTest::keyClick(&window, Qt::Key_Right, Qt::ShiftModifier);
    const QRectF keyboardLocked = controller.selectedFrameRect();
    QVERIFY(qAbs(keyboardLocked.width() - original.width() - 10.0 / window.width()) < 0.000001);
    QVERIFY(qAbs(keyboardLocked.width() / keyboardLocked.height() - normalizedAspect) < 0.000001);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), original);

    // The dedicated image panel changes the session preference without
    // creating a PDF draft. Its unlocked handles change one dimension only.
    const QString panelPath = QFINDTESTDATA("../src/Components/ImagePropertiesPanel.qml");
    QVERIFY(!panelPath.isEmpty());
    QQuickWindow panelWindow;
    panelWindow.resize(260, 842);
    QQmlComponent panelComponent(&engine, QUrl::fromLocalFile(panelPath));
    const QVariantMap panelProperties{
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("editController"), QVariant::fromValue(&controller)},
        {QStringLiteral("collapsed"), false},
        {QStringLiteral("width"), 260.0},
        {QStringLiteral("height"), 842.0}};
    std::unique_ptr<QObject> panel(
        panelComponent.createWithInitialProperties(panelProperties));
    QVERIFY2(panel, qPrintable(panelComponent.errorString()));
    auto *panelItem = qobject_cast<QQuickItem *>(panel.get());
    QVERIFY(panelItem);
    panelItem->setParentItem(panelWindow.contentItem());
    panelWindow.show();
    QTRY_VERIFY_WITH_TIMEOUT(panelWindow.isExposed(), 5000);
    QPointer<QQuickItem> checkbox = panel->findChild<QQuickItem *>(
        QStringLiteral("imageAspectRatioLock"));
    QVERIFY(checkbox);
    QTRY_VERIFY(checkbox->isVisible());
    QVERIFY(checkbox->isEnabled());
    QVERIFY(checkbox->property("checked").toBool());
    for (QObject *child : overlay->findChildren<QObject *>()) {
        QVERIFY(child->property("text").toString() != QLatin1String("ImageAspectRatioLocked")
                || child->metaObject()->indexOfProperty("checked") < 0);
    }
    QTest::mouseClick(&panelWindow, Qt::LeftButton, Qt::NoModifier, centerInWindow(checkbox));
    QVERIFY(!controller.imageAspectRatioLocked());
    QVERIFY(!controller.hasDrafts());

    controller.setImageAspectRatioLocked(true);
    QAccessibleInterface *accessibleCheckbox = QAccessible::queryAccessibleInterface(checkbox);
    QVERIFY(accessibleCheckbox);
    QAccessibleActionInterface *checkboxActions = accessibleCheckbox->actionInterface();
    QVERIFY(checkboxActions);
    QVERIFY(checkboxActions->actionNames().contains(QAccessibleActionInterface::toggleAction()));
    checkboxActions->doAction(QAccessibleActionInterface::toggleAction());
    QVERIFY(!checkbox->property("checked").toBool());
    QVERIFY(!controller.imageAspectRatioLocked());
    QVERIFY(!controller.hasDrafts());
    panel->setProperty("collapsed", true);
    QTRY_VERIFY(checkbox.isNull());
    panel->setProperty("collapsed", false);
    checkbox = panel->findChild<QQuickItem *>(QStringLiteral("imageAspectRatioLock"));
    QVERIFY(checkbox);
    QTRY_VERIFY(checkbox->isVisible());
    QVERIFY(!checkbox->property("checked").toBool());
    QVERIFY(!controller.hasDrafts());
    panelWindow.hide();
    window.requestActivate();
    rightHandle = handle(1, 0);
    QVERIFY(rightHandle);
    rightHandle->forceActiveFocus();
    QTest::keyClick(&window, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(controller.selectedFrameRect().height(), original.height());
    QVERIFY(controller.selectedFrameRect().width() > original.width());
    controller.undo();

    rightHandle = handle(1, 0);
    QVERIFY(rightHandle);
    QPoint start = centerInWindow(rightHandle);
    QPoint end = start + QPoint(35, 50);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&window, end);
    QCOMPARE(controller.selectedFrameRect().height(), original.height());
    QVERIFY(controller.selectedFrameRect().width() > original.width());
    QTest::keyClick(&window, Qt::Key_Escape);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
    QCOMPARE(controller.selectedFrameRect(), original);
    QVERIFY(!controller.hasDrafts());

    controller.setImageAspectRatioLocked(true);
    for (const QPoint delta : {QPoint(1000, 1000), QPoint(-1000, -1000)}) {
        QQuickItem *cornerHandle = handle(1, 1);
        QVERIFY(cornerHandle);
        start = centerInWindow(cornerHandle);
        end = start + delta;
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&window, end);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
        const QRectF resized = controller.selectedFrameRect();
        QVERIFY(resized != original);
        QVERIFY(resized.right() <= 1.0 + 0.000001);
        QVERIFY(resized.bottom() <= 1.0 + 0.000001);
        QVERIFY(resized.width() * window.width() >= 24.0 - 0.000001);
        QVERIFY(resized.height() * window.height() >= 24.0 - 0.000001);
        QVERIFY(qAbs(resized.width() / resized.height() - normalizedAspect) < 0.000001);
        QCOMPARE(resized.topLeft(), original.topLeft());
        controller.undo();
        QCOMPARE(controller.selectedFrameRect(), original);
    }
}

void PDFEditControllerTest::savesResizedTopLevelImageRoundTrip_data()
{
    QTest::addColumn<QList<qreal>>("imageMatrix");
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("offsetCrop");
    QTest::addColumn<bool>("overwrite");
    const QList<QPair<QByteArray, QList<qreal>>> matrices{
        {"upright", {120, 0, 0, 60, 300, 500}},
        {"rotated-image", {0, 120, -60, 0, 360, 500}},
        {"skewed", {120, 25, 35, 60, 300, 500}},
        {"reflected", {-120, 25, 35, 60, 420, 500}},
    };
    for (const auto &entry : matrices) {
        for (int rotation = 0; rotation < 4; ++rotation) {
            for (const bool overwrite : {false, true}) {
                const QByteArray name = entry.first + "-crop-rotate-"
                    + QByteArray::number(rotation * 90)
                    + (overwrite ? "-save" : "-save-as");
                QTest::newRow(name.constData())
                    << entry.second << rotation << true << overwrite;
            }
        }
    }
    QTest::newRow("uncropped-save-as") << matrices.constFirst().second << 0 << false << false;
    QTest::newRow("uncropped-save") << matrices.constFirst().second << 0 << false << true;
}

void PDFEditControllerTest::savesResizedTopLevelImageRoundTrip()
{
    QFETCH(QList<qreal>, imageMatrix);
    QFETCH(int, rotation);
    QFETCH(bool, offsetCrop);
    QFETCH(bool, overwrite);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("resize-image-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = overwrite ? sourcePath : sourcePath + QStringLiteral(".copy.pdf");
    const FS_MATRIX matrix{
        float(imageMatrix[0]), float(imageMatrix[1]), float(imageMatrix[2]),
        float(imageMatrix[3]), float(imageMatrix[4]), float(imageMatrix[5])};
    QVERIFY(createTopLevelImagePdf(sourcePath, matrix, rotation, offsetCrop));
    const ImageSaveInspection original = inspectImages(sourcePath);
    QVERIFY(original.matrixReadable);
    QVERIFY(original.pixelsReadable);
    QCOMPARE(original.normalizedCorners.size(), 4);
    QVERIFY(original.topLeftPixel != original.bottomLeftPixel);
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray sourceBytes = sourceFile.readAll();
    sourceFile.close();

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageResizable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    const QRectF targetFrame(0.18, 0.23,
                            sourceFrame.width() * 1.35,
                            sourceFrame.height() * 0.65);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(targetFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), targetFrame);
    QVERIFY(overwrite ? controller.save()
                      : controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const ImageSaveInspection saved = inspectImages(outputPath);
    QVERIFY(saved.matrixReadable);
    QCOMPARE(saved.count, 1);
    QCOMPARE(saved.firstTopLevelIndex, original.firstTopLevelIndex);
    QCOMPARE(saved.pixelWidth, original.pixelWidth);
    QCOMPARE(saved.pixelHeight, original.pixelHeight);
    QCOMPARE(saved.pixels, original.pixels);
    QCOMPARE(saved.normalizedCorners.size(), 4);
    for (int index = 0; index < 4; ++index) {
        const QPointF corner = original.normalizedCorners[index];
        const QPointF expected(
            targetFrame.x() + (corner.x() - sourceFrame.x()) * 1.35,
            targetFrame.y() + (corner.y() - sourceFrame.y()) * 0.65);
        const QPointF actual = saved.normalizedCorners[index];
        QVERIFY2(QLineF(expected, actual).length() < 0.0001,
                 qPrintable(QStringLiteral("corner %1: expected (%2,%3), got (%4,%5)")
                     .arg(index).arg(expected.x()).arg(expected.y()).arg(actual.x()).arg(actual.y())));
    }
    const qreal originalDeterminant = original.matrix.a * original.matrix.d
        - original.matrix.b * original.matrix.c;
    const qreal savedDeterminant = saved.matrix.a * saved.matrix.d
        - saved.matrix.b * saved.matrix.c;
    QVERIFY(originalDeterminant * savedDeterminant > 0.0);
    QVERIFY(normalizedPageText(outputPath).contains(QString::fromLatin1(kImageFixtureText)));

    const QVariantMap reopenedCandidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!reopenedCandidate.isEmpty());
    const QRectF reopenedFrame = reopenedCandidate.value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(QLineF(reopenedFrame.topLeft(), targetFrame.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(reopenedFrame.bottomRight(), targetFrame.bottomRight()).length() < 0.0001);
    QVERIFY(reopenedCandidate.value(QStringLiteral("imageResizable")).toBool());
    if (!overwrite) {
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        QCOMPARE(sourceFile.readAll(), sourceBytes);
    }
}

void PDFEditControllerTest::restoresSourceImagePreviewAfterResize()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The source image preview test needs a Qt Quick render screen.");
    }
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("resize-preview-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("resize-preview-reference.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath, {-120, 25, 35, 60, 420, 500}, 1, true));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QRectF sourceFrame = controller.selectedFrameRect();
    const QRectF targetFrame(0.18, 0.23, sourceFrame.width() * 1.5, sourceFrame.height() * 0.75);

    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(650, 500);
    TestablePDFPageItem pageItem(window.contentItem());
    pageItem.setSize(window.size());
    pageItem.setEditController(&controller);
    pageItem.setDocument(&document);
    pageItem.setPage(0);
    pageItem.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    const auto grabPage = [&]() {
        QElapsedTimer timer;
        timer.start();
        while ((!pageItem.draftTextureCurrent() || pageItem.m_renderPending)
               && timer.elapsed() < 5000) {
            QTest::qWait(10);
        }
        return pageItem.draftTextureCurrent() && !pageItem.m_renderPending
            ? window.grabWindow() : QImage();
    };
    const QImage originalPreview = grabPage();
    QVERIFY(!originalPreview.isNull());

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(targetFrame);
    const QImage livePreview = grabPage();
    QVERIFY(!livePreview.isNull());
    QVERIFY(changedPixelCount(originalPreview, livePreview, livePreview.rect()) > 100);
    controller.finishSelectedFrameResize();
    const PDFSaveWriter::Result saved = PDFSaveWriter::save(controller, outputPath);
    QVERIFY2(saved.succeeded(), qPrintable(saved.detail));
    const QImage reference = renderFirstPage(outputPath, livePreview.size());
    QVERIFY(!reference.isNull());
    QVERIFY(changedPixelCount(reference, livePreview, reference.rect()) < reference.width() * reference.height() / 200);

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.4, 0.3, 0.3, 0.3));
    const QImage canceledPreview = grabPage();
    QVERIFY(!canceledPreview.isNull());
    QVERIFY(changedPixelCount(canceledPreview, livePreview, livePreview.rect()) > 100);
    controller.cancelSelectedFrameResize();
    QCOMPARE(grabPage(), livePreview);

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.4, 0.3, 0.3, 0.3));
    QVERIFY(!grabPage().isNull());
    controller.updateSelectedFrameRect(targetFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(grabPage(), livePreview);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(grabPage(), originalPreview);
    controller.redo();
    QCOMPARE(grabPage(), livePreview);
    controller.discardDrafts();
    QCOMPARE(grabPage(), originalPreview);
}

void PDFEditControllerTest::deletesTopLevelImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-image-source.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    QCOMPARE(imageCandidate.value(QStringLiteral("editable")).toBool(), true);
    const QString objectId = imageCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QPointF hitPoint = imageCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .center();
    const auto containsImage = [&objectId](const QVariantList &candidates) {
        return std::any_of(
            candidates.cbegin(),
            candidates.cend(),
            [&objectId](const QVariant &candidateValue) {
                const QVariantMap candidate = candidateValue.toMap();
                return candidate.value(QStringLiteral("kind")).toString()
                        == QLatin1String("image")
                    && candidate.value(QStringLiteral("objectId")).toString()
                        == objectId;
            });
    };

    QCOMPARE(
        controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedKind(), QStringLiteral("image"));
    QVERIFY(controller.selectedEditable());

    controller.deleteSelectedObject();

    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(controller.selectedObjectId(), QString());
    const QVariantList deletionDrafts = controller.draftsForPage(0);
    QCOMPARE(deletionDrafts.size(), 1);
    QVERIFY(deletionDrafts.constFirst().toMap()
        .value(QStringLiteral("deleted"))
        .toBool());
    QCOMPARE(
        deletionDrafts.constFirst().toMap()
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
    QVERIFY(controller.allDraftedTextObjectsSuppressibleForPage(0));
    QVERIFY(!containsImage(waitForCandidates(controller, 0)));
    QVERIFY(controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
    QVERIFY(!controller.selectCandidate(0, objectId));

    controller.undo();

    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 0);
    QVERIFY(containsImage(waitForCandidates(controller, 0)));
    QCOMPARE(
        controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(controller.selectCandidate(0, objectId));

    controller.redo();

    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
    QCOMPARE(controller.selectedObjectId(), QString());
    QVERIFY(!containsImage(waitForCandidates(controller, 0)));
    QVERIFY(controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
}

void PDFEditControllerTest::savesAddedImageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-image-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-image-input.png"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-image-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#2b5fd8")));
    for (int y = 0; y < image.height() / 2; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor(QStringLiteral("#d86f4b")));
        }
    }
    QVERIFY(image.save(imagePath, "PNG"));
    QCOMPARE(inspectImages(sourcePath).count, 0);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap added = controller.addImage(
        0,
        0.45,
        0.35,
        QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    const QRectF frame = added.value(QStringLiteral("frameRect")).toRectF();
    QVERIFY(QFile::remove(imagePath));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const ImageSaveInspection inspection = inspectImages(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.pixelWidth, 80U);
    QCOMPARE(inspection.pixelHeight, 40U);
    QVERIFY(inspection.pixelsReadable);
    QVERIFY(inspection.topLeftPixel.red() > inspection.topLeftPixel.blue());
    QVERIFY(inspection.bottomLeftPixel.blue() > inspection.bottomLeftPixel.red());
    QCOMPARE(inspectImages(sourcePath).count, 0);

    const QVariantList savedCandidates = waitForCandidates(controller, 0);
    const auto savedImage = std::find_if(
        savedCandidates.cbegin(),
        savedCandidates.cend(),
        [](const QVariant &value) {
            return value.toMap().value(QStringLiteral("kind")).toString()
                == QLatin1String("image");
        });
    QVERIFY(savedImage != savedCandidates.cend());
    const QRectF savedRect = savedImage->toMap()
        .value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(qAbs(savedRect.x() - frame.x()) < 0.001);
    QVERIFY(qAbs(savedRect.y() - frame.y()) < 0.001);
    QVERIFY(qAbs(savedRect.width() - frame.width()) < 0.001);
    QVERIFY(qAbs(savedRect.height() - frame.height()) < 0.001);
}

void PDFEditControllerTest::savesMovedTopLevelImageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-move-image-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-move-image-output.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    const ImageSaveInspection sourceInspection = inspectImages(sourcePath);
    QVERIFY(sourceInspection.readable);
    QVERIFY(sourceInspection.matrixReadable);
    QCOMPARE(sourceInspection.count, 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    const QRectF originalFrame = imageCandidate
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF movedFrame(
        qMax<qreal>(0.0, originalFrame.x() - 0.2),
        qMin<qreal>(1.0 - originalFrame.height(), originalFrame.y() + 0.1),
        originalFrame.width(),
        originalFrame.height());
    QVERIFY(movedFrame != originalFrame);
    QVERIFY(controller.selectCandidate(
        0,
        imageCandidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());

    const ImageSaveInspection savedInspection = inspectImages(outputPath);
    QVERIFY(savedInspection.readable);
    QVERIFY(savedInspection.matrixReadable);
    QCOMPARE(savedInspection.count, 1);
    QCOMPARE(savedInspection.firstTopLevelIndex, sourceInspection.firstTopLevelIndex);
    QCOMPARE(savedInspection.pixelWidth, sourceInspection.pixelWidth);
    QCOMPARE(savedInspection.pixelHeight, sourceInspection.pixelHeight);
    QCOMPARE(savedInspection.pixelsReadable, sourceInspection.pixelsReadable);
    QCOMPARE(savedInspection.topLeftPixel, sourceInspection.topLeftPixel);
    QCOMPARE(savedInspection.bottomLeftPixel, sourceInspection.bottomLeftPixel);
    QVERIFY(qAbs(savedInspection.matrix.a - sourceInspection.matrix.a) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.b - sourceInspection.matrix.b) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.c - sourceInspection.matrix.c) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.d - sourceInspection.matrix.d) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.e - sourceInspection.matrix.e) > 1.0F);
    QVERIFY(qAbs(savedInspection.matrix.f - sourceInspection.matrix.f) > 1.0F);
    QVERIFY(normalizedPageText(outputPath).contains(
        QString::fromLatin1(kImageFixtureText)));

    const QVariantMap savedCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY(!savedCandidate.isEmpty());
    const QRectF savedRect = savedCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    QVERIFY(qAbs(savedRect.x() - movedFrame.x()) < 0.001);
    QVERIFY(qAbs(savedRect.y() - movedFrame.y()) < 0.001);
    QVERIFY(qAbs(savedRect.width() - movedFrame.width()) < 0.001);
    QVERIFY(qAbs(savedRect.height() - movedFrame.height()) < 0.001);

    const ImageSaveInspection unchangedSourceInspection = inspectImages(sourcePath);
    QVERIFY(unchangedSourceInspection.matrixReadable);
    QCOMPARE(unchangedSourceInspection.matrix.e, sourceInspection.matrix.e);
    QCOMPARE(unchangedSourceInspection.matrix.f, sourceInspection.matrix.f);
}

void PDFEditControllerTest::savesDeletedTopLevelImageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-delete-image-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-delete-image-output.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    const ImageSaveInspection sourceBeforeSave = inspectImages(sourcePath);
    QVERIFY(sourceBeforeSave.readable);
    QCOMPARE(sourceBeforeSave.count, 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    QVERIFY(imageCandidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(controller.selectCandidate(
        0,
        imageCandidate.value(QStringLiteral("objectId")).toString()));
    controller.deleteSelectedObject();
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const ImageSaveInspection savedInspection = inspectImages(outputPath);
    QVERIFY(savedInspection.readable);
    QCOMPARE(savedInspection.count, 0);
    QVERIFY(normalizedPageText(outputPath).contains(
        QString::fromLatin1(kImageFixtureText)));

    const ImageSaveInspection unchangedSourceInspection = inspectImages(sourcePath);
    QVERIFY(unchangedSourceInspection.readable);
    QCOMPARE(unchangedSourceInspection.count, 1);
    QVERIFY(normalizedPageText(sourcePath).contains(
        QString::fromLatin1(kImageFixtureText)));
}

void PDFEditControllerTest::rotatesImagesWithHistory_data()
{
    QTest::addColumn<bool>("square");
    QTest::newRow("rectangular") << false;
    QTest::newRow("square") << true;
}

void PDFEditControllerTest::rotatesImagesWithHistory()
{
    QFETCH(bool, square);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("rotate-history-%1.pdf").arg(square));
    QVERIFY(createTopLevelImagePdf(sourcePath,
        {120, 0, 0, square ? 120.0F : 60.0F, 200, 400}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    const QString objectId = candidate.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectCandidate(0, objectId));
    QVERIFY(controller.selectedImageRotatable());
    const QRectF original = controller.selectedFrameRect();
    const QSizeF pageSize = document.pageSize(0);
    controller.rotateSelectedImage(1);
    const QRectF rotated = controller.selectedFrameRect();
    QVERIFY(QLineF(original.center(), rotated.center()).length() < 0.000001);
    QVERIFY(qAbs(rotated.width() * pageSize.width()
                 - original.height() * pageSize.height()) < 0.000001);
    QVERIFY(qAbs(rotated.height() * pageSize.height()
                 - original.width() * pageSize.width()) < 0.000001);
    QCOMPARE(controller.selectedContentBounds(), rotated);
    QCOMPARE(controller.selectedSourceRect(), original);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 1);
    const auto moves = controller.draftedObjectMovesForPage(0);
    QCOMPARE(moves.size(), 1);
    QCOMPARE(moves.constFirst().imageQuarterTurns, 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.textEditActive());

    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), original);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.draftedObjectMovesForPage(0).isEmpty());
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), rotated);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 1);

    // A full turn and an inverse turn both restore the source without leaving
    // an orientation-only draft, even when the frame never changes (square).
    for (int turn = 0; turn < 3; ++turn)
        controller.rotateSelectedImage(1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(QLineF(controller.selectedFrameRect().topLeft(), original.topLeft()).length() < 0.000001);
    controller.rotateSelectedImage(-1);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 3);
    controller.rotateSelectedImage(1);
    QVERIFY(!controller.hasDrafts());

    // A half turn is a real edit even though its selection frame is identical.
    controller.rotateSelectedImage(2);
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 2);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedFrameRect(), original);
}

void PDFEditControllerTest::savesRotatedSourceImageRoundTrip_data()
{
    QTest::addColumn<QList<qreal>>("imageMatrix");
    QTest::addColumn<int>("pageRotation");
    QTest::addColumn<int>("quarterTurns");
    QTest::addColumn<bool>("overwrite");
    const QList<QPair<QByteArray, QList<qreal>>> matrices{
        {"upright", {120, 0, 0, 60, 300, 500}},
        {"reflected-skew", {-120, 25, 35, 60, 420, 500}},
    };
    for (const auto &entry : matrices) {
        for (int pageRotation = 0; pageRotation < 4; ++pageRotation) {
            for (int turn : {1, -1, 2}) {
                const QByteArray name = entry.first + "-page-"
                    + QByteArray::number(pageRotation * 90) + "-turn-"
                    + QByteArray::number(turn);
                QTest::newRow(name.constData())
                    << entry.second << pageRotation << turn << (pageRotation % 2 == 0);
            }
        }
    }
}

void PDFEditControllerTest::savesRotatedSourceImageRoundTrip()
{
    QFETCH(QList<qreal>, imageMatrix);
    QFETCH(int, pageRotation);
    QFETCH(int, quarterTurns);
    QFETCH(bool, overwrite);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("rotate-source-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = overwrite ? sourcePath : sourcePath + QStringLiteral(".copy.pdf");
    QVERIFY(createTopLevelImagePdf(sourcePath,
        {float(imageMatrix[0]), float(imageMatrix[1]), float(imageMatrix[2]),
         float(imageMatrix[3]), float(imageMatrix[4]), float(imageMatrix[5])},
        pageRotation, true));
    const ImageSaveInspection original = inspectImages(sourcePath);
    QVERIFY(original.matrixReadable);
    QVERIFY(original.pixelsReadable);
    QCOMPARE(original.normalizedCorners.size(), 4);
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = sourceFile.readAll();
    sourceFile.close();
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageRotatable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    const QSizeF pageSize = document.pageSize(0);
    controller.rotateSelectedImage(quarterTurns);
    const QRectF rotatedFrame = controller.selectedFrameRect();
    // Subsequent movement and nonuniform resizing retain the chosen orientation.
    const QRectF targetFrame(0.18, 0.23,
                            rotatedFrame.width() * 1.2,
                            rotatedFrame.height() * 0.75);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(targetFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), targetFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).constFirst().imageQuarterTurns,
             (quarterTurns + 4) % 4);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), rotatedFrame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), targetFrame);
    QVERIFY(overwrite ? controller.save() : controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    const ImageSaveInspection saved = inspectImages(outputPath);
    QVERIFY(saved.matrixReadable);
    QCOMPARE(saved.count, original.count);
    QCOMPARE(saved.firstTopLevelIndex, original.firstTopLevelIndex);
    QCOMPARE(saved.pixelWidth, original.pixelWidth);
    QCOMPARE(saved.pixelHeight, original.pixelHeight);
    QCOMPARE(saved.pixels, original.pixels);
    QCOMPARE(saved.normalizedCorners.size(), 4);
    // Use a physical page-space rotation as the oracle; normalized x/y have
    // different scales on these nonsquare cropped and intrinsically rotated pages.
    const QTransform rotation = QTransform().rotate(quarterTurns * 90);
    for (int index = 0; index < original.normalizedCorners.size(); ++index) {
        const QPointF delta = original.normalizedCorners[index] - sourceFrame.center();
        const QPointF rotated = rotation.map(QPointF(
            delta.x() * pageSize.width(), delta.y() * pageSize.height()));
        const QPointF expected = targetFrame.center() + QPointF(
            rotated.x() / pageSize.width() * 1.2,
            rotated.y() / pageSize.height() * 0.75);
        QVERIFY2(QLineF(expected, saved.normalizedCorners[index]).length() < 0.0001,
                 qPrintable(QStringLiteral("corner %1: expected (%2,%3), got (%4,%5)")
                     .arg(index).arg(expected.x()).arg(expected.y())
                     .arg(saved.normalizedCorners[index].x()).arg(saved.normalizedCorners[index].y())));
    }
    QVERIFY(normalizedPageText(outputPath).contains(QString::fromLatin1(kImageFixtureText)));
    const QVariantMap reopened = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!reopened.isEmpty());
    const QRectF savedFrame = reopened.value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(QLineF(savedFrame.topLeft(), targetFrame.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(savedFrame.bottomRight(), targetFrame.bottomRight()).length() < 0.0001);
    QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageRotatable());
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 0);
    if (!overwrite) {
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        QCOMPARE(sourceFile.readAll(), originalBytes);
    }
}

void PDFEditControllerTest::savesRotatedAddedImageRoundTrip_data()
{
    QTest::addColumn<int>("quarterTurns");
    QTest::newRow("clockwise") << 1;
    QTest::newRow("counterclockwise") << -1;
    QTest::newRow("half-turn") << 2;
}

void PDFEditControllerTest::savesRotatedAddedImageRoundTrip()
{
    QFETCH(int, quarterTurns);
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-added-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-added-input.png"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("rotate-added-%1.pdf").arg(quarterTurns));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    QPainter imagePainter(&image);
    imagePainter.fillRect(QRect(0, 0, 40, 20), Qt::red);
    imagePainter.fillRect(QRect(40, 0, 40, 20), Qt::green);
    imagePainter.fillRect(QRect(0, 20, 40, 20), Qt::blue);
    imagePainter.fillRect(QRect(40, 20, 40, 20), Qt::yellow);
    imagePainter.end();
    QVERIFY(image.save(imagePath, "PNG"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap added = controller.addImage(0, 0.45, 0.5, QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    QVERIFY(controller.selectedImageRotatable());
    const QRectF initialFrame = controller.selectedFrameRect();
    controller.rotateSelectedImage(quarterTurns);
    const QRectF frame = controller.selectedFrameRect();
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(),
             (quarterTurns + 4) % 4);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), initialFrame);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), frame);
    QVERIFY(QFile::remove(imagePath));

    const QImage expected = image.transformed(QTransform().rotate(quarterTurns * 90));
    DraftImageItem item;
    item.setWidth(expected.width());
    item.setHeight(expected.height());
    item.setSourceImage(QVariant::fromValue(image));
    item.setImageQuarterTurns((quarterTurns + 4) % 4);
    QImage preview(expected.size(), QImage::Format_ARGB32);
    preview.fill(Qt::transparent);
    QPainter painter(&preview);
    item.paint(&painter);
    painter.end();
    QCOMPARE(preview, expected);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const ImageSaveInspection saved = inspectImages(outputPath);
    QCOMPARE(saved.count, 1);
    QCOMPARE(saved.pixelWidth, 80U);
    QCOMPARE(saved.pixelHeight, 40U);
    QCOMPARE(saved.pixels, image);
    QCOMPARE(inspectImages(sourcePath).count, 0);
    const QImage rendered = renderFirstPage(outputPath, QSize(1190, 1684));
    QVERIFY(!rendered.isNull());
    for (const QPointF point : {QPointF(0.25, 0.25), QPointF(0.75, 0.25),
                                QPointF(0.25, 0.75), QPointF(0.75, 0.75)}) {
        const QColor expectedColor = expected.pixelColor(
            int(point.x() * expected.width()), int(point.y() * expected.height()));
        const QColor actualColor = rendered.pixelColor(
            qRound((frame.x() + point.x() * frame.width()) * rendered.width()),
            qRound((frame.y() + point.y() * frame.height()) * rendered.height()));
        QCOMPARE(actualColor, expectedColor);
    }
    const QVariantMap reopened = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!reopened.isEmpty());
    QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageRotatable());
    const QRectF reopenedFrame = controller.selectedFrameRect();
    QVERIFY(QLineF(reopenedFrame.topLeft(), frame.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(reopenedFrame.bottomRight(), frame.bottomRight()).length() < 0.0001);
}

void PDFEditControllerTest::rejectsUnsupportedImageRotation_data()
{
    QTest::addColumn<QString>("fixture");
    for (const char *fixture : {"clipped", "nested", "singular", "off-page", "too-tall"})
        QTest::newRow(fixture) << QString::fromLatin1(fixture);
}

void PDFEditControllerTest::rejectsUnsupportedImageRotation()
{
    QFETCH(QString, fixture);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("rotate-unsupported-%1.pdf").arg(fixture));
    if (fixture == QLatin1String("clipped") || fixture == QLatin1String("nested")) {
        const bool nested = fixture == QLatin1String("nested");
        const QByteArray content = nested ? QByteArray("/Fm0 Do\n")
            : QByteArray("q 110 120 90 40 re W n 120 0 0 60 100 100 cm /Im0 Do Q\n");
        const QByteArray formContent("q 120 0 0 60 100 100 cm /Im0 Do Q\n");
        QList<QByteArray> objects{
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] ")
                + (nested ? QByteArray("/Resources << /XObject << /Fm0 6 0 R >> >> ")
                          : QByteArray("/Resources << /XObject << /Im0 5 0 R >> >> "))
                + QByteArray("/Contents 4 0 R >>"),
            QByteArray("<< /Length ") + QByteArray::number(content.size())
                + QByteArray(" >>\nstream\n") + content + QByteArray("endstream"),
            QByteArray("<< /Type /XObject /Subtype /Image /Width 2 /Height 2 ")
                + QByteArray("/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 12 >>\nstream\n")
                + QByteArray::fromHex("ff000000ff000000ffffffff") + QByteArray("\nendstream"),
        };
        if (nested) {
            objects.append(QByteArray("<< /Type /XObject /Subtype /Form /BBox [0 0 400 400] ")
                + QByteArray("/Resources << /XObject << /Im0 5 0 R >> >> /Length ")
                + QByteArray::number(formContent.size()) + QByteArray(" >>\nstream\n")
                + formContent + QByteArray("endstream"));
        }
        QVERIFY(writePdfObjects(sourcePath, objects));
    } else {
        const FS_MATRIX matrix = fixture == QLatin1String("singular")
            ? FS_MATRIX{120, 120, 60, 60, 100, 100}
            : fixture == QLatin1String("off-page")
                ? FS_MATRIX{120, 0, 0, 60, -20, 620}
                : FS_MATRIX{60, 0, 0, 700, 100, 100};
        QVERIFY(createTopLevelImagePdf(sourcePath, matrix));
    }
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.rotateSelectedImage(1);
    QVERIFY(!controller.hasDrafts());
    const QVariantMap candidate = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!controller.selectedImageRotatable());
    const QRectF original = controller.selectedFrameRect();
    controller.rotateSelectedImage(1);
    controller.rotateSelectedImage(-1);
    QCOMPARE(controller.selectedFrameRect(), original);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.draftedObjectMovesForPage(0).isEmpty());
}

void PDFEditControllerTest::keepsRotatedImageInsidePage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-near-edge.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath, {160, 0, 0, 60, 400, 0}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageRotatable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    controller.rotateSelectedImage(1);
    const QRectF rotated = controller.selectedFrameRect();
    QVERIFY(rotated.left() >= 0.0);
    QVERIFY(rotated.top() >= 0.0);
    QVERIFY(rotated.right() <= 1.0 + 0.000001);
    QVERIFY(rotated.bottom() <= 1.0 + 0.000001);
    // Candidate bounds are quantized through PDFium's integer page projection.
    // Rotation must preserve those measured physical dimensions exactly.
    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(qAbs(rotated.width() * pageSize.width()
                 - sourceFrame.height() * pageSize.height()) < 0.000001);
    QVERIFY(qAbs(rotated.height() * pageSize.height()
                 - sourceFrame.width() * pageSize.width()) < 0.000001);
    QVERIFY(qAbs(rotated.bottom() - 1.0) < 0.000001);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::restoresSourceImagePreviewAfterRotation()
{
    if (QGuiApplication::screens().isEmpty())
        QSKIP("The source image rotation preview test needs a Qt Quick render screen.");
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-preview-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("rotate-preview-reference.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath, {-120, 25, 35, 60, 420, 500}, 1, true));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(650, 500);
    TestablePDFPageItem pageItem(window.contentItem());
    pageItem.setSize(window.size());
    pageItem.setEditController(&controller);
    pageItem.setDocument(&document);
    pageItem.setPage(0);
    pageItem.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    const auto grabPage = [&]() {
        QElapsedTimer timer;
        timer.start();
        while ((!pageItem.draftTextureCurrent() || pageItem.m_renderPending)
               && timer.elapsed() < 5000)
            QTest::qWait(10);
        return pageItem.draftTextureCurrent() && !pageItem.m_renderPending
            ? window.grabWindow() : QImage();
    };
    const QImage original = grabPage();
    QVERIFY(!original.isNull());
    controller.rotateSelectedImage(1);
    const QImage preview = grabPage();
    QVERIFY(!preview.isNull());
    QVERIFY(changedPixelCount(original, preview, preview.rect()) > 100);
    const PDFSaveWriter::Result result = PDFSaveWriter::save(controller, outputPath);
    QVERIFY2(result.succeeded(), qPrintable(result.detail));
    const QImage saved = renderFirstPage(outputPath, preview.size());
    QVERIFY(!saved.isNull());
    QVERIFY(changedPixelCount(saved, preview, saved.rect()) < saved.width() * saved.height() / 200);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(grabPage(), original);
    controller.redo();
    QCOMPARE(grabPage(), preview);
    controller.discardDrafts();
    QCOMPARE(grabPage(), original);
}

void PDFEditControllerTest::rotatesImageThroughPropertiesPanel()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-panel.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QRectF original = controller.selectedFrameRect();

    const QString themePath = QFINDTESTDATA("../src/Components/AppTheme.qml");
    const QString panelPath = QFINDTESTDATA("../src/Components/ImagePropertiesPanel.qml");
    QVERIFY(registerQmlTestTypes(themePath));
    QQuickWindow window;
    window.resize(264, 500);
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(panelPath));
    const QVariantMap properties{
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("editController"), QVariant::fromValue(&controller)},
        {QStringLiteral("collapsed"), false},
        {QStringLiteral("width"), 264.0},
        {QStringLiteral("height"), 500.0}};
    std::unique_ptr<QObject> panel(component.createWithInitialProperties(properties));
    QVERIFY2(panel, qPrintable(component.errorString()));
    auto *panelItem = qobject_cast<QQuickItem *>(panel.get());
    QVERIFY(panelItem);
    panelItem->setParentItem(window.contentItem());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    auto *left = panel->findChild<QQuickItem *>(QStringLiteral("rotateImageLeftButton"));
    auto *right = panel->findChild<QQuickItem *>(QStringLiteral("rotateImageRightButton"));
    QVERIFY(left);
    QVERIFY(right);
    QTRY_VERIFY(left->isVisible() && right->isVisible());
    QVERIFY(left->isEnabled() && right->isEnabled());
    const auto center = [](QQuickItem *item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    };
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center(right));
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 1);
    QVERIFY(controller.hasDrafts());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center(left));
    QCOMPARE(controller.selectedFrameRect(), original);
    QVERIFY(!controller.hasDrafts());
    controller.undo();
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 1);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), original);
    controller.clearSelection();
    QTRY_VERIFY(!left->isEnabled() && !right->isEnabled());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center(right));
    QVERIFY(!controller.hasDrafts());
}
