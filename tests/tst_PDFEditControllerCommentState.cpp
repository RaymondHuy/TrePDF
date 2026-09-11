#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::showsCommentStatusActionsMenuUi()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-status-actions-ui.pdf"));
    QVERIFY(createRootCommentOnlyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

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
        {QStringLiteral("editController"), QVariant::fromValue(&controller)},
        {QStringLiteral("width"), 320.0},
        {QStringLiteral("height"), 640.0}};
    std::unique_ptr<QObject> panel(
        panelComponent.createWithInitialProperties(initialProperties));
    QVERIFY2(panel, qPrintable(panelComponent.errorString()));
    auto *panelItem = qobject_cast<QQuickItem *>(panel.get());
    QVERIFY(panelItem);

    QQuickWindow window;
    window.resize(360, 680);
    panelItem->setParentItem(window.contentItem());
    window.show();

    const auto findQuickItem = [](
                                   auto &&self,
                                   QQuickItem *item,
                                   const QString &objectName) -> QQuickItem * {
        if (!item) {
            return nullptr;
        }
        if (item->objectName() == objectName) {
            return item;
        }
        for (QQuickItem *child : item->childItems()) {
            if (QQuickItem *match = self(self, child, objectName)) {
                return match;
            }
        }
        return nullptr;
    };
    const auto findQuickItemForComment = [](
                                             auto &&self,
                                             QQuickItem *item,
                                             const QString &objectName,
                                             const QString &objectId)
        -> QQuickItem * {
        if (!item) {
            return nullptr;
        }
        if (item->objectName() == objectName) {
            for (QQuickItem *ancestor = item;
                 ancestor;
                 ancestor = ancestor->parentItem()) {
                if (ancestor->property("objectId").toString()
                    == objectId) {
                    return item;
                }
            }
        }
        for (QQuickItem *child : item->childItems()) {
            if (QQuickItem *match = self(
                    self,
                    child,
                    objectName,
                    objectId)) {
                return match;
            }
        }
        return nullptr;
    };

    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentIndexState(),
        PDFEditController::CommentIndexReady,
        5000);
    QCOMPARE(controller.commentItems().size(), 1);
    const QVariantMap rootComment
        = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        rootComment.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::NoCommentStatus));
    QVERIFY(!rootComment.value(QStringLiteral("commentMarked")).toBool());
    QVERIFY(rootComment
        .value(QStringLiteral("commentStateEditable"))
        .toBool());

    const auto findBadge = [&](const QString &objectName) {
        return findQuickItem(
            findQuickItem,
            window.contentItem(),
            objectName);
    };
    QTRY_VERIFY_WITH_TIMEOUT(
        findBadge(QStringLiteral("commentStatusBadge")) != nullptr,
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        findBadge(QStringLiteral("commentMarkedBadge")) != nullptr,
        1000);
    QVERIFY(!findBadge(
        QStringLiteral("commentStatusBadge"))->property("visible").toBool());
    QVERIFY(!findBadge(
        QStringLiteral("commentMarkedBadge"))->property("visible").toBool());

    const int rootPage = rootComment.value(QStringLiteral("page")).toInt();
    const QString rootObjectId = rootComment
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.setCommentStatus(
        rootPage,
        rootObjectId,
        PDFEditController::CancelledCommentStatus));
    QString cancelledBadgeText;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            QQuickItem *badge = findBadge(
                QStringLiteral("commentStatusBadge"));
            if (!badge || !badge->property("visible").toBool()) {
                return false;
            }
            cancelledBadgeText = badge->property("text").toString();
            return !cancelledBadgeText.isEmpty();
        })(),
        1000);
    QVERIFY(controller.setCommentMarked(rootPage, rootObjectId, true));
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            QQuickItem *badge = findBadge(
                QStringLiteral("commentMarkedBadge"));
            return badge
                && badge->property("visible").toBool()
                && badge->property("text").toString().contains(
                    QChar(0x2713));
        })(),
        1000);

    controller.discardDrafts();
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            QQuickItem *badge = findBadge(
                QStringLiteral("commentStatusBadge"));
            return badge && !badge->property("visible").toBool();
        })(),
        1000);
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            QQuickItem *badge = findBadge(
                QStringLiteral("commentMarkedBadge"));
            return badge && !badge->property("visible").toBool();
        })(),
        1000);

    QQuickItem *actionsButton = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            actionsButton = findQuickItem(
                findQuickItem,
                window.contentItem(),
                QStringLiteral("commentActionsButton"));
            return actionsButton != nullptr;
        })(),
        1000);
    QVERIFY(actionsButton->property("visible").toBool());
    QVERIFY(actionsButton->property("enabled").toBool());
    QVERIFY(!actionsButton->property("text").toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(
        actionsButton,
        "clicked",
        Qt::DirectConnection));

    QObject *menuLoader = panel->findChild<QObject *>(
        QStringLiteral("commentActionsMenuLoader"));
    QVERIFY(menuLoader);
    QObject *actionsMenu = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            actionsMenu = menuLoader->findChild<QObject *>(
                QStringLiteral("commentActionsMenu"));
            return actionsMenu != nullptr
                && actionsMenu->property("opened").toBool();
        })(),
        1000);
    QCOMPARE(actionsMenu->property("count").toInt(), 2);
    QVERIFY(actionsMenu->property("cascade").toBool());

    QObject *statusMenu = actionsMenu->findChild<QObject *>(
        QStringLiteral("setCommentStatusMenu"));
    QObject *markAction = actionsMenu->findChild<QObject *>(
        QStringLiteral("markCommentWithCheckmarkAction"));
    QVERIFY(statusMenu);
    QVERIFY(markAction);
    QCOMPARE(statusMenu->property("count").toInt(), 7);
    QVERIFY(statusMenu->property("enabled").toBool());
    QVERIFY(!statusMenu->property("title").toString().isEmpty());
    QVERIFY(markAction->property("enabled").toBool());
    QVERIFY(markAction->property("checkable").toBool());
    QVERIFY(!markAction->property("checked").toBool());
    QVERIFY(!markAction->property("text").toString().isEmpty());

    const QStringList statusActionNames{
        QStringLiteral("commentStatusNoneAction"),
        QStringLiteral("commentStatusAcceptedAction"),
        QStringLiteral("commentStatusRejectedAction"),
        QStringLiteral("commentStatusCancelledAction"),
        QStringLiteral("commentStatusCompletedAction"),
        QStringLiteral("commentStatusDeferredAction"),
        QStringLiteral("commentStatusFutureAction")};
    int checkedStatusCount = 0;
    QString cancelledActionText;
    QString completedActionText;
    for (const QString &actionName : statusActionNames) {
        QObject *action = actionsMenu->findChild<QObject *>(actionName);
        QVERIFY2(action, qPrintable(actionName));
        QVERIFY2(action->property("enabled").toBool(), qPrintable(actionName));
        QVERIFY2(action->property("checkable").toBool(), qPrintable(actionName));
        QVERIFY2(
            !action->property("text").toString().isEmpty(),
            qPrintable(actionName));
        if (action->property("checked").toBool()) {
            ++checkedStatusCount;
            QCOMPARE(
                actionName,
                QStringLiteral("commentStatusNoneAction"));
        }
        if (actionName == QLatin1String("commentStatusCancelledAction")) {
            cancelledActionText = action->property("text").toString();
        } else if (actionName
                   == QLatin1String("commentStatusCompletedAction")) {
            completedActionText = action->property("text").toString();
        }
    }
    QCOMPARE(checkedStatusCount, 1);
    QCOMPARE(cancelledBadgeText, cancelledActionText);
    QVERIFY(cancelledBadgeText != completedActionText);

    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(QMetaObject::invokeMethod(
        actionsMenu,
        "dismiss",
        Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(
        menuLoader->findChild<QObject *>(
            QStringLiteral("commentActionsMenu")) == nullptr,
        1000);
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.commentItems().size(), 1);

    const QVariantMap reply = controller.addCommentReply(
        rootPage,
        rootObjectId,
        QStringLiteral("Reply status UI"));
    QVERIFY(!reply.isEmpty());
    const QString replyObjectId
        = reply.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.setCommentStatus(
        rootPage,
        replyObjectId,
        PDFEditController::AcceptedCommentStatus));
    QVERIFY(controller.setCommentMarked(rootPage, replyObjectId, true));
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(),
        "setThreadRepliesExpanded",
        Qt::DirectConnection,
        Q_ARG(QVariant, rootPage),
        Q_ARG(QVariant, rootObjectId),
        Q_ARG(QVariant, true)));
    QTRY_COMPARE_WITH_TIMEOUT(
        panel->property("displayedCommentItems").toList().size(),
        2,
        1000);

    const auto replyItem = [&](const QString &objectName) {
        return findQuickItemForComment(
            findQuickItemForComment,
            window.contentItem(),
            objectName,
            replyObjectId);
    };
    QQuickItem *replyStatusBadge = nullptr;
    QQuickItem *replyMarkedBadge = nullptr;
    QQuickItem *replyActionsButton = nullptr;
    QQuickItem *nestedReplyButton = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            replyStatusBadge = replyItem(
                QStringLiteral("commentStatusBadge"));
            replyMarkedBadge = replyItem(
                QStringLiteral("commentMarkedBadge"));
            replyActionsButton = replyItem(
                QStringLiteral("commentActionsButton"));
            nestedReplyButton = replyItem(QStringLiteral("replyButton"));
            return replyStatusBadge
                && replyMarkedBadge
                && replyActionsButton
                && nestedReplyButton;
        })(),
        1000);
    QVERIFY(replyStatusBadge->property("visible").toBool());
    QVERIFY(replyMarkedBadge->property("visible").toBool());
    QVERIFY(replyActionsButton->property("visible").toBool());
    QVERIFY(replyActionsButton->property("enabled").toBool());
    QVERIFY(!nestedReplyButton->property("visible").toBool());

    QVERIFY(QMetaObject::invokeMethod(
        replyActionsButton,
        "clicked",
        Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&]() {
            actionsMenu = menuLoader->findChild<QObject *>(
                QStringLiteral("commentActionsMenu"));
            return actionsMenu
                && actionsMenu->property("opened").toBool();
        })(),
        1000);
    QCOMPARE(
        panel->property("commentActionsObjectId").toString(),
        replyObjectId);
    QCOMPARE(
        panel->property("commentActionsStatus").toInt(),
        static_cast<int>(PDFEditController::AcceptedCommentStatus));
    QVERIFY(panel->property("commentActionsMarked").toBool());
    QObject *acceptedAction = actionsMenu->findChild<QObject *>(
        QStringLiteral("commentStatusAcceptedAction"));
    markAction = actionsMenu->findChild<QObject *>(
        QStringLiteral("markCommentWithCheckmarkAction"));
    QVERIFY(acceptedAction);
    QVERIFY(markAction);
    QVERIFY(acceptedAction->property("checked").toBool());
    QVERIFY(markAction->property("checked").toBool());

    QVERIFY(QMetaObject::invokeMethod(
        actionsMenu,
        "dismiss",
        Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(
        menuLoader->findChild<QObject *>(
            QStringLiteral("commentActionsMenu")) == nullptr,
        1000);
    controller.discardDrafts();
    QCOMPARE(controller.commentItems().size(), 1);
}

void PDFEditControllerTest::editsSourceCommentStateWithUndoRedoAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-comment-state.pdf"));
    QVERIFY(createRootCommentOnlyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap sourceComment
        = controller.commentItems().constFirst().toMap();
    const int page = sourceComment.value(QStringLiteral("page")).toInt();
    const QString objectId = sourceComment
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!objectId.isEmpty());

    const auto verifyState = [&](PDFEditController::CommentStatus status,
                                 bool marked) {
        QCOMPARE(controller.commentItems().size(), 1);
        const QVariantMap comment
            = controller.commentItems().constFirst().toMap();
        QCOMPARE(comment.value(QStringLiteral("objectId")).toString(), objectId);
        QCOMPARE(
            comment.value(QStringLiteral("commentStatus")).toInt(),
            static_cast<int>(status));
        QCOMPARE(
            comment.value(QStringLiteral("commentMarked")).toBool(),
            marked);
        QVERIFY(comment
            .value(QStringLiteral("commentStateEditable"))
            .toBool());
    };

    verifyState(PDFEditController::NoCommentStatus, false);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(controller.setCommentStatus(
        page,
        objectId,
        PDFEditController::AcceptedCommentStatus));
    verifyState(PDFEditController::AcceptedCommentStatus, false);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());

    QVERIFY(controller.setCommentMarked(page, objectId, true));
    verifyState(PDFEditController::AcceptedCommentStatus, true);
    QCOMPARE(controller.undo(), -1);
    verifyState(PDFEditController::AcceptedCommentStatus, false);
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.undo(), -1);
    verifyState(PDFEditController::NoCommentStatus, false);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());

    QCOMPARE(controller.redo(), -1);
    verifyState(PDFEditController::AcceptedCommentStatus, false);
    QCOMPARE(controller.redo(), -1);
    verifyState(PDFEditController::AcceptedCommentStatus, true);

    const std::array<PDFEditController::CommentStatus, 5> transitions{
        PDFEditController::RejectedCommentStatus,
        PDFEditController::CancelledCommentStatus,
        PDFEditController::CompletedCommentStatus,
        PDFEditController::DeferredCommentStatus,
        PDFEditController::FutureCommentStatus};
    for (PDFEditController::CommentStatus status : transitions) {
        QVERIFY(controller.setCommentStatus(page, objectId, status));
        verifyState(status, true);
    }

    QCOMPARE(controller.undo(), -1);
    verifyState(PDFEditController::DeferredCommentStatus, true);
    QCOMPARE(controller.undo(), -1);
    verifyState(PDFEditController::CompletedCommentStatus, true);
    QCOMPARE(controller.redo(), -1);
    verifyState(PDFEditController::DeferredCommentStatus, true);
    QCOMPARE(controller.redo(), -1);
    verifyState(PDFEditController::FutureCommentStatus, true);

    controller.discardDrafts();
    verifyState(PDFEditController::NoCommentStatus, false);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(controller.setCommentStatus(
        page,
        objectId,
        PDFEditController::NoCommentStatus));
    QVERIFY(controller.setCommentMarked(page, objectId, false));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.setCommentStatus(
        page,
        objectId,
        static_cast<PDFEditController::CommentStatus>(-1)));
    QVERIFY(!controller.setCommentStatus(
        page,
        objectId,
        static_cast<PDFEditController::CommentStatus>(999)));
    QVERIFY(!controller.setCommentStatus(
        -1,
        objectId,
        PDFEditController::AcceptedCommentStatus));
    QVERIFY(!controller.setCommentMarked(
        page,
        QStringLiteral("missing-comment"),
        true));
    verifyState(PDFEditController::NoCommentStatus, false);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::savesAndRehydratesCommentStateTransitions()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-state-roundtrip-source.pdf"));
    const QString firstOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-state-roundtrip-first.pdf"));
    const QString secondOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-state-roundtrip-second.pdf"));
    const QString thirdOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-state-roundtrip-third.pdf"));
    QVERIFY(createRootCommentOnlyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const auto currentRoot = [&]() {
        return controller.commentItems().constFirst().toMap();
    };
    const auto transitionsForModel = [](
                                         const CommentStateInspection &inspection,
                                         const QString &stateModel) {
        QVector<CommentStateTransitionInspection> result;
        for (const CommentStateTransitionInspection &transition
             : inspection.transitions) {
            if (transition.stateModel == stateModel) {
                result.append(transition);
            }
        }
        return result;
    };
    const auto verifyCurrentState = [&](PDFEditController::CommentStatus status,
                                        bool marked) {
        const QVariantMap root = currentRoot();
        QCOMPARE(
            root.value(QStringLiteral("commentStatus")).toInt(),
            static_cast<int>(status));
        QCOMPARE(
            root.value(QStringLiteral("commentMarked")).toBool(),
            marked);
        QVERIFY(root
            .value(QStringLiteral("commentStateEditable"))
            .toBool());
        QCOMPARE(root.value(QStringLiteral("replyCount")).toInt(), 0);
    };

    QString objectId = currentRoot()
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.setCommentStatus(
        0,
        objectId,
        PDFEditController::AcceptedCommentStatus));
    QVERIFY(controller.setCommentMarked(0, objectId, true));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(firstOutputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentStateInspection firstInspection
        = inspectCommentStateTransitions(firstOutputPath);
    QVERIFY(firstInspection.readable);
    QCOMPARE(firstInspection.annotationCount, 3);
    QCOMPARE(firstInspection.transitions.size(), 2);
    const QVector<CommentStateTransitionInspection> firstReviews
        = transitionsForModel(firstInspection, QStringLiteral("Review"));
    const QVector<CommentStateTransitionInspection> firstMarks
        = transitionsForModel(firstInspection, QStringLiteral("Marked"));
    QCOMPARE(firstReviews.size(), 1);
    QCOMPARE(firstMarks.size(), 1);
    QCOMPARE(firstReviews.constFirst().state, QStringLiteral("Accepted"));
    QCOMPARE(firstMarks.constFirst().state, QStringLiteral("Marked"));
    QCOMPARE(firstReviews.constFirst().parentName, QStringLiteral("root-comment"));
    QCOMPARE(firstMarks.constFirst().parentName, QStringLiteral("root-comment"));
    QVERIFY(firstReviews.constFirst().relation.isEmpty());
    QVERIFY(firstMarks.constFirst().relation.isEmpty());
    QVERIFY(!firstReviews.constFirst().name.isEmpty());
    QVERIFY(!firstMarks.constFirst().name.isEmpty());
    QVERIFY(!firstReviews.constFirst().author.isEmpty());
    QCOMPARE(firstReviews.constFirst().author, firstMarks.constFirst().author);
    QVERIFY(!firstReviews.constFirst().modificationDate.isEmpty());
    QVERIFY(!firstMarks.constFirst().modificationDate.isEmpty());

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    verifyCurrentState(PDFEditController::AcceptedCommentStatus, true);
    objectId = currentRoot().value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.setCommentStatus(
        0,
        objectId,
        PDFEditController::RejectedCommentStatus));
    QVERIFY(controller.setCommentMarked(0, objectId, false));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(secondOutputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentStateInspection preservedFirst
        = inspectCommentStateTransitions(firstOutputPath);
    QCOMPARE(preservedFirst.annotationCount, 3);
    QCOMPARE(preservedFirst.transitions.size(), 2);

    const CommentStateInspection secondInspection
        = inspectCommentStateTransitions(secondOutputPath);
    QVERIFY(secondInspection.readable);
    QCOMPARE(secondInspection.annotationCount, 5);
    QCOMPARE(secondInspection.transitions.size(), 4);
    const QVector<CommentStateTransitionInspection> secondReviews
        = transitionsForModel(secondInspection, QStringLiteral("Review"));
    const QVector<CommentStateTransitionInspection> secondMarks
        = transitionsForModel(secondInspection, QStringLiteral("Marked"));
    QCOMPARE(secondReviews.size(), 2);
    QCOMPARE(secondMarks.size(), 2);
    QCOMPARE(secondReviews.at(0).state, QStringLiteral("Accepted"));
    QCOMPARE(secondReviews.at(1).state, QStringLiteral("Rejected"));
    QCOMPARE(secondReviews.at(1).parentName, secondReviews.at(0).name);
    QVERIFY(secondReviews.at(1).relation.isEmpty());
    QCOMPARE(secondMarks.at(0).state, QStringLiteral("Marked"));
    QCOMPARE(secondMarks.at(1).state, QStringLiteral("Unmarked"));
    QCOMPARE(secondMarks.at(1).parentName, secondMarks.at(0).name);
    QVERIFY(secondMarks.at(1).relation.isEmpty());

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    verifyCurrentState(PDFEditController::RejectedCommentStatus, false);
    objectId = currentRoot().value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.setCommentStatus(
        0,
        objectId,
        PDFEditController::NoCommentStatus));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(thirdOutputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentStateInspection thirdInspection
        = inspectCommentStateTransitions(thirdOutputPath);
    QVERIFY(thirdInspection.readable);
    QCOMPARE(thirdInspection.annotationCount, 6);
    QCOMPARE(thirdInspection.transitions.size(), 5);
    const QVector<CommentStateTransitionInspection> thirdReviews
        = transitionsForModel(thirdInspection, QStringLiteral("Review"));
    const QVector<CommentStateTransitionInspection> thirdMarks
        = transitionsForModel(thirdInspection, QStringLiteral("Marked"));
    QCOMPARE(thirdReviews.size(), 3);
    QCOMPARE(thirdMarks.size(), 2);
    QCOMPARE(thirdReviews.at(0).state, QStringLiteral("Accepted"));
    QCOMPARE(thirdReviews.at(1).state, QStringLiteral("Rejected"));
    QCOMPARE(thirdReviews.at(2).state, QStringLiteral("None"));
    QCOMPARE(thirdReviews.at(2).parentName, thirdReviews.at(1).name);
    QVERIFY(thirdReviews.at(2).relation.isEmpty());
    QCOMPARE(thirdMarks.at(1).state, QStringLiteral("Unmarked"));

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    verifyCurrentState(PDFEditController::NoCommentStatus, false);
}

