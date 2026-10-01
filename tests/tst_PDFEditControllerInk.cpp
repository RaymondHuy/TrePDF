#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::coalescesInkPreviewUpdates()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("ink-preview-coalescing.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::InkTool);

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
        {QStringLiteral("draftTextureCurrent"), true},
        {QStringLiteral("draftTextureReady"), true},
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

    const auto invokePointFunction = [&overlay](
                                         const char *method,
                                         qreal x,
                                         qreal y) {
        const QVariant xValue = x;
        const QVariant yValue = y;
        return QMetaObject::invokeMethod(
            overlay.get(),
            method,
            Qt::DirectConnection,
            Q_ARG(QVariant, xValue),
            Q_ARG(QVariant, yValue));
    };
    const auto strokePoints = [&overlay](const char *property) {
        return overlay->property(property).toList();
    };

    QVERIFY(invokePointFunction("beginInkDrawing", 10.0, 10.0));
    QVERIFY(overlay->property("inkDrawing").toBool());
    QCOMPARE(controller.pendingDrawingCommentPage(), 0);
    QCOMPARE(strokePoints("inkStrokePoints").size(), 1);
    QCOMPARE(strokePoints("inkPreviewStrokePoints").size(), 1);

    QSignalSpy captureChangedSpy(
        overlay.get(),
        SIGNAL(inkStrokePointsChanged()));
    QSignalSpy previewChangedSpy(
        overlay.get(),
        SIGNAL(inkPreviewStrokePointsChanged()));
    QVERIFY(captureChangedSpy.isValid());
    QVERIFY(previewChangedSpy.isValid());

    QVERIFY(invokePointFunction("updateInkDrawing", 20.0, 20.0));
    QVERIFY(invokePointFunction("updateInkDrawing", 30.0, 30.0));
    QCOMPARE(captureChangedSpy.count(), 0);
    QCOMPARE(previewChangedSpy.count(), 0);
    QCOMPARE(strokePoints("inkStrokePoints").size(), 3);
    QCOMPARE(strokePoints("inkPreviewStrokePoints").size(), 1);
    QVERIFY(overlay->property("inkPreviewUpdatePending").toBool());

    QVERIFY(QMetaObject::invokeMethod(
        overlay.get(),
        "publishInkPreview",
        Qt::DirectConnection));
    QCOMPARE(previewChangedSpy.count(), 1);
    QCOMPARE(
        strokePoints("inkPreviewStrokePoints"),
        strokePoints("inkStrokePoints"));
    QVERIFY(!overlay->property("inkPreviewUpdatePending").toBool());

    QVERIFY(invokePointFunction("updateInkDrawing", 40.0, 40.0));
    QCOMPARE(captureChangedSpy.count(), 0);
    QCOMPARE(strokePoints("inkStrokePoints").size(), 4);
    QCOMPARE(strokePoints("inkPreviewStrokePoints").size(), 3);
    QVERIFY(overlay->property("inkPreviewUpdatePending").toBool());

    QVERIFY(invokePointFunction("finishInkDrawing", 50.0, 50.0));
    QVERIFY(!overlay->property("inkDrawing").toBool());
    QVERIFY(strokePoints("inkStrokePoints").isEmpty());
    QVERIFY(strokePoints("inkPreviewStrokePoints").isEmpty());
    QVERIFY(!overlay->property("inkPreviewUpdatePending").toBool());
    QCOMPARE(controller.pendingDrawingCommentPage(), -1);
    const QVariantList drafts = controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantList savedStrokes = drafts.constFirst()
                                          .toMap()
                                          .value(QStringLiteral("inkStrokes"))
                                          .toList();
    QCOMPARE(savedStrokes.size(), 1);
    const QVariantList savedStroke = savedStrokes.constFirst().toList();
    QVERIFY(savedStroke.size() >= 2);
    const QPointF savedEndpoint = savedStroke.constLast().toPointF();
    QVERIFY(qAbs(savedEndpoint.x() - 0.25) < 0.000001);
    QVERIFY(qAbs(savedEndpoint.y() - 0.25) < 0.000001);

    controller.discardDrafts();
    controller.setTool(PDFEditController::InkTool);
    captureChangedSpy.clear();
    previewChangedSpy.clear();
    QVERIFY(invokePointFunction("beginInkDrawing", 0.0, 0.0));
    captureChangedSpy.clear();
    previewChangedSpy.clear();
    for (int index = 1; index < 4096; ++index) {
        const int row = index / 100;
        const int column = index % 100;
        const qreal x = (row % 2 == 0 ? column : 99 - column) * 2.0;
        const qreal y = row * 2.0;
        QVERIFY(invokePointFunction("updateInkDrawing", x, y));
    }
    QCOMPARE(strokePoints("inkStrokePoints").size(), 4096);
    QCOMPARE(strokePoints("inkPreviewStrokePoints").size(), 1);
    QCOMPARE(captureChangedSpy.count(), 0);
    QCOMPARE(previewChangedSpy.count(), 0);
    QVERIFY(overlay->property("inkPreviewUpdatePending").toBool());

    QVERIFY(invokePointFunction("updateInkDrawing", 192.0, 80.0));
    const QVariantList cappedPoints = strokePoints("inkStrokePoints");
    QCOMPARE(cappedPoints.size(), 4096);
    const QPointF cappedEndpoint = cappedPoints.constLast().toPointF();
    QVERIFY(qAbs(cappedEndpoint.x() - 0.96) < 0.000001);
    QVERIFY(qAbs(cappedEndpoint.y() - 0.40) < 0.000001);
    QCOMPARE(captureChangedSpy.count(), 0);

    QVERIFY(overlay->setProperty("pooled", true));
    QVERIFY(!overlay->property("inkDrawing").toBool());
    QVERIFY(strokePoints("inkStrokePoints").isEmpty());
    QVERIFY(strokePoints("inkPreviewStrokePoints").isEmpty());
    QVERIFY(!overlay->property("inkPreviewUpdatePending").toBool());
    QCOMPARE(controller.pendingDrawingCommentPage(), -1);
}

