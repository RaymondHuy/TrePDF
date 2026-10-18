pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDFWeb as Backend
import ".."

Rectangle {
    id: root

    required property Backend.BrowserDocument document
    required property BrowserEditController editor
    property bool fitWidth: true
    property real zoom: 1
    property int currentPage: 0
    property int pageDisplayMode: ReaderModes.ContinuousMode
    property int quarterTurns: 0
    readonly property bool documentLoaded: document !== null && document.pageCount > 0
    readonly property bool singlePage: pageDisplayMode === ReaderModes.SinglePageMode
    readonly property bool swappedAxes: quarterTurns % 2 !== 0
    readonly property real maximumPageWidth: {
        if (!documentLoaded)
            return 1
        if (singlePage) {
            const page = document.pages[Math.max(0, Math.min(document.pageCount - 1, currentPage))]
            return swappedAxes ? page.height : page.width
        }
        let maximum = 1
        for (const page of document.pages)
            maximum = Math.max(maximum, swappedAxes ? page.height : page.width)
        return maximum
    }
    readonly property real effectiveZoom: fitWidth
        ? Math.max(0.01, Math.min(5, (pages.width - 32) / maximumPageWidth))
        : Math.max(0.1, Math.min(5, zoom))
    property bool repositioning: false
    property int pendingPage: 0

    color: "#f2efec"

    function goToPage(page: int): void {
        if (!documentLoaded) {
            repositioning = false
            return
        }
        repositioning = true
        visiblePageTimer.stop()
        pendingPage = Math.max(0, Math.min(document.pageCount - 1, page))
        currentPage = pendingPage
        pages.cancelFlick()
        positionTimer.restart()
    }

    function setZoom(value: real): void {
        if (!Number.isFinite(value))
            return
        const page = currentPage
        repositioning = true
        zoom = Math.max(0.1, Math.min(5, value))
        fitWidth = false
        goToPage(page)
    }

    function fitToWidth(): void {
        const page = currentPage
        repositioning = true
        fitWidth = true
        goToPage(page)
    }

    function rotate(delta: int): void {
        quarterTurns = ((quarterTurns + delta) % 4 + 4) % 4
    }

    function setPageDisplayMode(mode: int): void {
        if (mode === ReaderModes.SinglePageMode || mode === ReaderModes.ContinuousMode)
            pageDisplayMode = mode
    }

    function updateCurrentPage(): void {
        if (!documentLoaded || singlePage || repositioning)
            return
        let bestPage = currentPage
        let bestArea = 0
        for (const child of pages.contentItem.children) {
            if (child.visiblePageArea === undefined || child.pooled)
                continue
            const area = child.visiblePageArea
            if (area > bestArea) {
                bestArea = area
                bestPage = child.pageNumber
            }
        }
        if (bestArea > 0)
            currentPage = bestPage
    }

    onEffectiveZoomChanged: goToPage(currentPage)
    onPageDisplayModeChanged: goToPage(currentPage)
    onQuarterTurnsChanged: goToPage(currentPage)

    Connections {
        target: root.document

        function onPagesChanged(): void {
            root.repositioning = true
            visiblePageTimer.stop()
            positionTimer.stop()
            root.currentPage = 0
            root.pendingPage = 0
            root.zoom = 1
            root.fitWidth = true
            root.quarterTurns = 0
            root.pageDisplayMode = ReaderModes.ContinuousMode
            pages.contentX = 0
            pages.positionViewAtBeginning()
            if (root.documentLoaded)
                root.goToPage(0)
            else
                root.repositioning = false
        }
    }

    Timer {
        id: positionTimer
        interval: 0
        onTriggered: {
            pages.forceLayout()
            pages.positionViewAtIndex(root.singlePage ? 0 : root.pendingPage,
                                      ListView.Beginning)
            pages.contentX = Math.max(0, (pages.contentWidth - pages.width) / 2)
            Qt.callLater(function() {
                if (!positionTimer.running)
                    root.repositioning = false
            })
        }
    }

    Timer {
        id: visiblePageTimer
        interval: 16
        onTriggered: root.updateCurrentPage()
    }

    ListView {
        id: pages
        anchors.fill: parent
        anchors.margins: 8
        visible: root.documentLoaded
        clip: true
        reuseItems: true
        cacheBuffer: 0
        model: !root.documentLoaded ? [] : root.singlePage
            ? [root.document.pages[root.currentPage]] : root.document.pages
        contentWidth: Math.max(width, root.maximumPageWidth * root.effectiveZoom + 32)
        flickableDirection: Flickable.HorizontalAndVerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ScrollBar.horizontal: ScrollBar {}
        onContentYChanged: {
            if (!root.repositioning)
                visiblePageTimer.restart()
        }
        onContentXChanged: {
            if (!root.repositioning)
                visiblePageTimer.restart()
        }
        onHeightChanged: {
            if (!root.repositioning)
                visiblePageTimer.restart()
        }

        delegate: Item {
            id: card

            required property int index
            required property var modelData
            property bool pooled: false
            readonly property int pageNumber: root.singlePage ? root.currentPage : index
            readonly property real visiblePageArea: Math.max(0,
                Math.min(y + pageFrame.y + pageFrame.height, pages.contentY + pages.height)
                    - Math.max(y + pageFrame.y, pages.contentY)) * Math.max(0,
                Math.min(pageFrame.x + pageFrame.width, pages.contentX + pages.width)
                    - Math.max(pageFrame.x, pages.contentX))
            readonly property bool onScreen: !pooled && root.visible && root.documentLoaded
                && visiblePageArea > 0
            width: pages.contentWidth
            height: root.singlePage ? Math.max(pages.height, pageFrame.height + 24)
                                   : pageFrame.height + 24
            ListView.onPooled: pooled = true
            ListView.onReused: pooled = false

            Item {
                id: pageFrame
                anchors.horizontalCenter: parent.horizontalCenter
                y: root.singlePage ? Math.max(12, (card.height - height) / 2) : 12
                width: root.swappedAxes ? sheet.height : sheet.width
                height: root.swappedAxes ? sheet.width : sheet.height

                Rectangle {
                    id: sheet
                    anchors.centerIn: parent
                    width: Math.max(1, card.modelData.width * root.effectiveZoom)
                    height: Math.max(1, card.modelData.height * root.effectiveZoom)
                    rotation: root.quarterTurns * 90
                    color: "white"
                    border.color: "#d4d1ca"

                    Backend.PageRaster {
                        id: raster
                        anchors.fill: parent
                        anchors.margins: 1
                        document: card.onScreen ? root.document : null
                        page: card.pageNumber
                        renderWidth: Math.min(4096, Math.max(1,
                            Math.ceil(width * root.Screen.devicePixelRatio)))
                        active: card.onScreen && !root.document.busy
                        Accessible.role: Accessible.Graphic
                        Accessible.name: qsTr("PDF page %1").arg(card.pageNumber + 1)
                    }
                    PageEditOverlay {
                        anchors.fill: parent
                        anchors.margins: 1
                        controller: root.editor
                        page: card.pageNumber
                        visible: card.onScreen && root.editor !== null && root.editor.active
                        enabled: visible && root.editor.editingAllowed
                    }
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    running: card.onScreen && raster.loading
                    visible: running
                }
                Label {
                    anchors.centerIn: parent
                    width: Math.max(1, Math.min(parent.width - 24, 360))
                    visible: raster.error.length > 0
                    text: raster.error
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    color: "#8b2f24"
                }
            }
        }
    }
}
