pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF
import "ios" as Platform

Rectangle {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    required property PDFSearchController searchController
    required property PDFFormController formController
    required property PDFFormDesignerController formDesignerController
    property bool viewTabActive: false
    property bool headerTabActive: false
    property bool footerMode: false
    property HeaderTextPlacement headerTextPlacement: null
    property HeaderImagePlacement headerImagePlacement: null
    property HeaderPageNumberPlacement headerPageNumberPlacement: null
    property bool formsTabActive: false
    property bool editTabActive: false
    property bool formFieldsVisible: false
    property bool formFieldsInteractive: false
    property bool textFieldToolActive: false
    property bool checkboxToolActive: false
    property bool radioButtonToolActive: false
    property bool dropdownToolActive: false
    property bool listBoxToolActive: false
    property bool combFieldToolActive: false
    property real zoom: 1.0
    property int viewRotation: 0
    property int pageDisplayMode: PDFDocumentSession.ContinuousMode
    property bool separateCoverPage: false
    property real pageGap: 28
    property bool updatesCurrentPage: true
    property int independentCurrentPage: 0
    property int activePagedLayoutIndex: 0
    property bool pendingPagedEntryAtEnd: false
    property bool paneActive: true
    property bool showActivePaneIndicator: false
    property string transientStatus
    readonly property string textSelectionNotice: {
        if (!viewTabActive || !document.loaded)
            return ""
        if (!document.allowsTextCopy) {
            //% "This document does not allow copying text."
            return qsTrId("DocumentTextCopyRestricted")
        }
        if (editController.hasDrafts) {
            //% "Save your changes to select and copy text."
            return qsTrId("SaveToSelectText")
        }
        return ""
    }
    property int pendingNavigationPage: -1
    property int navigationGeneration: 0
    property int pendingSearchNavigationGeneration: -1
    property real pendingSearchNavigationX: 0.5
    property real pendingSearchNavigationY: 0.0
    property real pendingSearchResultsRevision: -1
    property int pendingSearchNavigationAttempts: 0
    property int sessionChangeGeneration: 0
    property bool formDesignModePrepared: false
    property bool componentReady: false
    readonly property bool formDesignMode:
        formsTabActive || editTabActive
    readonly property bool facingMode:
        pageDisplayMode === PDFDocumentSession.FacingMode
        || pageDisplayMode === PDFDocumentSession.ContinuousFacingMode
    readonly property bool pagedMode:
        pageDisplayMode === PDFDocumentSession.SinglePageMode
        || pageDisplayMode === PDFDocumentSession.FacingMode
    readonly property int normalizedViewRotation:
        ((viewRotation % 360) + 360) % 360
    readonly property bool quarterTurn:
        normalizedViewRotation === 90 || normalizedViewRotation === 270
    readonly property real maximumViewPageWidth: quarterTurn
        ? document.maximumPageHeight
        : document.maximumPageWidth
    readonly property real pageLayoutContentWidth: {
        if (!document.loaded)
            return viewer.width

        const columnCount = facingMode ? 2 : 1
        const spreadGap = facingMode ? 16 : 0
        return Math.max(
            viewer.width,
            columnCount * (maximumViewPageWidth * zoom + 64) + spreadGap)
    }
    readonly property int viewCurrentPage: updatesCurrentPage
        ? document.currentPage
        : Math.max(
              0,
              Math.min(independentCurrentPage,
                       Math.max(0, document.pageCount - 1)))
    readonly property int layoutItemCount: {
        if (!document.loaded)
            return 0
        if (!facingMode)
            return document.pageCount
        if (separateCoverPage) {
            return document.pageCount > 0
                ? 1 + Math.ceil((document.pageCount - 1) / 2)
                : 0
        }
        return Math.ceil(document.pageCount / 2)
    }
    readonly property real viewportWidth: viewer.width
    readonly property real contentX: viewer.contentX
    readonly property real contentY: viewer.contentY
    readonly property bool canNavigateBackward:
        pagedMode
            ? activePagedLayoutIndex > 0
            : viewCurrentPage > 0
    readonly property bool canNavigateForward:
        pagedMode
            ? activePagedLayoutIndex + 1 < layoutItemCount
            : viewCurrentPage + 1 < document.pageCount

    signal touchZoomRequested(real value)
    signal openRequested()
    signal beginSelectedEditingRequested(string styleAction)
    signal deleteCommentRequested(int page, string objectId)
    signal navigationSettled(int generation)
    signal currentPageChangeRequested(int page)
    signal activated()
    signal viewportPositionChanged(real contentX, real contentY)

    color: "#f2efec"
    focus: paneActive
    Keys.priority: Keys.AfterItem
    Keys.onPressed: event => {
        if (!root.pagedMode || !root.document.loaded)
            return

        let handled = false
        if (event.key === Qt.Key_Down) {
            handled = root.scrollPagedBy(0, 48)
        } else if (event.key === Qt.Key_Up) {
            handled = root.scrollPagedBy(0, -48)
        } else if (event.key === Qt.Key_PageDown
                   || (event.key === Qt.Key_Space
                       && !(event.modifiers & Qt.ShiftModifier))) {
            handled = root.scrollPagedBy(0, viewer.height * 0.85)
        } else if (event.key === Qt.Key_PageUp
                   || (event.key === Qt.Key_Space
                       && (event.modifiers & Qt.ShiftModifier))) {
            handled = root.scrollPagedBy(0, -viewer.height * 0.85)
        } else if (event.key === Qt.Key_Right) {
            handled = root.scrollPagedBy(48, 0)
        } else if (event.key === Qt.Key_Left) {
            handled = root.scrollPagedBy(-48, 0)
        }
        event.accepted = handled
    }

    function syncFormDesignMode() {
        if (!formDesignMode) {
            formDesignModePrepared = false
            return
        }
        if (formDesignModePrepared)
            return

        formDesignModePrepared = true
        formDesignerController.prepareForDesignMode()
    }

    function layoutIndexForPage(page) {
        if (!facingMode)
            return page
        if (!separateCoverPage)
            return Math.floor(page / 2)
        return page <= 0 ? 0 : 1 + Math.floor((page - 1) / 2)
    }

    function firstPageForLayoutIndex(layoutIndex) {
        if (!facingMode)
            return layoutIndex
        if (!separateCoverPage)
            return layoutIndex * 2
        return layoutIndex === 0 ? 0 : layoutIndex * 2 - 1
    }

    function secondPageForLayoutIndex(layoutIndex) {
        if (!facingMode)
            return -1
        if (separateCoverPage && layoutIndex === 0)
            return -1
        const secondPage = firstPageForLayoutIndex(layoutIndex) + 1
        return secondPage < document.pageCount ? secondPage : -1
    }

    function setViewCurrentPage(page) {
        const clampedPage = Math.max(
            0,
            Math.min(page, Math.max(0, document.pageCount - 1)))
        if (updatesCurrentPage) {
            if (document.currentPage !== clampedPage)
                document.setCurrentPage(clampedPage)
        } else if (independentCurrentPage !== clampedPage) {
            currentPageChangeRequested(clampedPage)
        }
    }

    function fitWidthZoom(maximumViewPageWidth) {
        if (!document.loaded
                || maximumViewPageWidth <= 0
                || viewportWidth <= 0) {
            return zoom
        }
        const pageColumns = facingMode ? 2 : 1
        const horizontalChrome = facingMode ? 176 : 96
        return Math.max(
            0.1,
            (viewportWidth - horizontalChrome)
                / (maximumViewPageWidth * pageColumns))
    }

    function relayoutAroundCurrentPage() {
        if (!componentReady || !document.loaded)
            return
        const page = viewCurrentPage
        prepareForSessionChange()
        Qt.callLater(function() {
            if (root.document.loaded)
                root.navigateToPage(page)
        })
    }

    function navigateToPage(page) {
        if (!document.loaded || page < 0 || page >= document.pageCount)
            return -1

        cancelPendingSearchNavigation()
        const generation = ++navigationGeneration
        pendingNavigationPage = page
        setViewCurrentPage(page)
        const layoutIndex = layoutIndexForPage(page)
        if (pagedMode)
            activePagedLayoutIndex = layoutIndex

        Qt.callLater(function() {
            if (root.navigationGeneration !== generation
                    || root.pendingNavigationPage !== page) {
                return
            }

            if (!root.document.loaded || page >= root.document.pageCount) {
                root.pendingNavigationPage = -1
                root.navigationSettled(generation)
                return
            }

            const modelIndex = root.pagedMode ? 0 : layoutIndex
            viewer.positionViewAtIndex(modelIndex, ListView.Beginning)
            if (!root.pagedMode)
                viewer.contentY = Math.max(0, viewer.contentY - 16)

            Qt.callLater(function() {
                if (root.navigationGeneration !== generation
                        || root.pendingNavigationPage !== page) {
                    return
                }

                const pageLayout = viewer.itemAtIndex(modelIndex)
                    as PageLayoutDelegate
                if (pageLayout) {
                    const localCenter = pageLayout.pageCenterPoint(page)
                    const contentCenter = pageLayout.mapToItem(
                        viewer.contentItem,
                        localCenter.x,
                        localCenter.y)
                    viewer.contentX = Math.max(
                        0,
                        Math.min(
                            contentCenter.x - viewer.width * 0.5,
                            Math.max(0,
                                     viewer.contentWidth - viewer.width)))
                }
                if (root.pagedMode) {
                    viewer.contentY = root.pendingPagedEntryAtEnd
                        ? Math.max(0, viewer.contentHeight - viewer.height)
                        : 0
                    root.pendingPagedEntryAtEnd = false
                }
                root.setViewCurrentPage(page)
                root.pendingNavigationPage = -1
                root.navigationSettled(generation)
            })
        })
        return generation
    }

    function navigateByStep(direction, enterPreviousAtEnd) {
        if (!document.loaded || direction === 0)
            return -1

        if (pagedMode) {
            const targetLayout = activePagedLayoutIndex
                + (direction < 0 ? -1 : 1)
            if (targetLayout < 0 || targetLayout >= layoutItemCount)
                return -1
            pendingPagedEntryAtEnd = direction < 0
                && Boolean(enterPreviousAtEnd)
            return navigateToPage(firstPageForLayoutIndex(targetLayout))
        }

        const targetPage = viewCurrentPage + (direction < 0 ? -1 : 1)
        return navigateToPage(targetPage)
    }

    function setViewportPosition(x, y) {
        viewer.contentX = Math.max(
            0,
            Math.min(Number(x), Math.max(0, viewer.contentWidth - viewer.width)))
        viewer.contentY = Math.max(
            0,
            Math.min(Number(y), Math.max(0, viewer.contentHeight - viewer.height)))
    }

    function scrollPagedBy(deltaX, deltaY) {
        if (!pagedMode || !document.loaded)
            return false

        activated()
        // Consume additional wheel/keyboard events while the one active
        // delegate is being rebound to another page/spread. Trackpads can
        // otherwise advance through several spreads during one boundary
        // gesture before the new delegate has settled.
        if (pendingNavigationPage >= 0)
            return true

        const maximumX = Math.max(0, viewer.contentWidth - viewer.width)
        const maximumY = Math.max(0, viewer.contentHeight - viewer.height)
        const nextX = Math.max(
            0,
            Math.min(viewer.contentX + Number(deltaX), maximumX))
        const nextY = Math.max(
            0,
            Math.min(viewer.contentY + Number(deltaY), maximumY))
        const moved = Math.abs(nextX - viewer.contentX) > 0.25
            || Math.abs(nextY - viewer.contentY) > 0.25
        if (moved) {
            viewer.contentX = nextX
            viewer.contentY = nextY
            return true
        }

        if (deltaY > 0.25 && viewer.contentY >= maximumY - 0.5)
            return navigateByStep(1, false) >= 0
        if (deltaY < -0.25 && viewer.contentY <= 0.5)
            return navigateByStep(-1, true) >= 0
        return false
    }

    function cancelPendingSearchNavigation() {
        searchNavigationTimer.stop()
        pendingSearchNavigationGeneration = -1
        pendingSearchResultsRevision = -1
        pendingSearchNavigationAttempts = 0
    }

    function finishSearchNavigation() {
        const generation = pendingSearchNavigationGeneration
        const page = pendingNavigationPage
        if (navigationGeneration !== generation
                || page < 0
                || !document.loaded
                || page >= document.pageCount
                || searchController.resultsRevision
                    !== pendingSearchResultsRevision) {
            pendingNavigationPage = -1
            cancelPendingSearchNavigation()
            return
        }

        const layoutIndex = layoutIndexForPage(page)
        const modelIndex = pagedMode ? 0 : layoutIndex
        const delegate = viewer.itemAtIndex(modelIndex)
            as PageLayoutDelegate
        if (!delegate || !delegate.searchNavigationReadyForPage(page)) {
            ++pendingSearchNavigationAttempts
            if (pendingSearchNavigationAttempts >= 120) {
                pendingNavigationPage = -1
                cancelPendingSearchNavigation()
                return
            }
            viewer.positionViewAtIndex(modelIndex, ListView.Beginning)
            return
        }

        const localPoint = delegate.searchResultPoint(
            page,
            pendingSearchNavigationX,
            pendingSearchNavigationY)
        const contentPoint = delegate.mapToItem(
            viewer.contentItem,
            localPoint.x,
            localPoint.y)
        viewer.contentX = Math.max(
            0,
            Math.min(
                contentPoint.x - viewer.width * 0.5,
                Math.max(0, viewer.contentWidth - viewer.width)))
        viewer.contentY = Math.max(
            0,
            Math.min(
                contentPoint.y - viewer.height * 0.5,
                Math.max(0, viewer.contentHeight - viewer.height)))
        setViewCurrentPage(page)
        pendingNavigationPage = -1
        cancelPendingSearchNavigation()
        navigationSettled(generation)
    }

    function navigateToSearchResult(page,
                                    normalizedX,
                                    normalizedY,
                                    resultsRevision) {
        if (!document.loaded
                || page < 0
                || page >= document.pageCount
                || searchController.resultsRevision !== resultsRevision) {
            return -1
        }

        cancelPendingSearchNavigation()
        const generation = ++navigationGeneration
        pendingNavigationPage = page
        pendingSearchNavigationGeneration = generation
        pendingSearchNavigationX = normalizedX
        pendingSearchNavigationY = normalizedY
        pendingSearchResultsRevision = resultsRevision
        pendingSearchNavigationAttempts = 0
        setViewCurrentPage(page)
        const layoutIndex = layoutIndexForPage(page)
        if (pagedMode)
            activePagedLayoutIndex = layoutIndex
        viewer.positionViewAtIndex(
            pagedMode ? 0 : layoutIndex,
            ListView.Beginning)
        finishSearchNavigation()
        if (pendingSearchNavigationGeneration === generation)
            searchNavigationTimer.start()
        return generation
    }

    function prepareForSessionChange() {
        textSelection.clear()
        cancelPendingSearchNavigation()
        ++sessionChangeGeneration
        editController.finishSelectedFrameResize()
        editController.finishSelectedTextEdit()
        ++navigationGeneration
        pendingNavigationPage = -1
    }

    function beginSelectedEditing() {
        return beginSelectedEditingWithStyle("")
    }

    function beginSelectedEditingWithStyle(styleAction) {
        if (editController.selectedPage < 0)
            return false

        beginSelectedEditingRequested(String(styleAction || ""))
        return editController.textEditActive
    }

    function restorePosition(savedContentX, savedContentY) {
        prepareForSessionChange()
        const generation = navigationGeneration
        if (pagedMode)
            activePagedLayoutIndex = layoutIndexForPage(viewCurrentPage)
        Qt.callLater(function() {
            if (root.navigationGeneration !== generation)
                return
            if (!root.document.loaded) {
                root.navigationSettled(generation)
                return
            }

            root.setViewportPosition(savedContentX, savedContentY)
            root.syncCurrentPageFromScroll()
            root.navigationSettled(generation)
        })
        return generation
    }

    function syncCurrentPageFromScroll() {
        if (pagedMode
                || pendingNavigationPage >= 0
                || !document.loaded
                || viewer.count <= 0) {
            return
        }

        const candidates = ({})
        function addCandidate(index) {
            if (index < 0 || index >= viewer.count)
                return
            candidates[index] = true
        }

        addCandidate(layoutIndexForPage(viewCurrentPage))
        const sampleX = viewer.contentX + viewer.width * 0.5
        for (let sample = 0; sample <= 8; ++sample) {
            const sampleY = viewer.contentY
                + viewer.height * sample / 8
            const index = viewer.indexAt(sampleX, sampleY)
            addCandidate(index)
            addCandidate(index - 1)
            addCandidate(index + 1)
        }

        let dominantPage = -1
        let dominantArea = -1
        const indices = Object.keys(candidates)
        for (let position = 0; position < indices.length; ++position) {
            const index = Number(indices[position])
            const layoutDelegate = viewer.itemAtIndex(index)
                as PageLayoutDelegate
            if (!layoutDelegate)
                continue

            const firstPage = layoutDelegate.firstPage
            const firstArea = layoutDelegate.visibleAreaForPage(firstPage)
            if (firstArea > dominantArea
                    || (Math.abs(firstArea - dominantArea) < 0.5
                        && firstPage === viewCurrentPage)) {
                dominantPage = firstPage
                dominantArea = firstArea
            }

            const secondPage = layoutDelegate.secondPage
            if (secondPage >= 0) {
                const secondArea = layoutDelegate.visibleAreaForPage(secondPage)
                if (secondArea > dominantArea
                        || (Math.abs(secondArea - dominantArea) < 0.5
                            && secondPage === viewCurrentPage)) {
                    dominantPage = secondPage
                    dominantArea = secondArea
                }
            }
        }

        if (dominantPage >= 0)
            setViewCurrentPage(dominantPage)
    }

    onFormDesignModeChanged: syncFormDesignMode()
    onPageDisplayModeChanged: relayoutAroundCurrentPage()
    onSeparateCoverPageChanged: {
        if (facingMode)
            relayoutAroundCurrentPage()
    }
    onFormDesignerControllerChanged: {
        formDesignModePrepared = false
        syncFormDesignMode()
    }
    Component.onCompleted: {
        componentReady = true
        activePagedLayoutIndex = layoutIndexForPage(viewCurrentPage)
        syncFormDesignMode()
    }

    PDFTextSelectionController {
        id: textSelection
        objectName: "viewTextSelectionController"
    }

    onViewTabActiveChanged: textSelection.clear()
    onVisibleChanged: {
        if (!visible)
            textSelection.clear()
    }

    Connections {
        target: root.editController

        function onHistoryStateChanged() {
            textSelection.clear()
        }
    }

    Connections {
        target: root.formController
        enabled: root.updatesCurrentPage

        function onPageRollbackRequested(page) {
            root.navigateToPage(page)
        }

        function onPageNavigationRequested(page) {
            root.navigateToPage(page)
        }
    }

    Connections {
        target: root.searchController

        function onResultsChanged() {
            if (root.pendingSearchNavigationGeneration >= 0
                    && root.searchController.resultsRevision
                        !== root.pendingSearchResultsRevision) {
                root.pendingNavigationPage = -1
                root.cancelPendingSearchNavigation()
            }
        }
    }

    Timer {
        id: searchNavigationTimer

        interval: 16
        repeat: true
        onTriggered: root.finishSearchNavigation()
    }

    WheelHandler {
        id: pagedWheelHandler

        enabled: root.pagedMode && root.document.loaded
        target: null
        blocking: true
        onWheel: event => {
            let deltaX = -event.pixelDelta.x
            let deltaY = -event.pixelDelta.y
            if (Math.abs(deltaX) < 0.01 && Math.abs(deltaY) < 0.01) {
                deltaX = -event.angleDelta.x * 0.5
                deltaY = -event.angleDelta.y * 0.5
            }
            event.accepted = root.scrollPagedBy(deltaX, deltaY)
        }
    }

    HoverHandler {
        onHoveredChanged: {
            if (hovered)
                root.activated()
        }
    }

    Platform.DocumentInput {
        id: documentInput
        anchors.fill: parent
        z: 10
        theme: root.theme
        documentLoaded: root.document.loaded
        allowsTextCopy: root.document.allowsTextCopy
        hasDrafts: root.editController.hasDrafts
        saving: root.editController.saving
        textEditing: root.editController.textEditActive
        formTextFocused: root.formDesignerController.textFieldFocused
        viewTabActive: root.viewTabActive
        zoom: root.zoom
        selectionController: textSelection
        onZoomRequested: value => root.touchZoomRequested(value)
        onActivated: root.activated()
    }

    Timer {
        id: currentPageSyncTimer

        interval: 16
        repeat: false
        onTriggered: root.syncCurrentPageFromScroll()
    }

    ListView {
        id: viewer
        objectName: "documentPageList"

        anchors.fill: parent
        visible: root.document.loaded
        model: root.pagedMode
            ? root.document.loaded ? 1 : 0
            : root.layoutItemCount
        contentWidth: root.pageLayoutContentWidth
        spacing: root.pagedMode ? 0 : root.pageGap
        snapMode: ListView.NoSnap
        reuseItems: true
        cacheBuffer: Math.min(Math.max(height * 0.5, 256), 640)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.AutoFlickIfNeeded
        currentIndex: -1

        header: Item {
            width: ListView.view ? ListView.view.width : 0
            height: root.pagedMode ? 0 : 32
        }

        footer: Item {
            width: ListView.view ? ListView.view.width : 0
            height: root.pagedMode ? 0 : 32
        }

        ScrollBar.vertical: ScrollBar {
            implicitWidth: 12
            policy: ScrollBar.AlwaysOn
            palette.mid: "#847e79"
            palette.dark: "#625c58"
        }

        ScrollBar.horizontal: ScrollBar {
            objectName: "documentHorizontalScrollBar"
            implicitHeight: 12
            policy: viewer.contentWidth > viewer.width + 0.5
                ? ScrollBar.AlwaysOn
                : ScrollBar.AlwaysOff
            palette.mid: "#847e79"
            palette.dark: "#625c58"
        }

        onMovementStarted: root.activated()
        onContentYChanged: {
            root.viewportPositionChanged(contentX, contentY)
            if (!root.pagedMode)
                currentPageSyncTimer.restart()
        }
        onContentXChanged: {
            root.viewportPositionChanged(contentX, contentY)
            if (!root.pagedMode)
                currentPageSyncTimer.restart()
        }

        delegate: PageLayoutDelegate {
            id: layoutDelegate

            layoutIndex: root.pagedMode
                ? root.activePagedLayoutIndex
                : index
            theme: root.theme
            viewportItem: viewer
            document: root.document
            editController: root.editController
            searchController: root.searchController
            textSelectionController: textSelection
            viewTabActive: root.viewTabActive
            headerTabActive: root.headerTabActive
            footerMode: root.footerMode
            headerTextPlacement: root.headerTextPlacement
            headerImagePlacement: root.headerImagePlacement
            headerPageNumberPlacement: root.headerPageNumberPlacement
            textSelectionEnabled: documentInput.textSelectionEnabled
            formController: root.formController
            formDesignerController: root.formDesignerController
            formsTabActive: root.formsTabActive
            editTabActive: root.editTabActive
            formFieldsVisible: root.formFieldsVisible
            formFieldsInteractive: root.formFieldsInteractive
            textFieldToolActive: root.textFieldToolActive
            checkboxToolActive: root.checkboxToolActive
            radioButtonToolActive: root.radioButtonToolActive
            dropdownToolActive: root.dropdownToolActive
            listBoxToolActive: root.listBoxToolActive
            combFieldToolActive: root.combFieldToolActive
            zoom: root.zoom
            viewRotation: root.viewRotation
            currentPage: root.viewCurrentPage
            pageDisplayMode: root.pageDisplayMode
            separateCoverPage: root.separateCoverPage
            sessionChangeGeneration: root.sessionChangeGeneration
            onDeleteCommentRequested: (page, objectId) =>
                root.deleteCommentRequested(page, objectId)
            onTextSelectionStarted: root.activated()

            Connections {
                target: root

                function onBeginSelectedEditingRequested(styleAction) {
                    if (!layoutDelegate.pooled)
                        layoutDelegate.beginSelectedEditingWithStyle(styleAction)
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.showActivePaneIndicator && root.paneActive
        color: "transparent"
        border.width: 2
        border.color: root.theme.foxitOrange
        z: 9
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 14
        visible: !root.document.loaded

        Label {
            //% "No document open"
            text: qsTrId("NoDocumentOpen")
            font.pixelSize: 22
            color: "#33251d"
            Layout.alignment: Qt.AlignHCenter
        }

        OrangeButton {
            theme: root.theme
            //% "Open PDF"
            text: qsTrId("OpenPdf")
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
            || root.textSelectionNotice.length > 0
        z: 10

        Label {
            id: statusToastLabel

            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            text: root.transientStatus.length > 0
                ? root.transientStatus : root.textSelectionNotice
            textFormat: Text.PlainText
            color: "white"
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
}
