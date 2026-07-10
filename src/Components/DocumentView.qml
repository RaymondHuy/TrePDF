pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    property real zoom: 1.0
    property string transientStatus
    readonly property real viewportWidth: viewer.width

    signal openRequested()

    color: "#f2efec"

    function scrollToPage(page) {
        if (!document.loaded || page < 0 || page >= document.pageCount)
            return

        viewer.positionViewAtIndex(page, ListView.Beginning)
        viewer.contentY = Math.max(0, viewer.contentY - 16)
    }

    function syncCurrentPageFromScroll() {
        if (!document.loaded || viewer.count <= 0)
            return

        const page = viewer.indexAt(viewer.contentX + viewer.width * 0.5,
                                    viewer.contentY + viewer.height * 0.35)
        if (page >= 0 && document.currentPage !== page)
            document.setCurrentPage(page)
    }

    ListView {
        id: viewer

        anchors.fill: parent
        visible: root.document.loaded
        model: root.document.loaded ? root.document.pageCount : 0
        spacing: 28
        reuseItems: true
        cacheBuffer: Math.min(Math.max(height * 0.5, 256), 640)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.AutoFlickIfNeeded
        currentIndex: root.document.currentPage

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

        onContentYChanged: root.syncCurrentPageFromScroll()

        delegate: PdfPageDelegate {
            theme: root.theme
            document: root.document
            zoom: root.zoom
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 14
        visible: !root.document.loaded

        Label {
            text: qsTr("No document open")
            font.pixelSize: 22
            color: "#33251d"
            Layout.alignment: Qt.AlignHCenter
        }

        OrangeButton {
            theme: root.theme
            text: qsTr("Open PDF")
            onClicked: root.openRequested()
            Layout.alignment: Qt.AlignHCenter
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: Math.min(parent.width - 32, statusToastLabel.implicitWidth + 28)
        height: 36
        radius: 4
        color: "#e63a3b3f"
        visible: root.transientStatus.length > 0
        z: 10

        Label {
            id: statusToastLabel

            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            text: root.transientStatus
            color: "white"
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
