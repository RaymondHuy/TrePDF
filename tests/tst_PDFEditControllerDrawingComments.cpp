#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::ownsPendingDrawingInteractionAcrossPages()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("drawing-interaction-owner.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(document.pageCount(), 2);
    controller.setDocument(&document);

    QCOMPARE(controller.pendingDrawingCommentPage(), -1);
    QVERIFY(!controller.claimDrawingCommentInteraction(0));
    controller.setTool(PDFEditController::SquareTool);
    QCOMPARE(
        static_cast<int>(PDFEditController::SquareCommentTool),
        static_cast<int>(PDFEditController::SquareTool));

    QSignalSpy ownerChanged(
        &controller,
        &PDFEditController::pendingDrawingCommentPageChanged);
    QVERIFY(controller.claimDrawingCommentInteraction(0));
    QCOMPARE(controller.pendingDrawingCommentPage(), 0);
    QCOMPARE(ownerChanged.count(), 1);

    QVERIFY(controller.claimDrawingCommentInteraction(0));
    QCOMPARE(ownerChanged.count(), 1);
    QVERIFY(controller.claimDrawingCommentInteraction(1));
    QCOMPARE(controller.pendingDrawingCommentPage(), 1);
    QCOMPARE(ownerChanged.count(), 2);

    controller.releaseDrawingCommentInteraction(0);
    QCOMPARE(controller.pendingDrawingCommentPage(), 1);
    QCOMPARE(ownerChanged.count(), 2);
    controller.releaseDrawingCommentInteraction(1);
    QCOMPARE(controller.pendingDrawingCommentPage(), -1);
    QCOMPARE(ownerChanged.count(), 3);

    QVERIFY(controller.claimDrawingCommentInteraction(0));
    controller.setTool(PDFEditController::CircleTool);
    QCOMPARE(controller.pendingDrawingCommentPage(), -1);
    QVERIFY(!controller.claimDrawingCommentInteraction(-1));
    QVERIFY(!controller.claimDrawingCommentInteraction(2));
}

void PDFEditControllerTest::rejectsInvalidDrawingCommentGeometry_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<QVariantList>("vertices");

    QTest::newRow("square-extra-point")
        << static_cast<int>(PDFEditController::SquareTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.1, 0.1)),
               QVariant::fromValue(QPointF(0.3, 0.3)),
               QVariant::fromValue(QPointF(0.4, 0.4))};
    QTest::newRow("circle-one-point")
        << static_cast<int>(PDFEditController::CircleTool)
        << QVariantList{QVariant::fromValue(QPointF(0.1, 0.1))};
    QTest::newRow("line-extra-point")
        << static_cast<int>(PDFEditController::LineTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.1, 0.1)),
               QVariant::fromValue(QPointF(0.3, 0.3)),
               QVariant::fromValue(QPointF(0.4, 0.4))};
    QTest::newRow("polygon-too-few-points")
        << static_cast<int>(PDFEditController::PolygonTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.1, 0.1)),
               QVariant::fromValue(QPointF(0.3, 0.3))};
    QTest::newRow("polygon-zero-area")
        << static_cast<int>(PDFEditController::PolygonTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.1, 0.1)),
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(0.3, 0.3))};
    QTest::newRow("polyline-duplicate-points")
        << static_cast<int>(PDFEditController::PolyLineTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(0.2, 0.2))};
    QTest::newRow("ink-one-point")
        << static_cast<int>(PDFEditController::InkTool)
        << QVariantList{QVariant::fromValue(QPointF(0.2, 0.2))};
    QTest::newRow("ink-duplicate-points")
        << static_cast<int>(PDFEditController::InkTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(0.2, 0.2))};
    QTest::newRow("ink-too-short")
        << static_cast<int>(PDFEditController::InkTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(0.201, 0.2))};
    QTest::newRow("ink-non-finite")
        << static_cast<int>(PDFEditController::InkTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(
                   std::numeric_limits<qreal>::infinity(),
                   0.3))};

    QVariantList oversizedInkStroke;
    oversizedInkStroke.reserve(4097);
    for (int index = 0; index < 4097; ++index) {
        oversizedInkStroke.append(QVariant::fromValue(QPointF(
            static_cast<qreal>(index) / 4096.0,
            index % 2 == 0 ? 0.2 : 0.3)));
    }
    QTest::newRow("ink-too-many-points")
        << static_cast<int>(PDFEditController::InkTool)
        << oversizedInkStroke;
}

void PDFEditControllerTest::rejectsInvalidDrawingCommentGeometry()
{
    QFETCH(int, tool);
    QFETCH(QVariantList, vertices);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("invalid-drawing-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(static_cast<PDFEditController::Tool>(tool));

    QVERIFY(controller.addDrawingComment(0, vertices).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.draftsForPage(0).size(), 0);
    QCOMPARE(controller.tool(), static_cast<PDFEditController::Tool>(tool));
}

void PDFEditControllerTest::
    defersDrawingCommentListUpdatesDuringFrameGestures_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QVariantList>("vertices");
    QTest::addColumn<QString>("geometryKey");

    QTest::newRow("square")
        << static_cast<int>(PDFEditController::SquareTool)
        << QStringLiteral("square")
        << QVariantList{
               QVariant::fromValue(QPointF(0.14, 0.18)),
               QVariant::fromValue(QPointF(0.42, 0.38))}
        << QStringLiteral("annotationPoints");

    QVariantList inkPoints;
    inkPoints.reserve(256);
    for (int index = 0; index < 256; ++index) {
        inkPoints.append(QVariant::fromValue(QPointF(
            0.12 + static_cast<qreal>(index) * 0.001,
            index % 2 == 0 ? 0.20 : 0.32)));
    }
    QTest::newRow("ink")
        << static_cast<int>(PDFEditController::InkTool)
        << QStringLiteral("ink")
        << inkPoints
        << QStringLiteral("annotationStrokes");
}

void PDFEditControllerTest::
    defersDrawingCommentListUpdatesDuringFrameGestures()
{
    QFETCH(int, tool);
    QFETCH(QString, kind);
    QFETCH(QVariantList, vertices);
    QFETCH(QString, geometryKey);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("deferred-comment-geometry-%1.pdf").arg(kind));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(static_cast<PDFEditController::Tool>(tool));

    const QVariantMap added = controller.addDrawingComment(0, vertices);
    QVERIFY(!added.isEmpty());
    QCOMPARE(added.value(QStringLiteral("kind")).toString(), kind);
    const QString objectId = added
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF originalFrame = added
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QVariantList originalGeometry = added
        .value(geometryKey)
        .toList();
    QVERIFY(!objectId.isEmpty());
    QVERIFY(originalFrame.isValid());
    QVERIFY(!originalGeometry.isEmpty());
    QCOMPARE(controller.commentItems().size(), 1);

    QSignalSpy commentsChangedSpy(
        &controller,
        &PDFEditController::commentsChanged);
    QSignalSpy selectionChangedSpy(
        &controller,
        &PDFEditController::selectionChanged);
    QSignalSpy selectedGeometryChangedSpy(
        &controller,
        &PDFEditController::selectedGeometryChanged);
    QSignalSpy pageDataChangedSpy(
        &controller,
        &PDFEditController::pageDataChanged);
    QSignalSpy draftRenderStateChangedSpy(
        &controller,
        &PDFEditController::draftRenderStateChanged);
    QSignalSpy historyStateChangedSpy(
        &controller,
        &PDFEditController::historyStateChanged);
    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    QVERIFY(controller.textEditActive());
    commentsChangedSpy.clear();
    selectionChangedSpy.clear();
    selectedGeometryChangedSpy.clear();
    pageDataChangedSpy.clear();
    draftRenderStateChangedSpy.clear();
    historyStateChangedSpy.clear();

    QRectF finalFrame;
    constexpr int updateCount = 12;
    for (int index = 1; index <= updateCount; ++index) {
        const qreal progress
            = static_cast<qreal>(index) / updateCount;
        finalFrame = QRectF(
            originalFrame.x() + 0.12 * progress,
            originalFrame.y() + 0.08 * progress,
            originalFrame.width() * (1.0 - 0.25 * progress),
            originalFrame.height() * (1.0 + 0.30 * progress));
        QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(finalFrame));
        controller.updateSelectedFrameRect(finalFrame);
        QCOMPARE(controller.selectedFrameRect(), finalFrame);
        QCOMPARE(commentsChangedSpy.count(), 0);
        QCOMPARE(selectionChangedSpy.count(), 0);
        QCOMPARE(selectedGeometryChangedSpy.count(), index);
        QCOMPARE(pageDataChangedSpy.count(), 0);
        QCOMPARE(draftRenderStateChangedSpy.count(), 0);
        QCOMPARE(historyStateChangedSpy.count(), 0);
    }

    controller.finishSelectedFrameResize();
    QVERIFY(!controller.textEditActive());
    QCOMPARE(commentsChangedSpy.count(), 1);
    QCOMPARE(pageDataChangedSpy.count(), 1);
    QVERIFY(draftRenderStateChangedSpy.count() >= 1);
    QVERIFY(historyStateChangedSpy.count() >= 1);
    QCOMPARE(controller.selectedFrameRect(), finalFrame);

    const auto commentForObject = [&controller, &objectId]() {
        for (const QVariant &commentValue : controller.commentItems()) {
            const QVariantMap comment = commentValue.toMap();
            if (comment.value(QStringLiteral("objectId")).toString()
                == objectId) {
                return comment;
            }
        }
        return QVariantMap();
    };
    const QVariantList expectedFinalGeometry
        = kind == QLatin1String("ink")
        ? transformedInkStrokes(
              originalGeometry,
              originalFrame,
              finalFrame)
        : transformedDrawingVertices(
              originalGeometry,
              originalFrame,
              finalFrame);
    QVariantMap comment = commentForObject();
    QVERIFY(!comment.isEmpty());
    QCOMPARE(
        comment.value(QStringLiteral("frameRect")).toRectF(),
        finalFrame);
    QCOMPARE(comment.value(geometryKey).toList(), expectedFinalGeometry);

    commentsChangedSpy.clear();
    pageDataChangedSpy.clear();
    draftRenderStateChangedSpy.clear();
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(commentsChangedSpy.count(), 1);
    QCOMPARE(pageDataChangedSpy.count(), 1);
    QCOMPARE(draftRenderStateChangedSpy.count(), 1);
    comment = commentForObject();
    QCOMPARE(
        comment.value(QStringLiteral("frameRect")).toRectF(),
        originalFrame);
    QCOMPARE(comment.value(geometryKey).toList(), originalGeometry);

    commentsChangedSpy.clear();
    pageDataChangedSpy.clear();
    draftRenderStateChangedSpy.clear();
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(commentsChangedSpy.count(), 1);
    QCOMPARE(pageDataChangedSpy.count(), 1);
    QCOMPARE(draftRenderStateChangedSpy.count(), 1);
    comment = commentForObject();
    QCOMPARE(
        comment.value(QStringLiteral("frameRect")).toRectF(),
        finalFrame);
    QCOMPARE(comment.value(geometryKey).toList(), expectedFinalGeometry);

    commentsChangedSpy.clear();
    controller.beginSelectedFrameResize();
    commentsChangedSpy.clear();
    selectionChangedSpy.clear();
    selectedGeometryChangedSpy.clear();
    pageDataChangedSpy.clear();
    draftRenderStateChangedSpy.clear();
    historyStateChangedSpy.clear();
    const QRectF cancelledFrame = finalFrame.translated(-0.05, -0.04);
    for (int index = 1; index <= updateCount; ++index) {
        const qreal progress
            = static_cast<qreal>(index) / updateCount;
        const QRectF previewFrame(
            QPointF(
                finalFrame.x()
                    + (cancelledFrame.x() - finalFrame.x()) * progress,
                finalFrame.y()
                    + (cancelledFrame.y() - finalFrame.y()) * progress),
            finalFrame.size());
        controller.updateSelectedFrameRect(previewFrame);
        QCOMPARE(controller.selectedFrameRect(), previewFrame);
        QCOMPARE(commentsChangedSpy.count(), 0);
        QCOMPARE(selectionChangedSpy.count(), 0);
        QCOMPARE(selectedGeometryChangedSpy.count(), index);
        QCOMPARE(pageDataChangedSpy.count(), 0);
        QCOMPARE(draftRenderStateChangedSpy.count(), 0);
        QCOMPARE(historyStateChangedSpy.count(), 0);
    }
    controller.cancelSelectedFrameResize();
    QVERIFY(!controller.textEditActive());
    QVERIFY(commentsChangedSpy.count() <= 1);
    QCOMPARE(controller.selectedFrameRect(), finalFrame);
    comment = commentForObject();
    QCOMPARE(
        comment.value(QStringLiteral("frameRect")).toRectF(),
        finalFrame);
    QCOMPARE(comment.value(geometryKey).toList(), expectedFinalGeometry);
}

