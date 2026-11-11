#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFTableCellMetadata.h"
#include "PDFTableFrameRecognition.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QSet>
#include <QTranslator>
#include <cmath>

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::addsShapeWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-shape-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::ArrowShape);
    controller.setTool(PDFEditController::AddShapeTool);

    const QPointF startPoint(0.18, 0.42);
    const QPointF endPoint(0.62, 0.425);
    const QPointF constrainedEnd = controller.constrainedShapeEnd(
        0,
        startPoint,
        endPoint);
    QCOMPARE(constrainedEnd.x(), endPoint.x());
    QCOMPARE(constrainedEnd.y(), startPoint.y());
    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(controller.shapeDragIsValid(0, startPoint, endPoint));
    QVERIFY(!controller.shapeDragIsValid(
        0,
        startPoint,
        QPointF(
            startPoint.x() + 2.0 / pageSize.width(),
            startPoint.y())));
    const QVariantMap added = controller.addShape(
        0,
        startPoint,
        endPoint);
    QVERIFY(!added.isEmpty());
    QCOMPARE(
        added.value(QStringLiteral("kind")).toString(),
        QStringLiteral("addedShape"));
    QCOMPARE(
        added.value(QStringLiteral("shapeType")).toInt(),
        static_cast<int>(PDFEditController::ArrowShape));
    QCOMPARE(
        added.value(QStringLiteral("shapeStrokeColor")).value<QColor>(),
        QColor(Qt::black));
    QCOMPARE(
        added.value(QStringLiteral("shapeFillColor")).value<QColor>(),
        QColor(Qt::transparent));
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(controller.draftsForPage(0).size(), 1);

    const QString objectId = added
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF frame = added
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QPointF shapeStart = added
        .value(QStringLiteral("shapeStart"))
        .toPointF();
    const QPointF shapeEnd = added
        .value(QStringLiteral("shapeEnd"))
        .toPointF();
    QVERIFY(frame.contains(startPoint));
    QVERIFY(frame.contains(constrainedEnd));
    QVERIFY(qAbs(shapeStart.y() - 0.5) < 0.001);
    QVERIFY(qAbs(shapeEnd.y() - 0.5) < 0.001);
    const QPointF savedStart(
        frame.x() + shapeStart.x() * frame.width(),
        frame.y() + shapeStart.y() * frame.height());
    const QPointF savedEnd(
        frame.x() + shapeEnd.x() * frame.width(),
        frame.y() + shapeEnd.y() * frame.height());
    QVERIFY(QLineF(savedStart, startPoint).length() < 0.00001);
    QVERIFY(QLineF(savedEnd, constrainedEnd).length() < 0.00001);
    const qreal strokeWidthRatio = added
        .value(QStringLiteral("shapeStrokeWidthRatio"))
        .toReal();
    const qreal hitTolerancePoints = controller.shapeHitToleranceRatio(
        0,
        strokeWidthRatio) * pageSize.height();
    QVERIFY(qAbs(hitTolerancePoints - 3.0) < 0.001);
    const qreal strokeWidthPoints = qMax<qreal>(
        0.75,
        strokeWidthRatio * pageSize.height());
    const qreal lineLengthPoints = QLineF(
        QPointF(
            startPoint.x() * pageSize.width(),
            startPoint.y() * pageSize.height()),
        QPointF(
            constrainedEnd.x() * pageSize.width(),
            constrainedEnd.y() * pageSize.height())).length();
    const qreal arrowHalfWidth = qMin(
        qMax<qreal>(6.0, strokeWidthPoints * 4.0),
        lineLengthPoints * 0.35) * 0.55;
    QVERIFY(frame.height() * pageSize.height()
        >= 2.0 * (arrowHalfWidth + strokeWidthPoints / 2.0) - 0.001);
    QCOMPARE(controller.selectedObjectId(), objectId);
    QCOMPARE(
        controller.candidateAt(0, frame.center().x(), frame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);

    const QRectF resizedFrame(
        frame.x() + 0.04,
        frame.y() + 0.05,
        frame.width() * 0.75,
        frame.height() * 2.0);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);

    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);

    controller.deleteSelectedObject();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.candidateAt(
        0,
        resizedFrame.center().x(),
        resizedFrame.center().y()).isEmpty());

    controller.undo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);

    controller.setShapeType(PDFEditController::LineShape);
    controller.setTool(PDFEditController::AddShapeTool);
    const QVariantMap diagonalLine = controller.addShape(
        0,
        QPointF(0.10, 0.10),
        QPointF(0.50, 0.50));
    QVERIFY(!diagonalLine.isEmpty());
    const QString diagonalLineId = diagonalLine
        .value(QStringLiteral("objectId"))
        .toString();
    QCOMPARE(
        controller.candidateAt(0, 0.30, 0.30)
            .value(QStringLiteral("objectId")).toString(),
        diagonalLineId);
    QVERIFY(
        controller.candidateAt(0, 0.10, 0.50)
            .value(QStringLiteral("objectId")).toString()
        != diagonalLineId);

    controller.setShapeType(PDFEditController::DiamondShape);
    controller.setTool(PDFEditController::AddShapeTool);
    const QVariantMap transparentDiamond = controller.addShape(
        0,
        QPointF(0.65, 0.10),
        QPointF(0.85, 0.30));
    QVERIFY(!transparentDiamond.isEmpty());
    const QString transparentDiamondId = transparentDiamond
        .value(QStringLiteral("objectId"))
        .toString();
    QCOMPARE(
        controller.candidateAt(0, 0.75, 0.20)
            .value(QStringLiteral("objectId")).toString(),
        transparentDiamondId);
    QCOMPARE(
        controller.candidateAt(0, 0.75, 0.10)
            .value(QStringLiteral("objectId")).toString(),
        transparentDiamondId);

    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.draftsForPage(0).isEmpty());
}

void PDFEditControllerTest::savesShapesRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-shapes-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-shapes-output.pdf"));
    QVERIFY(createOffsetCropPdf(sourcePath));
    const ShapeSaveInspection sourceInspection
        = inspectShapePaths(sourcePath);
    QVERIFY(sourceInspection.readable);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    struct ShapeInput {
        PDFEditController::ShapeType type;
        QPointF start;
        QPointF end;
    };
    const QVector<ShapeInput> shapes{
        {PDFEditController::RectangleShape, {0.08, 0.12}, {0.28, 0.24}},
        {PDFEditController::EllipseShape, {0.38, 0.12}, {0.58, 0.24}},
        {PDFEditController::LineShape, {0.68, 0.22}, {0.88, 0.12}},
        {PDFEditController::ArrowShape, {0.08, 0.38}, {0.28, 0.385}},
        {PDFEditController::TriangleShape, {0.38, 0.30}, {0.58, 0.42}},
        {PDFEditController::DiamondShape, {0.68, 0.30}, {0.88, 0.42}},
    };
    QVector<QPointF> expectedEnds;
    expectedEnds.reserve(shapes.size());
    for (const ShapeInput &shape : shapes) {
        controller.setShapeType(shape.type);
        controller.setTool(PDFEditController::AddShapeTool);
        expectedEnds.append(controller.constrainedShapeEnd(
            0,
            shape.start,
            shape.end));
        QVERIFY(!controller.addShape(
            0,
            shape.start,
            shape.end).isEmpty());
    }
    QCOMPARE(controller.draftsForPage(0).size(), shapes.size());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const ShapeSaveInspection savedInspection
        = inspectShapePaths(outputPath);
    QVERIFY(savedInspection.readable);
    QCOMPARE(
        savedInspection.count,
        sourceInspection.count + shapes.size());
    QVERIFY(savedInspection.filledCount
        >= sourceInspection.filledCount + 4);
    QVERIFY(savedInspection.openCount
        >= sourceInspection.openCount + 2);
    QVERIFY(savedInspection.bezierCount
        >= sourceInspection.bezierCount + 1);
    QVERIFY(savedInspection.blackStrokeCount
        >= sourceInspection.blackStrokeCount + shapes.size());
    QVERIFY(savedInspection.transparentFillCount
        >= sourceInspection.transparentFillCount + 4);
    const auto verifyOpenPathEndpoints = [&savedInspection,
                                          &sourceInspection,
                                          &shapes,
                                          &expectedEnds](
                                             qsizetype shapeIndex) {
        const qsizetype pathIndex = sourceInspection.count + shapeIndex;
        QVERIFY(pathIndex
            < savedInspection.normalizedPathPoints.size());
        const QVector<QPointF> &points
            = savedInspection.normalizedPathPoints.at(pathIndex);
        QVERIFY(points.size() >= 2);
        QVERIFY(QLineF(points.at(0), shapes.at(shapeIndex).start).length()
            < 0.002);
        QVERIFY(QLineF(points.at(1), expectedEnds.at(shapeIndex)).length()
            < 0.002);
    };
    verifyOpenPathEndpoints(2);
    verifyOpenPathEndpoints(3);

    const QString verificationPath = qEnvironmentVariable(
        "TREPDF_SHAPE_TEST_OUTPUT");
    if (!verificationPath.isEmpty()) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(outputPath, verificationPath));
    }
}

void PDFEditControllerTest::editsShapePropertiesWithUndoRedo_data()
{
    QTest::addColumn<int>("shapeType");
    QTest::addColumn<bool>("supportsFill");
    QTest::newRow("rectangle") << int(PDFEditController::RectangleShape) << true;
    QTest::newRow("oval") << int(PDFEditController::EllipseShape) << true;
    QTest::newRow("line") << int(PDFEditController::LineShape) << false;
    QTest::newRow("arrow") << int(PDFEditController::ArrowShape) << false;
    QTest::newRow("triangle") << int(PDFEditController::TriangleShape) << true;
    QTest::newRow("diamond") << int(PDFEditController::DiamondShape) << true;
}

void PDFEditControllerTest::editsShapePropertiesWithUndoRedo()
{
    QFETCH(int, shapeType);
    QFETCH(bool, supportsFill);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("shape-properties-%1.pdf").arg(shapeType));
    QVERIFY(createOffsetCropPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setShapeType(static_cast<PDFEditController::ShapeType>(shapeType));
    controller.setTool(PDFEditController::AddShapeTool);
    const QVariantMap added = controller.addShape(
        0, QPointF(0.2, 0.35), QPointF(0.6, 0.7));
    QVERIFY(!added.isEmpty());
    const QString objectId = added.value(QStringLiteral("objectId")).toString();
    const QRectF frame = controller.selectedFrameRect();
    QVERIFY(controller.selectedDrawingPropertiesEditable());
    QCOMPARE(controller.selectedDrawingSupportsFill(), supportsFill);
    QCOMPARE(controller.selectedDrawingStrokeColor(), QColor(Qt::black));
    QCOMPARE(controller.selectedDrawingOpacity(), 1.0);
    QVERIFY(!controller.selectedDrawingHasInterior());

    const auto verifyInteriorHit = [&controller, &frame, &objectId]() {
        const QVariantList draftsBefore = controller.draftsForPage(0);
        QSignalSpy historyChanges(&controller, &PDFEditController::historyStateChanged);
        controller.clearSelection();
        const QVariantMap hovered = controller.candidateAt(
            0, frame.center().x(), frame.center().y());
        QCOMPARE(hovered.value(QStringLiteral("objectId")).toString(), objectId);
        QVERIFY(controller.selectCandidate(
            0, hovered.value(QStringLiteral("objectId")).toString()));
        QCOMPARE(controller.draftsForPage(0), draftsBefore);
        QCOMPARE(historyChanges.count(), 0);
    };
    if (supportsFill) {
        // Hover and click use the closed shape's interior even without fill,
        // and neither operation should materialize a style/history change.
        verifyInteriorHit();
        QVERIFY(!controller.selectedDrawingHasInterior());
        if (shapeType != PDFEditController::RectangleShape) {
            const QPointF outsideContour(
                frame.left() + frame.width() * 0.05,
                frame.top() + frame.height() * 0.05);
            QVERIFY(controller.candidateAt(0, outsideContour.x(), outsideContour.y())
                        .value(QStringLiteral("objectId")).toString() != objectId);
        }
    } else {
        const QPointF offStroke(
            frame.left() + frame.width() * 0.15,
            frame.top() + frame.height() * 0.85);
        QVERIFY(controller.candidateAt(0, offStroke.x(), offStroke.y())
                    .value(QStringLiteral("objectId")).toString() != objectId);
        QCOMPARE(controller.candidateAt(0, frame.center().x(), frame.center().y())
                     .value(QStringLiteral("objectId")).toString(), objectId);
    }

    const QColor stroke(29, 101, 183);
    QVERIFY(controller.setSelectedDrawingStrokeColor(QColor(29, 101, 183, 17)));
    QCOMPARE(controller.selectedDrawingStrokeColor(), stroke);
    QVERIFY(controller.setSelectedDrawingStrokeColor(stroke));
    QVERIFY(!controller.setSelectedDrawingStrokeColor(QColor()));
    QVERIFY(!controller.setSelectedDrawingInteriorColor(QColor()));
    QVERIFY(!controller.setSelectedDrawingOpacity(
        std::numeric_limits<qreal>::quiet_NaN()));
    QVERIFY(!controller.setSelectedDrawingOpacity(
        std::numeric_limits<qreal>::infinity()));
    controller.undo();
    QCOMPARE(controller.selectedDrawingStrokeColor(), QColor(Qt::black));
    controller.redo();
    QCOMPARE(controller.selectedDrawingStrokeColor(), stroke);

    const QColor interior(223, 164, 38);
    if (supportsFill) {
        QVERIFY(controller.setSelectedDrawingInteriorColor(QColor(223, 164, 38, 3)));
        QCOMPARE(controller.selectedDrawingInteriorColor(), interior);
        QVERIFY(controller.selectedDrawingHasInterior());
        QCOMPARE(controller.candidateAt(0, frame.center().x(), frame.center().y())
                     .value(QStringLiteral("objectId")).toString(), objectId);
        QVERIFY(controller.setSelectedDrawingInteriorColor(interior));
        controller.undo();
        QVERIFY(!controller.selectedDrawingHasInterior());
        verifyInteriorHit();
        controller.redo();
        QCOMPARE(controller.selectedDrawingInteriorColor(), interior);
    } else {
        QVERIFY(!controller.setSelectedDrawingInteriorColor(interior));
        QVERIFY(!controller.clearSelectedDrawingInteriorColor());
        QVERIFY(!controller.selectedDrawingHasInterior());
    }

    QVERIFY(controller.setSelectedDrawingOpacity(0.4));
    QVERIFY(controller.setSelectedDrawingOpacity(0.4));
    QCOMPARE(controller.selectedDrawingOpacity(), 0.4);
    const QVariantMap preview = controller.selectedDraftPreview();
    QColor effectiveStroke = stroke;
    effectiveStroke.setAlphaF(0.4);
    QCOMPARE(preview.value(QStringLiteral("shapeStrokeColor")).value<QColor>(),
             effectiveStroke);
    if (supportsFill) {
        QColor effectiveInterior = interior;
        effectiveInterior.setAlphaF(0.4);
        QCOMPARE(preview.value(QStringLiteral("shapeFillColor")).value<QColor>(),
                 effectiveInterior);
    }
    controller.undo();
    QCOMPARE(controller.selectedDrawingOpacity(), 1.0);
    QCOMPARE(controller.selectedDrawingStrokeColor(), stroke);
    controller.redo();
    QCOMPARE(controller.selectedDrawingOpacity(), 0.4);

    QVERIFY(controller.setSelectedDrawingOpacity(-1.0));
    QCOMPARE(controller.selectedDrawingOpacity(), 0.0);
    if (supportsFill) {
        verifyInteriorHit();
    }
    QVERIFY(controller.setSelectedDrawingOpacity(2.0));
    QCOMPARE(controller.selectedDrawingOpacity(), 1.0);
    controller.undo();
    QCOMPARE(controller.selectedDrawingOpacity(), 0.0);
    controller.undo();
    QCOMPARE(controller.selectedDrawingOpacity(), 0.4);
    if (supportsFill) {
        QVERIFY(controller.clearSelectedDrawingInteriorColor());
        QVERIFY(controller.clearSelectedDrawingInteriorColor());
        QVERIFY(!controller.selectedDrawingHasInterior());
        verifyInteriorHit();
        QVERIFY(controller.setSelectedDrawingOpacity(0.0));
        verifyInteriorHit();
        QVERIFY(!controller.selectedDrawingHasInterior());
        controller.undo();
        QCOMPARE(controller.selectedDrawingOpacity(), 0.4);
        controller.undo();
        QCOMPARE(controller.selectedDrawingInteriorColor(), interior);
    }
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.draftsForPage(0).isEmpty());
}

void PDFEditControllerTest::editsSavedShapesRoundTrip_data()
{
    editsShapePropertiesWithUndoRedo_data();
}

void PDFEditControllerTest::editsSavedShapesRoundTrip()
{
    QFETCH(int, shapeType);
    QFETCH(bool, supportsFill);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("shape-reopen-%1-source.pdf").arg(shapeType));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("shape-reopen-%1-saved.pdf").arg(shapeType));
    const QString editedPath = m_temporaryDirectory.filePath(
        QStringLiteral("shape-reopen-%1-edited.pdf").arg(shapeType));
    QVERIFY(createOffsetCropPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setShapeType(static_cast<PDFEditController::ShapeType>(shapeType));
    controller.setTool(PDFEditController::AddShapeTool);
    QVERIFY(!controller.addShape(
        0, QPointF(0.2, 0.35), QPointF(0.55, 0.6)).isEmpty());
    const QRectF addedFrame = controller.selectedFrameRect();
    const QColor initialStroke(24, 86, 171);
    const QColor initialInterior(230, 178, 52);
    QVERIFY(controller.setSelectedDrawingStrokeColor(initialStroke));
    if (supportsFill) {
        QVERIFY(controller.setSelectedDrawingInteriorColor(initialInterior));
    }
    QVERIFY(controller.setSelectedDrawingOpacity(0.6));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    const ShapeSaveInspection firstSave = inspectShapePaths(savedPath);
    QVERIFY(firstSave.readable);
    QCOMPARE(firstSave.count, 1);
    QColor effectiveStroke = initialStroke;
    effectiveStroke.setAlphaF(0.6);
    QCOMPARE(firstSave.strokeColors.constFirst(), effectiveStroke);
    if (supportsFill) {
        QColor effectiveInterior = initialInterior;
        effectiveInterior.setAlphaF(0.6);
        QCOMPARE(firstSave.fillColors.constFirst(), effectiveInterior);
    }

    const QVariantMap sourceShape = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY2(!sourceShape.isEmpty(), "A saved TrePDF shape must rehydrate for editing");
    QCOMPARE(sourceShape.value(QStringLiteral("shapeType")).toInt(), shapeType);
    const QString sourceId = sourceShape.value(QStringLiteral("objectId")).toString();
    controller.setTool(PDFEditController::CommentSelectTool);
    QVERIFY(!controller.selectCandidate(0, sourceId));
    controller.setTool(PDFEditController::SelectTool);
    QVERIFY(controller.selectCandidate(0, sourceId));
    QVERIFY(controller.selectedDrawingPropertiesEditable());
    QCOMPARE(controller.selectedDrawingSupportsFill(), supportsFill);
    QCOMPARE(controller.selectedDrawingStrokeColor(), initialStroke);
    QVERIFY(qAbs(controller.selectedDrawingOpacity() - 0.6) < 0.005);
    QCOMPARE(controller.selectedDrawingHasInterior(), supportsFill);
    const QRectF originalFrame = controller.selectedFrameRect();
    QVERIFY(QLineF(originalFrame.topLeft(), addedFrame.topLeft()).length() < 0.002);
    QVERIFY(QLineF(originalFrame.bottomRight(), addedFrame.bottomRight()).length() < 0.002);
    QVERIFY(controller.setSelectedDrawingStrokeColor(initialStroke));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(originalFrame);
    controller.finishSelectedFrameResize();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    const QColor finalStroke(187, 45, 78);
    QVERIFY(controller.setSelectedDrawingStrokeColor(finalStroke));
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
    QVERIFY(controller.allDraftedTextObjectsSuppressibleForPage(0));
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.draftedTextObjectPathsForPage(0).isEmpty());
    controller.redo();
    QCOMPARE(controller.selectedDrawingStrokeColor(), finalStroke);

    const QRectF movedFrame = originalFrame.translated(0.04, 0.03);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    const QRectF resizedFrame(movedFrame.topLeft(),
                             QSizeF(movedFrame.width() * 1.15,
                                    movedFrame.height() * 0.8));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    controller.deleteSelectedObject();
    QVERIFY(!controller.selectCandidate(0, sourceId));
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
    controller.undo();
    QVERIFY(controller.selectCandidate(0, sourceId));
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedDrawingStrokeColor(), finalStroke);

    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!waitForCandidates(controller, 0).isEmpty());
    QVERIFY(controller.selectCandidate(0, sourceId));
    QCOMPARE(controller.selectedDrawingStrokeColor(), initialStroke);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(controller.setSelectedDrawingStrokeColor(finalStroke));
    QVERIFY(controller.setSelectedDrawingOpacity(0.8));
    if (supportsFill) {
        QVERIFY(controller.clearSelectedDrawingInteriorColor());
    }
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(editedPath)));

    const ShapeSaveInspection editedSave = inspectShapePaths(editedPath);
    QCOMPARE(editedSave.count, 1);
    effectiveStroke = finalStroke;
    effectiveStroke.setAlphaF(0.8);
    QCOMPARE(editedSave.strokeColors.constFirst(), effectiveStroke);
    const QVariantMap editedShape = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(!editedShape.isEmpty());
    QCOMPARE(editedShape.value(QStringLiteral("shapeType")).toInt(), shapeType);
    QVERIFY(controller.selectCandidate(
        0, editedShape.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedDrawingStrokeColor(), finalStroke);
    QVERIFY(qAbs(controller.selectedDrawingOpacity() - 0.8) < 0.005);
    QVERIFY(!controller.selectedDrawingHasInterior());
    QVERIFY(QLineF(controller.selectedFrameRect().topLeft(),
                   resizedFrame.topLeft()).length() < 0.002);
    QVERIFY(QLineF(controller.selectedFrameRect().bottomRight(),
                   resizedFrame.bottomRight()).length() < 0.002);
    const QString editedId = editedShape.value(QStringLiteral("objectId")).toString();
    const QRectF reopenedFrame = controller.selectedFrameRect();
    if (supportsFill) {
        controller.clearSelection();
        const QVariantMap hovered = controller.candidateAt(
            0, reopenedFrame.center().x(), reopenedFrame.center().y());
        QCOMPARE(hovered.value(QStringLiteral("objectId")).toString(), editedId);
        QVERIFY(controller.selectCandidate(
            0, hovered.value(QStringLiteral("objectId")).toString()));
        QVERIFY(!controller.selectedDrawingHasInterior());
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
    } else {
        const QPointF offStroke(
            reopenedFrame.left() + reopenedFrame.width() * 0.15,
            reopenedFrame.top() + reopenedFrame.height() * 0.85);
        QVERIFY(controller.candidateAt(0, offStroke.x(), offStroke.y())
                    .value(QStringLiteral("objectId")).toString() != editedId);
    }
    controller.deleteSelectedObject();
    QVERIFY(controller.save());
    QCOMPARE(inspectShapePaths(editedPath).count, 0);
    QVERIFY(candidateForKind(waitForCandidates(controller, 0),
                             QStringLiteral("sourceShape")).isEmpty());
    // Both Save As operations must leave their inputs intact.
    QCOMPARE(inspectShapePaths(sourcePath).count, 0);
    QCOMPARE(inspectShapePaths(savedPath).strokeColors.constFirst(),
             firstSave.strokeColors.constFirst());
}

void PDFEditControllerTest::guardsShapePropertiesDuringSave()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("shape-properties-save-guard-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("shape-properties-save-guard-saved.pdf"));
    QVERIFY(createOffsetCropPdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::RectangleShape);
    controller.setTool(PDFEditController::AddShapeTool);
    QVERIFY(!controller.addShape(0, QPointF(0.2, 0.3), QPointF(0.5, 0.6)).isEmpty());
    const QColor stroke(46, 92, 173);
    QVERIFY(controller.setSelectedDrawingStrokeColor(stroke));
    QSignalSpy finished(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(savedPath)));
    QVERIFY(controller.saving());
    QVERIFY(!controller.setSelectedDrawingStrokeColor(Qt::red));
    QVERIFY(!controller.setSelectedDrawingInteriorColor(Qt::yellow));
    QVERIFY(!controller.clearSelectedDrawingInteriorColor());
    QVERIFY(!controller.setSelectedDrawingOpacity(0.2));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QVERIFY(finished.constFirst().constFirst().toBool());
    QCOMPARE(inspectShapePaths(savedPath).strokeColors.constFirst(), stroke);
}

