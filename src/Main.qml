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
    title: qsTr("TrePDF")

    property real zoom: 1.0
    property string transientStatus: ""
    readonly property color foxitOrange: "#d97a32"
    readonly property color foxitDarkOrange: "#b85f22"
    readonly property color foxitHoverOrange: "#e99052"
    readonly property color foxitPaleOrange: "#fff4ec"
    readonly property color foxitBorderOrange: "#c66b2a"
    readonly property color ribbonSurface: "#fffdfb"
    readonly property color ribbonBorder: "#ddd6d1"
    readonly property color ribbonTextColor: "#35363a"
    readonly property color signGreen: "#2f7656"
    readonly property color signPaleGreen: "#edf7f1"

    palette.highlight: foxitOrange
    palette.button: foxitPaleOrange
    palette.buttonText: "#32251c"
    palette.window: "#f7f2ee"

    component RibbonTabButton: TabButton {
        id: control

        implicitWidth: 104
        implicitHeight: 34

        contentItem: Text {
            text: control.text
            font.family: control.font.family
            font.pixelSize: control.font.pixelSize
            font.weight: control.checked ? Font.DemiBold : Font.Medium
            color: control.checked ? window.foxitDarkOrange : "#56575c"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            color: control.checked
                ? window.foxitPaleOrange
                : control.hovered
                    ? "#f6f1ed"
                    : "transparent"

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 3
                color: control.checked ? window.foxitOrange : "transparent"
            }
        }
    }

    component RibbonButton: ToolButton {
        id: control

        property url iconSource
        property string toolTipText: text
        property var command
        property bool requiresDocument: true
        property bool primary: false
        property bool signing: false

        implicitWidth: 80
        implicitHeight: 56
        enabled: !requiresDocument || pdfDocument.loaded
        display: AbstractButton.TextUnderIcon
        spacing: 3
        leftPadding: 5
        rightPadding: 5
        topPadding: 5
        bottomPadding: 4
        font.pixelSize: 11
        palette.buttonText: enabled ? window.ribbonTextColor : "#a7a4a1"
        icon.source: iconSource
        icon.width: 22
        icon.height: 22
        icon.color: !enabled
            ? "#aaa8a6"
            : signing
                ? window.signGreen
                : primary
                    ? window.foxitDarkOrange
                    : "#4a4b50"

        onClicked: {
            if (control.command)
                control.command()
            else
                window.showComingSoon(control.text)
        }

        background: Rectangle {
            radius: 4
            color: !control.enabled
                ? "transparent"
                : control.down
                    ? control.signing ? "#dceee4" : "#f5dfcf"
                    : control.hovered || control.primary
                        ? control.signing ? window.signPaleGreen : window.foxitPaleOrange
                        : "transparent"
            border.color: control.primary
                ? control.signing ? "#a8cfb9" : "#edc5a6"
                : "transparent"
            border.width: 1
        }

        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: toolTipText
    }

    component RibbonGroup: Item {
        id: group

        default property alias tools: toolRow.data
        property string title

        implicitWidth: toolRow.implicitWidth + 18
        implicitHeight: 76

        Row {
            id: toolRow

            x: 6
            y: 2
            spacing: 2
        }

        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            text: group.title
            color: "#817c78"
            font.pixelSize: 10
            horizontalAlignment: Text.AlignHCenter
        }

        Rectangle {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.topMargin: 6
            anchors.bottomMargin: 6
            width: 1
            color: window.ribbonBorder
            Accessible.ignored: true
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

    function showComingSoon(toolName) {
        transientStatus = qsTr("%1 is coming soon").arg(toolName)
        transientStatusTimer.restart()
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

    Component {
        id: viewRibbonComponent

        Flickable {
            clip: true
            contentWidth: viewRibbonRow.implicitWidth + 16
            contentHeight: height
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: viewRibbonRow

                x: 8
                height: 76

                RibbonGroup {
                    title: qsTr("File")

                    RibbonButton {
                        text: qsTr("Open")
                        toolTipText: qsTr("Open PDF")
                        iconSource: "assets/icons/folder-open.svg"
                        requiresDocument: false
                        primary: true
                        command: function() { openDialog.open() }
                    }
                }

                RibbonGroup {
                    title: qsTr("Navigation")

                    RibbonButton {
                        text: qsTr("Previous")
                        toolTipText: qsTr("Previous Page")
                        iconSource: "assets/icons/chevron-left.svg"
                        enabled: pdfDocument.currentPage > 0
                        command: function() { window.goToPage(pdfDocument.currentPage - 1) }
                    }

                    RibbonButton {
                        text: qsTr("Next")
                        toolTipText: qsTr("Next Page")
                        iconSource: "assets/icons/chevron-right.svg"
                        enabled: pdfDocument.loaded && pdfDocument.currentPage + 1 < pdfDocument.pageCount
                        command: function() { window.goToPage(pdfDocument.currentPage + 1) }
                    }
                }

                RibbonGroup {
                    title: qsTr("Zoom")

                    RibbonButton {
                        text: qsTr("Zoom Out")
                        iconSource: "assets/icons/zoom-out.svg"
                        command: function() { window.setZoom(window.zoom / 1.15) }
                    }

                    RibbonButton {
                        text: qsTr("Zoom In")
                        iconSource: "assets/icons/zoom-in.svg"
                        command: function() { window.setZoom(window.zoom * 1.15) }
                    }

                    RibbonButton {
                        text: qsTr("Fit Width")
                        iconSource: "assets/icons/maximize-2.svg"
                        command: function() { window.fitToWidth() }
                    }
                }

                RibbonGroup {
                    title: qsTr("Page")

                    RibbonButton {
                        text: qsTr("Rotate Left")
                        iconSource: "assets/icons/rotate-ccw.svg"
                    }

                    RibbonButton {
                        text: qsTr("Rotate Right")
                        iconSource: "assets/icons/rotate-cw.svg"
                    }
                }

                RibbonGroup {
                    title: qsTr("Find & Panels")

                    RibbonButton {
                        text: qsTr("Thumbnails")
                        iconSource: "assets/icons/panel-left.svg"
                    }

                    RibbonButton {
                        text: qsTr("Search")
                        iconSource: "assets/icons/search.svg"
                    }
                }
            }

            ScrollBar.horizontal: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
        }
    }

    Component {
        id: editRibbonComponent

        Flickable {
            clip: true
            contentWidth: editRibbonRow.implicitWidth + 16
            contentHeight: height
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: editRibbonRow

                x: 8
                height: 76

                RibbonGroup {
                    title: qsTr("Content")

                    RibbonButton {
                        text: qsTr("Select")
                        iconSource: "assets/icons/mouse-pointer-2.svg"
                        primary: true
                    }

                    RibbonButton {
                        text: qsTr("Edit Text")
                        iconSource: "assets/icons/type.svg"
                    }

                    RibbonButton {
                        text: qsTr("Add Text")
                        iconSource: "assets/icons/type.svg"
                    }
                }

                RibbonGroup {
                    title: qsTr("Objects")

                    RibbonButton {
                        text: qsTr("Image")
                        toolTipText: qsTr("Add Image")
                        iconSource: "assets/icons/image-plus.svg"
                    }

                    RibbonButton {
                        text: qsTr("Link")
                        toolTipText: qsTr("Add Link")
                        iconSource: "assets/icons/link-2.svg"
                    }

                    RibbonButton {
                        text: qsTr("Shapes")
                        iconSource: "assets/icons/shapes.svg"
                    }
                }

                RibbonGroup {
                    title: qsTr("Markup")

                    RibbonButton {
                        text: qsTr("Highlight")
                        iconSource: "assets/icons/highlighter.svg"
                    }

                    RibbonButton {
                        text: qsTr("Underline")
                        iconSource: "assets/icons/underline.svg"
                    }

                    RibbonButton {
                        text: qsTr("Strikeout")
                        iconSource: "assets/icons/strikethrough.svg"
                    }
                }

                RibbonGroup {
                    title: qsTr("History")

                    RibbonButton {
                        text: qsTr("Undo")
                        iconSource: "assets/icons/undo-2.svg"
                    }

                    RibbonButton {
                        text: qsTr("Redo")
                        iconSource: "assets/icons/redo-2.svg"
                    }
                }
            }

            ScrollBar.horizontal: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
        }
    }

    Component {
        id: signRibbonComponent

        Flickable {
            clip: true
            contentWidth: signRibbonRow.implicitWidth + 16
            contentHeight: height
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: signRibbonRow

                x: 8
                height: 76

                RibbonGroup {
                    title: qsTr("Sign")

                    RibbonButton {
                        text: qsTr("Fill && Sign")
                        iconSource: "assets/icons/pen-line.svg"
                        signing: true
                        primary: true
                    }

                    RibbonButton {
                        text: qsTr("Signature")
                        toolTipText: qsTr("Add Signature")
                        iconSource: "assets/icons/signature.svg"
                        signing: true
                    }

                    RibbonButton {
                        text: qsTr("Initials")
                        toolTipText: qsTr("Add Initials")
                        iconSource: "assets/icons/signature.svg"
                        signing: true
                    }
                }

                RibbonGroup {
                    title: qsTr("Fields")

                    RibbonButton {
                        text: qsTr("Date")
                        toolTipText: qsTr("Add Date")
                        iconSource: "assets/icons/calendar-days.svg"
                        signing: true
                    }

                    RibbonButton {
                        text: qsTr("Check")
                        toolTipText: qsTr("Add Check Mark")
                        iconSource: "assets/icons/square-check-big.svg"
                        signing: true
                    }
                }

                RibbonGroup {
                    title: qsTr("Workflow")

                    RibbonButton {
                        text: qsTr("Request")
                        toolTipText: qsTr("Request Signatures")
                        iconSource: "assets/icons/send.svg"
                        signing: true
                    }

                    RibbonButton {
                        text: qsTr("Validate")
                        toolTipText: qsTr("Validate Signatures")
                        iconSource: "assets/icons/shield-check.svg"
                        signing: true
                    }
                }
            }

            ScrollBar.horizontal: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
        }
    }

    header: Column {
        width: window.width
        spacing: 0

        Rectangle {
            width: parent.width
            height: 34
            color: "#fffaf6"

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: window.ribbonBorder
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 0

                TabBar {
                    id: ribbonTabs

                    Layout.preferredWidth: 312
                    Layout.fillHeight: true
                    spacing: 0
                    background: Item {}

                    RibbonTabButton {
                        text: qsTr("View")
                    }

                    RibbonTabButton {
                        text: qsTr("Edit")
                    }

                    RibbonTabButton {
                        text: qsTr("E-Sign")
                    }
                }

                Item {
                    Layout.fillWidth: true
                }
            }
        }

        Rectangle {
            width: parent.width
            height: 78
            color: window.ribbonSurface

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: window.ribbonBorder
            }

            Loader {
                anchors.fill: parent
                sourceComponent: ribbonTabs.currentIndex === 0
                    ? viewRibbonComponent
                    : ribbonTabs.currentIndex === 1
                        ? editRibbonComponent
                        : signRibbonComponent
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
            cacheBuffer: Math.min(Math.max(height * 0.5, 256), 640)
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

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 20
            width: Math.min(parent.width - 32, statusToastLabel.implicitWidth + 28)
            height: 36
            radius: 4
            color: "#e63a3b3f"
            visible: window.transientStatus.length > 0
            z: 10

            Label {
                id: statusToastLabel

                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                text: window.transientStatus
                color: "white"
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
