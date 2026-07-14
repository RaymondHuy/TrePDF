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
    property string selectionActionsObjectId: ""
    property real selectCursorX: 0
    property real selectCursorY: 0
    property bool selectCursorVisible: false
    readonly property real fallbackBleed: 2
    readonly property real editBoxRadius: 4
    readonly property color editBoxColor: "#f5a55a"
    readonly property real editBoxLeftBleed: 3
    readonly property real editBoxRightBleed: 3
    readonly property real hoverBoxVerticalBleed: 3
    readonly property real editBoxVerticalBleed: 3
    readonly property real minimumTextPixelSize: 6
    readonly property bool interactionActive: !pooled
        && controller.tool !== PDFEditController.BrowseTool
    readonly property bool editorUsesCleanTexture: draftTextureReady
        && hasActiveDraft(editingObjectId)
    readonly property bool selectCursorReady: selectCursorImage.status === Image.Ready
    readonly property bool selectCursorActive: selectCursorReady
        && selectCursorVisible
        && !editing
    readonly property bool addTextCursorReady: addTextCursorImage.status === Image.Ready
    readonly property bool addTextCursorActive: addTextCursorReady
        && interactionActive
        && !editing
        && controller.tool === PDFEditController.AddTextTool
        && hitArea.containsMouse

    visible: !pooled && (interactionActive || drafts.length > 0)
    enabled: interactionActive

    function reloadData() {
        if (pooled) {
            drafts = []
            hoverCandidate = ({})
            hideSelectCursor()
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

                if (previousDraft.added === true)
                    continue
                else if (previousDraft.restoration === true)
                    displayedDrafts.push(previousDraft)
                else if (previousDraft.originalText !== undefined)
                    displayedDrafts.push(restorationOverlay(previousDraft))
            }
        }
        drafts = displayedDrafts
        if (!interactionActive) {
            hoverCandidate = ({})
            hideSelectCursor()
        }
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
            displayFontSizeRatio: draft.displayFontSizeRatio,
            fontStretch: draft.fontStretch,
            fontBold: draft.fontBold,
            hasBaseline: draft.hasBaseline,
            originX: draft.originX,
            baselineY: draft.baselineY,
            color: draft.color,
            restoration: true
        }
    }

    function hasCandidate(candidate) {
        return candidate !== null
            && candidate.objectId !== undefined
            && candidate.objectId.length > 0
    }

    function isTextBoxCandidate(candidate) {
        return hasCandidate(candidate)
            && (candidate.kind === "text"
                || candidate.kind === "addedText"
                || candidate.kind === "freeText")
    }

    function updateSelectCursor(x, y, candidate) {
        if (controller.tool !== PDFEditController.SelectTool
                || !isTextBoxCandidate(candidate)) {
            hideSelectCursor()
            return
        }

        selectCursorX = x
        selectCursorY = y
        selectCursorVisible = true
    }

    function hideSelectCursor() {
        selectCursorVisible = false
    }

    function updateHoverAtPosition(x, y) {
        const candidate = candidateAtPosition(x, y)
        hoverCandidate = candidate
        updateSelectCursor(x, y, candidate)
    }

    function candidateAtPosition(x, y) {
        if (!interactionActive
                || controller.tool === PDFEditController.AddTextTool
                || width <= 0
                || height <= 0) {
            return ({})
        }

        return controller.candidateAt(page, x / width, y / height)
    }

    function beginAddingText(x, y) {
        if (controller.tool !== PDFEditController.AddTextTool
                || width <= 0
                || height <= 0) {
            return
        }

        if (editing)
            commitEditing()

        const candidate = controller.addText(page, x / width, y / height)
        if (!hasCandidate(candidate))
            return

        editingObjectId = candidate.objectId
        editor.text = candidate.text
        editing = true
        Qt.callLater(function() {
            editor.forceActiveFocus()
        })
    }

    function selectCandidate(candidate) {
        if (editing && editingObjectId !== candidate.objectId)
            commitEditing()

        return controller.selectCandidate(page, candidate.objectId)
    }

    function showSelectionActions(x, y, candidate) {
        if (controller.tool !== PDFEditController.SelectTool
                || !candidate.editable
                || !selectCandidate(candidate)) {
            selectionActions.close()
            return
        }

        selectionActionsObjectId = candidate.objectId
        hideSelectCursor()
        selectionActions.openAt(x, y)
    }

    function beginEditing(candidate) {
        if (!candidate.editable
                || (controller.tool !== PDFEditController.EditTextTool
                    && controller.tool !== PDFEditController.SelectTool)) {
            return
        }

        if (!controller.selectCandidate(page, candidate.objectId))
            return

        hideSelectCursor()
        controller.beginSelectedTextEdit()
        editingObjectId = candidate.objectId
        editor.text = controller.selectedText
        editing = true
        Qt.callLater(function() {
            editor.forceActiveFocus()
            editor.deselect()
            editor.cursorPosition = editor.text.length
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

    function deleteSelectedText() {
        if (controller.selectedPage !== page || !controller.selectedEditable)
            return

        controller.commitSelectedText("")
        controller.clearSelection()
        reloadData()
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
        selectionActions.close()
        if (editing)
            commitEditing()
        reloadData()
    }

    onPooledChanged: {
        selectionActions.close()
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
            selectionActions.close()
            if (root.editing)
                root.commitEditing()
            root.hoverCandidate = ({})
            root.hideSelectCursor()
            root.reloadData()
        }

        function onSelectionChanged() {
            if (selectionActions.opened
                    && (root.controller.selectedPage !== root.page
                        || root.controller.selectedObjectId
                            !== root.selectionActionsObjectId)) {
                selectionActions.close()
            }
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

            readonly property real originalX: modelData.x * root.width
            readonly property real originalY: modelData.y * root.height
            readonly property real originalWidth: Math.max(1, modelData.width * root.width)
            readonly property real originalHeight: Math.max(1, modelData.height * root.height)
            readonly property real horizontalPadding: 2
            readonly property real measuredTextWidth: Math.max(
                draftTextMetrics.advanceWidth,
                draftTextMetrics.boundingRect.width)
            readonly property real expandedTextWidth: Math.max(
                originalWidth,
                measuredTextWidth + horizontalPadding)
            readonly property real textWidth: Math.min(root.width, expandedTextWidth)
            readonly property real textPageX: Math.max(
                0,
                Math.min(originalX, root.width - textWidth))
            readonly property real textHeight: Math.max(
                originalHeight + root.fallbackBleed * 2,
                draftText.implicitHeight)
            readonly property real fontPixelSize: Math.max(
                modelData.added === true ? root.minimumTextPixelSize : 1,
                modelData.displayFontSizeRatio * root.height)
            readonly property font displayFont: root.controller.fontForDisplay(
                modelData.fontFamily,
                modelData.fontStretch,
                modelData.fontBold,
                fontPixelSize)
            readonly property bool usesBaseline: modelData.hasBaseline === true
                && modelData.added !== true
            readonly property real textOriginX: modelData.originX * root.width
            readonly property real textBaselineY: modelData.baselineY * root.height
            readonly property real textPageY: originalY + (originalHeight - textHeight) / 2
            readonly property bool textNeedsFitting: expandedTextWidth > root.width
            readonly property bool hovered: root.hasCandidate(root.hoverCandidate)
                && root.hoverCandidate.objectId === modelData.objectId
            readonly property bool selected: root.controller.selectedPage === root.page
                && root.controller.selectedObjectId === modelData.objectId

            x: textPageX
            y: textPageY
            width: textWidth
            height: textHeight
            visible: !(root.editing && root.editingObjectId === modelData.objectId)
            z: modelData.added === true ? 1 : 0

            readonly property real leftBleed: Math.min(root.fallbackBleed, originalX)
            readonly property real topBleed: Math.min(root.fallbackBleed, originalY)
            readonly property real rightBleed: Math.min(
                root.fallbackBleed,
                Math.max(0, root.width - originalX - originalWidth))
            readonly property real bottomBleed: Math.min(
                root.fallbackBleed,
                Math.max(0, root.height - originalY - originalHeight))

            Rectangle {
                x: draftItem.originalX - draftItem.x - draftItem.leftBleed
                y: draftItem.originalY - draftItem.y - draftItem.topBleed
                width: draftItem.originalWidth
                    + draftItem.leftBleed
                    + draftItem.rightBleed
                height: draftItem.originalHeight
                    + draftItem.topBleed
                    + draftItem.bottomBleed
                color: "#fffdfb"
                visible: !root.draftTextureReady
                    && draftItem.modelData.added !== true
            }

            TextMetrics {
                id: draftTextMetrics

                text: draftItem.modelData.text
                font: draftItem.displayFont
            }

            Text {
                id: draftText

                x: 0
                y: draftItem.usesBaseline
                    ? draftItem.textBaselineY - draftItem.y - baselineOffset
                    : 0
                width: draftItem.width
                height: draftItem.usesBaseline ? implicitHeight : draftItem.height
                leftPadding: draftItem.usesBaseline
                    ? Math.max(0, draftItem.textOriginX - draftItem.x)
                    : draftItem.horizontalPadding / 2
                rightPadding: draftItem.usesBaseline ? 0 : draftItem.horizontalPadding / 2
                text: draftItem.modelData.text
                color: draftItem.modelData.color
                font: draftItem.displayFont
                fontSizeMode: draftItem.textNeedsFitting ? Text.HorizontalFit : Text.FixedSize
                minimumPixelSize: 1
                verticalAlignment: draftItem.usesBaseline ? Text.AlignTop : Text.AlignVCenter
                wrapMode: Text.NoWrap
                elide: Text.ElideNone
            }

            Rectangle {
                anchors.fill: parent
                radius: root.editBoxRadius
                color: draftItem.selected ? "#14d97a32" : "transparent"
                border.width: 2
                border.color: root.editBoxColor
                visible: draftItem.modelData.added === true
                    && root.interactionActive
                    && !root.editing
                    && (draftItem.selected || draftItem.hovered)
            }

            MouseArea {
                anchors.fill: parent
                enabled: draftItem.modelData.added === true
                    && root.interactionActive
                    && root.controller.tool !== PDFEditController.AddTextTool
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                preventStealing: false
                cursorShape: root.controller.tool === PDFEditController.SelectTool
                    ? root.selectCursorActive ? Qt.BlankCursor : Qt.ArrowCursor
                    : Qt.IBeamCursor
                onEntered: {
                    root.updateSelectCursor(
                        draftItem.x + mouseX,
                        draftItem.y + mouseY,
                        draftItem.modelData)
                    root.hoverCandidate = draftItem.modelData
                }
                onPositionChanged: mouse => {
                    root.updateSelectCursor(
                        draftItem.x + mouse.x,
                        draftItem.y + mouse.y,
                        draftItem.modelData)
                    root.hoverCandidate = draftItem.modelData
                }
                onExited: {
                    root.hideSelectCursor()
                    if (root.hasCandidate(root.hoverCandidate)
                            && root.hoverCandidate.objectId
                                === draftItem.modelData.objectId) {
                        root.hoverCandidate = ({})
                    }
                }
                onPressed: hitArea.forceActiveFocus()
                onClicked: mouse => {
                    if (root.controller.tool === PDFEditController.SelectTool) {
                        root.showSelectionActions(
                            draftItem.x + mouse.x,
                            draftItem.y + mouse.y,
                            draftItem.modelData)
                    } else {
                        root.selectCandidate(draftItem.modelData)
                    }
                }
                onDoubleClicked: root.beginEditing(draftItem.modelData)
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
        cursorShape: root.selectCursorActive || root.addTextCursorActive
            ? Qt.BlankCursor
            : root.controller.tool === PDFEditController.AddTextTool
            ? Qt.IBeamCursor
            : root.hasCandidate(root.hoverCandidate)
            ? root.hoverCandidate.editable ? Qt.IBeamCursor : Qt.ForbiddenCursor
            : Qt.ArrowCursor
        activeFocusOnTab: true
        Accessible.role: Accessible.Pane
        //% "PDF edit surface"
        Accessible.name: qsTrId("PdfEditSurface")
        //% "Use Tab or arrow keys to select content, then press Enter to edit text"
        Accessible.description: qsTrId("PdfEditSurfaceDescription")

        onEntered: root.updateHoverAtPosition(mouseX, mouseY)
        onPositionChanged: mouse => root.updateHoverAtPosition(mouse.x, mouse.y)
        onExited: {
            root.hideSelectCursor()
            root.hoverCandidate = ({})
        }
        onPressed: forceActiveFocus()
        onClicked: mouse => {
            if (root.controller.tool === PDFEditController.AddTextTool) {
                root.beginAddingText(mouse.x, mouse.y)
                return
            }

            root.hoverCandidate = root.candidateAtPosition(mouse.x, mouse.y)
            if (root.hasCandidate(root.hoverCandidate)) {
                if (root.controller.tool === PDFEditController.SelectTool
                        && root.hoverCandidate.editable) {
                    root.showSelectionActions(
                        mouse.x,
                        mouse.y,
                        root.hoverCandidate)
                } else {
                    selectionActions.close()
                    root.selectCandidate(root.hoverCandidate)
                }
            } else {
                selectionActions.close()
                root.controller.clearSelection()
            }
        }
        onDoubleClicked: mouse => {
            if (root.controller.tool === PDFEditController.AddTextTool)
                return

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

    Popup {
        id: selectionActions

        parent: root
        width: 136
        height: 62
        padding: 4
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        function openAt(pointX, pointY) {
            const margin = 4
            const gap = 8
            x = Math.max(
                margin,
                Math.min(root.width - width - margin, pointX - width / 2))
            const aboveY = pointY - height - gap
            y = aboveY >= margin
                ? aboveY
                : Math.max(
                    margin,
                    Math.min(root.height - height - margin, pointY + gap))
            open()
        }

        onClosed: root.selectionActionsObjectId = ""

        background: Rectangle {
            radius: 6
            color: root.theme.ribbonSurface
            border.width: 1
            border.color: root.theme.ribbonBorder
        }

        contentItem: Row {
            spacing: 2

            ToolButton {
                id: editAction

                width: 63
                height: 54
                display: AbstractButton.TextUnderIcon
                spacing: 2
                //% "Edit"
                text: qsTrId("Edit")
                icon.source: "../assets/icons/edit-text.svg"
                icon.width: 24
                icon.height: 24
                icon.color: "transparent"
                font.pixelSize: 10

                background: Rectangle {
                    radius: 4
                    color: editAction.down
                        ? "#f5dfcf"
                        : editAction.hovered
                            ? root.theme.foxitPaleOrange
                            : "transparent"
                }

                onClicked: {
                    selectionActions.close()
                    root.beginSelectedEditing()
                }
            }

            ToolButton {
                id: deleteAction

                width: 63
                height: 54
                display: AbstractButton.TextUnderIcon
                spacing: 2
                //% "Delete"
                text: qsTrId("Delete")
                icon.source: "../assets/icons/remove-text.svg"
                icon.width: 24
                icon.height: 24
                icon.color: "transparent"
                font.pixelSize: 10

                background: Rectangle {
                    radius: 4
                    color: deleteAction.down
                        ? "#f4d9d7"
                        : deleteAction.hovered
                            ? "#fff0ef"
                            : "transparent"
                }

                onClicked: {
                    selectionActions.close()
                    root.deleteSelectedText()
                }
            }
        }
    }

    Image {
        id: selectCursorImage

        x: root.selectCursorX - 3
        y: root.selectCursorY - 3
        width: 22
        height: 22
        source: "../assets/icons/mouse-pointer-filled-orange.svg"
        sourceSize.width: 22
        sourceSize.height: 22
        fillMode: Image.PreserveAspectFit
        smooth: true
        visible: root.selectCursorActive
            && root.interactionActive
            && root.controller.tool === PDFEditController.SelectTool
        z: 4
        Accessible.ignored: true
    }

    Image {
        id: addTextCursorImage

        x: hitArea.mouseX - width / 2
        y: hitArea.mouseY - height / 2
        width: 22
        height: 22
        source: "../assets/icons/add.svg"
        sourceSize.width: 22
        sourceSize.height: 22
        fillMode: Image.PreserveAspectFit
        smooth: true
        visible: root.addTextCursorActive
        z: 4
        Accessible.ignored: true
    }

    Rectangle {
        readonly property real originalX: root.hasCandidate(root.hoverCandidate)
            ? root.hoverCandidate.x * root.width
            : 0
        readonly property real originalY: root.hasCandidate(root.hoverCandidate)
            ? root.hoverCandidate.y * root.height
            : 0
        readonly property real originalWidth: root.hasCandidate(root.hoverCandidate)
            ? Math.max(1, root.hoverCandidate.width * root.width)
            : 0
        readonly property real originalHeight: root.hasCandidate(root.hoverCandidate)
            ? Math.max(1, root.hoverCandidate.height * root.height)
            : 0

        x: root.hasCandidate(root.hoverCandidate)
            ? Math.max(0, originalX - root.editBoxLeftBleed)
            : 0
        y: root.hasCandidate(root.hoverCandidate)
            ? Math.max(0, originalY - root.hoverBoxVerticalBleed)
            : 0
        width: root.hasCandidate(root.hoverCandidate)
            ? Math.max(
                1,
                Math.min(
                    root.width,
                    originalX + originalWidth + root.editBoxRightBleed) - x)
            : 0
        height: root.hasCandidate(root.hoverCandidate)
            ? Math.max(
                1,
                Math.min(
                    root.height,
                    originalY + originalHeight + root.hoverBoxVerticalBleed) - y)
            : 0
        radius: root.editBoxRadius
        color: "transparent"
        border.width: 2
        border.color: root.hasCandidate(root.hoverCandidate) && root.hoverCandidate.editable
            ? root.editBoxColor
            : "#a5a7aa"
        z: 2
        visible: root.interactionActive
            && !root.editing
            && root.hasCandidate(root.hoverCandidate)
            && root.hoverCandidate.added !== true
    }

    Rectangle {
        readonly property real originalX: root.controller.selectedX * root.width
        readonly property real originalY: root.controller.selectedY * root.height
        readonly property real originalWidth: root.controller.selectedWidth * root.width
        readonly property real originalHeight: root.controller.selectedHeight * root.height

        x: Math.max(0, originalX - root.editBoxLeftBleed)
        y: Math.max(0, originalY - root.editBoxVerticalBleed)
        width: Math.max(
            1,
            Math.min(
                root.width,
                originalX + originalWidth + root.editBoxRightBleed) - x)
        height: Math.max(
            1,
            Math.min(
                root.height,
                originalY + originalHeight + root.editBoxVerticalBleed) - y)
        radius: root.editBoxRadius
        color: "#14d97a32"
        border.width: 2
        border.color: root.controller.selectedEditable
            ? root.editBoxColor
            : "#8a8d92"
        z: 2
        visible: root.interactionActive
            && !root.editing
            && root.controller.selectedPage === root.page
            && root.controller.selectedObjectId.length > 0
            && root.controller.selectedKind !== "addedText"
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
        z: 2
        visible: editorFrame.visible
            && root.controller.selectedKind !== "addedText"
            && !root.editorUsesCleanTexture
    }

    Rectangle {
        id: editorFrame

        readonly property real originalX: root.controller.selectedX * root.width
        readonly property real originalY: root.controller.selectedY * root.height
        readonly property real originalWidth: root.controller.selectedWidth * root.width
        readonly property real originalHeight: root.controller.selectedHeight * root.height
        readonly property bool editsAddedText: root.controller.selectedKind === "addedText"
        readonly property real leftBleed: editsAddedText
            ? 0
            : root.editBoxLeftBleed
        readonly property real rightBleed: editsAddedText
            ? 0
            : root.editBoxRightBleed
        readonly property real verticalBleed: editsAddedText
            ? 0
            : root.editBoxVerticalBleed
        readonly property real baseFontPixelSize: Math.max(
            editsAddedText ? root.minimumTextPixelSize : 1,
            root.controller.selectedDisplayFontSizeRatio * root.height)
        readonly property font displayFont: root.controller.fontForDisplay(
            root.controller.selectedFontFamily,
            root.controller.selectedFontStretch,
            root.controller.selectedFontBold,
            baseFontPixelSize)
        readonly property bool usesBaseline: !editsAddedText
            && root.controller.selectedHasBaseline
        readonly property real textOriginX: root.controller.selectedOriginX * root.width
        readonly property real textBaselineY: root.controller.selectedBaselineY * root.height
        readonly property real inputMargin: editsAddedText
            ? Math.min(1, width / 4, height / 4)
            : Math.min(4, width / 4, height / 4)
        readonly property real measuredTextWidth: Math.max(
            editorTextMetrics.advanceWidth,
            editorTextMetrics.boundingRect.width)
        readonly property real baseDesiredWidth: editsAddedText
            ? Math.max(originalWidth, measuredTextWidth + 2)
            : Math.max(80, originalWidth)
        readonly property real desiredWidth: baseDesiredWidth + leftBleed + rightBleed
        readonly property real baseDesiredHeight: editsAddedText
            ? Math.max(
                originalHeight + root.fallbackBleed * 2,
                editor.implicitHeight)
            : Math.max(17, originalHeight)
        readonly property real desiredHeight: baseDesiredHeight + verticalBleed * 2

        x: Math.max(
            0,
            Math.min(originalX - leftBleed, root.width - width))
        y: editsAddedText
            ? originalY + (originalHeight - height) / 2
            : Math.max(
                0,
                Math.min(originalY - verticalBleed, root.height - height))
        width: Math.min(root.width, desiredWidth)
        height: editsAddedText ? desiredHeight : Math.min(root.height, desiredHeight)
        color: editsAddedText || root.editorUsesCleanTexture
            ? "transparent"
            : "#fffdfb"
        border.color: root.editBoxColor
        border.width: 2
        radius: root.editBoxRadius
        z: 2
        visible: root.editing
            && root.controller.selectedPage === root.page
            && root.controller.selectedEditable

        TextMetrics {
            id: editorTextMetrics

            text: editor.text
            font: editorFrame.displayFont
        }

        TextInput {
            id: editor

            x: editorFrame.usesBaseline ? 0 : editorFrame.inputMargin
            y: editorFrame.usesBaseline
                ? editorFrame.textBaselineY - editorFrame.y - baselineOffset
                : editorFrame.inputMargin
            width: editorFrame.usesBaseline
                ? parent.width
                : Math.max(1, parent.width - editorFrame.inputMargin * 2)
            height: editorFrame.usesBaseline
                ? implicitHeight
                : Math.max(1, parent.height - editorFrame.inputMargin * 2)
            leftPadding: editorFrame.usesBaseline
                ? Math.max(0, editorFrame.textOriginX - editorFrame.x)
                : 0
            color: root.controller.selectedColor
            selectionColor: "#DCEBFF"
            selectedTextColor: root.theme.ribbonTextColor
            font: editorFrame.displayFont
            selectByMouse: true
            verticalAlignment: editorFrame.usesBaseline
                ? TextInput.AlignTop
                : TextInput.AlignVCenter
            clip: !editorFrame.usesBaseline
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
        z: 2
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
        z: 2
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