void PDFEditControllerTest::editsLegacyUnmarkedShapesRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("legacy-unmarked-shapes-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("legacy-unmarked-shapes-edited.pdf"));
    // Match the legacy blank.pdf geometry, including its negative rectangle
    // height and transparent fill enabled through the B painting operator.
    const QByteArray content(
        "q /GS0 gs 0 0 0 RG 0 0 0 rg 1.7424 w 1 J 1 j "
        "279.67188 737.85938 m 309.84375 706.5625 l "
        "249.48438 706.5625 l h B "
        "80.78125 443.76562 158.85938 -73.859375 re B Q");
    const QString inputPath = qEnvironmentVariable("TREPDF_UNMARKED_TEST_INPUT");
    QVERIFY(inputPath.isEmpty()
                ? createUnmarkedShapeContentPdf(sourcePath, content)
                : QFile::copy(inputPath, sourcePath));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = sourceFile.readAll();
    sourceFile.close();
    const ShapeSaveInspection original = inspectShapePaths(sourcePath);
    QCOMPARE(original.count, 2);
    QCOMPARE(original.managedCount, 0);
    QCOMPARE(original.transparentFillCount, 2);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantList candidates = waitForCandidates(controller, 0);
    QCOMPARE(candidates.size(), 2);
    const QVector<int> expectedTypes{
        PDFEditController::TriangleShape, PDFEditController::RectangleShape};
    // The editable frame includes the stroke: canonical shapes inset their
    // path centerline by half its width on every edge.
    constexpr qreal strokeInset = 1.7424 / 2.0;
    const QVector<QRectF> expectedFrames{
        QRectF((249.48438 - strokeInset) / 612.0,
               (792.0 - 737.85938 - strokeInset) / 792.0,
               (309.84375 - 249.48438 + 2.0 * strokeInset) / 612.0,
               (737.85938 - 706.5625 + 2.0 * strokeInset) / 792.0),
        QRectF((80.78125 - strokeInset) / 612.0,
               (792.0 - 443.76562 - strokeInset) / 792.0,
               (158.85938 + 2.0 * strokeInset) / 612.0,
               (73.859375 + 2.0 * strokeInset) / 792.0)};
    QVector<QRectF> finalFrames;
    QVector<QColor> finalStrokes{QColor(193, 42, 66), QColor(24, 106, 182)};
    const QColor interior(226, 175, 47);

    for (qsizetype index = 0; index < candidates.size(); ++index) {
        const QVariantMap candidate = candidates.at(index).toMap();
        QCOMPARE(candidate.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("sourceShape"));
        QVERIFY(candidate.value(QStringLiteral("editable")).toBool());
        QCOMPARE(candidate.value(QStringLiteral("shapeType")).toInt(),
                 expectedTypes.at(index));
        const QString objectId = candidate.value(QStringLiteral("objectId")).toString();
        controller.setTool(PDFEditController::CommentSelectTool);
        QVERIFY(!controller.selectCandidate(0, objectId));
        controller.setTool(PDFEditController::SelectTool);
        QVERIFY(controller.selectCandidate(0, objectId));
        const QRectF frame = controller.selectedFrameRect();
        QVERIFY(QLineF(frame.topLeft(), expectedFrames.at(index).topLeft()).length()
                < 0.0001);
        QVERIFY(QLineF(frame.bottomRight(), expectedFrames.at(index).bottomRight()).length()
                < 0.0001);
        QVERIFY(controller.selectedDrawingPropertiesEditable());
        QVERIFY(controller.selectedDrawingSupportsFill());
        QVERIFY(!controller.selectedDrawingHasInterior());
        QCOMPARE(controller.selectedDrawingStrokeColor(), QColor(Qt::black));
        QCOMPARE(controller.selectedDrawingOpacity(), 1.0);
        // Legacy unfilled shapes support the same interior hover/click as
        // newly added shapes without creating a draft just by selecting them.
        QCOMPARE(controller.candidateAt(0, frame.center().x(), frame.bottom())
                     .value(QStringLiteral("objectId")).toString(), objectId);
        controller.clearSelection();
        const QVariantMap hovered = controller.candidateAt(
            0, frame.center().x(), frame.center().y());
        QCOMPARE(hovered.value(QStringLiteral("objectId")).toString(), objectId);
        QVERIFY(controller.selectCandidate(
            0, hovered.value(QStringLiteral("objectId")).toString()));
        QVERIFY(!controller.selectedDrawingHasInterior());
        QVERIFY(controller.setSelectedDrawingStrokeColor(Qt::black));
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());

        QVERIFY(controller.setSelectedDrawingStrokeColor(finalStrokes.at(index)));
        QVERIFY(controller.setSelectedDrawingInteriorColor(interior));
        QCOMPARE(controller.candidateAt(0, frame.center().x(), frame.center().y())
                     .value(QStringLiteral("objectId")).toString(), objectId);
        QVERIFY(controller.setSelectedDrawingOpacity(0.6));
        const QRectF moved = frame.translated(0.04, 0.05);
        controller.beginSelectedFrameResize();
        controller.updateSelectedFrameRect(moved);
        controller.finishSelectedFrameResize();
        const QRectF resized(moved.topLeft(),
                             QSizeF(moved.width() * 1.2, moved.height() * 1.3));
        controller.beginSelectedFrameResize();
        controller.updateSelectedFrameRect(resized);
        controller.finishSelectedFrameResize();
        QCOMPARE(controller.selectedFrameRect(), resized);
        QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
        QVERIFY(controller.allDraftedTextObjectsSuppressibleForPage(0));
        controller.undo();
        QCOMPARE(controller.selectedFrameRect(), moved);
        controller.redo();
        QCOMPARE(controller.selectedFrameRect(), resized);
        controller.deleteSelectedObject();
        QVERIFY(!controller.selectCandidate(0, objectId));
        controller.undo();
        QVERIFY(controller.selectCandidate(0, objectId));
        QCOMPARE(controller.selectedFrameRect(), resized);
        controller.discardDrafts();
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
        QVERIFY(controller.selectCandidate(0, objectId));
        QCOMPARE(controller.selectedFrameRect(), frame);
        QCOMPARE(controller.selectedDrawingStrokeColor(), QColor(Qt::black));
        QVERIFY(!controller.selectedDrawingHasInterior());
        finalFrames.append(resized);
    }

    // Edit both legacy paths in one save transaction, then verify their new
    // canonical metadata is sufficient for a subsequent reopen/edit/save.
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        QVERIFY(controller.selectCandidate(
            0, candidates.at(index).toMap().value(QStringLiteral("objectId")).toString()));
        QVERIFY(controller.setSelectedDrawingStrokeColor(finalStrokes.at(index)));
        QVERIFY(controller.setSelectedDrawingInteriorColor(interior));
        QVERIFY(controller.setSelectedDrawingOpacity(0.6));
        controller.beginSelectedFrameResize();
        controller.updateSelectedFrameRect(finalFrames.at(index));
        controller.finishSelectedFrameResize();
    }
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 2);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(inspectShapePaths(savedPath).managedCount, 2);
    QVERIFY(document.open(QUrl::fromLocalFile(savedPath)));
    const QVariantList reopened = waitForCandidates(controller, 0);
    QCOMPARE(reopened.size(), 2);
    for (qsizetype index = 0; index < reopened.size(); ++index) {
        const QVariantMap candidate = reopened.at(index).toMap();
        QCOMPARE(candidate.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("sourceShape"));
        QCOMPARE(candidate.value(QStringLiteral("shapeType")).toInt(),
                 expectedTypes.at(index));
        QVERIFY(controller.selectCandidate(
            0, candidate.value(QStringLiteral("objectId")).toString()));
        QCOMPARE(controller.selectedDrawingStrokeColor(), finalStrokes.at(index));
        QCOMPARE(controller.selectedDrawingInteriorColor(), interior);
        QVERIFY(qAbs(controller.selectedDrawingOpacity() - 0.6) < 0.005);
        QVERIFY(QLineF(controller.selectedFrameRect().topLeft(),
                       finalFrames.at(index).topLeft()).length() < 0.0001);
        QVERIFY(QLineF(controller.selectedFrameRect().bottomRight(),
                       finalFrames.at(index).bottomRight()).length() < 0.0001);
        QVERIFY(controller.clearSelectedDrawingInteriorColor());
    }
    QVERIFY(controller.save());
    QCOMPARE(inspectShapePaths(savedPath).count, 2);
    QCOMPARE(inspectShapePaths(savedPath).managedCount, 2);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    QCOMPARE(sourceFile.readAll(), originalBytes);
    QCOMPARE(inspectShapePaths(sourcePath).managedCount, 0);
}

void PDFEditControllerTest::recognizesEquivalentUnmarkedPolygons_data()
{
    QTest::addColumn<QByteArray>("pathContent");
    QTest::addColumn<int>("shapeType");
    QTest::addColumn<QByteArray>("pageGeometry");
    QTest::addColumn<QSizeF>("pageSize");
    QTest::addColumn<QRectF>("displayFrame");
    const QByteArray normalPage("/MediaBox [0 0 612 792]");
    const QSizeF normalSize(612.0, 792.0);
    // Each path uses the same 100..220 by 300..400 bounds. Its editable
    // frame includes half of the two-point outline on every edge.
    const QRectF normalFrame(99.0, 391.0, 122.0, 102.0);
    QTest::newRow("rectangle-cyclic")
        << QByteArray("220 300 m 100 300 l 100 400 l 220 400 l h S")
        << int(PDFEditController::RectangleShape)
        << normalPage << normalSize << normalFrame;
    QTest::newRow("rectangle-reversed")
        << QByteArray("220 300 m 220 400 l 100 400 l 100 300 l h S")
        << int(PDFEditController::RectangleShape)
        << normalPage << normalSize << normalFrame;
    QTest::newRow("triangle-cyclic")
        << QByteArray("220 300 m 100 300 l 160 400 l h S")
        << int(PDFEditController::TriangleShape)
        << normalPage << normalSize << normalFrame;
    QTest::newRow("triangle-reversed")
        << QByteArray("100 300 m 220 300 l 160 400 l h S")
        << int(PDFEditController::TriangleShape)
        << normalPage << normalSize << normalFrame;
    QTest::newRow("diamond-cyclic")
        << QByteArray("220 350 m 160 300 l 100 350 l 160 400 l h S")
        << int(PDFEditController::DiamondShape)
        << normalPage << normalSize << normalFrame;
    QTest::newRow("diamond-reversed-explicit-close")
        << QByteArray("100 350 m 160 300 l 220 350 l 160 400 l 100 350 l h S")
        << int(PDFEditController::DiamondShape)
        << normalPage << normalSize << normalFrame;
    const QByteArray croppedPage(
        "/MediaBox [20 30 632 822] /CropBox [50 100 450 700]");
    QTest::newRow("rectangle-offset-crop")
        << QByteArray("100 300 120 100 re S")
        << int(PDFEditController::RectangleShape)
        << croppedPage << QSizeF(400.0, 600.0)
        << QRectF(49.0, 299.0, 122.0, 102.0);
    // Page rotation changes the displayed coordinate system while the path
    // remains a representable diamond, so Save must retain the same frame.
    QTest::newRow("diamond-offset-crop-rotated")
        << QByteArray("100 350 m 160 300 l 220 350 l 160 400 l h S")
        << int(PDFEditController::DiamondShape)
        << croppedPage + " /Rotate 90" << QSizeF(600.0, 400.0)
        << QRectF(199.0, 49.0, 102.0, 122.0);
}

void PDFEditControllerTest::recognizesEquivalentUnmarkedPolygons()
{
    QFETCH(QByteArray, pathContent);
    QFETCH(int, shapeType);
    QFETCH(QByteArray, pageGeometry);
    QFETCH(QSizeF, pageSize);
    QFETCH(QRectF, displayFrame);
    const QString tag = QString::fromLatin1(QTest::currentDataTag());
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("equivalent-unmarked-%1.pdf").arg(tag));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("equivalent-unmarked-%1-saved.pdf").arg(tag));
    QVERIFY(createUnmarkedShapeContentPdf(
        sourcePath, QByteArray("q 0 0 0 RG 2 w 1 J 1 j ") + pathContent + " Q",
        pageGeometry));
    QCOMPARE(inspectShapePaths(sourcePath).managedCount, 0);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(document.pageSize(0), pageSize);
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantList candidates = waitForCandidates(controller, 0);
    QCOMPARE(candidates.size(), 1);
    const QVariantMap candidate = candidates.constFirst().toMap();
    QCOMPARE(candidate.value(QStringLiteral("kind")).toString(),
             QStringLiteral("sourceShape"));
    QCOMPARE(candidate.value(QStringLiteral("shapeType")).toInt(), shapeType);
    QVERIFY(candidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    const QRectF expectedFrame(
        displayFrame.x() / pageSize.width(),
        displayFrame.y() / pageSize.height(),
        displayFrame.width() / pageSize.width(),
        displayFrame.height() / pageSize.height());
    QVERIFY(QLineF(controller.selectedFrameRect().topLeft(),
                   expectedFrame.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(controller.selectedFrameRect().bottomRight(),
                   expectedFrame.bottomRight()).length() < 0.0001);
    QVERIFY(controller.selectedDrawingPropertiesEditable());
    QVERIFY(!controller.selectedDrawingHasInterior());
    QCOMPARE(controller.selectedDrawingStrokeColor(), QColor(Qt::black));
    QCOMPARE(controller.selectedDrawingOpacity(), 1.0);
    const QColor editedStroke(32, 104, 178);
    QVERIFY(controller.setSelectedDrawingStrokeColor(editedStroke));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
    QCOMPARE(inspectShapePaths(savedPath).managedCount, 1);
    QVERIFY(document.open(QUrl::fromLocalFile(savedPath)));
    const QVariantList reopened = waitForCandidates(controller, 0);
    QCOMPARE(reopened.size(), 1);
    const QVariantMap savedCandidate = reopened.constFirst().toMap();
    QCOMPARE(savedCandidate.value(QStringLiteral("kind")).toString(),
             QStringLiteral("sourceShape"));
    QCOMPARE(savedCandidate.value(QStringLiteral("shapeType")).toInt(), shapeType);
    QVERIFY(controller.selectCandidate(
        0, savedCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedDrawingStrokeColor(), editedStroke);
    QVERIFY(QLineF(controller.selectedFrameRect().topLeft(),
                   expectedFrame.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(controller.selectedFrameRect().bottomRight(),
                   expectedFrame.bottomRight()).length() < 0.0001);
    QCOMPARE(inspectShapePaths(sourcePath).managedCount, 0);
}

void PDFEditControllerTest::ignoresUnmarkedSourceShapePaths()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("unmarked-shape-paths.pdf"));
    const QByteArray content("q 0 0 0 RG 1 w 1 J 1 j "
                             "100 300 m 220 300 l 230 350 l 110 380 l h S "
                             "300 300 m 420 380 l S Q");
    QVERIFY(writePdfObjects(sourcePath, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] "
                   "/Resources << >> /Contents 4 0 R >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n") + content + QByteArray("\nendstream")
    }));
    QCOMPARE(inspectShapePaths(sourcePath).count, 2);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    QVERIFY(candidateForKind(waitForCandidates(controller, 0),
                             QStringLiteral("sourceShape")).isEmpty());
    QVERIFY(!controller.selectedDrawingPropertiesEditable());
    QVERIFY(!controller.setSelectedDrawingStrokeColor(Qt::red));
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::rejectsUnsupportedUnmarkedShapes_data()
{
    QTest::addColumn<QByteArray>("content");
    const QByteArray style("q /GS0 gs 0 0 0 RG 0 0 0 rg 1.7424 w 1 J 1 j ");
    const QByteArray rectangle("80 370 160 74 re B ");
    QTest::newRow("dashed") << style + "[3 2] 0 d " + rectangle + "Q";
    QTest::newRow("clipped") << style + "80 370 80 74 re W n " + rectangle + "Q";
    QTest::newRow("transformed") << style + "1 0 0 1 25 0 cm " + rectangle + "Q";
    QTest::newRow("unequal-visible-alpha")
        << style + "/GS1 gs " + rectangle + "Q";
    QTest::newRow("marked-content")
        << style + "/ExternalDrawing BMC " + rectangle + "EMC Q";
    QTest::newRow("malformed-trepdf-mark")
        << style + "/TrePDFShape << /Data (broken metadata) >> BDC "
            + rectangle + "EMC Q";
    QTest::newRow("asymmetric-triangle")
        << style + "90 400 m 240 370 l 80 370 l h B Q";
    QTest::newRow("extra-subpath")
        << style + "80 370 160 74 re 120 390 40 20 re B Q";
}

void PDFEditControllerTest::rejectsUnsupportedUnmarkedShapes()
{
    QFETCH(QByteArray, content);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("unsupported-unmarked-shape-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createUnmarkedShapeContentPdf(sourcePath, content));
    QVERIFY(inspectShapePaths(sourcePath).count > 0);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    QVERIFY(candidateForKind(waitForCandidates(controller, 0),
                             QStringLiteral("sourceShape")).isEmpty());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::rejectsTamperedSourceShapeProvenance_data()
{
    QTest::addColumn<bool>("changeGeometry");
    QTest::newRow("malformed-metadata") << false;
    QTest::newRow("path-disagrees-with-metadata") << true;
}

void PDFEditControllerTest::rejectsTamperedSourceShapeProvenance()
{
    QFETCH(bool, changeGeometry);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("shape-provenance-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("shape-provenance-saved.pdf"));
    const QString tamperedPath = m_temporaryDirectory.filePath(
        QStringLiteral("shape-provenance-tampered.pdf"));
    QVERIFY(createOffsetCropPdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::RectangleShape);
    controller.setTool(PDFEditController::AddShapeTool);
    QVERIFY(!controller.addShape(0, QPointF(0.2, 0.3), QPointF(0.5, 0.6)).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
    QVERIFY(!candidateForKind(waitForCandidates(controller, 0),
                              QStringLiteral("sourceShape")).isEmpty());
    QVERIFY(tamperMarkedShapePath(savedPath, tamperedPath, changeGeometry));
    QVERIFY(document.open(QUrl::fromLocalFile(tamperedPath)));
    QVERIFY(candidateForKind(waitForCandidates(controller, 0),
                             QStringLiteral("sourceShape")).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(inspectShapePaths(tamperedPath).count, 1);
}

namespace {
QSet<int> tableTextFontSizes(const QString &sourcePath)
{
    PDFiumProcessLock lock;
    const QByteArray path = QFile::encodeName(sourcePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(path.constData(), nullptr);
    if (!document)
        return {};
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page)
        return {};
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    QSet<int> sizes;
    for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
        const auto object = FPDFPage_GetObject(page, index);
        if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT)
            continue;
        float size = 0;
        if (!FPDFTextObj_GetFontSize(object, &size) || !std::isfinite(size)
            || std::abs(size - std::round(size)) > .001)
            return {};
        sizes.insert(qRound(size));
    }
    return sizes;
}

QJsonObject readTableTestMetadata(const QString &sourcePath)
{
    PDFiumProcessLock lock;
    const QByteArray path = QFile::encodeName(sourcePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(path.constData(), nullptr);
    if (!document)
        return {};
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page)
        return {};
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, 0);
    if (!object || FPDFPageObj_CountMarks(object) != 1)
        return {};
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(object, 0);
    unsigned long length = 0;
    if (!FPDFPageObjMark_GetParamBlobValue(mark, "Data", nullptr, 0, &length)
        || length == 0 || length > 128 * 1024)
        return {};
    QByteArray bytes(qsizetype(length), Qt::Uninitialized);
    if (!FPDFPageObjMark_GetParamBlobValue(mark, "Data",
            reinterpret_cast<unsigned char *>(bytes.data()), length, &length))
        return {};
    return QJsonDocument::fromJson(bytes).object();
}

QVector<QPointF> tableCellTextDirections(const QString &sourcePath)
{
    PDFiumProcessLock lock;
    const QByteArray filename = QFile::encodeName(sourcePath);
    const auto document = FPDF_LoadDocument(filename.constData(), nullptr);
    if (!document)
        return {};
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    const auto page = FPDF_LoadPage(document, 0);
    if (!page)
        return {};
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    const PDFShapeUtils::CoordinateMapper mapper(page,
        QSizeF(FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)));
    QVector<QPointF> result;
    for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
        const auto object = FPDFPage_GetObject(page, index);
        if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT || !PDFTableCellMetadata::hasMark(object))
            continue;
        FS_MATRIX matrix{};
        QPointF origin, horizontal;
        if (!FPDFPageObj_GetMatrix(object, &matrix)
            || !mapper.devicePoint(QPointF(matrix.e, matrix.f), origin)
            || !mapper.devicePoint(QPointF(matrix.e + matrix.a, matrix.f + matrix.b), horizontal))
            return {};
        const QPointF delta = horizontal - origin;
        const qreal length = std::hypot(delta.x(), delta.y());
        if (length < 0.00001)
            return {};
        result.append(delta / length);
    }
    return result;
}

bool rewriteTableTestMetadata(const QString &sourcePath, const QString &targetPath,
                              const QString &key, const QVariant &value)
{
    PDFiumProcessLock lock;
    const QByteArray path = QFile::encodeName(sourcePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(path.constData(), nullptr);
    if (!document)
        return false;
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page)
        return false;
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, 0);
    if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH
        || FPDFPageObj_CountMarks(object) != 1)
        return false;
    FPDF_PAGEOBJECTMARK mark = FPDFPageObj_GetMark(object, 0);
    unsigned long length = 0;
    if (!FPDFPageObjMark_GetParamBlobValue(mark, "Data", nullptr, 0, &length)
        || length == 0 || length > 128 * 1024)
        return false;
    QByteArray bytes(qsizetype(length), Qt::Uninitialized);
    if (!FPDFPageObjMark_GetParamBlobValue(mark, "Data",
            reinterpret_cast<unsigned char *>(bytes.data()), length, &length))
        return false;
    QJsonObject metadata = QJsonDocument::fromJson(bytes).object();
    if (metadata.isEmpty())
        return false;
    if (value.isValid())
        metadata.insert(key, QJsonValue::fromVariant(value));
    else
        metadata.remove(key);
    bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    if (!FPDFPageObjMark_SetBlobParam(document, object, mark, "Data",
            reinterpret_cast<const unsigned char *>(bytes.constData()),
            static_cast<unsigned long>(bytes.size())) || !FPDFPage_GenerateContent(page))
        return false;
    PdfFileWriter writer(targetPath);
    return writer.open() && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
}

bool writeLegacyTableFormats(const QString &sourcePath, const QString &targetPath)
{
    PDFiumProcessLock lock;
    const QByteArray path = QFile::encodeName(sourcePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(path.constData(), nullptr);
    if (!document)
        return false;
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page)
        return false;
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
        const auto object = FPDFPage_GetObject(page, index);
        if (FPDFPageObj_CountMarks(object) != 1)
            return false;
        const auto mark = FPDFPageObj_GetMark(object, 0);
        unsigned long length = 0;
        if (!FPDFPageObjMark_GetParamBlobValue(mark, "Data", nullptr, 0, &length)
            || length == 0 || length > 128 * 1024)
            return false;
        QByteArray bytes(qsizetype(length), Qt::Uninitialized);
        if (!FPDFPageObjMark_GetParamBlobValue(mark, "Data",
                reinterpret_cast<unsigned char *>(bytes.data()), length, &length))
            return false;
        QJsonObject data = QJsonDocument::fromJson(bytes).object();
        if (index == 0) {
            if (data.value(QStringLiteral("version")).toInt() != 6)
                return false;
            data[QStringLiteral("version")] = 5;
            QJsonArray formats;
            for (const auto &value : data.value(QStringLiteral("cellFormats")).toArray()) {
                auto format = value.toObject();
                auto style = format.value(QStringLiteral("defaultStyle")).toObject();
                style.remove(QStringLiteral("fontSize"));
                format[QStringLiteral("defaultStyle")] = style;
                QJsonArray spans;
                for (const auto &spanValue : format.value(QStringLiteral("spans")).toArray()) {
                    auto span = spanValue.toObject();
                    span.remove(QStringLiteral("fontSize"));
                    spans.append(span);
                }
                format[QStringLiteral("spans")] = spans;
                formats.append(format);
            }
            data[QStringLiteral("cellFormats")] = formats;
        } else {
            if (data.value(QStringLiteral("version")).toInt() != 3)
                return false;
            data[QStringLiteral("version")] = 2;
            auto style = data.value(QStringLiteral("style")).toObject();
            style.remove(QStringLiteral("fontSize"));
            data[QStringLiteral("style")] = style;
        }
        bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
        if (!FPDFPageObjMark_SetBlobParam(document, object, mark, "Data",
                reinterpret_cast<const unsigned char *>(bytes.constData()),
                static_cast<unsigned long>(bytes.size())))
            return false;
    }
    if (!FPDFPage_GenerateContent(page))
        return false;
    PdfFileWriter writer(targetPath);
    return writer.open() && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
}

bool createTableTestPage(const QString &path, int rotation)
{
    const QByteArray page = QByteArrayLiteral(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 640 800] "
        "/CropBox [20 30 620 770] /Resources << >> /Contents 4 0 R /Rotate ")
        + QByteArray::number(rotation * 90) + " >>";
    return writePdfObjects(path, {
        QByteArrayLiteral("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArrayLiteral("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        page, QByteArrayLiteral("<< /Length 0 >>\nstream\n\nendstream")});
}

bool createImportedTableFrame(const QString &path, bool filled, int rotation,
                              const QString &change = {})
{
    QByteArray content = QByteArrayLiteral(
        "BT /F1 12 Tf 65 550 Td (Independent text) Tj ET\n"
        "BT /F1 12 Tf 365 550 Td (123.45) Tj ET\n0 g 0 G 1 w\n");
    if (change == QLatin1String("marked"))
        content += "/TrePDFShape BMC\n";
    if (change == QLatin1String("dashed"))
        content += "[3 2] 0 d\n";
    if (change == QLatin1String("clipped"))
        content += "q 50 100 250 500 re W n\n";
    const auto rectangle = [&](int x, int y, int width, int height) {
        content += QByteArray::number(x) + ' ' + QByteArray::number(y) + ' '
            + QByteArray::number(width) + ' ' + QByteArray::number(height) + " re f\n";
    };
    const auto line = [&](int x1, int y1, int x2, int y2) {
        content += QByteArray::number(x1) + ' ' + QByteArray::number(y1) + " m "
            + QByteArray::number(x2) + ' ' + QByteArray::number(y2) + " l S\n";
    };
    if (filled) {
        // Office-style borders: split strips, junction squares and duplicate
        // corners, as opposed to a single already-semantic table path.
        for (int x = 50; x <= 550; x += 100) {
            rectangle(x, 595, 1, x == 50 || x == 550 ? 6 : 5);
            rectangle(x, 600, 1, 1);
            if (x < 550)
                rectangle(x + 1, 600, 99, 1);
        }
        for (int x = 50; x <= 550; x += 100) {
            rectangle(x, 101, 1, 494);
            rectangle(x, 100, 1, 1);
            if (x == 50 || x == 550)
                rectangle(x, 100, 1, 1);
            if (x < 550)
                rectangle(x + 1, 100, 99, 1);
        }
    } else {
        line(50, 100, 550, 100);
        line(50, 600, 550, 600);
        line(50, 100, 50, 600);
        if (change != QLatin1String("missing-border"))
            line(550, 100, 550, 600);
        if (change != QLatin1String("one-cell")) {
            if (change == QLatin1String("mixed-color"))
                content += "1 0 0 RG\n";
            line(200, change == QLatin1String("partial-divider") ? 350 : 100, 200, 600);
            content += "0 G\n";
            if (change == QLatin1String("interleaved-text"))
                content += "BT /F1 12 Tf 365 300 Td (Between paths) Tj ET\n";
            line(350, 100, 350, 600);
            line(50, 350, 550, 350);
        }
    }
    if (change == QLatin1String("marked"))
        content += "EMC\n";
    if (change == QLatin1String("clipped"))
        content += "Q\n";
    const QByteArray page = QByteArrayLiteral(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 700] "
        "/CropBox [20 30 580 670] /Resources << /Font << /F1 5 0 R >> >> "
        "/Contents 4 0 R /Rotate ") + QByteArray::number(rotation * 90) + " >>";
    return writePdfObjects(path, {
        QByteArrayLiteral("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArrayLiteral("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"), page,
        "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n"
            + content + "endstream",
        QByteArrayLiteral("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")});
}

