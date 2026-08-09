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
    property string activeEditorObjectId: ""

    signal collapsedChangeRequested(bool collapsed)
    signal pageRequested(int page)

    readonly property int commentCount:
        editController.commentItems
            ? editController.commentItems.length
            : 0
    readonly property bool textEditPending: activeEditor !== null
        && activeEditor.dirty === true

    implicitWidth: collapsed ? 42 : 280
    color: theme.ribbonSurface

    function commitActiveEditor() {
        const editor = root.activeEditor
        if (editor === null)
            return

        if (root.editController
                && root.editController.selectedObjectId
                    === root.activeEditorObjectId
                && typeof editor.commitText === "function") {
            editor.commitText()
        }
        root.activeEditor = null
        root.activeEditorObjectId = ""
    }

    function requestCollapsed(collapsed) {
        if (collapsed)
            root.commitActiveEditor()
        root.collapsedChangeRequested(collapsed)
    }

    function commitPendingEdit() {
        commitActiveEditor()
        return true
    }

    function removeComment(index) {
        const comments = root.editController.commentItems
        if (!root.document.loaded
                || root.editController.saving
                || index < 0
                || index >= comments.length) {
            return
        }

        const comment = comments[index]
        const page = Number(comment.page)
        const objectId = String(comment.objectId)
        if (page < 0 || objectId.length === 0)
            return

        root.commitActiveEditor()
        root.editController.removeComment(page, objectId)
    }

    function ensureCommentIndex() {
        if (document && editController && document.loaded)
            editController.requestCommentIndex()
    }

    onCollapsedChanged: {
        if (collapsed)
            commitActiveEditor()
    }
    onDocumentChanged: ensureCommentIndex()
    onEditControllerChanged: ensureCommentIndex()
    Component.onCompleted: ensureCommentIndex()
    Component.onDestruction: commitActiveEditor()

    Connections {
        target: root.document

        function onLoadedChanged() {
            root.ensureCommentIndex()
        }
    }

    Connections {
        target: root.editController

        function onCommentsChanged() {
            root.ensureCommentIndex()
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
                        model: root.editController.commentItems
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
                            readonly property bool editable:
                                modelData.editable === undefined
                                    ? false
                                    : Boolean(modelData.editable)
                            readonly property bool added:
                                modelData.added === undefined
                                    ? false
                                    : Boolean(modelData.added)
                            readonly property bool selected:
                                root.editController.selectedObjectId
                                    === objectId
                            readonly property bool stickyNote:
                                kind.toLowerCase() === "stickynote"
                            readonly property bool editorVisible:
                                selected && stickyNote && editable

                            width: ListView.view
                                ? ListView.view.width
                                : 0
                            height: cardContent.implicitHeight + 20
                            radius: 7
                            color: selected
                                ? root.theme.foxitPaleOrange
                                : hoverHandler.hovered
                                    ? "#fff9f5"
                                    : "#ffffff"
                            border.width: selected || commentCard.activeFocus
                                ? 1
                                : 0
                            border.color: selected
                                ? root.theme.foxitBorderOrange
                                : root.theme.ribbonBorder
                            activeFocusOnTab: true
                            Accessible.role: Accessible.ListItem
                            //% "%1, page %2"
                            Accessible.name:
                                qsTrId("CommentItemAccessibleName")
                                    .arg(label)
                                    .arg(page + 1)

                            function activateComment() {
                                const targetObjectId = objectId
                                const targetPage = page
                                root.commitActiveEditor()
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
                                acceptedButtons: Qt.LeftButton
                                onTapped: commentCard.activateComment()
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
                                        radius: 2
                                        color: commentCard.markerColor
                                        Accessible.ignored: true
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: commentCard.label
                                        color: root.theme.ribbonTextColor
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        visible: commentCard.added
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
                                        color: "#747176"
                                        font.pixelSize: 10
                                    }
                                    ToolButton {
                                        id: showCommentButton

                                        Layout.preferredWidth: 20
                                        Layout.fillHeight: true
                                        icon.source: "../assets/icons/down.svg"
                                        icon.width: 12
                                        icon.height: 12
                                        icon.color: "transparent"
                                        enabled: commentList.count > 1
                                        Accessible.name: ToolTip.text

                                        background: Rectangle {
                                            radius: 4
                                            color: showCommentButton.down
                                                ? "#f5dfcf"
                                                : showCommentButton.hovered
                                                    ? root.theme.foxitPaleOrange
                                                    : "white"
                                            border.color: showCommentButton.visualFocus
                                                ? root.theme.foxitDarkOrange
                                                : root.theme.ribbonBorder
                                            border.width: showCommentButton.visualFocus
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
                                            && commentCard.page >= 0
                                            && commentCard.objectId.length > 0
                                        focusPolicy: Qt.TabFocus
                                        //% "Delete selected comment"
                                        ToolTip.text:
                                            qsTrId("DeleteSelectedComment")
                                        ToolTip.visible: hovered
                                        ToolTip.delay: 500
                                        Accessible.name: ToolTip.text
                                        onClicked: root.removeComment(commentCard.index)

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
                                    visible:
                                        commentCard.commentAuthor.length > 0
                                        || commentCard.commentDateText.length > 0
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

                                TextArea {
                                    id: commentEditor

                                    property string sourceText:
                                        commentCard.commentText
                                    property bool dirty: false

                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 92
                                    visible: commentCard.editorVisible
                                    text: sourceText
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

                                    function commitText() {
                                        if (!dirty
                                                || !root.editController
                                                || root.editController
                                                    .selectedObjectId
                                                    !== commentCard.objectId) {
                                            return
                                        }

                                        dirty = false
                                        root.editController
                                            .setSelectedCommentText(text)
                                    }

                                    onSourceTextChanged: {
                                        if (!activeFocus) {
                                            text = sourceText
                                            dirty = false
                                        }
                                    }
                                    onTextChanged: {
                                        if (activeFocus)
                                            dirty = text !== sourceText
                                    }
                                    onActiveFocusChanged: {
                                        if (activeFocus) {
                                            root.activeEditor = commentEditor
                                            root.activeEditorObjectId
                                                = commentCard.objectId
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
                                                        === Qt.Key_Enter)) {
                                            commentEditor.commitText()
                                            event.accepted = true
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