void PDFEditControllerTest::importsAndSavesReplyCommentStateIndependently()
{
    const bool userWasSet = qEnvironmentVariableIsSet("USER");
    const QByteArray originalUser = qgetenv("USER");
    const auto restoreUser = qScopeGuard([userWasSet, originalUser]() {
        if (userWasSet) {
            qputenv("USER", originalUser);
        } else {
            qunsetenv("USER");
        }
    });
    QVERIFY(qputenv("USER", "trepdfreviewer"));

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("reply-comment-state-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("reply-comment-state-output.pdf"));
    QVERIFY(createReplyCommentStateHistoryPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const auto currentComments = [&]() {
        QPair<QVariantMap, QVariantMap> result;
        for (const QVariant &value : controller.commentItems()) {
            const QVariantMap comment = value.toMap();
            if (comment.value(QStringLiteral("isReply")).toBool()) {
                result.second = comment;
            } else {
                result.first = comment;
            }
        }
        return result;
    };
    auto [root, reply] = currentComments();
    QVERIFY(!root.isEmpty());
    QVERIFY(!reply.isEmpty());
    QCOMPARE(
        root.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::CancelledCommentStatus));
    QVERIFY(!root.value(QStringLiteral("commentMarked")).toBool());
    QCOMPARE(
        reply.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::DeferredCommentStatus));
    QVERIFY(reply.value(QStringLiteral("commentMarked")).toBool());
    QVERIFY(root.value(QStringLiteral("commentStateEditable")).toBool());
    QVERIFY(reply.value(QStringLiteral("commentStateEditable")).toBool());
    QCOMPARE(
        reply.value(QStringLiteral("parentObjectId")).toString(),
        root.value(QStringLiteral("objectId")).toString());

    const int replyPage = reply.value(QStringLiteral("page")).toInt();
    const QString replyObjectId
        = reply.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.setCommentStatus(
        replyPage,
        replyObjectId,
        PDFEditController::CompletedCommentStatus));
    QVERIFY(controller.setCommentMarked(replyPage, replyObjectId, false));

    std::tie(root, reply) = currentComments();
    QCOMPARE(
        root.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::CancelledCommentStatus));
    QVERIFY(!root.value(QStringLiteral("commentMarked")).toBool());
    QCOMPARE(
        reply.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::CompletedCommentStatus));
    QVERIFY(!reply.value(QStringLiteral("commentMarked")).toBool());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentStateInspection inspection
        = inspectCommentStateTransitions(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.annotationCount, 9);
    QCOMPARE(inspection.transitions.size(), 7);
    bool foundCompletedReplyState = false;
    bool foundUnmarkedReplyState = false;
    for (const CommentStateTransitionInspection &transition
         : inspection.transitions) {
        if (transition.stateModel == QLatin1String("Review")
            && transition.state == QLatin1String("Completed")) {
            QCOMPARE(
                transition.parentName,
                QStringLiteral("reply-deferred-upper"));
            QCOMPARE(
                transition.author,
                QStringLiteral("TREPDFREVIEWER"));
            foundCompletedReplyState = true;
        } else if (transition.stateModel == QLatin1String("Marked")
                   && transition.state == QLatin1String("Unmarked")) {
            QCOMPARE(
                transition.parentName,
                QStringLiteral("reply-marked"));
            QCOMPARE(
                transition.author,
                QStringLiteral("TREPDFREVIEWER"));
            foundUnmarkedReplyState = true;
        }
    }
    QVERIFY(foundCompletedReplyState);
    QVERIFY(foundUnmarkedReplyState);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    std::tie(root, reply) = currentComments();
    QCOMPARE(
        root.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::CancelledCommentStatus));
    QVERIFY(!root.value(QStringLiteral("commentMarked")).toBool());
    QCOMPARE(
        reply.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::CompletedCommentStatus));
    QVERIFY(!reply.value(QStringLiteral("commentMarked")).toBool());
}

