pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

Item {
    id: root
    objectName: "headerImageOverlay"

    required property AppTheme theme
    required property PDFEditController editController
    required property int page
    required property real zoom
    property HeaderImagePlacement placement: null

    MouseArea {
        objectName: "headerImagePlacementArea"
        anchors.fill: parent
        enabled: root.placement !== null && root.placement.armed
        visible: enabled
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.CrossCursor
        onClicked: mouse => root.placement.place(root.page,
            mouse.x / root.zoom, mouse.y / root.zoom)
    }

    Loader {
        id: imageLoader
        active: root.visible && root.placement !== null
            && root.placement.active && root.placement.page === root.page
        x: active ? root.placement.xPoints * root.zoom : 0
        y: active ? root.placement.yPoints * root.zoom : 0
        width: active ? root.placement.widthPoints * root.zoom : 0
        height: active ? root.placement.heightPoints * root.zoom : 0

        sourceComponent: FocusScope {
            id: imageFrame
            objectName: "headerImageFrame"
            activeFocusOnTab: true
            Accessible.role: Accessible.Graphic
            Accessible.name: root.editController.footerMode
                //% "Footer image"
                ? qsTrId("FooterImage")
                //% "Header image"
                : qsTrId("HeaderImage")
            Component.onCompleted: forceActiveFocus()

            Image {
                id: preview
                objectName: "headerImagePreview"
                anchors.centerIn: parent
                readonly property bool quarterTurn: root.placement !== null
                    && root.placement.quarterTurns % 2 !== 0
                width: quarterTurn ? imageFrame.height : imageFrame.width
                height: quarterTurn ? imageFrame.width : imageFrame.height
                source: root.placement ? root.placement.previewSource : ""
                sourceSize: Qt.size(1024, 1024)
                rotation: root.placement ? root.placement.quarterTurns * 90 : 0
                fillMode: Image.Stretch
                smooth: true
                asynchronous: true
                cache: false
                Accessible.ignored: true
                onStatusChanged: {
                    if (status === Image.Error && root.placement) {
                        root.placement.errorText = root.editController.footerMode
                            //% "Could not display the footer image. Choose the image again."
                            ? qsTrId("FooterImagePreviewFailed")
                            //% "Could not display the header image. Choose the image again."
                            : qsTrId("HeaderImagePreviewFailed")
                    }
                }
            }

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: root.theme.foxitOrange
                border.width: 1
                Accessible.ignored: true
            }

            MouseArea {
                anchors.fill: parent
                enabled: root.placement !== null && root.placement.canSubmit
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                property point startPoint
                property rect startFrame
                onPressed: mouse => {
                    imageFrame.forceActiveFocus()
                    startPoint = mapToItem(root, mouse.x, mouse.y)
                    startFrame = Qt.rect(root.placement.xPoints, root.placement.yPoints,
                        root.placement.widthPoints, root.placement.heightPoints)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const point = mapToItem(root, mouse.x, mouse.y)
                    root.placement.updateFrame(startFrame.x + (point.x - startPoint.x) / root.zoom,
                        startFrame.y + (point.y - startPoint.y) / root.zoom,
                        startFrame.width, startFrame.height)
                }
            }

            Repeater {
                model: [
                    { horizontal: -1, vertical: -1 }, { horizontal: 0, vertical: -1 },
                    { horizontal: 1, vertical: -1 }, { horizontal: 1, vertical: 0 },
                    { horizontal: 1, vertical: 1 }, { horizontal: 0, vertical: 1 },
                    { horizontal: -1, vertical: 1 }, { horizontal: -1, vertical: 0 }
                ]
                delegate: Rectangle {
                    id: resizeHandle
                    required property var modelData
                    required property int index
                    objectName: "headerImageResizeHandle" + index
                    x: (modelData.horizontal + 1) * imageFrame.width / 2 - width / 2
                    y: (modelData.vertical + 1) * imageFrame.height / 2 - height / 2
                    width: 8
                    height: 8
                    color: "white"
                    border.color: root.theme.foxitOrange
                    Accessible.ignored: true

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        enabled: root.placement !== null && root.placement.canSubmit
                        cursorShape: resizeHandle.modelData.horizontal === 0 ? Qt.SizeVerCursor
                            : resizeHandle.modelData.vertical === 0 ? Qt.SizeHorCursor
                            : resizeHandle.modelData.horizontal === resizeHandle.modelData.vertical
                                ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                        property point startPoint
                        property rect startFrame
                        onPressed: mouse => {
                            imageFrame.forceActiveFocus()
                            startPoint = mapToItem(root, mouse.x, mouse.y)
                            startFrame = Qt.rect(root.placement.xPoints, root.placement.yPoints,
                                root.placement.widthPoints, root.placement.heightPoints)
                        }
                        onPositionChanged: mouse => {
                            if (!pressed)
                                return
                            const point = mapToItem(root, mouse.x, mouse.y)
                            root.placement.resizeFrame(startFrame, resizeHandle.modelData.horizontal,
                                resizeHandle.modelData.vertical, (point.x - startPoint.x) / root.zoom,
                                (point.y - startPoint.y) / root.zoom)
                        }
                    }
                }
            }

            Keys.onPressed: event => {
                if (!root.placement || !root.placement.canSubmit)
                    return
                if (event.key === Qt.Key_Escape || event.key === Qt.Key_Delete
                        || event.key === Qt.Key_Backspace) {
                    root.placement.cancel()
                    event.accepted = true
                } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right
                        || event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                    const step = (event.modifiers & Qt.ShiftModifier) !== 0 ? 10 : 1
                    const dx = event.key === Qt.Key_Left ? -step : event.key === Qt.Key_Right ? step : 0
                    const dy = event.key === Qt.Key_Up ? -step : event.key === Qt.Key_Down ? step : 0
                    root.placement.updateFrame(root.placement.xPoints + dx,
                        root.placement.yPoints + dy, root.placement.widthPoints, root.placement.heightPoints)
                    event.accepted = true
                }
            }

            Connections {
                target: root.placement
                function onFocusRequested() { imageFrame.forceActiveFocus() }
            }
        }
    }
}