void PDFEditControllerTest::refreshesSourceDrawingAfterFrameGesture_data()
{
    QTest::addColumn<QString>("kind");

    QTest::newRow("square") << QStringLiteral("square");
    QTest::newRow("ink") << QStringLiteral("ink");
}

void PDFEditControllerTest::refreshesSourceDrawingAfterFrameGesture()
{
    QFETCH(QString, kind);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-drawing-frame-exit-%1.pdf").arg(kind));
    QVERIFY(createUnsupportedMarkupCommentsPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    const QVariantMap sourceDrawing = candidateForKind(
        waitForCandidates(controller, 0),
        kind);
    QVERIFY(!sourceDrawing.isEmpty());
    QVERIFY(sourceDrawing.value(QStringLiteral("editable")).toBool());
    const QString objectId = sourceDrawing
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF originalFrame = sourceDrawing
        .value(QStringLiteral("frameRect"))
        .toRectF();
    QVERIFY(controller.selectCandidate(0, objectId));

    QSignalSpy commentsChangedSpy(
        &controller,
        &PDFEditController::commentsChanged);
    QSignalSpy pageDataChangedSpy(
        &controller,
        &PDFEditController::pageDataChanged);
    QSignalSpy draftRenderStateChangedSpy(
        &controller,
        &PDFEditController::draftRenderStateChanged);

    const quint64 initialRevision = controller.draftRevisionForPage(0);
    controller.beginSelectedFrameResize();
    const quint64 noOpSuppressionRevision
        = controller.draftRevisionForPage(0);
    QVERIFY(noOpSuppressionRevision > initialRevision);
    QCOMPARE(
        controller.deletedSourceCommentAnnotationsForPage(0).size(),
        1);
    commentsChangedSpy.clear();
    pageDataChangedSpy.clear();
    draftRenderStateChangedSpy.clear();

    controller.finishSelectedFrameResize();
    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.draftRevisionForPage(0)
        > noOpSuppressionRevision);
    QVERIFY(controller.deletedSourceCommentAnnotationsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(commentsChangedSpy.count(), 0);
    QCOMPARE(pageDataChangedSpy.count(), 1);
    QVERIFY(draftRenderStateChangedSpy.count() >= 1);

    controller.beginSelectedFrameResize();
    const quint64 cancelSuppressionRevision
        = controller.draftRevisionForPage(0);
    QCOMPARE(
        controller.deletedSourceCommentAnnotationsForPage(0).size(),
        1);
    commentsChangedSpy.clear();
    pageDataChangedSpy.clear();
    draftRenderStateChangedSpy.clear();

    const QRectF previewFrame = originalFrame.translated(0.08, 0.04);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(previewFrame));
    controller.updateSelectedFrameRect(previewFrame);
    QCOMPARE(controller.selectedFrameRect(), previewFrame);
    QCOMPARE(controller.draftRevisionForPage(0), cancelSuppressionRevision);
    QCOMPARE(commentsChangedSpy.count(), 0);
    QCOMPARE(pageDataChangedSpy.count(), 0);
    QCOMPARE(draftRenderStateChangedSpy.count(), 0);

    controller.cancelSelectedFrameResize();
    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.draftRevisionForPage(0)
        > cancelSuppressionRevision);
    QVERIFY(controller.deletedSourceCommentAnnotationsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(commentsChangedSpy.count(), 0);
    QCOMPARE(pageDataChangedSpy.count(), 1);
    QVERIFY(draftRenderStateChangedSpy.count() >= 1);
}

void PDFEditControllerTest::savesDrawingCommentsRoundTrip_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<int>("subtype");
    QTest::addColumn<QVariantList>("vertices");

    QTest::newRow("square")
        << static_cast<int>(PDFEditController::SquareTool)
        << QStringLiteral("square")
        << static_cast<int>(FPDF_ANNOT_SQUARE)
        << QVariantList{
               QVariant::fromValue(QPointF(0.14, 0.18)),
               QVariant::fromValue(QPointF(0.42, 0.38))};
    QTest::newRow("circle")
        << static_cast<int>(PDFEditController::CircleTool)
        << QStringLiteral("circle")
        << static_cast<int>(FPDF_ANNOT_CIRCLE)
        << QVariantList{
               QVariant::fromValue(QPointF(0.18, 0.22)),
               QVariant::fromValue(QPointF(0.46, 0.44))};
    QTest::newRow("line")
        << static_cast<int>(PDFEditController::LineTool)
        << QStringLiteral("line")
        << static_cast<int>(FPDF_ANNOT_LINE)
        << QVariantList{
               QVariant::fromValue(QPointF(0.12, 0.28)),
               QVariant::fromValue(QPointF(0.54, 0.34))};
    QTest::newRow("polygon")
        << static_cast<int>(PDFEditController::PolygonTool)
        << QStringLiteral("polygon")
        << static_cast<int>(FPDF_ANNOT_POLYGON)
        << QVariantList{
               QVariant::fromValue(QPointF(0.16, 0.18)),
               QVariant::fromValue(QPointF(0.48, 0.22)),
               QVariant::fromValue(QPointF(0.34, 0.48))};
    QTest::newRow("polyline")
        << static_cast<int>(PDFEditController::PolyLineTool)
        << QStringLiteral("polyLine")
        << static_cast<int>(FPDF_ANNOT_POLYLINE)
        << QVariantList{
               QVariant::fromValue(QPointF(0.12, 0.16)),
               QVariant::fromValue(QPointF(0.34, 0.30)),
               QVariant::fromValue(QPointF(0.56, 0.20))};
}

