import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: pageCard

    required property int index
    required property AppTheme theme
    required property PDFDocument document
    property real zoom: 1.0
    property bool pooled: false
    readonly property real pageMargin: 32
    readonly property real viewWidth: ListView.view ? ListView.view.width : 0

    width: Math.max(viewWidth, document.maximumPageWidth * zoom + pageMargin * 2)
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

        document: pageCard.pooled ? null : pageCard.document
        page: pageCard.index
        zoom: pageCard.zoom
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
        border.color: pageCard.index === pageCard.document.currentPage
            ? pageCard.theme.foxitOrange
            : "#c6cad1"
        border.width: pageCard.index === pageCard.document.currentPage ? 2 : 1
        visible: pageView.width > 0 && pageView.height > 0 && !pageCard.pooled
    }

    Label {
        id: pageLabel

        text: qsTr("Page %1").arg(pageCard.index + 1)
        color: pageCard.index === pageCard.document.currentPage
            ? pageCard.theme.foxitDarkOrange
            : "#6b625d"
        font.pixelSize: 12
        x: Math.max(pageCard.pageMargin, (pageCard.width - width) / 2)
        y: pageView.height + 8
        visible: !pageCard.pooled
    }
}
