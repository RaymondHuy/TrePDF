pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root
    objectName: "tableCellPropertiesPanel"

    required property AppTheme theme
    required property TableCellEditing editingState
    readonly property bool editorAvailable: editingState.editorItem !== null
    readonly property var fontFamilies: editingState.controller.headerTextFontFamilies(
        -1, editingState.bold, editingState.italic)

    signal revealCellRequested()

    implicitWidth: 264
    color: theme.ribbonSurface

    function applyFormatting(property, value, restoreFocus = true) {
        if (property !== "fontSize")
            fontSizeBox.commitSizeText(false)
        if (editingState.editorItem)
            editingState.editorItem.applyFormatting(property, value, restoreFocus)
    }

    Keys.onEscapePressed: event => {
        root.editingState.cancel()
        event.accepted = true
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: root.theme.ribbonBorder
        Accessible.ignored: true
    }

    ScrollView {
        id: panelScroll
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: panelScroll.availableWidth
            spacing: 12

            Label {
                Layout.fillWidth: true
                //% "Cell properties"
                text: qsTrId("TableCellProperties")
                color: root.theme.ribbonTextColor
                font.pixelSize: 17
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            Label {
                Layout.fillWidth: true
                //% "Row %1 · Column %2"
                text: qsTrId("TableCellPosition").arg(root.editingState.row + 1)
                    .arg(root.editingState.column + 1)
                color: "#747176"
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: root.theme.ribbonBorder
                Accessible.ignored: true
            }

            Label {
                Layout.fillWidth: true
                //% "Font family"
                text: qsTrId("FontFamily")
                color: root.theme.ribbonTextColor
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            ComboBox {
                id: familyBox
                objectName: "tableCellFontBox"
                Layout.fillWidth: true
                enabled: root.editorAvailable
                model: root.fontFamilies
                currentIndex: root.fontFamilies.indexOf(root.editingState.fontFamily)
                displayText: root.editingState.fontFamily
                //% "Font family"
                Accessible.name: qsTrId("FontFamily")
                onActivated: index => root.applyFormatting("family", textAt(index))
            }

            Label {
                Layout.fillWidth: true
                //% "Font size"
                text: qsTrId("FontSize")
                color: root.theme.ribbonTextColor
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            ComboBox {
                id: fontSizeBox
                objectName: "tableCellFontSizeBox"
                property bool pending: false

                function syncCurrentSize() {
                    pending = false
                    const sizeText = String(root.editingState.fontSize)
                    currentIndex = model.indexOf(sizeText)
                    editText = sizeText
                }

                function commitSizeText(restoreFocus = true) {
                    if (!pending)
                        return
                    const entered = editText.trim()
                    const points = Number(entered)
                    const apply = root.editingState.active && root.editorAvailable
                        && /^[0-9]+$/.test(entered) && Number.isInteger(points)
                        && points >= 1 && points <= 512
                    pending = false
                    if (apply)
                        root.applyFormatting("fontSize", points, restoreFocus)
                    syncCurrentSize()
                }

                Layout.fillWidth: true
                enabled: root.editorAvailable
                editable: true
                model: ["8", "9", "10", "11", "12", "14", "16", "18", "24", "36", "48", "72"]
                currentIndex: -1
                inputMethodHints: Qt.ImhDigitsOnly
                //% "Font size"
                Accessible.name: qsTrId("FontSize")
                Component.onCompleted: syncCurrentSize()
                onActivated: index => {
                    pending = true
                    editText = textAt(index)
                    commitSizeText()
                }
                onAccepted: commitSizeText()

                Connections {
                    target: root.editingState
                    function onFontSizeChanged() {
                        if (!fontSizeBox.pending)
                            fontSizeBox.syncCurrentSize()
                    }
                    function onFormatCommitRequested() { fontSizeBox.commitSizeText(false) }
                }

                contentItem: TextField {
                    id: fontSizeInput
                    objectName: "tableCellFontSizeInput"
                    leftPadding: 12
                    rightPadding: activeFocus ? 3 : 1
                    topPadding: 6 - fontSizeBox.padding
                    bottomPadding: 6 - fontSizeBox.padding
                    text: fontSizeBox.editText
                    font: fontSizeBox.font
                    enabled: fontSizeBox.editable
                    autoScroll: true
                    readOnly: fontSizeBox.down
                    inputMethodHints: fontSizeBox.inputMethodHints
                    validator: fontSizeBox.validator
                    selectByMouse: true
                    color: fontSizeBox.palette.text
                    selectionColor: fontSizeBox.palette.highlight
                    selectedTextColor: fontSizeBox.palette.highlightedText
                    verticalAlignment: Text.AlignVCenter
                    //% "Font size"
                    Accessible.name: qsTrId("FontSize")
                    onTextEdited: fontSizeBox.pending = true
                    onActiveFocusChanged: {
                        if (!activeFocus && !fontSizeBox.popup.visible)
                            fontSizeBox.commitSizeText(false)
                    }
                    Keys.onPressed: event => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                && !fontSizeBox.popup.visible) {
                            fontSizeBox.commitSizeText()
                            event.accepted = true
                        }
                    }
                    background: Rectangle {
                        radius: 6
                        color: fontSizeBox.palette.base
                        border.width: fontSizeInput.activeFocus ? 2 : 1
                        border.color: fontSizeInput.activeFocus
                            ? root.theme.foxitBorderOrange : root.theme.ribbonBorder
                    }
                }

                validator: IntValidator { bottom: 1; top: 512 }

                background: Rectangle {
                    radius: 6
                    color: fontSizeBox.palette.button
                    border.width: 1
                    border.color: fontSizeBox.activeFocus
                        ? root.theme.foxitBorderOrange : root.theme.ribbonBorder
                }
            }

            RowLayout {
                spacing: 6
                enabled: root.editorAvailable

                ToolButton {
                    objectName: "tableCellBoldButton"
                    Layout.preferredWidth: 36
                    checkable: true
                    checked: root.editingState.bold
                    focusPolicy: Qt.TabFocus
                    display: AbstractButton.IconOnly
                    //% "Bold"
                    text: qsTrId("Bold")
                    icon.source: "../assets/icons/bold-text.svg"
                    icon.width: 18
                    icon.height: 18
                    ToolTip.text: text
                    ToolTip.visible: hovered
                    Accessible.name: text
                    onClicked: {
                        root.applyFormatting("bold", !root.editingState.bold)
                        checked = Qt.binding(() => root.editingState.bold)
                    }
                }

                ToolButton {
                    objectName: "tableCellItalicButton"
                    Layout.preferredWidth: 36
                    checkable: true
                    checked: root.editingState.italic
                    focusPolicy: Qt.TabFocus
                    display: AbstractButton.IconOnly
                    //% "Italic"
                    text: qsTrId("Italic")
                    icon.source: "../assets/icons/italic-font.svg"
                    icon.width: 18
                    icon.height: 18
                    ToolTip.text: text
                    ToolTip.visible: hovered
                    Accessible.name: text
                    onClicked: {
                        root.applyFormatting("italic", !root.editingState.italic)
                        checked = Qt.binding(() => root.editingState.italic)
                    }
                }

                ToolButton {
                    objectName: "tableCellUnderlineButton"
                    Layout.preferredWidth: 36
                    checkable: true
                    checked: root.editingState.underline
                    focusPolicy: Qt.TabFocus
                    display: AbstractButton.IconOnly
                    //% "Underline"
                    text: qsTrId("Underline")
                    icon.source: "../assets/icons/underline-text.svg"
                    icon.width: 18
                    icon.height: 18
                    ToolTip.text: text
                    ToolTip.visible: hovered
                    Accessible.name: text
                    onClicked: {
                        root.applyFormatting("underline", !root.editingState.underline)
                        checked = Qt.binding(() => root.editingState.underline)
                    }
                }
            }

            Button {
                objectName: "tableCellRevealButton"
                Layout.fillWidth: true
                visible: !root.editorAvailable
                //% "Go to cell"
                text: qsTrId("TableCellGoTo")
                onClicked: root.revealCellRequested()
            }

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
                //% "Enter: new line · Tab: next cell · Ctrl/Cmd+Enter: finish · Esc: cancel"
                text: qsTrId("TableCellInlineHint")
                color: "#747176"
                wrapMode: Text.Wrap
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    objectName: "tableCellCancelButton"
                    Layout.fillWidth: true
                    //% "Cancel"
                    text: qsTrId("Cancel")
                    onClicked: root.editingState.cancel()
                }
                Button {
                    objectName: "tableCellApplyButton"
                    Layout.fillWidth: true
                    //% "Apply"
                    text: qsTrId("TableCellApply")
                    onClicked: root.editingState.commit()
                }
            }
        }
    }
}
