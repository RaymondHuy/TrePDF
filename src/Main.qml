pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF

ApplicationWindow {
    id: window

    width: 1200
    height: 800
    minimumWidth: 760
    minimumHeight: 520
    visible: true
    //% "TrePDF"
    title: qsTrId("ApplicationTitle")

    property real zoom: 1.0
    property string transientStatus: ""
    property string pendingDraftAction: ""
    property bool allowCloseWithDrafts: false

    palette.highlight: appTheme.foxitOrange
    palette.button: appTheme.foxitPaleOrange
    palette.buttonText: "#32251c"
    palette.window: "#f7f2ee"

    function setZoom(value) {
        zoom = Math.max(0.1, Math.min(5.0, value))
    }

    function fitToWidth() {
        if (!pdfDocument.loaded
                || pdfDocument.maximumPageWidth <= 0
                || documentView.viewportWidth <= 0) {
            return
        }

        setZoom((documentView.viewportWidth - 96) / pdfDocument.maximumPageWidth)
    }

    function goToPage(page) {
        documentView.navigateToPage(page)
    }

    function openFile(url) {
        if (pdfDocument.open(url)) {
            Qt.callLater(fitToWidth)
            Qt.callLater(function() { goToPage(0) })
        }
    }

    function requestOpen() {
        if (editController.hasDrafts) {
            pendingDraftAction = "open"
            discardDraftPopup.open()
        } else {
            openDialog.open()
        }
    }

    function continueAfterDiscard() {
        const action = pendingDraftAction
        pendingDraftAction = ""
        editController.discardDrafts()
        discardDraftPopup.close()

        if (action === "open") {
            Qt.callLater(function() { openDialog.open() })
        } else if (action === "quit") {
            allowCloseWithDrafts = true
            Qt.callLater(function() { window.close() })
        }
    }

    function showComingSoon(toolName) {
        //% "%1 is coming soon"
        showStatus(qsTrId("FeatureComingSoon").arg(toolName))
    }

    function showStatus(message) {
        transientStatus = message
        transientStatusTimer.restart()
    }

    onClosing: close => {
        if (!allowCloseWithDrafts && editController.hasDrafts) {
            close.accepted = false
            pendingDraftAction = "quit"
            discardDraftPopup.open()
        }
    }

    AppTheme {
        id: appTheme
    }

    Timer {
        id: transientStatusTimer

        interval: 2600
        onTriggered: window.transientStatus = ""
    }

    PDFDocument {
        id: pdfDocument
    }

    PDFEditController {
        id: editController

        document: pdfDocument
        onStatusMessage: message => window.showStatus(message)
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: editController.canUndo && !editController.textEditActive
        onActivated: editController.undo()
    }

    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: editController.canRedo && !editController.textEditActive
        onActivated: editController.redo()
    }

    FileDialog {
        id: openDialog

        //% "Open PDF"
        title: qsTrId("OpenPdf")
        //% "PDF files (*.pdf)"
        nameFilters: [qsTrId("PdfFileFilter")]
        onAccepted: window.openFile(selectedFile)
    }

    Popup {
        id: discardDraftPopup

        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
        width: Math.min(390, window.width - 32)
        height: discardDraftContent.implicitHeight + 32
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: 6
            color: "#fffdfb"
            border.color: appTheme.ribbonBorder
            border.width: 1
        }

        contentItem: ColumnLayout {
            id: discardDraftContent

            spacing: 14

            Label {
                Layout.fillWidth: true
                //% "Discard draft changes?"
                text: qsTrId("DiscardDraftChangesQuestion")
                color: appTheme.ribbonTextColor
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                //% "Your edits exist only in this session and have not been saved to the PDF."
                text: qsTrId("UnsavedEditsDescription")
                color: "#656268"
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item {
                    Layout.fillWidth: true
                }

                Button {
                    //% "Cancel"
                    text: qsTrId("Cancel")
                    onClicked: {
                        window.pendingDraftAction = ""
                        discardDraftPopup.close()
                    }
                }

                OrangeButton {
                    Layout.preferredWidth: 104
                    theme: appTheme
                    //% "Discard"
                    text: qsTrId("Discard")
                    onClicked: window.continueAfterDiscard()
                }
            }
        }
    }

    header: AppRibbon {
        width: window.width
        theme: appTheme
        document: pdfDocument
        editController: editController
        onOpenRequested: window.requestOpen()
        onPageRequested: page => window.goToPage(page)
        onZoomOutRequested: window.setZoom(window.zoom / 1.15)
        onZoomInRequested: window.setZoom(window.zoom * 1.15)
        onFitWidthRequested: window.fitToWidth()
        onToolRequested: toolName => window.showComingSoon(toolName)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        DocumentView {
            id: documentView

            Layout.fillWidth: true
            Layout.fillHeight: true
            theme: appTheme
            document: pdfDocument
            editController: editController
            zoom: window.zoom
            transientStatus: window.transientStatus
            onOpenRequested: window.requestOpen()
        }

        Loader {
            id: textFormatPanelLoader

            property bool panelCollapsed: false

            Layout.preferredWidth: active
                ? panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: pdfDocument.loaded
                && editController.tool !== PDFEditController.BrowseTool
            visible: active

            sourceComponent: Component {
                TextFormatPanel {
                    theme: appTheme
                    collapsed: textFormatPanelLoader.panelCollapsed
                    availableFontFamilies: editController.availableFontFamilies
                    selectedFontFamily: editController.selectedFontFamily
                    selectedFontBold: editController.selectedFontBold
                    selectedFontItalic: editController.selectedFontItalic
                    selectedFontUnderline: editController.selectedFontUnderline
                    selectedColor: editController.selectedColor
                    formattingEnabled: editController.selectedEditable
                    onFontFamilyChangeRequested: fontFamily => {
                        editController.setSelectedFontFamily(fontFamily)
                    }
                    onFontBoldChangeRequested: enabled => {
                        editController.setSelectedFontBold(enabled)
                    }
                    onFontItalicChangeRequested: enabled => {
                        editController.setSelectedFontItalic(enabled)
                    }
                    onFontUnderlineChangeRequested: enabled => {
                        editController.setSelectedFontUnderline(enabled)
                    }
                    onFontColorChangeRequested: color => {
                        editController.setSelectedColor(color)
                    }
                    onCollapsedChangeRequested: collapsed => {
                        textFormatPanelLoader.panelCollapsed = collapsed
                    }
                }
            }
        }
    }
}