void PDFEditControllerTest::rehydratesAndExtendsCanonicalMixedCaseCommentStateHistory()
{
    const bool userWasSet = qEnvironmentVariableIsSet("USER");
    const QByteArray originalUser = qgetenv("USER");
    const auto restoreUser = qScopeGuard([userWasSet, originalUser]() {
        if (userWasSet) {
            qputenv("USER", originalUser);
        } else {
            qunsetenv("USER");
        }
    });
    QVERIFY(qputenv("USER", "trepdfreviewer"));

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-case-comment-state-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-case-comment-state-output.pdf"));
    QVERIFY(createMixedCaseCommentStateHistoryPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    QVariantMap root = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        root.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::CancelledCommentStatus));
    const QString objectId = root.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.setCommentStatus(
        0,
        objectId,
        PDFEditController::DeferredCommentStatus));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentStateInspection inspection
        = inspectCommentStateTransitions(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.transitions.size(), 6);
    const CommentStateTransitionInspection &saved
        = inspection.transitions.constLast();
    QCOMPARE(saved.state, QStringLiteral("Deferred"));
    QCOMPARE(saved.stateModel, QStringLiteral("Review"));
    QCOMPARE(saved.author, QStringLiteral("TREPDFREVIEWER"));
    QCOMPARE(saved.parentName, QStringLiteral("foxit-cancelled"));
    QVERIFY(saved.relation.isEmpty());

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    root = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        root.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::DeferredCommentStatus));
}

