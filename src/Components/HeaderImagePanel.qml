pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root
    objectName: "headerImagePanel"

    required property AppTheme theme
    required property HeaderImagePlacement placement
    readonly property bool inputFocused: pageRangeInput.activeFocus
    readonly property bool canSubmit: placement.canSubmit
        && (allPagesSelector.checked || pageRangeInput.text.trim().length > 0)

    implicitWidth: 280
    color: theme.ribbonSurface
    Keys.onEscapePressed: event => {
        if (!root.placement.submitting) {
            root.placement.cancel()
            event.accepted = true
        }
    }

    function submit() {
        if (canSubmit)
            placement.submit(allPagesSelector.checked ? "all" : pageRangeInput.text.trim())
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
                //% "Header image"
                text: qsTrId("HeaderImage")
                color: root.theme.ribbonTextColor
                font.pixelSize: 17
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                ToolButton {
                    id: rotateLeftButton
                    objectName: "headerImageRotateLeftButton"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    enabled: root.placement.canRotate
                    //% "Rotate left 90°"
                    text: qsTrId("RotateImageLeft90")
                    icon.source: "../assets/icons/rotate-ccw.svg"
                    icon.width: 18
                    icon.height: 18
                    icon.color: enabled ? root.theme.foxitDarkOrange : "#aaa8a6"
                    display: AbstractButton.TextUnderIcon
                    font.pixelSize: 11
                    palette.buttonText: root.theme.ribbonTextColor
                    focusPolicy: Qt.TabFocus
                    onClicked: root.placement.rotate(-1)
                    background: Rectangle {
                        radius: 4
                        color: rotateLeftButton.down ? "#f5dfcf"
                            : rotateLeftButton.hovered ? root.theme.foxitPaleOrange : "transparent"
                        border.color: rotateLeftButton.visualFocus
                            ? root.theme.foxitDarkOrange : root.theme.ribbonBorder
                    }
                }

                ToolButton {
                    id: rotateRightButton
                    objectName: "headerImageRotateRightButton"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    enabled: root.placement.canRotate
                    //% "Rotate right 90°"
                    text: qsTrId("RotateImageRight90")
                    icon.source: "../assets/icons/rotate-cw.svg"
                    icon.width: 18
                    icon.height: 18
                    icon.color: enabled ? root.theme.foxitDarkOrange : "#aaa8a6"
                    display: AbstractButton.TextUnderIcon
                    font.pixelSize: 11
                    palette.buttonText: root.theme.ribbonTextColor
                    focusPolicy: Qt.TabFocus
                    onClicked: root.placement.rotate(1)
                    background: Rectangle {
                        radius: 4
                        color: rotateRightButton.down ? "#f5dfcf"
                            : rotateRightButton.hovered ? root.theme.foxitPaleOrange : "transparent"
                        border.color: rotateRightButton.visualFocus
                            ? root.theme.foxitDarkOrange : root.theme.ribbonBorder
                    }
                }
            }

            CheckBox {
                id: aspectRatioLock
                objectName: "headerImageAspectRatioLock"
                Layout.fillWidth: true
                enabled: root.placement.canSubmit
                checked: root.placement.aspectRatioLocked
                //% "Lock aspect ratio"
                text: qsTrId("ImageAspectRatioLocked")
                font.pixelSize: 11
                palette.highlight: root.theme.foxitDarkOrange
                contentItem: Text {
                    leftPadding: aspectRatioLock.indicator.width + aspectRatioLock.spacing
                    text: aspectRatioLock.text
                    font: aspectRatioLock.font
                    color: aspectRatioLock.enabled ? root.theme.ribbonTextColor : "#aaa8a6"
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.WordWrap
                }
                onCheckedChanged: root.placement.aspectRatioLocked = checked
            }

            Label {
                Layout.fillWidth: true
                //% "Apply to"
                text: qsTrId("HeaderApplyTo")
                color: root.theme.ribbonTextColor
                font.weight: Font.DemiBold
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                ButtonGroup { id: pageSelectionGroup }

                RadioButton {
                    id: allPagesSelector
                    objectName: "headerImageAllPagesSelector"
                    Layout.fillWidth: true
                    ButtonGroup.group: pageSelectionGroup
                    checked: true
                    //% "All pages"
                    text: qsTrId("HeaderAllPages")
                    palette.highlight: root.theme.foxitDarkOrange
                    onToggled: root.placement.errorText = ""
                }

                RadioButton {
                    id: customPagesSelector
                    objectName: "headerImageCustomPagesSelector"
                    Layout.fillWidth: true
                    ButtonGroup.group: pageSelectionGroup
                    //% "Selected pages"
                    text: qsTrId("HeaderSelectedPages")
                    palette.highlight: root.theme.foxitDarkOrange
                    onToggled: {
                        root.placement.errorText = ""
                        if (checked)
                            pageRangeInput.forceActiveFocus()
                    }
                }

                TextField {
                    id: pageRangeInput
                    objectName: "headerImagePageRangeInput"
                    Layout.fillWidth: true
                    enabled: customPagesSelector.checked
                    maximumLength: 2048
                    selectByMouse: true
                    //% "For example: 1-3, 5"
                    placeholderText: qsTrId("HeaderPageRangePlaceholder")
                    //% "Selected pages"
                    Accessible.name: qsTrId("HeaderSelectedPages")
                    onTextChanged: root.placement.errorText = ""
                    onAccepted: root.submit()
                }
            }

            Label {
                Layout.fillWidth: true
                //% "Apply creates a draft. Save or Save As writes the image to the PDF."
                text: qsTrId("HeaderImageDraftHint")
                color: root.theme.ribbonTextColor
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            Label {
                objectName: "headerImageError"
                Layout.fillWidth: true
                visible: root.placement.errorText.length > 0
                text: root.placement.errorText
                color: "#b3261e"
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Button {
                    objectName: "headerImageCancelButton"
                    Layout.fillWidth: true
                    enabled: !root.placement.submitting
                    //% "Cancel"
                    text: qsTrId("Cancel")
                    onClicked: root.placement.cancel()
                }

                Button {
                    objectName: "headerImageApplyButton"
                    Layout.fillWidth: true
                    enabled: root.canSubmit
                    highlighted: true
                    //% "Apply"
                    text: qsTrId("HeaderApply")
                    onClicked: root.submit()
                }
            }
        }
    }
}
