#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::limitsCommentSelectToolToComments()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-select-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);

    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    const QString textObjectId = textCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QPointF textHitPoint = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .center();
    QVERIFY(controller.selectCandidate(0, textObjectId));
    QVERIFY(controller.addBlankPage() >= 0);

    controller.setTool(PDFEditController::CommentSelectTool);
    QVERIFY(controller.selectedObjectId().isEmpty());
    QVERIFY(controller.undo() >= 0);
    QVERIFY(controller.selectedObjectId().isEmpty());
    QVERIFY(controller.candidateAt(
        0,
        textHitPoint.x(),
        textHitPoint.y()).isEmpty());
    QVERIFY(!controller.selectCandidate(0, textObjectId));

    controller.setTool(PDFEditController::StickyNoteTool);
    const QVariantMap note = controller.addStickyNote(
        0,
        QPointF(0.85, 0.85));
    QVERIFY(!note.isEmpty());
    const QString noteObjectId = note
        .value(QStringLiteral("objectId"))
        .toString();
    const QPointF noteHitPoint = note
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .center();

    controller.setTool(PDFEditController::CommentSelectTool);
    QCOMPARE(controller.selectedObjectId(), noteObjectId);
    QCOMPARE(
        controller.candidateAt(
            0,
            noteHitPoint.x(),
            noteHitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        noteObjectId);
    QVERIFY(controller.selectCandidate(0, noteObjectId));

    const QVariantList candidates = controller.candidatesForPage(0);
    QVERIFY(!candidateForKind(candidates, QStringLiteral("stickyNote")).isEmpty());
}

void PDFEditControllerTest::addsStickyNoteWithPanelModelAndUndo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-sticky-note-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::StickyNoteTool);

    const QVariantMap note = controller.addStickyNote(
        0,
        QPointF(0.2, 0.3));
    QCOMPARE(
        note.value(QStringLiteral("kind")).toString(),
        QStringLiteral("stickyNote"));
    QCOMPARE(controller.tool(), PDFEditController::CommentSelectTool);
    QCOMPARE(controller.selectedObjectId(),
             note.value(QStringLiteral("objectId")).toString());
    const QDateTime creationDate = note
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    QVERIFY(creationDate.isValid());
    QVERIFY(!note
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        note.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(
        controller.commentItems()
            .constFirst()
            .toMap()
            .value(QStringLiteral("page"))
            .toInt(),
        0);

    const QString commentText = QStringLiteral("Cần kiểm tra lại số liệu.");
    controller.setSelectedCommentText(commentText);
    QCOMPARE(controller.selectedText(), commentText);
    QCOMPARE(
        controller.commentItems()
            .constFirst()
            .toMap()
            .value(QStringLiteral("text"))
            .toString(),
        commentText);
    const QDateTime modificationDate = controller.commentItems()
        .constFirst()
        .toMap()
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime();
    QVERIFY(modificationDate.isValid());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedText(), QString());
    const QVariantMap undoneNote = controller.commentItems()
        .constFirst()
        .toMap();
    QVERIFY(!undoneNote
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        undoneNote.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedText(), commentText);
    const QVariantMap redoneNote = controller.commentItems()
        .constFirst()
        .toMap();
    QCOMPARE(
        redoneNote.value(QStringLiteral("commentModifiedDate")).toDateTime(),
        modificationDate);
    QCOMPARE(
        redoneNote.value(QStringLiteral("commentDate")).toDateTime(),
        modificationDate);
    QCOMPARE(controller.selectRelativeComment(1), 0);

    QVERIFY(controller.removeComment(
        0,
        note.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.commentItems().isEmpty());
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 1);
}

void PDFEditControllerTest::addsFreeTextWithPanelModelAndUndo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-free-text-source.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::FreeTextTool);

    const QRectF placement(0.12, 0.16, 0.32, 0.12);
    const QVariantMap freeText = controller.addFreeText(0, placement);
    QVERIFY(!freeText.isEmpty());
    QCOMPARE(
        freeText.value(QStringLiteral("kind")).toString(),
        QStringLiteral("freeText"));
    QCOMPARE(controller.tool(), PDFEditController::CommentSelectTool);
    const QString objectId = freeText
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!objectId.isEmpty());
    QCOMPARE(controller.selectedPage(), 0);
    QCOMPARE(controller.selectedObjectId(), objectId);
    QCOMPARE(controller.commentItems().size(), 1);

    const QVariantMap initialItem = controller.commentItems()
        .constFirst()
        .toMap();
    QCOMPARE(
        initialItem.value(QStringLiteral("kind")).toString(),
        QStringLiteral("freeText"));
    QVERIFY(initialItem
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    QVERIFY(initialItem
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime()
        .isValid());

    const QString commentText
        = QStringLiteral("Nội dung Free Text có dấu tiếng Việt.");
    QVERIFY(controller.setCommentText(0, objectId, commentText));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        commentText);
    QVERIFY(controller.hasDrafts());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QString());
    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.commentItems().isEmpty());
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        commentText);

    QVERIFY(controller.removeComment(0, objectId));
    QVERIFY(controller.commentItems().isEmpty());
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 1);
    controller.discardDrafts();
    QVERIFY(controller.commentItems().isEmpty());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::addsCommentReplyWithOneLevelModelAndUndo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-comment-reply-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::StickyNoteTool);
    const QVariantMap root = controller.addStickyNote(
        0,
        QPointF(0.2, 0.3));
    QVERIFY(!root.isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Root comment"));
    const QString rootObjectId = root
        .value(QStringLiteral("objectId"))
        .toString();

    const QString replyText = QStringLiteral("First-level reply");
    const QVariantMap reply = controller.addCommentReply(
        0,
        rootObjectId,
        replyText);
    QVERIFY(!reply.isEmpty());
    const QString replyObjectId = reply
        .value(QStringLiteral("objectId"))
        .toString();
    QCOMPARE(
        reply.value(QStringLiteral("kind")).toString(),
        QStringLiteral("stickyNote"));
    QCOMPARE(
        reply.value(QStringLiteral("parentObjectId")).toString(),
        rootObjectId);
    QVERIFY(reply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(controller.selectedCommentReply());

    const QVariantList pageDrafts = controller.draftsForPage(0);
    QCOMPARE(pageDrafts.size(), 1);
    QCOMPARE(
        pageDrafts.constFirst().toMap()
            .value(QStringLiteral("objectId")).toString(),
        rootObjectId);
    const QVariantList pageCandidates = controller.candidatesForPage(0);
    for (const QVariant &pageCandidate : pageCandidates) {
        QVERIFY(
            pageCandidate.toMap()
                .value(QStringLiteral("objectId")).toString()
            != replyObjectId);
    }

    QVariantList comments = controller.commentItems();
    QCOMPARE(comments.size(), 2);
    const QVariantMap rootItem = comments.at(0).toMap();
    const QVariantMap replyItem = comments.at(1).toMap();
    QVERIFY(!rootItem.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(rootItem.value(QStringLiteral("replyCount")).toInt(), 1);
    QVERIFY(replyItem.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        replyItem.value(QStringLiteral("parentObjectId")).toString(),
        rootObjectId);
    QCOMPARE(
        replyItem.value(QStringLiteral("text")).toString(),
        replyText);

    QVERIFY(controller.addCommentReply(
        0,
        replyItem.value(QStringLiteral("objectId")).toString(),
        QStringLiteral("Rejected nested reply")).isEmpty());
    QVERIFY(controller.addCommentReply(
        0,
        rootObjectId,
        QStringLiteral("   \n  ")).isEmpty());
    QCOMPARE(controller.commentItems().size(), 2);

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.commentItems().size(), 2);

    QVERIFY(controller.removeComment(0, replyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("replyCount")).toInt(),
        0);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 2);

    QVERIFY(controller.selectComment(0, replyObjectId));
    QCOMPARE(controller.selectedObjectId(), replyObjectId);
    QVERIFY(controller.removeComment(0, rootObjectId));
    QVERIFY(controller.commentItems().isEmpty());
    QVERIFY(controller.selectedObjectId().isEmpty());
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 2);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("replyCount")).toInt(),
        1);
}

