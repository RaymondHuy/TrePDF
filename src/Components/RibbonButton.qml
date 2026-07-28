import QtQuick
import QtQuick.Controls.Basic

ToolButton {
    id: control

    required property AppTheme theme
    required property bool documentLoaded
    property url iconSource
    property string toolTipText: text
    property bool requiresDocument: true
    property bool primary: false
    property bool signing: false
    property bool preserveIconColors: false
    property bool menuIndicatorVisible: false
    property string menuToolTipText: ""

    signal menuRequested()

    implicitWidth: Math.max(80, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: 56
    enabled: !requiresDocument || documentLoaded
    display: AbstractButton.TextUnderIcon
    spacing: 3
    leftPadding: 5
    rightPadding: menuIndicatorVisible ? 22 : 5
    topPadding: 5
    bottomPadding: 4
    font.pixelSize: 11
    palette.buttonText: enabled ? theme.ribbonTextColor : "#a7a4a1"
    icon.source: iconSource
    icon.width: 22
    icon.height: 22
    icon.color: preserveIconColors
        ? "transparent"
        : !enabled
            ? "#aaa8a6"
            : signing
                ? theme.signGreen
                : primary
                    ? theme.foxitDarkOrange
                    : "#4a4b50"

    background: Rectangle {
        radius: 4
        color: !control.enabled
            ? "transparent"
            : control.down
                ? control.signing ? "#dceee4" : "#f5dfcf"
                : control.hovered || control.primary
                    ? control.signing ? control.theme.signPaleGreen : control.theme.foxitPaleOrange
                    : "transparent"
        border.color: control.primary
            ? control.signing ? "#a8cfb9" : "#edc5a6"
            : "transparent"
        border.width: 1
    }

    ToolButton {
        id: menuButton

        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 18
        topInset: 1
        rightInset: 1
        bottomInset: 1
        visible: control.menuIndicatorVisible
        focusPolicy: Qt.TabFocus
        text: "\u25be"
        font.pixelSize: 17
        Accessible.name: control.menuToolTipText
        onClicked: control.menuRequested()

        contentItem: Label {
            text: menuButton.text
            color: !menuButton.enabled
                ? "#aaa8a6"
                : control.primary
                    ? control.theme.foxitDarkOrange
                    : "#4a4b50"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: 3
            color: menuButton.down
                ? "#f5dfcf"
                : menuButton.hovered
                    ? control.theme.foxitPaleOrange
                    : "transparent"

            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.topMargin: 5
                anchors.bottomMargin: 5
                width: 1
                color: control.primary || menuButton.hovered
                    ? control.theme.foxitBorderOrange
                    : "transparent"
            }
        }

        ToolTip.visible: hovered && control.menuToolTipText.length > 0
        ToolTip.delay: 500
        ToolTip.text: control.menuToolTipText
    }

    ToolTip.visible: hovered
        && (!menuIndicatorVisible || !menuButton.hovered)
    ToolTip.delay: 500
    ToolTip.text: toolTipText
}
