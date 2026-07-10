import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import TrePDF

ApplicationWindow {
    id: window

    width: 1200
    height: 800
    minimumWidth: 760
    minimumHeight: 520
    visible: true
    title: qsTr("TrePDF")

    property real zoom: 1.0
    property string transientStatus: ""

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

    function scrollToPage(page) {
        documentView.scrollToPage(page)
    }

    function goToPage(page) {
        pdfDocument.setCurrentPage(page)
        Qt.callLater(function() { scrollToPage(pdfDocument.currentPage) })
    }

    function openFile(url) {
        if (pdfDocument.open(url)) {
            Qt.callLater(fitToWidth)
            Qt.callLater(function() { scrollToPage(0) })
        }
    }

    function showComingSoon(toolName) {
        transientStatus = qsTr("%1 is coming soon").arg(toolName)
        transientStatusTimer.restart()
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

    FileDialog {
        id: openDialog

        title: qsTr("Open PDF")
        nameFilters: [qsTr("PDF files (*.pdf)")]
        onAccepted: window.openFile(selectedFile)
    }

    header: AppRibbon {
        width: window.width
        theme: appTheme
        document: pdfDocument
        onOpenRequested: openDialog.open()
        onPageRequested: page => window.goToPage(page)
        onZoomOutRequested: window.setZoom(window.zoom / 1.15)
        onZoomInRequested: window.setZoom(window.zoom * 1.15)
        onFitWidthRequested: window.fitToWidth()
        onToolRequested: toolName => window.showComingSoon(toolName)
    }

    DocumentView {
        id: documentView

        anchors.fill: parent
        theme: appTheme
        document: pdfDocument
        zoom: window.zoom
        transientStatus: window.transientStatus
        onOpenRequested: openDialog.open()
    }
}