void PDFEditControllerTest::distinguishesCommentIndexLoadingAndFailureFromEmptyState()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-state-source.pdf"));
    const QString backupPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-state-source.backup"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QCOMPARE(
        controller.commentIndexState(),
        PDFEditController::CommentIndexIdle);

    QVERIFY(QFile::rename(sourcePath, backupPath));
    bool sourceRestored = false;
    const auto restoreSource = qScopeGuard([&]() {
        if (!sourceRestored) {
            QFile::remove(sourcePath);
            QFile::rename(backupPath, sourcePath);
        }
    });
    QVERIFY(writePdfObjects(
        sourcePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray("(not a page dictionary)"),
        }));
    bool malformedPageFailsToLoad = false;
    {
        PDFiumProcessLock pdfiumLock;
        const QByteArray encodedPath = QFile::encodeName(sourcePath);
        FPDF_DOCUMENT malformedDocument = FPDF_LoadDocument(
            encodedPath.constData(),
            nullptr);
        if (malformedDocument) {
            FPDF_PAGE malformedPage = FPDF_LoadPage(malformedDocument, 0);
            malformedPageFailsToLoad
                = FPDF_GetPageCount(malformedDocument) == 1
                && !malformedPage;
            if (malformedPage) {
                FPDF_ClosePage(malformedPage);
            }
            FPDF_CloseDocument(malformedDocument);
        }
    }
    QVERIFY(malformedPageFailsToLoad);

    const QString themePath = QFINDTESTDATA(
        "../src/Components/AppTheme.qml");
    const QString panelPath = QFINDTESTDATA(
        "../src/Components/CommentPanel.qml");
    QVERIFY(!themePath.isEmpty());
    QVERIFY(!panelPath.isEmpty());
    QVERIFY(registerQmlTestTypes(themePath));

    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(
        QByteArrayLiteral("import TrePDF\nAppTheme {}"),
        QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));

    QQmlComponent panelComponent(
        &engine,
        QUrl::fromLocalFile(panelPath));
    const QVariantMap initialProperties{
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("document"), QVariant::fromValue(&document)},
        {QStringLiteral("editController"), QVariant::fromValue(&controller)}};
    std::unique_ptr<QObject> panel;
    {
        PDFiumProcessLock mainThreadLock;
        panel.reset(
            panelComponent.createWithInitialProperties(initialProperties));
        QVERIFY2(panel, qPrintable(panelComponent.errorString()));
        QCOMPARE(
            controller.commentIndexState(),
            PDFEditController::CommentIndexLoading);
        QVERIFY(panel->property("commentIndexLoading").toBool());
        QVERIFY(!panel->property("commentIndexFailed").toBool());
        QVERIFY(!panel->property("showNoCommentsState").toBool());
    }

    QObject *status = panel->findChild<QObject *>(
        QStringLiteral("commentIndexStatus"));
    QObject *statusLabel = panel->findChild<QObject *>(
        QStringLiteral("commentIndexStatusLabel"));
    QObject *retryButton = panel->findChild<QObject *>(
        QStringLiteral("retryCommentIndexButton"));
    QObject *banner = panel->findChild<QObject *>(
        QStringLiteral("commentIndexBanner"));
    QObject *bannerLabel = panel->findChild<QObject *>(
        QStringLiteral("commentIndexBannerLabel"));
    QObject *bannerRetryButton = panel->findChild<QObject *>(
        QStringLiteral("retryCommentIndexBannerButton"));
    QVERIFY(status);
    QVERIFY(statusLabel);
    QVERIFY(retryButton);
    QVERIFY(banner);
    QVERIFY(bannerLabel);
    QVERIFY(bannerRetryButton);
    QVERIFY(status->property("visible").toBool());
    QVERIFY(!banner->property("visible").toBool());
    QVERIFY(!retryButton->property("visible").toBool());
    const QString loadingText = statusLabel->property("text").toString();
    QVERIFY(!loadingText.isEmpty());

    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentIndexState(),
        PDFEditController::CommentIndexFailed,
        5000);
    QVERIFY(!panel->property("commentIndexLoading").toBool());
    QVERIFY(panel->property("commentIndexFailed").toBool());
    QVERIFY(!panel->property("showNoCommentsState").toBool());
    QVERIFY(status->property("visible").toBool());
    QVERIFY(!banner->property("visible").toBool());
    QVERIFY(retryButton->property("visible").toBool());
    QVERIFY(retryButton->property("enabled").toBool());
    const QString failedText = statusLabel->property("text").toString();
    QVERIFY(!failedText.isEmpty());
    QVERIFY(failedText != loadingText);

    QVariantMap partialComment;
    QString bannerLoadingText;
    {
        PDFiumProcessLock mainThreadLock;
        partialComment = controller.addStickyNote(
            0,
            QPointF(0.2, 0.3));
        QVERIFY(!partialComment.isEmpty());
        QCOMPARE(controller.commentItems().size(), 1);
        QCOMPARE(
            controller.commentIndexState(),
            PDFEditController::CommentIndexLoading);
        QVERIFY(panel->property("commentIndexLoading").toBool());
        QVERIFY(panel->property("showCommentIndexBanner").toBool());
        QVERIFY(!panel->property("showNoCommentsState").toBool());
        QVERIFY(!status->property("visible").toBool());
        QVERIFY(banner->property("visible").toBool());
        QVERIFY(!bannerRetryButton->property("visible").toBool());
        bannerLoadingText = bannerLabel->property("text").toString();
        QVERIFY(!bannerLoadingText.isEmpty());
    }

    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentIndexState(),
        PDFEditController::CommentIndexFailed,
        5000);
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(panel->property("commentIndexFailed").toBool());
    QVERIFY(panel->property("showCommentIndexBanner").toBool());
    QVERIFY(!panel->property("showNoCommentsState").toBool());
    QVERIFY(!status->property("visible").toBool());
    QVERIFY(banner->property("visible").toBool());
    QVERIFY(bannerRetryButton->property("visible").toBool());
    QVERIFY(bannerRetryButton->property("enabled").toBool());
    const QString bannerFailedText = bannerLabel->property("text").toString();
    QVERIFY(!bannerFailedText.isEmpty());
    QVERIFY(bannerFailedText != bannerLoadingText);

    QVERIFY(QFile::remove(sourcePath));
    QVERIFY(QFile::rename(backupPath, sourcePath));
    sourceRestored = true;
    QVERIFY(QMetaObject::invokeMethod(
        bannerRetryButton,
        "clicked",
        Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentIndexState(),
        PDFEditController::CommentIndexReady,
        5000);
    QVERIFY(!panel->property("commentIndexLoading").toBool());
    QVERIFY(!panel->property("commentIndexFailed").toBool());
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(!panel->property("showCommentIndexBanner").toBool());
    QVERIFY(!panel->property("showNoCommentsState").toBool());
    QVERIFY(!status->property("visible").toBool());
    QVERIFY(!banner->property("visible").toBool());
    QVERIFY(!bannerRetryButton->property("visible").toBool());
    QVERIFY(controller.removeComment(
        0,
        partialComment.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.commentItems().isEmpty());
    QVERIFY(panel->property("showNoCommentsState").toBool());
    QVERIFY(status->property("visible").toBool());
    QVERIFY(!retryButton->property("visible").toBool());
    const QString emptyText = statusLabel->property("text").toString();
    QVERIFY(!emptyText.isEmpty());
    QVERIFY(emptyText != failedText);
}

void PDFEditControllerTest::boundsWholeDocumentCommentIndexMemory()
{
    const QString oversizedPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-memory-budget.pdf"));
    const QString smallPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-memory-small.pdf"));
    QVERIFY(createCommentIndexMemoryBudgetPdf(oversizedPath));
    QVERIFY(createRootCommentOnlyPdf(smallPath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(oversizedPath)));
    QCOMPARE(document.pageCount(), 3);
    controller.setDocument(&document);

    controller.requestCommentIndex();
    QCOMPARE(
        controller.commentIndexState(),
        PDFEditController::CommentIndexLoading);
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentIndexState(),
        PDFEditController::CommentIndexLimited,
        10000);
    const QVariantList partialIndex = controller.commentItems();
    // Page 1 starts with a small root before its oversized reply. The whole
    // page must be discarded when the reply crosses the remaining budget.
    QCOMPARE(partialIndex.size(), 1);
    QCOMPARE(
        partialIndex.constFirst().toMap()
            .value(QStringLiteral("text")).toString().size(),
        kCommentIndexBudgetFieldCharacters);
    QCOMPARE(
        partialIndex.constFirst().toMap()
            .value(QStringLiteral("commentAuthor")).toString().size(),
        kCommentIndexBudgetFieldCharacters);
    QCOMPARE(
        partialIndex.constFirst().toMap()
            .value(QStringLiteral("page")).toInt(),
        0);

    QSignalSpy limitedStateSpy(
        &controller,
        &PDFEditController::commentIndexStateChanged);
    controller.requestCommentIndex();
    QTest::qWait(100);
    QCOMPARE(limitedStateSpy.count(), 0);
    QCOMPARE(
        controller.commentIndexState(),
        PDFEditController::CommentIndexLimited);
    QCOMPARE(controller.commentItems().size(), partialIndex.size());

    controller.setDocument(nullptr);
    QCOMPARE(
        controller.commentIndexState(),
        PDFEditController::CommentIndexIdle);
    QVERIFY(document.open(QUrl::fromLocalFile(smallPath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentIndexState(),
        PDFEditController::CommentIndexReady,
        5000);
    QCOMPARE(controller.commentItems().size(), 1);
}

void PDFEditControllerTest::keepsUncommittedCommentPanelState()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-panel-pending-state.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap rootComment = controller.addStickyNote(
        0,
        QPointF(0.2, 0.3));
    QVERIFY(!rootComment.isEmpty());
    const QString rootObjectId = rootComment
        .value(QStringLiteral("objectId"))
        .toString();
    const qsizetype initialCommentCount = controller.commentItems().size();
    QCOMPARE(initialCommentCount, 1);

    const QString themePath = QFINDTESTDATA(
        "../src/Components/AppTheme.qml");
    const QString panelPath = QFINDTESTDATA(
        "../src/Components/CommentPanel.qml");
    QVERIFY(!themePath.isEmpty());
    QVERIFY(!panelPath.isEmpty());
    QVERIFY(registerQmlTestTypes(themePath));

    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(
        QByteArrayLiteral("import TrePDF\nAppTheme {}"),
        QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));

    QQmlComponent panelComponent(
        &engine,
        QUrl::fromLocalFile(panelPath));
    const QVariantMap initialProperties{
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("document"), QVariant::fromValue(&document)},
        {QStringLiteral("editController"), QVariant::fromValue(&controller)}};
    const auto createPanel = [&]() {
        std::unique_ptr<QObject> panel(
            panelComponent.createWithInitialProperties(initialProperties));
        if (!panel) {
            qWarning().noquote() << panelComponent.errorString();
        }
        return panel;
    };
    const auto commitPendingEdit = [](QObject *panel) {
        QVariant result;
        const bool invoked = QMetaObject::invokeMethod(
            panel,
            "commitPendingEdit",
            Qt::DirectConnection,
            Q_RETURN_ARG(QVariant, result));
        return qMakePair(invoked, result.toBool());
    };

    {
        std::unique_ptr<QObject> panel = createPanel();
        QVERIFY(panel);
        const QString replyText = QStringLiteral("Unsent reply");
        QVERIFY(panel->setProperty("replyTargetPage", 0));
        QVERIFY(panel->setProperty("replyTargetObjectId", rootObjectId));
        QVERIFY(panel->setProperty("replyDraftText", replyText));
        QVERIFY(panel->property("textEditPending").toBool());

        QSignalSpy deleteCommentRequestSpy(
            panel.get(),
            SIGNAL(deleteCommentRequested(int,QString)));
        QVERIFY(deleteCommentRequestSpy.isValid());
        QVERIFY(QMetaObject::invokeMethod(
            panel.get(),
            "removeComment",
            Qt::DirectConnection,
            Q_ARG(QVariant, 0),
            Q_ARG(QVariant, rootObjectId)));
        QCOMPARE(deleteCommentRequestSpy.count(), 1);
        QCOMPARE(panel->property("replyDraftText").toString(), replyText);
        QCOMPARE(controller.commentItems().size(), initialCommentCount);

        const auto [invoked, committed] = commitPendingEdit(panel.get());
        QVERIFY(invoked);
        QVERIFY(!committed);
        QCOMPARE(panel->property("replyDraftText").toString(), replyText);
        QCOMPARE(controller.commentItems().size(), initialCommentCount);

        panel->setProperty("visible", false);
        QCoreApplication::processEvents();
        QCOMPARE(panel->property("replyDraftText").toString(), replyText);
        QVERIFY(panel->property("textEditPending").toBool());
        panel.reset();
        QCoreApplication::processEvents();
        QCOMPARE(controller.commentItems().size(), initialCommentCount);
    }

    {
        std::unique_ptr<QObject> panel = createPanel();
        QVERIFY(panel);
        const QString oversizedText(
            controller.maximumCommentCharacters() + 1,
            QLatin1Char('x'));
        QVERIFY(panel->setProperty("activeEditorPage", 0));
        QVERIFY(panel->setProperty("activeEditorObjectId", rootObjectId));
        QVERIFY(panel->setProperty("activeEditorSourceText", QString()));
        QVERIFY(panel->setProperty("activeEditorText", oversizedText));
        QVERIFY(panel->setProperty("activeEditorDirty", true));
        QVERIFY(panel->property("textEditPending").toBool());

        const auto [invoked, committed] = commitPendingEdit(panel.get());
        QVERIFY(invoked);
        QVERIFY(!committed);
        panel->setProperty("visible", false);
        QCoreApplication::processEvents();
        QCOMPARE(
            panel->property("activeEditorText").toString(),
            oversizedText);
        QVERIFY(panel->property("activeEditorDirty").toBool());
        QVERIFY(panel->property("textEditPending").toBool());
    }
}

