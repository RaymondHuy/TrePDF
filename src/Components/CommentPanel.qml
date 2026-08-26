pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController

    property bool collapsed: false
    property var activeEditor: null
    property int activeEditorPage: -1
    property string activeEditorObjectId: ""
    property string activeEditorSourceText: ""
    property string activeEditorText: ""
    property bool activeEditorDirty: false
    property int replyTargetPage: -1
    property string replyTargetObjectId: ""
    property string replyDraftText: ""
    property var expandedReplyThreads: ({})

    signal collapsedChangeRequested(bool collapsed)
    signal pageRequested(int page)

    readonly property int commentCount:
        editController.commentItems
            ? editController.commentItems.length
            : 0
    readonly property var displayedCommentItems: {
        const comments = root.editController.commentItems
            ? root.editController.commentItems
            : []
        const displayedComments = []
        const expandedThreads = root.expandedReplyThreads
        for (let index = 0; index < comments.length; ++index) {
            const comment = comments[index]
            const isReply = comment.isReply === undefined
                ? false
                : Boolean(comment.isReply)
            if (!isReply) {
                displayedComments.push(comment)
                continue
            }
            const threadKey = String(Number(comment.page))
                + "\u001f"
                + String(comment.parentObjectId)
            if (expandedThreads[threadKey] === true)
                displayedComments.push(comment)
        }
        return displayedComments
    }
    readonly property bool textEditPending:
        activeEditorDirty || replyTargetObjectId.length > 0
    readonly property int replyIndent: 30
    readonly property int maximumCommentCharacters:
        root.editController.maximumCommentCharacters

    implicitWidth: collapsed ? 42 : 280
    color: theme.ribbonSurface

    function commitActiveEditor() {
        if (root.activeEditorObjectId.length === 0)
            return true

        if (root.activeEditorDirty
                && root.activeEditorText.length
                    > root.maximumCommentCharacters) {
            return false
        }
        if (root.activeEditorDirty
                && (!root.editController
                    || !root.editController.setCommentText(
                        root.activeEditorPage,
                        root.activeEditorObjectId,
                        root.activeEditorText))) {
            return false
        }
        root.activeEditor = null
        root.activeEditorPage = -1
        root.activeEditorObjectId = ""
        root.activeEditorSourceText = ""
        root.activeEditorText = ""
        root.activeEditorDirty = false
        return true
    }

    function beginCommentEdit(page, objectId, sourceText, editor) {
        const sameComment = root.activeEditorPage === page
            && root.activeEditorObjectId === objectId
        if (!sameComment && !root.commitActiveEditor())
            return false

        if (!sameComment) {
            root.activeEditorPage = page
            root.activeEditorObjectId = objectId
            root.activeEditorSourceText = sourceText
            root.activeEditorText = sourceText
            root.activeEditorDirty = false
        } else if (!root.activeEditorDirty
                   && root.activeEditorSourceText !== sourceText) {
            root.activeEditorSourceText = sourceText
            root.activeEditorText = sourceText
        }
        root.activeEditor = editor
        return true
    }

    function requestCollapsed(collapsed) {
        if (collapsed && !root.commitPendingEdit())
            return
        root.collapsedChangeRequested(collapsed)
    }

    function commitPendingEdit() {
        if (!commitActiveEditor())
            return false
        if (root.replyTargetObjectId.length === 0)
            return true
        if (root.replyDraftText.trim().length === 0) {
            root.cancelPendingReply()
            return true
        }
        return false
    }

    function commentThreadKey(page, objectId) {
        return String(page) + "\u001f" + objectId
    }

    function threadRepliesExpanded(page, objectId) {
        return root.expandedReplyThreads[
            root.commentThreadKey(page, objectId)] === true
    }

    function setThreadRepliesExpanded(page, objectId, expanded) {
        const threadKey = root.commentThreadKey(page, objectId)
        if (root.threadRepliesExpanded(page, objectId) === expanded)
            return

        const nextExpandedThreads = ({})
        const currentExpandedThreads = root.expandedReplyThreads
        for (const existingKey in currentExpandedThreads)
            nextExpandedThreads[existingKey]
                = currentExpandedThreads[existingKey]
        if (expanded)
            nextExpandedThreads[threadKey] = true
        else
            delete nextExpandedThreads[threadKey]
        root.expandedReplyThreads = nextExpandedThreads
    }

    function toggleThreadReplies(page, objectId) {
        const expanded = root.threadRepliesExpanded(page, objectId)
        if (expanded && !root.commitActiveEditor())
            return
        root.setThreadRepliesExpanded(page, objectId, !expanded)
    }

    function expandSelectedReply() {
        if (!root.editController)
            return

        const selectedPage = root.editController.selectedPage
        const selectedObjectId = root.editController.selectedObjectId
        if (selectedPage < 0 || selectedObjectId.length === 0)
            return
        const comments = root.editController.commentItems
        for (let index = 0; index < comments.length; ++index) {
            const comment = comments[index]
            if (Number(comment.page) !== selectedPage
                    || String(comment.objectId) !== selectedObjectId
                    || !Boolean(comment.isReply)) {
                continue
            }
            root.setThreadRepliesExpanded(
                selectedPage,
                String(comment.parentObjectId),
                true)
            return
        }
    }

    function beginReply(page, objectId) {
        if (!root.document.loaded
                || root.editController.saving
                || page < 0
                || objectId.length === 0) {
            return
        }
        if (!root.commitActiveEditor())
            return
        if (root.replyTargetObjectId.length > 0
                && (root.replyTargetPage !== page
                    || root.replyTargetObjectId !== objectId)) {
            if (root.replyDraftText.trim().length > 0)
                return
            root.cancelPendingReply()
        }
        root.replyTargetPage = page
        root.replyTargetObjectId = objectId
        root.replyDraftText = ""
    }

    function cancelPendingReply() {
        root.replyTargetPage = -1
        root.replyTargetObjectId = ""
        root.replyDraftText = ""
    }

    function replyTargetExists() {
        if (root.replyTargetObjectId.length === 0)
            return true

        const comments = root.editController.commentItems
        for (let index = 0; index < comments.length; ++index) {
            const comment = comments[index]
            if (Number(comment.page) === root.replyTargetPage
                    && String(comment.objectId)
                        === root.replyTargetObjectId
                    && !Boolean(comment.isReply)) {
                return true
            }
        }
        return false
    }

    function submitPendingReply() {
        if (root.replyTargetObjectId.length === 0)
            return true

        const replyText = root.replyDraftText
        if (replyText.trim().length === 0) {
            root.cancelPendingReply()
            return true
        }
        if (replyText.length > root.maximumCommentCharacters)
            return false
        if (!root.document.loaded || root.editController.saving)
            return false

        const replyPage = root.replyTargetPage
        const result = root.editController.addCommentReply(
            replyPage,
            root.replyTargetObjectId,
            replyText)
        if (!result
                || result.objectId === undefined
                || String(result.objectId).length === 0) {
            return false
        }
        root.setThreadRepliesExpanded(
            replyPage,
            root.replyTargetObjectId,
            true)
        root.cancelPendingReply()
        root.pageRequested(replyPage)
        return true
    }

    function removeComment(page, objectId) {
        if (!root.document.loaded
                || root.editController.saving
                || page < 0
                || objectId.length === 0) {
            return
        }

        if (root.replyTargetPage === page
                && root.replyTargetObjectId === objectId) {
            root.cancelPendingReply()
        }
        if (!root.commitActiveEditor())
            return
        root.editController.removeComment(page, objectId)
    }

    function ensureCommentIndex() {
        if (document && editController && document.loaded)
            editController.requestCommentIndex()
    }

    onCollapsedChanged: {
        if (collapsed)
            commitPendingEdit()
    }
    onDocumentChanged: {
        cancelPendingReply()
        expandedReplyThreads = ({})
        ensureCommentIndex()
    }
    onEditControllerChanged: {
        cancelPendingReply()
        expandedReplyThreads = ({})
        ensureCommentIndex()
    }
    Component.onCompleted: ensureCommentIndex()

    Connections {
        target: root.document

        function onLoadedChanged() {
            if (!root.document.loaded) {
                root.cancelPendingReply()
                root.expandedReplyThreads = ({})
            }
            root.ensureCommentIndex()
        }
    }

    Connections {
        target: root.editController

        function onCommentsChanged() {
            if (!root.replyTargetExists())
                root.cancelPendingReply()
            root.ensureCommentIndex()
        }

        function onSelectionChanged() {
            root.expandSelectedReply()
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: root.theme.ribbonBorder
        Accessible.ignored: true
    }

    ToolButton {
        id: expandButton

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 8
        width: 32
        height: 32
        visible: root.collapsed
        icon.source: "../assets/icons/chevron-left.svg"
        icon.width: 18
        icon.height: 18
        icon.color: root.theme.foxitDarkOrange
        display: AbstractButton.IconOnly
        focusPolicy: Qt.TabFocus
        //% "Expand comments panel"
        ToolTip.text: qsTrId("ExpandCommentsPanel")
        ToolTip.visible: hovered
        ToolTip.delay: 500
        Accessible.name: ToolTip.text
        onClicked: root.requestCollapsed(false)

        background: Rectangle {
            radius: 4
            color: expandButton.down
                ? "#f5dfcf"
                : expandButton.hovered
                    ? root.theme.foxitPaleOrange
                    : "transparent"
            border.color: expandButton.visualFocus
                ? root.theme.foxitDarkOrange
                : "transparent"
            border.width: expandButton.visualFocus ? 1 : 0
        }
    }

    Loader {
        anchors.fill: parent
        anchors.leftMargin: 1
        active: !root.collapsed

        sourceComponent: Component {
            ColumnLayout {
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 8
                    Layout.topMargin: 10
                    Layout.bottomMargin: 10
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        //% "Comments (%1)"
                        text: qsTrId("CommentsCount")
                            .arg(root.commentCount)
                        color: root.theme.ribbonTextColor
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }

                    ToolButton {
                        id: closeButton

                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        icon.source: "../assets/icons/chevron-right.svg"
                        icon.width: 16
                        icon.height: 16
                        icon.color: root.theme.foxitDarkOrange
                        display: AbstractButton.IconOnly
                        focusPolicy: Qt.TabFocus
                        //% "Close comments panel"
                        ToolTip.text: qsTrId("CloseCommentsPanel")
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        Accessible.name: ToolTip.text
                        onClicked: root.requestCollapsed(true)

                        background: Rectangle {
                            radius: 4
                            color: closeButton.down
                                ? "#f5dfcf"
                                : closeButton.hovered
                                    ? root.theme.foxitPaleOrange
                                    : "transparent"
                            border.color: closeButton.visualFocus
                                ? root.theme.foxitDarkOrange
                                : "transparent"
                            border.width: closeButton.visualFocus ? 1 : 0
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: root.theme.ribbonBorder
                    Accessible.ignored: true
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 0

                    ListView {
                        id: commentList

                        anchors.fill: parent
                        anchors.margins: 12
                        model: root.displayedCommentItems
                        spacing: 8
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        enabled: root.document.loaded
                        activeFocusOnTab: count > 0
                        Accessible.role: Accessible.List
                        //% "Comments"
                        Accessible.name: qsTrId("CommentsList")

                        delegate: Rectangle {
                            id: commentCard

                            required property int index
                            required property var modelData

                            readonly property string objectId:
                                modelData.objectId === undefined
                                    ? ""
                                    : String(modelData.objectId)
                            readonly property int page:
                                modelData.page === undefined
                                    ? -1
                                    : Number(modelData.page)
                            readonly property string kind:
                                modelData.kind === undefined
                                    ? ""
                                    : String(modelData.kind)
                            readonly property string label:
                                modelData.label === undefined
                                    ? kind
                                    : String(modelData.label)
                            readonly property string commentText:
                                modelData.text === undefined
                                    ? ""
                                    : String(modelData.text)
                            readonly property string commentAuthor:
                                modelData.commentAuthor === undefined
                                    ? ""
                                    : String(modelData.commentAuthor)
                            readonly property var commentDate:
                                modelData.commentDate === undefined
                                    ? null
                                    : modelData.commentDate
                            readonly property string commentDateText: {
                                if (commentDate === null
                                        || commentDate === undefined
                                        || isNaN(commentDate.getTime())) {
                                    return ""
                                }
                                return Qt.formatDateTime(
                                    commentDate,
                                    Qt.locale(),
                                    Locale.ShortFormat)
                            }
                            readonly property color markerColor:
                                modelData.color === undefined
                                    ? root.theme.foxitOrange
                                    : modelData.color
                            readonly property bool commentTextEditable:
                                modelData.commentTextEditable === undefined
                                    ? false
                                    : Boolean(
                                        modelData.commentTextEditable)
                            readonly property string commentTextEditReason:
                                modelData.commentTextEditReason === undefined
                                    ? ""
                                    : String(
                                        modelData.commentTextEditReason)
                            readonly property bool added:
                                modelData.added === undefined
                                    ? false
                                    : Boolean(modelData.added)
                            readonly property bool isReply:
                                modelData.isReply === undefined
                                    ? false
                                    : Boolean(modelData.isReply)
                            readonly property string parentObjectId:
                                modelData.parentObjectId === undefined
                                    ? ""
                                    : String(modelData.parentObjectId)
                            readonly property int replyCount:
                                modelData.replyCount === undefined
                                    ? 0
                                    : Number(modelData.replyCount)
                            readonly property string replyThreadKey:
                                String(page) + "\u001f" + objectId
                            readonly property bool repliesExpanded:
                                !isReply
                                    && root.expandedReplyThreads[
                                        replyThreadKey] === true
                            readonly property int hierarchyIndent:
                                isReply ? root.replyIndent : 0
                            readonly property bool selected:
                                root.editController.selectedPage === page
                                    && root.editController.selectedObjectId
                                        === objectId
                            readonly property bool editorVisible:
                                selected && commentTextEditable
                                    && (root.replyTargetPage !== page
                                        || root.replyTargetObjectId
                                            !== objectId)

                            width: ListView.view
                                ? Math.max(
                                    0,
                                    ListView.view.width
                                        - hierarchyIndent)
                                : 0
                            height: cardContent.implicitHeight + 20
                            radius: 7
                            color: selected
                                ? root.theme.foxitPaleOrange
                                : hoverHandler.hovered
                                    ? "#fff9f5"
                                    : isReply
                                        ? "#fffaf6"
                                        : "#ffffff"
                            border.width: selected
                                    || commentCard.activeFocus
                                    || isReply
                                ? 1
                                : 0
                            border.color: selected
                                ? root.theme.foxitBorderOrange
                                : root.theme.ribbonBorder
                            activeFocusOnTab: true
                            transform: Translate {
                                x: commentCard.hierarchyIndent
                            }
                            Accessible.role: Accessible.ListItem
                            //% "%1, page %2"
                            Accessible.name:
                                qsTrId("CommentItemAccessibleName")
                                    .arg(label)
                                    .arg(page + 1)
                            Accessible.description: commentTextEditReason
                            ToolTip.visible: hoverHandler.hovered
                                && commentTextEditReason.length > 0
                            ToolTip.text: commentTextEditReason

                            function activateComment() {
                                const targetObjectId = objectId
                                const targetPage = page
                                if (!root.commitPendingEdit())
                                    return
                                if (!root.document.loaded
                                        || targetObjectId.length === 0
                                        || targetPage < 0) {
                                    return
                                }

                                root.editController.tool
                                    = PDFEditController.SelectTool
                                root.editController.selectComment(
                                    targetPage,
                                    targetObjectId)
                                root.pageRequested(targetPage)
                            }

                            Accessible.onPressAction: activateComment()
                            Keys.onPressed: function(event) {
                                if (event.key === Qt.Key_Return
                                        || event.key === Qt.Key_Enter
                                        || event.key === Qt.Key_Space) {
                                    commentCard.activateComment()
                                    event.accepted = true
                                }
                            }

                            HoverHandler {
                                id: hoverHandler
                            }

                            TapHandler {
                                enabled: !commentCard.editorVisible
                                    && (root.replyTargetPage
                                            !== commentCard.page
                                        || root.replyTargetObjectId
                                            !== commentCard.objectId)
                                acceptedButtons: Qt.LeftButton
                                onTapped: commentCard.activateComment()
                            }

                            Item {
                                visible: commentCard.isReply
                                x: -22
                                y: -commentList.spacing
                                width: 22
                                height: commentCard.height
                                    + commentList.spacing
                                Accessible.ignored: true

                                Rectangle {
                                    x: 0
                                    y: 0
                                    width: 2
                                    height: parent.height
                                    radius: 1
                                    color: root.theme.ribbonBorder
                                }

                                Rectangle {
                                    x: 0
                                    y: commentList.spacing + 20
                                    width: parent.width
                                    height: 2
                                    radius: 1
                                    color: root.theme.foxitBorderOrange
                                }
                            }

                            ColumnLayout {
                                id: cardContent

                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 7

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Rectangle {
                                        Layout.preferredWidth: 5
                                        Layout.preferredHeight: 24
                                        visible: !commentCard.isReply
                                        radius: 2
                                        color: commentCard.markerColor
                                        Accessible.ignored: true
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: !commentCard.isReply
                                        text: commentCard.label
                                        color: root.theme.ribbonTextColor
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        visible: commentCard.isReply
                                        text: commentCard.commentAuthor
                                        color: "#747176"
                                        font.pixelSize: 10
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        visible: !commentCard.isReply
                                            && commentCard.added
                                        Layout.preferredHeight: 20
                                        leftPadding: 7
                                        rightPadding: 7
                                        //% "New"
                                        text: qsTrId("NewComment")
                                        color: root.theme.foxitDarkOrange
                                        font.pixelSize: 10
                                        verticalAlignment: Text.AlignVCenter

                                        background: Rectangle {
                                            radius: 10
                                            color: root.theme.foxitPaleOrange
                                            border.width: 1
                                            border.color:
                                                root.theme.foxitBorderOrange
                                        }
                                    }

                                    Label {
                                        //% "Page %1"
                                        text: qsTrId("PageNumber")
                                            .arg(commentCard.page + 1)
                                        visible: !commentCard.isReply
                                        color: "#747176"
                                        font.pixelSize: 10
                                    }

                                    Label {
                                        visible: commentCard.isReply
                                            && commentCard.commentDateText
                                                .length > 0
                                        text: commentCard.commentDateText
                                        color: "#747176"
                                        font.pixelSize: 10
                                    }

                                    ToolButton {
                                        id: showCommentButton

                                        Layout.preferredWidth: 20
                                        Layout.fillHeight: true
                                        visible: !commentCard.isReply
                                            && commentCard.replyCount > 0
                                        icon.source:
                                            "../assets/icons/down.svg"
                                        icon.width: 12
                                        icon.height: 12
                                        icon.color: "transparent"
                                        rotation:
                                            commentCard.repliesExpanded
                                                ? 180
                                                : 0
                                        focusPolicy: Qt.TabFocus
                                        //% "Show or hide replies"
                                        ToolTip.text:
                                            qsTrId("ToggleReplies")
                                        ToolTip.visible: hovered
                                        ToolTip.delay: 500
                                        Accessible.name: ToolTip.text
                                        onClicked:
                                            root.toggleThreadReplies(
                                                commentCard.page,
                                                commentCard.objectId)

                                        background: Rectangle {
                                            radius: 4
                                            color: showCommentButton.down
                                                ? "#f5dfcf"
                                                : showCommentButton.hovered
                                                    ? root.theme
                                                        .foxitPaleOrange
                                                    : "white"
                                            border.color:
                                                showCommentButton.visualFocus
                                                    ? root.theme
                                                        .foxitDarkOrange
                                                    : root.theme.ribbonBorder
                                            border.width:
                                                showCommentButton.visualFocus
                                                    ? 2
                                                    : 1
                                        }
                                    }
                                    ToolButton {
                                        id: removeCommentButton

                                        Layout.preferredWidth: 20
                                        Layout.fillHeight: true
                                        icon.source: "../assets/icons/cancel.svg"
                                        icon.width: 12
                                        icon.height: 12
                                        icon.color: "transparent"
                                        enabled: root.document.loaded
                                            && !root.editController.saving
                                            && commentCard.commentTextEditable
                                            && commentCard.page >= 0
                                            && commentCard.objectId.length > 0
                                        focusPolicy: Qt.TabFocus
                                        //% "Delete selected comment"
                                        ToolTip.text:
                                            qsTrId("DeleteSelectedComment")
                                        ToolTip.visible: hovered
                                        ToolTip.delay: 500
                                        Accessible.name: ToolTip.text
                                        onClicked: root.removeComment(
                                            commentCard.page,
                                            commentCard.objectId)

                                        background: Rectangle {
                                            radius: 4
                                            color: removeCommentButton.down
                                                ? "#f5dfcf"
                                                : removeCommentButton.hovered
                                                    ? root.theme.foxitPaleOrange
                                                    : "white"
                                            border.color: removeCommentButton.visualFocus
                                                ? root.theme.foxitDarkOrange
                                                : root.theme.ribbonBorder
                                            border.width: removeCommentButton.visualFocus
                                                ? 2
                                                : 1
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 13
                                    visible: !commentCard.isReply
                                        && (commentCard.commentAuthor.length > 0
                                            || commentCard.commentDateText
                                                .length > 0)
                                    spacing: 8

                                    Label {
                                        Layout.fillWidth: true
                                        visible:
                                            commentCard.commentAuthor.length > 0
                                        text: commentCard.commentAuthor
                                        color: "#747176"
                                        font.pixelSize: 10
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        visible:
                                            commentCard.commentDateText.length > 0
                                        text: commentCard.commentDateText
                                        color: "#747176"
                                        font.pixelSize: 10
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: commentCard.commentText.length > 0
                                        && !commentCard.editorVisible
                                    text: commentCard.commentText
                                    color: "#625e61"
                                    font.pixelSize: 11
                                    wrapMode: Text.Wrap
                                    elide: Text.ElideRight
                                    maximumLineCount: 3
                                }

                                ToolButton {
                                    id: replyButton

                                    Layout.alignment: Qt.AlignLeft
                                    visible: !commentCard.isReply
                                        && (root.replyTargetPage
                                                !== commentCard.page
                                            || root.replyTargetObjectId
                                                !== commentCard.objectId)
                                    enabled: root.document.loaded
                                        && !root.editController.saving
                                        && commentCard.commentTextEditable
                                    focusPolicy: Qt.TabFocus
                                    //% "Reply"
                                    text: qsTrId("Reply")
                                    icon.source:
                                        "../assets/icons/comment.svg"
                                    icon.width: 14
                                    icon.height: 14
                                    icon.color:
                                        root.theme.ribbonTextColor
                                    display: AbstractButton.TextBesideIcon
                                    spacing: 6
                                    Accessible.name: text
                                    onClicked: root.beginReply(
                                        commentCard.page,
                                        commentCard.objectId)

                                    background: Rectangle {
                                        radius: 4
                                        color: replyButton.down
                                            ? "#f5dfcf"
                                            : replyButton.hovered
                                                ? root.theme.foxitPaleOrange
                                                : "transparent"
                                        border.color:
                                            replyButton.visualFocus
                                                ? root.theme.foxitDarkOrange
                                                : "transparent"
                                        border.width:
                                            replyButton.visualFocus ? 1 : 0
                                    }
                                }

                                Loader {
                                    id: replyEditorLoader

                                    Layout.fillWidth: true
                                    active: !commentCard.isReply
                                        && root.replyTargetPage
                                            === commentCard.page
                                        && root.replyTargetObjectId
                                            === commentCard.objectId
                                    visible: active

                                    sourceComponent: Component {
                                        ColumnLayout {
                                            spacing: 6

                                            TextArea {
                                                id: replyEditor

                                                //% "%1 / %2 characters"
                                                readonly property string characterCountText:
                                                    qsTrId(
                                                        "CommentCharacterCount")
                                                        .arg(length)
                                                        .arg(root
                                                            .maximumCommentCharacters)

                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 80
                                                text: root.replyDraftText
                                                wrapMode: TextEdit.Wrap
                                                selectByMouse: true
                                                persistentSelection: true
                                                font.pixelSize: 12
                                                color: root.theme.ribbonTextColor
                                                selectionColor:
                                                    root.theme.foxitHoverOrange
                                                selectedTextColor: "#ffffff"
                                                //% "Write a reply"
                                                placeholderText:
                                                    qsTrId("WriteReplyPlaceholder")
                                                //% "Reply text"
                                                Accessible.name:
                                                    qsTrId("ReplyText")
                                                Accessible.description:
                                                    characterCountText
                                                Component.onCompleted:
                                                    forceActiveFocus()
                                                onTextChanged:
                                                    root.replyDraftText = text
                                                Keys.onPressed: function(event) {
                                                    const commitModifier
                                                        = event.modifiers
                                                            & (Qt.ControlModifier
                                                                | Qt.MetaModifier)
                                                    if (commitModifier
                                                            && (event.key
                                                                    === Qt.Key_Return
                                                                || event.key
                                                                    === Qt.Key_Enter)
                                                            && !replyEditor
                                                                .inputMethodComposing) {
                                                        event.accepted = true
                                                        if (replyEditor.length
                                                                <= root
                                                                    .maximumCommentCharacters) {
                                                            root
                                                                .submitPendingReply()
                                                        }
                                                    } else if (event.key
                                                            === Qt.Key_Escape) {
                                                        root.cancelPendingReply()
                                                        event.accepted = true
                                                    }
                                                }

                                                background: Rectangle {
                                                    radius: 5
                                                    color: "#ffffff"
                                                    border.width: 1
                                                    border.color:
                                                        replyEditor.activeFocus
                                                            ? root.theme
                                                                .foxitBorderOrange
                                                            : root.theme
                                                                .ribbonBorder
                                                }
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                text: replyEditor
                                                    .characterCountText
                                                color: replyEditor.length
                                                        > root
                                                            .maximumCommentCharacters
                                                    ? "#b3261e"
                                                    : "#747176"
                                                font.pixelSize: 10
                                                horizontalAlignment:
                                                    Text.AlignRight
                                                Accessible.ignored: true
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 6

                                                Item {
                                                    Layout.fillWidth: true
                                                }

                                                Button {
                                                    //% "Cancel reply"
                                                    text: qsTrId("CancelReply")
                                                    onClicked:
                                                        root.cancelPendingReply()
                                                }

                                                Button {
                                                    //% "Send reply"
                                                    text: qsTrId("SendReply")
                                                    enabled:
                                                        root.replyDraftText
                                                            .trim().length > 0
                                                        && root.replyDraftText
                                                            .length
                                                            <= root
                                                                .maximumCommentCharacters
                                                        && !root.editController
                                                            .saving
                                                    onClicked:
                                                        root.submitPendingReply()
                                                }
                                            }
                                        }
                                    }

                                }

                                TextArea {
                                    id: commentEditor

                                    //% "%1 / %2 characters"
                                    readonly property string characterCountText:
                                        qsTrId("CommentCharacterCount")
                                            .arg(length)
                                            .arg(root.maximumCommentCharacters)
                                    property string sourceText:
                                        commentCard.commentText
                                    readonly property bool ownsActiveEdit:
                                        root.activeEditorPage
                                            === commentCard.page
                                        && root.activeEditorObjectId
                                            === commentCard.objectId
                                    readonly property bool dirty:
                                        ownsActiveEdit
                                            && root.activeEditorDirty

                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 92
                                    visible: commentCard.editorVisible
                                    enabled: !root.editController.saving
                                    text: ownsActiveEdit
                                        ? root.activeEditorText
                                        : sourceText
                                    wrapMode: TextEdit.Wrap
                                    selectByMouse: true
                                    persistentSelection: true
                                    font.pixelSize: 12
                                    color: root.theme.ribbonTextColor
                                    selectionColor:
                                        root.theme.foxitHoverOrange
                                    selectedTextColor: "#ffffff"
                                    //% "Write a comment"
                                    placeholderText:
                                        qsTrId("WriteCommentPlaceholder")
                                    //% "Comment text"
                                    Accessible.name: qsTrId("CommentText")
                                    Accessible.description:
                                        characterCountText

                                    function commitText() {
                                        if (!ownsActiveEdit || !dirty)
                                            return true
                                        if (length
                                                > root
                                                    .maximumCommentCharacters) {
                                            return false
                                        }
                                        return root.commitActiveEditor()
                                    }

                                    onSourceTextChanged: {
                                        if (!activeFocus
                                                && !ownsActiveEdit) {
                                            text = sourceText
                                        }
                                    }
                                    onTextChanged: {
                                        if (activeFocus
                                                && ownsActiveEdit) {
                                            root.activeEditorText = text
                                            root.activeEditorDirty
                                                = text
                                                    !== root
                                                        .activeEditorSourceText
                                        }
                                    }
                                    onActiveFocusChanged: {
                                        if (activeFocus) {
                                            if (!root.beginCommentEdit(
                                                    commentCard.page,
                                                    commentCard.objectId,
                                                    sourceText,
                                                    commentEditor)) {
                                                commentEditor.focus = false
                                            }
                                        } else if (root.activeEditor
                                                === commentEditor) {
                                            root.commitActiveEditor()
                                        }
                                    }
                                    Keys.onPressed: function(event) {
                                        const commitModifier
                                            = event.modifiers
                                                & (Qt.ControlModifier
                                                    | Qt.MetaModifier)
                                        if (commitModifier
                                                && (event.key
                                                        === Qt.Key_Return
                                                    || event.key
                                                        === Qt.Key_Enter)
                                                && !commentEditor
                                                    .inputMethodComposing) {
                                            event.accepted = true
                                            if (commentEditor.length
                                                    <= root
                                                        .maximumCommentCharacters
                                                    && commentEditor
                                                        .commitText()) {
                                                root.beginCommentEdit(
                                                    commentCard.page,
                                                    commentCard.objectId,
                                                    commentEditor.text,
                                                    commentEditor)
                                            }
                                        }
                                    }

                                    background: Rectangle {
                                        radius: 5
                                        color: "#ffffff"
                                        border.width: 1
                                        border.color:
                                            commentEditor.activeFocus
                                                ? root.theme
                                                    .foxitBorderOrange
                                                : root.theme.ribbonBorder
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: commentCard.editorVisible
                                    text: commentEditor.characterCountText
                                    color: commentEditor.length
                                            > root.maximumCommentCharacters
                                        ? "#b3261e"
                                        : "#747176"
                                    font.pixelSize: 10
                                    horizontalAlignment: Text.AlignRight
                                    Accessible.ignored: true
                                }
                            }
                        }

                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        width: Math.max(0, parent.width - 32)
                        visible: commentList.count === 0
                        //% "No comments yet."
                        text: qsTrId("NoCommentsYet")
                        color: "#747176"
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
}
