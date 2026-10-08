pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root
    objectName: "headerTextOverlay"

    required property AppTheme theme
    required property PDFEditController editController
    required property int page
    required property real zoom
    property HeaderTextPlacement placement: null

    MouseArea {
        objectName: "headerTextPlacementArea"
        anchors.fill: parent
        enabled: root.placement !== null && root.placement.armed
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.IBeamCursor
        onClicked: mouse => root.placement.place(root.page,
            mouse.x / root.zoom, mouse.y / root.zoom)
    }

    Loader {
        id: editorLoader
        active: root.visible && root.placement !== null
            && root.placement.active && root.placement.page === root.page
        x: active ? root.placement.xPoints * root.zoom : 0
        y: active ? root.placement.yPoints * root.zoom : 0
        width: active ? root.placement.widthPoints : 0
        readonly property real maximumHeightPoints: active ? Math.max(0, root.height / root.zoom
            - root.placement.yPoints) : 0
        scale: root.zoom
        transformOrigin: Item.TopLeft

        sourceComponent: Component {
            ScrollView {
                id: editorScroll
                height: Math.min(editorLoader.maximumHeightPoints,
                    Math.max(root.placement ? root.placement.initialHeightPoints : 0,
                        textInput.contentHeight + textInput.topPadding + textInput.bottomPadding))
                contentWidth: availableWidth
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                TextArea {
                    id: textInput
                    objectName: "headerInlineTextInput"
                    width: editorScroll.availableWidth
                    padding: 0
                    topPadding: 1
                    font: root.editController.fontForDisplay(
                        root.placement ? root.placement.fontFamily : "",
                        100, root.placement ? root.placement.fontBold : false,
                        root.placement ? root.placement.fontItalic : false,
                        root.placement ? root.placement.fontUnderline : false,
                        root.placement ? root.placement.fontSize : 12)
                    text: root.placement ? root.placement.text : ""
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    color: root.placement ? root.placement.textColor : "black"
                    palette.highlight: root.theme.foxitOrange
                    //% "Enter header text"
                    placeholderText: qsTrId("HeaderTextPlaceholder")
                    //% "Header text"
                    Accessible.name: qsTrId("HeaderText")
                    onTextChanged: {
                        if (text.length > 700) {
                            const lastCodeUnit = text.charCodeAt(699)
                            remove(lastCodeUnit >= 0xD800 && lastCodeUnit <= 0xDBFF
                                ? 699 : 700, text.length)
                        }
                        if (root.placement && root.placement.text !== text)
                            root.placement.text = text
                    }
                    onActiveFocusChanged: {
                        if (root.placement)
                            root.placement.inputFocused = activeFocus
                    }
                    Component.onCompleted: forceActiveFocus()
                    Component.onDestruction: {
                        if (root.placement && activeFocus)
                            root.placement.inputFocused = false
                    }
                    Keys.onEscapePressed: event => {
                        root.placement.cancel()
                        event.accepted = true
                    }
                    background: Rectangle {
                        color: "#e6ffffff"
                        border.color: root.theme.foxitOrange
                        border.width: 1 / root.zoom
                    }

                    Connections {
                        target: root.placement
                        function onFocusRequested() { textInput.forceActiveFocus() }
                    }
                }
            }
        }
    }
}
