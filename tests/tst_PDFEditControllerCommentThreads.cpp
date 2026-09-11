#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::flattensNestedSourceCommentReplies()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("nested-comment-replies-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("nested-comment-replies-deleted.pdf"));
    QVERIFY(createNestedCommentRepliesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);

    const QVariantList comments = controller.commentItems();
    const QVariantMap root = comments.at(0).toMap();
    const QString rootObjectId = root
        .value(QStringLiteral("objectId"))
        .toString();
    QCOMPARE(
        root.value(QStringLiteral("text")).toString(),
        QStringLiteral("Root comment"));
    QCOMPARE(root.value(QStringLiteral("replyCount")).toInt(), 2);
    for (qsizetype index = 1; index < comments.size(); ++index) {
        const QVariantMap reply = comments.at(index).toMap();
        QVERIFY(reply.value(QStringLiteral("isReply")).toBool());
        QCOMPARE(
            reply.value(QStringLiteral("parentObjectId")).toString(),
            rootObjectId);
        QCOMPARE(reply.value(QStringLiteral("replyCount")).toInt(), 0);
    }
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("First reply"));
    QCOMPARE(
        comments.at(2).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Nested reply"));
    QVERIFY(controller.addCommentReply(
        0,
        comments.at(1).toMap()
            .value(QStringLiteral("objectId")).toString(),
        QStringLiteral("Rejected nested reply")).isEmpty());

    const QString firstReplyObjectId = comments.at(1).toMap()
        .value(QStringLiteral("objectId"))
        .toString();
    const QString nestedReplyObjectId = comments.at(2).toMap()
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.selectComment(0, nestedReplyObjectId));
    QVERIFY(controller.removeComment(0, firstReplyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(controller.selectedObjectId().isEmpty());
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("replyCount")).toInt(),
        0);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 3);

    QVERIFY(controller.removeComment(0, rootObjectId));
    QVERIFY(controller.commentItems().isEmpty());
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 3);
    QVERIFY(controller.removeComment(0, firstReplyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentReplyInspection inspection
        = inspectCommentReply(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.textAnnotationCount, 1);
    const StickyNoteSaveInspection rootInspection
        = inspectStickyNotes(outputPath);
    QVERIFY(rootInspection.readable);
    QCOMPARE(rootInspection.count, 1);
    QCOMPARE(rootInspection.contents, QStringLiteral("Root comment"));
}

void PDFEditControllerTest::excludesCommentStateAnnotationsFromCommentModels()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-state-thread.pdf"));
    QVERIFY(createCommentStateThreadPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentIndexState(),
        PDFEditController::CommentIndexReady,
        5000);

    const auto verifyModel = [](const QVariantList &comments) {
        QCOMPARE(comments.size(), 2);
        const QVariantMap root = comments.at(0).toMap();
        const QVariantMap reply = comments.at(1).toMap();
        const QString rootObjectId = root
            .value(QStringLiteral("objectId"))
            .toString();
        QCOMPARE(
            root.value(QStringLiteral("text")).toString(),
            QStringLiteral("Root comment"));
        QVERIFY(!root.value(QStringLiteral("isReply")).toBool());
        QCOMPARE(root.value(QStringLiteral("replyCount")).toInt(), 1);
        QVERIFY(!root.value(QStringLiteral("deletable")).toBool());
        QCOMPARE(
            reply.value(QStringLiteral("text")).toString(),
            QStringLiteral("Visible reply"));
        QVERIFY(reply.value(QStringLiteral("isReply")).toBool());
        QCOMPARE(
            reply.value(QStringLiteral("parentObjectId")).toString(),
            rootObjectId);
    };

    verifyModel(controller.commentItems());
    verifyModel(controller.commentPanelItems());
    QVERIFY(!controller.selectComment(0, QStringLiteral("annotation:2")));
    QVERIFY(!controller.selectComment(0, QStringLiteral("annotation:3")));
    QVERIFY(!controller.selectComment(0, QStringLiteral("annotation:4")));
}