void PDFEditControllerTest::movesAndResizesAddedInkWithUndoRedoAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-added-ink-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::InkTool);
    const QVariantList inputPoints{
        QVariant::fromValue(QPointF(0.12, 0.18)),
        QVariant::fromValue(QPointF(0.20, 0.28)),
        QVariant::fromValue(QPointF(0.31, 0.20)),
        QVariant::fromValue(QPointF(0.43, 0.32)),
    };

    const QVariantMap added = controller.addDrawingComment(0, inputPoints);
    QVERIFY(!added.isEmpty());
    QCOMPARE(
        added.value(QStringLiteral("kind")).toString(),
        QStringLiteral("ink"));
    QVERIFY(!added.value(QStringLiteral("geometryChanged")).toBool());
    const QString objectId = added
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!objectId.isEmpty());
    const QRectF originalFrame = added
        .value(QStringLiteral("frameRect"))
        .toRectF();
    QCOMPARE(
        added.value(QStringLiteral("sourceRect")).toRectF(),
        originalFrame);
    QCOMPARE(
        added.value(QStringLiteral("contentBounds")).toRectF(),
        originalFrame);
    const QVariantList originalStrokes = added
        .value(QStringLiteral("inkStrokes"))
        .toList();
    QCOMPARE(originalStrokes.size(), 1);
    QCOMPARE(
        added.value(QStringLiteral("annotationStrokes")).toList(),
        originalStrokes);

    const QPointF translation(0.30, 0.25);
    const QRectF movedFrame = originalFrame.translated(translation);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(movedFrame));
    QVERIFY(!originalFrame.intersects(movedFrame));
    const QVariantList movedStrokes = transformedInkStrokes(
        originalStrokes,
        originalFrame,
        movedFrame);

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    QVERIFY(controller.textEditActive());
    controller.updateSelectedFrameRect(movedFrame);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.selectedContentBounds(), movedFrame);
    controller.finishSelectedFrameResize();
    QVERIFY(!controller.textEditActive());

    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    const QVariantList movedDrafts = controller.draftsForPage(0);
    QCOMPARE(movedDrafts.size(), 1);
    const QVariantMap movedDraft = movedDrafts.constFirst().toMap();
    QCOMPARE(
        movedDraft.value(QStringLiteral("sourceRect")).toRectF(),
        originalFrame);
    QCOMPARE(
        movedDraft.value(QStringLiteral("frameRect")).toRectF(),
        movedFrame);
    QCOMPARE(
        movedDraft.value(QStringLiteral("contentBounds")).toRectF(),
        movedFrame);
    QVERIFY(movedDraft.value(QStringLiteral("geometryChanged")).toBool());
    QCOMPARE(
        movedDraft.value(QStringLiteral("inkStrokes")).toList(),
        movedStrokes);
    QCOMPARE(
        movedDraft.value(QStringLiteral("annotationStrokes")).toList(),
        movedStrokes);
    QCOMPARE(
        controller.candidateAt(
            0,
            movedFrame.center().x(),
            movedFrame.center().y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(controller.candidateAt(
        0,
        originalFrame.center().x(),
        originalFrame.center().y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);

    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canRedo());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(controller.selectedContentBounds(), originalFrame);
    const QVariantMap undoneDraft = controller.draftsForPage(0)
        .constFirst()
        .toMap();
    QVERIFY(!undoneDraft.value(QStringLiteral("geometryChanged")).toBool());
    QCOMPARE(
        undoneDraft.value(QStringLiteral("inkStrokes")).toList(),
        originalStrokes);
    QCOMPARE(
        undoneDraft.value(QStringLiteral("annotationStrokes")).toList(),
        originalStrokes);

    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.selectedContentBounds(), movedFrame);
    const QVariantMap redoneDraft = controller.draftsForPage(0)
        .constFirst()
        .toMap();
    QVERIFY(redoneDraft.value(QStringLiteral("geometryChanged")).toBool());
    QCOMPARE(
        redoneDraft.value(QStringLiteral("inkStrokes")).toList(),
        movedStrokes);

    const QRectF resizedFrame(
        movedFrame.x() - 0.04,
        movedFrame.y() + 0.02,
        movedFrame.width() * 0.65,
        movedFrame.height() * 1.40);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(resizedFrame));
    const QVariantList resizedStrokes = transformedInkStrokes(
        originalStrokes,
        originalFrame,
        resizedFrame);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
    const QVariantMap resizedDraft = controller.draftsForPage(0)
        .constFirst()
        .toMap();
    QCOMPARE(
        resizedDraft.value(QStringLiteral("inkStrokes")).toList(),
        resizedStrokes);
    QCOMPARE(
        resizedDraft.value(QStringLiteral("annotationStrokes")).toList(),
        resizedStrokes);

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("inkStrokes"))
            .toList(),
        movedStrokes);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("inkStrokes"))
            .toList(),
        resizedStrokes);

    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QCOMPARE(controller.selectedObjectId(), QString());
    const QVariantList discardedCandidates = waitForCandidates(controller, 0);
    QVERIFY(std::none_of(
        discardedCandidates.cbegin(),
        discardedCandidates.cend(),
        [&objectId](const QVariant &candidateValue) {
            return candidateValue.toMap()
                    .value(QStringLiteral("objectId"))
                    .toString()
                == objectId;
        }));
}

