import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control

    required property AppTheme theme

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? "#ffffff" : "#f2c4a5"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 112
        implicitHeight: 34
        radius: 4
        color: control.down
            ? control.theme.foxitDarkOrange
            : control.hovered
                ? control.theme.foxitHoverOrange
                : control.theme.foxitOrange
        border.color: control.theme.foxitBorderOrange
        border.width: 1
    }
}