QJsonArray importedFrameTextSnapshot(const QString &path)
{
    PDFiumProcessLock lock;
    const QByteArray filename = QFile::encodeName(path);
    const auto document = FPDF_LoadDocument(filename.constData(), nullptr);
    if (!document)
        return {};
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    const auto page = FPDF_LoadPage(document, 0);
    if (!page)
        return {};
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    const auto textPage = FPDFText_LoadPage(page);
    if (!textPage)
        return {};
    const auto closeTextPage = qScopeGuard([&] { FPDFText_ClosePage(textPage); });
    QJsonArray result;
    for (int i = 0; i < FPDFPage_CountObjects(page); ++i) {
        const auto object = FPDFPage_GetObject(page, i);
        if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT)
            continue;
        const auto bytes = FPDFTextObj_GetText(object, textPage, nullptr, 0);
        if (bytes < 2 || bytes > 1024)
            return {};
        QVector<FPDF_WCHAR> text(bytes / 2);
        FS_MATRIX matrix{};
        float left = 0, bottom = 0, right = 0, top = 0;
        if (FPDFTextObj_GetText(object, textPage, text.data(), bytes) != bytes
            || !FPDFPageObj_GetMatrix(object, &matrix)
            || !FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top))
            return {};
        result.append(QJsonArray{
            QString::fromUtf16(reinterpret_cast<const char16_t *>(text.constData()), text.size() - 1),
            matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f, left, bottom, right, top});
    }
    return result;
}

bool tamperTableCellObject(const QString &sourcePath, const QString &targetPath,
                           const QString &change)
{
    PDFiumProcessLock lock;
    const QByteArray path = QFile::encodeName(sourcePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(path.constData(), nullptr);
    if (!document)
        return false;
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page)
        return false;
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    FPDF_PAGEOBJECT cell = nullptr;
    int cellIndex = -1;
    for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
        const auto object = FPDFPage_GetObject(page, index);
        if (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_TEXT) {
            cell = object;
            cellIndex = index;
            break;
        }
    }
    if (!cell)
        return false;
    if (change == QLatin1String("missing-underline") || change == QLatin1String("moved-underline")
        || change == QLatin1String("missing-space-pair")) {
        FPDF_PAGEOBJECT underline = nullptr;
        for (int index = 1; index < FPDFPage_CountObjects(page); ++index) {
            const auto object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_PATH && FPDFPageObj_CountMarks(object) == 1) {
                underline = object;
                break;
            }
        }
        if (!underline)
            return false;
        if (change != QLatin1String("moved-underline")) {
            if (!FPDFPage_RemoveObject(page, underline))
                return false;
            FPDFPageObj_Destroy(underline);
            if (change == QLatin1String("missing-space-pair")) {
                if (!FPDFPage_RemoveObject(page, cell))
                    return false;
                FPDFPageObj_Destroy(cell);
            }
        } else {
            FPDFPageObj_Transform(underline, 1, 0, 0, 1, 20, 0);
        }
    } else if (change == QLatin1String("missing-fragment")) {
        if (!FPDFPage_RemoveObject(page, cell))
            return false;
        FPDFPageObj_Destroy(cell);
    } else if (change == QLatin1String("moved-fragment")) {
        FPDFPageObj_Transform(cell, 1, 0, 0, 1, 20, 0);
    } else if (change == QLatin1String("rotated-fragment")) {
        FS_MATRIX matrix{};
        if (!FPDFPageObj_GetMatrix(cell, &matrix))
            return false;
        std::swap(matrix.a, matrix.c);
        std::swap(matrix.b, matrix.d);
        if (!FPDFPageObj_SetMatrix(cell, &matrix))
            return false;
    } else if (change == QLatin1String("child-rotation")) {
        const auto mark = FPDFPageObj_GetMark(cell, 0);
        unsigned long length = 0;
        if (!mark || !FPDFPageObjMark_GetParamBlobValue(mark, "Data", nullptr, 0, &length)
            || length == 0 || length > 16 * 1024)
            return false;
        QByteArray bytes(qsizetype(length), Qt::Uninitialized);
        if (!FPDFPageObjMark_GetParamBlobValue(mark, "Data",
                reinterpret_cast<unsigned char *>(bytes.data()), length, &length))
            return false;
        auto data = QJsonDocument::fromJson(bytes).object();
        data.insert(QStringLiteral("rotation"), 270);
        bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
        if (!FPDFPageObjMark_SetBlobParam(document, cell, mark, "Data",
                reinterpret_cast<const unsigned char *>(bytes.constData()),
                static_cast<unsigned long>(bytes.size())))
            return false;
    } else if (change == QLatin1String("recolored-fragment")) {
        if (!FPDFPageObj_SetFillColor(cell, 255, 0, 0, 255))
            return false;
    } else if (change == QLatin1String("duplicate-marker")) {
        if (!FPDFPageObj_AddMark(cell, "TrePDFTableCell"))
            return false;
    } else if (change == QLatin1String("interleaved-paint")) {
        // A foreign opaque path between grid and text must never be moved
        // above the text when a table group is replaced during Save.
        const auto paint = FPDFPageObj_CreateNewPath(40, 40);
        if (!paint)
            return false;
        if (!FPDFPath_LineTo(paint, 600, 40)
            || !FPDFPath_LineTo(paint, 600, 760)
            || !FPDFPath_BezierTo(paint, 400, 780, 200, 720, 40, 760)
            || !FPDFPath_Close(paint)
            || !FPDFPageObj_SetFillColor(paint, 255, 255, 255, 255)
            || !FPDFPath_SetDrawMode(paint, FPDF_FILLMODE_WINDING, false)
            || !FPDFPage_InsertObjectAtIndex(page, paint, static_cast<size_t>(cellIndex))) {
            FPDFPageObj_Destroy(paint);
            return false;
        }
    } else if (change == QLatin1String("malformed-marker")) {
        bool changed = false;
        for (int index = 0; index < FPDFPageObj_CountMarks(cell); ++index) {
            const auto mark = FPDFPageObj_GetMark(cell, index);
            FPDF_WCHAR name[64]{};
            unsigned long length = 0;
            if (!FPDFPageObjMark_GetName(mark, name, sizeof(name), &length)
                || length > sizeof(name)
                || QString::fromUtf16(reinterpret_cast<const char16_t *>(name))
                    != QLatin1String("TrePDFTableCell"))
                continue;
            const QByteArray invalid = QByteArrayLiteral("{invalid");
            changed = FPDFPageObjMark_SetBlobParam(document, cell, mark, "Data",
                reinterpret_cast<const unsigned char *>(invalid.constData()),
                static_cast<unsigned long>(invalid.size()));
            break;
        }
        if (!changed)
            return false;
    } else {
        return false;
    }
    if (!FPDFPage_GenerateContent(page))
        return false;
    PdfFileWriter writer(targetPath);
    return writer.open() && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
}
}

void PDFEditControllerTest::editsImportedTableFrames_data()
{
    QTest::addColumn<bool>("filled");
    QTest::addColumn<int>("rotation");
    QTest::newRow("filled-five-columns") << true << 0;
    QTest::newRow("stroked") << false << 0;
    QTest::newRow("stroked-90") << false << 1;
    QTest::newRow("stroked-180") << false << 2;
    QTest::newRow("stroked-270") << false << 3;
}

void PDFEditControllerTest::editsImportedTableFrames()
{
    QFETCH(bool, filled);
    QFETCH(int, rotation);
    const QString suffix = QString::fromLatin1(QTest::currentDataTag());
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("imported-frame-%1.pdf").arg(suffix));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("imported-frame-saved-%1.pdf").arg(suffix));
    QVERIFY(createImportedTableFrame(path, filled, rotation));
    {
        PDFiumProcessLock lock;
        const QByteArray filename = QFile::encodeName(path);
        const auto pdf = FPDF_LoadDocument(filename.constData(), nullptr);
        QVERIFY(pdf);
        const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(pdf); });
        const auto page = FPDF_LoadPage(pdf, 0);
        QVERIFY(page);
        const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
        auto cancelled = std::make_shared<std::atomic_bool>(true);
        QVERIFY(PDFTableFrameRecognition::scan(page,
            QSizeF(FPDF_GetPageWidthF(page), FPDF_GetPageHeightF(page)), cancelled).isEmpty());
    }
    const auto originalText = importedFrameTextSnapshot(path);
    QCOMPARE(originalText.size(), 2);
    const QString originalPageText = pageText(path);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    const auto candidates = waitForCandidates(controller, 0);
    const auto table = candidateForKind(candidates, QStringLiteral("sourceShape"));
    QVERIFY(!table.isEmpty());
    QCOMPARE(table.value(QStringLiteral("shapeType")).toInt(), int(PDFEditController::TableShape));
    QVERIFY(table.value(QStringLiteral("tableFrameOnly")).toBool());
    QCOMPARE(table.value(QStringLiteral("tableRows")).toInt(), filled ? 1 : rotation % 2 ? 3 : 2);
    QCOMPARE(table.value(QStringLiteral("tableColumns")).toInt(), filled ? 5 : rotation % 2 ? 2 : 3);
    int shapeCount = 0;
    for (const auto &candidate : candidates)
        shapeCount += candidate.toMap().value(QStringLiteral("kind")).toString() == QLatin1String("sourceShape");
    QCOMPARE(shapeCount, 1); // Member paths are not independently editable shapes.
    QVERIFY(!candidateForKind(candidates, QStringLiteral("text")).isEmpty());
    QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    const QRectF frame = controller.selectedFrameRect();
    QCOMPARE(controller.candidateAt(0, frame.left(), frame.center().y())
        .value(QStringLiteral("objectId")).toString(), table.value(QStringLiteral("objectId")).toString());
    QVERIFY(controller.candidateAt(0, frame.left() + frame.width() * .07,
        frame.top() + frame.height() * .55).isEmpty());
    const auto textCandidate = candidateForKind(candidates, QStringLiteral("text"));
    const auto textCenter = textCandidate.value(QStringLiteral("sourceRect")).toRectF().center();
    QCOMPARE(controller.candidateAt(0, textCenter.x(), textCenter.y())
        .value(QStringLiteral("kind")).toString(), QStringLiteral("text"));
    const auto originalColumns = controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList();
    QVERIFY(!controller.setSelectedTableCellText(0, 0, QStringLiteral("Must stay independent")).isEmpty());
    QVERIFY(!controller.hasDrafts());
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(true, 0, .12);
    controller.updateSelectedTableDivider(true, 0, .15);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), frame);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList().at(0).toDouble(), .15);
    const auto suppressed = controller.draftedTextObjectPathsForPage(0);
    QCOMPARE(suppressed.size(), filled ? 36 : 7);
    for (const auto &objectPath : suppressed) {
        QCOMPARE(objectPath.size(), 1);
        QVERIFY(objectPath.first() >= 2); // Source text must not be suppressed.
    }
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList(), originalColumns);
    controller.redo();
    const auto columns = controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList();
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(true, 0, .2);
    controller.cancelSelectedFrameResize();
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList(), columns);
    if (!filled) {
        controller.beginSelectedFrameResize();
        controller.updateSelectedTableDivider(false, 0, .2);
        controller.finishSelectedFrameResize();
    }
    const auto rows = controller.selectedDraftPreview().value(QStringLiteral("tableRowStops")).toList();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QCOMPARE(importedFrameTextSnapshot(saved), originalText);
    QCOMPARE(pageText(saved), originalPageText);
    QCOMPARE(inspectShapePaths(saved).count, 1); // One managed frame replaces all source rules.
    PDFDocument reopenedDocument;
    PDFEditController reopened;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(saved)));
    reopened.setDocument(&reopenedDocument);
    const auto again = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QVERIFY(!again.isEmpty());
    QVERIFY(again.value(QStringLiteral("tableFrameOnly")).toBool());
    QCOMPARE(again.value(QStringLiteral("tableColumnStops")).toList(), columns);
    QCOMPARE(again.value(QStringLiteral("tableRowStops")).toList(), rows);
    QVERIFY(reopened.selectCandidate(0, again.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!reopened.setSelectedTableCellText(0, 0, QStringLiteral("Still independent")).isEmpty());
    reopened.beginSelectedFrameResize();
    reopened.updateSelectedFrameRect(frame.adjusted(.01, .01, -.01, -.01));
    reopened.finishSelectedFrameResize();
    QVERIFY(reopened.save());
    QCOMPARE(importedFrameTextSnapshot(saved), originalText);
    const auto resized = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QVERIFY(reopened.selectCandidate(0, resized.value(QStringLiteral("objectId")).toString()));
    reopened.deleteSelectedObject();
    reopened.undo();
    QVERIFY(!reopened.hasDrafts());
    reopened.redo();
    QVERIFY(reopened.save());
    QCOMPARE(inspectShapePaths(saved).count, 0);
    QCOMPARE(importedFrameTextSnapshot(saved), originalText);
    QCOMPARE(pageText(saved), originalPageText);
}

void PDFEditControllerTest::rejectsUnsafeImportedTableFrames_data()
{
    QTest::addColumn<QString>("change");
    for (const auto *change : {"missing-border", "partial-divider", "marked", "dashed",
                               "clipped", "mixed-color", "interleaved-text", "one-cell"})
        QTest::newRow(change) << QString::fromLatin1(change);
}

void PDFEditControllerTest::rejectsUnsafeImportedTableFrames()
{
    QFETCH(QString, change);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("unsafe-frame-%1.pdf").arg(change));
    QVERIFY(createImportedTableFrame(path, false, 0, change));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    const auto candidates = waitForCandidates(controller, 0);
    for (const auto &candidate : candidates) {
        const auto map = candidate.toMap();
        QVERIFY(map.value(QStringLiteral("kind")).toString() != QLatin1String("sourceShape")
            || map.value(QStringLiteral("shapeType")).toInt() != PDFEditController::TableShape);
    }
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::addsTableWithAtomicHistory()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("table-history.pdf"));
    QVERIFY(createTableTestPage(sourcePath, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QCOMPARE(controller.tableRows(), 3);
    QCOMPARE(controller.tableColumns(), 3);
    controller.setTableRows(0);
    controller.setTableColumns(21);
    QCOMPARE(controller.tableRows(), 3);
    QCOMPARE(controller.tableColumns(), 3);
    controller.setTableRows(3);
    controller.setTableColumns(4);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTool(PDFEditController::AddShapeTool);
    QVERIFY(controller.addShape(0, QPointF(0.2, 0.3), QPointF(0.2, 0.3)).isEmpty());
    QVERIFY(controller.addShape(-1, QPointF(0.2, 0.3), QPointF(0.6, 0.7)).isEmpty());
    QVERIFY(!controller.hasDrafts());
    const QVariantMap added = controller.addShape(0, QPointF(0.6, 0.7), QPointF(0.2, 0.3));
    QVERIFY(!added.isEmpty());
    QCOMPARE(added.value(QStringLiteral("shapeType")).toInt(), int(PDFEditController::TableShape));
    QCOMPARE(added.value(QStringLiteral("tableRows")).toInt(), 3);
    QCOMPARE(added.value(QStringLiteral("tableColumns")).toInt(), 4);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    const QString objectId = added.value(QStringLiteral("objectId")).toString();
    const QRectF initialFrame = controller.selectedFrameRect();
    QVERIFY(QLineF(initialFrame.topLeft(), QPointF(0.2, 0.3)).length() < 0.00001);
    QVERIFY(QLineF(initialFrame.bottomRight(), QPointF(0.6, 0.7)).length() < 0.00001);
    // A cell interior is a hit too; the grid is one movable object.
    QCOMPARE(controller.candidateAt(0, 0.237, 0.351).value(QStringLiteral("objectId")).toString(), objectId);
    QVERIFY(controller.selectedDrawingPropertiesEditable());
    QVERIFY(!controller.selectedDrawingSupportsFill());
    QVERIFY(!controller.setSelectedDrawingInteriorColor(Qt::yellow));
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    controller.setTableRows(8);
    controller.setTableColumns(9);
    const QRectF resized(0.25, 0.32, 0.5, 0.3);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resized);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resized);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableRows")).toInt(), 3);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumns")).toInt(), 4);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), initialFrame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), resized);
    controller.deleteSelectedObject();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    controller.undo();
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumns")).toInt(), 4);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
}


void PDFEditControllerTest::insertsTableTracksWithAtomicHistory_data()
{
    QTest::addColumn<bool>("column");
    QTest::addColumn<int>("beforeIndex");
    for (bool column : {false, true}) {
        for (int index = 0; index <= 2; ++index)
            QTest::newRow(qPrintable(QStringLiteral("%1-%2")
                .arg(column ? QStringLiteral("column") : QStringLiteral("row")).arg(index)))
                << column << index;
    }
}

void PDFEditControllerTest::insertsTableTracksWithAtomicHistory()
{
    using namespace PDFTableCellFormat;
    QFETCH(bool, column);
    QFETCH(int, beforeIndex);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("insert-track-%1.pdf")
        .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(2);
    controller.setTableColumns(2);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.45, .45)).isEmpty());
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(false, 0, .4);
    controller.updateSelectedTableDivider(true, 0, .3);
    controller.finishSelectedFrameResize();
    const QStringList texts{QStringLiteral("AB"), QStringLiteral("CD"), QStringLiteral("EF"), QStringLiteral("GH")};
    QVector<Cell> formats;
    for (int index = 0; index < 4; ++index) {
        const Style style{testFontFamily(), index % 2 != 0, index / 2 != 0, index == 3, qreal(12 + index)};
        Cell format = plain(texts.at(index), style);
        format.rotation = index * 90;
        // Character formatting must move with existing text, but is not copied into blank cells.
        format.spans = {{0, 1, style}, {1, 1, {style.family, !style.bold, style.italic, style.underline, style.fontSize}}};
        const QString error = controller.setSelectedTableCellContent(index / 2, index % 2, texts.at(index), toVariant(format));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        formats.append(format);
    }
    const QVariantMap before = controller.selectedDraftPreview();
    const QRectF oldFrame = controller.selectedFrameRect();
    const QVariantList oldRects = before.value(QStringLiteral("tableCellRects")).toList();
    QCOMPARE(oldRects.size(), 4);
    const QString error = column ? controller.insertSelectedTableColumn(beforeIndex)
                                 : controller.insertSelectedTableRow(beforeIndex);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QVariantMap after = controller.selectedDraftPreview();
    const int rows = column ? 2 : 3;
    const int columns = column ? 3 : 2;
    QCOMPARE(after.value(QStringLiteral("tableRows")).toInt(), rows);
    QCOMPARE(after.value(QStringLiteral("tableColumns")).toInt(), columns);
    QCOMPARE(controller.selectedFrameRect().topLeft(), oldFrame.topLeft());
    const QVariantList rects = after.value(QStringLiteral("tableCellRects")).toList();
    const QStringList actualTexts = after.value(QStringLiteral("tableCellTexts")).toStringList();
    const QVariantList actualFormats = after.value(QStringLiteral("tableCellFormats")).toList();
    QCOMPARE(rects.size(), rows * columns);
    QCOMPARE(actualTexts.size(), rows * columns);
    QCOMPARE(actualFormats.size(), rows * columns);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            const bool inserted = (column ? col : row) == beforeIndex;
            const int oldRow = column ? row : inserted ? qMin(beforeIndex, 1) : row - (row > beforeIndex);
            const int oldCol = !column ? col : inserted ? qMin(beforeIndex, 1) : col - (col > beforeIndex);
            const int oldIndex = oldRow * 2 + oldCol;
            const int index = row * columns + col;
            QCOMPARE(actualTexts.at(index), inserted ? QString() : texts.at(oldIndex));
            Cell expected = formats.at(oldIndex);
            if (inserted)
                expected.spans.clear();
            QCOMPARE(actualFormats.at(index).toMap(), toVariant(expected));
            const QSizeF oldSize = oldRects.at(oldIndex).toRectF().size();
            const QSizeF size = rects.at(index).toRectF().size();
            QVERIFY(qAbs(size.width() - oldSize.width()) < .000001);
            QVERIFY(qAbs(size.height() - oldSize.height()) < .000001);
        }
    }
    const QSizeF pageSize = document.pageSize(0);
    const qreal stroke = qMax<qreal>(.75, before.value(QStringLiteral("shapeStrokeWidthRatio")).toReal() * pageSize.height());
    const qreal fraction = column ? (beforeIndex == 0 ? .3 : .7) : (beforeIndex == 0 ? .4 : .6);
    const qreal extra = ((column ? oldFrame.width() * pageSize.width() : oldFrame.height() * pageSize.height()) - stroke) * fraction;
    QVERIFY(qAbs(controller.selectedFrameRect().width() - oldFrame.width() - (column ? extra / pageSize.width() : 0)) < .000001);
    QVERIFY(qAbs(controller.selectedFrameRect().height() - oldFrame.height() - (column ? 0 : extra / pageSize.height())) < .000001);
    controller.undo();
    QCOMPARE(controller.selectedDraftPreview(), before);
    controller.redo();
    QCOMPARE(controller.selectedDraftPreview(), after);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::savesInsertedTableTracks_data()
{
    QTest::addColumn<bool>("rotated");
    QTest::newRow("legacy-v2") << false;
    QTest::newRow("rotated-v8") << true;
}

void PDFEditControllerTest::savesInsertedTableTracks()
{
    using namespace PDFTableCellFormat;
    QFETCH(bool, rotated);
    const QString suffix = QString::fromLatin1(QTest::currentDataTag());
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("insert-saved-%1.pdf").arg(suffix));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("insert-saved-result-%1.pdf").arg(suffix));
    QVERIFY(createTableTestPage(path, rotated ? 1 : 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(2);
    controller.setTableColumns(2);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.35, .35)).isEmpty());
    Cell format = plain(QStringLiteral("Keep"), {testFontFamily(), true, false, true, 14});
    format.rotation = 270;
    if (rotated)
        QVERIFY(controller.setSelectedTableCellContent(1, 1, QStringLiteral("Keep"), toVariant(format)).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QCOMPARE(readTableTestMetadata(saved).value(QStringLiteral("version")).toInt(), rotated ? 8 : 2);
    auto table = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    const auto original = controller.selectedDraftPreview();
    QVERIFY(controller.insertSelectedTableRow(1).isEmpty());
    QVERIFY(controller.insertSelectedTableColumn(2).isEmpty());
    const auto inserted = controller.selectedDraftPreview();
    QCOMPARE(inserted.value(QStringLiteral("tableRows")).toInt(), 3);
    QCOMPARE(inserted.value(QStringLiteral("tableColumns")).toInt(), 3);
    controller.undo();
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumns")).toInt(), 2);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedDraftPreview(), original);
    controller.redo();
    controller.redo();
    QCOMPARE(controller.selectedDraftPreview(), inserted);
    controller.discardDrafts();
    QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedDraftPreview(), original);
    QVERIFY(controller.insertSelectedTableRow(1).isEmpty());
    QVERIFY(controller.insertSelectedTableColumn(2).isEmpty());
    QVERIFY(controller.save());

    PDFDocument reopenedDocument;
    PDFEditController reopened;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(saved)));
    reopened.setDocument(&reopenedDocument);
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QVERIFY(!table.isEmpty());
    QCOMPARE(table.value(QStringLiteral("tableRows")).toInt(), 3);
    QCOMPARE(table.value(QStringLiteral("tableColumns")).toInt(), 3);
    QCOMPARE(table.value(QStringLiteral("tableCellTexts")), inserted.value(QStringLiteral("tableCellTexts")));
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")), inserted.value(QStringLiteral("tableCellFormats")));
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    if (rotated) {
        QCOMPARE(table.value(QStringLiteral("tableCellTexts")).toStringList().at(7), QStringLiteral("Keep"));
        QCOMPARE(table.value(QStringLiteral("tableCellFormats")).toList().at(7).toMap(), toVariant(format));
    }
    // Further insertion and divider editing must validate against the saved source's own dimensions.
    QVERIFY(reopened.insertSelectedTableRow(0).isEmpty());
    QVERIFY(reopened.insertSelectedTableColumn(3).isEmpty());
    const auto beforeDivider = reopened.selectedDraftPreview();
    const qreal lastStop = beforeDivider.value(QStringLiteral("tableColumnStops")).toList().last().toReal();
    reopened.beginSelectedFrameResize();
    reopened.updateSelectedTableDivider(true, 2, lastStop + .02);
    reopened.finishSelectedFrameResize();
    QVERIFY(reopened.selectedDraftPreview().value(QStringLiteral("tableColumnStops"))
        != beforeDivider.value(QStringLiteral("tableColumnStops")));
    QVERIFY(reopened.save());
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(table.value(QStringLiteral("tableRows")).toInt(), 4);
    QCOMPARE(table.value(QStringLiteral("tableColumns")).toInt(), 4);
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    reopened.deleteSelectedObject();
    QVERIFY(reopened.save());
    QCOMPARE(topLevelPageObjectCount(saved), 0);
    QVERIFY(pageText(saved).trimmed().isEmpty());
}

