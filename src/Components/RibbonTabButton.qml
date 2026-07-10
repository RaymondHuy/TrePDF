import QtQuick
import QtQuick.Controls.Basic

TabButton {
    id: control

    required property AppTheme theme

    implicitWidth: 104
    implicitHeight: 34

    contentItem: Text {
        text: control.text
        font.family: control.font.family
        font.pixelSize: control.font.pixelSize
        font.weight: control.checked ? Font.DemiBold : Font.Medium
        color: control.checked ? control.theme.foxitDarkOrange : "#56575c"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        color: control.checked
            ? control.theme.foxitPaleOrange
            : control.hovered
                ? "#f6f1ed"
                : "transparent"

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 3
            color: control.checked ? control.theme.foxitOrange : "transparent"
        }
    }
}