void PDFEditControllerTest::savesInkCommentRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("ink-comment-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("ink-comment-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(!pageSize.isEmpty());
    const QColor drawingColor(32, 105, 196, 153);
    controller.setDrawingCommentColor(drawingColor);
    controller.setTool(PDFEditController::InkTool);
    const QVariantList inputPoints{
        QVariant::fromValue(QPointF(0.12, 0.18)),
        QVariant::fromValue(QPointF(0.18, 0.27)),
        QVariant::fromValue(QPointF(0.27, 0.20)),
        QVariant::fromValue(QPointF(0.36, 0.34)),
        QVariant::fromValue(QPointF(0.48, 0.24)),
    };

    const QVariantMap added = controller.addDrawingComment(0, inputPoints);
    QVERIFY(!added.isEmpty());
    QCOMPARE(
        added.value(QStringLiteral("kind")).toString(),
        QStringLiteral("ink"));
    const QVariantList addedStrokes = added
        .value(QStringLiteral("inkStrokes"))
        .toList();
    QCOMPARE(addedStrokes.size(), 1);
    QCOMPARE(
        added.value(QStringLiteral("annotationStrokes")).toList(),
        addedStrokes);
    const QVariantList addedPath = addedStrokes.constFirst().toList();
    QVERIFY(addedPath.size() >= 2);
    QVERIFY(addedPath.size() <= 1024);
    QCOMPARE(
        addedPath.constFirst().toPointF(),
        inputPoints.constFirst().toPointF());
    QCOMPARE(
        addedPath.constLast().toPointF(),
        inputPoints.constLast().toPointF());
    QCOMPARE(
        added.value(QStringLiteral("drawingStrokeColor")).value<QColor>(),
        QColor(32, 105, 196));
    QCOMPARE(
        added.value(QStringLiteral("strokeColor")).value<QColor>(),
        QColor(32, 105, 196));
    QVERIFY(qAbs(
        added.value(QStringLiteral("drawingOpacity")).toReal()
            - drawingColor.alphaF()) < 0.002);
    QVERIFY(qAbs(
        added.value(QStringLiteral("drawingBorderWidth")).toReal()
            - 1.5) < 0.002);
    QVERIFY(qAbs(
        added.value(QStringLiteral("strokeWidthRatio")).toReal()
            - 1.5 / pageSize.height()) < 0.000001);
    QCOMPARE(controller.tool(), PDFEditController::CommentSelectTool);
    QCOMPARE(controller.selectedKind(), QStringLiteral("ink"));
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
    const QRectF originalFrame = added
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF resizedFrame(
        originalFrame.x() + 0.04,
        originalFrame.y() - 0.03,
        originalFrame.width() * 0.72,
        originalFrame.height() * 1.35);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(resizedFrame));
    QVERIFY(controller.selectCandidate(0, annotationId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    const QVariantMap resizedDraft = controller.draftsForPage(0)
        .constFirst()
        .toMap();
    QCOMPARE(
        resizedDraft.value(QStringLiteral("frameRect")).toRectF(),
        resizedFrame);
    QCOMPARE(
        resizedDraft.value(QStringLiteral("contentBounds")).toRectF(),
        resizedFrame);
    const QVariantList resizedStrokes = resizedDraft
        .value(QStringLiteral("inkStrokes"))
        .toList();
    const QVariantList resizedPath = resizedStrokes.constFirst().toList();
    QCOMPARE(
        resizedStrokes,
        transformedInkStrokes(
            addedStrokes,
            originalFrame,
            resizedFrame));
    const int sourcePageObjectCount = topLevelPageObjectCount(sourcePath);
    QVERIFY(sourcePageObjectCount > 0);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(topLevelPageObjectCount(outputPath), sourcePageObjectCount);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_INK), 0);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_INK), 1);

    const CommentAnnotationInspection commentInspection
        = inspectCommentAnnotation(outputPath, annotationId);
    QVERIFY(commentInspection.readable);
    QCOMPARE(commentInspection.subtype, FPDF_ANNOT_INK);
    QVERIFY(commentInspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(
        commentInspection.subject,
        added.value(QStringLiteral("label")).toString());
    QVERIFY(commentInspection.hasAppearance);
    QVERIFY(!commentInspection.appearance.isEmpty());
    QVERIFY(qAbs(
        commentInspection.rect.left
            - resizedFrame.left() * pageSize.width()) < 0.01);
    QVERIFY(qAbs(
        commentInspection.rect.right
            - resizedFrame.right() * pageSize.width()) < 0.01);
    QVERIFY(qAbs(
        commentInspection.rect.top
            - (1.0 - resizedFrame.top()) * pageSize.height()) < 0.01);
    QVERIFY(qAbs(
        commentInspection.rect.bottom
            - (1.0 - resizedFrame.bottom()) * pageSize.height()) < 0.01);
    QVERIFY(commentInspection.appearance.contains(QStringLiteral("/GS gs")));
    QVERIFY(commentInspection.appearance.contains(QStringLiteral("1.5 w")));
    QVERIFY(commentInspection.appearance.contains(QStringLiteral("1 J 1 j")));
    QVERIFY(commentInspection.appearance.contains(QStringLiteral("[] 0 d")));
    QVERIFY(QRegularExpression(QStringLiteral(R"((?:^|\s)m(?:\s|$))"))
        .match(commentInspection.appearance)
        .hasMatch());
    QVERIFY(QRegularExpression(QStringLiteral(R"((?:^|\s)l(?:\s|$))"))
        .match(commentInspection.appearance)
        .hasMatch());

    const InkAnnotationInspection inkInspection = inspectInkAnnotation(
        outputPath,
        annotationId);
    QVERIFY(inkInspection.readable);
    QCOMPARE(inkInspection.count, 1);
    QVERIFY(inkInspection.annotationFound);
    QCOMPARE(inkInspection.paths.size(), 1);
    QCOMPARE(inkInspection.paths.constFirst().size(), resizedPath.size());
    for (qsizetype pointIndex = 0;
         pointIndex < resizedPath.size();
         ++pointIndex) {
        const QPointF actual = inkInspection.paths.constFirst().at(
            pointIndex);
        const QPointF expected = resizedPath.at(pointIndex).toPointF();
        QVERIFY(qAbs(actual.x() / pageSize.width() - expected.x()) < 0.002);
        QVERIFY(qAbs(
            (1.0 - actual.y() / pageSize.height()) - expected.y()) < 0.002);
    }
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
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/InkList\s*\[)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/C\s*\[)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/CA\s+)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/BS\s*<<)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/W\s+1\.5\b)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/S\s*/S\b)")));
    QVERIFY(hasDictionaryPattern(
        QStringLiteral(R"(/AP\s*<<\s*/N\s+\d+\s+0\s+R\b)")));
    QVERIFY(!annotationDictionary.contains("/Vertices"));
    QVERIFY(!hasDictionaryPattern(QStringLiteral(R"(/L\b)")));

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
    QVERIFY(qAbs(
        appearance.boundingBox.at(0) - commentInspection.rect.left) < 0.01);
    QVERIFY(qAbs(
        appearance.boundingBox.at(1) - commentInspection.rect.bottom) < 0.01);
    QVERIFY(qAbs(
        appearance.boundingBox.at(2) - commentInspection.rect.right) < 0.01);
    QVERIFY(qAbs(
        appearance.boundingBox.at(3) - commentInspection.rect.top) < 0.01);

    controller.requestCommentIndex();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.commentItems().isEmpty(), 5000);
    bool rehydrated = false;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("kind")).toString()
            != QLatin1String("ink")) {
            continue;
        }
        QVERIFY(comment
            .value(QStringLiteral("commentTextEditable"))
            .toBool());
        QCOMPARE(
            comment
                .value(QStringLiteral("drawingStrokeColor"))
                .value<QColor>(),
            QColor(32, 105, 196));
        QVERIFY(qAbs(
            comment.value(QStringLiteral("drawingOpacity")).toReal()
                - drawingColor.alphaF()) < 0.01);
        QVERIFY(qAbs(
            comment.value(QStringLiteral("drawingBorderWidth")).toReal()
                - 1.5) < 0.002);
        const QRectF rehydratedFrame = comment
            .value(QStringLiteral("frameRect"))
            .toRectF();
        QVERIFY(qAbs(rehydratedFrame.x() - resizedFrame.x()) < 0.002);
        QVERIFY(qAbs(rehydratedFrame.y() - resizedFrame.y()) < 0.002);
        QVERIFY(qAbs(
            rehydratedFrame.width() - resizedFrame.width()) < 0.002);
        QVERIFY(qAbs(
            rehydratedFrame.height() - resizedFrame.height()) < 0.002);
        const QVariantList rehydratedStrokes = comment
            .value(QStringLiteral("inkStrokes"))
            .toList();
        QCOMPARE(rehydratedStrokes.size(), 1);
        QCOMPARE(
            comment.value(QStringLiteral("annotationStrokes")).toList(),
            rehydratedStrokes);
        const QVariantList rehydratedPath = rehydratedStrokes
            .constFirst()
            .toList();
        QCOMPARE(rehydratedPath.size(), resizedPath.size());
        for (qsizetype pointIndex = 0;
             pointIndex < resizedPath.size();
             ++pointIndex) {
            const QPointF actual = rehydratedPath.at(pointIndex).toPointF();
            const QPointF expected = resizedPath.at(pointIndex).toPointF();
            QVERIFY(qAbs(actual.x() - expected.x()) < 0.002);
            QVERIFY(qAbs(actual.y() - expected.y()) < 0.002);
        }
        rehydrated = true;
        break;
    }
    QVERIFY(rehydrated);
}