void PDFEditControllerTest::savesDrawingCommentsRoundTrip()
{
    QFETCH(int, tool);
    QFETCH(QString, kind);
    QFETCH(int, subtype);
    QFETCH(QVariantList, vertices);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("drawing-comment-%1-source.pdf").arg(kind));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("drawing-comment-%1-output.pdf").arg(kind));
    const QString resizedOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("drawing-comment-%1-resized.pdf").arg(kind));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QColor drawingColor(32, 105, 196, 153);
    controller.setDrawingCommentColor(drawingColor);
    controller.setTool(static_cast<PDFEditController::Tool>(tool));

    const QVariantMap added = controller.addDrawingComment(0, vertices);
    QVERIFY(!added.isEmpty());
    QCOMPARE(added.value(QStringLiteral("kind")).toString(), kind);
    QCOMPARE(
        added.value(QStringLiteral("drawingVertices")).toList().size(),
        vertices.size());
    QCOMPARE(
        added.value(QStringLiteral("annotationPoints")).toList(),
        added.value(QStringLiteral("drawingVertices")).toList());
    QCOMPARE(
        added.value(QStringLiteral("drawingStrokeColor")).value<QColor>(),
        QColor(32, 105, 196));
    QCOMPARE(
        added.value(QStringLiteral("strokeColor")).value<QColor>(),
        QColor(32, 105, 196));
    QVERIFY(!added.value(QStringLiteral("fillColor")).value<QColor>().isValid());
    QVERIFY(qAbs(
        added.value(QStringLiteral("drawingOpacity")).toReal()
            - drawingColor.alphaF()) < 0.002);
    QVERIFY(qAbs(
        added.value(QStringLiteral("opacity")).toReal()
            - drawingColor.alphaF()) < 0.002);
    QVERIFY(qAbs(
        added.value(QStringLiteral("strokeWidthRatio")).toReal()
            - 1.5 / document.pageSize(0).height()) < 0.000001);
    QCOMPARE(added.value(QStringLiteral("borderStyle")).toInt(), 0);
    QVERIFY(added.value(QStringLiteral("dashPattern")).toList().isEmpty());
    QCOMPARE(added.value(QStringLiteral("borderEffect")).toInt(), 0);
    QCOMPARE(added.value(QStringLiteral("lineStartStyle")).toInt(), 0);
    QCOMPARE(added.value(QStringLiteral("lineEndStyle")).toInt(), 0);
    QCOMPARE(controller.tool(), PDFEditController::CommentSelectTool);
    QCOMPARE(controller.selectedKind(), kind);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(controller.undo(), -1);
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.redo(), -1);
    QVERIFY(controller.hasDrafts());

    const QString annotationId = added
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!annotationId.isEmpty());
    const QRectF addedFrame = added
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF savedFrame(
        addedFrame.x() + 0.05,
        addedFrame.y() + 0.04,
        addedFrame.width() * 0.80,
        addedFrame.height() * 1.10);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(savedFrame));
    const QVariantList savedVertices = transformedDrawingVertices(
        added.value(QStringLiteral("annotationPoints")).toList(),
        addedFrame,
        savedFrame);
    QVERIFY(controller.selectCandidate(0, annotationId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(savedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), savedFrame);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("annotationPoints"))
            .toList(),
        savedVertices);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedFrameRect(), addedFrame);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), savedFrame);

    const int sourcePageObjectCount = topLevelPageObjectCount(sourcePath);
    QVERIFY(sourcePageObjectCount > 0);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(topLevelPageObjectCount(outputPath), sourcePageObjectCount);
    QCOMPARE(
        annotationSubtypeCount(sourcePath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype)),
        0);
    QCOMPARE(
        annotationSubtypeCount(outputPath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype)),
        1);

    const CommentAnnotationInspection inspection
        = inspectCommentAnnotation(outputPath, annotationId);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.subtype, subtype);
    QVERIFY(inspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(
        inspection.subject,
        added.value(QStringLiteral("label")).toString());
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearance.isEmpty());
    QVERIFY(inspection.appearance.contains(QStringLiteral("/GS gs")));
    QVERIFY(inspection.appearance.contains(QStringLiteral("1.5 w")));
    QVERIFY(inspection.appearance.contains(QStringLiteral("1 J 1 j")));
    QVERIFY(inspection.appearance.contains(QStringLiteral("[] 0 d")));
    const QRegularExpression closePathOperator(
        QStringLiteral(R"((?:^|\s)h(?:\s|$))"));
    const QRegularExpression cubicPathOperator(
        QStringLiteral(R"((?:^|\s)c(?:\s|$))"));
    if (kind == QLatin1String("square")
        || kind == QLatin1String("circle")
        || kind == QLatin1String("polygon")) {
        QCOMPARE(inspection.appearance.count(closePathOperator), 1);
    } else {
        QCOMPARE(inspection.appearance.count(closePathOperator), 0);
    }
    QCOMPARE(
        inspection.appearance.count(cubicPathOperator),
        kind == QLatin1String("circle") ? 4 : 0);
    const QByteArray annotationDictionary = annotationDictionaryBytes(
        outputPath,
        annotationId);
    QVERIFY(!annotationDictionary.isEmpty());
    const QString annotationDictionaryText = QString::fromLatin1(
        annotationDictionary);
    const auto hasDictionaryPattern = [&annotationDictionaryText](
                                          const QString &pattern) {
        return QRegularExpression(pattern)
            .match(annotationDictionaryText)
            .hasMatch();
    };
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/C\s*\[)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/CA\s+)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/BS\s*<<)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/W\s+1\.5\b)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/S\s*/S\b)")));
    QVERIFY(hasDictionaryPattern(
        QStringLiteral(R"(/AP\s*<<\s*/N\s+\d+\s+0\s+R\b)")));
    QVERIFY(!annotationDictionary.contains("/IC"));
    QVERIFY(!annotationDictionary.contains("/BE"));
    QVERIFY(!annotationDictionary.contains("/IT"));
    const QRegularExpression lineGeometryKey(
        QStringLiteral(R"(/L\b)"));
    const QRegularExpression verticesGeometryKey(
        QStringLiteral(R"(/Vertices\b)"));
    const QRegularExpression lineEndingKey(
        QStringLiteral(R"(/LE\b)"));
    QCOMPARE(
        lineGeometryKey.match(annotationDictionaryText).hasMatch(),
        kind == QLatin1String("line"));
    QCOMPARE(
        verticesGeometryKey.match(annotationDictionaryText).hasMatch(),
        kind == QLatin1String("polygon")
            || kind == QLatin1String("polyLine"));
    QCOMPARE(
        lineEndingKey.match(annotationDictionaryText).hasMatch(),
        kind == QLatin1String("line")
            || kind == QLatin1String("polyLine"));
    if (kind == QLatin1String("line")
        || kind == QLatin1String("polyLine")) {
        QVERIFY(hasDictionaryPattern(
            QStringLiteral(
                R"(/LE\s*\[\s*/None\s+/None\s*\])")));
    }
    const bool openDrawingComment
        = kind == QLatin1String("line")
        || kind == QLatin1String("polyLine");
    struct BorderDictionarySemantics {
        QString type;
        QString style;
        qreal width = -1.0;
        bool hasDashPattern = false;
        QString dashPattern;
    };
    const auto borderDictionarySemantics = [](
        const QByteArray &dictionary) {
        BorderDictionarySemantics semantics;
        const QString dictionaryText = QString::fromLatin1(dictionary);
        const auto nameValue = [&dictionaryText](const QString &key) {
            const QRegularExpression expression(
                QStringLiteral(R"(/%1\s*/([^\s/<>{}\[\]()]+))")
                    .arg(QRegularExpression::escape(key)));
            return expression.match(dictionaryText).captured(1);
        };
        semantics.type = nameValue(QStringLiteral("Type"));
        semantics.style = nameValue(QStringLiteral("S"));

        const QRegularExpression widthExpression(
            QStringLiteral(
                R"(/W\s+([+-]?(?:\d+(?:\.\d*)?|\.\d+)))"));
        const QRegularExpressionMatch widthMatch
            = widthExpression.match(dictionaryText);
        if (widthMatch.hasMatch()) {
            semantics.width = widthMatch.captured(1).toDouble();
        }

        const QRegularExpression dashExpression(
            QStringLiteral(R"(/D\s*(\[[^\]]*\]))"));
        const QRegularExpressionMatch dashMatch
            = dashExpression.match(dictionaryText);
        semantics.hasDashPattern = dashMatch.hasMatch();
        if (semantics.hasDashPattern) {
            semantics.dashPattern = dashMatch.captured(1);
            semantics.dashPattern.remove(
                QRegularExpression(QStringLiteral(R"(\s+)")));
        }
        return semantics;
    };
    BorderDictionarySemantics originalBorderSemantics;
    QStringList originalLineEndings;
    if (openDrawingComment) {
        const PdfDictionaryRange annotationRange = pdfDictionaryRangeAt(
            annotationDictionary,
            0);
        const PdfDictionaryRange borderRange = pdfDirectDictionaryValue(
            annotationDictionary,
            annotationRange,
            QByteArrayLiteral("BS"));
        QVERIFY(borderRange.isValid());
        originalBorderSemantics = borderDictionarySemantics(
            annotationDictionary.mid(
                borderRange.start,
                borderRange.end - borderRange.start));
        QCOMPARE(originalBorderSemantics.type, QStringLiteral("Border"));
        QVERIFY(!originalBorderSemantics.style.isEmpty());
        QVERIFY(originalBorderSemantics.width >= 0.0);

        const QRegularExpression lineEndingsExpression(
            QStringLiteral(
                R"(/LE\s*\[\s*/([A-Za-z]+)\s*/([A-Za-z]+)\s*\])"));
        const QRegularExpressionMatch lineEndingsMatch
            = lineEndingsExpression.match(annotationDictionaryText);
        QVERIFY(lineEndingsMatch.hasMatch());
        originalLineEndings = {
            lineEndingsMatch.captured(1),
            lineEndingsMatch.captured(2)};
    }
    const AnnotationAppearanceStructureInspection appearance
        = inspectAnnotationAppearanceStructure(outputPath, annotationId);
    QVERIFY(appearance.readable);
    QVERIFY(appearance.annotationFound);
    QVERIFY(appearance.normalAppearanceIsIndirect);
    QVERIFY(appearance.appearanceObjectFound);
    QVERIFY(appearance.appearanceIsStream);
    QVERIFY(appearance.typeIsXObject);
    QVERIFY(appearance.subtypeIsForm);
    QVERIFY(appearance.matrixIsIdentity);
    QCOMPARE(appearance.boundingBox.size(), 4);
    QVERIFY(appearance.hasResources);
    QVERIFY(appearance.hasExtGState);
    QVERIFY(appearance.hasGraphicsState);
    QVERIFY(appearance.graphicsStateTypeIsExtGState);
    QVERIFY(appearance.blendModeIsNormal);
    QVERIFY(appearance.alphaSourceIsFalse);
    QVERIFY(qAbs(
        appearance.strokingOpacity - drawingColor.alphaF()) < 0.002);
    QVERIFY(qAbs(
        appearance.nonStrokingOpacity - drawingColor.alphaF()) < 0.002);

    controller.requestCommentIndex();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.commentItems().isEmpty(), 5000);
    QVariantMap rehydratedComment;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("kind")).toString() == kind) {
            QCOMPARE(
                comment.value(QStringLiteral("kind")).toString(),
                kind);
            QVERIFY(comment
                .value(QStringLiteral("commentTextEditable"))
                .toBool());
            QVERIFY(comment.value(QStringLiteral("editable")).toBool());
            QCOMPARE(
                comment
                    .value(QStringLiteral("drawingStrokeColor"))
                    .value<QColor>(),
                QColor(32, 105, 196));
            QCOMPARE(
                comment.value(QStringLiteral("strokeColor")).value<QColor>(),
                QColor(32, 105, 196));
            QVERIFY(qAbs(
                comment
                    .value(QStringLiteral("drawingOpacity"))
                    .toReal()
                    - drawingColor.alphaF()) < 0.01);
            QVERIFY(qAbs(
                comment
                    .value(QStringLiteral("drawingBorderWidth"))
                    .toReal()
                    - 1.5) < 0.002);
            QVERIFY(qAbs(
                comment.value(QStringLiteral("strokeWidthRatio")).toReal()
                    - 1.5 / document.pageSize(0).height()) < 0.000001);
            const QVariantList rehydratedVertices = comment
                .value(QStringLiteral("annotationPoints"))
                .toList();
            QCOMPARE(rehydratedVertices.size(), savedVertices.size());
            for (qsizetype index = 0;
                 index < rehydratedVertices.size();
                 ++index) {
                const QPointF actual = rehydratedVertices.at(index).toPointF();
                const QPointF expected = savedVertices.at(index).toPointF();
                QVERIFY(qAbs(actual.x() - expected.x()) < 0.002);
                QVERIFY(qAbs(actual.y() - expected.y()) < 0.002);
            }
            rehydratedComment = comment;
            break;
        }
    }
    QVERIFY(!rehydratedComment.isEmpty());

    QVERIFY(!waitForCandidates(controller, 0).isEmpty());
    const QRectF sourceFrame = rehydratedComment
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QVariantList sourceVertices = rehydratedComment
        .value(QStringLiteral("annotationPoints"))
        .toList();
    const QRectF resizedFrame(
        sourceFrame.x() + 0.08,
        sourceFrame.y() + 0.08,
        sourceFrame.width() * 0.75,
        sourceFrame.height() * 1.25);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(resizedFrame));
    const QVariantList resizedVertices = transformedDrawingVertices(
        sourceVertices,
        sourceFrame,
        resizedFrame);
    QVERIFY(controller.selectComment(
        0,
        rehydratedComment.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedDrawingPropertiesEditable());
    if (openDrawingComment) {
        QVERIFY(!controller.selectedDrawingSupportsFill());
        QVERIFY(controller.setSelectedDrawingStrokeColor(
            QColor(255, 0, 0)));
        QVERIFY(controller.setSelectedDrawingOpacity(0.4));
        QVERIFY(controller.hasDrafts());
    }
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("annotationPoints"))
            .toList(),
        resizedVertices);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(resizedOutputPath)));

    const CommentAnnotationInspection resizedInspection
        = inspectCommentAnnotation(resizedOutputPath, annotationId);
    QVERIFY(resizedInspection.readable);
    QCOMPARE(resizedInspection.subtype, subtype);
    QVERIFY(resizedInspection.hasAppearance);
    QVERIFY(!resizedInspection.appearance.isEmpty());
    QVERIFY(resizedInspection.appearance != inspection.appearance);
    const AnnotationAppearanceStructureInspection resizedAppearance
        = inspectAnnotationAppearanceStructure(
            resizedOutputPath,
            annotationId);
    QVERIFY(resizedAppearance.readable);
    QVERIFY(resizedAppearance.annotationFound);
    QVERIFY(resizedAppearance.appearanceObjectFound);
    QCOMPARE(resizedAppearance.boundingBox.size(), 4);
    QVERIFY(qAbs(
        resizedAppearance.boundingBox.at(0)
            - resizedInspection.rect.left) < 0.02);
    QVERIFY(qAbs(
        resizedAppearance.boundingBox.at(1)
            - resizedInspection.rect.bottom) < 0.02);
    QVERIFY(qAbs(
        resizedAppearance.boundingBox.at(2)
            - resizedInspection.rect.right) < 0.02);
    QVERIFY(qAbs(
        resizedAppearance.boundingBox.at(3)
            - resizedInspection.rect.top) < 0.02);
    const QByteArray resizedDictionary = annotationDictionaryBytes(
        resizedOutputPath,
        annotationId);
    QVERIFY(!resizedDictionary.contains(
        "/TrePDFSourceDrawingGeometryId"));
    if (openDrawingComment) {
        const QString resizedDictionaryText = QString::fromLatin1(
            resizedDictionary);
        const PdfDictionaryRange resizedAnnotationRange
            = pdfDictionaryRangeAt(resizedDictionary, 0);
        const PdfDictionaryRange resizedBorderRange
            = pdfDirectDictionaryValue(
                resizedDictionary,
                resizedAnnotationRange,
                QByteArrayLiteral("BS"));
        QVERIFY(resizedBorderRange.isValid());
        const BorderDictionarySemantics resizedBorderSemantics
            = borderDictionarySemantics(resizedDictionary.mid(
                resizedBorderRange.start,
                resizedBorderRange.end - resizedBorderRange.start));
        QCOMPARE(resizedBorderSemantics.type, originalBorderSemantics.type);
        QCOMPARE(resizedBorderSemantics.style, originalBorderSemantics.style);
        QVERIFY(qAbs(
            resizedBorderSemantics.width
                - originalBorderSemantics.width) < 0.000001);
        QCOMPARE(
            resizedBorderSemantics.hasDashPattern,
            originalBorderSemantics.hasDashPattern);
        QCOMPARE(
            resizedBorderSemantics.dashPattern,
            originalBorderSemantics.dashPattern);

        const QRegularExpression lineEndingsExpression(
            QStringLiteral(
                R"(/LE\s*\[\s*/([A-Za-z]+)\s*/([A-Za-z]+)\s*\])"));
        const QRegularExpressionMatch lineEndingsMatch
            = lineEndingsExpression.match(resizedDictionaryText);
        QVERIFY2(
            lineEndingsMatch.hasMatch(),
            resizedDictionary.constData());
        QCOMPARE(
            QStringList({
                lineEndingsMatch.captured(1),
                lineEndingsMatch.captured(2)}),
            originalLineEndings);
        const QRegularExpression strokeColorExpression(
            QStringLiteral(
                R"(/C\s*\[\s*1(?:\.0+)?\s+0(?:\.0+)?\s+0(?:\.0+)?\s*\])"));
        QVERIFY(strokeColorExpression
            .match(resizedDictionaryText)
            .hasMatch());
        QVERIFY(QRegularExpression(QStringLiteral(R"(/CA\s+0\.4\b)"))
            .match(resizedDictionaryText)
            .hasMatch());
        const QRegularExpression appearanceStrokeColorExpression(
            QStringLiteral(
                R"(1(?:\.0+)?\s+0(?:\.0+)?\s+0(?:\.0+)?\s+RG\b)"));
        QVERIFY(appearanceStrokeColorExpression
            .match(resizedInspection.appearance)
            .hasMatch());
        QVERIFY(qAbs(resizedAppearance.strokingOpacity - 0.4) < 0.002);
        QVERIFY(qAbs(resizedAppearance.nonStrokingOpacity - 0.4) < 0.002);
    }
    QCOMPARE(
        inspectCommentAnnotation(outputPath, annotationId).appearance,
        inspection.appearance);
}

