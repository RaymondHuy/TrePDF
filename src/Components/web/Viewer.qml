pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDFWeb

ApplicationWindow {
    id: root

    width: 1100
    height: 780
    visible: true
    title: pdf.fileName.length > 0
        ? qsTr("%1 — TrePDF").arg(pdf.fileName) : qsTr("TrePDF Web")
    color: "#eae7e2"

    property bool fitWidth: true
    property real zoom: 1
    property int currentPage: 0
    readonly property bool hasDocument: pdf.pageCount > 0
    readonly property real maximumPageWidth: {
        let maximum = 1
        for (const page of pdf.pages)
            maximum = Math.max(maximum, page.width)
        return maximum
    }
    readonly property real pageScale: fitWidth
        ? Math.max(0.05, (pages.width - 48) / maximumPageWidth) : zoom

    function changeZoom(delta: real): void {
        const page = currentPage
        zoom = Math.max(0.5, Math.min(2, pageScale + delta))
        fitWidth = false
        Qt.callLater(() => goToPage(page))
    }

    function fitToWidth(): void {
        const page = currentPage
        fitWidth = true
        Qt.callLater(() => goToPage(page))
    }

    function goToPage(page: int): void {
        if (!hasDocument)
            return
        const target = Math.max(0, Math.min(pdf.pageCount - 1, page))
        pages.positionViewAtIndex(target, ListView.Beginning)
        currentPage = target
    }

    function updateCurrentPage(): void {
        const x = pages.contentX + pages.width / 2
        const y = pages.contentY + Math.min(120, pages.height / 2)
        const page = pages.indexAt(x, y)
        if (page >= 0)
            currentPage = page
    }

    BrowserDocument { id: pdf }

    Connections {
        target: pdf
        function onPagesChanged(): void {
            root.currentPage = 0
            pages.positionViewAtBeginning()
        }
    }

    header: ToolBar {
        padding: 12
        background: Rectangle { color: "#faf9f6" }

        contentItem: ColumnLayout {
            spacing: 10

            RowLayout {
                spacing: 8
                Layout.fillWidth: true

                Label {
                    text: qsTr("TrePDF")
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                    color: "#25362c"
                }
                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: pdf.fileName
                    elide: Text.ElideMiddle
                    color: "#626962"
                    horizontalAlignment: Text.AlignHCenter
                }
                BusyIndicator {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    visible: pdf.busy && root.hasDocument
                    running: visible
                }
                Button {
                    text: qsTr("Open PDF")
                    enabled: pdf.ready && !pdf.busy
                    highlighted: true
                    onClicked: pdf.openFile()
                }
                ToolButton {
                    text: root.width < 540 ? qsTr("↓") : qsTr("Download")
                    Accessible.name: qsTr("Download original PDF")
                    enabled: root.hasDocument && !pdf.busy
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Download original PDF")
                    onClicked: pdf.downloadFile()
                }
                ToolButton {
                    text: root.width < 540 ? qsTr("×") : qsTr("Close")
                    Accessible.name: qsTr("Close PDF")
                    enabled: root.hasDocument && !pdf.busy
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Close PDF")
                    onClicked: pdf.closeFile()
                }
            }

            RowLayout {
                visible: root.hasDocument
                Layout.fillWidth: true
                spacing: 4

                ToolButton {
                    text: qsTr("‹")
                    Accessible.name: qsTr("Previous page")
                    enabled: root.currentPage > 0 && !pdf.busy
                    onClicked: root.goToPage(root.currentPage - 1)
                }
                Label {
                    Layout.minimumWidth: 64
                    text: qsTr("%1 / %2").arg(root.currentPage + 1).arg(pdf.pageCount)
                    horizontalAlignment: Text.AlignHCenter
                    Accessible.name: qsTr("Page %1 of %2")
                        .arg(root.currentPage + 1).arg(pdf.pageCount)
                }
                ToolButton {
                    text: qsTr("›")
                    Accessible.name: qsTr("Next page")
                    enabled: root.currentPage + 1 < pdf.pageCount && !pdf.busy
                    onClicked: root.goToPage(root.currentPage + 1)
                }
                Item { Layout.fillWidth: true }
                ToolButton {
                    text: qsTr("−")
                    Accessible.name: qsTr("Zoom out")
                    enabled: root.pageScale > 0.5
                    onClicked: root.changeZoom(-0.1)
                }
                Label {
                    Layout.minimumWidth: 44
                    text: qsTr("%1%").arg(Math.round(root.pageScale * 100))
                    horizontalAlignment: Text.AlignHCenter
                }
                ToolButton {
                    text: qsTr("+")
                    Accessible.name: qsTr("Zoom in")
                    enabled: root.pageScale < 2
                    onClicked: root.changeZoom(0.1)
                }
                ToolButton {
                    text: qsTr("Fit width")
                    checkable: true
                    checked: root.fitWidth
                    onClicked: root.fitToWidth()
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Label {
            Layout.fillWidth: true
            visible: pdf.error.length > 0
            text: pdf.error
            padding: 12
            wrapMode: Text.Wrap
            color: "#8b2f24"
            background: Rectangle { color: "#fff0e9" }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: pages
                anchors.fill: parent
                anchors.margins: 12
                visible: root.hasDocument
                clip: true
                reuseItems: true
                cacheBuffer: 0
                model: pdf.pages
                contentWidth: Math.max(width, root.maximumPageWidth * root.pageScale + 24)
                flickableDirection: Flickable.HorizontalAndVerticalFlick
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                ScrollBar.horizontal: ScrollBar {}
                onContentYChanged: root.updateCurrentPage()
                onHeightChanged: root.updateCurrentPage()

                delegate: Item {
                    id: card
                    required property int index
                    required property var modelData
                    property bool pooled: false
                    readonly property bool onScreen: !pooled && root.hasDocument
                        && y + height > pages.contentY
                        && y < pages.contentY + pages.height
                    width: pages.contentWidth
                    height: sheet.height + 36
                    ListView.onPooled: pooled = true
                    ListView.onReused: pooled = false

                    Rectangle {
                        id: sheet
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: Math.max(1, card.modelData.width * root.pageScale)
                        height: Math.max(1, card.modelData.height * root.pageScale)
                        color: "white"
                        border.color: "#d4d1ca"

                        PageRaster {
                            id: raster
                            anchors.fill: parent
                            anchors.margins: 1
                            document: pdf
                            page: card.index
                            renderWidth: Math.min(4096,
                                Math.max(1, Math.ceil(raster.width * root.Screen.devicePixelRatio)))
                            active: card.onScreen && !pdf.busy
                            Accessible.role: Accessible.Graphic
                            Accessible.name: qsTr("PDF page %1").arg(card.index + 1)
                        }
                        BusyIndicator {
                            anchors.centerIn: parent
                            running: card.onScreen && raster.loading
                            visible: running
                        }
                        Label {
                            anchors.centerIn: parent
                            width: Math.min(parent.width - 24, 360)
                            visible: raster.error.length > 0
                            text: raster.error
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                            color: "#8b2f24"
                        }
                    }
                    Label {
                        anchors.top: sheet.bottom
                        anchors.topMargin: 8
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Page %1").arg(card.index + 1)
                        font.pixelSize: 12
                        color: "#6c7169"
                    }
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 440)
                visible: !root.hasDocument
                spacing: 16

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    running: pdf.busy || (!pdf.ready && pdf.error.length === 0)
                    visible: running
                }
                Label {
                    Layout.fillWidth: true
                    text: pdf.busy ? qsTr("Opening PDF…")
                        : !pdf.ready && pdf.error.length === 0 ? qsTr("Preparing PDF viewer…")
                        : qsTr("Open a PDF to start reading")
                    font.pixelSize: 24
                    font.weight: Font.DemiBold
                    color: "#25362c"
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("View, scroll and zoom. Your PDF is processed locally in this browser.")
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    color: "#626962"
                }
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Choose a PDF")
                    enabled: pdf.ready && !pdf.busy
                    highlighted: true
                    onClicked: pdf.openFile()
                }
            }
        }
    }

    footer: Label {
        text: qsTr("Read-only preview · Download keeps the original PDF · Reloading clears the open file")
        padding: 10
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: 12
        color: "#626962"
        background: Rectangle { color: "#faf9f6" }
    }
}