void PDFEditControllerTest::rejectsInvalidTableTrackInsertion()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("insert-track-invalid.pdf"));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(!controller.insertSelectedTableRow(0).isEmpty());
    QVERIFY(!controller.insertSelectedTableColumn(0).isEmpty());
    controller.setShapeType(PDFEditController::RectangleShape);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.4, .4)).isEmpty());
    QVERIFY(!controller.insertSelectedTableRow(0).isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(2);
    controller.setTableColumns(2);
    QVERIFY(!controller.addShape(0, QPointF(.75, .75), QPointF(.95, .95)).isEmpty());
    const auto before = controller.selectedDraftPreview();
    for (int index : {-1, 0, 2, 3}) {
        QVERIFY(!controller.insertSelectedTableRow(index).isEmpty());
        QVERIFY(!controller.insertSelectedTableColumn(index).isEmpty());
        QCOMPARE(controller.selectedDraftPreview(), before);
    }
    controller.undo(); // Every rejection leaves only creation on the undo stack.
    QVERIFY(!controller.hasDrafts());
    controller.setTableRows(20);
    controller.setTableColumns(20);
    QVERIFY(!controller.addShape(0, QPointF(.05, .05), QPointF(.45, .45)).isEmpty());
    QVERIFY(!controller.insertSelectedTableRow(20).isEmpty());
    QVERIFY(!controller.insertSelectedTableColumn(0).isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.setTableRows(2);
    controller.setTableColumns(2);
    QVERIFY(!controller.addShape(0, QPointF(.1, .04), QPointF(.4, .12)).isEmpty());
    controller.setHeaderSelectionOnly(true);
    QVERIFY(!controller.selectedObjectId().isEmpty());
    const auto header = controller.selectedDraftPreview();
    QVERIFY(!controller.insertSelectedTableRow(2).isEmpty());
    QCOMPARE(controller.selectedDraftPreview(), header);
    controller.setHeaderSelectionOnly(false);
    QVERIFY(controller.insertSelectedTableRow(2).isEmpty());
    controller.undo();
    QCOMPARE(controller.selectedDraftPreview(), header);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::removesTableTracksWithAtomicHistory_data()
{
    QTest::addColumn<bool>("column");
    QTest::newRow("last-row") << false;
    QTest::newRow("last-column") << true;
}

void PDFEditControllerTest::removesTableTracksWithAtomicHistory()
{
    using namespace PDFTableCellFormat;
    QFETCH(bool, column);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-track-%1.pdf")
        .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.8, .8)).isEmpty());
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(false, 0, .2);
    controller.updateSelectedTableDivider(false, 1, .6);
    controller.updateSelectedTableDivider(true, 0, .3);
    controller.updateSelectedTableDivider(true, 1, .75);
    controller.finishSelectedFrameResize();
    for (int index = 0; index < 9; ++index) {
        const QString text = QStringLiteral("%1x").arg(index);
        const Style style{testFontFamily(), index % 2 != 0, index % 3 == 0, true, qreal(12 + index)};
        Cell format{style, {{0, 1, style}, {1, 1, {style.family, !style.bold, style.italic, false, style.fontSize}}}};
        format.rotation = (index % 4) * 90;
        const QString error = controller.setSelectedTableCellContent(index / 3, index % 3, text, toVariant(format));
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    const QVariantMap before = controller.selectedDraftPreview();
    const QRectF oldFrame = controller.selectedFrameRect();
    const QString error = column ? controller.removeSelectedTableColumn() : controller.removeSelectedTableRow();
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QVariantMap after = controller.selectedDraftPreview();
    const int rows = column ? 3 : 2;
    const int columns = column ? 2 : 3;
    QCOMPARE(after.value(QStringLiteral("tableRows")).toInt(), rows);
    QCOMPARE(after.value(QStringLiteral("tableColumns")).toInt(), columns);
    QCOMPARE(controller.selectedFrameRect().topLeft(), oldFrame.topLeft());
    const auto oldRects = before.value(QStringLiteral("tableCellRects")).toList();
    const auto rects = after.value(QStringLiteral("tableCellRects")).toList();
    const auto oldTexts = before.value(QStringLiteral("tableCellTexts")).toStringList();
    const auto texts = after.value(QStringLiteral("tableCellTexts")).toStringList();
    const auto oldFormats = before.value(QStringLiteral("tableCellFormats")).toList();
    const auto formats = after.value(QStringLiteral("tableCellFormats")).toList();
    QCOMPARE(rects.size(), rows * columns);
    QCOMPARE(texts.size(), rows * columns);
    QCOMPARE(formats.size(), rows * columns);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            const int oldIndex = row * 3 + col;
            const int index = row * columns + col;
            QCOMPARE(texts.at(index), oldTexts.at(oldIndex));
            QCOMPARE(formats.at(index), oldFormats.at(oldIndex));
            const QRectF oldRect = oldRects.at(oldIndex).toRectF();
            const QRectF rect = rects.at(index).toRectF();
            QVERIFY(QLineF(oldRect.topLeft(), rect.topLeft()).length() < .000001);
            QVERIFY(QLineF(oldRect.bottomRight(), rect.bottomRight()).length() < .000001);
        }
    }
    const QSizeF pageSize = document.pageSize(0);
    const qreal stroke = qMax<qreal>(.75, before.value(QStringLiteral("shapeStrokeWidthRatio")).toReal() * pageSize.height());
    const qreal removed = ((column ? oldFrame.width() * pageSize.width() : oldFrame.height() * pageSize.height()) - stroke)
        * (column ? .25 : .4);
    QVERIFY(qAbs(controller.selectedFrameRect().width() - oldFrame.width() + (column ? removed / pageSize.width() : 0)) < .000001);
    QVERIFY(qAbs(controller.selectedFrameRect().height() - oldFrame.height() + (column ? 0 : removed / pageSize.height())) < .000001);
    controller.undo();
    QCOMPARE(controller.selectedDraftPreview(), before);
    controller.redo();
    QCOMPARE(controller.selectedDraftPreview(), after);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::savesRemovedTableTracks_data()
{
    QTest::addColumn<int>("pageRotation");
    QTest::newRow("upright") << 0;
    QTest::newRow("rotated-page") << 1;
}

void PDFEditControllerTest::savesRemovedTableTracks()
{
    using namespace PDFTableCellFormat;
    QFETCH(int, pageRotation);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-saved-%1.pdf").arg(pageRotation));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("remove-saved-result-%1.pdf").arg(pageRotation));
    QVERIFY(createTableTestPage(path, pageRotation));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(2);
    controller.setTableColumns(2);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.8, .8)).isEmpty());
    const QStringList texts{QStringLiteral("Keep"), QStringLiteral("Column"), QStringLiteral("Row"), QStringLiteral("Both")};
    for (int index = 0; index < 4; ++index) {
        Cell format = plain(texts.at(index), {testFontFamily(), index % 2 == 0, index % 2 != 0, true, qreal(12 + index)});
        format.rotation = index * 90;
        QVERIFY(controller.setSelectedTableCellContent(index / 2, index % 2, texts.at(index), toVariant(format)).isEmpty());
    }
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    auto table = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    const auto original = controller.selectedDraftPreview();
    QVERIFY(controller.removeSelectedTableRow().isEmpty());
    QVERIFY(controller.removeSelectedTableColumn().isEmpty());
    const auto reduced = controller.selectedDraftPreview();
    QCOMPARE(reduced.value(QStringLiteral("tableCellTexts")).toStringList(), QStringList{QStringLiteral("Keep")});
    controller.undo();
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedDraftPreview(), original);
    controller.redo();
    controller.redo();
    QCOMPARE(controller.selectedDraftPreview(), reduced);
    controller.discardDrafts();
    QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedDraftPreview(), original);
    QVERIFY(controller.removeSelectedTableRow().isEmpty());
    QVERIFY(controller.save());
    QVERIFY(pageText(saved).contains(QStringLiteral("Keep")));
    QVERIFY(pageText(saved).contains(QStringLiteral("Column")));
    QVERIFY(!pageText(saved).contains(QStringLiteral("Row")));
    QVERIFY(!pageText(saved).contains(QStringLiteral("Both")));

    PDFDocument reopenedDocument;
    PDFEditController reopened;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(saved)));
    reopened.setDocument(&reopenedDocument);
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(table.value(QStringLiteral("tableRows")).toInt(), 1);
    QCOMPARE(table.value(QStringLiteral("tableColumns")).toInt(), 2);
    QCOMPARE(table.value(QStringLiteral("tableCellTexts")).toStringList(), texts.first(2));
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    QVERIFY(reopened.removeSelectedTableColumn().isEmpty());
    QVERIFY(reopened.save());
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(table.value(QStringLiteral("tableRows")).toInt(), 1);
    QCOMPARE(table.value(QStringLiteral("tableColumns")).toInt(), 1);
    QCOMPARE(table.value(QStringLiteral("tableCellTexts")), reduced.value(QStringLiteral("tableCellTexts")));
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")), reduced.value(QStringLiteral("tableCellFormats")));
    QVERIFY(table.value(QStringLiteral("tableColumnStops")).toList().isEmpty());
    QVERIFY(table.value(QStringLiteral("tableRowStops")).toList().isEmpty());
    QVERIFY(!pageText(saved).contains(QStringLiteral("Column")));
    QCOMPARE(inspectShapePaths(saved).count, 2); // The grid and retained cell's underline only.
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!reopened.removeSelectedTableRow().isEmpty());
    QVERIFY(!reopened.removeSelectedTableColumn().isEmpty());
    QVERIFY(!reopened.hasDrafts());
    QVERIFY(!reopened.canUndo());
    reopened.deleteSelectedObject();
    QVERIFY(reopened.save());
    QCOMPARE(topLevelPageObjectCount(saved), 0);
}

void PDFEditControllerTest::rejectsInvalidTableTrackRemoval()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("remove-track-invalid.pdf"));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(!controller.removeSelectedTableRow().isEmpty());
    QVERIFY(!controller.removeSelectedTableColumn().isEmpty());
    controller.setShapeType(PDFEditController::RectangleShape);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.4, .4)).isEmpty());
    QVERIFY(!controller.removeSelectedTableRow().isEmpty());
    QVERIFY(!controller.removeSelectedTableColumn().isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.4, .4)).isEmpty());
    const auto before = controller.selectedDraftPreview();
    QVERIFY(!controller.removeSelectedTableRow().isEmpty());
    QVERIFY(!controller.removeSelectedTableColumn().isEmpty());
    QCOMPARE(controller.selectedDraftPreview(), before);
    controller.undo(); // Rejections leave only table creation in history.
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::changesTableTracksInBatches_data()
{
    QTest::addColumn<bool>("column");
    QTest::newRow("rows") << false;
    QTest::newRow("columns") << true;
}

void PDFEditControllerTest::changesTableTracksInBatches()
{
    using namespace PDFTableCellFormat;
    QFETCH(bool, column);
    const QString suffix = QString::fromLatin1(QTest::currentDataTag());
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("batch-tracks-%1.pdf").arg(suffix));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("batch-tracks-saved-%1.pdf").arg(suffix));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(3);
    controller.setTableColumns(3);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.48, .48)).isEmpty());
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(false, 0, .25);
    controller.updateSelectedTableDivider(false, 1, .65);
    controller.updateSelectedTableDivider(true, 0, .3);
    controller.updateSelectedTableDivider(true, 1, .7);
    controller.finishSelectedFrameResize();
    for (int index = 0; index < 9; ++index) {
        const QString text = QStringLiteral("%1x").arg(index);
        const Style style{testFontFamily(), index % 2 != 0, index % 3 == 0, true, qreal(12 + index)};
        Cell format{style, {{0, 1, style}, {1, 1, {style.family, !style.bold, style.italic, false, style.fontSize}}}};
        format.rotation = (index % 4) * 90;
        QVERIFY(controller.setSelectedTableCellContent(index / 3, index % 3, text, toVariant(format)).isEmpty());
    }
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    auto table = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    const auto original = controller.selectedDraftPreview();
    const QString insertError = column ? controller.insertSelectedTableColumns(1, 2)
                                       : controller.insertSelectedTableRows(1, 2);
    QVERIFY2(insertError.isEmpty(), qPrintable(insertError));
    const auto inserted = controller.selectedDraftPreview();
    const int rows = column ? 3 : 5;
    const int columns = column ? 5 : 3;
    QCOMPARE(inserted.value(QStringLiteral("tableRows")).toInt(), rows);
    QCOMPARE(inserted.value(QStringLiteral("tableColumns")).toInt(), columns);
    const auto originalTexts = original.value(QStringLiteral("tableCellTexts")).toStringList();
    const auto originalFormats = original.value(QStringLiteral("tableCellFormats")).toList();
    const auto originalRects = original.value(QStringLiteral("tableCellRects")).toList();
    const auto texts = inserted.value(QStringLiteral("tableCellTexts")).toStringList();
    const auto formats = inserted.value(QStringLiteral("tableCellFormats")).toList();
    const auto rects = inserted.value(QStringLiteral("tableCellRects")).toList();
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            const int track = column ? col : row;
            const bool blank = track == 1 || track == 2;
            const int sourceTrack = blank ? 1 : track >= 3 ? track - 2 : track;
            const int oldIndex = (column ? row : sourceTrack) * 3 + (column ? sourceTrack : col);
            const int index = row * columns + col;
            QCOMPARE(texts.at(index), blank ? QString() : originalTexts.at(oldIndex));
            QVariantMap expected = originalFormats.at(oldIndex).toMap();
            if (blank)
                expected.insert(QStringLiteral("spans"), QVariantList{});
            QCOMPARE(formats.at(index).toMap(), expected);
            const auto size = rects.at(index).toRectF().size();
            const auto oldSize = originalRects.at(oldIndex).toRectF().size();
            QVERIFY(qAbs(size.width() - oldSize.width()) < .000001);
            QVERIFY(qAbs(size.height() - oldSize.height()) < .000001);
        }
    }
    controller.undo(); // Both inserted tracks are one history action.
    QCOMPARE(controller.selectedDraftPreview(), original);
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(controller.selectedDraftPreview(), inserted);
    QVERIFY(controller.save());

    PDFDocument reopenedDocument;
    PDFEditController reopened;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(saved)));
    reopened.setDocument(&reopenedDocument);
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(table.value(QStringLiteral("tableCellTexts")), inserted.value(QStringLiteral("tableCellTexts")));
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")), inserted.value(QStringLiteral("tableCellFormats")));
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    const auto beforeRemoval = reopened.selectedDraftPreview();
    const QString removeError = column ? reopened.removeSelectedTableColumns(QStringLiteral(" 5, 2, 2 "))
                                       : reopened.removeSelectedTableRows(QStringLiteral(" 5, 2, 2 "));
    QVERIFY2(removeError.isEmpty(), qPrintable(removeError));
    const auto removed = reopened.selectedDraftPreview();
    QCOMPARE(removed.value(QStringLiteral("tableRows")).toInt(), 3);
    QCOMPARE(removed.value(QStringLiteral("tableColumns")).toInt(), 3);
    const QVector<int> keptTracks{0, 2, 3}; // Original indices, sorted and deduplicated before removal.
    const auto keptTexts = removed.value(QStringLiteral("tableCellTexts")).toStringList();
    const auto keptFormats = removed.value(QStringLiteral("tableCellFormats")).toList();
    const auto keptRects = removed.value(QStringLiteral("tableCellRects")).toList();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int oldIndex = (column ? row : keptTracks.at(row)) * columns + (column ? keptTracks.at(col) : col);
            const int index = row * 3 + col;
            QCOMPARE(keptTexts.at(index), texts.at(oldIndex));
            QCOMPARE(keptFormats.at(index), formats.at(oldIndex));
            const auto size = keptRects.at(index).toRectF().size();
            const auto oldSize = rects.at(oldIndex).toRectF().size();
            QVERIFY(qAbs(size.width() - oldSize.width()) < .000001);
            QVERIFY(qAbs(size.height() - oldSize.height()) < .000001);
        }
    }
    reopened.undo();
    QCOMPARE(reopened.selectedDraftPreview(), beforeRemoval);
    QVERIFY(!reopened.hasDrafts());
    reopened.redo();
    QCOMPARE(reopened.selectedDraftPreview(), removed);
    QVERIFY(reopened.save());
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(table.value(QStringLiteral("tableCellTexts")), removed.value(QStringLiteral("tableCellTexts")));
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")), removed.value(QStringLiteral("tableCellFormats")));
    QCOMPARE(table.value(QStringLiteral("tableRows")).toInt(), 3);
    QCOMPARE(table.value(QStringLiteral("tableColumns")).toInt(), 3);
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    reopened.deleteSelectedObject();
    QVERIFY(reopened.save());
    QCOMPARE(topLevelPageObjectCount(saved), 0); // No children remain from either batch operation.
}

void PDFEditControllerTest::rejectsInvalidTableTrackBatches()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("batch-tracks-invalid.pdf"));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.5, .5)).isEmpty());
    const auto before = controller.selectedDraftPreview();
    for (int count : {-1, 0, 18, 1000000}) {
        QVERIFY(!controller.insertSelectedTableRows(1, count).isEmpty());
        QVERIFY(!controller.insertSelectedTableColumns(1, count).isEmpty());
        QCOMPARE(controller.selectedDraftPreview(), before);
    }
    for (int index : {-1, 4}) {
        QVERIFY(!controller.insertSelectedTableRows(index, 2).isEmpty());
        QVERIFY(!controller.insertSelectedTableColumns(index, 2).isEmpty());
        QCOMPARE(controller.selectedDraftPreview(), before);
    }
    for (const auto &indices : {QString(), QStringLiteral(" "), QStringLiteral("1,,2"), QStringLiteral("1,"),
            QStringLiteral("0"), QStringLiteral("-1"), QStringLiteral("4"), QStringLiteral("1.5"),
            QStringLiteral("1-2"), QStringLiteral("x"), QStringLiteral("1,2,3"), QStringLiteral("3,2,1,1")}) {
        QVERIFY2(!controller.removeSelectedTableRows(indices).isEmpty(), qPrintable(indices));
        QVERIFY2(!controller.removeSelectedTableColumns(indices).isEmpty(), qPrintable(indices));
        QCOMPARE(controller.selectedDraftPreview(), before);
    }
    controller.undo(); // All rejections leave only creation on the history stack.
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.addShape(0, QPointF(.6, .6), QPointF(.95, .95)).isEmpty());
    const auto nearEdge = controller.selectedDraftPreview();
    QVERIFY(!controller.insertSelectedTableRows(0, 2).isEmpty());
    QVERIFY(!controller.insertSelectedTableColumns(0, 2).isEmpty());
    QCOMPARE(controller.selectedDraftPreview(), nearEdge);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::insertsImportedTableFrameTracks()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("insert-imported-frame.pdf"));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("insert-imported-frame-saved.pdf"));
    QVERIFY(createImportedTableFrame(path, false, 0));
    const auto originalText = importedFrameTextSnapshot(path);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    auto table = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(table.value(QStringLiteral("tableFrameOnly")).toBool());
    QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(.1, .1, .4, .4));
    controller.finishSelectedFrameResize();
    QVERIFY(controller.insertSelectedTableRow(1).isEmpty());
    QVERIFY(controller.insertSelectedTableColumn(0).isEmpty());
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableRows")).toInt(), 3);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumns")).toInt(), 4);
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList().isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QCOMPARE(importedFrameTextSnapshot(saved), originalText);
    QCOMPARE(inspectShapePaths(saved).count, 1);
    table = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(table.value(QStringLiteral("tableFrameOnly")).toBool());
    QCOMPARE(table.value(QStringLiteral("tableRows")).toInt(), 3);
    QCOMPARE(table.value(QStringLiteral("tableColumns")).toInt(), 4);
    QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.insertSelectedTableColumn(4).isEmpty());
    QVERIFY(controller.save());
    QCOMPARE(importedFrameTextSnapshot(saved), originalText);
    table = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    for (int row = 3; row > 1; --row)
        QVERIFY(controller.removeSelectedTableRow().isEmpty());
    for (int column = 5; column > 1; --column)
        QVERIFY(controller.removeSelectedTableColumn().isEmpty());
    QVERIFY(controller.save());
    QCOMPARE(importedFrameTextSnapshot(saved), originalText);
    table = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(table.value(QStringLiteral("tableFrameOnly")).toBool());
    QCOMPARE(table.value(QStringLiteral("tableRows")).toInt(), 1);
    QCOMPARE(table.value(QStringLiteral("tableColumns")).toInt(), 1);
    QCOMPARE(inspectShapePaths(saved).count, 1);
}

void PDFEditControllerTest::editsTableDividersWithAtomicHistory()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("table-divider-history.pdf"));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableColumns(4);
    QVERIFY(!controller.addShape(0, QPointF(.2, .2), QPointF(.8, .8)).isEmpty());
    const QRectF frame = controller.selectedFrameRect();
    const QString objectId = controller.selectedObjectId();
    const auto stops = [&](bool column) {
        return controller.selectedDraftPreview().value(column
            ? QStringLiteral("tableColumnStops") : QStringLiteral("tableRowStops")).toList();
    };
    controller.updateSelectedTableDivider(true, 0, .15); // Requires a gesture.
    QVERIFY(stops(true).isEmpty());
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(true, -1, .15);
    controller.updateSelectedTableDivider(true, 3, .15);
    controller.updateSelectedTableDivider(false, 0, std::numeric_limits<qreal>::quiet_NaN());
    QVERIFY(stops(true).isEmpty());
    QVERIFY(stops(false).isEmpty());
    controller.updateSelectedTableDivider(true, 0, .2);
    controller.updateSelectedTableDivider(true, 0, .15);
    controller.finishSelectedFrameResize();
    QCOMPARE(stops(true), QVariantList({.15, .5, .75}));
    QCOMPARE(controller.selectedFrameRect(), frame);
    QCOMPARE(controller.tableColumns(), 4);
    controller.undo();
    QVERIFY(stops(true).isEmpty());
    controller.undo(); // Only creation remains below the completed gesture.
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    controller.redo();
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(stops(true), QVariantList({.15, .5, .75}));

    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(false, 0, .2);
    controller.cancelSelectedFrameResize();
    QVERIFY(stops(false).isEmpty());
    QCOMPARE(stops(true), QVariantList({.15, .5, .75}));
    controller.beginSelectedTextEdit();
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(true, 1, .6);
    controller.cancelSelectedFrameResize(); // The enclosing edit remains active.
    QCOMPARE(stops(true), QVariantList({.15, .5, .75}));
    controller.finishSelectedTextEdit();
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(true, 0, 1.0);
    QCOMPARE(stops(true).at(0).toDouble(), .495);
    controller.updateSelectedTableDivider(false, 0, -1.0);
    QCOMPARE(stops(false).at(0).toDouble(), .005);
    controller.cancelSelectedFrameResize();
    const QRectF resized(.1, .1, .7, .5);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resized);
    controller.finishSelectedFrameResize();
    QCOMPARE(stops(true), QVariantList({.15, .5, .75}));
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.undo();
    QVERIFY(stops(true).isEmpty());
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(true, 0, .15);
    controller.updateSelectedTableDivider(true, 0, .25);
    controller.finishSelectedFrameResize(); // Returning to start creates no history.
    QVERIFY(stops(true).isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.setShapeType(PDFEditController::RectangleShape);
    QVERIFY(!controller.addShape(0, QPointF(.2, .2), QPointF(.8, .8)).isEmpty());
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(true, 0, .1);
    controller.finishSelectedFrameResize();
    QVERIFY(stops(true).isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::savesUnevenTableDividers_data()
{
    QTest::addColumn<int>("rotation");
    for (int rotation = 0; rotation < 4; ++rotation)
        QTest::newRow(qPrintable(QString::number(rotation))) << rotation;
}

void PDFEditControllerTest::savesUnevenTableDividers()
{
    QFETCH(int, rotation);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("uneven-%1.pdf").arg(rotation));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("uneven-saved-%1.pdf").arg(rotation));
    QVERIFY(createTableTestPage(path, rotation));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableColumns(4);
    QVERIFY(!controller.addShape(0, QPointF(.15, .2), QPointF(.75, .7)).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QCOMPARE(readTableTestMetadata(saved).value(QStringLiteral("version")).toInt(), 2);
    const auto original = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(controller.selectCandidate(0, original.value(QStringLiteral("objectId")).toString()));
    const QRectF frame = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(true, 0, .12);
    controller.updateSelectedTableDivider(true, 2, .87);
    controller.updateSelectedTableDivider(false, 0, .2);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QVERIFY(controller.save());
    const auto metadata = readTableTestMetadata(saved);
    QCOMPARE(metadata.value(QStringLiteral("version")).toInt(), 3);
    QCOMPARE(metadata.value(QStringLiteral("columnStops")).toArray(), QJsonArray({.12, .5, .87}));
    QCOMPARE(metadata.value(QStringLiteral("rowStops")).toArray(), QJsonArray({.2, 2.0 / 3}));
    // Check actual PDF path coordinates independently of its metadata.
    const auto inspection = inspectShapePaths(saved);
    QCOMPARE(inspection.count, 1);
    const auto points = inspection.normalizedPathPoints.constFirst();
    QCOMPARE(points.size(), 16);
    qreal left = 1, right = 0, top = 1, bottom = 0;
    for (const auto &point : points) {
        left = qMin(left, point.x()); right = qMax(right, point.x());
        top = qMin(top, point.y()); bottom = qMax(bottom, point.y());
    }
    for (int i = 0; i < 2; ++i) {
        const qreal ratio = (points.at(6 + i * 2).y() - top) / (bottom - top);
        QVERIFY(qAbs(ratio - (i == 0 ? .2 : 2.0 / 3)) < .0001);
    }
    const QList<qreal> columns{.12, .5, .87};
    for (int i = 0; i < 3; ++i) {
        const qreal ratio = (points.at(10 + i * 2).x() - left) / (right - left);
        QVERIFY(qAbs(ratio - columns.at(i)) < .0001);
    }
    PDFDocument reopenedDocument;
    PDFEditController reopened;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(saved)));
    reopened.setDocument(&reopenedDocument);
    const auto source = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QVERIFY(!source.isEmpty());
    QCOMPARE(source.value(QStringLiteral("tableColumnStops")).toList(), QVariantList({.12, .5, .87}));
    QVERIFY(reopened.selectCandidate(0, source.value(QStringLiteral("objectId")).toString()));
    reopened.beginSelectedFrameResize();
    reopened.updateSelectedTableDivider(false, 1, .8);
    reopened.updateSelectedFrameRect(QRectF(.2, .25, .5, .4));
    reopened.finishSelectedFrameResize();
    QVERIFY(reopened.save());
    const auto again = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(again.value(QStringLiteral("tableRowStops")).toList(), QVariantList({.2, .8}));
    QCOMPARE(again.value(QStringLiteral("tableColumnStops")).toList(), QVariantList({.12, .5, .87}));
}

