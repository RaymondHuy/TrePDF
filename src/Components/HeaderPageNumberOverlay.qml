pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFEditController editController
    required property int page
    required property real zoom
    property HeaderPageNumberPlacement placement: null
    readonly property string pageLabel: placement ? placement.labelForPage(page) : ""

    MouseArea {
        objectName: "headerPageNumberPlacementArea"
        anchors.fill: parent
        enabled: root.placement !== null && root.placement.armed
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.CrossCursor
        onClicked: mouse => root.placement.place(root.page,
            mouse.x / root.zoom, mouse.y / root.zoom)
    }

    Loader {
        id: previewLoader
        active: root.visible && root.pageLabel.length > 0
        x: active ? root.placement.xPoints * root.zoom : 0
        y: active ? root.placement.yPoints * root.zoom : 0
        width: active ? Math.max(0, Math.min(root.placement.widthPoints,
            root.width / root.zoom - root.placement.xPoints)) : 0
        scale: root.zoom
        transformOrigin: Item.TopLeft

        sourceComponent: Component {
            Rectangle {
                objectName: "headerPageNumberPreview"
                height: Math.min(numberText.implicitHeight + 1,
                    Math.max(0, root.height / root.zoom - root.placement.yPoints))
                color: "transparent"
                border.color: root.theme.foxitOrange
                border.width: 1 / root.zoom
                clip: true

                Text {
                    id: numberText
                    y: 1
                    width: parent.width
                    text: root.pageLabel
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: "black"
                    font: root.editController.fontForDisplay(
                        root.placement.fontFamily, 100, false, false, false, 12)
                }
            }
        }
    }
}