void PDFEditControllerTest::hidesCommentRepliesFromViewer()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The PDF viewer regression test needs a Qt Quick render screen.");
    }

    const QString rootOnlyPath = m_temporaryDirectory.filePath(
        QStringLiteral("viewer-root-comment-only.pdf"));
    const QString repliesPath = m_temporaryDirectory.filePath(
        QStringLiteral("viewer-comment-replies.pdf"));
    QVERIFY(createRootCommentOnlyPdf(rootOnlyPath));
    QVERIFY(createNestedCommentRepliesPdf(repliesPath));

    const QSize renderSize(240, 240);
    const QImage rootOnlyImage = renderFirstPageInViewer(
        rootOnlyPath,
        renderSize);
    const QImage repliesImage = renderFirstPageInViewer(
        repliesPath,
        renderSize);
    QVERIFY(!rootOnlyImage.isNull());
    QVERIFY(!repliesImage.isNull());
    QCOMPARE(rootOnlyImage.size(), repliesImage.size());

    QImage blankImage(rootOnlyImage.size(), QImage::Format_ARGB32);
    blankImage.fill(Qt::white);
    QVERIFY(changedPixelCount(
        rootOnlyImage,
        blankImage,
        rootOnlyImage.rect()) > 0);
    QCOMPARE(
        changedPixelCount(
            rootOnlyImage,
            repliesImage,
            rootOnlyImage.rect()),
        0);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(repliesPath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);
    QCOMPARE(controller.commentItems().size(), 3);
    const QVariantMap sourceReply = controller.commentItems().at(1).toMap();
    QVERIFY(sourceReply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(controller.selectComment(
        0,
        sourceReply.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedCommentReply());
    const QVariantList pageCandidates = waitForCandidates(controller, 0);
    bool rootCandidateFound = false;
    for (const QVariant &candidate : pageCandidates) {
        const QVariantMap candidateMap = candidate.toMap();
        QVERIFY(!candidateMap.value(QStringLiteral("isReply")).toBool());
        rootCandidateFound = rootCandidateFound
            || (candidateMap.value(QStringLiteral("kind")).toString()
                    == QStringLiteral("stickyNote")
                && candidateMap.value(QStringLiteral("text")).toString()
                    == QStringLiteral("Root comment"));
    }
    QVERIFY(rootCandidateFound);
}

void PDFEditControllerTest::hidesUnresolvedSourceReplyFromViewerModel()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("viewer-unresolved-comment-reply.pdf"));
    QVERIFY(createUnsupportedRootCommentReplyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap reply = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        reply.value(QStringLiteral("text")).toString(),
        QStringLiteral("Unresolved reply"));
    QVERIFY(!reply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(controller.selectComment(
        0,
        reply.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedCommentReply());

    QVERIFY(waitForCandidates(controller, 0).isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.candidateAt(0, 0.32, 0.22).isEmpty());
}

void PDFEditControllerTest::deletesMixedSubtypeSourceReplyThread()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-subtype-replies-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-subtype-replies-output.pdf"));
    QVERIFY(createMixedSubtypeCommentRepliesPdf(sourcePath));
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_TEXT), 2);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_FREETEXT), 1);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_STAMP), 1);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_INK), 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 6, 5000);

    QVariantMap root;
    QVariantMap supportedNestedReply;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        const QString text = comment.value(QStringLiteral("text")).toString();
        if (text == QLatin1String("Mixed root")) {
            root = comment;
        } else if (text == QLatin1String("Text after FreeText")) {
            supportedNestedReply = comment;
        }
    }
    QVERIFY(!root.isEmpty());
    QVERIFY(!supportedNestedReply.isEmpty());
    QVERIFY(supportedNestedReply.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        supportedNestedReply.value(QStringLiteral("parentObjectId")).toString(),
        root.value(QStringLiteral("objectId")).toString());

    QVERIFY(controller.removeComment(
        0,
        root.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.commentItems().isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_TEXT), 0);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_FREETEXT), 0);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_STAMP), 0);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_INK), 0);
    const CommentAnnotationInspection projectionInspection
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("mixed-projection"));
    QVERIFY(projectionInspection.readable);
    QCOMPARE(projectionInspection.annotationIndex, -1);
    QCOMPARE(projectionInspection.annotationCount, 0);
}

void PDFEditControllerTest::rejectsNonMarkupReplyDescendantDeletion()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("non-markup-reply-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("non-markup-reply-output.pdf"));
    QVERIFY(createNonMarkupCommentReplyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap root = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        root.value(QStringLiteral("text")).toString(),
        QStringLiteral("Protected root"));
    QVERIFY(controller.removeComment(
        0,
        root.value(QStringLiteral("objectId")).toString()));

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        controller,
        outputPath);
    QCOMPARE(
        result.error,
        PDFSaveWriter::Error::CommentAnnotationRemovalFailed);
    QVERIFY(!QFile::exists(outputPath));
    QVERIFY(controller.hasDrafts());
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_TEXT), 1);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_LINK), 1);
}

