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
        if (!fieldsVisible)
            controller.clearTextFieldFocus()
    }
    onFieldsInteractiveChanged: {
        if (!fieldsInteractive)
            controller.clearTextFieldFocus()
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
        property real resizedWidth: 0
        property real resizedHeight: 0

        x: formFieldLayer.activeFieldRect.x * root.width
        y: formFieldLayer.activeFieldRect.y * root.height
        width: resizing
            ? resizedWidth
            : formFieldLayer.activeFieldRect.width * root.width
        height: resizing
            ? resizedHeight
            : formFieldLayer.activeFieldRect.height * root.height
        visible: formFieldLayer.activeFieldId.length > 0
        z: 4

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
            z: 20
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

        Rectangle {
            id: resizeHandle

            x: activeFieldFrame.width - width / 2
            y: activeFieldFrame.height - height / 2
            width: 12
            height: 12
            visible: root.formsActive
                && !formFieldLayer.activeFieldSourceBacked
            radius: 3
            color: "white"
            border.color: root.theme.foxitDarkOrange
            border.width: 2
            z: 20

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.SizeFDiagCursor
                preventStealing: true

                onPressed: {
                    activeFieldFrame.resizedWidth = activeFieldFrame.width
                    activeFieldFrame.resizedHeight = activeFieldFrame.height
                    activeFieldFrame.resizing = true
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return

                    const position = mapToItem(root, mouse.x, mouse.y)
                    const maximumWidth = root.width - activeFieldFrame.x
                    const maximumHeight = root.height - activeFieldFrame.y
                    const minimumWidth = Math.min(60, maximumWidth)
                    const minimumHeight = Math.min(24, maximumHeight)
                    activeFieldFrame.resizedWidth = Math.max(
                        minimumWidth,
                        Math.min(maximumWidth,
                                 position.x - activeFieldFrame.x))
                    activeFieldFrame.resizedHeight = Math.max(
                        minimumHeight,
                        Math.min(maximumHeight,
                                 position.y - activeFieldFrame.y))
                }
                onReleased: {
                    root.controller.setTextFieldRect(
                        formFieldLayer.activeFieldId,
                        activeFieldFrame.x / root.width,
                        activeFieldFrame.y / root.height,
                        activeFieldFrame.resizedWidth / root.width,
                        activeFieldFrame.resizedHeight / root.height)
                    activeFieldFrame.resizing = false
                }
                onCanceled: activeFieldFrame.resizing = false
            }
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
                radius: 5
            }
        }
    }
}