void PDFEditControllerTest::movesAndResizesSourceInkRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-source-ink.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("move-source-ink-output.pdf"));
    QVERIFY(createUnsupportedMarkupCommentsPdf(sourcePath));

    const QString annotationName = QStringLiteral("unsupported-7");
    const CommentAnnotationInspection originalComment
        = inspectCommentAnnotation(sourcePath, annotationName);
    const InkAnnotationInspection originalInk = inspectInkAnnotation(
        sourcePath,
        annotationName);
    QVERIFY(originalComment.readable);
    QCOMPARE(originalComment.subtype, FPDF_ANNOT_INK);
    QCOMPARE(originalComment.richContents, QStringLiteral("ink-rich-original"));
    QVERIFY(originalComment.hasAppearance);
    QVERIFY(!originalComment.appearance.isEmpty());
    QVERIFY(originalInk.readable);
    QVERIFY(originalInk.annotationFound);
    QCOMPARE(originalInk.paths.size(), 2);
    const int originalPageObjectCount = topLevelPageObjectCount(sourcePath);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 12, 5000);

    QVariantMap sourceInk;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("text")).toString()
            == QLatin1String("unsupported-ink")) {
            sourceInk = comment;
            break;
        }
    }
    QVERIFY(!sourceInk.isEmpty());
    QCOMPARE(
        sourceInk.value(QStringLiteral("kind")).toString(),
        QStringLiteral("ink"));
    QVERIFY(sourceInk.value(QStringLiteral("editable")).toBool());
    QVERIFY(sourceInk
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    const QString objectId = sourceInk
        .value(QStringLiteral("objectId"))
        .toString();
    const QVariantMap pageInk = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("ink"));
    QVERIFY(!pageInk.isEmpty());
    QCOMPARE(
        pageInk.value(QStringLiteral("objectId")).toString(),
        objectId);
    const QRectF originalFrame = sourceInk
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QVariantList originalNormalizedStrokes = sourceInk
        .value(QStringLiteral("inkStrokes"))
        .toList();
    QCOMPARE(originalNormalizedStrokes.size(), 2);
    QCOMPARE(originalNormalizedStrokes.constFirst().toList().size(), 3);
    QCOMPARE(originalNormalizedStrokes.constLast().toList().size(), 2);

    const QPointF translation(0.25, 0.20);
    const QRectF movedFrame = originalFrame.translated(translation);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(movedFrame));
    QVERIFY(controller.selectCandidate(0, objectId));
    QVERIFY(controller.selectedEditable());
    QVERIFY(!controller.selectedDrawingPropertiesEditable());
    QVERIFY(!controller.setSelectedDrawingStrokeColor(QColor(210, 45, 65)));
    QVERIFY(!controller.setSelectedDrawingOpacity(0.4));
    QVERIFY(!controller.hasDrafts());
    controller.beginSelectedFrameResize();
    QCOMPARE(
        controller.deletedSourceCommentAnnotationsForPage(0).size(),
        1);
    controller.updateSelectedFrameRect(movedFrame);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.selectedContentBounds(), movedFrame);
    controller.finishSelectedFrameResize();

    const QVariantList movedDrafts = controller.draftsForPage(0);
    QCOMPARE(movedDrafts.size(), 1);
    const QVariantMap movedDraft = movedDrafts.constFirst().toMap();
    QVERIFY(movedDraft.value(QStringLiteral("geometryChanged")).toBool());
    QCOMPARE(
        controller.deletedSourceCommentAnnotationsForPage(0).size(),
        1);
    const QVariantList movedNormalizedStrokes = movedDraft
        .value(QStringLiteral("inkStrokes"))
        .toList();
    QCOMPARE(movedNormalizedStrokes.size(), originalNormalizedStrokes.size());
    for (qsizetype strokeIndex = 0;
         strokeIndex < originalNormalizedStrokes.size();
         ++strokeIndex) {
        const QVariantList originalStroke
            = originalNormalizedStrokes.at(strokeIndex).toList();
        const QVariantList movedStroke
            = movedNormalizedStrokes.at(strokeIndex).toList();
        QCOMPARE(movedStroke.size(), originalStroke.size());
        for (qsizetype pointIndex = 0;
             pointIndex < originalStroke.size();
             ++pointIndex) {
            QCOMPARE(
                movedStroke.at(pointIndex).toPointF(),
                originalStroke.at(pointIndex).toPointF() + translation);
        }
    }

    QCOMPARE(controller.undo(), -1);
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(controller.deletedSourceCommentAnnotationsForPage(0).isEmpty());
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QVERIFY(controller.hasDrafts());

    const QRectF resizedFrame(
        movedFrame.x() + 0.03,
        movedFrame.y() - 0.04,
        movedFrame.width() * 0.70,
        movedFrame.height() * 2.50);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(resizedFrame));
    const QVariantList resizedNormalizedStrokes = transformedInkStrokes(
        originalNormalizedStrokes,
        originalFrame,
        resizedFrame);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("inkStrokes"))
            .toList(),
        resizedNormalizedStrokes);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(topLevelPageObjectCount(outputPath), originalPageObjectCount);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_INK), 1);

    const CommentAnnotationInspection movedComment
        = inspectCommentAnnotation(outputPath, annotationName);
    const InkAnnotationInspection movedInk = inspectInkAnnotation(
        outputPath,
        annotationName);
    QVERIFY(movedComment.readable);
    QCOMPARE(movedComment.subtype, FPDF_ANNOT_INK);
    QCOMPARE(movedComment.contents, originalComment.contents);
    QCOMPARE(movedComment.richContents, originalComment.richContents);
    QCOMPARE(movedComment.flags, originalComment.flags);
    QCOMPARE(movedComment.appearance, originalComment.appearance);
    QVERIFY(!movedComment.modificationDate.isEmpty());
    QVERIFY(movedComment.modificationDate
        != originalComment.modificationDate);
    QVERIFY(qAbs(
        movedComment.rect.left - resizedFrame.left() * 300.0) < 0.02);
    QVERIFY(qAbs(
        movedComment.rect.right - resizedFrame.right() * 300.0) < 0.02);
    QVERIFY(qAbs(
        movedComment.rect.top
            - (1.0 - resizedFrame.top()) * 600.0) < 0.02);
    QVERIFY(qAbs(
        movedComment.rect.bottom
            - (1.0 - resizedFrame.bottom()) * 600.0) < 0.02);
    QVERIFY(movedInk.readable);
    QVERIFY(movedInk.annotationFound);
    QCOMPARE(movedInk.paths.size(), resizedNormalizedStrokes.size());
    for (qsizetype strokeIndex = 0;
         strokeIndex < resizedNormalizedStrokes.size();
         ++strokeIndex) {
        const QVariantList normalizedStroke
            = resizedNormalizedStrokes.at(strokeIndex).toList();
        const QVector<QPointF> pageStroke = movedInk.paths.at(strokeIndex);
        QCOMPARE(pageStroke.size(), normalizedStroke.size());
        for (qsizetype pointIndex = 0;
             pointIndex < normalizedStroke.size();
             ++pointIndex) {
            const QPointF normalizedPoint
                = normalizedStroke.at(pointIndex).toPointF();
            const QPointF expectedPagePoint(
                normalizedPoint.x() * 300.0,
                (1.0 - normalizedPoint.y()) * 600.0);
            const QPointF actualPagePoint = pageStroke.at(pointIndex);
            QVERIFY(qAbs(
                actualPagePoint.x() - expectedPagePoint.x()) < 0.01);
            QVERIFY(qAbs(
                actualPagePoint.y() - expectedPagePoint.y()) < 0.01);
        }
    }
    const QByteArray movedDictionary = annotationDictionaryBytes(
        outputPath,
        annotationName);
    QVERIFY(movedDictionary.contains("/CustomInkKey"));
    QVERIFY(movedDictionary.contains("keep-me"));
    QVERIFY(movedDictionary.contains("/RC"));
    QVERIFY(movedDictionary.contains("ink-rich-original"));
    QVERIFY(!movedDictionary.contains(
        "/TrePDFSourceDrawingGeometryId"));

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 12, 5000);
    QVariantMap rehydratedInk;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("text")).toString()
            == QLatin1String("unsupported-ink")) {
            rehydratedInk = comment;
            break;
        }
    }
    QVERIFY(!rehydratedInk.isEmpty());
    QVERIFY(rehydratedInk.value(QStringLiteral("editable")).toBool());
    const QRectF rehydratedFrame = rehydratedInk
        .value(QStringLiteral("frameRect"))
        .toRectF();
    QVERIFY(qAbs(rehydratedFrame.x() - resizedFrame.x()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.y() - resizedFrame.y()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.width() - resizedFrame.width()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.height() - resizedFrame.height()) < 0.001);
}

