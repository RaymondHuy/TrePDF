pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root
    required property TableCellEditing editingState
    required property AppTheme theme
    required property real pointScale
    property int editorGeneration: -1
    readonly property real layoutScale: Math.max(0.01, pointScale)
    readonly property rect normalizedCell: editingState.cellRect

    function synchronize() {
        if (!input.initialized || input.synchronizing || !editingState.active
                || editorGeneration !== editingState.focusGeneration)
            return
        input.synchronizing = true
        const snapshot = editingState.controller.tableCellEditorState(input.textDocument)
        if (snapshot.text !== undefined) {
            editingState.text = snapshot.text
            editingState.cellFormat = snapshot.format
            editingState.editorState = snapshot
            editingState.fontFamily = snapshot.fontFamily
            editingState.fontSize = snapshot.fontSize
            editingState.textRotation = snapshot.rotation
            editingState.bold = snapshot.bold
            editingState.italic = snapshot.italic
            editingState.underline = snapshot.underline
        }
        input.synchronizing = false
    }

    function applyFormatting(property, value, restoreFocus = true) {
        if (!editingState.active || editorGeneration !== editingState.focusGeneration)
            return
        synchronize()
        input.synchronizing = true
        const error = editingState.controller.applyTableCellEditorFormat(
            input.textDocument, property, value)
        input.synchronizing = false
        synchronize()
        editingState.errorText = error
        if (restoreFocus) {
            input.forceActiveFocus()
            textViewport.revealCursor()
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: root.editingState.commit()
    }

    Rectangle {
        id: cellFrame
        objectName: "tableCellInlineEditor"
        x: root.normalizedCell.x * root.width
        y: root.normalizedCell.y * root.height
        width: root.normalizedCell.width * root.width
        height: root.normalizedCell.height * root.height
        color: "white"
        border.width: 1
        border.color: root.editingState.errorText.length > 0 ? "#b3261e" : root.theme.foxitOrange

        Rectangle {
            z: -1
            x: -2 * root.layoutScale
            y: -2 * root.layoutScale
            width: parent.width + 4 * root.layoutScale
            height: parent.height + 4 * root.layoutScale
            color: "white"
        }

        Item {
            id: rotatedViewport
            objectName: "tableCellRotatedViewport"
            readonly property int textRotation: root.editingState.textRotation
            readonly property bool sideways: textRotation === 90 || textRotation === 270
            x: textRotation === 90 || textRotation === 180 ? cellFrame.width : 0
            y: textRotation === 180 || textRotation === 270 ? cellFrame.height : 0
            width: sideways ? cellFrame.height : cellFrame.width
            height: sideways ? cellFrame.width : cellFrame.height
            rotation: textRotation
            transformOrigin: Item.TopLeft

            Flickable {
                id: textViewport
                x: 0
                y: root.layoutScale
                width: rotatedViewport.width / root.layoutScale
                height: Math.max(1, rotatedViewport.height / root.layoutScale - 1)
                scale: root.layoutScale
                transformOrigin: Item.TopLeft
                clip: true
                contentWidth: width
                contentHeight: Math.max(height, input.contentHeight)
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height

                function revealCursor() {
                    const cursor = input.cursorRectangle
                    if (cursor.y < contentY)
                        contentY = cursor.y
                    else if (cursor.y + cursor.height > contentY + height)
                        contentY = cursor.y + cursor.height - height
                }

                TextArea {
                    id: input
                    objectName: "tableCellTextInput"
                    property bool initialized: false
                    property bool synchronizing: false
                    width: textViewport.width
                    height: Math.max(textViewport.height, contentHeight)
                    padding: 0
                    background: null
                    color: "#000000"
                    selectionColor: "#f4ba83"
                    selectedTextColor: "#000000"
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                    selectByMouse: true
                    persistentSelection: true
                    font: root.editingState.controller.fontForDisplay(
                        root.editingState.cellFormat.defaultStyle.family || root.editingState.fontFamily,
                        100, false, false, false, root.editingState.cellFormat.defaultStyle.fontSize || 12)
                    //% "Cell text"
                    Accessible.name: qsTrId("TableCellText")
                    onTextChanged: {
                        if (initialized && !synchronizing) {
                            root.editingState.errorText = ""
                            Qt.callLater(root.synchronize)
                        }
                    }
                    onCursorPositionChanged: Qt.callLater(root.synchronize)
                    onSelectionStartChanged: Qt.callLater(root.synchronize)
                    onSelectionEndChanged: Qt.callLater(root.synchronize)
                    onActiveFocusChanged: {
                        root.editingState.inputFocused = activeFocus
                        if (!activeFocus)
                            root.synchronize()
                    }
                    onCursorRectangleChanged: textViewport.revealCursor()
                    Component.onCompleted: {
                        text = root.editingState.text
                        const saved = root.editingState.editorState
                        const position = Math.min(Number(saved.cursorPosition ?? length), length)
                        const start = Math.min(Number(saved.selectionStart ?? position), length)
                        const end = Math.min(Number(saved.selectionEnd ?? position), length)
                        select(position === start ? end : start, position === start ? start : end)
                        root.editingState.controller.initializeTableCellEditor(textDocument,
                            root.editingState.text, root.editingState.cellFormat, saved)
                        initialized = true
                        root.synchronize()
                    }
                    Component.onDestruction: {
                        root.synchronize()
                        initialized = false
                    }
                    Keys.onShortcutOverride: event => {
                        const command = event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)
                        event.accepted = !(command && (event.key === Qt.Key_S
                            || event.key === Qt.Key_W || event.key === Qt.Key_O || event.key === Qt.Key_F))
                    }
                    Keys.onPressed: event => {
                        const command = event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)
                        if (command && event.key === Qt.Key_B) {
                            root.applyFormatting("bold", !root.editingState.bold)
                            event.accepted = true
                        } else if (command && event.key === Qt.Key_I) {
                            root.applyFormatting("italic", !root.editingState.italic)
                            event.accepted = true
                        } else if (command && event.key === Qt.Key_U) {
                            root.applyFormatting("underline", !root.editingState.underline)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape) {
                            root.editingState.cancel()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                            root.editingState.advance(event.key === Qt.Key_Backtab
                                || (event.modifiers & Qt.ShiftModifier))
                            event.accepted = true
                        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                   && (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier))) {
                            root.editingState.commit()
                            event.accepted = true
                        }
                    }
                }
            }
        }
    }

    function focusInput() {
        if (root.editingState.active && editorGeneration === editingState.focusGeneration) {
            input.forceActiveFocus()
            textViewport.revealCursor()
        }
    }
    Component.onCompleted: {
        editorGeneration = root.editingState.focusGeneration
        root.editingState.editorItem = root
        synchronize()
        Qt.callLater(root.focusInput)
    }
    Component.onDestruction: {
        if (root.editingState.editorItem === root) {
            root.editingState.formatCommitRequested()
            synchronize()
            root.editingState.editorItem = null
        }
    }
    Connections {
        target: root.editingState
        function onFocusRequested() { Qt.callLater(root.focusInput) }
        function onFocusGenerationChanged() { Qt.callLater(root.focusInput) }
    }
}