void PDFEditControllerTest::editsSelectedDrawingPropertiesWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("selected-drawing-properties.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquareTool);

    const QVariantMap added = controller.addDrawingComment(
        0,
        QVariantList{
            QVariant::fromValue(QPointF(0.14, 0.18)),
            QVariant::fromValue(QPointF(0.42, 0.38))});
    QVERIFY(!added.isEmpty());
    QCOMPARE(
        added.value(QStringLiteral("kind")).toString(),
        QStringLiteral("square"));
    QVERIFY(controller.selectedDrawingPropertiesEditable());
    QVERIFY(controller.selectedDrawingSupportsFill());

    const QColor initialStroke = controller.selectedDrawingStrokeColor();
    const qreal initialOpacity = controller.selectedDrawingOpacity();
    QVERIFY(initialStroke.isValid());
    QVERIFY(!controller.selectedDrawingHasInterior());
    QVERIFY(!controller.selectedDrawingInteriorColor().isValid());
    QCOMPARE(
        initialStroke,
        added.value(QStringLiteral("drawingStrokeColor")).value<QColor>());
    QVERIFY(qAbs(
        initialOpacity
            - added.value(QStringLiteral("drawingOpacity")).toReal())
        < 0.000001);
    QVERIFY(!controller.selectedDraftPreview()
        .value(QStringLiteral("styleChanged"))
        .toBool());

    const QColor editedStroke(20, 90, 180);
    controller.setSelectedDrawingStrokeColor(editedStroke);
    QCOMPARE(controller.selectedDrawingStrokeColor(), editedStroke);
    QVariantMap preview = controller.selectedDraftPreview();
    QVERIFY(preview.value(QStringLiteral("styleChanged")).toBool());
    QCOMPARE(
        preview.value(QStringLiteral("drawingStrokeColor")).value<QColor>(),
        editedStroke);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("drawingStrokeColor"))
            .value<QColor>(),
        editedStroke);

    // Reapplying the selected value is a no-op. A single undo must restore
    // the original stroke instead of consuming a duplicate command.
    controller.setSelectedDrawingStrokeColor(editedStroke);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedDrawingStrokeColor(), initialStroke);
    QVERIFY(!controller.selectedDraftPreview()
        .value(QStringLiteral("styleChanged"))
        .toBool());
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedDrawingStrokeColor(), editedStroke);

    const QColor editedInterior(236, 170, 35);
    constexpr qreal editedOpacity = 0.35;
    controller.setSelectedDrawingInteriorColor(editedInterior);
    controller.setSelectedDrawingOpacity(editedOpacity);
    QVERIFY(controller.selectedDrawingHasInterior());
    QCOMPARE(controller.selectedDrawingInteriorColor(), editedInterior);
    QVERIFY(qAbs(controller.selectedDrawingOpacity() - editedOpacity)
        < 0.000001);

    preview = controller.selectedDraftPreview();
    QVERIFY(preview.value(QStringLiteral("styleChanged")).toBool());
    QVERIFY(preview.value(QStringLiteral("drawingHasInterior")).toBool());
    QCOMPARE(
        preview.value(QStringLiteral("drawingInteriorColor")).value<QColor>(),
        editedInterior);
    QVERIFY(qAbs(
        preview.value(QStringLiteral("drawingOpacity")).toReal()
            - editedOpacity)
        < 0.000001);

    // Each accepted property change is one command and undo must retain the
    // other property deltas already present in the draft.
    QCOMPARE(controller.undo(), -1);
    QVERIFY(qAbs(controller.selectedDrawingOpacity() - initialOpacity)
        < 0.000001);
    QVERIFY(controller.selectedDrawingHasInterior());
    QCOMPARE(controller.selectedDrawingStrokeColor(), editedStroke);

    QCOMPARE(controller.undo(), -1);
    QVERIFY(!controller.selectedDrawingHasInterior());
    QVERIFY(!controller.selectedDrawingInteriorColor().isValid());
    QCOMPARE(controller.selectedDrawingStrokeColor(), editedStroke);

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedDrawingStrokeColor(), initialStroke);
    QVERIFY(!controller.selectedDraftPreview()
        .value(QStringLiteral("styleChanged"))
        .toBool());

    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedDrawingStrokeColor(), editedStroke);
    QCOMPARE(controller.selectedDrawingInteriorColor(), editedInterior);
    QVERIFY(qAbs(controller.selectedDrawingOpacity() - editedOpacity)
        < 0.000001);

    controller.clearSelectedDrawingInteriorColor();
    QVERIFY(!controller.selectedDrawingHasInterior());
    QVERIFY(!controller.selectedDrawingInteriorColor().isValid());
    preview = controller.selectedDraftPreview();
    QVERIFY(!preview.value(QStringLiteral("drawingHasInterior")).toBool());
    QVERIFY(!preview
        .value(QStringLiteral("drawingInteriorColor"))
        .value<QColor>()
        .isValid());
    QVERIFY(preview.value(QStringLiteral("styleChanged")).toBool());

    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.selectedDrawingHasInterior());
    QCOMPARE(controller.selectedDrawingInteriorColor(), editedInterior);
    QCOMPARE(controller.redo(), -1);
    QVERIFY(!controller.selectedDrawingHasInterior());

    controller.discardDrafts();
}

