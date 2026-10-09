pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

Item {
    id: root

    required property int index
    property int layoutIndex: index
    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    required property PDFSearchController searchController
    property PDFTextSelectionController textSelectionController
    property bool viewTabActive: false
    property bool headerTabActive: false
    property HeaderTextPlacement headerTextPlacement: null
    property HeaderImagePlacement headerImagePlacement: null
    property HeaderPageNumberPlacement headerPageNumberPlacement: null
    property bool textSelectionEnabled: true
    required property PDFFormController formController
    required property PDFFormDesignerController formDesignerController
    required property Flickable viewportItem
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
    property int currentPage: 0
    property int pageDisplayMode: PDFDocumentSession.ContinuousMode
    property bool separateCoverPage: false
    property int sessionChangeGeneration: 0
    property bool pooled: false

    readonly property bool facingMode:
        pageDisplayMode === PDFDocumentSession.FacingMode
        || pageDisplayMode === PDFDocumentSession.ContinuousFacingMode
    readonly property bool pagedMode:
        pageDisplayMode === PDFDocumentSession.SinglePageMode
        || pageDisplayMode === PDFDocumentSession.FacingMode
    readonly property int firstPage: {
        if (!facingMode)
            return layoutIndex
        if (!separateCoverPage)
            return layoutIndex * 2
        return layoutIndex === 0 ? 0 : layoutIndex * 2 - 1
    }
    readonly property int secondPage: {
        if (!facingMode || (separateCoverPage && layoutIndex === 0))
            return -1
        const candidate = firstPage + 1
        return candidate < document.pageCount ? candidate : -1
    }
    readonly property bool hasSecondPage: secondPage >= 0
    readonly property int normalizedViewRotation:
        ((viewRotation % 360) + 360) % 360
    readonly property bool quarterTurn:
        normalizedViewRotation === 90 || normalizedViewRotation === 270
    readonly property real maximumViewPageWidth: quarterTurn
        ? document.maximumPageHeight
        : document.maximumPageWidth
    readonly property real spreadGap: facingMode ? 16 : 0
    readonly property real pageColumnWidth: Math.max(
        facingMode
            ? Math.max(0, (viewportItem.width - spreadGap) * 0.5)
            : viewportItem.width,
        maximumViewPageWidth * zoom + 64)
    readonly property real contentHeight: Math.max(
        firstPageDelegate.height,
        secondPageDelegate ? secondPageDelegate.height : 0)
    readonly property PdfPageDelegate secondPageDelegate:
        secondPageLoader.status === Loader.Ready
            ? secondPageLoader.item as PdfPageDelegate
            : null

    signal deleteCommentRequested(int page, string objectId)
    signal textSelectionStarted()

    width: facingMode
        ? Math.max(viewportItem.width,
                   pageColumnWidth * 2 + spreadGap)
        : pageColumnWidth
    height: pagedMode
        ? Math.max(viewportItem.height, contentHeight)
        : contentHeight
    clip: pagedMode

    function containsPage(page) {
        return page === firstPage || page === secondPage
    }

    function delegateForPage(page) {
        if (page === firstPage)
            return firstPageDelegate
        if (page === secondPage)
            return secondPageDelegate
        return null
    }

    function searchNavigationReadyForPage(page) {
        const pageDelegate = delegateForPage(page)
        return pageDelegate ? pageDelegate.searchNavigationReady : false
    }

    function searchResultPoint(page, normalizedX, normalizedY) {
        const pageDelegate = delegateForPage(page)
        if (!pageDelegate)
            return Qt.point(width * 0.5, height * 0.5)
        const point = pageDelegate.searchResultPoint(normalizedX, normalizedY)
        return pageDelegate.mapToItem(root, point.x, point.y)
    }

    function pageCenterPoint(page) {
        const pageDelegate = delegateForPage(page)
        if (!pageDelegate)
            return Qt.point(width * 0.5, height * 0.5)
        return pageDelegate.mapToItem(
            root,
            pageDelegate.width * 0.5,
            pageDelegate.height * 0.5)
    }

    function pageNearestToPoint(x, y) {
        if (!hasSecondPage)
            return firstPage
        const firstCenter = pageCenterPoint(firstPage)
        const secondCenter = pageCenterPoint(secondPage)
        const firstDistance = Math.abs(x - firstCenter.x)
            + Math.abs(y - firstCenter.y)
        const secondDistance = Math.abs(x - secondCenter.x)
            + Math.abs(y - secondCenter.y)
        return firstDistance <= secondDistance ? firstPage : secondPage
    }

    function visibleAreaForPage(page) {
        const pageDelegate = delegateForPage(page)
        if (!pageDelegate)
            return 0

        const topLeft = pageDelegate.mapToItem(viewportItem, 0, 0)
        const bottomRight = pageDelegate.mapToItem(
            viewportItem,
            pageDelegate.width,
            pageDelegate.height)
        const left = Math.max(0, Math.min(topLeft.x, bottomRight.x))
        const right = Math.min(
            viewportItem.width,
            Math.max(topLeft.x, bottomRight.x))
        const top = Math.max(0, Math.min(topLeft.y, bottomRight.y))
        const bottom = Math.min(
            viewportItem.height,
            Math.max(topLeft.y, bottomRight.y))
        return Math.max(0, right - left) * Math.max(0, bottom - top)
    }

    function prepareForSessionChange() {
        firstPageDelegate.prepareForSessionChange()
        if (secondPageDelegate)
            secondPageDelegate.prepareForSessionChange()
    }

    function beginSelectedEditing() {
        beginSelectedEditingWithStyle("")
    }

    function beginSelectedEditingWithStyle(styleAction) {
        const selectedPage = editController.selectedPage
        const pageDelegate = delegateForPage(selectedPage)
        if (pageDelegate)
            pageDelegate.beginSelectedEditingWithStyle(styleAction)
    }

    onSessionChangeGenerationChanged: prepareForSessionChange()

    ListView.onPooled: pooled = true
    ListView.onReused: pooled = false

    PdfPageDelegate {
        id: firstPageDelegate

        x: root.facingMode && !root.hasSecondPage
            ? (root.width - width) * 0.5
            : 0
        y: root.pagedMode ? Math.max(0, (root.height - height) * 0.5) : 0
        page: root.firstPage
        theme: root.theme
        document: root.document
        editController: root.editController
        searchController: root.searchController
        textSelectionController: root.textSelectionController
        viewTabActive: root.viewTabActive
        headerTabActive: root.headerTabActive
        headerTextPlacement: root.headerTextPlacement
        headerImagePlacement: root.headerImagePlacement
        headerPageNumberPlacement: root.headerPageNumberPlacement
        textSelectionEnabled: root.textSelectionEnabled
        formController: root.formController
        formDesignerController: root.formDesignerController
        viewportItem: root.viewportItem
        layoutOrigin: Qt.point(root.x, root.y)
        availableWidth: root.pageColumnWidth
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
        currentPage: root.currentPage
        pooled: root.pooled
        sessionChangeGeneration: root.sessionChangeGeneration
        onTextSelectionStarted: root.textSelectionStarted()
        onDeleteCommentRequested: (page, objectId) =>
            root.deleteCommentRequested(page, objectId)
    }

    Loader {
        id: secondPageLoader

        x: root.pageColumnWidth + root.spreadGap
        y: root.pagedMode && root.secondPageDelegate
            ? Math.max(0,
                       (root.height - root.secondPageDelegate.height) * 0.5)
            : 0
        active: root.hasSecondPage && !root.pooled
        sourceComponent: Component {
            PdfPageDelegate {
                page: root.secondPage
                theme: root.theme
                document: root.document
                editController: root.editController
                searchController: root.searchController
                textSelectionController: root.textSelectionController
                viewTabActive: root.viewTabActive
                headerTabActive: root.headerTabActive
                headerTextPlacement: root.headerTextPlacement
                headerImagePlacement: root.headerImagePlacement
                headerPageNumberPlacement: root.headerPageNumberPlacement
                textSelectionEnabled: root.textSelectionEnabled
                formController: root.formController
                formDesignerController: root.formDesignerController
                viewportItem: root.viewportItem
                layoutOrigin: Qt.point(
                    root.x + secondPageLoader.x,
                    root.y + secondPageLoader.y)
                availableWidth: root.pageColumnWidth
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
                currentPage: root.currentPage
                pooled: root.pooled
                sessionChangeGeneration: root.sessionChangeGeneration
                onTextSelectionStarted: root.textSelectionStarted()
                onDeleteCommentRequested: (page, objectId) =>
                    root.deleteCommentRequested(page, objectId)
            }
        }
    }
}
