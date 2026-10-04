import QtQuick

QtObject {
    required property Item overlay
    readonly property var inputMethod: Qt.inputMethod
    readonly property bool active: true
    readonly property real topInset: overlay ? overlay.SafeArea.margins.top : 0
    readonly property real leftInset: overlay ? overlay.SafeArea.margins.left : 0
    readonly property real availableWidth: overlay
        ? overlay.width - leftInset - overlay.SafeArea.margins.right : 0
    readonly property real availableHeight: {
        if (!overlay)
            return 0
        let bottom = overlay.height - overlay.SafeArea.margins.bottom
        const keyboard = inputMethod.keyboardRectangle
        // A floating iPad keyboard should not collapse the entire dialog.
        if (inputMethod.visible && keyboard.width >= overlay.width * 0.8)
            bottom = Math.min(bottom, keyboard.y)
        return Math.max(0, bottom - topInset)
    }
}
