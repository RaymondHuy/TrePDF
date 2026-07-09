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
    readonly property color foxitOrange: "#d97a32"
    readonly property color foxitDarkOrange: "#b85f22"
    readonly property color foxitHoverOrange: "#e99052"
    readonly property color foxitPaleOrange: "#fff4ec"
    readonly property color foxitBorderOrange: "#c66b2a"
    readonly property color toolbarTextColor: "#fffaf6"
    readonly property color disabledToolbarTextColor: "#f2c4a5"

    palette.highlight: foxitOrange
    palette.button: foxitPaleOrange
    palette.buttonText: "#32251c"
    palette.window: "#f7f2ee"

    component OrangeToolButton: ToolButton {
        id: control

        leftPadding: 10
        rightPadding: 10
        topPadding: 6
        bottomPadding: 6

        contentItem: Text {
            text: control.text
            font: control.font
            color: control.enabled ? window.toolbarTextColor : window.disabledToolbarTextColor
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            implicitWidth: 34
            implicitHeight: 30
            radius: 4
            color: !control.enabled
                ? "transparent"
                : control.down
                    ? window.foxitDarkOrange
                    : control.hovered
                        ? window.foxitHoverOrange
                        : "transparent"
            border.color: control.hovered && control.enabled ? "#efb27f" : "transparent"
            border.width: 1
        }
    }

    component OrangeButton: Button {
        id: control

        contentItem: Text {
            text: control.text
            font: control.font
            color: control.enabled ? "#ffffff" : "#f2c4a5"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            implicitWidth: 112
            implicitHeight: 34
            radius: 4
            color: control.down
                ? window.foxitDarkOrange
                : control.hovered
                    ? window.foxitHoverOrange
                    : window.foxitOrange
            border.color: window.foxitBorderOrange
            border.width: 1
        }
    }

    function setZoom(value) {
        zoom = Math.max(0.1, Math.min(5.0, value))
    }

    function fitToWidth() {
        if (!pdfDocument.loaded || pdfDocument.maximumPageWidth <= 0 || viewer.width <= 0)
            return

        setZoom((viewer.width - 96) / pdfDocument.maximumPageWidth)
    }

    function scrollToPage(page) {
        if (!pdfDocument.loaded || page < 0 || page >= pdfDocument.pageCount)
            return

        viewer.positionViewAtIndex(page, ListView.Beginning)
        viewer.contentY = Math.max(0, viewer.contentY - 16)
    }

    function goToPage(page) {
        pdfDocument.setCurrentPage(page)
        Qt.callLater(function() { scrollToPage(pdfDocument.currentPage) })
    }

    function syncCurrentPageFromScroll() {
        if (!pdfDocument.loaded || viewer.count <= 0)
            return

        const page = viewer.indexAt(viewer.contentX + viewer.width * 0.5,
                                    viewer.contentY + viewer.height * 0.35)
        if (page >= 0 && pdfDocument.currentPage !== page) {
            pdfDocument.setCurrentPage(page)
        }
    }

    function openFile(url) {
        if (pdfDocument.open(url)) {
            Qt.callLater(fitToWidth)
            Qt.callLater(function() { scrollToPage(0) })
        }
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
        background: Rectangle {
            color: window.foxitOrange
        }

        delegate: MenuBarItem {
            id: menuBarItem

            contentItem: Text {
                text: menuBarItem.text
                font: menuBarItem.font
                color: window.toolbarTextColor
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                color: menuBarItem.highlighted ? window.foxitDarkOrange : "transparent"
            }
        }

        Menu {
            title: qsTr("File")
            background: Rectangle {
                color: "#fff8f2"
                border.color: "#e0b08d"
                border.width: 1
            }

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
            background: Rectangle {
                color: "#fff8f2"
                border.color: "#e0b08d"
                border.width: 1
            }

            MenuItem {
                text: qsTr("Previous Page")
                enabled: pdfDocument.currentPage > 0
                onTriggered: window.goToPage(pdfDocument.currentPage - 1)
            }

            MenuItem {
                text: qsTr("Next Page")
                enabled: pdfDocument.loaded && pdfDocument.currentPage + 1 < pdfDocument.pageCount
                onTriggered: window.goToPage(pdfDocument.currentPage + 1)
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
        background: Rectangle {
            color: window.foxitOrange
        }

        RowLayout {
            anchors.fill: parent
            spacing: 6

            OrangeToolButton {
                text: qsTr("Open")
                onClicked: openDialog.open()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Open PDF")
            }

            ToolSeparator {}

            OrangeToolButton {
                text: "<"
                enabled: pdfDocument.currentPage > 0
                onClicked: window.goToPage(pdfDocument.currentPage - 1)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Previous Page")
            }

            Label {
                text: pdfDocument.loaded
                    ? qsTr("%1 / %2").arg(pdfDocument.currentPage + 1).arg(pdfDocument.pageCount)
                    : qsTr("- / -")
                horizontalAlignment: Text.AlignHCenter
                color: window.toolbarTextColor
                Layout.preferredWidth: 82
            }

            OrangeToolButton {
                text: ">"
                enabled: pdfDocument.loaded && pdfDocument.currentPage + 1 < pdfDocument.pageCount
                onClicked: window.goToPage(pdfDocument.currentPage + 1)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Next Page")
            }

            ToolSeparator {}

            OrangeToolButton {
                text: "-"
                enabled: pdfDocument.loaded
                onClicked: window.setZoom(window.zoom / 1.15)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Zoom Out")
            }

            Label {
                text: qsTr("%1%").arg(Math.round(window.zoom * 100))
                horizontalAlignment: Text.AlignHCenter
                color: window.toolbarTextColor
                Layout.preferredWidth: 64
            }

            OrangeToolButton {
                text: "+"
                enabled: pdfDocument.loaded
                onClicked: window.setZoom(window.zoom * 1.15)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Zoom In")
            }

            OrangeToolButton {
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
                color: window.toolbarTextColor
                Layout.maximumWidth: 420
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#f2efec"

        ListView {
            id: viewer

            anchors.fill: parent
            visible: pdfDocument.loaded
            model: pdfDocument.loaded ? pdfDocument.pageCount : 0
            spacing: 28
            reuseItems: true
            cacheBuffer: Math.max(height * 1.5, 1200)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.AutoFlickIfNeeded
            currentIndex: pdfDocument.currentPage

            header: Item {
                width: ListView.view ? ListView.view.width : 0
                height: 32
            }

            footer: Item {
                width: ListView.view ? ListView.view.width : 0
                height: 32
            }

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            ScrollBar.horizontal: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            onContentYChanged: window.syncCurrentPageFromScroll()

            delegate: Item {
                id: pageCard

                required property int index

                property bool pooled: false
                readonly property real pageMargin: 32

                width: Math.max(ListView.view.width, pdfDocument.maximumPageWidth * window.zoom + pageMargin * 2)
                height: pageView.implicitHeight + pageLabel.implicitHeight + 12

                ListView.onPooled: pooled = true
                ListView.onReused: pooled = false

                Rectangle {
                    x: pageView.x + 4
                    y: pageView.y + 6
                    width: pageView.width
                    height: pageView.height
                    color: "#24000000"
                    radius: 2
                    visible: pageView.width > 0 && pageView.height > 0 && !pageCard.pooled
                }

                PDFPageView {
                    id: pageView

                    document: pageCard.pooled ? null : pdfDocument
                    page: pageCard.index
                    zoom: window.zoom
                    pageColor: "white"
                    x: Math.max(pageCard.pageMargin, (pageCard.width - width) / 2)
                    y: 0
                    width: implicitWidth
                    height: implicitHeight
                    visible: !pageCard.pooled
                }

                Rectangle {
                    x: pageView.x
                    y: pageView.y
                    width: pageView.width
                    height: pageView.height
                    color: "transparent"
                    border.color: pageCard.index === pdfDocument.currentPage ? window.foxitOrange : "#c6cad1"
                    border.width: pageCard.index === pdfDocument.currentPage ? 2 : 1
                    visible: pageView.width > 0 && pageView.height > 0 && !pageCard.pooled
                }

                Label {
                    id: pageLabel

                    text: qsTr("Page %1").arg(pageCard.index + 1)
                    color: pageCard.index === pdfDocument.currentPage ? window.foxitDarkOrange : "#6b625d"
                    font.pixelSize: 12
                    x: Math.max(pageCard.pageMargin, (pageCard.width - width) / 2)
                    y: pageView.height + 8
                    visible: !pageCard.pooled
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
                color: "#33251d"
                Layout.alignment: Qt.AlignHCenter
            }

            OrangeButton {
                text: qsTr("Open PDF")
                onClicked: openDialog.open()
                Layout.alignment: Qt.AlignHCenter
            }
        }
    }
}