void PDFEditControllerTest::movesSourceInkWithoutChangingAppearance()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("translate-source-ink.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("translate-source-ink-output.pdf"));
    QVERIFY(createUnsupportedMarkupCommentsPdf(sourcePath));

    const QString annotationName = QStringLiteral("unsupported-7");
    const CommentAnnotationInspection originalComment
        = inspectCommentAnnotation(sourcePath, annotationName);
    const AnnotationAppearanceStructureInspection originalAppearance
        = inspectAnnotationAppearanceStructure(sourcePath, annotationName);
    QVERIFY(originalComment.readable);
    QVERIFY(originalComment.hasAppearance);
    QVERIFY(!originalComment.appearance.isEmpty());
    QVERIFY(originalAppearance.readable);
    QVERIFY(originalAppearance.annotationFound);
    QCOMPARE(originalAppearance.boundingBox.size(), 4);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap sourceInk = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("ink"));
    QVERIFY(!sourceInk.isEmpty());
    const QString objectId = sourceInk
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF originalFrame = sourceInk
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QVariantList originalStrokes = sourceInk
        .value(QStringLiteral("inkStrokes"))
        .toList();
    const QPointF translation(0.25, 0.20);
    const QRectF movedFrame = originalFrame.translated(translation);
    const QVariantList movedStrokes = transformedInkStrokes(
        originalStrokes,
        originalFrame,
        movedFrame);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(movedFrame));

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("inkStrokes"))
            .toList(),
        movedStrokes);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentAnnotationInspection movedComment
        = inspectCommentAnnotation(outputPath, annotationName);
    const InkAnnotationInspection movedInk = inspectInkAnnotation(
        outputPath,
        annotationName);
    QVERIFY(movedComment.readable);
    QCOMPARE(movedComment.contents, originalComment.contents);
    QCOMPARE(movedComment.richContents, originalComment.richContents);
    QCOMPARE(movedComment.flags, originalComment.flags);
    QCOMPARE(movedComment.appearance, originalComment.appearance);
    QVERIFY(qAbs(
        movedComment.rect.left - movedFrame.left() * 300.0) < 0.02);
    QVERIFY(qAbs(
        movedComment.rect.right - movedFrame.right() * 300.0) < 0.02);
    QVERIFY(qAbs(
        movedComment.rect.top
            - (1.0 - movedFrame.top()) * 600.0) < 0.02);
    QVERIFY(qAbs(
        movedComment.rect.bottom
            - (1.0 - movedFrame.bottom()) * 600.0) < 0.02);
    QVERIFY(movedInk.readable);
    QCOMPARE(movedInk.paths.size(), movedStrokes.size());
    for (qsizetype strokeIndex = 0;
         strokeIndex < movedStrokes.size();
         ++strokeIndex) {
        const QVariantList normalizedStroke
            = movedStrokes.at(strokeIndex).toList();
        const QVector<QPointF> pageStroke = movedInk.paths.at(strokeIndex);
        QCOMPARE(pageStroke.size(), normalizedStroke.size());
        for (qsizetype pointIndex = 0;
             pointIndex < normalizedStroke.size();
             ++pointIndex) {
            const QPointF normalizedPoint
                = normalizedStroke.at(pointIndex).toPointF();
            const QPointF actualPoint = pageStroke.at(pointIndex);
            QVERIFY(qAbs(
                actualPoint.x() - normalizedPoint.x() * 300.0) < 0.01);
            QVERIFY(qAbs(
                actualPoint.y()
                    - (1.0 - normalizedPoint.y()) * 600.0) < 0.01);
        }
    }

    const AnnotationAppearanceStructureInspection movedAppearance
        = inspectAnnotationAppearanceStructure(outputPath, annotationName);
    QVERIFY(movedAppearance.readable);
    QVERIFY(movedAppearance.annotationFound);
    QCOMPARE(movedAppearance.normalAppearanceIsIndirect,
             originalAppearance.normalAppearanceIsIndirect);
    QCOMPARE(movedAppearance.normalAppearanceObjectNumber,
             originalAppearance.normalAppearanceObjectNumber);
    QCOMPARE(movedAppearance.normalAppearanceGeneration,
             originalAppearance.normalAppearanceGeneration);
    QCOMPARE(movedAppearance.appearanceIsStream,
             originalAppearance.appearanceIsStream);
    QCOMPARE(movedAppearance.matrixIsIdentity,
             originalAppearance.matrixIsIdentity);
    QCOMPARE(movedAppearance.boundingBox, originalAppearance.boundingBox);
    const QByteArray movedDictionary = annotationDictionaryBytes(
        outputPath,
        annotationName);
    QVERIFY(movedDictionary.contains("/CustomInkKey"));
    QVERIFY(movedDictionary.contains("keep-me"));
    QVERIFY(movedDictionary.contains("/RC"));
    QVERIFY(movedDictionary.contains("ink-rich-original"));
    QVERIFY(!movedDictionary.contains(
        "/TrePDFSourceDrawingGeometryId"));
}

