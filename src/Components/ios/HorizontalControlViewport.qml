import QtQuick

Item {
    id: root

    default property alias content: contentHost.data
    property real contentImplicitWidth: 0
    property real contentImplicitHeight: 0
    readonly property real contentWidth: width

    implicitWidth: contentImplicitWidth
    implicitHeight: contentImplicitHeight

    readonly property Item _contentHost: Item {
        id: contentHost
        parent: root
        width: root.contentWidth
        height: root.height
    }
}
