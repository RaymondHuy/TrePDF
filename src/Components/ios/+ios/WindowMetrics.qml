import QtQuick

QtObject {
    id: root
    required property Window window
    readonly property real minimumWidth: 320
    readonly property real minimumHeight: 320
    readonly property real topInset: window.SafeArea.margins.top
    readonly property real bottomInset: window.SafeArea.margins.bottom
    readonly property real leftInset: window.SafeArea.margins.left
    readonly property real rightInset: window.SafeArea.margins.right
    readonly property var inputMethod: Qt.inputMethod
    readonly property real keyboardInset: inputMethod.visible
        && inputMethod.keyboardRectangle.width >= window.width * 0.8
        ? Math.max(0, window.height - inputMethod.keyboardRectangle.y) : 0
}