void PDFEditControllerTest::deletesSourceCommentsWithUndoRedoDiscardAndSave()
{
    const QString basePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comments-base.pdf"));
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comments-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comments-output.pdf"));
    QVERIFY(createFontStylePdf(basePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(basePath)));
    controller.setDocument(&document);
    const QVariantMap textMarkupCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kUprightText));
    QVERIFY(!textMarkupCandidate.isEmpty());
    const QRectF textMarkupRect = textMarkupCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();

    controller.setTool(PDFEditController::HighlightTool);
    QVERIFY(!controller.addHighlight(
        0,
        textMarkupRect).isEmpty());
    controller.setTool(PDFEditController::UnderlineTool);
    QVERIFY(!controller.addHighlight(
        0,
        textMarkupRect).isEmpty());
    controller.setTool(PDFEditController::StrikeoutTool);
    QVERIFY(!controller.addHighlight(
        0,
        textMarkupRect).isEmpty());
    controller.setTool(PDFEditController::SquigglyTool);
    QVERIFY(!controller.addHighlight(
        0,
        textMarkupRect).isEmpty());
    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.2, 0.6)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Source comment"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(sourcePath)));
    QVERIFY(!controller.hasDrafts());

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 5, 5000);
    const QVariantMap stickyNote = candidateForKind(
        controller.commentItems(),
        QStringLiteral("stickyNote"));
    QVERIFY(!stickyNote.isEmpty());
    QVERIFY(controller.removeComment(
        stickyNote.value(QStringLiteral("page")).toInt(),
        stickyNote.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.commentItems().size(), 4);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 5);
    QVERIFY(controller.canRedo());
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.commentItems().size(), 4);

    controller.discardDrafts();
    QCOMPARE(controller.commentItems().size(), 5);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const QVariantList commentsToDelete = controller.commentItems();
    for (const QVariant &commentValue : commentsToDelete) {
        const QVariantMap comment = commentValue.toMap();
        QVERIFY(controller.removeComment(
            comment.value(QStringLiteral("page")).toInt(),
            comment.value(QStringLiteral("objectId")).toString()));
    }
    QVERIFY(controller.commentItems().isEmpty());
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    QCOMPARE(inspectHighlights(sourcePath).count, 1);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            sourcePath,
            FPDF_ANNOT_UNDERLINE).count,
        1);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            sourcePath,
            FPDF_ANNOT_STRIKEOUT).count,
        1);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            sourcePath,
            FPDF_ANNOT_SQUIGGLY).count,
        1);
    QCOMPARE(inspectStickyNotes(sourcePath).count, 1);

    QCOMPARE(inspectHighlights(outputPath).count, 0);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            outputPath,
            FPDF_ANNOT_UNDERLINE).count,
        0);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            outputPath,
            FPDF_ANNOT_STRIKEOUT).count,
        0);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            outputPath,
            FPDF_ANNOT_SQUIGGLY).count,
        0);
    QCOMPARE(inspectStickyNotes(outputPath).count, 0);
    QVERIFY(normalizedPageText(outputPath).contains(
        QString::fromLatin1(kRegularText)));
}

