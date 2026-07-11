pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root

    required property int page
    required property bool pooled
    required property AppTheme theme
    required property PDFEditController controller

    property var drafts: []
    property var hoverCandidate: ({})
    property bool editing: false
    property string editingObjectId: ""
    readonly property bool interactionActive: !pooled
        && controller.tool !== PDFEditController.BrowseTool

    visible: !pooled && (interactionActive || drafts.length > 0)
    enabled: interactionActive

    function reloadData() {
        if (pooled) {
            drafts = []
            hoverCandidate = ({})
            return
        }

        drafts = controller.draftsForPage(page)
        if (!interactionActive)
            hoverCandidate = ({})
    }

    function hasCandidate(candidate) {
        return candidate !== null
            && candidate.objectId !== undefined
            && candidate.objectId.length > 0
    }

    function candidateAtPosition(x, y) {
        if (!interactionActive || width <= 0 || height <= 0)
            return ({})

        return controller.candidateAt(page, x / width, y / height)
    }

    function selectCandidate(candidate) {
        if (editing && editingObjectId !== candidate.objectId)
            commitEditing()

        controller.selectCandidate(page, candidate.objectId)
    }

    function beginEditing(candidate) {
        if (!candidate.editable || controller.tool !== PDFEditController.EditTextTool)
            return

        if (!controller.selectCandidate(page, candidate.objectId))
            return

        controller.beginSelectedTextEdit()
        editingObjectId = candidate.objectId
        editor.text = controller.selectedText
        editing = true
        Qt.callLater(function() {
            editor.forceActiveFocus()
            editor.selectAll()
        })
    }

    function commitEditing() {
        if (!editing)
            return

        controller.updateSelectedTextPreview(editor.text)
        controller.finishSelectedTextEdit()
        editing = false
        editingObjectId = ""
        reloadData()
    }

    function cancelEditing() {
        controller.cancelSelectedTextEdit()
        editing = false
        editingObjectId = ""
    }

    function beginSelectedEditing() {
        if (controller.selectedPage !== page || !controller.selectedEditable)
            return

        beginEditing({
            objectId: controller.selectedObjectId,
            editable: controller.selectedEditable
        })
    }

    function selectRelativeCandidate(direction) {
        const candidates = controller.candidatesForPage(page)
        const selectable = []
        for (let index = 0; index < candidates.length; ++index) {
            if (controller.tool !== PDFEditController.EditTextTool
                    || candidates[index].editable) {
                selectable.push(candidates[index])
            }
        }
        if (selectable.length === 0)
            return false

        let currentIndex = -1
        for (let index = 0; index < selectable.length; ++index) {
            if (controller.selectedPage === page
                    && selectable[index].objectId === controller.selectedObjectId) {
                currentIndex = index
                break
            }
        }

        const targetIndex = currentIndex < 0
            ? direction > 0 ? 0 : selectable.length - 1
            : currentIndex + direction
        if (targetIndex < 0 || targetIndex >= selectable.length)
            return false

        return controller.selectCandidate(page, selectable[targetIndex].objectId)
    }

    onPageChanged: {
        if (editing)
            commitEditing()
        reloadData()
    }

    onPooledChanged: {
        if (pooled && editing)
            commitEditing()
        reloadData()
    }

    onInteractionActiveChanged: reloadData()
    Component.onCompleted: reloadData()

    Connections {
        target: root.controller

        function onPageDataChanged(changedPage) {
            if (changedPage < 0 || changedPage === root.page)
                root.reloadData()
        }

        function onToolChanged() {
            if (root.editing)
                root.commitEditing()
            root.reloadData()
        }

        function onSelectionChanged() {
            if (root.editing
                    && (root.controller.selectedPage !== root.page
                        || root.controller.selectedObjectId !== root.editingObjectId)) {
                root.editing = false
                root.editingObjectId = ""
            }
        }
    }

    Repeater {
        model: root.drafts

        delegate: Rectangle {
            required property var modelData

            x: modelData.x * root.width
            y: modelData.y * root.height
            width: Math.max(1, modelData.width * root.width)
            height: Math.max(1, modelData.height * root.height)
            color: "#fffdfb"
            visible: !(root.editing && root.editingObjectId === modelData.objectId)
            clip: true

            Text {
                anchors.fill: parent
                anchors.margins: 1
                text: parent.modelData.text
                color: parent.modelData.color
                font.family: parent.modelData.fontFamily
                font.pixelSize: Math.max(6, parent.modelData.fontSizeRatio * root.height)
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }
    }

    MouseArea {
        id: hitArea

        anchors.fill: parent
        enabled: root.interactionActive
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        preventStealing: false
        cursorShape: root.hasCandidate(root.hoverCandidate)
            ? root.hoverCandidate.editable ? Qt.IBeamCursor : Qt.ForbiddenCursor
            : Qt.ArrowCursor
        activeFocusOnTab: true
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("PDF edit surface")
        Accessible.description: qsTr("Use Tab or arrow keys to select content, then press Enter to edit text")

        onPositionChanged: mouse => {
            root.hoverCandidate = root.candidateAtPosition(mouse.x, mouse.y)
        }
        onExited: root.hoverCandidate = ({})
        onPressed: forceActiveFocus()
        onClicked: mouse => {
            root.hoverCandidate = root.candidateAtPosition(mouse.x, mouse.y)
            if (root.hasCandidate(root.hoverCandidate))
                root.selectCandidate(root.hoverCandidate)
            else
                root.controller.clearSelection()
        }
        onDoubleClicked: mouse => {
            root.hoverCandidate = root.candidateAtPosition(mouse.x, mouse.y)
            if (root.hasCandidate(root.hoverCandidate))
                root.beginEditing(root.hoverCandidate)
        }

        Keys.onReturnPressed: event => {
            root.beginSelectedEditing()
            event.accepted = true
        }
        Keys.onEnterPressed: event => {
            root.beginSelectedEditing()
            event.accepted = true
        }
        Keys.onTabPressed: event => {
            event.accepted = root.selectRelativeCandidate(1)
        }
        Keys.onBacktabPressed: event => {
            event.accepted = root.selectRelativeCandidate(-1)
        }
        Keys.onDownPressed: event => {
            event.accepted = root.selectRelativeCandidate(1)
        }
        Keys.onUpPressed: event => {
            event.accepted = root.selectRelativeCandidate(-1)
        }
    }

    Rectangle {
        x: root.hasCandidate(root.hoverCandidate) ? root.hoverCandidate.x * root.width : 0
        y: root.hasCandidate(root.hoverCandidate) ? root.hoverCandidate.y * root.height : 0
        width: root.hasCandidate(root.hoverCandidate)
            ? Math.max(1, root.hoverCandidate.width * root.width)
            : 0
        height: root.hasCandidate(root.hoverCandidate)
            ? Math.max(1, root.hoverCandidate.height * root.height)
            : 0
        color: "transparent"
        border.width: 2
        border.color: root.hasCandidate(root.hoverCandidate) && root.hoverCandidate.editable
            ? root.theme.foxitHoverOrange
            : "#a5a7aa"
        visible: root.interactionActive && root.hasCandidate(root.hoverCandidate)
    }

    Rectangle {
        x: root.controller.selectedX * root.width
        y: root.controller.selectedY * root.height
        width: root.controller.selectedWidth * root.width
        height: root.controller.selectedHeight * root.height
        color: "#14d97a32"
        border.width: 2
        border.color: root.controller.selectedEditable
            ? root.theme.foxitOrange
            : "#8a8d92"
        visible: root.interactionActive
            && root.controller.selectedPage === root.page
            && root.controller.selectedObjectId.length > 0
    }

    Rectangle {
        id: editorFrame

        x: root.controller.selectedX * root.width
        y: root.controller.selectedY * root.height
        width: Math.max(80, root.controller.selectedWidth * root.width)
        height: Math.max(26, root.controller.selectedHeight * root.height)
        color: "#fffdfb"
        border.color: root.theme.foxitOrange
        border.width: 2
        radius: 2
        visible: root.editing
            && root.controller.selectedPage === root.page
            && root.controller.selectedEditable

        TextInput {
            id: editor

            anchors.fill: parent
            anchors.margins: 4
            color: root.controller.selectedColor
            font.family: root.controller.selectedFontFamily
            font.pixelSize: Math.max(8, root.controller.selectedFontSizeRatio * root.height)
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            onTextEdited: root.controller.updateSelectedTextPreview(text)
            onAccepted: root.commitEditing()

            Keys.onEscapePressed: event => {
                root.cancelEditing()
                event.accepted = true
            }
        }
    }

    Rectangle {
        anchors.left: editorFrame.left
        anchors.bottom: editorFrame.top
        anchors.bottomMargin: 4
        width: editorHint.implicitWidth + 14
        height: 22
        radius: 3
        color: "#e63a3b3f"
        visible: editorFrame.visible

        Label {
            id: editorHint

            anchors.centerIn: parent
            text: qsTr("Enter to apply · Esc to cancel")
            color: "white"
            font.pixelSize: 10
        }
    }

    Rectangle {
        x: Math.min(Math.max(0, root.controller.selectedX * root.width),
                    Math.max(0, root.width - width))
        y: Math.max(0, root.controller.selectedY * root.height - height - 5)
        width: Math.min(root.width, selectionReason.implicitWidth + 18)
        height: 26
        radius: 3
        color: "#e65d6065"
        visible: root.interactionActive
            && root.controller.selectedPage === root.page
            && root.controller.selectedObjectId.length > 0
            && !root.controller.selectedEditable

        Label {
            id: selectionReason

            anchors.fill: parent
            anchors.leftMargin: 9
            anchors.rightMargin: 9
            text: root.controller.selectedReason
            color: "white"
            font.pixelSize: 11
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
    }
}
