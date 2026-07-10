import QtQuick
import QtQuick.Controls.Basic

Item {
    id: group

    required property AppTheme theme
    default property alias tools: toolRow.data
    property string title

    implicitWidth: toolRow.implicitWidth + 18
    implicitHeight: 76

    Row {
        id: toolRow

        x: 6
        y: 2
        spacing: 2
    }

    Label {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 2
        text: group.title
        color: "#817c78"
        font.pixelSize: 10
        horizontalAlignment: Text.AlignHCenter
    }

    Rectangle {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.topMargin: 6
        anchors.bottomMargin: 6
        width: 1
        color: group.theme.ribbonBorder
        Accessible.ignored: true
    }
}