void PDFEditControllerTest::deletesSourceCommentPopupRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comment-popup-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comment-popup-output.pdf"));
    QVERIFY(createStickyNotePopupPdf(sourcePath));
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_TEXT), 2);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_POPUP), 2);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    const QVariantList indexedComments = controller.commentItems();
    const QVariantMap comment = indexedComments.constFirst().toMap();
    QCOMPARE(
        comment.value(QStringLiteral("text")).toString(),
        QStringLiteral("Reverse-only popup comment"));
    QCOMPARE(
        comment.value(QStringLiteral("commentAuthor")).toString(),
        QStringLiteral("Alice Example"));
    const QDateTime creationDate = comment
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    QCOMPARE(
        creationDate.toString(Qt::ISODate),
        QStringLiteral("2024-01-02T03:04:05+07:00"));
    QVERIFY(!comment
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        comment.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);

    const QVariantMap preservedComment = indexedComments.at(1).toMap();
    QCOMPARE(
        preservedComment.value(QStringLiteral("commentAuthor")).toString(),
        QStringLiteral("Bob Example"));
    const QDateTime modificationDate = preservedComment
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime();
    QCOMPARE(
        modificationDate.toString(Qt::ISODate),
        QStringLiteral("2024-03-04T05:06:07+07:00"));
    QCOMPARE(
        preservedComment.value(QStringLiteral("commentDate")).toDateTime(),
        modificationDate);
    QVERIFY(controller.removeComment(
        comment.value(QStringLiteral("page")).toInt(),
        comment.value(QStringLiteral("objectId")).toString()));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_TEXT), 2);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_POPUP), 2);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_TEXT), 1);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_POPUP), 1);
    const StickyNoteSaveInspection remaining = inspectStickyNotes(outputPath);
    QCOMPARE(remaining.count, 1);
    QCOMPARE(remaining.contents, QStringLiteral("Preserved popup comment"));
    QCOMPARE(remaining.author, QStringLiteral("Bob Example"));
    QCOMPARE(
        remaining.creationDate,
        QStringLiteral("D:20240203040506+07'00'"));
    QCOMPARE(
        remaining.modificationDate,
        QStringLiteral("D:20240304050607+07'00'"));
    QVERIFY(hasSingleBidirectionalCommentPopup(outputPath));
}

void PDFEditControllerTest::savesCommentReplyRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-comment-reply-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-comment-reply-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    PDFFormDesignerController formDesigner;
    formDesigner.setDocument(&document);
    controller.setFormDesignerController(&formDesigner);
    const QString formFieldId = formDesigner.addTextField(
        0,
        0.55,
        0.72,
        0.3,
        0.06);
    QVERIFY(!formFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        formFieldId,
        QStringLiteral("ReplyCompanion")));
    controller.setTool(PDFEditController::StickyNoteTool);

    const QString rootText = QStringLiteral("Root comment");
    const QString replyText = QStringLiteral("Persisted reply");
    const QVariantMap root = controller.addStickyNote(
        0,
        QPointF(0.2, 0.3));
    QVERIFY(!root.isEmpty());
    controller.setSelectedCommentText(rootText);
    const QVariantMap reply = controller.addCommentReply(
        0,
        root.value(QStringLiteral("objectId")).toString(),
        replyText);
    QVERIFY(!reply.isEmpty());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const CommentReplyInspection sourceInspection
        = inspectCommentReply(sourcePath);
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.textAnnotationCount, 0);

    const CommentReplyInspection inspection
        = inspectCommentReply(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.textAnnotationCount, 2);
    QVERIFY(inspection.rootIndex >= 0);
    QVERIFY(inspection.replyIndex >= 0);
    QVERIFY(inspection.rootIndex != inspection.replyIndex);
    QCOMPARE(inspection.rootContents, rootText);
    QCOMPARE(inspection.replyContents, replyText);
    QCOMPARE(inspection.relation, QStringLiteral("R"));
    QVERIFY(!inspection.parentThreadId.isEmpty());
    QCOMPARE(inspection.replyToThreadId, inspection.parentThreadId);
    const CommentAnnotationInspection rootAppearance
        = inspectCommentAnnotation(
            outputPath,
            root.value(QStringLiteral("objectId")).toString());
    const CommentAnnotationInspection replyAppearance
        = inspectCommentAnnotation(
            outputPath,
            reply.value(QStringLiteral("objectId")).toString());
    QVERIFY(rootAppearance.hasAppearance);
    QVERIFY(!rootAppearance.appearance.isEmpty());
    QVERIFY(replyAppearance.hasAppearance);
    QVERIFY(!replyAppearance.appearance.isEmpty());
    const AcroFormFieldInspection fieldInspection
        = inspectAcroFormField(
            outputPath,
            QStringLiteral("ReplyCompanion"));
    QVERIFY(fieldInspection.readable);
    QCOMPARE(fieldInspection.widgetCount, 1);
    QCOMPARE(fieldInspection.fieldType, FPDF_FORMFIELD_TEXTFIELD);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    const QVariantList comments = controller.commentItems();
    const QVariantMap rehydratedRoot = comments.at(0).toMap();
    const QVariantMap rehydratedReply = comments.at(1).toMap();
    const QString rehydratedRootObjectId = rehydratedRoot
        .value(QStringLiteral("objectId"))
        .toString();

    QCOMPARE(
        rehydratedRoot.value(QStringLiteral("text")).toString(),
        rootText);
    QVERIFY(!rehydratedRoot.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        rehydratedRoot.value(QStringLiteral("parentObjectId")).toString(),
        QString());
    QCOMPARE(
        rehydratedRoot.value(QStringLiteral("replyCount")).toInt(),
        1);
    QCOMPARE(
        rehydratedReply.value(QStringLiteral("text")).toString(),
        replyText);
    QVERIFY(rehydratedReply.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        rehydratedReply.value(QStringLiteral("parentObjectId")).toString(),
        rehydratedRootObjectId);
    QCOMPARE(
        rehydratedReply.value(QStringLiteral("replyCount")).toInt(),
        0);
    QVERIFY(rehydratedRoot.value(QStringLiteral("editable")).toBool());
    QVERIFY(!rehydratedReply.value(QStringLiteral("editable")).toBool());
    QVERIFY(rehydratedRoot
        .value(QStringLiteral("commentTextEditable")).toBool());
    QVERIFY(rehydratedReply
        .value(QStringLiteral("commentTextEditable")).toBool());
}

