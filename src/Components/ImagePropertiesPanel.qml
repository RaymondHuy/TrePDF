pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root
    objectName: "imagePropertiesPanel"

    required property AppTheme theme
    required property PDFEditController editController
    required property bool collapsed

    signal collapsedChangeRequested(bool collapsed)

    implicitWidth: collapsed ? 42 : 264
    color: theme.ribbonSurface

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
        //% "Expand image properties"
        ToolTip.text: qsTrId("ExpandImageProperties")
        ToolTip.visible: hovered
        ToolTip.delay: 500
        Accessible.name: ToolTip.text
        onClicked: root.collapsedChangeRequested(false)

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
            ScrollView {
                id: propertiesScroll

                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: propertiesScroll.availableWidth
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 8
                        Layout.topMargin: 10
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            //% "Image properties"
                            text: qsTrId("ImageProperties")
                            color: root.theme.ribbonTextColor
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
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
                            focusPolicy: Qt.TabFocus
                            //% "Collapse image properties"
                            ToolTip.text: qsTrId("CollapseImageProperties")
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            Accessible.name: ToolTip.text
                            onClicked: root.collapsedChangeRequested(true)

                            background: Rectangle {
                                radius: 4
                                color: collapseButton.down
                                    ? "#f5dfcf"
                                    : collapseButton.hovered
                                        ? root.theme.foxitPaleOrange
                                        : "transparent"
                                border.color: collapseButton.visualFocus
                                    ? root.theme.foxitDarkOrange
                                    : "transparent"
                                border.width: collapseButton.visualFocus ? 1 : 0
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: root.theme.ribbonBorder
                        Accessible.ignored: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        spacing: 8

                        ToolButton {
                            id: rotateImageLeftButton
                            objectName: "rotateImageLeftButton"

                            Layout.fillWidth: true
                            Layout.preferredHeight: 54
                            enabled: root.editController.selectedImageRotatable
                                && !root.editController.saving
                            //% "Rotate left 90°"
                            text: qsTrId("RotateImageLeft90")
                            icon.source: "../assets/icons/rotate-ccw.svg"
                            icon.width: 18
                            icon.height: 18
                            icon.color: enabled
                                ? root.theme.foxitDarkOrange : "#aaa8a6"
                            display: AbstractButton.TextUnderIcon
                            font.pixelSize: 11
                            palette.buttonText: root.theme.ribbonTextColor
                            focusPolicy: Qt.TabFocus
                            onClicked: root.editController.rotateSelectedImage(-1)

                            background: Rectangle {
                                radius: 4
                                color: rotateImageLeftButton.down
                                    ? "#f5dfcf"
                                    : rotateImageLeftButton.hovered
                                        ? root.theme.foxitPaleOrange : "transparent"
                                border.color: rotateImageLeftButton.visualFocus
                                    ? root.theme.foxitDarkOrange : root.theme.ribbonBorder
                            }
                        }

                        ToolButton {
                            id: rotateImageRightButton
                            objectName: "rotateImageRightButton"

                            Layout.fillWidth: true
                            Layout.preferredHeight: 54
                            enabled: root.editController.selectedImageRotatable
                                && !root.editController.saving
                            //% "Rotate right 90°"
                            text: qsTrId("RotateImageRight90")
                            icon.source: "../assets/icons/rotate-cw.svg"
                            icon.width: 18
                            icon.height: 18
                            icon.color: enabled
                                ? root.theme.foxitDarkOrange : "#aaa8a6"
                            display: AbstractButton.TextUnderIcon
                            font.pixelSize: 11
                            palette.buttonText: root.theme.ribbonTextColor
                            focusPolicy: Qt.TabFocus
                            onClicked: root.editController.rotateSelectedImage(1)

                            background: Rectangle {
                                radius: 4
                                color: rotateImageRightButton.down
                                    ? "#f5dfcf"
                                    : rotateImageRightButton.hovered
                                        ? root.theme.foxitPaleOrange : "transparent"
                                border.color: rotateImageRightButton.visualFocus
                                    ? root.theme.foxitDarkOrange : root.theme.ribbonBorder
                            }
                        }
                    }

                    CheckBox {
                        id: imageAspectRatioLock
                        objectName: "imageAspectRatioLock"

                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.bottomMargin: 16
                        enabled: root.editController.selectedImageResizable
                            && !root.editController.saving
                        checked: root.editController.imageAspectRatioLocked
                        //% "Lock aspect ratio"
                        text: qsTrId("ImageAspectRatioLocked")
                        font.pixelSize: 11
                        palette.highlight: root.theme.foxitDarkOrange

                        contentItem: Text {
                            leftPadding: imageAspectRatioLock.indicator.width
                                + imageAspectRatioLock.spacing
                            text: imageAspectRatioLock.text
                            font: imageAspectRatioLock.font
                            color: imageAspectRatioLock.enabled
                                ? root.theme.ribbonTextColor : "#aaa8a6"
                            verticalAlignment: Text.AlignVCenter
                            wrapMode: Text.WordWrap
                        }

                        // Accessibility toggles checked without clicked/toggled.
                        onCheckedChanged: root.editController.imageAspectRatioLocked
                            = imageAspectRatioLock.checked
                    }
                }
            }
        }
    }
}
