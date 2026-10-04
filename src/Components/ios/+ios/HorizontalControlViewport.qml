import QtQuick

Flickable {
    id: root

    default property alias content: contentHost.data
    property real contentImplicitWidth: 0
    property real contentImplicitHeight: 0

    implicitWidth: contentImplicitWidth
    implicitHeight: contentImplicitHeight
    contentWidth: Math.max(width, contentImplicitWidth)
    contentHeight: height
    interactive: contentWidth > width
    flickableDirection: Flickable.HorizontalFlick
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    readonly property Item _contentHost: Item {
        id: contentHost
        parent: root.contentItem
        width: root.contentWidth
        height: root.height
    }
}
