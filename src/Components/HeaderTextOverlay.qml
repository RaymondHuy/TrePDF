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
        // Disabled MouseAreas can still override the Select cursor in Qt Quick.
        visible: enabled
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
                width: editorLoader.width
                height: Math.min(editorLoader.maximumHeightPoints,
                    Math.max(root.placement ? root.placement.initialHeightPoints : 0,
                        textInput.contentHeight + textInput.topPadding + textInput.bottomPadding))
                contentWidth: availableWidth
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                TextArea {
                    id: textInput
                    objectName: "headerInlineTextInput"
                    property bool initialized: false
                    property bool synchronizing: false
                    property int placementPage: -1

                    function restoreFocus() {
                        // A dialog can clear the window's root focus scope.
                        const window = textInput.Window.window
                        if (window)
                            window.requestActivate()
                        forceActiveFocus(Qt.OtherFocusReason)
                    }

                    function synchronize() {
                        if (!initialized || synchronizing || !root.placement
                                || !root.placement.active
                                || root.placement.page !== placementPage)
                            return
                        synchronizing = true
                        const state = root.editController.headerTextEditorState(textDocument)
                        root.placement.text = state.text
                        root.placement.colors = state.colors
                        root.placement.fontFamilies = state.fontFamilies
                        root.placement.fontFamily = state.fontFamily
                        root.placement.typingFamilyPosition = state.typingFamilyPosition
                        root.placement.cursorPosition = state.cursorPosition
                        root.placement.typingPosition = state.typingPosition
                        root.placement.textColor = state.color
                        synchronizing = false
                    }

                    // The page frame owns the width; a wrapped placeholder must
                    // not feed its implicit size back into the ScrollView.
                    implicitWidth: editorLoader.width
                    implicitHeight: contentHeight + topPadding + bottomPadding
                    width: editorScroll.availableWidth
                    padding: 0
                    topPadding: 1
                    font: root.editController.fontForDisplay(
                        root.placement ? root.placement.baseFontFamily : "",
                        100, root.placement ? root.placement.fontBold : false,
                        root.placement ? root.placement.fontItalic : false,
                        root.placement ? root.placement.fontUnderline : false,
                        root.placement ? root.placement.fontSize : 12)
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    persistentSelection: true
                    color: "black"
                    palette.highlight: root.theme.foxitOrange
                    placeholderText: root.editController.footerMode
                        //% "Enter footer text"
                        ? qsTrId("FooterTextPlaceholder")
                        //% "Enter header text"
                        : qsTrId("HeaderTextPlaceholder")
                    Accessible.name: root.editController.footerMode
                        //% "Footer text"
                        ? qsTrId("FooterText")
                        //% "Header text"
                        : qsTrId("HeaderText")
                    onTextChanged: {
                        if (!initialized || synchronizing)
                            return
                        if (text.length > 700) {
                            const lastCodeUnit = text.charCodeAt(699)
                            remove(lastCodeUnit >= 0xD800 && lastCodeUnit <= 0xDBFF
                                ? 699 : 700, text.length)
                        }
                        if (root.placement && root.placement.text !== text)
                            root.placement.text = text
                        Qt.callLater(synchronize)
                    }
                    onCursorPositionChanged: Qt.callLater(synchronize)
                    onActiveFocusChanged: {
                        if (root.placement)
                            root.placement.inputFocused = activeFocus
                        if (!activeFocus)
                            synchronize()
                    }
                    Component.onCompleted: {
                        placementPage = root.placement.page
                        text = root.placement.text
                        cursorPosition = Math.min(root.placement.cursorPosition, length)
                        root.editController.initializeHeaderTextEditor(textDocument,
                            root.placement.colors, root.placement.typingPosition,
                            root.placement.textColor, root.placement.fontFamilies,
                            root.placement.typingFamilyPosition, root.placement.fontFamily)
                        initialized = true
                        forceActiveFocus()
                    }
                    Component.onDestruction: {
                        synchronize()
                        initialized = false
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
                        function onFocusRequested() { textInput.restoreFocus() }
                        function onSynchronizeRequested() { textInput.synchronize() }
                        function onTypingFontFamilyRequested(value) {
                            root.editController.setHeaderTextEditorFontFamily(textInput.textDocument, value)
                            textInput.synchronize()
                            Qt.callLater(textInput.restoreFocus)
                        }
                        function onTypingColorRequested(value) {
                            root.editController.setHeaderTextEditorColor(textInput.textDocument, value)
                            textInput.synchronize()
                            Qt.callLater(textInput.restoreFocus)
                        }
                    }
                }
            }
        }
    }
}