void PDFEditControllerTest::savesStateForAddedCommentBeforeFirstSave()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("added-comment-state-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("added-comment-state-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::StickyNoteTool);
    const QVariantMap added = controller.addStickyNote(
        0,
        QPointF(0.2, 0.3));
    QVERIFY(!added.isEmpty());
    const QString objectId = added
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!objectId.isEmpty());
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(controller.commentItems().constFirst().toMap()
        .value(QStringLiteral("commentStateEditable"))
        .toBool());

    QVERIFY(controller.setCommentStatus(
        0,
        objectId,
        PDFEditController::DeferredCommentStatus));
    QVERIFY(controller.setCommentMarked(0, objectId, true));
    const QVariantMap pending
        = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        pending.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::DeferredCommentStatus));
    QVERIFY(pending.value(QStringLiteral("commentMarked")).toBool());
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    const CommentAnnotationInspection savedRoot
        = inspectCommentAnnotation(outputPath, objectId);
    QVERIFY(savedRoot.readable);
    QCOMPARE(savedRoot.annotationIndex, 0);
    QCOMPARE(savedRoot.subtype, FPDF_ANNOT_TEXT);

    const CommentStateInspection inspection
        = inspectCommentStateTransitions(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.annotationCount, 3);
    QCOMPARE(inspection.transitions.size(), 2);
    for (const CommentStateTransitionInspection &transition
         : inspection.transitions) {
        QCOMPARE(transition.parentName, objectId);
        QVERIFY(transition.relation.isEmpty());
    }
    const auto deferred = std::find_if(
        inspection.transitions.cbegin(),
        inspection.transitions.cend(),
        [](const CommentStateTransitionInspection &transition) {
            return transition.stateModel == QLatin1String("Review");
        });
    const auto marked = std::find_if(
        inspection.transitions.cbegin(),
        inspection.transitions.cend(),
        [](const CommentStateTransitionInspection &transition) {
            return transition.stateModel == QLatin1String("Marked");
        });
    QVERIFY(deferred != inspection.transitions.cend());
    QVERIFY(marked != inspection.transitions.cend());
    QCOMPARE(deferred->state, QStringLiteral("Deferred"));
    QCOMPARE(marked->state, QStringLiteral("Marked"));

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    const QVariantMap rehydrated
        = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        rehydrated.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::DeferredCommentStatus));
    QVERIFY(rehydrated.value(QStringLiteral("commentMarked")).toBool());
    QVERIFY(rehydrated
        .value(QStringLiteral("commentStateEditable"))
        .toBool());
    QCOMPARE(rehydrated.value(QStringLiteral("replyCount")).toInt(), 0);
}