void PDFEditControllerTest::normalizesCommentContentsLineEndingsOnSave()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-line-endings-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-line-endings-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    controller.setTool(PDFEditController::StickyNoteTool);
    const QVariantMap root = controller.addStickyNote(
        0,
        QPointF(0.12, 0.16));
    QVERIFY(!root.isEmpty());
    const QString rootObjectId = root
        .value(QStringLiteral("objectId"))
        .toString();
    const QString mixedLineEndings
        = QStringLiteral("Dòng 1\nDòng 2\r\nDòng 3\rDòng 4");
    const QString editingText
        = QStringLiteral("Dòng 1\nDòng 2\nDòng 3\nDòng 4");
    const QString pdfText
        = QStringLiteral("Dòng 1\rDòng 2\rDòng 3\rDòng 4");
    QVERIFY(controller.setCommentText(
        0,
        rootObjectId,
        mixedLineEndings));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        editingText);

    const QVariantMap reply = controller.addCommentReply(
        0,
        rootObjectId,
        mixedLineEndings);
    QVERIFY(!reply.isEmpty());
    controller.setTool(PDFEditController::FreeTextTool);
    const QVariantMap freeText = controller.addFreeText(
        0,
        QRectF(0.15, 0.55, 0.35, 0.15));
    QVERIFY(!freeText.isEmpty());
    QVERIFY(controller.setCommentText(
        0,
        freeText.value(QStringLiteral("objectId")).toString(),
        mixedLineEndings));
    controller.setTool(PDFEditController::HighlightTool);
    const QVariantMap highlight = controller.addHighlight(
        0,
        textCandidate.value(QStringLiteral("sourceRect")).toRectF());
    QVERIFY(!highlight.isEmpty());
    QVERIFY(controller.setCommentText(
        0,
        highlight.value(QStringLiteral("objectId")).toString(),
        mixedLineEndings));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QVector<QString> annotationNames{
        rootObjectId,
        reply.value(QStringLiteral("objectId")).toString(),
        freeText.value(QStringLiteral("objectId")).toString(),
        highlight.value(QStringLiteral("objectId")).toString(),
    };
    for (const QString &annotationName : annotationNames) {
        const CommentAnnotationInspection inspection
            = inspectCommentAnnotation(outputPath, annotationName);
        QVERIFY(inspection.readable);
        QCOMPARE(inspection.contents, pdfText);
        QVERIFY(!inspection.contents.contains(QLatin1Char('\n')));
        QVERIFY(inspection.hasAppearance);
        QVERIFY(!inspection.appearance.isEmpty());
    }

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 4, 5000);
    for (const QVariant &commentValue : controller.commentItems()) {
        QCOMPARE(
            commentValue.toMap().value(QStringLiteral("text")).toString(),
            editingText);
    }
}

