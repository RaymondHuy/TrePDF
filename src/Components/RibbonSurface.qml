import QtQuick

Rectangle {
    id: root

    required property AppTheme theme
    default property alias content: contentItem.data

    implicitHeight: 78
    color: theme.ribbonSurface

    data: [
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.theme.ribbonBorder
        },
        Item {
            id: contentItem
            anchors.fill: parent
        }
    ]
}