void PDFEditControllerTest::guardsRibbonActionsWithPendingCommentPanel()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("ribbon-pending-comment-actions.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    const QString themePath = QFINDTESTDATA(
        "../src/Components/AppTheme.qml");
    const QString mainPath = QFINDTESTDATA("../src/Main.qml");
    QVERIFY(!themePath.isEmpty());
    QVERIFY(!mainPath.isEmpty());
    QVERIFY(registerQmlTestTypes(themePath));

    QQmlEngine engine;
    QQmlComponent mainComponent(
        &engine,
        QUrl::fromLocalFile(mainPath));
    std::unique_ptr<QObject> mainWindow(mainComponent.create());
    QVERIFY2(mainWindow, qPrintable(mainComponent.errorString()));
    QVERIFY(mainWindow->setProperty("visible", false));

    auto *tabModel = mainWindow->findChild<PDFTabModel *>();
    QVERIFY(tabModel);
    PDFDocumentSession *session = tabModel->open(
        QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);
    QCOMPARE(session->document()->pageCount(), 3);

    QObject *workspace = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            const QList<QObject *> objects
                = mainWindow->findChildren<QObject *>();
            const auto candidate = std::find_if(
                objects.cbegin(),
                objects.cend(),
                [](QObject *object) {
                    return object->metaObject()->indexOfProperty(
                               "commentPanelEditPending") >= 0;
                });
            workspace = candidate == objects.cend() ? nullptr : *candidate;
            return workspace != nullptr;
        })(),
        5000);
    QVERIFY(workspace->setProperty("commentPanelEditPending", true));

    QObject *commentPanel = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            const QList<QObject *> objects
                = workspace->findChildren<QObject *>();
            const auto candidate = std::find_if(
                objects.cbegin(),
                objects.cend(),
                [](QObject *object) {
                    return object->metaObject()->indexOfProperty(
                               "replyDraftText") >= 0
                        && object->metaObject()->indexOfMethod(
                               "commitPendingEdit()") >= 0;
                });
            commentPanel = candidate == objects.cend() ? nullptr : *candidate;
            return commentPanel != nullptr;
        })(),
        5000);

    PDFEditController *controller = session->editController();
    const QVariantMap rootComment = controller->addStickyNote(
        0,
        QPointF(0.2, 0.3));
    const QVariantMap transientComment = controller->addStickyNote(
        0,
        QPointF(0.4, 0.3));
    QVERIFY(!rootComment.isEmpty());
    QVERIFY(!transientComment.isEmpty());
    const QString rootObjectId = rootComment
        .value(QStringLiteral("objectId"))
        .toString();
    QCOMPARE(controller->commentItems().size(), 2);
    QCOMPARE(controller->undo(), -1);
    QCOMPARE(controller->commentItems().size(), 1);
    QVERIFY(controller->canUndo());
    QVERIFY(controller->canRedo());

    const QString replyText = QStringLiteral("Reply still being drafted");
    QVERIFY(commentPanel->setProperty("replyTargetPage", 0));
    QVERIFY(commentPanel->setProperty(
        "replyTargetObjectId", rootObjectId));
    QVERIFY(commentPanel->setProperty("replyDraftText", replyText));
    QVERIFY(commentPanel->property("textEditPending").toBool());

    const auto invokeBooleanAction = [&](const char *method) {
        QVariant result;
        const bool invoked = QMetaObject::invokeMethod(
            mainWindow.get(),
            method,
            Qt::DirectConnection,
            Q_RETURN_ARG(QVariant, result));
        return qMakePair(invoked, result.toBool());
    };
    const auto verifyPendingState = [&]() {
        QCOMPARE(controller->commentItems().size(), 1);
        QCOMPARE(
            commentPanel->property("replyDraftText").toString(),
            replyText);
        QVERIFY(commentPanel->property("textEditPending").toBool());
    };

    const auto [redoInvoked, redone] = invokeBooleanAction("requestRedo");
    QVERIFY(redoInvoked);
    QVERIFY(!redone);
    QVERIFY(controller->canRedo());
    verifyPendingState();

    const auto [undoInvoked, undone] = invokeBooleanAction("requestUndo");
    QVERIFY(undoInvoked);
    QVERIFY(!undone);
    QVERIFY(controller->canUndo());
    verifyPendingState();

    QVariant deleteResult;
    QVERIFY(QMetaObject::invokeMethod(
        mainWindow.get(),
        "requestDeleteComment",
        Qt::DirectConnection,
        Q_RETURN_ARG(QVariant, deleteResult),
        Q_ARG(QVariant, 0),
        Q_ARG(QVariant, rootObjectId)));
    QVERIFY(!deleteResult.toBool());
    verifyPendingState();

    QVERIFY(QMetaObject::invokeMethod(
        mainWindow.get(),
        "requestDeletePage",
        Qt::DirectConnection,
        Q_ARG(QVariant, 0)));
    QCOMPARE(session->document()->pageCount(), 3);
    QVERIFY(!mainWindow->property("deletePageSession")
                 .value<PDFDocumentSession *>());
    verifyPendingState();

    QVERIFY(QMetaObject::invokeMethod(
        commentPanel,
        "cancelPendingReply",
        Qt::DirectConnection));
    const QString oversizedText(
        controller->maximumCommentCharacters() + 1,
        QLatin1Char('x'));
    QVERIFY(commentPanel->setProperty("activeEditorPage", 0));
    QVERIFY(commentPanel->setProperty(
        "activeEditorObjectId", rootObjectId));
    QVERIFY(commentPanel->setProperty(
        "activeEditorSourceText",
        rootComment.value(QStringLiteral("text")).toString()));
    QVERIFY(commentPanel->setProperty("activeEditorText", oversizedText));
    QVERIFY(commentPanel->setProperty("activeEditorDirty", true));

    deleteResult.clear();
    QVERIFY(QMetaObject::invokeMethod(
        mainWindow.get(),
        "requestDeleteComment",
        Qt::DirectConnection,
        Q_RETURN_ARG(QVariant, deleteResult),
        Q_ARG(QVariant, 0),
        Q_ARG(QVariant, rootObjectId)));
    QVERIFY(!deleteResult.toBool());
    QCOMPARE(controller->commentItems().size(), 1);
    QCOMPARE(
        commentPanel->property("activeEditorText").toString(),
        oversizedText);
    QVERIFY(commentPanel->property("activeEditorDirty").toBool());

    const QString editedText = QStringLiteral("Committed before undo");
    QVERIFY(commentPanel->setProperty("activeEditorText", editedText));
    const auto [editUndoInvoked, editUndone]
        = invokeBooleanAction("requestUndo");
    QVERIFY(editUndoInvoked);
    QVERIFY(editUndone);
    QCOMPARE(controller->commentItems().size(), 1);
    QCOMPARE(
        controller->commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        rootComment.value(QStringLiteral("text")).toString());

    QVERIFY(mainWindow->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::editsSourceStickyNoteComment()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-sticky-note.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-sticky-note-output.pdf"));
    QVERIFY(createStickyNotePopupPdf(sourcePath));

    const CommentAnnotationInspection sourceInspection
        = inspectCommentAnnotation(
            sourcePath,
            QStringLiteral("preserved-popup-comment"));
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.annotationCount, 4);
    QCOMPARE(sourceInspection.subtype, FPDF_ANNOT_TEXT);
    QVERIFY(sourceInspection.richContents.contains(
        sourceInspection.contents));
    QVERIFY(sourceInspection.hasAppearance);
    QVERIFY(!sourceInspection.appearance.isEmpty());
    QVERIFY(sourceInspection.popupIndex >= 0);
    QVERIFY(sourceInspection.popupParentMatches);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const QVariantMap sourceComment = controller.commentItems().at(1).toMap();
    QCOMPARE(
        sourceComment.value(QStringLiteral("text")).toString(),
        sourceInspection.contents);
    QVERIFY(sourceComment.value(QStringLiteral("editable")).toBool());
    QVERIFY(sourceComment
        .value(QStringLiteral("commentTextEditable")).toBool());
    const QRectF sourceFrame = sourceComment
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF savedFrame(
        0.34,
        0.18,
        sourceFrame.width() * 1.5,
        sourceFrame.height() * 1.25);
    const QString objectId = sourceComment
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.selectComment(0, objectId));

    const QString draftText = QStringLiteral("Edited source sticky note");
    controller.setSelectedCommentText(draftText);
    QCOMPARE(controller.selectedText(), draftText);
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        draftText);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        sourceInspection.contents);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());

    QCOMPARE(controller.redo(), -1);
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        draftText);
    QVERIFY(controller.hasDrafts());

    controller.discardDrafts();
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        sourceInspection.contents);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const QString savedText = QStringLiteral("Saved source sticky note");
    QVERIFY(controller.selectComment(0, objectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(savedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), savedFrame);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), savedFrame);
    controller.setSelectedCommentText(savedText);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection outputInspection
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("preserved-popup-comment"));
    QVERIFY(outputInspection.readable);
    QCOMPARE(
        outputInspection.annotationCount,
        sourceInspection.annotationCount);
    QCOMPARE(outputInspection.subtype, sourceInspection.subtype);
    QCOMPARE(outputInspection.contents, savedText);
    QVERIFY(outputInspection.richContents.contains(savedText));
    QVERIFY(!outputInspection.richContents.contains(
        sourceInspection.contents));
    QCOMPARE(outputInspection.name, sourceInspection.name);
    QCOMPARE(outputInspection.author, sourceInspection.author);
    QCOMPARE(
        outputInspection.creationDate,
        sourceInspection.creationDate);
    QVERIFY(!outputInspection.modificationDate.isEmpty());
    QVERIFY(
        outputInspection.modificationDate
        != sourceInspection.modificationDate);
    QCOMPARE(
        outputInspection.hasAppearance,
        sourceInspection.hasAppearance);
    QCOMPARE(outputInspection.appearance, sourceInspection.appearance);
    QVERIFY(outputInspection.popupIndex >= 0);
    QVERIFY(outputInspection.popupParentMatches);
    QVERIFY(qAbs(outputInspection.rect.left
                 - savedFrame.left() * 200.0) < 0.05);
    QVERIFY(qAbs(outputInspection.rect.right
                 - savedFrame.right() * 200.0) < 0.05);
    QVERIFY(qAbs(outputInspection.rect.top
                 - (1.0 - savedFrame.top()) * 200.0) < 0.05);
    QVERIFY(qAbs(outputInspection.rect.bottom
                 - (1.0 - savedFrame.bottom()) * 200.0) < 0.05);
    const QByteArray outputDictionary = annotationDictionaryBytes(
        outputPath,
        QStringLiteral("preserved-popup-comment"));
    QVERIFY(!outputDictionary.contains(
        "TrePDFSourceCommentGeometryId"));
    const AnnotationAppearanceStructureInspection sourceAppearanceStructure
        = inspectAnnotationAppearanceStructure(
            sourcePath,
            QStringLiteral("preserved-popup-comment"));
    const AnnotationAppearanceStructureInspection outputAppearanceStructure
        = inspectAnnotationAppearanceStructure(
            outputPath,
            QStringLiteral("preserved-popup-comment"));
    QVERIFY(sourceAppearanceStructure.appearanceObjectFound);
    QVERIFY(outputAppearanceStructure.appearanceObjectFound);
    QCOMPARE(
        outputAppearanceStructure.boundingBox,
        sourceAppearanceStructure.boundingBox);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        savedText,
        5000);
    const QVariantMap rehydratedComment
        = controller.commentItems().at(1).toMap();
    QCOMPARE(
        rehydratedComment.value(QStringLiteral("text")).toString(),
        savedText);
    const QRectF rehydratedFrame = rehydratedComment
        .value(QStringLiteral("frameRect"))
        .toRectF();
    QVERIFY(qAbs(rehydratedFrame.x() - savedFrame.x()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.y() - savedFrame.y()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.width() - savedFrame.width()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.height() - savedFrame.height()) < 0.001);
    QVERIFY(rehydratedComment.value(QStringLiteral("editable")).toBool());
    QVERIFY(rehydratedComment
        .value(QStringLiteral("commentTextEditable")).toBool());
}