void PDFEditControllerTest::rejectsInvalidTableDividers_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QVariant>("value");
    QTest::newRow("missing") << QStringLiteral("rowStops") << QVariant();
    QTest::newRow("not-array") << QStringLiteral("rowStops") << QVariant(QStringLiteral("0.2,0.7"));
    QTest::newRow("empty") << QStringLiteral("rowStops") << QVariant(QVariantList{});
    QTest::newRow("wrong-count") << QStringLiteral("rowStops") << QVariant(QVariantList{.2});
    QTest::newRow("reversed") << QStringLiteral("rowStops") << QVariant(QVariantList{.7, .2});
    QTest::newRow("coincident") << QStringLiteral("rowStops") << QVariant(QVariantList{.2, .2});
    QTest::newRow("too-close") << QStringLiteral("rowStops") << QVariant(QVariantList{.2, .20001});
    QTest::newRow("on-edge") << QStringLiteral("rowStops") << QVariant(QVariantList{0., .7});
    QTest::newRow("outside") << QStringLiteral("rowStops") << QVariant(QVariantList{.2, 1.1});
    QTest::newRow("string-number") << QStringLiteral("rowStops") << QVariant(QVariantList{QStringLiteral("0.2"), .7});
    QTest::newRow("path-disagrees") << QStringLiteral("rowStops") << QVariant(QVariantList{.3, .7});
    QTest::newRow("column-count") << QStringLiteral("columnStops") << QVariant(QVariantList{.1});
    QTest::newRow("downgraded") << QStringLiteral("version") << QVariant(2);
}

void PDFEditControllerTest::rejectsInvalidTableDividers()
{
    QFETCH(QString, key);
    QFETCH(QVariant, value);
    const QString suffix = QString::fromLatin1(QTest::currentDataTag());
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("divider-invalid-%1.pdf").arg(suffix));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("divider-valid-%1.pdf").arg(suffix));
    const QString tampered = m_temporaryDirectory.filePath(QStringLiteral("divider-tampered-%1.pdf").arg(suffix));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    QVERIFY(!controller.addShape(0, QPointF(.2, .2), QPointF(.8, .8)).isEmpty());
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(false, 0, .2);
    controller.updateSelectedTableDivider(false, 1, .7);
    controller.finishSelectedFrameResize();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QVERIFY(rewriteTableTestMetadata(saved, tampered, key, value));
    QVERIFY(document.open(QUrl::fromLocalFile(tampered)));
    QVERIFY(candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape")).isEmpty());
    QCOMPARE(inspectShapePaths(tampered).count, 1);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::editsTableCellFormatsWithAtomicHistory()
{
    using namespace PDFTableCellFormat;
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("cell-format-history.pdf"));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    const Style regular{testFontFamily()};
    const QString text = QStringLiteral("Plain Bold Italic Under");
    const Cell format{regular, {{0, 6, regular}, {6, 5, {regular.family, true}},
        {11, 7, {regular.family, false, true}}, {18, 5, {regular.family, false, false, true}}}};
    const auto formats = [&] { return controller.selectedDraftPreview().value(QStringLiteral("tableCellFormats")).toList(); };
    const QVariantMap expected = toVariant(format);
    const QString error = controller.setSelectedTableCellContent(0, 0, text, expected);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(formats(), QVariantList{expected});
    QVERIFY(controller.setSelectedTableCellContent(0, 0, text, expected).isEmpty());
    controller.undo(); // Text and all range formatting form one document command.
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList().isEmpty());
    QVERIFY(formats().isEmpty());
    controller.redo();
    QCOMPARE(formats(), QVariantList{expected});
    Cell changed = format;
    changed.spans[0].style.underline = true;
    changed.spans[0].style.fontSize = 24;
    QVERIFY(controller.setSelectedTableCellContent(0, 0, text, toVariant(changed)).isEmpty());
    QCOMPARE(formats(), QVariantList{toVariant(changed)});
    controller.undo();
    QCOMPARE(formats(), QVariantList{expected});
    controller.undo();
    controller.undo(); // Identical formatting did not append a hidden history entry.
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::rejectsInvalidTableCellFormats()
{
    using namespace PDFTableCellFormat;
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("cell-format-invalid.pdf"));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    const QString text = QStringLiteral("Bounded text");
    const Cell validFormat = plain(text, {testFontFamily(), true, true, true});
    const auto reject = [&](const QVariantMap &map) {
        return !controller.setSelectedTableCellContent(0, 0, text, map).isEmpty();
    };
    for (const QString &key : {QStringLiteral("start"), QStringLiteral("length"), QStringLiteral("bold")}) {
        QVariantMap map = toVariant(validFormat);
        QVariantList spans = map.value(QStringLiteral("spans")).toList();
        QVariantMap span = spans.first().toMap();
        span[key] = key == QLatin1String("bold") ? QVariant(QStringLiteral("true")) : QVariant(.5);
        spans[0] = span;
        map[QStringLiteral("spans")] = spans;
        QVERIFY2(reject(map), qPrintable(key));
    }
    for (const QVariant &size : {QVariant(0), QVariant(-1), QVariant(513), QVariant(12.5),
                                 QVariant(QStringLiteral("18")), QVariant(true)}) {
        for (const bool defaultStyle : {false, true}) {
            QVariantMap map = toVariant(validFormat);
            if (defaultStyle) {
                QVariantMap style = map.value(QStringLiteral("defaultStyle")).toMap();
                style[QStringLiteral("fontSize")] = size;
                map[QStringLiteral("defaultStyle")] = style;
            } else {
                QVariantList spans = map.value(QStringLiteral("spans")).toList();
                QVariantMap span = spans.first().toMap();
                span[QStringLiteral("fontSize")] = size;
                spans[0] = span;
                map[QStringLiteral("spans")] = spans;
            }
            QVERIFY2(reject(map), qPrintable(size.toString()));
        }
    }
    Cell oversized = validFormat;
    oversized.spans[0].style.fontSize = 512;
    QVERIFY(reject(toVariant(oversized))); // A legal size can still exceed the cell's frame.
    Cell gap = validFormat;
    gap.spans[0].start = 1;
    QVERIFY(reject(toVariant(gap)));
    Cell overlap = validFormat;
    overlap.spans.append(validFormat.spans.first());
    QVERIFY(reject(toVariant(overlap)));
    Cell missingFace = plain(text, {QStringLiteral("TrePDFNoSuchTableFont"), true});
    QVERIFY(reject(toVariant(missingFace)));
    const QString regularOnly = QStringLiteral("TrePDFTableRegularOnly");
    QString fontError;
    QVERIFY2(ApplicationFonts::installDownloadedFont(renamedTestFontData(regularOnly, false, false),
        regularOnly, false, false, uint('a'), fontError), qPrintable(fontError));
    QVERIFY(reject(toVariant(plain(text, {regularOnly, true}))));
    const QString overflow = QStringLiteral("line\n").repeated(70);
    QVERIFY(!controller.setSelectedTableCellContent(0, 0, overflow,
        toVariant(plain(overflow, {testFontFamily(), true}))).isEmpty());
    const QString surrogate = QString::fromUcs4(U"\U0001F600");
    Cell split{{testFontFamily()}, {{0, 1, {testFontFamily()}}, {1, 1, {testFontFamily(), true}}}};
    QVERIFY(!controller.setSelectedTableCellContent(0, 0, surrogate, toVariant(split)).isEmpty());
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList().isEmpty());
    QVERIFY(controller.setSelectedTableCellContent(0, 0, text,
        toVariant(plain(text, {testFontFamily(), false, false, false, 1}))).isEmpty());
    controller.undo();
    QVERIFY(controller.setSelectedTableCellContent(0, 0, QString(),
        toVariant(Cell{{testFontFamily(), false, false, false, 512}, {}})).isEmpty());
    controller.undo(); // Boundary sizes are valid when their content fits.
    controller.undo();
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::rejectsTableCellInkOverflowAndPreservesPaddedText()
{
    using namespace PDFTableCellFormat;
    for (int rotation = 0; rotation < 4; ++rotation) {
        const QString path = m_temporaryDirectory.filePath(QStringLiteral("cell-ink-%1.pdf").arg(rotation));
        const QString saved = m_temporaryDirectory.filePath(QStringLiteral("cell-ink-saved-%1.pdf").arg(rotation));
        QVERIFY(createTableTestPage(path, rotation));
        PDFDocument document;
        PDFEditController controller;
        QVERIFY(document.open(QUrl::fromLocalFile(path)));
        controller.setDocument(&document);
        controller.setShapeType(PDFEditController::TableShape);
        controller.setTableRows(2);
        controller.setTableColumns(1);
        QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());

        const QString text = QStringLiteral("j");
        const Cell large = plain(text, {testFontFamily(), false, true, true, 72});
        // Its 16-point advance fits, but the italic glyph paints about eight
        // points to the left. Reject it before Save would lose table ownership.
        QVERIFY(!controller.setSelectedTableCellContent(0, 0, text, toVariant(large)).isEmpty());
        QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList().isEmpty());

        const Cell small = plain(text, {testFontFamily(), false, true, true, 12});
        QVERIFY(controller.setSelectedTableCellContent(0, 0, text, toVariant(small)).isEmpty());
        const QString padded = QStringLiteral(" j");
        const Cell inset = plain(padded, {testFontFamily(), false, true, true, 72});
        QVERIFY(controller.setSelectedTableCellContent(1, 0, padded, toVariant(inset)).isEmpty());
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
        QCOMPARE(tableTextFontSizes(saved), QSet<int>({12, 72}));
        QCOMPARE(inspectShapePaths(saved).count, 4); // Grid; padding space and glyphs have separate underlines.

        PDFDocument reopenedDocument;
        PDFEditController reopened;
        QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(saved)));
        reopened.setDocument(&reopenedDocument);
        const auto table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
        QVERIFY2(!table.isEmpty(), "Accepted large italic text must remain a managed table after reopening.");
        QCOMPARE(table.value(QStringLiteral("tableCellTexts")).toStringList(), QStringList({text, padded}));
        QCOMPARE(table.value(QStringLiteral("tableCellFormats")).toList(), QVariantList({toVariant(small), toVariant(inset)}));
    }
}

void PDFEditControllerTest::savesFormattedTableCellsRoundTrip_data()
{
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("mixedSizes");
    for (int rotation = 0; rotation < 4; ++rotation) {
        QTest::newRow(qPrintable(QStringLiteral("%1-regular-size").arg(rotation * 90))) << rotation << false;
        QTest::newRow(qPrintable(QStringLiteral("%1-mixed-sizes").arg(rotation * 90))) << rotation << true;
    }
}

void PDFEditControllerTest::savesFormattedTableCellsRoundTrip()
{
    using namespace PDFTableCellFormat;
    QFETCH(int, rotation);
    QFETCH(bool, mixedSizes);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("formatted-cell-%1.pdf").arg(rotation));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("formatted-cell-saved-%1.pdf").arg(rotation));
    QVERIFY(createTableTestPage(path, rotation));
    const QString alternate = QStringLiteral("TrePDFTableAlternate");
    QString fontError;
    QVERIFY2(ApplicationFonts::installDownloadedFont(renamedTestFontData(alternate, false, true),
        alternate, false, true, uint('a'), fontError), qPrintable(fontError));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(2);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    const QString text = QStringLiteral("Plain Bold Italic Under")
        + (mixedSizes ? QStringLiteral("\n") : QString());
    const Style regular{testFontFamily()};
    Cell format{regular, {{0, 6, regular}, {6, 5, {regular.family, true}},
        {11, 7, {alternate, false, true}}, {18, int(text.size()) - 18, {regular.family, false, false, true}}}};
    Cell empty{{regular.family, true, true, true}, {}};
    if (mixedSizes) {
        format.spans[0].style.fontSize = 9;
        format.spans[1].style.fontSize = 18;
        format.spans[2].style.fontSize = 24;
        format.spans[3].style.fontSize = 20;
        empty.defaultStyle.fontSize = 22;
    }
    const QString error = controller.setSelectedTableCellContent(0, 0, text, toVariant(format));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(controller.setSelectedTableCellContent(1, 0, QString(), toVariant(empty)).isEmpty());
    const QVariantList expected{toVariant(format), toVariant(empty)};
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QCOMPARE(readTableTestMetadata(saved).value(QStringLiteral("version")).toInt(), 6);
    QCOMPARE(tableTextFontSizes(saved), mixedSizes ? QSet<int>({9, 18, 20, 24}) : QSet<int>({12}));
    QVERIFY(inspectShapePaths(saved).count > 1); // Grid plus a real vector underline.
    QVERIFY(pageText(saved).contains(text.trimmed()));
    const auto bold = textObjectFontInfo(saved, QStringLiteral("Bold"));
    QVERIFY(bold.found);
    QVERIFY(bold.weight >= 600);
    QCOMPARE(bold.embedded, 1);
    const auto italic = textObjectFontInfo(saved, QStringLiteral("Italic"));
    QVERIFY(italic.found);
    QVERIFY(italic.italic);
    QCOMPARE(italic.familyName, alternate);
    PDFDocument reopenedDocument;
    PDFEditController reopened;
    if (!mixedSizes) {
        const QString legacy = m_temporaryDirectory.filePath(QStringLiteral("formatted-cell-legacy-%1.pdf").arg(rotation));
        QVERIFY(writeLegacyTableFormats(saved, legacy));
        QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(legacy)));
        reopened.setDocument(&reopenedDocument);
        const auto oldTable = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
        QVERIFY2(!oldTable.isEmpty(), "Version 5 tables without fontSize fields remain editable.");
        QCOMPARE(oldTable.value(QStringLiteral("tableCellFormats")).toList(), expected);
    }
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(saved)));
    reopened.setDocument(&reopenedDocument);
    auto candidates = waitForCandidates(reopened, 0);
    auto table = candidateForKind(candidates, QStringLiteral("sourceShape"));
    QVERIFY2(!table.isEmpty(), "A rich table and its underline children must rehydrate together.");
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")).toList(), expected);
    QCOMPARE(tableTextFontSizes(saved), mixedSizes ? QSet<int>({9, 18, 20, 24}) : QSet<int>({12}));
    for (const auto &candidate : candidates) {
        if (candidate.toMap().value(QStringLiteral("objectId")) != table.value(QStringLiteral("objectId")))
            QVERIFY(!candidate.toMap().value(QStringLiteral("editable")).toBool());
    }
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    reopened.beginSelectedFrameResize();
    reopened.updateSelectedTableDivider(false, 0, .55);
    reopened.updateSelectedFrameRect(QRectF(.12, .12, .75, .75));
    reopened.finishSelectedFrameResize();
    QVERIFY(reopened.save());
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")).toList(), expected);
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    const QString changed = QStringLiteral("Changed");
    QVERIFY(reopened.setSelectedTableCellContent(0, 0, changed, toVariant(plain(changed, regular))).isEmpty());
    QVERIFY(reopened.save());
    QCOMPARE(inspectShapePaths(saved).count, 1); // No orphan underline from previous content.
    QVERIFY(pageText(saved).contains(changed));
    QVERIFY(!pageText(saved).contains(QStringLiteral("Under")));
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")).toList().at(1).toMap(), toVariant(empty));
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    reopened.deleteSelectedObject();
    reopened.undo();
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(reopened.selectedDraftPreview().value(QStringLiteral("tableCellFormats")).toList().at(1).toMap(), toVariant(empty));
    reopened.deleteSelectedObject();
    QVERIFY(reopened.save());
    QCOMPARE(topLevelPageObjectCount(saved), 0);
    QVERIFY(pageText(saved).trimmed().isEmpty());
}

void PDFEditControllerTest::editsTableCellRotationWithAtomicHistory()
{
    using namespace PDFTableCellFormat;
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("cell-rotation-history.pdf"));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .25)).isEmpty());
    const Style regular{testFontFamily()};
    const QString text = QStringLiteral("Rotation");
    Cell format = plain(text, regular);
    format.rotation = 90;
    QVERIFY(controller.setSelectedTableCellContent(0, 0, text, toVariant(format)).isEmpty());
    const auto rotation = [&] {
        return controller.selectedDraftPreview().value(QStringLiteral("tableCellFormats")).toList()
            .value(0).toMap().value(QStringLiteral("rotation")).toInt();
    };
    QCOMPARE(rotation(), 90);
    QVERIFY(controller.setSelectedTableCellContent(0, 0, text, toVariant(format)).isEmpty());
    controller.undo(); // An identical rotation does not create an extra command.
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList().isEmpty());
    controller.redo();
    QCOMPARE(rotation(), 90);
    QVERIFY(controller.setSelectedTableCellText(0, 0, QStringLiteral("Changed")).isEmpty());
    QCOMPARE(rotation(), 90); // Plain text changes retain the cell-level property.
    controller.undo();
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList(), QStringList{text});
    const QVariantMap before = controller.selectedDraftPreview();
    for (const QVariant &angle : {QVariant(-90), QVariant(45), QVariant(360), QVariant(90.5),
                                  QVariant(QStringLiteral("90")), QVariant(true)}) {
        QVariantMap invalid = toVariant(format);
        invalid[QStringLiteral("rotation")] = angle;
        QVERIFY2(!controller.setSelectedTableCellContent(0, 0, text, invalid).isEmpty(), qPrintable(angle.toString()));
        QCOMPARE(controller.selectedDraftPreview(), before);
    }
    Cell empty = plain(QString(), regular);
    empty.rotation = 270;
    QVERIFY(controller.setSelectedTableCellContent(0, 0, QString(), toVariant(empty)).isEmpty());
    QCOMPARE(rotation(), 270);
    controller.undo();
    QCOMPARE(rotation(), 90);
    // The rotated layout has a tall logical frame; the unrotated frame is too short.
    const QString lines = QStringLiteral("x\n").repeated(14) + QStringLiteral("x");
    Cell tall = plain(lines, regular);
    QVERIFY(!controller.setSelectedTableCellContent(0, 0, lines, toVariant(tall)).isEmpty());
    tall.rotation = 90;
    QVERIFY(controller.setSelectedTableCellContent(0, 0, lines, toVariant(tall)).isEmpty());
    QCOMPARE(rotation(), 90);
    controller.undo();
    QCOMPARE(controller.selectedDraftPreview(), before);
    controller.undo();
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::savesRotatedTableCellsRoundTrip_data()
{
    QTest::addColumn<int>("pageRotation");
    QTest::addColumn<int>("cellRotation");
    for (int pageRotation : {0, 1}) {
        for (int cellRotation : {0, 90, 180, 270})
            QTest::newRow(qPrintable(QStringLiteral("page-%1-cell-%2").arg(pageRotation * 90).arg(cellRotation)))
                << pageRotation << cellRotation;
    }
}

void PDFEditControllerTest::savesRotatedTableCellsRoundTrip()
{
    using namespace PDFTableCellFormat;
    QFETCH(int, pageRotation);
    QFETCH(int, cellRotation);
    const QString suffix = QStringLiteral("%1-%2").arg(pageRotation).arg(cellRotation);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("rotated-cell-%1.pdf").arg(suffix));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("rotated-cell-saved-%1.pdf").arg(suffix));
    QVERIFY(createTableTestPage(path, pageRotation));
    const QString alternate = QStringLiteral("TrePDFRotatedCellAlternate");
    QString fontError;
    QVERIFY2(ApplicationFonts::installDownloadedFont(renamedTestFontData(alternate, false, true),
        alternate, false, true, uint('a'), fontError), qPrintable(fontError));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(4);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    const QString text = QStringLiteral("Plain Bold Italic Under\nNext");
    const Style regular{testFontFamily()};
    Cell format{regular, {{0, 6, {regular.family, false, false, false, 9}},
        {6, 5, {regular.family, true, false, false, 18}},
        {11, 7, {alternate, false, true, false, 16}},
        {18, int(text.size()) - 18, {regular.family, false, false, true, 20}}}};
    format.rotation = cellRotation;
    const QString spaces = QStringLiteral("   ");
    Cell spaceFormat = plain(spaces, {regular.family, false, false, true, 24});
    spaceFormat.rotation = cellRotation;
    Cell empty{{regular.family, true, true, true, 22}, {}};
    empty.rotation = cellRotation;
    const QString error = controller.setSelectedTableCellContent(0, 0, text, toVariant(format));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(controller.setSelectedTableCellContent(1, 0, spaces, toVariant(spaceFormat)).isEmpty());
    QVERIFY(controller.setSelectedTableCellContent(2, 0, QString(), toVariant(empty)).isEmpty());
    const QString zeroText = QStringLiteral("Zero");
    const Cell zeroFormat = plain(zeroText, regular);
    QVERIFY(controller.setSelectedTableCellContent(3, 0, zeroText, toVariant(zeroFormat)).isEmpty());
    const QVariantList expected{toVariant(format), toVariant(spaceFormat), toVariant(empty), toVariant(zeroFormat)};
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QCOMPARE(readTableTestMetadata(saved).value(QStringLiteral("version")).toInt(), cellRotation ? 8 : 6);
    const auto directions = tableCellTextDirections(saved);
    QVERIFY(!directions.isEmpty());
    const QPointF expectedDirection = cellRotation == 0 ? QPointF(1, 0)
        : cellRotation == 90 ? QPointF(0, 1) : cellRotation == 180 ? QPointF(-1, 0) : QPointF(0, -1);
    for (qsizetype index = 0; index < directions.size(); ++index) {
        const QPointF expected = index + 1 == directions.size() ? QPointF(1, 0) : expectedDirection;
        QVERIFY2(QLineF(directions[index], expected).length() < .001,
            "PDF text axes must rotate in displayed page coordinates without rotating neighboring cells.");
    }
    QVERIFY(pageText(saved).contains(QStringLiteral("Plain")));
    const auto bold = textObjectFontInfo(saved, QStringLiteral("Bold"));
    QVERIFY(bold.found && bold.weight >= 600 && bold.embedded == 1);
    const auto italic = textObjectFontInfo(saved, QStringLiteral("Italic"));
    QVERIFY(italic.found && italic.italic);
    QCOMPARE(italic.familyName, alternate);
    QCOMPARE(tableTextFontSizes(saved), QSet<int>({9, 12, 16, 18, 20, 24}));
    QVERIFY(inspectShapePaths(saved).count > 2);
    PDFDocument reopenedDocument;
    PDFEditController reopened;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(saved)));
    reopened.setDocument(&reopenedDocument);
    auto candidates = waitForCandidates(reopened, 0);
    auto table = candidateForKind(candidates, QStringLiteral("sourceShape"));
    QVERIFY2(!table.isEmpty(), "Rotated text, spaces and underline children must remain one editable table.");
    QCOMPARE(table.value(QStringLiteral("tableCellTexts")).toStringList(), QStringList({text, spaces, QString(), zeroText}));
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")).toList(), expected);
    for (const auto &candidate : candidates) {
        if (candidate.toMap().value(QStringLiteral("objectId")) != table.value(QStringLiteral("objectId")))
            QVERIFY(!candidate.toMap().value(QStringLiteral("editable")).toBool());
    }
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    reopened.beginSelectedFrameResize();
    reopened.updateSelectedTableDivider(false, 0, .4);
    reopened.updateSelectedFrameRect(QRectF(.12, .12, .75, .75));
    reopened.finishSelectedFrameResize();
    QVERIFY(reopened.save());
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")).toList(), expected);
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    const QString changed = QStringLiteral("Changed");
    Cell changedFormat = plain(changed, regular);
    changedFormat.rotation = cellRotation;
    QVERIFY(reopened.setSelectedTableCellContent(0, 0, changed, toVariant(changedFormat)).isEmpty());
    QVERIFY(reopened.save());
    QVERIFY(pageText(saved).contains(changed));
    QVERIFY(!pageText(saved).contains(QStringLiteral("Under")));
    QCOMPARE(inspectShapePaths(saved).count, 2); // Only the grid and spaces' underline remain.
    table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(table.value(QStringLiteral("tableCellFormats")).toList().at(2).toMap(), toVariant(empty));
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    reopened.deleteSelectedObject();
    reopened.undo();
    QVERIFY(!reopened.hasDrafts());
    reopened.redo();
    QVERIFY(reopened.save());
    QCOMPARE(topLevelPageObjectCount(saved), 0);
    QVERIFY(pageText(saved).trimmed().isEmpty());
}

void PDFEditControllerTest::rejectsTamperedTableCellRotation_data()
{
    QTest::addColumn<QString>("change");
    for (const auto *change : {"invalid-angle", "missing-angle", "parent-rotation",
                               "legacy-rotation", "child-rotation", "rotated-fragment"})
        QTest::newRow(change) << QString::fromLatin1(change);
}

void PDFEditControllerTest::rejectsTamperedTableCellRotation()
{
    using namespace PDFTableCellFormat;
    QFETCH(QString, change);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("rotation-valid-%1.pdf").arg(change));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("rotation-saved-%1.pdf").arg(change));
    const QString tampered = m_temporaryDirectory.filePath(QStringLiteral("rotation-tampered-%1.pdf").arg(change));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    const QString text = QStringLiteral("Rotated");
    Cell format = plain(text, {testFontFamily(), false, false, true});
    format.rotation = 90;
    QVERIFY(controller.setSelectedTableCellContent(0, 0, text, toVariant(format)).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    if (change == QLatin1String("child-rotation") || change == QLatin1String("rotated-fragment")) {
        QVERIFY(tamperTableCellObject(saved, tampered, change));
    } else if (change == QLatin1String("legacy-rotation")) {
        QVERIFY(rewriteTableTestMetadata(saved, tampered, QStringLiteral("version"), 6));
    } else {
        auto altered = toVariant(format);
        if (change == QLatin1String("missing-angle"))
            altered.remove(QStringLiteral("rotation"));
        else
            altered[QStringLiteral("rotation")] = change == QLatin1String("invalid-angle") ? 45 : 270;
        QVERIFY(rewriteTableTestMetadata(saved, tampered, QStringLiteral("cellFormats"), QVariantList{altered}));
    }
    QVERIFY(document.open(QUrl::fromLocalFile(tampered)));
    const auto candidates = waitForCandidates(controller, 0);
    QVERIFY(candidateForKind(candidates, QStringLiteral("sourceShape")).isEmpty());
    for (const auto &candidate : candidates)
        QVERIFY(!candidate.toMap().value(QStringLiteral("editable")).toBool());
}

