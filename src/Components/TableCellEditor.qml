pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Item {
    id: root
    required property TableCellEditing editingState
    required property AppTheme theme
    required property real pointScale
    readonly property real layoutScale: Math.max(0.01, pointScale)
    readonly property rect normalizedCell: editingState.cellRect

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

        Flickable {
            id: textViewport
            x: 0
            y: root.layoutScale
            width: cellFrame.width / root.layoutScale
            height: Math.max(1, cellFrame.height / root.layoutScale - 1)
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
                font: root.editingState.controller.fontForDisplay(root.editingState.fontFamily,
                    100, false, false, false, root.editingState.fontSize)
                //% "Cell text"
                Accessible.name: qsTrId("TableCellText")
                text: root.editingState.text
                onTextChanged: {
                    if (root.editingState.text !== text) {
                        root.editingState.text = text
                        root.editingState.errorText = ""
                    }
                }
                onActiveFocusChanged: root.editingState.inputFocused = activeFocus
                onCursorRectangleChanged: textViewport.revealCursor()
                Keys.onShortcutOverride: event => {
                    const command = event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)
                    event.accepted = !(command && (event.key === Qt.Key_S
                        || event.key === Qt.Key_W || event.key === Qt.Key_O || event.key === Qt.Key_F))
                }
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Escape) {
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

    Rectangle {
        id: actions
        x: Math.max(0, Math.min(cellFrame.x, root.width - width))
        y: cellFrame.y + cellFrame.height + height + 4 <= root.height
            ? cellFrame.y + cellFrame.height + 4 : Math.max(0, cellFrame.y - height - 4)
        width: Math.min(340, root.width)
        height: actionLayout.implicitHeight + 16
        color: root.theme.ribbonSurface
        border.color: root.theme.ribbonBorder
        radius: 5

        MouseArea { anchors.fill: parent }
        ColumnLayout {
            id: actionLayout
            anchors.fill: parent
            anchors.margins: 8
            spacing: 6
            Label {
                objectName: "tableCellError"
                Layout.fillWidth: true
                visible: root.editingState.errorText.length > 0
                text: root.editingState.errorText
                color: "#b3261e"
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
            Label {
                Layout.fillWidth: true
                visible: root.editingState.errorText.length === 0
                //% "Enter: new line · Tab: next cell · Ctrl/Cmd+Enter: finish · Esc: cancel"
                text: qsTrId("TableCellInlineHint")
                color: root.theme.ribbonTextColor
                wrapMode: Text.Wrap
                font.pixelSize: 11
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    objectName: "tableCellCancelButton"
                    //% "Cancel"
                    text: qsTrId("Cancel")
                    onClicked: root.editingState.cancel()
                }
                Button {
                    objectName: "tableCellApplyButton"
                    //% "Apply"
                    text: qsTrId("TableCellApply")
                    onClicked: root.editingState.commit()
                }
            }
        }
    }

    function focusInput() {
        if (root.editingState.active) {
            input.forceActiveFocus()
            input.cursorPosition = input.length
            textViewport.revealCursor()
        }
    }
    Component.onCompleted: {
        root.editingState.editorItem = root
        Qt.callLater(root.focusInput)
    }
    Component.onDestruction: {
        if (root.editingState.editorItem === root)
            root.editingState.editorItem = null
    }
    Connections {
        target: root.editingState
        function onFocusRequested() { Qt.callLater(root.focusInput) }
        function onFocusGenerationChanged() { Qt.callLater(root.focusInput) }
    }
}