void PDFEditControllerTest::keepsDrawingPreviewAlignedWithDisplayedTexture()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("drawing-preview-texture-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("drawing-preview-texture-saved.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquareTool);
    QVERIFY(!controller.addDrawingComment(
        0,
        QVariantList{
            QVariant::fromValue(QPointF(0.16, 0.20)),
            QVariant::fromValue(QPointF(0.46, 0.42))}).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));

    controller.requestCommentIndex();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.commentItems().isEmpty(), 5000);
    const QVariantMap sourceSquare = candidateForKind(
        controller.commentItems(),
        QStringLiteral("square"));
    QVERIFY(!sourceSquare.isEmpty());
    const QString objectId = sourceSquare
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!waitForCandidates(controller, 0).isEmpty());
    QVERIFY(controller.selectComment(0, objectId));
    QVERIFY(controller.selectedDrawingPropertiesEditable());
    const QColor originalStroke = controller.selectedDrawingStrokeColor();
    const QColor editedStroke(
        255 - originalStroke.red(),
        255 - originalStroke.green(),
        255 - originalStroke.blue());
    QVERIFY(editedStroke != originalStroke);
    QVERIFY(controller.setSelectedDrawingStrokeColor(editedStroke));
    QCOMPARE(controller.selectedDrawingStrokeColor(), editedStroke);
    QCOMPARE(controller.draftsForPage(0).size(), 1);

    const QString themePath = QFINDTESTDATA(
        "../src/Components/AppTheme.qml");
    const QString overlayPath = editOverlayQmlPath();
    QVERIFY(!themePath.isEmpty());
    QVERIFY(!overlayPath.isEmpty());
    QVERIFY(registerQmlTestTypes(themePath));

    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(
        QByteArrayLiteral("import TrePDF\nAppTheme {}"),
        QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));

    QQmlComponent overlayComponent(
        &engine,
        QUrl::fromLocalFile(overlayPath));
    const QVariantMap initialProperties{
        {QStringLiteral("page"), 0},
        {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), false},
        {QStringLiteral("draftTextureReady"), false},
        {QStringLiteral("displayedSuppressedCommentObjectIds"),
         QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), 1.0},
        {QStringLiteral("width"), 200.0},
        {QStringLiteral("height"), 200.0}};
    std::unique_ptr<QObject> overlay(
        overlayComponent.createWithInitialProperties(initialProperties));
    QVERIFY2(overlay, qPrintable(overlayComponent.errorString()));

    const auto reload = [&overlay]() {
        QVERIFY(QMetaObject::invokeMethod(
            overlay.get(),
            "reloadData",
            Qt::DirectConnection));
    };
    const auto previewCanRender = [&overlay](const QVariantMap &draft) {
        QVariant result;
        const QVariant argument = draft;
        const bool invoked = QMetaObject::invokeMethod(
            overlay.get(),
            "drawingPreviewCanRender",
            Qt::DirectConnection,
            Q_RETURN_ARG(QVariant, result),
            Q_ARG(QVariant, argument));
        return invoked && result.toBool();
    };
    const auto drawingDrafts = [&overlay]() {
        const QVariant value = overlay->property("drawingCommentDrafts");
        return value.metaType() == QMetaType::fromType<QJSValue>()
            ? value.value<QJSValue>().toVariant().toList()
            : value.toList();
    };

    reload();
    QVariantList visibleDrafts = drawingDrafts();
    QCOMPARE(visibleDrafts.size(), 1);
    QVariantMap styleDraft = visibleDrafts.constFirst().toMap();
    QVERIFY(styleDraft.value(QStringLiteral("styleChanged")).toBool());
    QVERIFY(!previewCanRender(styleDraft));

    QVERIFY(overlay->setProperty(
        "displayedSuppressedCommentObjectIds",
        QStringList{objectId}));
    reload();
    visibleDrafts = drawingDrafts();
    QCOMPARE(visibleDrafts.size(), 1);
    styleDraft = visibleDrafts.constFirst().toMap();
    QVERIFY(previewCanRender(styleDraft));

    QCOMPARE(controller.undo(), -1);
    reload();
    visibleDrafts = drawingDrafts();
    QCOMPARE(visibleDrafts.size(), 1);
    const QVariantMap restoration = visibleDrafts.constFirst().toMap();
    QVERIFY(restoration.value(QStringLiteral("restoration")).toBool());
    QVERIFY(previewCanRender(restoration));

    QVERIFY(overlay->setProperty(
        "displayedSuppressedCommentObjectIds",
        QStringList()));
    reload();
    QVERIFY(drawingDrafts().isEmpty());

    QCOMPARE(controller.redo(), -1);
    reload();
    visibleDrafts = drawingDrafts();
    QCOMPARE(visibleDrafts.size(), 1);
    QVERIFY(!previewCanRender(visibleDrafts.constFirst().toMap()));
}

