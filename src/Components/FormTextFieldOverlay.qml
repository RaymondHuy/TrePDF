pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFFormDesignerController controller
    required property int page
    property bool pooled: false
    property bool formsActive: false
    property bool editActive: false
    property bool fieldsVisible: false
    property bool fieldsInteractive: false
    property bool textFieldToolActive: false
    property rect placementRect: Qt.rect(0, 0, 0, 0)
    property real placementStartX: 0
    property real placementStartY: 0
    property string pendingFocusFieldId: ""
    property string loadedEditorFieldId: ""
    readonly property real resizeHandleSize: 8
    readonly property real resizeHandleHitSize: 24
    readonly property real minimumResizeWidth: 60
    readonly property real minimumResizeHeight: 24

    function resetPlacement() {
        placementRect = Qt.rect(0, 0, 0, 0)
    }

    function updatePlacement(x, y) {
        const boundedX = Math.max(0, Math.min(width, x))
        const boundedY = Math.max(0, Math.min(height, y))
        placementRect = Qt.rect(
            Math.min(placementStartX, boundedX),
            Math.min(placementStartY, boundedY),
            Math.abs(boundedX - placementStartX),
            Math.abs(boundedY - placementStartY))
    }

    function resizedFieldRect(startRect, horizontal, vertical, deltaX, deltaY) {
        const pageWidth = Math.max(1, root.width)
        const pageHeight = Math.max(1, root.height)
        const minimumWidth = Math.min(
            Number(startRect.width),
            root.minimumResizeWidth / pageWidth)
        const minimumHeight = Math.min(
            Number(startRect.height),
            root.minimumResizeHeight / pageHeight)
        let left = Number(startRect.x)
        let top = Number(startRect.y)
        let right = left + Number(startRect.width)
        let bottom = top + Number(startRect.height)

        if (horizontal < 0) {
            left = Math.max(
                0,
                Math.min(
                    right - minimumWidth,
                    left + deltaX / pageWidth))
        } else if (horizontal > 0) {
            right = Math.min(
                1,
                Math.max(
                    left + minimumWidth,
                    right + deltaX / pageWidth))
        }

        if (vertical < 0) {
            top = Math.max(
                0,
                Math.min(
                    bottom - minimumHeight,
                    top + deltaY / pageHeight))
        } else if (vertical > 0) {
            bottom = Math.min(
                1,
                Math.max(
                    top + minimumHeight,
                    bottom + deltaY / pageHeight))
        }

        return Qt.rect(left, top, right - left, bottom - top)
    }

    function syncActiveTextEditor() {
        if (activeTextEditorLoader.status !== Loader.Ready)
            return

        const editor = activeTextEditorLoader.item as TextField
        if (!editor)
            return
        const fieldId = formFieldLayer.activeFieldId
        const fieldChanged = root.loadedEditorFieldId !== fieldId
        if (!fieldChanged
                && editor.text === formFieldLayer.activeFieldValue)
            return
        if (fieldChanged && editor.activeFocus
                && root.loadedEditorFieldId.length > 0) {
            root.controller.setTextFieldFocused(
                root.loadedEditorFieldId,
                false)
        }
        root.loadedEditorFieldId = fieldId
        if (editor.text !== formFieldLayer.activeFieldValue)
            editor.text = formFieldLayer.activeFieldValue
        if (fieldChanged && editor.activeFocus && fieldId.length > 0)
            root.controller.setTextFieldFocused(fieldId, true)
    }

    function focusPendingTextField() {
        if (pendingFocusFieldId.length === 0
                || formFieldLayer.activeFieldId !== pendingFocusFieldId
                || activeTextEditorLoader.status !== Loader.Ready)
            return

        const editor = activeTextEditorLoader.item as TextField
        if (!editor)
            return
        pendingFocusFieldId = ""
        editor.forceActiveFocus()
    }

    function finishPlacement() {
        if (width <= 0 || height <= 0) {
            resetPlacement()
            return
        }

        let fieldRect = placementRect
        const clickPlacement = fieldRect.width < 4 && fieldRect.height < 4
        if (clickPlacement) {
            const defaultWidth = Math.min(160, width)
            const defaultHeight = Math.min(28, height)
            fieldRect = Qt.rect(
                Math.min(placementStartX, width - defaultWidth),
                Math.min(placementStartY, height - defaultHeight),
                defaultWidth,
                defaultHeight)
        } else if (fieldRect.width < 20 || fieldRect.height < 18) {
            resetPlacement()
            return
        }

        const fieldId = controller.addTextField(
            page,
            fieldRect.x / width,
            fieldRect.y / height,
            fieldRect.width / width,
            fieldRect.height / height)
        resetPlacement()
        if (fieldId.length === 0)
            return

        pendingFocusFieldId = fieldId
        controller.selectTextField(fieldId)
        Qt.callLater(function() {
            root.focusPendingTextField()
        })
    }

    function prepareForSessionChange() {
        pendingFocusFieldId = ""
        activeFieldFrame.cancelResize()
        resetPlacement()
    }

    onPooledChanged: prepareForSessionChange()
    onFormsActiveChanged: {
        if (!formsActive) {
            prepareForSessionChange()
            controller.clearTextFieldSelection()
        }
    }
    onFieldsVisibleChanged: {
        if (!fieldsVisible) {
            activeFieldFrame.cancelResize()
            controller.clearTextFieldFocus()
        }
    }
    onFieldsInteractiveChanged: {
        if (!fieldsInteractive) {
            activeFieldFrame.cancelResize()
            controller.clearTextFieldFocus()
        }
    }
    onTextFieldToolActiveChanged: {
        if (!textFieldToolActive)
            resetPlacement()
    }

    MouseArea {
        id: placementArea

        anchors.fill: parent
        enabled: root.formsActive
            && root.textFieldToolActive
            && !root.pooled
        acceptedButtons: Qt.LeftButton
        cursorShape: enabled ? Qt.CrossCursor : Qt.ArrowCursor
        preventStealing: true
        z: 0

        onPressed: mouse => {
            root.placementStartX = Math.max(0, Math.min(width, mouse.x))
            root.placementStartY = Math.max(0, Math.min(height, mouse.y))
            root.placementRect = Qt.rect(
                root.placementStartX,
                root.placementStartY,
                0,
                0)
        }
        onPositionChanged: mouse => {
            if (pressed)
                root.updatePlacement(mouse.x, mouse.y)
        }
        onReleased: mouse => {
            root.updatePlacement(mouse.x, mouse.y)
            root.finishPlacement()
        }
        onCanceled: root.resetPlacement()
    }

    Rectangle {
        x: root.placementRect.x
        y: root.placementRect.y
        width: root.placementRect.width
        height: root.placementRect.height
        visible: placementArea.pressed
            && width > 0
            && height > 0
        color: "#22f58220"
        border.color: root.theme.foxitDarkOrange
        border.width: 1
        z: 1
    }

    component EdgeResizeHandle: Item {
        id: resizeHandle

        required property int horizontal
        required property int vertical
        required property int resizeCursor
        property rect dragStartRect: Qt.rect(0, 0, 0, 0)
        property point dragStartPosition: Qt.point(0, 0)

        x: horizontal < 0
            ? -width / 2
            : horizontal > 0
                ? activeFieldFrame.width - width / 2
                : (activeFieldFrame.width - width) / 2
        y: vertical < 0
            ? -height / 2
            : vertical > 0
                ? activeFieldFrame.height - height / 2
                : (activeFieldFrame.height - height) / 2
        width: root.resizeHandleHitSize
        height: root.resizeHandleHitSize
        visible: (root.formsActive || root.editActive)
            && root.fieldsInteractive
            && !root.pooled
        z: 20

        Rectangle {
            anchors.centerIn: parent
            width: root.resizeHandleSize
            height: root.resizeHandleSize
            radius: width / 2
            color: "#f5a55a"
            border.width: 1
            border.color: "#fffdfb"
            Accessible.ignored: true
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: resizeHandle.resizeCursor
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            //% "Resize text box"
            Accessible.name: qsTrId("ResizeTextBox")

            onPressed: mouse => {
                const fieldId = formFieldLayer.activeFieldId
                if (fieldId.length === 0) {
                    mouse.accepted = false
                    return
                }

                resizeHandle.dragStartRect
                    = formFieldLayer.activeFieldRect
                resizeHandle.dragStartPosition = mapToItem(
                    root,
                    mouse.x,
                    mouse.y)
                activeFieldFrame.beginResize(
                    fieldId,
                    resizeHandle.dragStartRect)
                mouse.accepted = true
            }
            onPositionChanged: mouse => {
                if (!pressed || !activeFieldFrame.resizing)
                    return

                const currentPosition = mapToItem(
                    root,
                    mouse.x,
                    mouse.y)
                activeFieldFrame.updateResize(
                    root.resizedFieldRect(
                        resizeHandle.dragStartRect,
                        resizeHandle.horizontal,
                        resizeHandle.vertical,
                        currentPosition.x
                            - resizeHandle.dragStartPosition.x,
                        currentPosition.y
                            - resizeHandle.dragStartPosition.y))
            }
            onReleased: activeFieldFrame.finishResize()
            onCanceled: activeFieldFrame.cancelResize()

            Keys.onPressed: event => {
                const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
                let deltaX = 0
                let deltaY = 0
                if (resizeHandle.horizontal !== 0) {
                    if (event.key === Qt.Key_Left)
                        deltaX = -step
                    else if (event.key === Qt.Key_Right)
                        deltaX = step
                }
                if (resizeHandle.vertical !== 0) {
                    if (event.key === Qt.Key_Up)
                        deltaY = -step
                    else if (event.key === Qt.Key_Down)
                        deltaY = step
                }
                if (deltaX === 0 && deltaY === 0)
                    return

                const fieldId = formFieldLayer.activeFieldId
                const startRect = formFieldLayer.activeFieldRect
                activeFieldFrame.beginResize(fieldId, startRect)
                activeFieldFrame.updateResize(
                    root.resizedFieldRect(
                        startRect,
                        resizeHandle.horizontal,
                        resizeHandle.vertical,
                        deltaX,
                        deltaY))
                activeFieldFrame.finishResize()
                event.accepted = true
            }
        }
    }

    FormFieldLayer {
        id: formFieldLayer

        anchors.fill: parent
        controller: root.controller
        page: root.page
        pooled: root.pooled
        formsActive: root.formsActive
        sourceFieldsVisible: root.formsActive || root.editActive
        fieldsVisible: root.fieldsVisible
        fieldsInteractive: root.fieldsInteractive
        accentColor: root.theme.foxitDarkOrange
        z: 2

        onFieldActivated: fieldId => {
            root.pendingFocusFieldId = fieldId
            Qt.callLater(function() {
                root.focusPendingTextField()
            })
        }
        onActiveFieldChanged: root.syncActiveTextEditor()
    }

    Item {
        id: activeFieldFrame

        property bool resizing: false
        property rect resizedRect: Qt.rect(0, 0, 0, 0)
        property string resizingFieldId: ""

        x: (resizing
            ? resizedRect.x
            : formFieldLayer.activeFieldRect.x) * root.width
        y: (resizing
            ? resizedRect.y
            : formFieldLayer.activeFieldRect.y) * root.height
        width: (resizing
            ? resizedRect.width
            : formFieldLayer.activeFieldRect.width) * root.width
        height: (resizing
            ? resizedRect.height
            : formFieldLayer.activeFieldRect.height) * root.height
        visible: formFieldLayer.activeFieldId.length > 0
        z: 4

        function beginResize(fieldId, fieldRect) {
            if (fieldId.length === 0 || fieldRect.width <= 0
                    || fieldRect.height <= 0)
                return

            resizingFieldId = fieldId
            resizedRect = fieldRect
            resizing = true
        }

        function updateResize(fieldRect) {
            if (resizing)
                resizedRect = fieldRect
        }

        function finishResize() {
            if (!resizing)
                return

            const fieldId = resizingFieldId
            const fieldRect = resizedRect
            if (fieldId.length > 0 && root.width > 0 && root.height > 0) {
                root.controller.setTextFieldRect(
                    fieldId,
                    fieldRect.x,
                    fieldRect.y,
                    fieldRect.width,
                    fieldRect.height)
            }
            resizing = false
            resizingFieldId = ""
        }

        function cancelResize() {
            resizing = false
            resizingFieldId = ""
        }

        Rectangle {
            anchors.fill: parent
            visible: formFieldLayer.activeFieldType
                !== PDFFormDesignerController.Text
            color: "transparent"
            border.color: root.theme.foxitDarkOrange
            border.width: root.formsActive ? 2 : 0
            radius: 5
        }

        Loader {
            id: activeTextEditorLoader

            anchors.fill: parent
            active: activeFieldFrame.visible
                && formFieldLayer.activeFieldType
                    === PDFFormDesignerController.Text
            sourceComponent: activeTextEditorComponent

            onLoaded: {
                root.loadedEditorFieldId = ""
                root.syncActiveTextEditor()
                root.focusPendingTextField()
            }
            onActiveChanged: {
                if (!active && root.loadedEditorFieldId.length > 0) {
                    root.controller.setTextFieldFocused(
                        root.loadedEditorFieldId,
                        false)
                    root.loadedEditorFieldId = ""
                }
            }
        }

        ToolButton {
            id: deleteFieldButton

            x: activeFieldFrame.width - width / 2
            y: -height / 2
            width: 20
            height: 20
            visible: root.formsActive
                && !formFieldLayer.activeFieldSourceBacked
                && !activeFieldFrame.resizing
            enabled: visible
            focusPolicy: Qt.NoFocus
            text: "\u00d7"
            padding: 0
            font.pixelSize: 16
            font.bold: true
            z: 30
            //% "Delete"
            Accessible.name: qsTrId("Delete")
            onClicked: root.controller.removeTextField(
                formFieldLayer.activeFieldId)

            background: Rectangle {
                radius: 10
                color: deleteFieldButton.down
                    ? "#f5dfcf"
                    : deleteFieldButton.hovered
                        ? root.theme.foxitPaleOrange
                        : "white"
                border.color: root.theme.foxitDarkOrange
                border.width: 1
            }
        }

        EdgeResizeHandle {
            horizontal: 0
            vertical: -1
            resizeCursor: Qt.SizeVerCursor
        }

        EdgeResizeHandle {
            horizontal: 1
            vertical: 0
            resizeCursor: Qt.SizeHorCursor
        }

        EdgeResizeHandle {
            horizontal: 0
            vertical: 1
            resizeCursor: Qt.SizeVerCursor
        }

        EdgeResizeHandle {
            horizontal: -1
            vertical: 0
            resizeCursor: Qt.SizeHorCursor
        }
    }

    Component {
        id: activeTextEditorComponent

        TextField {
            id: activeTextEditor

            enabled: root.fieldsInteractive
                && (root.formsActive
                    || !(formFieldLayer.activeFieldFlags
                         & PDFFormDesignerController.ReadOnly))
            clip: false
            selectByMouse: true
            activeFocusOnPress: true
            leftPadding: 7
            rightPadding: 7
            topPadding: 2
            bottomPadding: 2
            color: "#202124"
            font.pixelSize: Math.max(10, Math.min(14, height * 0.5))
            //% "Enter text"
            placeholderText: qsTrId("EnterFormText")

            onTextEdited: {
                if (root.loadedEditorFieldId.length > 0)
                    root.controller.setTextFieldText(
                        root.loadedEditorFieldId,
                        text)
            }
            onActiveFocusChanged: {
                if (root.loadedEditorFieldId.length > 0)
                    root.controller.setTextFieldFocused(
                        root.loadedEditorFieldId,
                        activeFocus)
            }

            TapHandler {
                enabled: root.formsActive
                acceptedButtons: Qt.LeftButton
                onTapped: root.controller.selectTextField(
                    root.loadedEditorFieldId)
            }

            background: Rectangle {
                color: "white"
                border.color: activeTextEditor.activeFocus
                        || root.formsActive
                    ? root.theme.foxitDarkOrange
                    : "#7f8c99"
                border.width: activeTextEditor.activeFocus
                        || root.formsActive
                    ? 2
                    : 1
                radius: 0
            }
        }
    }
}