void PDFEditControllerTest::editsSourceTextMarkupComment_data()
{
    QTest::addColumn<QByteArray>("pdfSubtypeName");
    QTest::addColumn<int>("subtype");
    QTest::addColumn<QString>("annotationName");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QByteArray>("sourceText");
    QTest::addColumn<QString>("savedText");

    QTest::newRow("highlight")
        << QByteArray("Highlight")
        << static_cast<int>(FPDF_ANNOT_HIGHLIGHT)
        << QStringLiteral("editable-source-highlight")
        << QStringLiteral("highlight")
        << QByteArray("Source highlight comment")
        << QStringLiteral("Edited highlight comment");
    QTest::newRow("underline")
        << QByteArray("Underline")
        << static_cast<int>(FPDF_ANNOT_UNDERLINE)
        << QStringLiteral("editable-source-underline")
        << QStringLiteral("underline")
        << QByteArray("Source underline comment")
        << QStringLiteral("Edited underline comment");
    QTest::newRow("squiggly")
        << QByteArray("Squiggly")
        << static_cast<int>(FPDF_ANNOT_SQUIGGLY)
        << QStringLiteral("editable-source-squiggly")
        << QStringLiteral("squiggly")
        << QByteArray("Source squiggly comment")
        << QStringLiteral("Edited squiggly comment");
    QTest::newRow("strikeout")
        << QByteArray("StrikeOut")
        << static_cast<int>(FPDF_ANNOT_STRIKEOUT)
        << QStringLiteral("editable-source-strikeout")
        << QStringLiteral("strikeout")
        << QByteArray("Source strikeout comment")
        << QStringLiteral("Edited strikeout comment");
}