void PDFEditControllerTest::savesSelectedDrawingPropertiesRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("selected-drawing-properties-source.pdf"));
    const QString addedOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("selected-drawing-properties-added.pdf"));
    const QString sourceOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("selected-drawing-properties-source-edited.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquareTool);

    const QVariantMap added = controller.addDrawingComment(
        0,
        QVariantList{
            QVariant::fromValue(QPointF(0.16, 0.20)),
            QVariant::fromValue(QPointF(0.46, 0.42))});
    QVERIFY(!added.isEmpty());
    const QString annotationId
        = added.value(QStringLiteral("objectId")).toString();
    const QColor addedStroke(24, 96, 184);
    const QColor addedInterior(238, 174, 38);
    constexpr qreal addedOpacity = 0.42;
    QVERIFY(controller.setSelectedDrawingStrokeColor(addedStroke));
    QVERIFY(controller.setSelectedDrawingInteriorColor(addedInterior));
    QVERIFY(controller.setSelectedDrawingOpacity(addedOpacity));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(addedOutputPath)));

    const auto dictionaryColor = [](const QByteArray &dictionary,
                                    const QString &key) {
        static const QString number
            = QStringLiteral(R"(([+-]?(?:\d+(?:\.\d*)?|\.\d+)))");
        const QRegularExpression expression(
            QStringLiteral(R"(/%1\s*\[\s*%2\s+%2\s+%2\s*\])")
                .arg(QRegularExpression::escape(key), number));
        const QRegularExpressionMatch match = expression.match(
            QString::fromLatin1(dictionary));
        if (!match.hasMatch()) {
            return QColor();
        }
        return QColor::fromRgbF(
            match.captured(1).toDouble(),
            match.captured(2).toDouble(),
            match.captured(3).toDouble());
    };
    const auto appearanceColor = [](const QString &appearance,
                                    const QString &paintOperator) {
        static const QString number
            = QStringLiteral(R"(([+-]?(?:\d+(?:\.\d*)?|\.\d+)))");
        const QRegularExpression expression(
            QStringLiteral(R"(%1\s+%1\s+%1\s+%2\b)")
                .arg(
                    number,
                    QRegularExpression::escape(paintOperator)));
        const QRegularExpressionMatch match = expression.match(appearance);
        if (!match.hasMatch()) {
            return QColor();
        }
        return QColor::fromRgbF(
            match.captured(1).toDouble(),
            match.captured(2).toDouble(),
            match.captured(3).toDouble());
    };
    const QByteArray addedDictionary = annotationDictionaryBytes(
        addedOutputPath,
        annotationId);
    const PdfDictionaryRange addedDictionaryRange
        = pdfDictionaryRangeAt(addedDictionary, 0);
    const PdfDictionaryRange addedBorderRange = pdfDirectDictionaryValue(
        addedDictionary,
        addedDictionaryRange,
        QByteArrayLiteral("BS"));
    QVERIFY(addedBorderRange.isValid());
    const QByteArray addedBorderDictionary = addedDictionary.mid(
        addedBorderRange.start,
        addedBorderRange.end - addedBorderRange.start);

    const CommentAnnotationInspection addedInspection
        = inspectCommentAnnotation(addedOutputPath, annotationId);
    QVERIFY(addedInspection.readable);
    QCOMPARE(addedInspection.subtype, FPDF_ANNOT_SQUARE);
    QCOMPARE(
        appearanceColor(addedInspection.appearance, QStringLiteral("RG"))
            .rgb(),
        addedStroke.rgb());
    QCOMPARE(
        appearanceColor(addedInspection.appearance, QStringLiteral("rg"))
            .rgb(),
        addedInterior.rgb());
    QCOMPARE(
        dictionaryColor(addedDictionary, QStringLiteral("C")).rgb(),
        addedStroke.rgb());
    QCOMPARE(
        dictionaryColor(addedDictionary, QStringLiteral("IC")).rgb(),
        addedInterior.rgb());
    QVERIFY(addedInspection.hasAppearance);
    const AnnotationAppearanceStructureInspection addedAppearance
        = inspectAnnotationAppearanceStructure(
            addedOutputPath,
            annotationId);
    QVERIFY(addedAppearance.appearanceObjectFound);
    QVERIFY(qAbs(addedAppearance.strokingOpacity - addedOpacity) < 0.002);
    QVERIFY(qAbs(addedAppearance.nonStrokingOpacity - addedOpacity) < 0.002);
    QVERIFY(addedDictionary.contains("/IC"));
    QVERIFY(QRegularExpression(QStringLiteral(R"(/CA\s+0\.42\b)"))
        .match(QString::fromLatin1(addedDictionary))
        .hasMatch());

    controller.requestCommentIndex();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.commentItems().isEmpty(), 5000);
    QVariantMap sourceComment;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("kind")).toString()
                == QLatin1String("square")) {
            sourceComment = comment;
            break;
        }
    }
    QVERIFY(!sourceComment.isEmpty());
    QVERIFY(controller.selectComment(
        0,
        sourceComment.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedDrawingPropertiesEditable());
    QVERIFY(controller.selectedDrawingSupportsFill());
    QCOMPARE(controller.selectedDrawingStrokeColor(), addedStroke);
    QCOMPARE(controller.selectedDrawingInteriorColor(), addedInterior);
    QVERIFY(qAbs(controller.selectedDrawingOpacity() - addedOpacity) < 0.01);

    const QColor sourceStroke(32, 154, 92);
    constexpr qreal sourceOpacity = 0.73;
    QVERIFY(controller.setSelectedDrawingStrokeColor(sourceStroke));
    QVERIFY(controller.clearSelectedDrawingInteriorColor());
    QVERIFY(controller.setSelectedDrawingOpacity(sourceOpacity));
    QVERIFY(controller.selectedDraftPreview()
        .value(QStringLiteral("styleChanged"))
        .toBool());
    const QVector<PDFEditController::DraftAnnotationDeletion> suppressions
        = controller.deletedSourceCommentAnnotationsForPage(0);
    QCOMPARE(suppressions.size(), 1);
    QCOMPARE(suppressions.constFirst().annotationSubtype, FPDF_ANNOT_SQUARE);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(sourceOutputPath)));

    const CommentAnnotationInspection sourceInspection
        = inspectCommentAnnotation(sourceOutputPath, annotationId);
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.subtype, FPDF_ANNOT_SQUARE);
    QVERIFY(sourceInspection.hasAppearance);
    QVERIFY(sourceInspection.appearance != addedInspection.appearance);
    QCOMPARE(
        appearanceColor(sourceInspection.appearance, QStringLiteral("RG"))
            .rgb(),
        sourceStroke.rgb());
    QVERIFY(!appearanceColor(
        sourceInspection.appearance,
        QStringLiteral("rg")).isValid());
    QVERIFY(qAbs(sourceInspection.rect.left - addedInspection.rect.left) < 0.02);
    QVERIFY(qAbs(sourceInspection.rect.bottom - addedInspection.rect.bottom) < 0.02);
    QVERIFY(qAbs(sourceInspection.rect.right - addedInspection.rect.right) < 0.02);
    QVERIFY(qAbs(sourceInspection.rect.top - addedInspection.rect.top) < 0.02);

    const AnnotationAppearanceStructureInspection sourceAppearance
        = inspectAnnotationAppearanceStructure(
            sourceOutputPath,
            annotationId);
    QVERIFY(sourceAppearance.appearanceObjectFound);
    QVERIFY(qAbs(sourceAppearance.strokingOpacity - sourceOpacity) < 0.002);
    QVERIFY(qAbs(sourceAppearance.nonStrokingOpacity - sourceOpacity) < 0.002);
    const QByteArray sourceDictionary = annotationDictionaryBytes(
        sourceOutputPath,
        annotationId);
    const PdfDictionaryRange sourceDictionaryRange
        = pdfDictionaryRangeAt(sourceDictionary, 0);
    const PdfDictionaryRange sourceBorderRange = pdfDirectDictionaryValue(
        sourceDictionary,
        sourceDictionaryRange,
        QByteArrayLiteral("BS"));
    QVERIFY(sourceBorderRange.isValid());
    const QByteArray sourceBorderDictionary = sourceDictionary.mid(
        sourceBorderRange.start,
        sourceBorderRange.end - sourceBorderRange.start);
    const QRegularExpression solidBorderStyle(
        QStringLiteral(R"(/S\s*/S\b)"));
    const QRegularExpression borderWidth(
        QStringLiteral(R"(/W\s+1\.5\b)"));
    QVERIFY(solidBorderStyle.match(
        QString::fromLatin1(addedBorderDictionary)).hasMatch());
    QVERIFY(solidBorderStyle.match(
        QString::fromLatin1(sourceBorderDictionary)).hasMatch());
    QVERIFY(borderWidth.match(
        QString::fromLatin1(addedBorderDictionary)).hasMatch());
    QVERIFY(borderWidth.match(
        QString::fromLatin1(sourceBorderDictionary)).hasMatch());
    QCOMPARE(
        dictionaryColor(sourceDictionary, QStringLiteral("C")).rgb(),
        sourceStroke.rgb());
    QVERIFY(!dictionaryColor(
        sourceDictionary,
        QStringLiteral("IC")).isValid());
    QVERIFY(!sourceDictionary.contains("/IC"));
    QVERIFY(!sourceDictionary.contains(
        "/TrePDFSourceDrawingGeometryId"));
    QVERIFY(QRegularExpression(QStringLiteral(R"(/CA\s+0\.73\b)"))
        .match(QString::fromLatin1(sourceDictionary))
        .hasMatch());
}