void PDFEditControllerTest::keepsStickyNoteNamesUniqueAcrossReopen()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("sticky-note-name-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("sticky-note-name-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.2, 0.3)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("First comment"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const StickyNoteSaveInspection firstInspection
        = inspectStickyNotes(outputPath);
    QVERIFY(firstInspection.readable);
    QCOMPARE(firstInspection.count, 1);
    QCOMPARE(firstInspection.names.size(), 1);
    QVERIFY(!firstInspection.names.constFirst().isEmpty());
    const QString firstName = firstInspection.names.constFirst();

    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.55, 0.45)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Second comment"));
    QVERIFY(controller.save());

    const StickyNoteSaveInspection secondInspection
        = inspectStickyNotes(outputPath);
    QVERIFY(secondInspection.readable);
    QCOMPARE(secondInspection.count, 2);
    QCOMPARE(secondInspection.names.size(), 2);
    QSet<QString> uniqueNames;
    for (const QString &name : secondInspection.names) {
        QVERIFY(!name.isEmpty());
        uniqueNames.insert(name);
    }
    QCOMPARE(uniqueNames.size(), secondInspection.names.size());
    QVERIFY(uniqueNames.contains(firstName));
}

void PDFEditControllerTest::keepsMarkupReplyIdsUniqueAcrossReopen()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("markup-reply-id-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("markup-reply-id-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    controller.setTool(PDFEditController::HighlightTool);
    QVERIFY(!controller.addHighlight(
        0,
        textCandidate.value(QStringLiteral("sourceRect")).toRectF()).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    const QString sourceHighlightObjectId = controller.commentItems()
        .constFirst()
        .toMap()
        .value(QStringLiteral("objectId"))
        .toString();

    textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    controller.setTool(PDFEditController::HighlightTool);
    const QVariantMap secondHighlight = controller.addHighlight(
        0,
        textCandidate.value(QStringLiteral("sourceRect")).toRectF());
    QVERIFY(!secondHighlight.isEmpty());
    const QVariantMap reply = controller.addCommentReply(
        0,
        sourceHighlightObjectId,
        QStringLiteral("Reply to source highlight"));
    QVERIFY(!reply.isEmpty());
    QVERIFY(controller.save());

    const CommentAnnotationInspection replyInspection
        = inspectCommentAnnotation(
            outputPath,
            reply.value(QStringLiteral("objectId")).toString());
    QVERIFY(replyInspection.readable);
    QVERIFY(replyInspection.hasAppearance);
    QVERIFY(replyInspection.appearance.contains(QStringLiteral("/GS gs")));

    const AnnotationAppearanceStructureInspection appearanceStructure
        = inspectAnnotationAppearanceStructure(
            outputPath,
            reply.value(QStringLiteral("objectId")).toString());
    QVERIFY(appearanceStructure.readable);
    QVERIFY(appearanceStructure.annotationFound);
    QVERIFY(appearanceStructure.normalAppearanceIsIndirect);
    QVERIFY(appearanceStructure.appearanceObjectFound);
    QVERIFY(appearanceStructure.appearanceIsStream);
    QVERIFY(appearanceStructure.typeIsXObject);
    QVERIFY(appearanceStructure.subtypeIsForm);
    QCOMPARE(appearanceStructure.boundingBox.size(), 4);
    QVERIFY(appearanceStructure.hasResources);
    QVERIFY(appearanceStructure.hasExtGState);
    QVERIFY(appearanceStructure.hasGraphicsState);
    QVERIFY(appearanceStructure.graphicsStateTypeIsExtGState);
    QVERIFY(appearanceStructure.blendModeIsNormal);
    QVERIFY(appearanceStructure.alphaSourceIsFalse);

    constexpr qreal expectedReplyOpacity = 96.0 / 255.0;
    QVERIFY(qAbs(appearanceStructure.strokingOpacity
                 - expectedReplyOpacity) < 0.00001);
    QVERIFY(qAbs(appearanceStructure.nonStrokingOpacity
                 - expectedReplyOpacity) < 0.00001);
    QVERIFY(qAbs(appearanceStructure.boundingBox.at(0)
                 - replyInspection.rect.left) < 0.01);
    QVERIFY(qAbs(appearanceStructure.boundingBox.at(1)
                 - replyInspection.rect.bottom) < 0.01);
    QVERIFY(qAbs(appearanceStructure.boundingBox.at(2)
                 - replyInspection.rect.right) < 0.01);
    QVERIFY(qAbs(appearanceStructure.boundingBox.at(3)
                 - replyInspection.rect.top) < 0.01);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);
    int replyCount = 0;
    int rootsWithReplies = 0;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("isReply")).toBool()) {
            ++replyCount;
        } else if (comment.value(QStringLiteral("replyCount")).toInt() > 0) {
            ++rootsWithReplies;
        }
    }
    QCOMPARE(replyCount, 1);
    QCOMPARE(rootsWithReplies, 1);
}