void PDFEditControllerTest::editsSourceTextMarkupComment()
{
    QFETCH(QByteArray, pdfSubtypeName);
    QFETCH(int, subtype);
    QFETCH(QString, annotationName);
    QFETCH(QString, kind);
    QFETCH(QByteArray, sourceText);
    QFETCH(QString, savedText);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-%1.pdf").arg(kind));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-%1-output.pdf").arg(kind));
    QVERIFY(createTextMarkupCommentPdf(
        sourcePath,
        pdfSubtypeName,
        annotationName.toLatin1(),
        sourceText));

    const CommentAnnotationInspection sourceInspection
        = inspectCommentAnnotation(
            sourcePath,
            annotationName);
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.annotationCount, 1);
    QCOMPARE(sourceInspection.subtype, subtype);
    QCOMPARE(sourceInspection.quadPoints.size(), 2);
    QVERIFY(sourceInspection.hasAppearance);
    QVERIFY(sourceInspection.appearance.contains(
        QStringLiteral("TrePDF source text markup appearance sentinel")));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    const QVariantMap sourceComment
        = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        sourceComment.value(QStringLiteral("kind")).toString(),
        kind);
    QCOMPARE(
        sourceComment
            .value(QStringLiteral("highlightColor"))
            .value<QColor>(),
        QColor(33, 69, 156, 255));
    QVERIFY(sourceComment.value(QStringLiteral("editable")).toBool());
    QVERIFY(sourceComment
        .value(QStringLiteral("commentTextEditable")).toBool());
    const QString objectId = sourceComment
        .value(QStringLiteral("objectId"))
        .toString();

    const QRectF sourceFrame = sourceComment
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF savedFrame(0.18, 0.22, 0.55, 0.18);
    QVERIFY(controller.selectComment(0, objectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(savedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), savedFrame);
    const QVariantList transformedPreviewRects
        = controller.selectedDraftPreview()
              .value(QStringLiteral("highlightRects"))
              .toList();
    QCOMPARE(transformedPreviewRects.size(), 2);
    QRectF transformedPreviewBounds;
    for (const QVariant &previewRectValue : transformedPreviewRects) {
        const QVariantMap previewRect = previewRectValue.toMap();
        const QRectF rect(
            previewRect.value(QStringLiteral("x")).toReal(),
            previewRect.value(QStringLiteral("y")).toReal(),
            previewRect.value(QStringLiteral("width")).toReal(),
            previewRect.value(QStringLiteral("height")).toReal());
        transformedPreviewBounds = transformedPreviewBounds.isEmpty()
            ? rect
            : transformedPreviewBounds.united(rect);
    }
    QVERIFY(qAbs(transformedPreviewBounds.x() - savedFrame.x()) < 0.001);
    QVERIFY(qAbs(transformedPreviewBounds.y() - savedFrame.y()) < 0.001);
    QVERIFY(qAbs(transformedPreviewBounds.width() - savedFrame.width()) < 0.001);
    QVERIFY(qAbs(transformedPreviewBounds.height() - savedFrame.height()) < 0.001);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedFrameRect(), savedFrame);

    QVERIFY(controller.setCommentText(0, objectId, savedText));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        savedText);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentAnnotationInspection outputInspection
        = inspectCommentAnnotation(
            outputPath,
            annotationName);
    QVERIFY(outputInspection.readable);
    QCOMPARE(
        outputInspection.annotationCount,
        sourceInspection.annotationCount);
    QCOMPARE(outputInspection.subtype, sourceInspection.subtype);
    QCOMPARE(outputInspection.contents, savedText);
    QCOMPARE(outputInspection.name, sourceInspection.name);
    QCOMPARE(outputInspection.author, sourceInspection.author);
    QCOMPARE(
        outputInspection.creationDate,
        sourceInspection.creationDate);
    QVERIFY(!outputInspection.modificationDate.isEmpty());
    QVERIFY(
        outputInspection.modificationDate
        != sourceInspection.modificationDate);
    QVERIFY(outputInspection.hasAppearance);
    QCOMPARE(outputInspection.appearance, sourceInspection.appearance);
    constexpr qreal pageWidth = 200.0;
    constexpr qreal pageHeight = 200.0;
    QVERIFY(qAbs(outputInspection.rect.left
                 - savedFrame.left() * pageWidth) < 0.05);
    QVERIFY(qAbs(outputInspection.rect.right
                 - savedFrame.right() * pageWidth) < 0.05);
    QVERIFY(qAbs(outputInspection.rect.top
                 - (1.0 - savedFrame.top()) * pageHeight) < 0.05);
    QVERIFY(qAbs(outputInspection.rect.bottom
                 - (1.0 - savedFrame.bottom()) * pageHeight) < 0.05);
    QCOMPARE(
        outputInspection.quadPoints.size(),
        sourceInspection.quadPoints.size());
    const auto transformedPagePoint = [
        &sourceFrame,
        &savedFrame,
        pageWidth,
        pageHeight](qreal pageX, qreal pageY) {
        const QPointF normalized(
            pageX / pageWidth,
            1.0 - pageY / pageHeight);
        const QPointF transformed(
            savedFrame.left()
                + (normalized.x() - sourceFrame.left())
                    * savedFrame.width() / sourceFrame.width(),
            savedFrame.top()
                + (normalized.y() - sourceFrame.top())
                    * savedFrame.height() / sourceFrame.height());
        return QPointF(
            transformed.x() * pageWidth,
            (1.0 - transformed.y()) * pageHeight);
    };
    for (qsizetype quadIndex = 0;
         quadIndex < sourceInspection.quadPoints.size();
         ++quadIndex) {
        const FS_QUADPOINTSF sourceQuad
            = sourceInspection.quadPoints.at(quadIndex);
        const FS_QUADPOINTSF outputQuad
            = outputInspection.quadPoints.at(quadIndex);
        const std::array<QPointF, 4> expectedPoints{
            transformedPagePoint(sourceQuad.x1, sourceQuad.y1),
            transformedPagePoint(sourceQuad.x2, sourceQuad.y2),
            transformedPagePoint(sourceQuad.x3, sourceQuad.y3),
            transformedPagePoint(sourceQuad.x4, sourceQuad.y4)};
        const std::array<QPointF, 4> outputPoints{
            QPointF(outputQuad.x1, outputQuad.y1),
            QPointF(outputQuad.x2, outputQuad.y2),
            QPointF(outputQuad.x3, outputQuad.y3),
            QPointF(outputQuad.x4, outputQuad.y4)};
        for (size_t pointIndex = 0;
             pointIndex < expectedPoints.size();
             ++pointIndex) {
            QVERIFY(qAbs(outputPoints.at(pointIndex).x()
                         - expectedPoints.at(pointIndex).x()) < 0.05);
            QVERIFY(qAbs(outputPoints.at(pointIndex).y()
                         - expectedPoints.at(pointIndex).y()) < 0.05);
        }
    }
    const QByteArray outputDictionary = annotationDictionaryBytes(
        outputPath,
        annotationName);
    QVERIFY(!outputDictionary.contains(
        "TrePDFSourceCommentGeometryId"));
    const AnnotationAppearanceStructureInspection sourceAppearanceStructure
        = inspectAnnotationAppearanceStructure(sourcePath, annotationName);
    const AnnotationAppearanceStructureInspection outputAppearanceStructure
        = inspectAnnotationAppearanceStructure(outputPath, annotationName);
    QCOMPARE(
        outputAppearanceStructure.boundingBox,
        sourceAppearanceStructure.boundingBox);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        savedText,
        5000);
    const QVariantMap rehydratedComment
        = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        rehydratedComment.value(QStringLiteral("text")).toString(),
        savedText);
    const QRectF rehydratedFrame = rehydratedComment
        .value(QStringLiteral("frameRect"))
        .toRectF();
    QVERIFY(qAbs(rehydratedFrame.x() - savedFrame.x()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.y() - savedFrame.y()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.width() - savedFrame.width()) < 0.001);
    QVERIFY(qAbs(rehydratedFrame.height() - savedFrame.height()) < 0.001);
    QVERIFY(rehydratedComment.value(QStringLiteral("editable")).toBool());
    QVERIFY(rehydratedComment
        .value(QStringLiteral("commentTextEditable")).toBool());
}