void PDFEditControllerTest::rejectsInteriorColorForOpenDrawingComments_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QVariantList>("vertices");

    QTest::newRow("line")
        << static_cast<int>(PDFEditController::LineTool)
        << QStringLiteral("line")
        << QVariantList{
               QVariant::fromValue(QPointF(0.12, 0.24)),
               QVariant::fromValue(QPointF(0.52, 0.31))};
    QTest::newRow("polyline")
        << static_cast<int>(PDFEditController::PolyLineTool)
        << QStringLiteral("polyLine")
        << QVariantList{
               QVariant::fromValue(QPointF(0.12, 0.20)),
               QVariant::fromValue(QPointF(0.30, 0.34)),
               QVariant::fromValue(QPointF(0.54, 0.25))};
    QTest::newRow("ink")
        << static_cast<int>(PDFEditController::InkTool)
        << QStringLiteral("ink")
        << QVariantList{
               QVariant::fromValue(QPointF(0.14, 0.22)),
               QVariant::fromValue(QPointF(0.28, 0.36)),
               QVariant::fromValue(QPointF(0.46, 0.24))};
}

void PDFEditControllerTest::rejectsInteriorColorForOpenDrawingComments()
{
    QFETCH(int, tool);
    QFETCH(QString, kind);
    QFETCH(QVariantList, vertices);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("selected-%1-properties.pdf").arg(kind));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(static_cast<PDFEditController::Tool>(tool));

    const QVariantMap added = controller.addDrawingComment(0, vertices);
    QVERIFY(!added.isEmpty());
    QCOMPARE(added.value(QStringLiteral("kind")).toString(), kind);
    QVERIFY(controller.selectedDrawingPropertiesEditable());
    QVERIFY(!controller.selectedDrawingSupportsFill());
    QVERIFY(!controller.selectedDrawingHasInterior());
    QVERIFY(!controller.selectedDrawingInteriorColor().isValid());
    QVERIFY(!controller.selectedDraftPreview()
        .value(QStringLiteral("styleChanged"))
        .toBool());

    QSignalSpy historyStateChangedSpy(
        &controller,
        &PDFEditController::historyStateChanged);
    QSignalSpy pageDataChangedSpy(
        &controller,
        &PDFEditController::pageDataChanged);
    controller.setSelectedDrawingInteriorColor(QColor(220, 60, 90));
    controller.clearSelectedDrawingInteriorColor();

    QVERIFY(!controller.selectedDrawingHasInterior());
    QVERIFY(!controller.selectedDrawingInteriorColor().isValid());
    QVERIFY(!controller.selectedDraftPreview()
        .value(QStringLiteral("styleChanged"))
        .toBool());
    QCOMPARE(historyStateChangedSpy.count(), 0);
    QCOMPARE(pageDataChangedSpy.count(), 0);

    // A rejected fill edit must not insert an undo command. The next undo is
    // therefore still the original add operation.
    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.selectedDrawingPropertiesEditable());
}

void PDFEditControllerTest::
    editsRepliesAndDeletesSourceDrawingCommentsRoundTrip_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("sourceContents");
    QTest::addColumn<QString>("sourceName");
    QTest::addColumn<int>("subtype");

    QTest::newRow("square")
        << QStringLiteral("square")
        << QStringLiteral("unsupported-square")
        << QStringLiteral("unsupported-1")
        << static_cast<int>(FPDF_ANNOT_SQUARE);
    QTest::newRow("circle")
        << QStringLiteral("circle")
        << QStringLiteral("unsupported-circle")
        << QStringLiteral("unsupported-2")
        << static_cast<int>(FPDF_ANNOT_CIRCLE);
    QTest::newRow("line")
        << QStringLiteral("line")
        << QStringLiteral("unsupported-line")
        << QStringLiteral("unsupported-0")
        << static_cast<int>(FPDF_ANNOT_LINE);
    QTest::newRow("polygon")
        << QStringLiteral("polygon")
        << QStringLiteral("unsupported-polygon")
        << QStringLiteral("unsupported-3")
        << static_cast<int>(FPDF_ANNOT_POLYGON);
    QTest::newRow("polyline")
        << QStringLiteral("polyLine")
        << QStringLiteral("unsupported-polyline")
        << QStringLiteral("unsupported-4")
        << static_cast<int>(FPDF_ANNOT_POLYLINE);
    QTest::newRow("ink")
        << QStringLiteral("ink")
        << QStringLiteral("unsupported-ink")
        << QStringLiteral("unsupported-7")
        << static_cast<int>(FPDF_ANNOT_INK);
}

void PDFEditControllerTest::
    editsRepliesAndDeletesSourceDrawingCommentsRoundTrip()
{
    QFETCH(QString, kind);
    QFETCH(QString, sourceContents);
    QFETCH(QString, sourceName);
    QFETCH(int, subtype);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-drawing-%1.pdf").arg(kind));
    const QString editedPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-drawing-%1-edited.pdf").arg(kind));
    const QString deletedPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-drawing-%1-deleted.pdf").arg(kind));
    QVERIFY(createUnsupportedMarkupCommentsPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 12, 5000);
    QVERIFY(!waitForCandidates(controller, 0).isEmpty());

    QVariantMap sourceComment;
    for (const QVariant &value : controller.commentItems()) {
        const QVariantMap comment = value.toMap();
        if (comment.value(QStringLiteral("text")).toString()
                == sourceContents) {
            sourceComment = comment;
            break;
        }
    }
    QVERIFY(!sourceComment.isEmpty());
    QCOMPARE(sourceComment.value(QStringLiteral("kind")).toString(), kind);
    QVERIFY(sourceComment
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    QCOMPARE(
        sourceComment
            .value(QStringLiteral("drawingStrokeColor"))
            .value<QColor>(),
        QColor(26, 51, 77));
    QVERIFY(qAbs(
        sourceComment.value(QStringLiteral("drawingOpacity")).toReal()
            - 0.75) < 0.01);
    QVERIFY(qAbs(
        sourceComment.value(QStringLiteral("drawingBorderWidth")).toReal()
            - 2.0) < 0.01);
    QVERIFY(sourceComment.value(QStringLiteral("editable")).toBool());

    const CommentAnnotationInspection originalInspection
        = inspectCommentAnnotation(sourcePath, sourceName);
    QVERIFY(originalInspection.readable);
    QCOMPARE(originalInspection.subtype, subtype);
    QVERIFY(originalInspection.hasAppearance);
    QVERIFY(!originalInspection.appearance.isEmpty());

    const QString objectId = sourceComment
        .value(QStringLiteral("objectId"))
        .toString();
    QRectF editedFrame;
    QVariantList editedVertices;
    if (kind != QLatin1String("ink")) {
        const QRectF originalFrame = sourceComment
            .value(QStringLiteral("frameRect"))
            .toRectF();
        const QVariantList originalVertices = sourceComment
            .value(QStringLiteral("annotationPoints"))
            .toList();
        QVERIFY(originalFrame.isValid());
        QVERIFY(!originalVertices.isEmpty());
        editedFrame = QRectF(
            originalFrame.x() + 0.15,
            originalFrame.y() + 0.12,
            originalFrame.width() * 0.65,
            originalFrame.height() * 1.5);
        QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(editedFrame));
        editedVertices = transformedDrawingVertices(
            originalVertices,
            originalFrame,
            editedFrame);

        QVERIFY(controller.selectComment(0, objectId));
        QVERIFY(controller.selectedEditable());
        controller.beginSelectedFrameResize();
        QCOMPARE(
            controller.deletedSourceCommentAnnotationsForPage(0).size(),
            1);
        controller.updateSelectedFrameRect(editedFrame);
        controller.finishSelectedFrameResize();
        QCOMPARE(controller.selectedFrameRect(), editedFrame);
        QCOMPARE(controller.selectedContentBounds(), editedFrame);
        QCOMPARE(
            controller.draftsForPage(0)
                .constFirst()
                .toMap()
                .value(QStringLiteral("annotationPoints"))
                .toList(),
            editedVertices);

        QCOMPARE(controller.undo(), -1);
        QVERIFY(!controller.hasDrafts());
        QCOMPARE(controller.selectedFrameRect(), originalFrame);
        QCOMPARE(controller.redo(), -1);
        QVERIFY(controller.hasDrafts());
        QCOMPARE(controller.selectedFrameRect(), editedFrame);

        controller.discardDrafts();
        QVERIFY(!controller.hasDrafts());
        QVERIFY(controller.selectComment(0, objectId));
        QCOMPARE(controller.selectedFrameRect(), originalFrame);
        controller.beginSelectedFrameResize();
        controller.updateSelectedFrameRect(editedFrame);
        controller.finishSelectedFrameResize();
        QCOMPARE(controller.selectedFrameRect(), editedFrame);
    }
    const QString editedContents = QStringLiteral(
        "Drawing comment đã được chỉnh sửa.");
    const QString replyContents = QStringLiteral(
        "Phản hồi cho drawing comment.");
    QVERIFY(controller.setCommentText(0, objectId, editedContents));
    const QVariantMap reply = controller.addCommentReply(
        0,
        objectId,
        replyContents);
    QVERIFY(!reply.isEmpty());
    const QString replyId = reply
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!replyId.isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(editedPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection sourceStillOriginal
        = inspectCommentAnnotation(sourcePath, sourceName);
    QCOMPARE(sourceStillOriginal.contents, sourceContents);
    QCOMPARE(sourceStillOriginal.appearance, originalInspection.appearance);
    const CommentAnnotationInspection editedInspection
        = inspectCommentAnnotation(editedPath, sourceName);
    QVERIFY(editedInspection.readable);
    QCOMPARE(editedInspection.subtype, subtype);
    QCOMPARE(editedInspection.contents, editedContents);
    QCOMPARE(editedInspection.appearance, originalInspection.appearance);
    if (kind != QLatin1String("ink")) {
        QVERIFY(qAbs(
            editedInspection.rect.left - editedFrame.left() * 300.0)
            < 0.02);
        QVERIFY(qAbs(
            editedInspection.rect.right - editedFrame.right() * 300.0)
            < 0.02);
        QVERIFY(qAbs(
            editedInspection.rect.top
                - (1.0 - editedFrame.top()) * 600.0)
            < 0.02);
        QVERIFY(qAbs(
            editedInspection.rect.bottom
                - (1.0 - editedFrame.bottom()) * 600.0)
            < 0.02);
        const QByteArray editedDictionary = annotationDictionaryBytes(
            editedPath,
            sourceName);
        QVERIFY(!editedDictionary.contains(
            "/TrePDFSourceDrawingGeometryId"));
    }
    const CommentAnnotationInspection replyInspection
        = inspectCommentAnnotation(editedPath, replyId);
    QVERIFY(replyInspection.readable);
    QCOMPARE(replyInspection.subtype, FPDF_ANNOT_TEXT);
    QCOMPARE(replyInspection.contents, replyContents);
    QCOMPARE(replyInspection.parentName, sourceName);
    QCOMPARE(replyInspection.relation, QStringLiteral("R"));

    controller.requestCommentIndex();
    QTRY_VERIFY_WITH_TIMEOUT(controller.commentItems().size() >= 13, 5000);
    QVariantMap rehydratedComment;
    for (const QVariant &value : controller.commentItems()) {
        const QVariantMap comment = value.toMap();
        if (!comment.value(QStringLiteral("isReply")).toBool()
            && comment.value(QStringLiteral("kind")).toString() == kind
            && comment.value(QStringLiteral("text")).toString()
                == editedContents) {
            rehydratedComment = comment;
            break;
        }
    }
    QVERIFY(!rehydratedComment.isEmpty());
    QVERIFY(rehydratedComment.value(QStringLiteral("editable")).toBool());
    if (kind != QLatin1String("ink")) {
        const QRectF rehydratedFrame = rehydratedComment
            .value(QStringLiteral("frameRect"))
            .toRectF();
        QVERIFY(qAbs(rehydratedFrame.x() - editedFrame.x()) < 0.001);
        QVERIFY(qAbs(rehydratedFrame.y() - editedFrame.y()) < 0.001);
        QVERIFY(qAbs(
            rehydratedFrame.width() - editedFrame.width()) < 0.001);
        QVERIFY(qAbs(
            rehydratedFrame.height() - editedFrame.height()) < 0.001);
        const QVariantList rehydratedVertices = rehydratedComment
            .value(QStringLiteral("annotationPoints"))
            .toList();
        QCOMPARE(rehydratedVertices.size(), editedVertices.size());
        for (qsizetype index = 0;
             index < editedVertices.size();
             ++index) {
            const QPointF actual = rehydratedVertices.at(index).toPointF();
            const QPointF expected = editedVertices.at(index).toPointF();
            QVERIFY(qAbs(actual.x() - expected.x()) < 0.002);
            QVERIFY(qAbs(actual.y() - expected.y()) < 0.002);
        }
    }
    QVERIFY(controller.removeComment(
        0,
        rehydratedComment.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(deletedPath)));
    QVERIFY(!controller.hasDrafts());

    QCOMPARE(
        annotationSubtypeCount(
            sourcePath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype)),
        1);
    QCOMPARE(
        annotationSubtypeCount(
            deletedPath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype)),
        0);
    QCOMPARE(
        inspectCommentAnnotation(deletedPath, sourceName).annotationIndex,
        -1);
    QCOMPARE(
        inspectCommentAnnotation(deletedPath, replyId).annotationIndex,
        -1);
}