void PDFEditControllerTest::reopensTableCellsWithGeneratedSpacing_data()
{
    QTest::addColumn<QString>("firstCellText");
    QTest::newRow("short") << QStringLiteral("Alpha");
    QTest::newRow("maximum-length")
        << QString(PDFTableCellLayout::MaximumCellCharacters, QLatin1Char('i'));
}

void PDFEditControllerTest::reopensTableCellsWithGeneratedSpacing()
{
    using namespace PDFTableCellFormat;
    QFETCH(QString, firstCellText);
    const bool maximumLength = firstCellText.size() == PDFTableCellLayout::MaximumCellCharacters;
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("table-neighbor-source.pdf"));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("table-neighbor-saved.pdf"));
    const QString legacy = m_temporaryDirectory.filePath(QStringLiteral("table-neighbor-legacy.pdf"));
    if (maximumLength) {
        // Keep all 1,024 characters in one 12-point physical run, including
        // when the fixture is downgraded to v5's fixed 12-point format.
        QVERIFY(writePdfObjects(path, {
            QByteArrayLiteral("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArrayLiteral("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArrayLiteral("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 12000 800] "
                              "/Resources << >> /Contents 4 0 R >>"),
            QByteArrayLiteral("<< /Length 0 >>\nstream\n\nendstream")}));
    } else {
        QVERIFY(createTableTestPage(path, 0));
    }
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(3);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .4)).isEmpty());
    const QStringList contents{firstCellText, QStringLiteral("Bravo "), QStringLiteral("Charlie")};
    for (int column = 0; column < contents.size(); ++column)
        QVERIFY(controller.setSelectedTableCellContent(0, column, contents[column],
            toVariant(plain(contents[column], {testFontFamily()}))).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    {
        PDFiumProcessLock lock;
        const QByteArray filename = QFile::encodeName(saved);
        FPDF_DOCUMENT pdf = FPDF_LoadDocument(filename.constData(), nullptr);
        QVERIFY(pdf);
        const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(pdf); });
        FPDF_PAGE page = FPDF_LoadPage(pdf, 0);
        QVERIFY(page);
        const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        QVERIFY(textPage);
        const auto closeTextPage = qScopeGuard([&] { FPDFText_ClosePage(textPage); });
        const auto first = FPDFPage_GetObject(page, 1);
        const unsigned long length = FPDFTextObj_GetText(first, textPage, nullptr, 0);
        QCOMPARE(length, static_cast<unsigned long>(2 * (firstCellText.size() + 2)));
        if (maximumLength)
            QVERIFY(length > 2 * (PDFTableCellLayout::MaximumCellCharacters + 1));
        QVector<FPDF_WCHAR> text(length / 2);
        QCOMPARE(FPDFTextObj_GetText(first, textPage, text.data(), length), length);
        QCOMPARE(QString::fromUtf16(reinterpret_cast<const char16_t *>(text.constData()),
            text.size() - 1), firstCellText + QLatin1Char(' '));
        PDFTableCellMetadata::PageIndex index;
        QVERIFY(PDFTableCellMetadata::matchesObjectText(index, textPage, first, contents[0], {}));
        QVERIFY(!PDFTableCellMetadata::matchesObjectText(index, textPage, first,
            firstCellText + QLatin1Char(' '), {}));
    }
    QVERIFY(writeLegacyTableFormats(saved, legacy));
    for (const QString &file : {saved, legacy}) {
        PDFDocument reopenedDocument;
        PDFEditController reopened;
        QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(file)));
        reopened.setDocument(&reopenedDocument);
        const auto table = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
        QVERIFY2(!table.isEmpty(), "Generated inter-cell spacing must not invalidate the managed table.");
        QCOMPARE(table.value(QStringLiteral("tableCellTexts")).toStringList(), contents);
        QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
        QVERIFY(reopened.setSelectedTableCellText(0, 0, QStringLiteral("Edited")).isEmpty());
        QVERIFY(reopened.save());
        const auto again = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
        QVERIFY(!again.isEmpty());
        QCOMPARE(again.value(QStringLiteral("tableCellTexts")).toStringList(),
            QStringList({QStringLiteral("Edited"), contents[1], contents[2]}));
    }
}

void PDFEditControllerTest::savesUnderlinedTableSpaces()
{
    using namespace PDFTableCellFormat;
    for (int rotation = 0; rotation < 4; ++rotation) {
        const QString path = m_temporaryDirectory.filePath(QStringLiteral("space-cell-%1.pdf").arg(rotation));
        const QString saved = m_temporaryDirectory.filePath(QStringLiteral("space-cell-saved-%1.pdf").arg(rotation));
        QVERIFY(createTableTestPage(path, rotation));
        PDFDocument document;
        PDFEditController controller;
        QVERIFY(document.open(QUrl::fromLocalFile(path)));
        controller.setDocument(&document);
        controller.setShapeType(PDFEditController::TableShape);
        controller.setTableRows(1);
        controller.setTableColumns(1);
        QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
        const QString text = QStringLiteral("   ");
        const Cell format = plain(text, {testFontFamily(), false, false, true, 24});
        QVERIFY(controller.setSelectedTableCellContent(0, 0, text, toVariant(format)).isEmpty());
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
        QCOMPARE(inspectShapePaths(saved).count, 2); // Grid plus visible underline under spaces.
        auto table = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
        QVERIFY2(!table.isEmpty(), qPrintable(QStringLiteral("space-only group, rotation %1").arg(rotation)));
        QCOMPARE(table.value(QStringLiteral("tableCellTexts")).toStringList(), QStringList{text});
        QCOMPARE(table.value(QStringLiteral("tableCellFormats")).toList(), QVariantList{toVariant(format)});
        QVERIFY(controller.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
        controller.deleteSelectedObject();
        QVERIFY(controller.save());
        QCOMPARE(topLevelPageObjectCount(saved), 0);
    }
}

void PDFEditControllerTest::rejectsTamperedTableCellFormatting_data()
{
    QTest::addColumn<QString>("change");
    for (const auto &change : {QStringLiteral("missing-underline"), QStringLiteral("moved-underline"),
                              QStringLiteral("format-disagrees"), QStringLiteral("malformed-format"),
                              QStringLiteral("missing-space-pair"), QStringLiteral("size-disagrees"),
                              QStringLiteral("missing-size")})
        QTest::newRow(qPrintable(change)) << change;
}

void PDFEditControllerTest::rejectsTamperedTableCellFormatting()
{
    using namespace PDFTableCellFormat;
    QFETCH(QString, change);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("formatted-valid-%1.pdf").arg(change));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("formatted-saved-%1.pdf").arg(change));
    const QString tampered = m_temporaryDirectory.filePath(QStringLiteral("formatted-tampered-%1.pdf").arg(change));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    const QString text = change == QLatin1String("missing-space-pair")
        ? QStringLiteral("   ") : QStringLiteral("Underlined");
    const Cell format = plain(text, {testFontFamily(), false, false, true});
    QVERIFY(controller.setSelectedTableCellContent(0, 0, text, toVariant(format)).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    if (change.endsWith(QLatin1String("underline")) || change == QLatin1String("missing-space-pair")) {
        QVERIFY(tamperTableCellObject(saved, tampered, change));
    } else {
        QVariantMap altered = toVariant(format);
        QVariantList spans = altered.value(QStringLiteral("spans")).toList();
        QVariantMap span = spans.first().toMap();
        if (change == QLatin1String("format-disagrees"))
            span[QStringLiteral("underline")] = false;
        else if (change == QLatin1String("size-disagrees"))
            span[QStringLiteral("fontSize")] = 24;
        else if (change == QLatin1String("missing-size"))
            span.remove(QStringLiteral("fontSize"));
        else
            span[QStringLiteral("length")] = text.size() + 1;
        spans[0] = span;
        altered[QStringLiteral("spans")] = spans;
        QVERIFY(rewriteTableTestMetadata(saved, tampered, QStringLiteral("cellFormats"), QVariantList{altered}));
    }
    QVERIFY(document.open(QUrl::fromLocalFile(tampered)));
    const auto candidates = waitForCandidates(controller, 0);
    QVERIFY(candidateForKind(candidates, QStringLiteral("sourceShape")).isEmpty());
    for (const auto &candidate : candidates)
        QVERIFY(!candidate.toMap().value(QStringLiteral("editable")).toBool());
    if (change == QLatin1String("missing-space-pair"))
        QVERIFY(pageText(tampered).trimmed().isEmpty());
    else
        QVERIFY(pageText(tampered).contains(text));
}

void PDFEditControllerTest::editsTableCellsWithAtomicHistory()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("table-cell-history.pdf"));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(2);
    controller.setTableColumns(2);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    const QString objectId = controller.selectedObjectId();
    const auto cells = [&] {
        return controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList();
    };
    QVERIFY(cells().isEmpty());
    QVERIFY(controller.setSelectedTableCellText(0, 0, QString()).isEmpty()); // No-op.
    const QString input = QStringLiteral("Tiếng Việt\r\nDòng hai").normalized(QString::NormalizationForm_D);
    const QString normalized = QStringLiteral("Tiếng Việt\nDòng hai");
    QVERIFY(controller.setSelectedTableCellText(0, 0, input).isEmpty());
    QCOMPARE(cells(), QStringList({normalized, {}, {}, {}}));
    QVERIFY(!controller.selectedDraftPreview().value(QStringLiteral("tableFontFamily")).toString().isEmpty());
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableFontSize")).toDouble(), 12.0);
    const QVariantList rects = controller.selectedDraftPreview().value(QStringLiteral("tableCellRects")).toList();
    QCOMPARE(rects.size(), 4);
    for (const QVariant &rect : rects) {
        QVERIFY(!rect.toRectF().isEmpty());
        QVERIFY(controller.selectedFrameRect().contains(rect.toRectF()));
    }
    QVERIFY(controller.setSelectedTableCellText(0, 0, normalized).isEmpty()); // No-op.
    controller.undo();
    QVERIFY(cells().isEmpty());
    controller.undo(); // Both no-ops must leave creation as the next command.
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    controller.redo();
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(cells().at(0), normalized);
    QVERIFY(controller.setSelectedTableCellText(1, 1, QStringLiteral("42")).isEmpty());
    QCOMPARE(cells(), QStringList({normalized, {}, {}, QStringLiteral("42")}));
    QVERIFY(controller.setSelectedTableCellText(0, 0, QString()).isEmpty());
    QCOMPARE(cells(), QStringList({{}, {}, {}, QStringLiteral("42")}));
    QVERIFY(controller.setSelectedTableCellText(1, 1, QString()).isEmpty());
    QVERIFY(cells().isEmpty());
    controller.undo();
    QCOMPARE(cells().at(3), QStringLiteral("42"));
    controller.undo();
    QCOMPARE(cells().at(0), normalized);
    controller.deleteSelectedObject();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    controller.undo();
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(cells(), QStringList({normalized, {}, {}, QStringLiteral("42")}));
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::rejectsInvalidTableCellEdits()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("table-cell-validation.pdf"));
    QVERIFY(createTableTestPage(path, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(!controller.setSelectedTableCellText(0, 0, QStringLiteral("Text")).isEmpty());
    controller.setShapeType(PDFEditController::RectangleShape);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(0, 0, QStringLiteral("Text")).isEmpty());
    controller.discardDrafts();
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(2);
    controller.setTableColumns(2);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(-1, 0, QStringLiteral("Text")).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(2, 0, QStringLiteral("Text")).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(0, -1, QStringLiteral("Text")).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(0, 2, QStringLiteral("Text")).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(0, 0, QString(1025, QLatin1Char('a'))).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(0, 0, QString(QChar::Null)).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(0, 0, QStringLiteral("bad\tcontrol")).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(0, 0, QString(QChar(0xd800))).isEmpty());
    QVERIFY(!controller.setSelectedTableCellText(0, 0, QStringLiteral("Line\n").repeated(70)).isEmpty());
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toList().isEmpty());
    const QString text = QStringLiteral("A cell with content\nSecond line");
    QVERIFY(controller.setSelectedTableCellText(0, 0, text).isEmpty());
    const QRectF frame = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    QVERIFY(!controller.setSelectedTableCellText(0, 0, QStringLiteral("During resize")).isEmpty());
    controller.updateSelectedFrameRect(QRectF(.1, .1, .02, .02));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.beginSelectedFrameResize();
    controller.updateSelectedTableDivider(true, 0, .005);
    controller.updateSelectedTableDivider(false, 0, .005);
    controller.finishSelectedFrameResize();
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList().isEmpty());
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableRowStops")).toList().isEmpty());
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList().at(0), text);
    controller.undo(); // Rejected content and geometry never create history entries.
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toList().isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::savesTableCellsRoundTrip_data()
{
    QTest::addColumn<int>("rotation");
    QTest::addColumn<QString>("firstCell");
    for (int rotation = 0; rotation < 4; ++rotation) {
        const QString suffix = QString::number(rotation * 90);
        QTest::newRow(qPrintable(suffix)) << rotation << QStringLiteral("Tiếng Việt\nDòng hai");
        QTest::newRow(qPrintable(suffix + QStringLiteral("-whitespace")))
            << rotation << QStringLiteral("\n  Tiếng Việt  \n\n  Dòng hai  \n");
        QTest::newRow(qPrintable(suffix + QStringLiteral("-wrapped")))
            << rotation << (QStringLiteral("Tiếng Việt\nDòng hai ") + QStringLiteral("word ").repeated(40));
    }
}

void PDFEditControllerTest::savesTableCellsRoundTrip()
{
    QFETCH(int, rotation);
    QFETCH(QString, firstCell);
    const QString suffix = QString::fromLatin1(QTest::currentDataTag());
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("table-cell-source-%1.pdf").arg(suffix));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("table-cell-saved-%1.pdf").arg(suffix));
    const QString edited = m_temporaryDirectory.filePath(QStringLiteral("table-cell-edited-%1.pdf").arg(suffix));
    QVERIFY(createTableTestPage(path, rotation));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(2);
    controller.setTableColumns(2);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    QVERIFY(controller.setSelectedTableCellText(0, 0, firstCell).isEmpty());
    QVERIFY(controller.setSelectedTableCellText(1, 1, QStringLiteral("42")).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QCOMPARE(readTableTestMetadata(saved).value(QStringLiteral("version")).toInt(), 4);
    QCOMPARE(inspectShapePaths(saved).count, 1);
    const QString savedText = pageText(saved);
    QVERIFY2(savedText.contains(QStringLiteral("Tiếng Việt")), qPrintable(savedText));
    QVERIFY2(savedText.contains(QStringLiteral("Dòng hai")), qPrintable(savedText));
    QVERIFY(savedText.contains(QStringLiteral("42")));

    PDFDocument reopenedDocument;
    PDFEditController reopened;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(saved)));
    reopened.setDocument(&reopenedDocument);
    const QVariantList candidates = waitForCandidates(reopened, 0);
    const auto table = candidateForKind(candidates, QStringLiteral("sourceShape"));
    QVERIFY(!table.isEmpty());
    QCOMPARE(table.value(QStringLiteral("tableCellTexts")).toStringList(),
             QStringList({firstCell, {}, {}, QStringLiteral("42")}));
    for (const QVariant &entry : candidates) {
        const auto candidate = entry.toMap();
        if (candidate.value(QStringLiteral("objectId")) != table.value(QStringLiteral("objectId")))
            QVERIFY2(!candidate.value(QStringLiteral("editable")).toBool(),
                     "Managed cell text must not become an independently editable object.");
    }
    QVERIFY(reopened.selectCandidate(0, table.value(QStringLiteral("objectId")).toString()));
    reopened.beginSelectedFrameResize();
    reopened.updateSelectedTableDivider(true, 0, .6);
    reopened.updateSelectedTableDivider(false, 0, .55);
    reopened.finishSelectedFrameResize();
    const QRectF newFrame(.15, .15, .7, .7);
    reopened.beginSelectedFrameResize();
    reopened.updateSelectedFrameRect(newFrame);
    reopened.finishSelectedFrameResize();
    QCOMPARE(reopened.selectedFrameRect(), newFrame);
    QVERIFY(reopened.setSelectedTableCellText(0, 0, QStringLiteral("Đã sửa")).isEmpty());
    QVERIFY(reopened.setSelectedTableCellText(1, 1, QString()).isEmpty());
    QVERIFY(reopened.saveAs(QUrl::fromLocalFile(edited)));
    const QString editedText = pageText(edited);
    QVERIFY2(editedText.contains(QStringLiteral("Đã sửa")), qPrintable(editedText));
    QVERIFY(!editedText.contains(QStringLiteral("Tiếng Việt")));
    QVERIFY(!editedText.contains(QStringLiteral("Dòng hai")));
    QVERIFY(!editedText.contains(QStringLiteral("42")));
    QCOMPARE(inspectShapePaths(edited).count, 1);

    auto editedTable = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QCOMPARE(editedTable.value(QStringLiteral("tableCellTexts")).toStringList(),
             QStringList({QStringLiteral("Đã sửa"), {}, {}, {}}));
    QCOMPARE(editedTable.value(QStringLiteral("tableColumnStops")).toList(), QVariantList{QVariant(.6)});
    QCOMPARE(editedTable.value(QStringLiteral("tableRowStops")).toList(), QVariantList{QVariant(.55)});
    QVERIFY(reopened.selectCandidate(0, editedTable.value(QStringLiteral("objectId")).toString()));
    QVERIFY(reopened.setSelectedTableCellText(0, 0, QStringLiteral("Temporary")).isEmpty());
    reopened.discardDrafts();
    editedTable = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QVERIFY(reopened.selectCandidate(0, editedTable.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(reopened.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList().at(0),
             QStringLiteral("Đã sửa"));
    QVERIFY(reopened.setSelectedTableCellText(0, 0, QString()).isEmpty());
    reopened.undo();
    QVERIFY(!reopened.hasDrafts());
    QVERIFY(reopened.setSelectedTableCellText(0, 0, QString()).isEmpty());
    QVERIFY(reopened.save());
    QCOMPARE(inspectShapePaths(edited).count, 1);
    QVERIFY(pageText(edited).trimmed().isEmpty());
    editedTable = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QVERIFY(!editedTable.isEmpty());
    QVERIFY(editedTable.value(QStringLiteral("tableCellTexts")).toStringList().isEmpty());
    QVERIFY(reopened.selectCandidate(0, editedTable.value(QStringLiteral("objectId")).toString()));
    QVERIFY(reopened.setSelectedTableCellText(0, 0, QStringLiteral("Đã sửa")).isEmpty());
    QVERIFY(reopened.save());
    editedTable = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QVERIFY(reopened.selectCandidate(0, editedTable.value(QStringLiteral("objectId")).toString()));
    reopened.deleteSelectedObject();
    reopened.undo();
    QVERIFY(reopened.selectCandidate(0, editedTable.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(reopened.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList().at(0),
             QStringLiteral("Đã sửa"));
    reopened.deleteSelectedObject();
    QVERIFY(reopened.save());
    QCOMPARE(inspectShapePaths(edited).count, 0);
    QVERIFY(pageText(edited).trimmed().isEmpty());
    QVERIFY(waitForCandidates(reopened, 0).isEmpty());
}

void PDFEditControllerTest::savesMultipleTablesWithCellText()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("multiple-cell-tables-source.pdf"));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("multiple-cell-tables-saved.pdf"));
    QVERIFY(createTableTestPage(source, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .4)).isEmpty());
    QVERIFY(controller.setSelectedTableCellText(0, 0, QStringLiteral("First table")).isEmpty());
    QVERIFY(!controller.addShape(0, QPointF(.1, .55), QPointF(.9, .9)).isEmpty());
    QVERIFY(controller.setSelectedTableCellText(0, 0, QStringLiteral("Second table")).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QCOMPARE(inspectShapePaths(saved).count, 2);
    const auto tableWithText = [](const QVariantList &candidates, const QString &text) {
        for (const QVariant &entry : candidates) {
            const QVariantMap candidate = entry.toMap();
            if (candidate.value(QStringLiteral("kind")).toString() == QLatin1String("sourceShape")
                && candidate.value(QStringLiteral("tableCellTexts")).toStringList() == QStringList{text})
                return candidate;
        }
        return QVariantMap();
    };
    const auto candidates = waitForCandidates(controller, 0);
    const auto first = tableWithText(candidates, QStringLiteral("First table"));
    const auto second = tableWithText(candidates, QStringLiteral("Second table"));
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(controller.selectCandidate(0, first.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.setSelectedTableCellText(0, 0, QStringLiteral("Changed first")).isEmpty());
    QVERIFY(controller.selectCandidate(0, second.value(QStringLiteral("objectId")).toString()));
    controller.deleteSelectedObject();
    QVERIFY(controller.save());
    QCOMPARE(inspectShapePaths(saved).count, 1);
    const QString text = pageText(saved);
    QVERIFY2(text.contains(QStringLiteral("Changed first")), qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("First table")));
    QVERIFY(!text.contains(QStringLiteral("Second table")));
    QVERIFY(!tableWithText(waitForCandidates(controller, 0), QStringLiteral("Changed first")).isEmpty());
}

void PDFEditControllerTest::rejectsInvalidTableCellMetadata_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QVariant>("value");
    QTest::newRow("missing-id") << QStringLiteral("id") << QVariant();
    QTest::newRow("invalid-id") << QStringLiteral("id") << QVariant(QStringLiteral("table-1"));
    QTest::newRow("different-id") << QStringLiteral("id")
        << QVariant(QStringLiteral("8c989be4-d9f4-4e24-9b2c-cf4a61e02642"));
    QTest::newRow("missing-cells") << QStringLiteral("cells") << QVariant();
    QTest::newRow("wrong-cell-count") << QStringLiteral("cells")
        << QVariant(QVariantList{QStringLiteral("Cell"), QString()});
    QTest::newRow("non-string-cell") << QStringLiteral("cells")
        << QVariant(QVariantList{QVariant(42)});
    QTest::newRow("overlong-cell") << QStringLiteral("cells")
        << QVariant(QVariantList{QString(1025, QLatin1Char('a'))});
    QTest::newRow("control-in-cell") << QStringLiteral("cells")
        << QVariant(QVariantList{QStringLiteral("Bad\tcontrol")});
    QTest::newRow("non-normalized-cell") << QStringLiteral("cells")
        << QVariant(QVariantList{QStringLiteral("Tiếng Việt").normalized(QString::NormalizationForm_D)});
    QTest::newRow("changed-cell-text") << QStringLiteral("cells")
        << QVariant(QVariantList{QStringLiteral("Replaced")});
    QTest::newRow("missing-font") << QStringLiteral("font") << QVariant();
    QTest::newRow("empty-font") << QStringLiteral("font") << QVariant(QString());
    QTest::newRow("overlong-font") << QStringLiteral("font") << QVariant(QString(257, QLatin1Char('a')));
    QTest::newRow("invalid-size") << QStringLiteral("fontSize") << QVariant(13.0);
    QTest::newRow("string-size") << QStringLiteral("fontSize") << QVariant(QStringLiteral("12"));
}