void PDFEditControllerTest::editsSourceReplyComment()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-reply.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-reply-output.pdf"));
    QVERIFY(createNestedCommentRepliesPdf(sourcePath));

    const CommentAnnotationInspection sourceReply
        = inspectCommentAnnotation(
            sourcePath,
            QStringLiteral("first-reply"));
    const CommentAnnotationInspection sourceNestedReply
        = inspectCommentAnnotation(
            sourcePath,
            QStringLiteral("nested-reply"));
    QVERIFY(sourceReply.readable);
    QCOMPARE(sourceReply.annotationCount, 3);
    QCOMPARE(sourceReply.subtype, FPDF_ANNOT_TEXT);
    QCOMPARE(sourceReply.parentName, QStringLiteral("root-comment"));
    QCOMPARE(sourceReply.relation, QStringLiteral("R"));
    QVERIFY(!sourceReply.hasAppearance);
    QCOMPARE(sourceNestedReply.parentName, QStringLiteral("first-reply"));
    QCOMPARE(sourceNestedReply.relation, QStringLiteral("R"));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);
    const QVariantMap reply = controller.commentItems().at(1).toMap();
    QVERIFY(reply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(!reply.value(QStringLiteral("editable")).toBool());
    QVERIFY(reply.value(QStringLiteral("commentTextEditable")).toBool());
    const QString replyObjectId = reply
        .value(QStringLiteral("objectId"))
        .toString();

    const QString savedText = QStringLiteral("Edited source reply");
    QVERIFY(controller.setCommentText(0, replyObjectId, savedText));
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        savedText);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentAnnotationInspection outputReply
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("first-reply"));
    const CommentAnnotationInspection outputNestedReply
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("nested-reply"));
    QVERIFY(outputReply.readable);
    QCOMPARE(outputReply.annotationCount, sourceReply.annotationCount);
    QCOMPARE(outputReply.subtype, sourceReply.subtype);
    QCOMPARE(outputReply.contents, savedText);
    QCOMPARE(outputReply.parentName, sourceReply.parentName);
    QCOMPARE(outputReply.relation, QStringLiteral("R"));
    QVERIFY(outputReply.parentIndex >= 0);
    QVERIFY(outputReply.hasAppearance);
    QVERIFY(!outputReply.appearance.isEmpty());
    QVERIFY(!outputReply.modificationDate.isEmpty());
    QVERIFY(qAbs(outputReply.rect.left - sourceReply.rect.left) < 0.001);
    QVERIFY(qAbs(outputReply.rect.right - sourceReply.rect.right) < 0.001);
    QVERIFY(qAbs(outputReply.rect.top - sourceReply.rect.top) < 0.001);
    QVERIFY(qAbs(outputReply.rect.bottom - sourceReply.rect.bottom) < 0.001);
    QCOMPARE(outputNestedReply.contents, sourceNestedReply.contents);
    QCOMPARE(outputNestedReply.parentName, sourceNestedReply.parentName);
    QCOMPARE(outputNestedReply.relation, QStringLiteral("R"));
    QVERIFY(outputNestedReply.parentIndex >= 0);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);
    QVariantMap rehydratedReply;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        for (const QVariant &commentValue : controller.commentItems()) {
            const QVariantMap comment = commentValue.toMap();
            if (comment.value(QStringLiteral("text")).toString()
                == savedText) {
                rehydratedReply = comment;
                return true;
            }
        }
        return false;
    }(), 5000);
    QVERIFY(!rehydratedReply.isEmpty());
    QVERIFY(rehydratedReply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(!rehydratedReply.value(QStringLiteral("editable")).toBool());
    QVERIFY(rehydratedReply
        .value(QStringLiteral("commentTextEditable")).toBool());
}