void PDFEditControllerTest::indexesDrawingAndUnsupportedMarkupComments()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("unsupported-markup-comments.pdf"));
    QVERIFY(createUnsupportedMarkupCommentsPdf(sourcePath));
    const InkAnnotationInspection sourceInk = inspectInkAnnotation(
        sourcePath,
        QStringLiteral("unsupported-7"));
    QVERIFY(sourceInk.readable);
    QVERIFY(sourceInk.annotationFound);
    QCOMPARE(sourceInk.paths.size(), 2);
    QCOMPARE(sourceInk.paths.constFirst().size(), 5);
    QCOMPARE(sourceInk.paths.constLast().size(), 4);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 12, 5000);

    const QSet<QString> expectedContents{
        QStringLiteral("unsupported-line"),
        QStringLiteral("unsupported-square"),
        QStringLiteral("unsupported-circle"),
        QStringLiteral("unsupported-polygon"),
        QStringLiteral("unsupported-polyline"),
        QStringLiteral("unsupported-stamp"),
        QStringLiteral("unsupported-caret"),
        QStringLiteral("unsupported-ink"),
        QStringLiteral("unsupported-file-attachment"),
        QStringLiteral("unsupported-sound"),
        QStringLiteral("unsupported-redact"),
        QStringLiteral("unsupported-projection"),
    };
    QSet<QString> indexedContents;
    const QHash<QString, QString> drawingKinds{
        {QStringLiteral("unsupported-line"), QStringLiteral("line")},
        {QStringLiteral("unsupported-square"), QStringLiteral("square")},
        {QStringLiteral("unsupported-circle"), QStringLiteral("circle")},
        {QStringLiteral("unsupported-polygon"), QStringLiteral("polygon")},
        {QStringLiteral("unsupported-polyline"), QStringLiteral("polyLine")},
        {QStringLiteral("unsupported-ink"), QStringLiteral("ink")},
    };
    const QVariantList comments = controller.commentItems();
    for (const QVariant &commentValue : comments) {
        const QVariantMap comment = commentValue.toMap();
        const QString text = comment.value(QStringLiteral("text")).toString();
        const QString objectId = comment
            .value(QStringLiteral("objectId"))
            .toString();
        indexedContents.insert(text);
        const bool drawingComment = drawingKinds.contains(text);
        const bool stampComment
            = text == QLatin1String("unsupported-stamp");
        const bool supportedComment = drawingComment || stampComment;
        const bool pageEditable = drawingComment || stampComment;
        QCOMPARE(
            comment.value(QStringLiteral("kind")).toString(),
            drawingComment
                ? drawingKinds.value(text)
                : stampComment
                    ? QStringLiteral("stamp")
                    : QStringLiteral("comment"));
        QVERIFY(!comment.value(QStringLiteral("label")).toString().isEmpty());
        QCOMPARE(
            comment.value(QStringLiteral("commentAuthor")).toString(),
            QStringLiteral("Markup author"));
        QCOMPARE(
            comment.value(QStringLiteral("editable")).toBool(),
            pageEditable);
        QCOMPARE(
            comment.value(QStringLiteral("commentTextEditable")).toBool(),
            supportedComment);
        QCOMPARE(
            comment.value(QStringLiteral("commentTextEditReason"))
                .toString()
                .isEmpty(),
            supportedComment);
        QVERIFY(controller.selectComment(0, objectId));
        QCOMPARE(controller.selectedEditable(), pageEditable);
        if (stampComment) {
            QCOMPARE(
                comment.value(QStringLiteral("stampName")).toString(),
                QStringLiteral("Draft"));
            QCOMPARE(
                comment.value(QStringLiteral("stampText")).toString(),
                QStringLiteral("DRAFT"));
            QVERIFY(comment
                .value(QStringLiteral("stampColor"))
                .value<QColor>()
                .isValid());
        }
        if (supportedComment) {
            if (text == QLatin1String("unsupported-ink")) {
                const QVariantList strokes = comment
                    .value(QStringLiteral("inkStrokes"))
                    .toList();
                QCOMPARE(strokes.size(), 2);
                QCOMPARE(strokes.constFirst().toList().size(), 3);
                QCOMPARE(strokes.constLast().toList().size(), 2);
                QCOMPARE(
                    comment
                        .value(QStringLiteral("annotationStrokes"))
                        .toList(),
                    strokes);
            }
            QVERIFY(controller.setCommentText(
                0,
                objectId,
                QStringLiteral("Editable markup comment")));
            QVERIFY(controller.hasDrafts());
            controller.undo();
            QVERIFY(!controller.hasDrafts());

            QVERIFY(!controller.addCommentReply(
                0,
                objectId,
                QStringLiteral("Markup reply")).isEmpty());
            QVERIFY(controller.hasDrafts());
            controller.undo();
            QVERIFY(!controller.hasDrafts());

            QVERIFY(controller.removeComment(0, objectId));
            QVERIFY(controller.hasDrafts());
            controller.undo();
            QVERIFY(!controller.hasDrafts());
        } else {
            QVERIFY(!controller.setCommentText(
                0,
                objectId,
                QStringLiteral("Mutation must be rejected")));
            QVERIFY(!controller.removeComment(0, objectId));
            QVERIFY(controller.addCommentReply(
                0,
                objectId,
                QStringLiteral("Reply must be rejected")).isEmpty());
        }
    }
    QCOMPARE(indexedContents, expectedContents);
    QCOMPARE(controller.commentItems().size(), 12);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}
