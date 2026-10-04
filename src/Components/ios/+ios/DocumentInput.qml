import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property bool documentLoaded
    required property bool allowsTextCopy
    required property bool hasDrafts
    required property bool saving
    required property bool textEditing
    required property bool formTextFocused
    required property bool viewTabActive
    required property real zoom
    required property PDFTextSelectionController selectionController
    readonly property bool textSelectionEnabled: touchTextSelectionButton.checked

    signal zoomRequested(real value)
    signal activated()

    ToolButton {
        id: touchTextSelectionButton
        objectName: "mobileTextSelectionButton"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 20
        visible: root.viewTabActive && root.documentLoaded
        enabled: root.allowsTextCopy && !root.hasDrafts && !root.saving
        checkable: true
        //% "Select text"
        text: qsTrId("MobileSelectText")
        implicitHeight: 44
        leftPadding: 14
        rightPadding: 14
        background: Rectangle {
            radius: 6
            color: touchTextSelectionButton.checked ? root.theme.foxitPaleOrange : "#fffdfb"
            border.color: root.theme.foxitBorderOrange
        }
        onVisibleChanged: {
            if (!visible)
                checked = false
        }
    }

    ToolButton {
        objectName: "mobileCopySelectedTextButton"
        anchors.right: touchTextSelectionButton.left
        anchors.bottom: touchTextSelectionButton.bottom
        anchors.rightMargin: 8
        visible: touchTextSelectionButton.visible && touchTextSelectionButton.checked
            && root.selectionController.hasSelection
        implicitHeight: 44
        leftPadding: 14
        rightPadding: 14
        //% "Copy"
        text: qsTrId("CopySelectedText")
        onClicked: root.selectionController.copy()
        background: Rectangle {
            radius: 6
            color: "#fffdfb"
            border.color: root.theme.foxitBorderOrange
        }
    }

    PinchHandler {
        target: null
        enabled: root.viewTabActive && !touchTextSelectionButton.checked
            && root.documentLoaded && !root.saving
            && !root.textEditing && !root.formTextFocused
        acceptedDevices: PointerDevice.TouchScreen
        minimumPointCount: 2
        maximumPointCount: 2
        property real startZoom: 1.0
        onActiveChanged: {
            if (active) {
                startZoom = root.zoom
                root.activated()
            }
        }
        onActiveScaleChanged: {
            if (active)
                root.zoomRequested(startZoom * activeScale)
        }
    }
}