void PDFEditControllerTest::importsZeroRectUnderlineReply()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("zero-rect-underline-reply.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("zero-rect-underline-reply-output.pdf"));
    const QString deletedOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("zero-rect-underline-reply-deleted.pdf"));
    QVERIFY(createZeroRectUnderlineReplyPdf(sourcePath));

    const CommentAnnotationInspection sourceReply
        = inspectCommentAnnotation(
            sourcePath,
            QStringLiteral("foxit-underline-reply"));
    QVERIFY(sourceReply.readable);
    QCOMPARE(sourceReply.annotationCount, 3);
    QCOMPARE(sourceReply.subtype, FPDF_ANNOT_TEXT);
    QCOMPARE(
        sourceReply.parentName,
        QStringLiteral("foxit-underline-root"));
    QCOMPARE(sourceReply.relation, QStringLiteral("R"));
    QCOMPARE(sourceReply.rect.left, 0.0f);
    QCOMPARE(sourceReply.rect.right, 0.0f);
    QCOMPARE(sourceReply.rect.top, 0.0f);
    QCOMPARE(sourceReply.rect.bottom, 0.0f);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const QVariantMap root = controller.commentItems().at(0).toMap();
    const QVariantMap reply = controller.commentItems().at(1).toMap();
    QCOMPARE(
        root.value(QStringLiteral("kind")).toString(),
        QStringLiteral("underline"));
    QCOMPARE(
        root.value(QStringLiteral("text")).toString(),
        QStringLiteral("Root underline"));
    QCOMPARE(root.value(QStringLiteral("replyCount")).toInt(), 1);
    QVERIFY(reply.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        reply.value(QStringLiteral("text")).toString(),
        QStringLiteral("Foxit underline reply"));
    QCOMPARE(
        reply.value(QStringLiteral("parentObjectId")).toString(),
        root.value(QStringLiteral("objectId")).toString());
    QVERIFY(reply
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .isEmpty());

    const QString replyObjectId = reply
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.selectComment(0, replyObjectId));
    QVERIFY(controller.selectedCommentReply());
    QVERIFY(controller.removeComment(0, replyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 2);

    const QString savedText
        = QStringLiteral("Edited Foxit underline reply");
    QVERIFY(controller.setCommentText(0, replyObjectId, savedText));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentAnnotationInspection outputReply
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("foxit-underline-reply"));
    QVERIFY(outputReply.readable);
    QCOMPARE(outputReply.annotationCount, sourceReply.annotationCount);
    QCOMPARE(outputReply.subtype, sourceReply.subtype);
    QCOMPARE(outputReply.contents, savedText);
    QCOMPARE(outputReply.parentName, sourceReply.parentName);
    QCOMPARE(outputReply.relation, QStringLiteral("R"));
    QCOMPARE(outputReply.rect.left, sourceReply.rect.left);
    QCOMPARE(outputReply.rect.right, sourceReply.rect.right);
    QCOMPARE(outputReply.rect.top, sourceReply.rect.top);
    QCOMPARE(outputReply.rect.bottom, sourceReply.rect.bottom);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    QVariantMap rehydratedReply;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("text")).toString()
            == savedText) {
            rehydratedReply = comment;
            break;
        }
    }
    QVERIFY(!rehydratedReply.isEmpty());
    QVERIFY(rehydratedReply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(rehydratedReply
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .isEmpty());

    const QString rehydratedReplyObjectId = rehydratedReply
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.removeComment(0, rehydratedReplyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(deletedOutputPath)));

    const CommentAnnotationInspection deletedReply
        = inspectCommentAnnotation(
            deletedOutputPath,
            QStringLiteral("foxit-underline-reply"));
    QVERIFY(deletedReply.readable);
    QCOMPARE(deletedReply.annotationCount, 2);
    QCOMPARE(deletedReply.annotationIndex, -1);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("Root underline"));
}

void PDFEditControllerTest::rejectsEditingProtectedSourceComments()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("protected-source-comments.pdf"));
    QVERIFY(createProtectedCommentTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const QVariantList comments = controller.commentItems();
    for (const QVariant &commentValue : comments) {
        const QVariantMap comment = commentValue.toMap();
        QVERIFY(!comment.value(QStringLiteral("editable")).toBool());
        QVERIFY(!comment
            .value(QStringLiteral("commentTextEditable")).toBool());
        QVERIFY(!comment.value(QStringLiteral("deletable")).toBool());
        QVERIFY(controller.selectComment(
            comment.value(QStringLiteral("page")).toInt(),
            comment.value(QStringLiteral("objectId")).toString()));
        QVERIFY(!controller.selectedDeletable());
        controller.deleteSelectedObject();
        QVERIFY(!controller.setCommentText(
            comment.value(QStringLiteral("page")).toInt(),
            comment.value(QStringLiteral("objectId")).toString(),
            QStringLiteral("Rejected edit")));
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
        QVERIFY(!controller.canRedo());
    }

    QCOMPARE(controller.commentItems(), comments);
}

void PDFEditControllerTest::protectsLockedSourceCommentFromDeletion()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("locked-source-comment.pdf"));
    QVERIFY(createLockedCommentTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap comment = controller.commentItems().constFirst().toMap();
    QVERIFY(!comment.value(QStringLiteral("editable")).toBool());
    QVERIFY(comment
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    QVERIFY(!comment.value(QStringLiteral("deletable")).toBool());

    const int page = comment.value(QStringLiteral("page")).toInt();
    const QString objectId = comment
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.selectComment(page, objectId));
    QVERIFY(!controller.selectedEditable());
    QVERIFY(!controller.selectedDeletable());
    controller.deleteSelectedObject();
    QVERIFY(!controller.removeComment(page, objectId));
    QVERIFY(!controller.hasDrafts());

    QVERIFY(controller.setCommentText(
        page,
        objectId,
        QStringLiteral("Editable locked contents")));
    QVERIFY(controller.hasDrafts());
}

