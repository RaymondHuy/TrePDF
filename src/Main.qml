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
    title: qsTr("TrePDF")

    property real zoom: 1.0

    function setZoom(value) {
        zoom = Math.max(0.1, Math.min(5.0, value))
    }

    function fitToWidth() {
        if (!pdfDocument.loaded || pdfDocument.currentPageWidth <= 0 || viewer.availableWidth <= 0)
            return

        setZoom((viewer.availableWidth - 96) / pdfDocument.currentPageWidth)
    }

    function openFile(url) {
        if (pdfDocument.open(url))
            Qt.callLater(fitToWidth)
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

    menuBar: MenuBar {
        Menu {
            title: qsTr("File")

            MenuItem {
                text: qsTr("Open...")
                onTriggered: openDialog.open()
            }

            MenuItem {
                text: qsTr("Close")
                enabled: pdfDocument.loaded
                onTriggered: pdfDocument.close()
            }

            MenuSeparator {}

            MenuItem {
                text: qsTr("Quit")
                onTriggered: Qt.quit()
            }
        }

        Menu {
            title: qsTr("View")

            MenuItem {
                text: qsTr("Previous Page")
                enabled: pdfDocument.currentPage > 0
                onTriggered: pdfDocument.previousPage()
            }

            MenuItem {
                text: qsTr("Next Page")
                enabled: pdfDocument.loaded && pdfDocument.currentPage + 1 < pdfDocument.pageCount
                onTriggered: pdfDocument.nextPage()
            }

            MenuSeparator {}

            MenuItem {
                text: qsTr("Zoom In")
                enabled: pdfDocument.loaded
                onTriggered: window.setZoom(window.zoom * 1.15)
            }

            MenuItem {
                text: qsTr("Zoom Out")
                enabled: pdfDocument.loaded
                onTriggered: window.setZoom(window.zoom / 1.15)
            }

            MenuItem {
                text: qsTr("Fit Width")
                enabled: pdfDocument.loaded
                onTriggered: window.fitToWidth()
            }
        }
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            spacing: 6

            ToolButton {
                text: qsTr("Open")
                onClicked: openDialog.open()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Open PDF")
            }

            ToolSeparator {}

            ToolButton {
                text: "<"
                enabled: pdfDocument.currentPage > 0
                onClicked: pdfDocument.previousPage()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Previous Page")
            }

            Label {
                text: pdfDocument.loaded
                    ? qsTr("%1 / %2").arg(pdfDocument.currentPage + 1).arg(pdfDocument.pageCount)
                    : qsTr("- / -")
                horizontalAlignment: Text.AlignHCenter
                Layout.preferredWidth: 82
            }

            ToolButton {
                text: ">"
                enabled: pdfDocument.loaded && pdfDocument.currentPage + 1 < pdfDocument.pageCount
                onClicked: pdfDocument.nextPage()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Next Page")
            }

            ToolSeparator {}

            ToolButton {
                text: "-"
                enabled: pdfDocument.loaded
                onClicked: window.setZoom(window.zoom / 1.15)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Zoom Out")
            }

            Label {
                text: qsTr("%1%").arg(Math.round(window.zoom * 100))
                horizontalAlignment: Text.AlignHCenter
                Layout.preferredWidth: 64
            }

            ToolButton {
                text: "+"
                enabled: pdfDocument.loaded
                onClicked: window.setZoom(window.zoom * 1.15)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Zoom In")
            }

            ToolButton {
                text: qsTr("Fit")
                enabled: pdfDocument.loaded
                onClicked: window.fitToWidth()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Fit Width")
            }

            Item {
                Layout.fillWidth: true
            }

            Label {
                text: pdfDocument.statusText
                elide: Text.ElideMiddle
                Layout.maximumWidth: 420
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#eef0f3"

        ScrollView {
            id: viewer

            anchors.fill: parent
            visible: pdfDocument.loaded
            contentWidth: Math.max(availableWidth, pageSurface.implicitWidth)
            contentHeight: Math.max(availableHeight, pageSurface.implicitHeight)

            Item {
                id: pageSurface

                implicitWidth: pageView.implicitWidth + 64
                implicitHeight: pageView.implicitHeight + 64
                width: viewer.contentWidth
                height: viewer.contentHeight

                Rectangle {
                    x: pageView.x + 4
                    y: pageView.y + 6
                    width: pageView.width
                    height: pageView.height
                    color: "#24000000"
                    radius: 2
                    visible: pageView.width > 0 && pageView.height > 0
                }

                PDFPageView {
                    id: pageView

                    document: pdfDocument
                    page: pdfDocument.currentPage
                    zoom: window.zoom
                    pageColor: "white"
                    x: Math.max(32, (pageSurface.width - width) / 2)
                    y: 32
                    width: implicitWidth
                    height: implicitHeight
                }

                Rectangle {
                    x: pageView.x
                    y: pageView.y
                    width: pageView.width
                    height: pageView.height
                    color: "transparent"
                    border.color: "#c6cad1"
                    border.width: 1
                    visible: pageView.width > 0 && pageView.height > 0
                }
            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 14
            visible: !pdfDocument.loaded

            Label {
                text: qsTr("No document open")
                font.pixelSize: 22
                color: "#252a31"
                Layout.alignment: Qt.AlignHCenter
            }

            Button {
                text: qsTr("Open PDF")
                onClicked: openDialog.open()
                Layout.alignment: Qt.AlignHCenter
            }
        }
    }
}
