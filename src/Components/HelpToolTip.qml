pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

ToolTip {
    id: root

    required property AppTheme theme
    property string title: ""

    x: parent ? parent.width + 12 : 0
    y: -4
    width: 340
    padding: 14
    margins: 8
    popupType: Popup.Item
    delay: 500
    timeout: -1

    contentItem: Column {
        spacing: 10

        Label {
            width: parent.width
            visible: root.title.length > 0
            text: root.title
            textFormat: Text.PlainText
            color: root.theme.foxitDarkOrange
            font.pixelSize: 13
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Label {
            width: parent.width
            text: root.text
            textFormat: Text.PlainText
            color: root.theme.ribbonTextColor
            font.pixelSize: 12
            lineHeight: 1.2
            wrapMode: Text.WordWrap
        }
    }

    background: Rectangle {
        radius: 7
        color: root.theme.ribbonSurface
        border.color: root.theme.foxitBorderOrange
    }
}
