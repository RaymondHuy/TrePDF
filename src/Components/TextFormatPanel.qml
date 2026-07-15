pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

Rectangle {
    id: root

    required property AppTheme theme
    required property bool collapsed
    required property var availableFontFamilies
    required property string selectedFontFamily
    property bool formattingEnabled: false
    property color previewColor: "#2f3136"

    readonly property var displayedFontFamilies: {
        const families = []
        for (let index = 0; index < availableFontFamilies.length; ++index)
            families.push(availableFontFamilies[index])

        if (selectedFontFamily.length === 0)
            return families

        const selectedKey = selectedFontFamily.toLowerCase()
        for (let index = 0; index < families.length; ++index) {
            if (families[index].toLowerCase() === selectedKey)
                return families
        }

        families.unshift(selectedFontFamily)
        return families
    }
    readonly property int selectedFontFamilyIndex: {
        if (selectedFontFamily.length === 0)
            return -1

        const selectedKey = selectedFontFamily.toLowerCase()
        for (let index = 0; index < displayedFontFamilies.length; ++index) {
            if (displayedFontFamilies[index].toLowerCase() === selectedKey)
                return index
        }
        return -1
    }

    signal collapsedChangeRequested(bool collapsed)
    signal fontFamilyChangeRequested(string fontFamily)

    implicitWidth: collapsed ? 42 : 264
    color: theme.ribbonSurface

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: root.theme.ribbonBorder
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
        //% "Expand text formatting"
        ToolTip.text: qsTrId("ExpandTextFormatting")
        ToolTip.visible: hovered
        ToolTip.delay: 500
        onClicked: root.collapsedChangeRequested(false)

        background: Rectangle {
            radius: 4
            color: expandButton.down
                ? "#f5dfcf"
                : expandButton.hovered
                    ? root.theme.foxitPaleOrange
                    : "transparent"
        }
    }

    Loader {
        anchors.fill: parent
        anchors.leftMargin: 1
        active: !root.collapsed

        sourceComponent: Component {
            ScrollView {
                id: formatScroll

                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: formatScroll.availableWidth
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 8
                        Layout.topMargin: 10
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            //% "Text Formatting"
                            text: qsTrId("TextFormatting")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                        }

                        ToolButton {
                            id: collapseButton

                            Layout.preferredWidth: 32
                            Layout.preferredHeight: 32
                            icon.source: "../assets/icons/chevron-right.svg"
                            icon.width: 18
                            icon.height: 18
                            icon.color: root.theme.foxitDarkOrange
                            display: AbstractButton.IconOnly
                            //% "Collapse text formatting"
                            ToolTip.text: qsTrId("CollapseTextFormatting")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            onClicked: root.collapsedChangeRequested(true)

                            background: Rectangle {
                                radius: 4
                                color: collapseButton.down
                                    ? "#f5dfcf"
                                    : collapseButton.hovered
                                        ? root.theme.foxitPaleOrange
                                        : "transparent"
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        visible: !root.formattingEnabled
                        //% "Select editable text to enable formatting."
                        text: qsTrId("SelectEditableTextForFormatting")
                        color: "#747176"
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: root.theme.ribbonBorder
                    }

                    ColumnLayout {
                        id: formatControls

                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.bottomMargin: 16
                        spacing: 8
                        enabled: root.formattingEnabled

                        Label {
                            Layout.fillWidth: true
                            //% "Font family"
                            text: qsTrId("FontFamily")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }

                        ComboBox {
                            id: fontFamilyBox

                            Layout.fillWidth: true
                            model: root.displayedFontFamilies
                            currentIndex: root.selectedFontFamilyIndex
                            clip: true
                            onActivated: root.fontFamilyChangeRequested(
                                fontFamilyBox.currentText)

                            background: Rectangle {
                                radius: 6
                                color: fontFamilyBox.palette.button
                                border.width: 1
                                border.color: fontFamilyBox.activeFocus
                                    ? root.theme.foxitBorderOrange
                                    : root.theme.ribbonBorder
                            }

                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                            //% "Font size"
                            text: qsTrId("FontSize")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }

                        ComboBox {
                            id: fontSizeBox

                            Layout.fillWidth: true
                            editable: true
                            model: ["8", "9", "10", "11", "12", "14", "16", "18", "24", "36", "48", "72"]
                            currentIndex: 4
                            clip: true

                            background: Rectangle {
                                radius: 6
                                color: fontSizeBox.palette.button
                                border.width: 1
                                border.color: fontSizeBox.activeFocus
                                    ? root.theme.foxitBorderOrange
                                    : root.theme.ribbonBorder
                            }

                            contentItem: TextField {
                                id: fontSizeInput

                                leftPadding: 12
                                rightPadding: fontSizeBox.editable && activeFocus ? 3 : 1
                                topPadding: 6 - fontSizeBox.padding
                                bottomPadding: 6 - fontSizeBox.padding
                                text: fontSizeBox.editText
                                font: fontSizeBox.font
                                enabled: fontSizeBox.editable
                                autoScroll: fontSizeBox.editable
                                readOnly: fontSizeBox.down
                                inputMethodHints: fontSizeBox.inputMethodHints
                                validator: fontSizeBox.validator
                                selectByMouse: fontSizeBox.selectTextByMouse
                                color: fontSizeBox.palette.text
                                selectionColor: fontSizeBox.palette.highlight
                                selectedTextColor: fontSizeBox.palette.highlightedText
                                verticalAlignment: Text.AlignVCenter

                                background: Rectangle {
                                    visible: fontSizeBox.enabled
                                        && fontSizeBox.editable
                                        && !fontSizeBox.flat
                                    radius: 6
                                    color: fontSizeBox.palette.base
                                    border.width: fontSizeInput.activeFocus ? 2 : 1
                                    border.color: fontSizeInput.activeFocus
                                        ? root.theme.foxitBorderOrange
                                        : root.theme.ribbonBorder
                                }
                            }

                            validator: IntValidator {
                                bottom: 1
                                top: 512
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                            //% "Style"
                            text: qsTrId("FontStyle")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            ToolButton {
                                id: boldButton

                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 58
                                checkable: true
                                display: AbstractButton.TextUnderIcon
                                spacing: 3
                                //% "Bold"
                                text: qsTrId("Bold")
                                icon.source: "../assets/icons/bold-text.svg"
                                icon.width: 20
                                icon.height: 20
                                icon.color: !enabled
                                    ? "#aaa8a6"
                                    : checked
                                        ? root.theme.foxitDarkOrange
                                        : "transparent"
                                font.pixelSize: 10

                                background: Rectangle {
                                    radius: 4
                                    color: !boldButton.enabled
                                        ? "transparent"
                                        : boldButton.down
                                            ? "#f5dfcf"
                                            : boldButton.checked || boldButton.hovered
                                                ? root.theme.foxitPaleOrange
                                                : "transparent"
                                    border.width: boldButton.checked ? 1 : 0
                                    border.color: root.theme.foxitBorderOrange
                                }
                            }

                            ToolButton {
                                id: italicButton

                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 58
                                checkable: true
                                display: AbstractButton.TextUnderIcon
                                spacing: 3
                                //% "Italic"
                                text: qsTrId("Italic")
                                icon.source: "../assets/icons/italic-font.svg"
                                icon.width: 20
                                icon.height: 20
                                icon.color: !enabled
                                    ? "#aaa8a6"
                                    : checked
                                        ? root.theme.foxitDarkOrange
                                        : "transparent"
                                font.pixelSize: 10

                                background: Rectangle {
                                    radius: 4
                                    color: !italicButton.enabled
                                        ? "transparent"
                                        : italicButton.down
                                            ? "#f5dfcf"
                                            : italicButton.checked || italicButton.hovered
                                                ? root.theme.foxitPaleOrange
                                                : "transparent"
                                    border.width: italicButton.checked ? 1 : 0
                                    border.color: root.theme.foxitBorderOrange
                                }
                            }

                            ToolButton {
                                id: underlineButton

                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 58
                                checkable: true
                                display: AbstractButton.TextUnderIcon
                                spacing: 3
                                //% "Underline"
                                text: qsTrId("Underline")
                                icon.source: "../assets/icons/underline-text.svg"
                                icon.width: 20
                                icon.height: 20
                                icon.color: !enabled
                                    ? "#aaa8a6"
                                    : checked
                                        ? root.theme.foxitDarkOrange
                                        : "transparent"
                                font.pixelSize: 10

                                background: Rectangle {
                                    radius: 4
                                    color: !underlineButton.enabled
                                        ? "transparent"
                                        : underlineButton.down
                                            ? "#f5dfcf"
                                            : underlineButton.checked || underlineButton.hovered
                                                ? root.theme.foxitPaleOrange
                                                : "transparent"
                                    border.width: underlineButton.checked ? 1 : 0
                                    border.color: root.theme.foxitBorderOrange
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                            //% "Font color"
                            text: qsTrId("FontColor")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }

                        Button {
                            id: colorButton

                            Layout.fillWidth: true
                            Layout.preferredHeight: 38
                            onClicked: colorDialog.open()

                            contentItem: RowLayout {
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    radius: 3
                                    color: root.previewColor
                                    border.width: 1
                                    border.color: "#8a8785"
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: root.previewColor.toString().toUpperCase()
                                    color: colorButton.enabled
                                        ? root.theme.ribbonTextColor
                                        : "#aaa8a6"
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            background: Rectangle {
                                radius: 4
                                color: !colorButton.enabled
                                    ? "#f4f1ef"
                                    : colorButton.down
                                        ? "#f5dfcf"
                                        : colorButton.hovered
                                            ? root.theme.foxitPaleOrange
                                            : "#ffffff"
                                border.width: 1
                                border.color: colorButton.enabled
                                    ? root.theme.ribbonBorder
                                    : "#ddd9d6"
                            }
                        }
                    }
                }
            }
        }
    }

    ColorDialog {
        id: colorDialog

        selectedColor: root.previewColor
        //% "Font color"
        title: qsTrId("FontColor")
        onAccepted: root.previewColor = selectedColor
    }
}
