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

    implicitWidth: Math.max(80, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: 56
    enabled: !requiresDocument || documentLoaded
    display: AbstractButton.TextUnderIcon
    spacing: 3
    leftPadding: 5
    rightPadding: 5
    topPadding: 5
    bottomPadding: 4
    font.pixelSize: 11
    palette.buttonText: enabled ? theme.ribbonTextColor : "#a7a4a1"
    icon.source: iconSource
    icon.width: 22
    icon.height: 22
    icon.color: !enabled
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

    ToolTip.visible: hovered
    ToolTip.delay: 500
    ToolTip.text: toolTipText
}