void PDFEditControllerTest::rejectsInvalidTableCellMetadata()
{
    QFETCH(QString, key);
    QFETCH(QVariant, value);
    const QString suffix = QString::fromLatin1(QTest::currentDataTag());
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("cell-metadata-source-%1.pdf").arg(suffix));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("cell-metadata-saved-%1.pdf").arg(suffix));
    const QString tampered = m_temporaryDirectory.filePath(QStringLiteral("cell-metadata-tampered-%1.pdf").arg(suffix));
    QVERIFY(createTableTestPage(source, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    QVERIFY(controller.setSelectedTableCellText(0, 0, QStringLiteral("Cell")).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QVERIFY(rewriteTableTestMetadata(saved, tampered, key, value));
    QVERIFY(document.open(QUrl::fromLocalFile(tampered)));
    const auto candidates = waitForCandidates(controller, 0);
    QVERIFY(candidateForKind(candidates, QStringLiteral("sourceShape")).isEmpty());
    for (const QVariant &candidate : candidates)
        QVERIFY2(!candidate.toMap().value(QStringLiteral("editable")).toBool(),
                 "Malformed table groups must not expose editable child text.");
    QCOMPARE(inspectShapePaths(tampered).count, 1);
    QVERIFY(pageText(tampered).contains(QStringLiteral("Cell")));
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::rejectsTamperedTableCellObjects_data()
{
    QTest::addColumn<QString>("change");
    for (const QString &change : {QStringLiteral("missing-fragment"), QStringLiteral("moved-fragment"),
                                 QStringLiteral("recolored-fragment"), QStringLiteral("duplicate-marker"),
                                 QStringLiteral("malformed-marker"), QStringLiteral("interleaved-paint")})
        QTest::newRow(qPrintable(change)) << change;
}

void PDFEditControllerTest::rejectsTamperedTableCellObjects()
{
    QFETCH(QString, change);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("cell-object-source-%1.pdf").arg(change));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("cell-object-saved-%1.pdf").arg(change));
    const QString tampered = m_temporaryDirectory.filePath(QStringLiteral("cell-object-tampered-%1.pdf").arg(change));
    QVERIFY(createTableTestPage(source, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    QVERIFY(!controller.addShape(0, QPointF(.1, .1), QPointF(.9, .9)).isEmpty());
    QVERIFY(controller.setSelectedTableCellText(0, 0, QStringLiteral("First line\nSecond line")).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QVERIFY(tamperTableCellObject(saved, tampered, change));
    QVERIFY(document.open(QUrl::fromLocalFile(tampered)));
    const auto candidates = waitForCandidates(controller, 0);
    QVERIFY(candidateForKind(candidates, QStringLiteral("sourceShape")).isEmpty());
    for (const QVariant &candidate : candidates)
        QVERIFY2(!candidate.toMap().value(QStringLiteral("editable")).toBool(),
                 "Incomplete or altered table text must remain unavailable for editing.");
    QCOMPARE(inspectShapePaths(tampered).count, change == QLatin1String("interleaved-paint") ? 2 : 1);
    QVERIFY(!pageText(tampered).trimmed().isEmpty());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::savesAndEditsTableGrid_data()
{
    QTest::addColumn<int>("rows");
    QTest::addColumn<int>("columns");
    QTest::addColumn<int>("rotation");
    QTest::newRow("cropped") << 3 << 4 << 0;
    QTest::newRow("cropped-90") << 3 << 4 << 1;
    QTest::newRow("cropped-180") << 3 << 4 << 2;
    QTest::newRow("cropped-270") << 3 << 4 << 3;
    QTest::newRow("maximum-grid") << 20 << 20 << 0;
    QTest::newRow("single-cell") << 1 << 1 << 0;
    QTest::newRow("single-cell-90") << 1 << 1 << 1;
    QTest::newRow("single-cell-180") << 1 << 1 << 2;
    QTest::newRow("single-cell-270") << 1 << 1 << 3;
}

void PDFEditControllerTest::savesAndEditsTableGrid()
{
    QFETCH(int, rows);
    QFETCH(int, columns);
    QFETCH(int, rotation);
    const QString suffix = QString::fromLatin1(QTest::currentDataTag());
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("table-source-%1.pdf").arg(suffix));
    const QString savedPath = m_temporaryDirectory.filePath(QStringLiteral("table-saved-%1.pdf").arg(suffix));
    const QString editedPath = m_temporaryDirectory.filePath(QStringLiteral("table-edited-%1.pdf").arg(suffix));
    QVERIFY(createTableTestPage(sourcePath, rotation));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTableRows(rows);
    controller.setTableColumns(columns);
    controller.setShapeType(PDFEditController::TableShape);
    controller.setTool(PDFEditController::AddShapeTool);
    QVERIFY(!controller.addShape(0, QPointF(0.15, 0.2), QPointF(0.75, 0.7)).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
    const ShapeSaveInspection first = inspectShapePaths(savedPath);
    QVERIFY(first.readable);
    QCOMPARE(first.count, 1);
    QCOMPARE(first.managedCount, 1);
    QCOMPARE(first.filledCount, 0);
    QCOMPARE(first.normalizedPathPoints.size(), 1);
    const auto &points = first.normalizedPathPoints.constFirst();
    QCOMPARE(points.size(), 6 + 2 * (rows + columns - 2));
    qreal left = 1, right = 0, top = 1, bottom = 0;
    for (const QPointF &point : points) {
        left = qMin(left, point.x()); right = qMax(right, point.x());
        top = qMin(top, point.y()); bottom = qMax(bottom, point.y());
    }
    QVERIFY(qAbs(left - 0.15) < 0.003);
    QVERIFY(qAbs(right - 0.75) < 0.003);
    QVERIFY(qAbs(top - 0.2) < 0.003);
    QVERIFY(qAbs(bottom - 0.7) < 0.003);
    QVector<bool> horizontal(rows - 1, false), vertical(columns - 1, false);
    // Inspect the PDF path itself, independently from the marker. Internal
    // rules must span the frame and divide it into the requested equal cells.
    for (qsizetype index = 6; index + 1 < points.size(); index += 2) {
        const QPointF a = points.at(index), b = points.at(index + 1);
        if (qAbs(a.y() - b.y()) < 0.0001) {
            const int row = qRound((a.y() - top) / (bottom - top) * rows);
            QVERIFY(row > 0 && row < rows);
            QVERIFY(!horizontal.at(row - 1));
            horizontal[row - 1] = true;
            QVERIFY(qAbs(qMin(a.x(), b.x()) - left) < 0.0001);
            QVERIFY(qAbs(qMax(a.x(), b.x()) - right) < 0.0001);
            QVERIFY(qAbs(a.y() - (top + (bottom - top) * row / rows)) < 0.0001);
        } else {
            QVERIFY(qAbs(a.x() - b.x()) < 0.0001);
            const int column = qRound((a.x() - left) / (right - left) * columns);
            QVERIFY(column > 0 && column < columns);
            QVERIFY(!vertical.at(column - 1));
            vertical[column - 1] = true;
            QVERIFY(qAbs(qMin(a.y(), b.y()) - top) < 0.0001);
            QVERIFY(qAbs(qMax(a.y(), b.y()) - bottom) < 0.0001);
            QVERIFY(qAbs(a.x() - (left + (right - left) * column / columns)) < 0.0001);
        }
    }
    QVERIFY(std::all_of(horizontal.cbegin(), horizontal.cend(), [](bool value) { return value; }));
    QVERIFY(std::all_of(vertical.cbegin(), vertical.cend(), [](bool value) { return value; }));
    const QVariantMap source = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(!source.isEmpty());
    QCOMPARE(source.value(QStringLiteral("tableRows")).toInt(), rows);
    QCOMPARE(source.value(QStringLiteral("tableColumns")).toInt(), columns);
    QVERIFY(controller.selectCandidate(0, source.value(QStringLiteral("objectId")).toString()));
    const QRectF resized(0.2, 0.25, 0.55, 0.45);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resized);
    controller.finishSelectedFrameResize();
    QVERIFY(controller.setSelectedDrawingStrokeColor(QColor(20, 80, 180)));
    QVERIFY(controller.setSelectedDrawingOpacity(0.7));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(editedPath)));
    PDFDocument reopenedDocument;
    PDFEditController reopened;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(editedPath)));
    reopened.setDocument(&reopenedDocument);
    const QVariantMap edited = candidateForKind(waitForCandidates(reopened, 0), QStringLiteral("sourceShape"));
    QVERIFY(!edited.isEmpty());
    QCOMPARE(edited.value(QStringLiteral("tableRows")).toInt(), rows);
    QCOMPARE(edited.value(QStringLiteral("tableColumns")).toInt(), columns);
    QVERIFY(reopened.selectCandidate(0, edited.value(QStringLiteral("objectId")).toString()));
    QVERIFY(QLineF(reopened.selectedFrameRect().topLeft(), resized.topLeft()).length() < 0.002);
    QVERIFY(QLineF(reopened.selectedFrameRect().bottomRight(), resized.bottomRight()).length() < 0.002);
    QCOMPARE(reopened.selectedDrawingStrokeColor(), QColor(20, 80, 180));
    QVERIFY(qAbs(reopened.selectedDrawingOpacity() - 0.7) < 0.005);
    QCOMPARE(inspectShapePaths(editedPath).count, 1);
    reopened.deleteSelectedObject();
    reopened.undo();
    QVERIFY(reopened.selectCandidate(0, edited.value(QStringLiteral("objectId")).toString()));
    reopened.redo();
    QVERIFY(reopened.save());
    QCOMPARE(inspectShapePaths(editedPath).count, 0);
    QCOMPARE(inspectShapePaths(savedPath).count, 1);
    QCOMPARE(inspectShapePaths(sourcePath).count, 0);
}

void PDFEditControllerTest::rejectsInvalidTableMetadata_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QVariant>("value");
    QTest::newRow("zero-rows") << QStringLiteral("rows") << QVariant(0);
    QTest::newRow("negative-columns") << QStringLiteral("columns") << QVariant(-1);
    QTest::newRow("too-many-rows") << QStringLiteral("rows") << QVariant(21);
    QTest::newRow("fractional-columns") << QStringLiteral("columns") << QVariant(1.5);
    QTest::newRow("missing-rows") << QStringLiteral("rows") << QVariant();
    QTest::newRow("string-rows") << QStringLiteral("rows") << QVariant(QStringLiteral("1"));
    QTest::newRow("grid-disagrees-with-path") << QStringLiteral("rows") << QVariant(2);
    QTest::newRow("legacy-version") << QStringLiteral("version") << QVariant(1);
    QTest::newRow("unknown-version") << QStringLiteral("version") << QVariant(5);
    QTest::newRow("filled-grid") << QStringLiteral("fill") << QVariant(QStringLiteral("#ff0000"));
}

