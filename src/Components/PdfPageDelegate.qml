import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: pageCard

    required property int page
    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    required property PDFSearchController searchController
    property PDFTextSelectionController textSelectionController
    property bool viewTabActive: false
    property bool headerTabActive: false
    property bool footerMode: false
    property TableCellEditing tableCellState: null
    property HeaderTextPlacement headerTextPlacement: null
    property HeaderImagePlacement headerImagePlacement: null
    property HeaderPageNumberPlacement headerPageNumberPlacement: null
    property bool textSelectionEnabled: true
    required property PDFFormController formController
    required property PDFFormDesignerController formDesignerController
    required property Flickable viewportItem
    required property point layoutOrigin
    required property real availableWidth
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
    property bool pooled: false
    property int sessionChangeGeneration: 0
    readonly property real pageMargin: 32
    readonly property real viewWidth: availableWidth
    readonly property real viewContentX: viewportItem.contentX
    readonly property real viewContentY: viewportItem.contentY
    readonly property int normalizedViewRotation:
        ((viewRotation % 360) + 360) % 360
    readonly property bool quarterTurn:
        normalizedViewRotation === 90 || normalizedViewRotation === 270
    readonly property real maximumViewPageWidth: quarterTurn
        ? document.maximumPageHeight
        : document.maximumPageWidth
    readonly property bool searchNavigationReady: !pooled
        && pageSurface.width > 0
        && pageSurface.height > 0
    readonly property rect visibleRasterRect: {
        // mapFromItem needs explicit dependencies for ancestor layout/scroll changes.
        const mappingRevision = pageCard.viewContentX + pageCard.viewContentY
            + pageCard.layoutOrigin.x + pageCard.layoutOrigin.y
            + pageCard.x + pageCard.y
            + pageSurface.x + pageSurface.y
            + pageSurface.width + pageSurface.height
            + pageCard.normalizedViewRotation
        if (pageCard.pooled || !pageCard.visible || !pageCard.viewportItem.visible
                || !Number.isFinite(mappingRevision)
                || pageSurface.width <= 0 || pageSurface.height <= 0) {
            return Qt.rect(0, 0, 0, 0)
        }
        // Mapping into the shared surface reverses view rotation without scaling twice.
        const mapped = pageSurface.mapFromItem(
            pageCard.viewportItem,
            0,
            0,
            pageCard.viewportItem.width,
            pageCard.viewportItem.height)
        const left = Math.max(0, mapped.x)
        const top = Math.max(0, mapped.y)
        const right = Math.min(pageSurface.width, mapped.x + mapped.width)
        const bottom = Math.min(pageSurface.height, mapped.y + mapped.height)
        if (right <= left || bottom <= top)
            return Qt.rect(0, 0, 0, 0)
        return Qt.rect(left, top, right - left, bottom - top)
    }

    signal deleteCommentRequested(int page, string objectId)
    signal textSelectionStarted()

    width: Math.max(viewWidth, maximumViewPageWidth * zoom + pageMargin * 2)
    height: pageFrame.height + pageLabel.implicitHeight + 12

    function prepareForSessionChange() {
        pageInteractionOverlay.prepareForSessionChange()
    }

    function beginSelectedEditing() {
        beginSelectedEditingWithStyle("")
    }

    function beginSelectedEditingWithStyle(styleAction) {
        pageInteractionOverlay.beginSelectedEditingWithStyle(styleAction)
    }

    function searchResultPoint(normalizedX, normalizedY) {
        return pageSurface.mapToItem(
            pageCard,
            Math.max(0, Math.min(1, normalizedX)) * pageSurface.width,
            Math.max(0, Math.min(1, normalizedY)) * pageSurface.height)
    }

    onSessionChangeGenerationChanged: prepareForSessionChange()

    Item {
        id: pageFrame

        x: Math.max(pageCard.pageMargin, (pageCard.width - width) / 2)
        width: pageCard.quarterTurn
            ? pageView.implicitHeight
            : pageView.implicitWidth
        height: pageCard.quarterTurn
            ? pageView.implicitWidth
            : pageView.implicitHeight
    }

    Rectangle {
        x: pageFrame.x + 4
        y: pageFrame.y + 6
        width: pageFrame.width
        height: pageFrame.height
        color: "#24000000"
        radius: 2
        visible: pageFrame.width > 0 && pageFrame.height > 0 && !pageCard.pooled
    }

    Item {
        id: pageSurface

        anchors.centerIn: pageFrame
        width: pageView.implicitWidth
        height: pageView.implicitHeight
        rotation: pageCard.normalizedViewRotation
        visible: !pageCard.pooled

        PDFPageView {
            id: pageView

            document: pageCard.pooled ? null : pageCard.document
            editController: pageCard.pooled ? null : pageCard.editController
            formController: pageCard.pooled ? null : pageCard.formController
            formDesignerController: pageCard.pooled
                ? null
                : pageCard.formDesignerController
            formDesignMode: pageInteractionOverlay.formDesignMode
            page: pageCard.page
            zoom: pageCard.zoom
            visiblePageRect: pageCard.visibleRasterRect
            pageColor: "white"
            width: implicitWidth
            height: implicitHeight
        }

        Rectangle {
            anchors.fill: pageView
            color: "transparent"
            border.color: pageCard.page === pageCard.currentPage
                ? pageCard.theme.foxitOrange
                : "#c6cad1"
            border.width: pageCard.page === pageCard.currentPage ? 2 : 1
            visible: pageView.width > 0 && pageView.height > 0
        }

        SearchHighlightLayer {
            anchors.fill: pageView
            controller: pageCard.searchController
            page: pageCard.page
            pooled: pageCard.pooled
            z: 0.5
        }

        Rectangle {
            objectName: "headerAreaOverlay"
            y: pageCard.footerMode ? pageSurface.height - height : 0
            anchors.left: parent.left
            anchors.right: parent.right
            height: Math.min(pageSurface.height, 100 * pageCard.zoom)
            color: pageCard.theme.headerAreaColor
            visible: pageCard.headerTabActive && pageCard.document.loaded
                && !pageCard.pooled && width > 0 && height > 0
            enabled: false
            Accessible.ignored: true
            z: 0.75
        }

        PageInteractionOverlay {
            id: pageInteractionOverlay

            anchors.fill: pageView
            theme: pageCard.theme
            document: pageCard.document
            textSelectionController: pageCard.textSelectionController
            textSelectionActive: pageCard.viewTabActive && pageCard.textSelectionEnabled
                && pageCard.visibleRasterRect.width > 0
                && pageCard.visibleRasterRect.height > 0
            onTextSelectionStarted: pageCard.textSelectionStarted()
            editController: pageCard.editController
            tableCellState: pageCard.tableCellState
            formController: pageCard.formController
            formDesignerController: pageCard.formDesignerController
            page: pageCard.page
            pagePointScale: pageCard.zoom
            pooled: pageCard.pooled
            formsTabActive: pageCard.formsTabActive
            editTabActive: pageCard.editTabActive
            formFieldsVisible: pageCard.formFieldsVisible
            formFieldsInteractive: pageCard.formFieldsInteractive
            textFieldToolActive: pageCard.textFieldToolActive
            checkboxToolActive: pageCard.checkboxToolActive
            radioButtonToolActive: pageCard.radioButtonToolActive
            dropdownToolActive: pageCard.dropdownToolActive
            listBoxToolActive: pageCard.listBoxToolActive
            combFieldToolActive: pageCard.combFieldToolActive
            draftTextureCurrent: pageView.draftTextureCurrent
            draftTextureReady: pageView.draftTextureReady
            displayedSuppressedCommentObjectIds:
                pageView.displayedSuppressedCommentObjectIds
            viewportItem: pageCard.viewportItem
            viewportContentX: pageCard.viewContentX
            viewportContentY: pageCard.viewContentY
            viewportTransformState: [
                pageCard.normalizedViewRotation,
                pageCard.zoom,
                pageCard.width,
                pageCard.height,
                pageFrame.x,
                pageFrame.y,
                pageSurface.x,
                pageSurface.y,
                pageSurface.width,
                pageSurface.height
            ]
            z: 1
            onDeleteCommentRequested: (page, objectId) =>
                pageCard.deleteCommentRequested(page, objectId)
        }

        HeaderTextOverlay {
            y: pageCard.footerMode ? pageSurface.height - height : 0
            anchors.left: parent.left
            anchors.right: parent.right
            height: Math.min(pageSurface.height, 100 * pageCard.zoom)
            theme: pageCard.theme
            editController: pageCard.editController
            placement: pageCard.headerTextPlacement
            page: pageCard.page
            zoom: pageCard.zoom
            visible: pageCard.headerTabActive && pageCard.document.loaded
                && !pageCard.pooled && placement !== null
            enabled: visible && !pageCard.editController.saving
            z: 2
        }

        HeaderImageOverlay {
            y: pageCard.footerMode ? pageSurface.height - height : 0
            anchors.left: parent.left
            anchors.right: parent.right
            height: Math.min(pageSurface.height, 100 * pageCard.zoom)
            theme: pageCard.theme
            editController: pageCard.editController
            placement: pageCard.headerImagePlacement
            page: pageCard.page
            zoom: pageCard.zoom
            visible: pageCard.headerTabActive && pageCard.document.loaded
                && !pageCard.pooled && placement !== null
            enabled: visible && !pageCard.editController.saving
            z: 3
        }

        HeaderPageNumberOverlay {
            y: pageCard.footerMode ? pageSurface.height - height : 0
            anchors.left: parent.left
            anchors.right: parent.right
            height: Math.min(pageSurface.height, 100 * pageCard.zoom)
            theme: pageCard.theme
            editController: pageCard.editController
            placement: pageCard.headerPageNumberPlacement
            page: pageCard.page
            zoom: pageCard.zoom
            visible: pageCard.headerTabActive && pageCard.document.loaded
                && !pageCard.pooled && placement !== null
            enabled: visible && !pageCard.editController.saving
            z: 4
        }
    }

    Label {
        id: pageLabel

        //% "Page %1"
        text: qsTrId("PageNumber").arg(pageCard.page + 1)
        color: pageCard.page === pageCard.currentPage
            ? pageCard.theme.foxitDarkOrange
            : "#6b625d"
        font.pixelSize: 12
        x: Math.max(pageCard.pageMargin, (pageCard.width - width) / 2)
        y: pageFrame.height + 8
        visible: !pageCard.pooled
    }
}