void PDFEditControllerTest::
    keepsResizedSourceInkAppearanceAlignedWithInkList()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-ink-mismatched-appearance.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-ink-mismatched-appearance-output.pdf"));
    QVERIFY(createSourceInkWithMismatchedAppearancePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap sourceInk = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("ink"));
    QVERIFY(!sourceInk.isEmpty());
    QVERIFY(sourceInk.value(QStringLiteral("editable")).toBool());

    const QString objectId = sourceInk
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF originalFrame = sourceInk
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QVariantList originalStrokes = sourceInk
        .value(QStringLiteral("inkStrokes"))
        .toList();
    const QRectF resizedFrame(
        originalFrame.x() + 0.25,
        originalFrame.y() + 0.15,
        originalFrame.width() * 1.8,
        originalFrame.height() * 4.0);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(resizedFrame));
    const QVariantList resizedStrokes = transformedInkStrokes(
        originalStrokes,
        originalFrame,
        resizedFrame);

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("inkStrokes"))
            .toList(),
        resizedStrokes);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const QImage renderedOutput = renderFirstPageInViewer(
        outputPath,
        QSize(300, 600));
    QVERIFY(!renderedOutput.isNull());
    const QRect renderedBounds = redPixelBounds(renderedOutput);
    QVERIFY(renderedBounds.isValid());

    bool hasExpectedPoint = false;
    qreal minimumX = 0.0;
    qreal maximumX = 0.0;
    qreal minimumY = 0.0;
    qreal maximumY = 0.0;
    for (const QVariant &strokeValue : resizedStrokes) {
        for (const QVariant &pointValue : strokeValue.toList()) {
            const QPointF normalizedPoint = pointValue.toPointF();
            const QPointF devicePoint(
                normalizedPoint.x() * renderedOutput.width(),
                normalizedPoint.y() * renderedOutput.height());
            if (!hasExpectedPoint) {
                minimumX = maximumX = devicePoint.x();
                minimumY = maximumY = devicePoint.y();
                hasExpectedPoint = true;
            } else {
                minimumX = qMin(minimumX, devicePoint.x());
                maximumX = qMax(maximumX, devicePoint.x());
                minimumY = qMin(minimumY, devicePoint.y());
                maximumY = qMax(maximumY, devicePoint.y());
            }
        }
    }
    QVERIFY(hasExpectedPoint);
    const QRectF expectedPointBounds(
        QPointF(minimumX, minimumY),
        QPointF(maximumX, maximumY));
    const qreal maximumEdgeDelta = (std::max)({
        qAbs(renderedBounds.left() - expectedPointBounds.left()),
        qAbs(renderedBounds.right() - expectedPointBounds.right()),
        qAbs(renderedBounds.top() - expectedPointBounds.top()),
        qAbs(renderedBounds.bottom() - expectedPointBounds.bottom())});
    const QByteArray diagnostic = QStringLiteral(
        "Saved Ink /AP bounds [%1,%2 %3x%4] do not match resized "
        "/InkList bounds [%5,%6 %7x%8]")
        .arg(
            renderedBounds.x())
        .arg(renderedBounds.y())
        .arg(renderedBounds.width())
        .arg(renderedBounds.height())
        .arg(expectedPointBounds.x(), 0, 'f', 1)
        .arg(expectedPointBounds.y(), 0, 'f', 1)
        .arg(expectedPointBounds.width(), 0, 'f', 1)
        .arg(expectedPointBounds.height(), 0, 'f', 1)
        .toUtf8();
    QVERIFY2(maximumEdgeDelta <= 3.0, diagnostic.constData());
    const QByteArray savedDictionary = annotationDictionaryBytes(
        outputPath,
        QStringLiteral("ink:11111111-2222-4333-8444-555555555555"));
    QVERIFY(!savedDictionary.isEmpty());
    QVERIFY(!savedDictionary.contains(
        "/TrePDFSourceDrawingGeometryId"));
}

