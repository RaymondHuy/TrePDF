pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFDocumentSession session
    property string transientStatus

    signal openRequested()

    function setZoom(value) {
        session.zoom = Math.max(0.1, Math.min(5.0, value))
    }

    function fitToWidth() {
        if (!session.document.loaded
                || session.document.maximumPageWidth <= 0
                || documentView.viewportWidth <= 0) {
            return
        }

        setZoom((documentView.viewportWidth - 96)
                / session.document.maximumPageWidth)
    }

    function navigateToPage(page) {
        documentView.navigateToPage(page)
    }

    function prepareInlineStyleChange() {
        return session.editController.textEditActive
            || documentView.beginSelectedEditing()
    }

    function initializeView() {
        if (session.initialFitPending) {
            fitToWidth()
            session.initialFitPending = false
            Qt.callLater(function() {
                if (root.session.document.loaded)
                    documentView.navigateToPage(root.session.document.currentPage)
            })
        } else {
            documentView.restorePosition(session.scrollX, session.scrollY)
        }
    }

    function prepareForDeactivation() {
        documentView.prepareForSessionChange()
        session.scrollX = documentView.contentX
        session.scrollY = documentView.contentY
        session.prepareForDeactivation()
    }

    Component.onDestruction: prepareForDeactivation()

    Timer {
        interval: 0
        running: true
        repeat: false
        onTriggered: root.initializeView()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        DocumentView {
            id: documentView

            Layout.fillWidth: true
            Layout.fillHeight: true
            theme: root.theme
            document: root.session.document
            editController: root.session.editController
            zoom: root.session.zoom
            transientStatus: root.transientStatus
            onOpenRequested: root.openRequested()
        }

        Loader {
            id: textFormatPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && root.session.editController.tool !== PDFEditController.BrowseTool
            visible: active

            sourceComponent: Component {
                TextFormatPanel {
                    theme: root.theme
                    collapsed: root.session.panelCollapsed
                    availableFontFamilies:
                        root.session.editController.availableFontFamilies
                    fontFamiliesForFilter: function(filter) {
                        return root.session.editController.fontFamiliesForFilter(filter)
                    }
                    selectedFontFamily:
                        root.session.editController.selectedFontFamily
                    selectedFontBold: root.session.editController.selectedFontBold
                    selectedFontItalic:
                        root.session.editController.selectedFontItalic
                    selectedFontUnderline:
                        root.session.editController.selectedFontUnderline
                    selectedColor: root.session.editController.selectedColor
                    formattingEnabled:
                        root.session.editController.selectedEditable
                        && (root.session.editController.selectedKind === "text"
                            || root.session.editController.selectedKind
                                === "addedText")
                    onFontFamilyChangeRequested: fontFamily => {
                        root.session.editController.setSelectedFontFamily(fontFamily)
                    }
                    onFontBoldChangeRequested: enabled => {
                        const editWasActive = root.session.editController.textEditActive
                        if (root.prepareInlineStyleChange()) {
                            root.session.editController.setSelectedFontBold(
                                editWasActive
                                    ? enabled
                                    : !root.session.editController.selectedFontBold)
                        }
                    }
                    onFontItalicChangeRequested: enabled => {
                        const editWasActive = root.session.editController.textEditActive
                        if (root.prepareInlineStyleChange()) {
                            root.session.editController.setSelectedFontItalic(
                                editWasActive
                                    ? enabled
                                    : !root.session.editController.selectedFontItalic)
                        }
                    }
                    onFontUnderlineChangeRequested: enabled => {
                        const editWasActive = root.session.editController.textEditActive
                        if (root.prepareInlineStyleChange()) {
                            root.session.editController.setSelectedFontUnderline(
                                editWasActive
                                    ? enabled
                                    : !root.session.editController.selectedFontUnderline)
                        }
                    }
                    onFontColorChangeRequested: color => {
                        root.session.editController.setSelectedColor(color)
                    }
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }
    }
}