void PDFEditControllerTest::savesStateForAddedReplyBeforeFirstSave()
{
    const bool userWasSet = qEnvironmentVariableIsSet("USER");
    const QByteArray originalUser = qgetenv("USER");
    const auto restoreUser = qScopeGuard([userWasSet, originalUser]() {
        if (userWasSet) {
            qputenv("USER", originalUser);
        } else {
            qunsetenv("USER");
        }
    });
    QVERIFY(qputenv("USER", "trepdfreviewer"));

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("added-reply-state-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("added-reply-state-output.pdf"));
    QVERIFY(createMixedCaseCommentStateHistoryPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap root
        = controller.commentItems().constFirst().toMap();
    const QVariantMap addedReply = controller.addCommentReply(
        0,
        root.value(QStringLiteral("objectId")).toString(),
        QStringLiteral("Reply with state"));
    QVERIFY(!addedReply.isEmpty());
    const QString replyObjectId
        = addedReply.value(QStringLiteral("objectId")).toString();
    QVERIFY(!replyObjectId.isEmpty());
    QCOMPARE(controller.commentItems().size(), 2);

    QVariantMap pendingReply;
    for (const QVariant &value : controller.commentItems()) {
        const QVariantMap comment = value.toMap();
        if (comment.value(QStringLiteral("isReply")).toBool()) {
            pendingReply = comment;
            break;
        }
    }
    QVERIFY(!pendingReply.isEmpty());
    QVERIFY(pendingReply
        .value(QStringLiteral("commentStateEditable"))
        .toBool());
    QVERIFY(controller.setCommentStatus(
        0,
        replyObjectId,
        PDFEditController::FutureCommentStatus));
    QVERIFY(controller.setCommentMarked(0, replyObjectId, true));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentStateInspection inspection
        = inspectCommentStateTransitions(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.annotationCount, 9);
    QCOMPARE(inspection.transitions.size(), 7);
    int replyTransitionCount = 0;
    for (const CommentStateTransitionInspection &transition
         : inspection.transitions) {
        if (transition.parentName == replyObjectId) {
            ++replyTransitionCount;
            QCOMPARE(
                transition.author,
                QStringLiteral("TREPDFREVIEWER"));
            QVERIFY(transition.relation.isEmpty());
        }
    }
    QCOMPARE(replyTransitionCount, 2);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    QVariantMap rehydratedRoot;
    QVariantMap rehydratedReply;
    for (const QVariant &value : controller.commentItems()) {
        const QVariantMap comment = value.toMap();
        if (comment.value(QStringLiteral("isReply")).toBool()) {
            rehydratedReply = comment;
        } else {
            rehydratedRoot = comment;
        }
    }
    QVERIFY(!rehydratedRoot.isEmpty());
    QVERIFY(!rehydratedReply.isEmpty());
    QCOMPARE(
        rehydratedRoot.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::CancelledCommentStatus));
    QVERIFY(!rehydratedRoot.value(QStringLiteral("commentMarked")).toBool());
    QCOMPARE(
        rehydratedReply.value(QStringLiteral("commentStatus")).toInt(),
        static_cast<int>(PDFEditController::FutureCommentStatus));
    QVERIFY(rehydratedReply
        .value(QStringLiteral("commentMarked"))
        .toBool());
    QVERIFY(rehydratedReply
        .value(QStringLiteral("commentStateEditable"))
        .toBool());
}

void PDFEditControllerTest::rejectsProtectedCommentStateChanges()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("protected-comment-state.pdf"));
    QVERIFY(createProtectedCommentTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const QVariantList sourceComments = controller.commentItems();
    for (const QVariant &commentValue : sourceComments) {
        const QVariantMap comment = commentValue.toMap();
        const int page = comment.value(QStringLiteral("page")).toInt();
        const QString objectId = comment
            .value(QStringLiteral("objectId"))
            .toString();
        QVERIFY(!objectId.isEmpty());
        QCOMPARE(
            comment.value(QStringLiteral("commentStatus")).toInt(),
            static_cast<int>(PDFEditController::NoCommentStatus));
        QVERIFY(!comment.value(QStringLiteral("commentMarked")).toBool());
        QVERIFY(!comment
            .value(QStringLiteral("commentStateEditable"))
            .toBool());
        QVERIFY(!controller.setCommentStatus(
            page,
            objectId,
            PDFEditController::AcceptedCommentStatus));
        QVERIFY(!controller.setCommentMarked(page, objectId, true));
    }

    QCOMPARE(controller.commentItems(), sourceComments);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}