void PDFEditControllerTest::protectsLockedReplyAncestorsFromDeletion()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("locked-source-comment-thread.pdf"));
    QVERIFY(createLockedCommentReplyThreadPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);

    const QVariantList comments = controller.commentItems();
    for (const QVariant &commentValue : comments) {
        const QVariantMap comment = commentValue.toMap();
        QVERIFY(comment
            .value(QStringLiteral("commentTextEditable"))
            .toBool());
        QVERIFY(!comment.value(QStringLiteral("deletable")).toBool());
        const int page = comment.value(QStringLiteral("page")).toInt();
        const QString objectId = comment
            .value(QStringLiteral("objectId"))
            .toString();
        QVERIFY(controller.selectComment(page, objectId));
        QVERIFY(!controller.selectedDeletable());
        QVERIFY(!controller.removeComment(page, objectId));
    }

    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::protectsSourceFreeTextComments()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("protected-source-free-text.pdf"));
    QVERIFY(createProtectedFreeTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const QVariantList comments = controller.commentItems();
    for (const QVariant &commentValue : comments) {
        const QVariantMap comment = commentValue.toMap();
        QCOMPARE(
            comment.value(QStringLiteral("kind")).toString(),
            QStringLiteral("freeText"));
        QVERIFY(!comment.value(QStringLiteral("editable")).toBool());
        QVERIFY(!comment
            .value(QStringLiteral("commentTextEditable"))
            .toBool());
        const int page = comment.value(QStringLiteral("page")).toInt();
        const QString objectId = comment
            .value(QStringLiteral("objectId"))
            .toString();
        QVERIFY(!controller.setCommentText(
            page,
            objectId,
            QStringLiteral("Rejected Free Text edit")));
        QVERIFY(!controller.removeComment(page, objectId));
    }

    QCOMPARE(controller.commentItems(), comments);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::enforcesCommentTextLengthLimit()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-text-length-limit.pdf"));
    QVERIFY(createRootCommentOnlyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap root = controller.commentItems().constFirst().toMap();
    const QString rootObjectId = root
        .value(QStringLiteral("objectId"))
        .toString();
    const QString sourceText = root
        .value(QStringLiteral("text"))
        .toString();
    const QString acceptedText
        = QStringLiteral("\U0001F642").repeated(350);
    const QString rejectedText = acceptedText + QLatin1Char('x');
    QCOMPARE(acceptedText.size(), 700);
    QCOMPARE(rejectedText.size(), 701);

    QVERIFY(controller.setCommentText(0, rootObjectId, acceptedText));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        acceptedText);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    controller.discardDrafts();
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        sourceText);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(!controller.setCommentText(0, rootObjectId, rejectedText));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        sourceText);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(controller.setCommentText(0, rootObjectId, QString()));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QString());
    QVERIFY(controller.hasDrafts());
    controller.discardDrafts();

    const QVariantMap acceptedReply = controller.addCommentReply(
        0,
        rootObjectId,
        acceptedText);
    QVERIFY(!acceptedReply.isEmpty());
    QCOMPARE(
        acceptedReply.value(QStringLiteral("text")).toString(),
        acceptedText);
    QCOMPARE(controller.commentItems().size(), 2);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    controller.discardDrafts();
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(controller.addCommentReply(
        0,
        rootObjectId,
        rejectedText).isEmpty());
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        sourceText);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(controller.addCommentReply(
        0,
        rootObjectId,
        QString()).isEmpty());
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::boundsSharedAnnotationStringReads()
{
    PDFiumProcessLock pdfiumLock;
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    QVERIFY(document);
    const auto closeDocument = qScopeGuard([document]() {
        FPDF_CloseDocument(document);
    });

    FPDF_PAGE page = FPDFPage_New(document, 0, 200.0, 200.0);
    QVERIFY(page);
    const auto closePage = qScopeGuard([page]() {
        FPDF_ClosePage(page);
    });

    FPDF_ANNOTATION annotation = FPDFPage_CreateAnnot(
        page,
        FPDF_ANNOT_TEXT);
    QVERIFY(annotation);
    const auto closeAnnotation = qScopeGuard([annotation]() {
        FPDFPage_CloseAnnot(annotation);
    });

    constexpr unsigned long maximumBytes = 4UL * 1024UL * 1024UL;
    constexpr qsizetype maximumCharacters
        = maximumBytes / sizeof(FPDF_WCHAR) - 1;
    QString contents(maximumCharacters, QLatin1Char('a'));
    QVERIFY(FPDFAnnot_SetStringValue(
        annotation,
        "Contents",
        reinterpret_cast<const FPDF_WCHAR *>(contents.utf16())));
    QCOMPARE(
        FPDFAnnot_GetStringValue(annotation, "Contents", nullptr, 0),
        maximumBytes);
    QString boundedContents = PDFAnnotationUtils::annotationString(
        annotation,
        "Contents");
    QCOMPARE(boundedContents.size(), maximumCharacters);
    QVERIFY(boundedContents == contents);

    boundedContents.clear();
    contents.append(QLatin1Char('b'));
    QVERIFY(FPDFAnnot_SetStringValue(
        annotation,
        "Contents",
        reinterpret_cast<const FPDF_WCHAR *>(contents.utf16())));
    QCOMPARE(
        FPDFAnnot_GetStringValue(annotation, "Contents", nullptr, 0),
        maximumBytes + sizeof(FPDF_WCHAR));
    QVERIFY(PDFAnnotationUtils::annotationString(
        annotation,
        "Contents").isEmpty());
}

void PDFEditControllerTest::indexesCommentsAcrossDocument()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.15, 0.2)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("First comment"));
    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(2, QPointF(0.25, 0.3)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Third-page comment"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    const QVariantList comments = controller.commentItems();
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("page")).toInt(),
        0);
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("First comment"));
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("page")).toInt(),
        2);
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Third-page comment"));

    QCOMPARE(controller.selectRelativeComment(1), 0);
    QCOMPARE(controller.selectRelativeComment(1), 2);
    QCOMPARE(controller.selectRelativeComment(-1), 0);
}

void PDFEditControllerTest::reindexesCommentsAfterDiscardingPageDeletion()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-page-discard-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-page-discard-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    connect(
        &controller,
        &PDFEditController::commentsChanged,
        &controller,
        &PDFEditController::requestCommentIndex);

    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.15, 0.2)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("First-page comment"));
    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(1, QPointF(0.25, 0.3)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Second-page comment"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    QCOMPARE(
        controller.commentItems().at(0).toMap()
            .value(QStringLiteral("objectId")).toString(),
        QStringLiteral("annotation:0"));
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("objectId")).toString(),
        QStringLiteral("annotation:0"));

    document.setCurrentPage(0);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("Second-page comment"));

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    const QVariantList comments = controller.commentItems();
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("page")).toInt(),
        0);
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("First-page comment"));
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("page")).toInt(),
        1);
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Second-page comment"));
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("objectId")).toString(),
        comments.at(1).toMap().value(QStringLiteral("objectId")).toString());
    QVERIFY(controller.setCommentText(
        1,
        comments.at(1).toMap()
            .value(QStringLiteral("objectId")).toString(),
        QStringLiteral("Second-page identity edit")));
    QCOMPARE(
        controller.commentItems().at(0).toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("First-page comment"));
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("Second-page identity edit"));
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("Second-page comment"));
    QVERIFY(controller.removeComment(
        1,
        comments.at(1).toMap()
            .value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("First-page comment"));
}