void PDFEditControllerTest::rejectsInvalidTableMetadata()
{
    QFETCH(QString, key);
    QFETCH(QVariant, value);
    const QString suffix = QString::fromLatin1(QTest::currentDataTag());
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("table-invalid-source-%1.pdf").arg(suffix));
    const QString savedPath = m_temporaryDirectory.filePath(QStringLiteral("table-invalid-saved-%1.pdf").arg(suffix));
    const QString tamperedPath = m_temporaryDirectory.filePath(QStringLiteral("table-invalid-tampered-%1.pdf").arg(suffix));
    QVERIFY(createTableTestPage(sourcePath, 0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTableRows(1);
    controller.setTableColumns(1);
    controller.setShapeType(PDFEditController::TableShape);
    QVERIFY(!controller.addShape(0, QPointF(0.2, 0.3), QPointF(0.6, 0.7)).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
    QVERIFY(rewriteTableTestMetadata(savedPath, tamperedPath, key, value));
    QVERIFY(document.open(QUrl::fromLocalFile(tamperedPath)));
    // Even the 1x1 grid resembles a rectangle; an invalid marker must never
    // silently become an editable unmarked rectangle.
    QVERIFY(candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape")).isEmpty());
    QCOMPARE(inspectShapePaths(tamperedPath).count, 1);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::drawsTableThroughRibbonAndOverlay()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*(?:Binding loop|TypeError|ReferenceError).*")));
    QTranslator translator;
    const QString translationPath = qEnvironmentVariable("TREPDF_TABLE_TEST_TRANSLATION");
    if (!translationPath.isEmpty()) {
        QVERIFY(translator.load(translationPath));
        QCoreApplication::installTranslator(&translator);
    }
    const auto removeTranslator = qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("table-ui.pdf"));
    QVERIFY(createTableTestPage(sourcePath, 0));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window);
    window->resize(1200, 900);
    window->show();
    auto *tabs = root->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    auto *model = root->findChild<PDFTabModel *>();
    QVERIFY(tabs);
    QVERIFY(model);
    auto *session = model->open(QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);
    auto &controller = *session->editController();
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QTRY_COMPARE(controller.tool(), PDFEditController::SelectTool);
    const auto findItem = [](auto &&self, QQuickItem *parent, const QString &name) -> QQuickItem * {
        if (parent->objectName() == name)
            return parent;
        for (auto *child : parent->childItems()) {
            if (auto *match = self(self, child, name))
                return match;
        }
        return nullptr;
    };
    const auto itemNamed = [&](const QString &name) {
        return findItem(findItem, window->contentItem(), name);
    };
    const auto clickItem = [&](QQuickItem *item) {
        // Property panels scroll on shorter windows; reveal the real control before clicking.
        for (auto *parent = item->parentItem(); parent; parent = parent->parentItem()) {
            if (!parent->property("contentY").isValid())
                continue;
            const QRectF rect = item->mapRectToItem(parent, QRectF(0, 0, item->width(), item->height()));
            const qreal delta = rect.top() < 8 ? rect.top() - 8
                : rect.bottom() > parent->height() - 8 ? rect.bottom() - parent->height() + 8 : 0;
            const qreal maximum = qMax<qreal>(0, parent->property("contentHeight").toReal() - parent->height());
            parent->setProperty("contentY", qBound<qreal>(0, parent->property("contentY").toReal() + delta, maximum));
        }
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    };
    QQuickItem *tableButton = nullptr;
    QTRY_VERIFY((tableButton = itemNamed(QStringLiteral("editTableButton"))));
    QVERIFY(tableButton->isEnabled());
    QVERIFY(!itemNamed(QStringLiteral("tableRowsSpinBox")));
    clickItem(tableButton);
    QQuickItem *rows = nullptr;
    QQuickItem *columns = nullptr;
    QTRY_VERIFY((rows = itemNamed(QStringLiteral("tableRowsSpinBox"))));
    QTRY_VERIFY((columns = itemNamed(QStringLiteral("tableColumnsSpinBox"))));
    QCOMPARE(rows->property("from").toInt(), 1);
    QCOMPARE(rows->property("to").toInt(), 20);
    QVERIFY(rows->setProperty("value", 5));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!itemNamed(QStringLiteral("tableRowsSpinBox")));
    QCOMPARE(controller.tableRows(), 3);
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    QVERIFY(!controller.hasDrafts());

    clickItem(tableButton);
    QTRY_VERIFY((rows = itemNamed(QStringLiteral("tableRowsSpinBox"))));
    QTRY_VERIFY((columns = itemNamed(QStringLiteral("tableColumnsSpinBox"))));
    QCOMPARE(rows->property("value").toInt(), 3);
    QVERIFY(columns->setProperty("value", 4));
    QQuickItem *drawButton = itemNamed(QStringLiteral("drawTableButton"));
    QVERIFY(drawButton);
    const QString screenshotPrefix = qEnvironmentVariable("TREPDF_TABLE_TEST_SCREENSHOT");
    if (!screenshotPrefix.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshotPrefix + QStringLiteral("-popup.png")));
    }
    clickItem(drawButton);
    QTRY_VERIFY(!itemNamed(QStringLiteral("tableRowsSpinBox")));
    QCOMPARE(controller.shapeType(), PDFEditController::TableShape);
    QCOMPARE(controller.tool(), PDFEditController::AddShapeTool);
    QCOMPARE(controller.tableRows(), 3);
    QCOMPARE(controller.tableColumns(), 4);

    const auto findOverlay = [](auto &&self, QQuickItem *parent) -> QQuickItem * {
        if (parent->isVisible() && parent->width() > 100 && parent->height() > 100
            && parent->metaObject()->indexOfMethod("beginShape(QVariant,QVariant)") >= 0)
            return parent;
        for (auto *child : parent->childItems()) {
            if (auto *match = self(self, child))
                return match;
        }
        return nullptr;
    };
    QQuickItem *overlay = nullptr;
    QTRY_VERIFY((overlay = findOverlay(findOverlay, window->contentItem())));
    const auto start = overlay->mapToScene(QPointF(overlay->width() * .2, overlay->height() * .25)).toPoint();
    const auto end = overlay->mapToScene(QPointF(overlay->width() * .7, overlay->height() * .6)).toPoint();
    QVERIFY(window->contentItem()->contains(end));
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(window, end, 30);
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
    QVERIFY(!controller.hasDrafts());
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(window, end, 30);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
    QTRY_COMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableRows")).toInt(), 3);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumns")).toInt(), 4);
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    if (!screenshotPrefix.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshotPrefix + QStringLiteral("-grid.png")));
    }
    const QRectF tableFrame = controller.selectedFrameRect();
    QQuickItem *columnDivider = nullptr;
    QQuickItem *rowDivider = nullptr;
    QTRY_VERIFY((columnDivider = itemNamed(QStringLiteral("tableColumnDivider0"))));
    QTRY_VERIFY((rowDivider = itemNamed(QStringLiteral("tableRowDivider0"))));
    const auto dividerPoint = [](QQuickItem *item, bool column) {
        // Avoid the intersection with a perpendicular divider.
        return item->mapToScene(QPointF(item->width() * (column ? .5 : .1),
                                       item->height() * (column ? .1 : .5))).toPoint();
    };
    const QPoint columnStart = dividerPoint(columnDivider, true);
    const QPoint columnEnd = columnStart + QPoint(25, 0);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, columnStart);
    QTest::mouseMove(window, columnEnd, 30);
    QTRY_VERIFY(!controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList().isEmpty());
    QCOMPARE(controller.selectedFrameRect(), tableFrame);
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, columnEnd);
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList().isEmpty());
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, columnStart);
    QTest::mouseMove(window, columnEnd, 30);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, columnEnd);
    const QVariantList movedColumns = controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList();
    QCOMPARE(movedColumns.size(), 3);
    QVERIFY(movedColumns.at(0).toDouble() > .25);
    QCOMPARE(controller.selectedFrameRect(), tableFrame);
    controller.undo();
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList().isEmpty());
    controller.redo();
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList(), movedColumns);
    QTRY_VERIFY((rowDivider = itemNamed(QStringLiteral("tableRowDivider0"))));
    const QPoint rowStart = dividerPoint(rowDivider, false);
    const QPoint rowEnd = rowStart + QPoint(0, 20);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, rowStart);
    QTest::mouseMove(window, rowEnd, 30);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, rowEnd);
    const QVariantList movedRows = controller.selectedDraftPreview().value(QStringLiteral("tableRowStops")).toList();
    QCOMPARE(movedRows.size(), 2);
    QVERIFY(movedRows.at(0).toDouble() > 1.0 / 3);
    QCOMPARE(controller.selectedFrameRect(), tableFrame);
    if (!screenshotPrefix.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshotPrefix + QStringLiteral("-dividers.png")));
    }
    controller.undo(); // Row gesture.
    controller.undo(); // Column gesture.
    controller.undo(); // Creation.
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    const QString savedPath = m_temporaryDirectory.filePath(QStringLiteral("table-ui-saved.pdf"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
    const auto source = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QVERIFY(controller.selectCandidate(0, source.value(QStringLiteral("objectId")).toString()));
    QTRY_VERIFY((columnDivider = itemNamed(QStringLiteral("tableColumnDivider0"))));
    const QPoint savedStart = dividerPoint(columnDivider, true);
    const QPoint savedEnd = savedStart + QPoint(20, 0);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, savedStart);
    QTest::mouseMove(window, savedEnd, 30);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, savedEnd);
    QTRY_VERIFY(controller.hasDrafts());
    QVERIFY(controller.selectedDraftPreview().value(QStringLiteral("tableColumnStops")).toList().at(0).toDouble() > .25);
    QCOMPARE(controller.selectedFrameRect(), tableFrame);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    // Structure buttons open a lazy popup; changes happen only after Apply.
    QVERIFY(!itemNamed(QStringLiteral("tableRowReferenceBox")));
    QVERIFY(!itemNamed(QStringLiteral("tableColumnReferenceBox")));
    const auto structurePopupVisible = [&] {
        auto *apply = itemNamed(QStringLiteral("tableStructureApplyButton"));
        return apply && apply->isVisible();
    };
    const auto applyTableStructure = [&] {
        clickItem(itemNamed(QStringLiteral("tableStructureApplyButton")));
    };
    QVERIFY(!structurePopupVisible());
    QQuickItem *appendRow = nullptr;
    QTRY_VERIFY((appendRow = itemNamed(QStringLiteral("tableAppendRowButton"))) && appendRow->isEnabled());
    clickItem(appendRow);
    QTRY_VERIFY(structurePopupVisible());
    QCOMPARE(itemNamed(QStringLiteral("tableStructureCountBox"))->property("value").toInt(), 1);
    QCOMPARE(itemNamed(QStringLiteral("tableStructurePositionBox"))->property("value").toInt(), 4);
    QVERIFY(!controller.hasDrafts());
    applyTableStructure();
    QTRY_VERIFY(!structurePopupVisible());
    QTRY_COMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableRows")).toInt(), 4);
    QVERIFY(controller.selectedFrameRect().height() > tableFrame.height());
    QCOMPARE(controller.selectedFrameRect().width(), tableFrame.width());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QQuickItem *appendColumn = nullptr;
    QTRY_VERIFY((appendColumn = itemNamed(QStringLiteral("tableAppendColumnButton"))) && appendColumn->isEnabled());
    clickItem(appendColumn);
    QTRY_VERIFY(structurePopupVisible());
    QCOMPARE(itemNamed(QStringLiteral("tableStructurePositionBox"))->property("value").toInt(), 5);
    applyTableStructure();
    QTRY_VERIFY(!structurePopupVisible());
    QTRY_COMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumns")).toInt(), 5);
    QCOMPARE(controller.selectedFrameRect().height(), tableFrame.height());
    QVERIFY(controller.selectedFrameRect().width() > tableFrame.width());
    controller.undo();
    QVERIFY(!controller.hasDrafts());

    clickItem(appendRow);
    QTRY_VERIFY(structurePopupVisible());
    auto *structureCount = itemNamed(QStringLiteral("tableStructureCountBox"));
    auto *structurePosition = itemNamed(QStringLiteral("tableStructurePositionBox"));
    QVERIFY(structureCount && structurePosition);
    QVERIFY(structureCount->setProperty("value", 2));
    QVERIFY(structurePosition->setProperty("value", 2));
    if (!screenshotPrefix.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshotPrefix + QStringLiteral("-insert-tracks.png")));
    }
    applyTableStructure();
    QTRY_VERIFY(!structurePopupVisible());
    QTRY_COMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableRows")).toInt(), 5);
    const auto insertedRows = controller.selectedDraftPreview().value(QStringLiteral("tableRowStops")).toList();
    QCOMPARE(insertedRows.size(), 4);
    QVERIFY(qAbs(insertedRows.at(0).toReal() - .2) < .000001);
    controller.undo(); // Two rows inserted together are one action.
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), tableFrame);

    clickItem(appendColumn);
    QTRY_VERIFY(structurePopupVisible());
    clickItem(itemNamed(QStringLiteral("tableStructureCancelButton")));
    QTRY_VERIFY(!structurePopupVisible());
    QVERIFY(!controller.hasDrafts());
    QQuickItem *removeColumn = nullptr;
    QTRY_VERIFY((removeColumn = itemNamed(QStringLiteral("tableRemoveColumnButton"))) && removeColumn->isEnabled());
    clickItem(removeColumn);
    QTRY_VERIFY(structurePopupVisible());
    auto *structureIndices = itemNamed(QStringLiteral("tableStructureIndicesInput"));
    QVERIFY(structureIndices);
    QCOMPARE(structureIndices->property("text").toString(), QStringLiteral("4"));
    QVERIFY(structureIndices->setProperty("text", QStringLiteral("1,,3")));
    applyTableStructure();
    QTRY_VERIFY(structurePopupVisible());
    QVERIFY(!controller.hasDrafts());
    auto *structureError = itemNamed(QStringLiteral("tableStructureError"));
    QVERIFY(structureError && structureError->isVisible() && !structureError->property("text").toString().isEmpty());
    if (!screenshotPrefix.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshotPrefix + QStringLiteral("-remove-tracks.png")));
    }
    QVERIFY(structureIndices->setProperty("text", QStringLiteral("1,2,3,4")));
    applyTableStructure();
    QTRY_VERIFY(structurePopupVisible());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(structureIndices->setProperty("text", QStringLiteral(" 3, 1, 3 ")));
    applyTableStructure();
    QTRY_VERIFY(!structurePopupVisible());
    QTRY_COMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumns")).toInt(), 2);
    controller.undo(); // Nonadjacent duplicate indices still produce one action.
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), tableFrame);

    // Closing or replacing the selection/history invalidates popup input.
    clickItem(appendRow);
    QTRY_VERIFY(structurePopupVisible());
    applyTableStructure();
    QTRY_VERIFY(!structurePopupVisible());
    QVERIFY(controller.hasDrafts());
    clickItem(removeColumn);
    QTRY_VERIFY(structurePopupVisible());
    controller.undo();
    QTRY_VERIFY(!structurePopupVisible());
    QVERIFY(!controller.hasDrafts());
    clickItem(appendRow);
    QTRY_VERIFY(structurePopupVisible());
    controller.clearSelection();
    QTRY_VERIFY(!structurePopupVisible());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.selectCandidate(0, source.value(QStringLiteral("objectId")).toString()));

    QQuickItem *removeRow = nullptr;
    QTRY_VERIFY((removeRow = itemNamed(QStringLiteral("tableRemoveRowButton"))) && removeRow->isEnabled());
    QTRY_VERIFY((removeColumn = itemNamed(QStringLiteral("tableRemoveColumnButton"))) && removeColumn->isEnabled());
    clickItem(removeRow);
    QTRY_VERIFY(structurePopupVisible());
    applyTableStructure();
    QTRY_VERIFY(!structurePopupVisible());
    QTRY_COMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableRows")).toInt(), 2);
    QVERIFY(controller.selectedFrameRect().height() < tableFrame.height());
    clickItem(removeColumn);
    QTRY_VERIFY(structurePopupVisible());
    applyTableStructure();
    QTRY_VERIFY(!structurePopupVisible());
    QTRY_COMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumns")).toInt(), 3);
    QVERIFY(controller.selectedFrameRect().width() < tableFrame.width());
    clickItem(removeRow);
    QTRY_VERIFY(structurePopupVisible());
    applyTableStructure();
    QTRY_VERIFY(!structurePopupVisible());
    QTRY_COMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableRows")).toInt(), 1);
    QVERIFY(!removeRow->isEnabled());
    for (int count : {2, 1}) {
        clickItem(removeColumn);
        QTRY_VERIFY(structurePopupVisible());
        applyTableStructure();
        QTRY_VERIFY(!structurePopupVisible());
        QTRY_COMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableColumns")).toInt(), count);
    }
    QVERIFY(!removeColumn->isEnabled());
    for (int index = 0; index < 5; ++index)
        controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), tableFrame);
    const QPoint cellPoint = overlay->mapToScene(QPointF(
        (tableFrame.left() + tableFrame.width() / 8) * overlay->width(),
        (tableFrame.top() + tableFrame.height() / 6) * overlay->height())).toPoint();
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QQuickItem *cellInput = nullptr;
    QQuickItem *cellEditor = nullptr;
    const auto cellInputVisible = [&] {
        auto *input = itemNamed(QStringLiteral("tableCellTextInput"));
        return input && input->isVisible();
    };
    const auto cellPanel = [&] {
        return itemNamed(QStringLiteral("tableCellPropertiesPanel"));
    };
    const auto cellPanelVisible = [&] {
        auto *panel = cellPanel();
        return panel && panel->isVisible();
    };
    const auto currentCellTexts = [&] {
        return controller.selectedDraftPreview().value(QStringLiteral("tableCellTexts")).toStringList();
    };
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QTRY_VERIFY((cellEditor = itemNamed(QStringLiteral("tableCellInlineEditor"))) && cellEditor->isVisible());
    QVERIFY(!itemNamed(QStringLiteral("tableCellDialog")));
    QTRY_VERIFY(cellPanelVisible());
    const auto *selectionProperties = itemNamed(QStringLiteral("selectionPropertiesColumn"));
    QVERIFY(selectionProperties);
    QTRY_VERIFY(!selectionProperties->isVisible());
    QQuickItem *documentView = itemNamed(QStringLiteral("primaryDocumentView"));
    QVERIFY(documentView);
    QTRY_VERIFY(cellPanel()->mapToScene(QPointF(0, 0)).x()
                >= documentView->mapToScene(QPointF(documentView->width(), 0)).x() - 1);
    QVERIFY(!cellPanel()->isAncestorOf(cellInput));
    auto *cellFontBox = itemNamed(QStringLiteral("tableCellFontBox"));
    QVERIFY(cellFontBox && cellPanel()->isAncestorOf(cellFontBox));
    QVERIFY(itemNamed(QStringLiteral("tableCellFontSizeBox")));
    // The cell panel only formats content; structure actions belong to the table panel.
    QVERIFY(!itemNamed(QStringLiteral("tableCellAppendRowButton")));
    QVERIFY(!itemNamed(QStringLiteral("tableCellAppendColumnButton")));
    QVERIFY(!itemNamed(QStringLiteral("tableCellRemoveRowButton")));
    QVERIFY(!itemNamed(QStringLiteral("tableCellRemoveColumnButton")));
    const auto enterSizeText = [&](const QString &text) {
        auto *sizeInput = itemNamed(QStringLiteral("tableCellFontSizeInput"));
        if (!sizeInput || !sizeInput->isEnabled())
            return false;
        clickItem(sizeInput);
        QTest::keySequence(window, QKeySequence(QKeySequence::SelectAll));
        for (const QChar character : text)
            QTest::keyClick(window, character.toLatin1());
        return sizeInput->property("text").toString() == text;
    };
    const QRectF firstCellInOverlay(tableFrame.left() * overlay->width(),
        tableFrame.top() * overlay->height(), tableFrame.width() * overlay->width() / 4,
        tableFrame.height() * overlay->height() / 3);
    const auto inputRect = cellInput->mapRectToItem(overlay,
        QRectF(0, 0, cellInput->width(), cellInput->height()));
    QVERIFY2(firstCellInOverlay.adjusted(-2, -2, 2, 2).contains(inputRect),
             "Cell input must be inside the table cell, not in a separate dialog.");
    QVERIFY(cellInput->setProperty("text", QStringLiteral("Cancelled input")));
    auto *cellCancelButton = itemNamed(QStringLiteral("tableCellCancelButton"));
    QVERIFY(cellCancelButton && cellPanel()->isAncestorOf(cellCancelButton));
    clickItem(cellCancelButton);
    QTRY_VERIFY(!cellInputVisible());
    QTRY_VERIFY(!cellPanelVisible());
    QVERIFY(!controller.hasDrafts());

    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QVERIFY(cellInput->property("text").toString().isEmpty());
    const QString oversized(1025, QLatin1Char('a'));
    QVERIFY(cellInput->setProperty("text", oversized));
    cellInput->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    QQuickItem *cellError = nullptr;
    QTRY_VERIFY((cellError = itemNamed(QStringLiteral("tableCellError")))
                && cellError->isVisible() && !cellError->property("text").toString().isEmpty());
    QCOMPARE(cellInput->property("text").toString(), oversized);
    QVERIFY(!controller.hasDrafts());
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(cellInputVisible());
    QCOMPARE(cellInput->property("text").toString(), oversized);
    QTRY_VERIFY((cellEditor = itemNamed(QStringLiteral("tableCellInlineEditor"))) && cellEditor->isVisible());
    QVERIFY(firstCellInOverlay.adjusted(-2, -2, 2, 2).contains(
        cellEditor->mapToItem(overlay, QPointF(0, 0))));
    QVERIFY(!controller.hasDrafts()); // Invalid input must not advance to another cell.
    QTRY_VERIFY(cellPanelVisible());
    QVERIFY(cellPanel()->isAncestorOf(cellError));
    QVERIFY(!itemNamed(QStringLiteral("drawingOpacitySlider")));
    auto *cellApplyButton = itemNamed(QStringLiteral("tableCellApplyButton"));
    QVERIFY(cellApplyButton && cellPanel()->isAncestorOf(cellApplyButton));
    clickItem(cellApplyButton);
    QTRY_VERIFY(cellInputVisible());
    QTRY_VERIFY(cellPanelVisible());
    QCOMPARE(cellInput->property("text").toString(), oversized);
    QCOMPARE(controller.selectedDrawingOpacity(), 1.0);
    QVERIFY(!controller.hasDrafts()); // Applying invalid text keeps the panel and input available.
    QTest::keySequence(window, QKeySequence(QKeySequence::Save));
    QTRY_VERIFY(cellInputVisible());
    QVERIFY(!controller.saving());
    QVERIFY(pageText(savedPath).trimmed().isEmpty());
    QVERIFY(tabs->setProperty("currentIndex", 0));
    QTRY_COMPARE(tabs->property("currentIndex").toInt(), 1);
    QTRY_VERIFY(cellInputVisible());
    QCOMPARE(cellInput->property("text").toString(), oversized);
    QVERIFY(!controller.hasDrafts());
    if (!screenshotPrefix.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshotPrefix + QStringLiteral("-cell-error.png")));
    }

    // Enter remains a line break; finishing is an explicit Ctrl/Cmd+Enter.
    QVERIFY(cellInput->setProperty("text", QStringLiteral("Ô đầu")));
    QVERIFY(cellInput->setProperty("cursorPosition", QStringLiteral("Ô đầu").size()));
    cellInput->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(cellInput->property("text").toString(), QStringLiteral("Ô đầu\n"));
    const QString cellText = QStringLiteral("Ô đầu\nDòng hai");
    QVERIFY(cellInput->setProperty("text", cellText));
    // Recycling the page only destroys its editor view. Pending text belongs
    // to the workspace and must be restored when the page is visible again.
    const QPointer<QQuickItem> pooledInput = cellInput;
    const QPointer<QQuickItem> retainedPanel = cellPanel();
    QVERIFY(overlay->setProperty("pooled", true));
    QTRY_VERIFY(!cellInputVisible());
    QTRY_VERIFY(pooledInput.isNull());
    QVERIFY(!retainedPanel.isNull());
    QVERIFY(cellPanelVisible()); // Workspace-owned controls outlive the page delegate.
    QCOMPARE(cellPanel(), retainedPanel.data());
    auto *revealCellButton = itemNamed(QStringLiteral("tableCellRevealButton"));
    QVERIFY(revealCellButton && revealCellButton->isVisible() && revealCellButton->isEnabled());
    QVERIFY(!itemNamed(QStringLiteral("tableCellFontBox"))->isEnabled());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(overlay->setProperty("pooled", false));
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QTRY_VERIFY(!revealCellButton->isVisible());
    QVERIFY(itemNamed(QStringLiteral("tableCellFontBox"))->isEnabled());
    QVERIFY(!itemNamed(QStringLiteral("tableCellAppendRowButton")));
    QVERIFY(!itemNamed(QStringLiteral("tableCellAppendColumnButton")));
    QVERIFY(!itemNamed(QStringLiteral("tableCellRemoveRowButton")));
    QVERIFY(!itemNamed(QStringLiteral("tableCellRemoveColumnButton")));
    QCOMPARE(cellInput->property("text").toString(), cellText);
    QTRY_VERIFY(cellInput->hasActiveFocus());
    if (!screenshotPrefix.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshotPrefix + QStringLiteral("-cell-inline.png")));
    }
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(controller.hasDrafts());
    QTRY_VERIFY(!cellInputVisible());
    QTRY_VERIFY(!cellPanelVisible());
    QCOMPARE(currentCellTexts().at(0), cellText);
    QQuickItem *cellPreview = nullptr;
    QTRY_VERIFY((cellPreview = itemNamed(QStringLiteral("tableCellPreview0"))) && cellPreview->isVisible());
    QCOMPARE(cellPreview->property("text").toString(), cellText);
    if (!screenshotPrefix.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshotPrefix + QStringLiteral("-cell-text.png")));
    }
    controller.undo();
    QVERIFY(!controller.hasDrafts()); // All typing in one cell is one document Undo.
    controller.redo();
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.save());
    QVERIFY(pageText(savedPath).contains(QStringLiteral("Ô đầu")));
    auto savedCells = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QCOMPARE(savedCells.value(QStringLiteral("tableCellTexts")).toStringList().at(0), cellText);
    QVERIFY(controller.selectCandidate(0, savedCells.value(QStringLiteral("objectId")).toString()));
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QCOMPARE(cellInput->property("text").toString(), cellText);
    cellInput->forceActiveFocus();
    QVERIFY(cellInput->setProperty("cursorPosition", cellText.size()));
    for (const char character : QByteArrayLiteral(" temporary"))
        QTest::keyClick(window, character);
    QTRY_COMPARE(cellInput->property("text").toString(), cellText + QStringLiteral(" temporary"));
    QTest::keySequence(window, QKeySequence(QKeySequence::Undo));
    QTRY_COMPARE(cellInput->property("text").toString(), cellText);
    QVERIFY(cellInputVisible());
    QVERIFY(!controller.hasDrafts()); // Undo while typing stays local to the input.
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(!cellInputVisible());
    QVERIFY(!controller.hasDrafts()); // Finishing identical text is a no-op.

    // Standard Save must include text still being typed in the cell.
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    const QString savedInline = QStringLiteral("Saved inline");
    QVERIFY(cellInput->setProperty("text", savedInline));
    QVERIFY(QMetaObject::invokeMethod(cellInput, "select", Q_ARG(int, 0), Q_ARG(int, savedInline.size())));
    QVERIFY(enterSizeText(QStringLiteral("14")));
    // Save flushes the numeric field while it still owns keyboard focus.
    QTest::keySequence(window, QKeySequence(QKeySequence::Save));
    QTRY_VERIFY(!cellInputVisible());
    QTRY_VERIFY(!controller.saving());
    QVERIFY(!controller.hasDrafts());
    // A larger size can wrap at the word boundary in the narrow first cell.
    QTRY_VERIFY2(pageText(savedPath).simplified().contains(savedInline), qPrintable(pageText(savedPath)));
    savedCells = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("sourceShape"));
    QCOMPARE(savedCells.value(QStringLiteral("tableCellTexts")).toStringList().first(), savedInline);
    QCOMPARE(tableTextFontSizes(savedPath), QSet<int>({14}));
    const auto savedSizeSpans = savedCells.value(QStringLiteral("tableCellFormats")).toList()
        .first().toMap().value(QStringLiteral("spans")).toList();
    QVERIFY(!savedSizeSpans.isEmpty());
    QCOMPARE(savedSizeSpans.first().toMap().value(QStringLiteral("fontSize")).toInt(), 14);
    QVERIFY(controller.selectCandidate(0, savedCells.value(QStringLiteral("objectId")).toString()));

    // Tab commits this cell and opens its neighbor; Shift+Tab goes back.
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QVERIFY(cellInput->setProperty("text", QStringLiteral("First")));
    cellInput->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QTRY_VERIFY(cellInput->property("text").toString().isEmpty());
    QCOMPARE(currentCellTexts().at(0), QStringLiteral("First"));
    QVERIFY(cellInput->mapToItem(overlay, QPointF(0, 0)).x() > firstCellInOverlay.right() - 2);
    QVERIFY(cellInput->setProperty("text", QStringLiteral("Second")));
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QTRY_COMPARE(cellInput->property("text").toString(), QStringLiteral("First"));
    QCOMPARE(currentCellTexts().at(1), QStringLiteral("Second"));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!cellInputVisible());
    controller.undo();
    QCOMPARE(currentCellTexts().at(0), QStringLiteral("First"));
    QVERIFY(currentCellTexts().at(1).isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());

    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QVERIFY(cellInput->setProperty("text", QStringLiteral("Outside click")));
    const QPoint outsideCell = overlay->mapToScene(QPointF(
        (tableFrame.left() - .03) * overlay->width(),
        (tableFrame.top() + tableFrame.height() / 6) * overlay->height())).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, outsideCell);
    QTRY_VERIFY(!cellInputVisible());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.selectCandidate(0, savedCells.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(currentCellTexts().at(0), QStringLiteral("Outside click"));
    controller.undo();
    QVERIFY(!controller.hasDrafts());

    // Range formatting preserves the selection through toolbar interaction;
    // caret formatting affects only subsequently inserted text.
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QVERIFY(cellInput->setProperty("text", QStringLiteral("Range text")));
    QVERIFY(QMetaObject::invokeMethod(cellInput, "select", Q_ARG(int, 0), Q_ARG(int, 10)));
    QQuickItem *fontBox = itemNamed(QStringLiteral("tableCellFontBox"));
    QVERIFY(fontBox);
    int fixtureIndex = -1;
    for (int index = 0; index < fontBox->property("count").toInt(); ++index) {
        QString family;
        QVERIFY(QMetaObject::invokeMethod(fontBox, "textAt", Q_RETURN_ARG(QString, family), Q_ARG(int, index)));
        if (family == testFontFamily()) {
            fixtureIndex = index;
            break;
        }
    }
    QVERIFY(fixtureIndex >= 0);
    QVERIFY(fontBox->setProperty("currentIndex", fixtureIndex));
    QVERIFY(QMetaObject::invokeMethod(fontBox, "activated", Q_ARG(int, fixtureIndex)));
    QVERIFY(QMetaObject::invokeMethod(cellInput, "select", Q_ARG(int, 0), Q_ARG(int, 5)));
    for (const auto &name : {QStringLiteral("tableCellBoldButton"), QStringLiteral("tableCellItalicButton"),
                             QStringLiteral("tableCellUnderlineButton")}) {
        QQuickItem *button = itemNamed(name);
        QVERIFY(button && button->isEnabled());
        clickItem(button);
    }
    const auto inlineState = [&] {
        return controller.tableCellEditorState(cellInput->property("textDocument").value<QQuickTextDocument *>());
    };
    const auto inlineFormat = [&] {
        PDFTableCellFormat::Cell format;
        const auto snapshot = inlineState();
        const bool valid = PDFTableCellFormat::fromVariant(snapshot.value(QStringLiteral("format")).toMap(),
            snapshot.value(QStringLiteral("text")).toString(), format);
        return valid ? format : PDFTableCellFormat::Cell{};
    };
    QTRY_VERIFY(PDFTableCellFormat::styleAt(inlineFormat(), 0).bold);
    QVERIFY(PDFTableCellFormat::styleAt(inlineFormat(), 0).italic);
    QVERIFY(PDFTableCellFormat::styleAt(inlineFormat(), 0).underline);
    QCOMPARE(PDFTableCellFormat::styleAt(inlineFormat(), 0).family, testFontFamily());
    QVERIFY(!PDFTableCellFormat::styleAt(inlineFormat(), 6).bold);
    QVERIFY(!PDFTableCellFormat::styleAt(inlineFormat(), 6).italic);
    QVERIFY(!PDFTableCellFormat::styleAt(inlineFormat(), 6).underline);
    QCOMPARE(cellInput->property("selectionStart").toInt(), 0);
    QCOMPARE(cellInput->property("selectionEnd").toInt(), 5);
    QTest::keySequence(window, QKeySequence(QKeySequence::Undo));
    QTRY_VERIFY(!PDFTableCellFormat::styleAt(inlineFormat(), 0).underline);
    QVERIFY(PDFTableCellFormat::styleAt(inlineFormat(), 0).bold);
    QVERIFY(!controller.hasDrafts());
    QTest::keySequence(window, QKeySequence(QKeySequence::Redo));
    QTRY_VERIFY(PDFTableCellFormat::styleAt(inlineFormat(), 0).underline);
    const qreal unselectedSize = PDFTableCellFormat::styleAt(inlineFormat(), 6).fontSize;
    // Native redo restores a caret; select the range for this new formatting action.
    QVERIFY(QMetaObject::invokeMethod(cellInput, "select", Q_ARG(int, 0), Q_ARG(int, 5)));
    QVERIFY(enterSizeText(QStringLiteral("18")));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(PDFTableCellFormat::styleAt(inlineFormat(), 0).fontSize, 18);
    QCOMPARE(PDFTableCellFormat::styleAt(inlineFormat(), 6).fontSize, unselectedSize);
    QCOMPARE(cellInput->property("selectionStart").toInt(), 0);
    QCOMPARE(cellInput->property("selectionEnd").toInt(), 5);
    QTest::keySequence(window, QKeySequence(QKeySequence::Undo));
    QTRY_COMPARE(PDFTableCellFormat::styleAt(inlineFormat(), 0).fontSize, unselectedSize);
    QTest::keySequence(window, QKeySequence(QKeySequence::Redo));
    QTRY_COMPARE(PDFTableCellFormat::styleAt(inlineFormat(), 0).fontSize, 18);
    QVERIFY(QMetaObject::invokeMethod(cellInput, "select", Q_ARG(int, 10), Q_ARG(int, 10)));
    QVERIFY(enterSizeText(QStringLiteral("16")));
    QTest::keyClick(window, Qt::Key_Return);
    QCOMPARE(PDFTableCellFormat::styleAt(inlineFormat(), 0).fontSize, 18);
    QCOMPARE(PDFTableCellFormat::styleAt(inlineFormat(), 6).fontSize, unselectedSize);
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_U, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_Exclam);
    QTRY_COMPARE(cellInput->property("text").toString(), QStringLiteral("Range text!"));
    QVERIFY(PDFTableCellFormat::styleAt(inlineFormat(), 10).bold);
    QVERIFY(PDFTableCellFormat::styleAt(inlineFormat(), 10).italic);
    QVERIFY(PDFTableCellFormat::styleAt(inlineFormat(), 10).underline);
    QVERIFY(!PDFTableCellFormat::styleAt(inlineFormat(), 6).bold);
    QCOMPARE(PDFTableCellFormat::styleAt(inlineFormat(), 10).fontSize, 16);
    const auto beforeRotation = inlineFormat();
    auto *rotationBox = itemNamed(QStringLiteral("tableCellRotationBox"));
    QVERIFY(rotationBox && rotationBox->isEnabled());
    QVERIFY(rotationBox->setProperty("currentIndex", 1));
    QVERIFY(QMetaObject::invokeMethod(rotationBox, "activated", Q_ARG(int, 1)));
    QTRY_COMPARE(inlineFormat().rotation, 90);
    QCOMPARE(inlineFormat().spans, beforeRotation.spans);
    QCOMPARE(inlineFormat().defaultStyle, beforeRotation.defaultStyle);
    for (const int index : {2, 3, 1}) {
        QVERIFY(rotationBox->setProperty("currentIndex", index));
        QVERIFY(QMetaObject::invokeMethod(rotationBox, "activated", Q_ARG(int, index)));
        QTRY_COMPARE(inlineFormat().rotation, index * 90);
        QCOMPARE(inlineFormat().spans, beforeRotation.spans);
        auto *viewport = itemNamed(QStringLiteral("tableCellRotatedViewport"));
        auto *frame = itemNamed(QStringLiteral("tableCellInlineEditor"));
        QVERIFY(viewport && frame);
        QTRY_COMPARE(viewport->rotation(), qreal(index * 90));
        const QRectF transformed = viewport->mapRectToItem(frame,
            QRectF(0, 0, viewport->width(), viewport->height()));
        QVERIFY(QLineF(transformed.topLeft(), QPointF()).length() < .01);
        QVERIFY(QLineF(transformed.bottomRight(), QPointF(frame->width(), frame->height())).length() < .01);
    }
    const auto rotatedSnapshot = inlineState();
    QVERIFY(!controller.applyTableCellEditorFormat(
        cellInput->property("textDocument").value<QQuickTextDocument *>(),
        QStringLiteral("rotation"), 45).isEmpty());
    QCOMPARE(inlineState(), rotatedSnapshot);
    QVariantMap formattedSnapshot = inlineState().value(QStringLiteral("format")).toMap();
    if (!screenshotPrefix.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(window->grabWindow().save(screenshotPrefix + QStringLiteral("-cell-formatted.png")));
    }
    QVERIFY(enterSizeText(QStringLiteral("20")));
    // Pooling flushes a valid pending size before destroying the inline editor.
    QVERIFY(overlay->setProperty("pooled", true));
    QTRY_VERIFY(!cellInputVisible());
    QVERIFY(!itemNamed(QStringLiteral("tableCellFontSizeBox"))->isEnabled());
    QVERIFY(overlay->setProperty("pooled", false));
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QTRY_COMPARE(inlineState().value(QStringLiteral("format")).toMap(), formattedSnapshot);
    QCOMPARE(inlineFormat().rotation, 90);
    QCOMPARE(itemNamed(QStringLiteral("tableCellRotationBox"))->property("currentIndex").toInt(), 1);
    QCOMPARE(cellInput->property("cursorPosition").toInt(), 11);
    QTRY_COMPARE(inlineState().value(QStringLiteral("fontSize")).toInt(), 20);
    cellInput->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Question);
    QTRY_COMPARE(cellInput->property("text").toString(), QStringLiteral("Range text!?"));
    QCOMPARE(PDFTableCellFormat::styleAt(inlineFormat(), 11).fontSize, 20);
    formattedSnapshot = inlineState().value(QStringLiteral("format")).toMap();
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(!cellInputVisible());
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("tableCellFormats")).toList().first().toMap(), formattedSnapshot);
    controller.undo();
    QVERIFY(!controller.hasDrafts());

    // A rejected pending edit must retain all styles when its delegate recycles.
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    const QString complexText(258, QLatin1Char('x'));
    PDFTableCellFormat::Cell complexFormat{{testFontFamily()}, {}};
    for (int index = 0; index < complexText.size(); ++index)
        complexFormat.spans.append({index, 1, {testFontFamily(), index % 2 != 0, false, false}});
    controller.initializeTableCellEditor(cellInput->property("textDocument").value<QQuickTextDocument *>(),
        complexText, PDFTableCellFormat::toVariant(complexFormat), {});
    cellInput->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    QTRY_VERIFY(cellInputVisible());
    QVERIFY(!controller.hasDrafts());
    const QVariantMap complexSnapshot = inlineState().value(QStringLiteral("format")).toMap();
    QCOMPARE(complexSnapshot.value(QStringLiteral("spans")).toList().size(), 258);
    QVERIFY(overlay->setProperty("pooled", true));
    QTRY_VERIFY(!cellInputVisible());
    QVERIFY(overlay->setProperty("pooled", false));
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QTRY_COMPARE(inlineState().value(QStringLiteral("format")).toMap(), complexSnapshot);
    QTRY_VERIFY(cellInput->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!cellInputVisible());
    QVERIFY(!controller.hasDrafts());

    // Invalid font size stays editable; Apply commits a corrected size and rotation together.
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QVERIFY(cellInput->setProperty("text", QStringLiteral("Hi")));
    QVERIFY(QMetaObject::invokeMethod(cellInput, "select", Q_ARG(int, 0), Q_ARG(int, 2)));
    rotationBox = itemNamed(QStringLiteral("tableCellRotationBox"));
    QVERIFY(rotationBox->setProperty("currentIndex", 1));
    QVERIFY(QMetaObject::invokeMethod(rotationBox, "activated", Q_ARG(int, 1)));
    QTRY_COMPARE(inlineFormat().rotation, 90);
    QVERIFY(enterSizeText(QStringLiteral("0")));
    cellApplyButton = itemNamed(QStringLiteral("tableCellApplyButton"));
    QVERIFY(cellApplyButton && cellApplyButton->isEnabled());
    clickItem(cellApplyButton);
    QTRY_VERIFY(cellInputVisible());
    QCOMPARE(cellInput->property("text").toString(), QStringLiteral("Hi"));
    QVERIFY(!controller.hasDrafts());
    QTRY_VERIFY((cellError = itemNamed(QStringLiteral("tableCellError")))
        && cellError->isVisible() && !cellError->property("text").toString().isEmpty());
    QVERIFY(enterSizeText(QStringLiteral("20")));
    clickItem(cellApplyButton); // The size field still owns focus when Apply begins.
    QTRY_VERIFY(!cellInputVisible());
    QCOMPARE(currentCellTexts().at(0), QStringLiteral("Hi"));
    PDFTableCellFormat::Cell committedFormat;
    QVERIFY(PDFTableCellFormat::fromVariant(controller.selectedDraftPreview()
        .value(QStringLiteral("tableCellFormats")).toList().at(0).toMap(), QStringLiteral("Hi"), committedFormat));
    QCOMPARE(committedFormat.rotation, 90);
    QCOMPARE(PDFTableCellFormat::styleAt(committedFormat, 0).fontSize, 20);
    controller.undo();
    QVERIFY(!controller.hasDrafts());

    // Applying the cell restores shape properties; their edits keep a separate undo step.
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QVERIFY(cellInput->setProperty("text", QStringLiteral("Panel transition")));
    QTRY_VERIFY((cellApplyButton = itemNamed(QStringLiteral("tableCellApplyButton")))
                && cellApplyButton->isVisible() && cellApplyButton->isEnabled());
    clickItem(cellApplyButton);
    QTRY_VERIFY(!cellInputVisible());
    QTRY_VERIFY(!cellPanelVisible());
    QQuickItem *opacitySlider = nullptr;
    QTRY_VERIFY((opacitySlider = itemNamed(QStringLiteral("drawingOpacitySlider")))
                && opacitySlider->isVisible() && opacitySlider->isEnabled());
    clickItem(opacitySlider);
    QTRY_VERIFY(controller.selectedDrawingOpacity() < 1.0);
    QCOMPARE(currentCellTexts().at(0), QStringLiteral("Panel transition"));
    controller.undo();
    QCOMPARE(controller.selectedDrawingOpacity(), 1.0);
    QCOMPARE(currentCellTexts().at(0), QStringLiteral("Panel transition"));
    controller.undo();
    QVERIFY(!controller.hasDrafts());

    // The ribbon preflight commits valid input before changing tools/tabs.
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, cellPoint);
    QTRY_VERIFY((cellInput = itemNamed(QStringLiteral("tableCellTextInput"))) && cellInput->isVisible());
    QVERIFY(cellInput->setProperty("text", QStringLiteral("Tab transition")));
    QVERIFY(tabs->setProperty("currentIndex", 0));
    QTRY_COMPARE(tabs->property("currentIndex").toInt(), 0);
    QTRY_VERIFY(!cellInputVisible());
    QVERIFY(controller.hasDrafts());
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QTRY_COMPARE(controller.tool(), PDFEditController::SelectTool);
    QVERIFY(controller.selectCandidate(0, savedCells.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(currentCellTexts().at(0), QStringLiteral("Tab transition"));
    controller.discardDrafts();
    // Switching ribbon tabs destroys and recreates the lazy Edit ribbon.
    QTRY_VERIFY((tableButton = itemNamed(QStringLiteral("editTableButton")))
                && tableButton->isVisible() && tableButton->isEnabled());
    clickItem(tableButton);
    QTRY_VERIFY(itemNamed(QStringLiteral("tableRowsSpinBox")));
    QVERIFY(tabs->setProperty("currentIndex", 0));
    QTRY_VERIFY(!itemNamed(QStringLiteral("tableRowsSpinBox")));
    QVERIFY(root->setProperty("allowWindowClose", true));
}
