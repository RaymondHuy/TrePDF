pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root

    required property int page
    required property bool pooled
    required property bool draftTextureCurrent
    required property bool draftTextureReady
    required property AppTheme theme
    required property PDFEditController controller

    property var drafts: []
    property var hoverCandidate: ({})
    property bool editing: false
    property string editingObjectId: ""
    readonly property real fallbackBleed: 2
    readonly property bool interactionActive: !pooled
        && controller.tool !== PDFEditController.BrowseTool
    readonly property bool editorUsesCleanTexture: draftTextureReady
        && hasActiveDraft(editingObjectId)

    visible: !pooled && (interactionActive || drafts.length > 0)
    enabled: interactionActive

    function reloadData() {
        if (pooled) {
            drafts = []
            hoverCandidate = ({})
            return
        }

        const previousDrafts = drafts
        const currentDrafts = controller.draftsForPage(page)
        const displayedDrafts = []
        for (let index = 0; index < currentDrafts.length; ++index)
            displayedDrafts.push(currentDrafts[index])

        if (!draftTextureCurrent) {
            for (let index = 0; index < previousDrafts.length; ++index) {
                const previousDraft = previousDrafts[index]
                if (containsObject(currentDrafts, previousDraft.objectId))
                    continue

                if (previousDraft.restoration === true)
                    displayedDrafts.push(previousDraft)
                else if (previousDraft.originalText !== undefined)
                    displayedDrafts.push(restorationOverlay(previousDraft))
            }
        }
        drafts = displayedDrafts
        if (!interactionActive)
            hoverCandidate = ({})
    }

    function containsObject(items, objectId) {
        for (let index = 0; index < items.length; ++index) {
            if (items[index].objectId === objectId)
                return true
        }
        return false
    }

    function hasActiveDraft(objectId) {
        if (objectId.length === 0)
            return false

        for (let index = 0; index < drafts.length; ++index) {
            if (drafts[index].objectId === objectId
                    && drafts[index].restoration !== true) {
                return true
            }
        }
        return false
    }

    function restorationOverlay(draft) {
        return {
            objectId: draft.objectId,
            x: draft.x,
            y: draft.y,
            width: draft.width,
            height: draft.height,
            text: draft.originalText,
            originalText: draft.originalText,
            fontFamily: draft.fontFamily,
            fontSizeRatio: draft.fontSizeRatio,
            color: draft.color,
            restoration: true
        }
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
    onDraftTextureCurrentChanged: {
        if (draftTextureCurrent)
            reloadData()
    }
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

        delegate: Item {
            id: draftItem

            required property var modelData

            x: modelData.x * root.width
            y: modelData.y * root.height
            width: Math.max(1, modelData.width * root.width)
            height: Math.max(1, modelData.height * root.height)
            visible: !(root.editing && root.editingObjectId === modelData.objectId)

            readonly property real leftBleed: Math.min(root.fallbackBleed, x)
            readonly property real topBleed: Math.min(root.fallbackBleed, y)
            readonly property real rightBleed: Math.min(
                root.fallbackBleed,
                Math.max(0, root.width - x - width))
            readonly property real bottomBleed: Math.min(
                root.fallbackBleed,
                Math.max(0, root.height - y - height))

            Rectangle {
                x: -draftItem.leftBleed
                y: -draftItem.topBleed
                width: draftItem.width + draftItem.leftBleed + draftItem.rightBleed
                height: draftItem.height + draftItem.topBleed + draftItem.bottomBleed
                color: "#fffdfb"
                visible: !root.draftTextureReady
            }

            Text {
                x: 1
                y: (draftItem.height - height) / 2
                width: Math.max(1, draftItem.width - 2)
                height: Math.max(
                    draftItem.height + root.fallbackBleed * 2,
                    implicitHeight)
                text: draftItem.modelData.text
                color: draftItem.modelData.color
                font.family: draftItem.modelData.fontFamily
                font.pixelSize: Math.max(6, draftItem.modelData.fontSizeRatio * root.height)
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
        //% "PDF edit surface"
        Accessible.name: qsTrId("PdfEditSurface")
        //% "Use Tab or arrow keys to select content, then press Enter to edit text"
        Accessible.description: qsTrId("PdfEditSurfaceDescription")

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
        visible: root.interactionActive
            && !root.editing
            && root.hasCandidate(root.hoverCandidate)
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
            && !root.editing
            && root.controller.selectedPage === root.page
            && root.controller.selectedObjectId.length > 0
    }

    Rectangle {
        id: editorFallbackMask

        readonly property real originalX: root.controller.selectedX * root.width
        readonly property real originalY: root.controller.selectedY * root.height
        readonly property real originalWidth: root.controller.selectedWidth * root.width
        readonly property real originalHeight: root.controller.selectedHeight * root.height

        x: Math.max(0, originalX - root.fallbackBleed)
        y: Math.max(0, originalY - root.fallbackBleed)
        width: Math.max(1,
                        Math.min(root.width,
                                 originalX + originalWidth + root.fallbackBleed) - x)
        height: Math.max(1,
                         Math.min(root.height,
                                  originalY + originalHeight + root.fallbackBleed) - y)
        color: "#fffdfb"
        visible: editorFrame.visible && !root.editorUsesCleanTexture
    }

    Rectangle {
        id: editorFrame

        x: root.controller.selectedX * root.width
        y: root.controller.selectedY * root.height
        width: Math.max(80, root.controller.selectedWidth * root.width)
        height: Math.max(26, root.controller.selectedHeight * root.height)
        color: root.editorUsesCleanTexture ? "transparent" : "#fffdfb"
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
            //% "Enter to apply · Esc to cancel"
            text: qsTrId("ApplyOrCancelEditHint")
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