void PDFEditControllerTest::editsSourceInkWithFillAppearanceRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-ink-fill.pdf"));
    const QString editedPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-ink-fill-edited.pdf"));
    QVERIFY(createSourceInkWithFillAppearancePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap sourceComment = controller.commentItems()
        .constFirst()
        .toMap();
    QCOMPARE(
        sourceComment.value(QStringLiteral("kind")).toString(),
        QStringLiteral("ink"));
    QVERIFY(sourceComment
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    QVERIFY(sourceComment
        .value(QStringLiteral("drawingHasInterior"))
        .toBool());

    const QString objectId = sourceComment
        .value(QStringLiteral("objectId"))
        .toString();
    const CommentAnnotationInspection originalInspection
        = inspectCommentAnnotation(
            sourcePath,
            QStringLiteral("source-ink-fill"));
    const AnnotationAppearanceStructureInspection originalAppearance
        = inspectAnnotationAppearanceStructure(
            sourcePath,
            QStringLiteral("source-ink-fill"));
    QVERIFY(originalInspection.readable);
    QVERIFY(!originalInspection.appearance.isEmpty());
    QVERIFY(originalAppearance.readable);
    QVERIFY(originalAppearance.annotationFound);
    QVERIFY(originalAppearance.normalAppearanceIsIndirect);
    QVERIFY(originalAppearance.appearanceObjectFound);
    QVERIFY(originalAppearance.appearanceIsStream);
    QVERIFY(originalAppearance.matrixIsIdentity);
    QCOMPARE(originalAppearance.boundingBox.size(), 4);
    const QVariantMap pageInk = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("ink"));
    QVERIFY(!pageInk.isEmpty());
    QCOMPARE(
        pageInk.value(QStringLiteral("objectId")).toString(),
        objectId);
    const QRectF originalFrame = pageInk
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF resizedFrame(
        originalFrame.x() - 0.03,
        originalFrame.y() - 0.02,
        originalFrame.width() + 0.06,
        originalFrame.height() + 0.04);
    QVERIFY(QRectF(0.0, 0.0, 1.0, 1.0).contains(resizedFrame));
    QVERIFY(resizedFrame.contains(originalFrame));
    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    const QString editedContents = QStringLiteral(
        "Ink with fill appearance edited.");
    QVERIFY(controller.setCommentText(0, objectId, editedContents));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(editedPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection editedInspection
        = inspectCommentAnnotation(editedPath, QStringLiteral("source-ink-fill"));
    QVERIFY(editedInspection.readable);
    QCOMPARE(editedInspection.subtype, static_cast<int>(FPDF_ANNOT_INK));
    QCOMPARE(editedInspection.contents, editedContents);
    QVERIFY(editedInspection.hasAppearance);
    QCOMPARE(editedInspection.appearance, originalInspection.appearance);
    const AnnotationAppearanceStructureInspection editedAppearance
        = inspectAnnotationAppearanceStructure(
            editedPath,
            QStringLiteral("source-ink-fill"));
    QVERIFY(editedAppearance.readable);
    QVERIFY(editedAppearance.annotationFound);
    QCOMPARE(editedAppearance.normalAppearanceIsIndirect,
             originalAppearance.normalAppearanceIsIndirect);
    QCOMPARE(editedAppearance.normalAppearanceObjectNumber,
             originalAppearance.normalAppearanceObjectNumber);
    QCOMPARE(editedAppearance.normalAppearanceGeneration,
             originalAppearance.normalAppearanceGeneration);
    QCOMPARE(editedAppearance.appearanceObjectFound,
             originalAppearance.appearanceObjectFound);
    QCOMPARE(editedAppearance.appearanceIsStream,
             originalAppearance.appearanceIsStream);
    QCOMPARE(editedAppearance.matrixIsIdentity,
             originalAppearance.matrixIsIdentity);
    QCOMPARE(editedAppearance.boundingBox, originalAppearance.boundingBox);
    QVERIFY(qAbs(
        editedInspection.rect.left - resizedFrame.left() * 300.0) < 0.02);
    QVERIFY(qAbs(
        editedInspection.rect.right - resizedFrame.right() * 300.0) < 0.02);
    QVERIFY(qAbs(
        editedInspection.rect.top
            - (1.0 - resizedFrame.top()) * 600.0) < 0.02);
    QVERIFY(qAbs(
        editedInspection.rect.bottom
            - (1.0 - resizedFrame.bottom()) * 600.0) < 0.02);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    QVERIFY(controller.commentItems()
        .constFirst()
        .toMap()
        .value(QStringLiteral("drawingHasInterior"))
        .toBool());
}
