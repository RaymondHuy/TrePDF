import QtQuick

QtObject {
    required property Item overlay
    readonly property bool active: false
    readonly property real topInset: 0
    readonly property real leftInset: 0
    readonly property real availableWidth: overlay ? overlay.width : 0
    readonly property real availableHeight: overlay ? overlay.height : 0
}
