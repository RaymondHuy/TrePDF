pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import TrePDF

Item {
    id: root
    objectName: "documentWorkspace"

    required property AppTheme theme
    required property PDFDocumentSession session
    property bool viewTabActive: false
    property bool headerTabActive: false
    readonly property bool headerSelectionActive: headerTabActive
        && session.editController.headerSelectionOnly
        && session.editController.tool === PDFEditController.SelectTool
    readonly property HeaderTextPlacement headerTextPlacement: headerPlacement
    readonly property HeaderImagePlacement headerImagePlacement: headerImagePlacementState
    readonly property HeaderPageNumberPlacement headerPageNumberPlacement: headerPageNumberPlacementState
    readonly property bool hasPendingHeaderPlacement:
        (headerPlacement.active && headerPlacement.text.length > 0)
        || headerImagePlacementState.active || headerPageNumberPlacementState.active
    readonly property bool headerPageNumberInputFocused: {
        const panel = headerPageNumberPanelLoader.item as HeaderPageNumberPanel
        return panel ? panel.inputFocused : false
    }
    readonly property bool headerImageInputFocused: {
        const panel = headerImagePanelLoader.item as HeaderImagePanel
        return panel ? panel.inputFocused : false
    }
    readonly property bool headerSyncInputFocused: {
        if (headerSyncPanelLoader.status !== Loader.Ready)
            return false
        const panel = headerSyncPanelLoader.item as HeaderSyncPanel
        return panel ? panel.inputFocused : false
    }
    readonly property bool headerSyncPanelActive: headerSelectionActive
        && session.document.loaded && session.editController.headerSyncAvailable
    readonly property string headerSyncAnchorKey: session.editController.headerSyncKey
    property bool headerSyncAllMatchingPages: true
    property string headerSyncPageRange: ""
    onHeaderSyncAnchorKeyChanged: {
        headerSyncAllMatchingPages = true
        headerSyncPageRange = ""
        if (root.headerSyncAnchorKey.length > 0)
            Qt.callLater(root.startHeaderSync, root.headerSyncAnchorKey)
    }
    function startHeaderSync(anchorKey: string): void {
        // Starting a scan emits the same notifier used by headerSyncAnchorKey.
        const controller = root.session.editController
        if (root.headerSyncAnchorKey === anchorKey && controller.headerSyncAvailable)
            controller.findSimilarHeaders()
    }
    readonly property bool headerTextPanelOpen: headerPlacement.active
    readonly property bool headerTextInputFocused: {
        if (headerPlacement.inputFocused)
            return true
        if (headerTextPanelLoader.status !== Loader.Ready)
            return false
        const panel = headerTextPanelLoader.item as HeaderTextPanel
        return panel ? panel.inputFocused : false
    }
    property bool formsTabActive: false
    property bool editTabActive: false
    property bool signTabActive: false
    property bool commentTabActive: false
    property bool commentsPanelVisible: false
    property bool formFieldsVisible: false
    property bool formFieldsInteractive: false
    property bool textFieldToolActive: false
    property bool checkboxToolActive: false
    property bool radioButtonToolActive: false
    property bool dropdownToolActive: false
    property bool listBoxToolActive: false
    property bool combFieldToolActive: false
    property bool commentPanelEditPending: false
    property string transientStatus
    property bool initialViewReady: false
    property int initialNavigationGeneration: -1
    property int pendingSearchPage: -1
    property real pendingSearchX: 0.5
    property real pendingSearchY: 0.0
    property real pendingSearchResultsRevision: -1
    property int activePaneIndex: 0
    readonly property string selectedFormFieldId:
        session.formDesignerController.selectedFieldId
    readonly property var selectedFormField: selectedFormFieldId.length > 0
        ? session.formDesignerController.fieldData(selectedFormFieldId)
        : ({})
    readonly property bool selectedFormFieldIsComb:
        Number(selectedFormField.type) === PDFFormDesignerController.Text
        && (Number(selectedFormField.flags)
            & PDFFormDesignerController.Comb) !== 0
    readonly property bool textFieldOptionsPanelActive: formsTabActive
        && selectedFormFieldId.length > 0
        && Number(selectedFormField.type)
            === PDFFormDesignerController.Text
        && !selectedFormFieldIsComb
    readonly property bool combFieldOptionsPanelActive: formsTabActive
        && selectedFormFieldId.length > 0
        && selectedFormFieldIsComb
    readonly property bool dropdownOptionsPanelActive: formsTabActive
        && selectedFormFieldId.length > 0
        && Number(selectedFormField.type)
            === PDFFormDesignerController.Dropdown
    readonly property bool listBoxOptionsPanelActive: formsTabActive
        && selectedFormFieldId.length > 0
        && Number(selectedFormField.type)
            === PDFFormDesignerController.ListBox
    readonly property bool radioButtonOptionsPanelActive: formsTabActive
        && selectedFormFieldId.length > 0
        && Number(selectedFormField.type)
            === PDFFormDesignerController.RadioButton
    readonly property bool drawingCommentSelected: {
        const kind = session.editController.selectedKind
        return kind === "square"
            || kind === "circle"
            || kind === "line"
            || kind === "polygon"
            || kind === "polyLine"
            || kind === "ink"
    }
    readonly property bool drawingCommentPropertiesPanelActive:
        (editTabActive || headerSelectionActive) && (drawingCommentSelected
            || session.editController.selectedKind === "addedShape"
            || session.editController.selectedKind === "sourceShape")
    readonly property bool imagePropertiesPanelActive:
        (editTabActive || signTabActive || headerSelectionActive)
            && session.editController.selectedObjectId.length > 0
            && (session.editController.selectedKind === "image"
                || session.editController.selectedKind === "addedImage")
    readonly property DocumentView splitDocumentView:
        splitDocumentViewLoader.status === Loader.Ready
            ? splitDocumentViewLoader.item as DocumentView
            : null
    readonly property DocumentView activeDocumentView:
        paneView(activePaneIndex) || documentView
    readonly property int activePage: activeDocumentView
        ? activeDocumentView.viewCurrentPage
        : session.document.currentPage
    readonly property real activeZoom: activeDocumentView
        ? activeDocumentView.zoom
        : session.zoom
    readonly property bool canNavigateBackward: activeDocumentView
        ? activeDocumentView.canNavigateBackward
        : false
    readonly property bool canNavigateForward: activeDocumentView
        ? activeDocumentView.canNavigateForward
        : false
    readonly property bool hasPendingPanelEdits: {
        if (textFormatPanelLoader.status === Loader.Ready) {
            const textFormatPanel = textFormatPanelLoader.item as TextFormatPanel
            if (textFormatPanel && textFormatPanel.fontSizeEditPending)
                return true
        }

        if (commentPanelLoader.status === Loader.Ready) {
            const commentPanel = commentPanelLoader.item as CommentPanel
            if (commentPanel && commentPanel.textEditPending)
                return true
        }

        if (textFieldOptionsPanelLoader.status === Loader.Ready) {
            const textPanel = textFieldOptionsPanelLoader.item
                as TextFieldOptionsPanel
            if (textPanel && textPanel.maxLengthEditPending)
                return true
        }

        if (combFieldOptionsPanelLoader.status === Loader.Ready) {
            const combPanel = combFieldOptionsPanelLoader.item
                as CombFieldOptionsPanel
            if (combPanel && combPanel.maxLengthEditPending)
                return true
        }

        if (radioButtonOptionsPanelLoader.status === Loader.Ready) {
            const radioPanel = radioButtonOptionsPanelLoader.item
                as RadioButtonOptionsPanel
            if (radioPanel && radioPanel.groupNameEditPending)
                return true
        }
        return false
    }

    signal openRequested()
    signal headerImageFileRequested(HeaderImagePlacement placement)
    signal deleteCommentRequested(int page, string objectId)

    function openHeaderTextPanel() {
        if (!headerTabActive || !session.document.loaded || session.saving)
            return
        headerImagePlacementState.cancel()
        headerPageNumberPlacementState.cancel()
        headerPlacement.start()
    }

    function closeHeaderTextPanel() {
        headerPlacement.cancel()
    }

    function openHeaderImagePanel() {
        if (!headerTabActive || !session.document.loaded || session.saving)
            return
        headerPlacement.cancel()
        headerPageNumberPlacementState.cancel()
        headerImagePlacementState.start()
    }

    function openHeaderPageNumberPanel() {
        if (!headerTabActive || !session.document.loaded || session.saving)
            return
        headerPlacement.cancel()
        headerImagePlacementState.cancel()
        headerPageNumberPlacementState.start()
    }

    function closeHeaderPlacement() {
        headerPlacement.cancel()
        headerImagePlacementState.cancel()
        headerPageNumberPlacementState.cancel()
    }

    function submitPendingHeaderPlacement() {
        InputMethod.commit()
        headerPlacement.synchronizeRequested()
        if (headerPlacement.active && headerPlacement.text.length > 0) {
            const panel = headerTextPanelLoader.status === Loader.Ready
                ? headerTextPanelLoader.item as HeaderTextPanel : null
            if (panel)
                panel.submit()
        } else if (headerImagePlacementState.active) {
            const panel = headerImagePanelLoader.status === Loader.Ready
                ? headerImagePanelLoader.item as HeaderImagePanel : null
            if (panel)
                panel.submit()
        } else if (headerPageNumberPlacementState.active) {
            headerPageNumberPlacementState.submit()
        }
        return !hasPendingHeaderPlacement
    }

    onHeaderTabActiveChanged: {
        if (!headerTabActive)
            closeHeaderPlacement()
    }
    onSessionChanged: closeHeaderPlacement()

    HeaderTextPlacement {
        id: headerPlacement
        document: root.session.document
        editController: root.session.editController
    }

    HeaderImagePlacement {
        id: headerImagePlacementState
        document: root.session.document
        editController: root.session.editController
        onFileRequested: root.headerImageFileRequested(headerImagePlacementState)
    }

    HeaderPageNumberPlacement {
        id: headerPageNumberPlacementState
        document: root.session.document
        editController: root.session.editController
    }

    component WorkspaceDocumentView: DocumentView {
        required property int paneIndex
        readonly property bool secondaryPane: paneIndex !== 0
        readonly property var paneState: root.viewStateForPane(paneIndex)

        objectName: paneIndex === 0
            ? "primaryDocumentView"
            : "secondaryDocumentView"
        theme: root.theme
        document: root.session.document
        editController: root.session.editController
        searchController: root.session.searchController
        formController: root.session.formController
        formDesignerController: root.session.formDesignerController
        viewTabActive: root.viewTabActive
        headerTabActive: root.headerTabActive
        headerTextPlacement: secondaryPane ? null : root.headerTextPlacement
        headerImagePlacement: secondaryPane ? null : root.headerImagePlacement
        headerPageNumberPlacement: secondaryPane ? null : root.headerPageNumberPlacement
        formsTabActive: !secondaryPane && root.formsTabActive
        editTabActive: !secondaryPane && root.editTabActive
        formFieldsVisible: root.formFieldsVisible
        formFieldsInteractive: !secondaryPane
            && root.formFieldsInteractive
        textFieldToolActive: !secondaryPane && root.textFieldToolActive
        checkboxToolActive: !secondaryPane && root.checkboxToolActive
        radioButtonToolActive: !secondaryPane
            && root.radioButtonToolActive
        dropdownToolActive: !secondaryPane && root.dropdownToolActive
        listBoxToolActive: !secondaryPane && root.listBoxToolActive
        combFieldToolActive: !secondaryPane && root.combFieldToolActive
        zoom: paneIndex === 0
            ? root.session.zoom
            : paneState ? paneState.zoom : root.session.zoom
        viewRotation: root.session.viewRotation
        pageDisplayMode: root.session.pageDisplayMode
        separateCoverPage: root.session.separateCoverPage
        updatesCurrentPage: !secondaryPane
        independentCurrentPage: paneState ? paneState.currentPage : 0
        paneActive: root.activePaneIndex === paneIndex
        showActivePaneIndicator: root.session.splitViewEnabled
        transientStatus: secondaryPane ? "" : root.transientStatus
        onOpenRequested: root.openRequested()
        onDeleteCommentRequested: (page, objectId) =>
            root.deleteCommentRequested(page, objectId)
        onCurrentPageChangeRequested: page => {
            if (paneState)
                paneState.currentPage = page
        }
        onActivated: root.activePaneIndex = paneIndex
        onTouchZoomRequested: value => {
            root.activePaneIndex = paneIndex
            root.setZoom(value)
        }
    }

    function expandCommentsPanelForSelection() {
        if (commentTabActive
                && commentsPanelVisible
                && (session.editController.tool
                        === PDFEditController.FreeTextTool
                    || drawingCommentSelected)) {
            session.panelCollapsed = false
        }
    }

    onCommentTabActiveChanged: {
        if (!commentTabActive)
            return

        session.formDesignerController.clearTextFieldFocus()
        session.formDesignerController.clearTextFieldSelection()
        expandCommentsPanelForSelection()
    }

    onCommentsPanelVisibleChanged: expandCommentsPanelForSelection()

    Connections {
        target: root.session.editController

        function onToolChanged() {
            if (root.commentTabActive
                    && root.session.editController.tool
                        === PDFEditController.FreeTextTool) {
                root.session.panelCollapsed = false
            }
        }

        function onSelectionChanged() {
            root.expandCommentsPanelForSelection()
        }
    }

    onTextFieldOptionsPanelActiveChanged: {
        if (textFieldOptionsPanelActive)
            session.panelCollapsed = false
    }

    onCombFieldOptionsPanelActiveChanged: {
        if (combFieldOptionsPanelActive)
            session.panelCollapsed = false
    }

    onDrawingCommentPropertiesPanelActiveChanged: {
        if (drawingCommentPropertiesPanelActive)
            session.panelCollapsed = false
    }

    onImagePropertiesPanelActiveChanged: {
        if (imagePropertiesPanelActive)
            session.panelCollapsed = false
    }

    function viewStateForPane(paneIndex) {
        if (paneIndex === 1)
            return session.secondaryViewState
        return null
    }

    function paneView(paneIndex) {
        if (paneIndex === 0)
            return documentView
        if (paneIndex === 1)
            return splitDocumentView
        return null
    }

    function setZoom(value) {
        const clampedZoom = Math.max(0.1, Math.min(5.0, value))
        if (activePaneIndex === 0) {
            if (Math.abs(session.zoom - clampedZoom) < 0.000001)
                return
            if (!commitPendingPanelEdits())
                return
            const view = paneView(0)
            if (view)
                view.prepareForSessionChange()
            session.zoom = clampedZoom
            return
        }

        const paneState = viewStateForPane(activePaneIndex)
        if (paneState)
            paneState.zoom = clampedZoom
    }

    function zoomBy(factor) {
        setZoom(activeZoom * factor)
    }

    function setPageDisplayMode(mode) {
        if (session.pageDisplayMode === mode)
            return
        preparePaneViews()
        session.pageDisplayMode = mode
    }

    function toggleSeparateCoverPage() {
        const facingMode = session.pageDisplayMode
                === PDFDocumentSession.FacingMode
            || session.pageDisplayMode
                === PDFDocumentSession.ContinuousFacingMode
        if (!facingMode)
            return
        preparePaneViews()
        session.separateCoverPage = !session.separateCoverPage
    }

    function preparePaneViews() {
        for (let paneIndex = 0; paneIndex < 2; ++paneIndex) {
            const view = paneView(paneIndex)
            if (view)
                view.prepareForSessionChange()
        }
    }

    function storePanePosition(paneIndex) {
        if (paneIndex === 0)
            return
        const view = paneView(paneIndex)
        const paneState = viewStateForPane(paneIndex)
        if (view && paneState)
            paneState.setScrollPosition(view.contentX, view.contentY)
    }

    function storeSplitPositions() {
        storePanePosition(1)
    }

    function initializePaneState(paneIndex) {
        const paneState = viewStateForPane(paneIndex)
        if (!paneState || paneState.positionInitialized)
            return
        paneState.zoom = session.zoom
        paneState.currentPage = documentView.viewCurrentPage
        paneState.setScrollPosition(
            documentView.contentX,
            documentView.contentY)
    }

    function setSplitViewMode(mode) {
        if (mode < PDFDocumentSession.NoSplit
                || mode > PDFDocumentSession.HorizontalSplit
                || session.splitViewMode === mode) {
            return
        }

        storeSplitPositions()
        preparePaneViews()
        if (mode !== PDFDocumentSession.NoSplit)
            initializePaneState(1)
        session.splitViewMode = mode
        if (mode === PDFDocumentSession.NoSplit) {
            activePaneIndex = 0
        }
    }

    function toggleSplitView() {
        setSplitViewMode(session.splitViewEnabled
            ? PDFDocumentSession.NoSplit
            : PDFDocumentSession.VerticalSplit)
    }

    function restorePane(paneIndex, view) {
        const paneState = viewStateForPane(paneIndex)
        if (!view || !paneState)
            return
        if (paneState.positionInitialized) {
            view.restorePosition(paneState.scrollX, paneState.scrollY)
        } else {
            view.navigateToPage(documentView.viewCurrentPage)
        }
    }

    function fitToWidth() {
        const maximumViewPageWidth = session.viewRotation % 180 === 0
            ? session.document.maximumPageWidth
            : session.document.maximumPageHeight
        if (!session.document.loaded
                || maximumViewPageWidth <= 0
                || !activeDocumentView
                || activeDocumentView.viewportWidth <= 0) {
            return
        }

        setZoom(activeDocumentView.fitWidthZoom(maximumViewPageWidth))
    }

    function navigateToPage(page) {
        documentView.navigateToPage(page)
    }

    function navigateActivePaneToPage(page) {
        if (!activeDocumentView)
            return -1
        return activeDocumentView.navigateToPage(page)
    }

    function navigateByStep(direction) {
        if (activeDocumentView)
            activeDocumentView.navigateByStep(direction, false)
    }

    function clearPendingSearchNavigation() {
        pendingSearchPage = -1
        pendingSearchResultsRevision = -1
    }

    function dispatchPendingSearchNavigation() {
        if (!initialViewReady || pendingSearchPage < 0)
            return

        const page = pendingSearchPage
        const normalizedX = pendingSearchX
        const normalizedY = pendingSearchY
        const resultsRevision = pendingSearchResultsRevision
        clearPendingSearchNavigation()
        if (!viewTabActive
                || session.searchController.resultsRevision
                    !== resultsRevision) {
            return
        }
        documentView.navigateToSearchResult(
            page,
            normalizedX,
            normalizedY,
            resultsRevision)
    }

    function navigateToSearchResult(page,
                                    normalizedX,
                                    normalizedY,
                                    resultsRevision) {
        pendingSearchPage = page
        pendingSearchX = normalizedX
        pendingSearchY = normalizedY
        pendingSearchResultsRevision = resultsRevision
        dispatchPendingSearchNavigation()
    }

    function finishInitialView(generation) {
        if (initialViewReady
                || initialNavigationGeneration < 0
                || generation < initialNavigationGeneration) {
            return
        }
        initialViewReady = true
        dispatchPendingSearchNavigation()
    }

    function rotateView(quarterTurns) {
        if (!session.document.loaded)
            return

        const currentPage = session.document.currentPage
        session.rotateView(quarterTurns)
        Qt.callLater(function() {
            if (root.session.document.loaded)
                documentView.navigateToPage(currentPage)
        })
    }

    function applyInlineStyleChange(styleAction, enabled) {
        if (!session.editController.textEditActive) {
            documentView.beginSelectedEditingWithStyle(styleAction)
            return
        }
        if (styleAction === "bold")
            session.editController.setSelectedFontBold(enabled)
        else if (styleAction === "italic")
            session.editController.setSelectedFontItalic(enabled)
        else if (styleAction === "underline")
            session.editController.setSelectedFontUnderline(enabled)
    }

    function initializeView() {
        if (session.initialFitPending) {
            fitToWidth()
            session.initialFitPending = false
            initialNavigationGeneration = documentView.navigationGeneration + 1
            Qt.callLater(function() {
                if (root.initialViewReady)
                    return
                if (!root.session.document.loaded) {
                    root.initialViewReady = true
                    root.dispatchPendingSearchNavigation()
                    return
                }
                const generation = documentView.navigateToPage(
                    root.session.document.currentPage)
                root.initialNavigationGeneration = generation
                if (generation < 0) {
                    root.initialViewReady = true
                    root.dispatchPendingSearchNavigation()
                }
            })
        } else {
            initialNavigationGeneration = documentView.restorePosition(
                session.scrollX,
                session.scrollY)
        }
    }

    function commitPendingPanelEdits() {
        if (textFormatPanelLoader.status === Loader.Ready) {
            const textFormatPanel = textFormatPanelLoader.item as TextFormatPanel
            if (textFormatPanel && !textFormatPanel.commitPendingEdit())
                return false
        }

        if (commentPanelLoader.status === Loader.Ready) {
            const commentPanel = commentPanelLoader.item as CommentPanel
            if (commentPanel && !commentPanel.commitPendingEdit())
                return false
        }

        if (textFieldOptionsPanelLoader.status === Loader.Ready) {
            const textPanel = textFieldOptionsPanelLoader.item
                as TextFieldOptionsPanel
            if (textPanel && !textPanel.commitPendingEdit())
                return false
        }

        if (combFieldOptionsPanelLoader.status === Loader.Ready) {
            const combPanel = combFieldOptionsPanelLoader.item
                as CombFieldOptionsPanel
            if (combPanel && !combPanel.commitPendingEdit())
                return false
        }

        if (radioButtonOptionsPanelLoader.status === Loader.Ready) {
            const radioPanel = radioButtonOptionsPanelLoader.item
                as RadioButtonOptionsPanel
            if (radioPanel && !radioPanel.commitPendingEdit())
                return false
        }
        return true
    }

    function updateCommentPanelEditPending(pending) {
        if (pending) {
            commentPanelEditPending = true
            return
        }

        Qt.callLater(function() {
            if (commentPanelLoader.status !== Loader.Ready) {
                root.commentPanelEditPending = false
                return
            }
            const commentPanel = commentPanelLoader.item as CommentPanel
            root.commentPanelEditPending = commentPanel
                ? commentPanel.textEditPending
                : false
        })
    }

    function prepareForDeactivation() {
        if (!commitPendingPanelEdits())
            return false

        preparePaneViews()
        session.scrollX = documentView.contentX
        session.scrollY = documentView.contentY
        storeSplitPositions()
        session.prepareForDeactivation()
        return true
    }

    Component.onDestruction: prepareForDeactivation()

    onViewTabActiveChanged: {
        if (!viewTabActive)
            clearPendingSearchNavigation()
    }

    onEditTabActiveChanged: {
        if (!editTabActive)
            session.editController.cancelTextEditPreparation()
    }

    Timer {
        interval: 0
        running: true
        repeat: false
        onTriggered: root.initializeView()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        PageThumbnailPanel {
            Layout.preferredWidth: root.session.thumbnailsVisible ? 192 : 36
            Layout.fillHeight: true
            theme: root.theme
            session: root.session
            currentPage: root.activePage
            navigationEnabled: root.initialViewReady && !root.session.saving
            onPageRequested: page => {
                if (!root.commitPendingPanelEdits())
                    return
                root.preparePaneViews()
                root.session.prepareForDeactivation()
                root.navigateActivePaneToPage(page)
            }
        }

        Item {
            id: documentArea

            Layout.fillWidth: true
            Layout.fillHeight: true

            Rectangle {
                anchors.fill: parent
                color: root.theme.ribbonBorder

                GridLayout {
                    anchors.fill: parent
                    columns: root.session.splitViewMode
                            === PDFDocumentSession.VerticalSplit
                        ? 2 : 1
                    columnSpacing: columns > 1 ? 3 : 0
                    rowSpacing: root.session.splitViewMode
                            === PDFDocumentSession.HorizontalSplit
                        ? 3 : 0
                    uniformCellWidths: true
                    uniformCellHeights: true

                    WorkspaceDocumentView {
                        id: documentView

                        paneIndex: 0
                        Layout.row: 0
                        Layout.column: 0
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 1
                        onNavigationSettled: generation =>
                            root.finishInitialView(generation)
                    }

                    Loader {
                        id: splitDocumentViewLoader
                        objectName: "secondaryDocumentViewLoader"

                        Layout.row: root.session.splitViewMode
                                === PDFDocumentSession.HorizontalSplit
                            ? 1 : 0
                        Layout.column: root.session.splitViewMode
                                === PDFDocumentSession.HorizontalSplit
                            ? 0 : 1
                        Layout.fillWidth: active
                        Layout.fillHeight: active
                        Layout.preferredWidth: active ? 1 : 0
                        Layout.preferredHeight: active ? 1 : 0
                        active: root.session.document.loaded
                            && root.session.splitViewEnabled
                        visible: active
                        onLoaded: root.restorePane(1, item as DocumentView)

                        sourceComponent: Component {
                            WorkspaceDocumentView {
                                paneIndex: 1
                            }
                        }
                    }
                }
            }
        }

        Loader {
            id: headerTextPanelLoader
            objectName: "headerTextPanelLoader"

            Layout.preferredWidth: active ? 280 : 0
            Layout.fillHeight: true
            active: root.headerTextPanelOpen && root.headerTabActive
                && root.session.document.loaded && !root.session.saving
            visible: active
            sourceComponent: Component {
                HeaderTextPanel {
                    theme: root.theme
                    placement: root.headerTextPlacement
                }
            }
        }

        Loader {
            id: headerImagePanelLoader
            objectName: "headerImagePanelLoader"
            Layout.preferredWidth: active ? 280 : 0
            Layout.fillHeight: true
            active: root.headerImagePlacement.active && root.headerTabActive
                && root.session.document.loaded && !root.session.saving
            visible: active
            sourceComponent: Component {
                HeaderImagePanel {
                    theme: root.theme
                    placement: root.headerImagePlacement
                }
            }
        }

        Loader {
            id: headerPageNumberPanelLoader
            objectName: "headerPageNumberPanelLoader"
            Layout.preferredWidth: active ? 280 : 0
            Layout.fillHeight: true
            active: root.headerPageNumberPlacement.active && root.headerTabActive
                && root.session.document.loaded && !root.session.saving
            visible: active
            sourceComponent: Component {
                HeaderPageNumberPanel {
                    theme: root.theme
                    placement: root.headerPageNumberPlacement
                }
            }
        }

        Loader {
            id: commentPanelLoader

            Layout.preferredWidth: visible
                ? root.session.panelCollapsed ? 42 : 280
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && ((root.commentTabActive
                     && root.commentsPanelVisible)
                    || root.commentPanelEditPending)
            visible: root.session.document.loaded
                && root.commentTabActive
                && root.commentsPanelVisible
            onStatusChanged: {
                if (status === Loader.Null)
                    root.commentPanelEditPending = false
            }

            sourceComponent: Component {
                CommentPanel {
                    theme: root.theme
                    document: root.session.document
                    editController: root.session.editController
                    collapsed: root.session.panelCollapsed
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                    onTextEditPendingChanged:
                        root.updateCommentPanelEditPending(textEditPending)
                    onPageRequested: page => root.navigateToPage(page)
                    onDeleteCommentRequested: (page, objectId) =>
                        root.deleteCommentRequested(page, objectId)
                }
            }
        }

        Loader {
            id: textFieldOptionsPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && root.textFieldOptionsPanelActive
            visible: active

            sourceComponent: Component {
                TextFieldOptionsPanel {
                    theme: root.theme
                    controller: root.session.formDesignerController
                    fieldId: root.selectedFormFieldId
                    collapsed: root.session.panelCollapsed
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }

        Loader {
            id: dropdownOptionsPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && (root.dropdownOptionsPanelActive
                    || root.listBoxOptionsPanelActive)
            visible: active

            sourceComponent: Component {
                DropdownOptionsPanel {
                    theme: root.theme
                    controller: root.session.formDesignerController
                    fieldId: root.selectedFormFieldId
                    listBoxMode: root.listBoxOptionsPanelActive
                    collapsed: root.session.panelCollapsed
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }

        Loader {
            id: combFieldOptionsPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && root.combFieldOptionsPanelActive
            visible: active

            sourceComponent: Component {
                CombFieldOptionsPanel {
                    theme: root.theme
                    controller: root.session.formDesignerController
                    fieldId: root.selectedFormFieldId
                    collapsed: root.session.panelCollapsed
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }

        Loader {
            id: drawingCommentPropertiesPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && root.drawingCommentPropertiesPanelActive
            visible: active

            sourceComponent: Component {
                DrawingCommentPropertiesPanel {
                    theme: root.theme
                    document: root.session.document
                    editController: root.session.editController
                    collapsed: root.session.panelCollapsed
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }

        ColumnLayout {
            id: selectionProperties
            objectName: "selectionPropertiesColumn"
            readonly property bool syncOnly: root.headerSyncPanelActive
                && !imagePropertiesPanelLoader.active && !textFormatPanelLoader.active

            Layout.preferredWidth: visible
                ? root.session.panelCollapsed && !syncOnly ? 42 : 264 : 0
            Layout.minimumWidth: Layout.preferredWidth
            Layout.maximumWidth: Layout.preferredWidth
            Layout.fillWidth: false
            Layout.fillHeight: true
            spacing: 0
            visible: imagePropertiesPanelLoader.active || textFormatPanelLoader.active
                || root.headerSyncPanelActive

            Loader {
                id: headerSyncPanelLoader
                objectName: "headerSyncPanelLoader"

                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(implicitHeight,
                    selectionProperties.height * 0.65)
                active: root.headerSyncPanelActive
                    && (!root.session.panelCollapsed || selectionProperties.syncOnly)
                visible: active

                sourceComponent: Component {
                    HeaderSyncPanel {
                        theme: root.theme
                        editController: root.session.editController
                        pendingPanelEdits: root.hasPendingPanelEdits
                        allMatchingPages: root.headerSyncAllMatchingPages
                        pageRange: root.headerSyncPageRange
                        onPageScopeChangeRequested: allMatchingPages => {
                            root.headerSyncAllMatchingPages = allMatchingPages
                        }
                        onPageRangeChangeRequested: pages => {
                            root.headerSyncPageRange = pages
                        }
                        onApplyRequested: pages => {
                            if (root.prepareForDeactivation())
                                root.session.editController.applyHeaderChanges(pages)
                        }
                    }
                }
            }

            Loader {
                id: imagePropertiesPanelLoader

                Layout.fillWidth: true
                Layout.fillHeight: true
                active: root.session.document.loaded
                    && root.imagePropertiesPanelActive
                visible: active

                sourceComponent: Component {
                    ImagePropertiesPanel {
                        theme: root.theme
                        editController: root.session.editController
                        collapsed: root.session.panelCollapsed
                        onCollapsedChangeRequested: collapsed => {
                            root.session.panelCollapsed = collapsed
                        }
                    }
                }
            }

            Loader {
                id: textFormatPanelLoader

                Layout.fillWidth: true
                Layout.fillHeight: true
                active: root.session.document.loaded
                    && (root.editTabActive || (root.headerSelectionActive
                        && root.session.editController.selectedObjectId.length > 0))
                    && !root.textFieldOptionsPanelActive
                    && !root.combFieldOptionsPanelActive
                    && !root.dropdownOptionsPanelActive
                    && !root.listBoxOptionsPanelActive
                    && !root.radioButtonOptionsPanelActive
                    && !root.drawingCommentPropertiesPanelActive
                    && !root.imagePropertiesPanelActive
                    && root.session.editController.tool !== PDFEditController.BrowseTool
                visible: active

                sourceComponent: Component {
                    TextFormatPanel {
                        theme: root.theme
                        collapsed: root.session.panelCollapsed
                        availableFontFamilies:
                            root.session.editController.availableFontFamilies
                        fontFamiliesForFilter: function(filter) {
                            return root.session.editController.fontFamiliesForFilter(filter)
                        }
                        selectedFontFamily:
                            root.session.editController.selectedFontFamily
                        selectedFontSize: root.session.editController.selectedFontSize
                        selectedLetterSpacing: root.session.editController.selectedLetterSpacing
                        selectedWordSpacing: root.session.editController.selectedWordSpacing
                        selectedLineSpacing: root.session.editController.selectedLineSpacing
                        selectedParagraphSpaceBefore:
                            root.session.editController.selectedParagraphSpaceBefore
                        selectedParagraphSpaceAfter:
                            root.session.editController.selectedParagraphSpaceAfter
                        formattingSession: root.session
                        formattingContextKey: root.session.document.source + ":"
                            + root.session.editController.selectedPage + ":"
                            + root.session.editController.selectedObjectId + ":"
                            + root.session.editController.textEditActive + ":"
                            + root.session.editController.saving
                        selectedFontBold: root.session.editController.selectedFontBold
                        selectedFontItalic:
                            root.session.editController.selectedFontItalic
                        selectedFontUnderline:
                            root.session.editController.selectedFontUnderline
                        selectedColor: root.session.editController.selectedColor
                        selectedParagraphAlignment:
                            root.session.editController.selectedParagraphAlignment
                        formattingEnabled:
                            root.session.editController.selectedEditable
                            && (root.session.editController.selectedKind === "text"
                                || root.session.editController.selectedKind
                                    === "addedText")
                        onFontFamilyChangeRequested: fontFamily => {
                            if (!root.session.editController.textEditActive)
                                documentView.beginSelectedEditingWithStyle("fontFamily:" + fontFamily)
                            else
                                root.session.editController.setSelectedFontFamily(fontFamily)
                        }
                        onFontSizeChangeRequested: fontSize => {
                            root.session.editController.setSelectedFontSize(fontSize)
                        }
                        onLetterSpacingChangeRequested: points => {
                            root.session.editController.setSelectedLetterSpacing(points)
                        }
                        onWordSpacingChangeRequested: points => {
                            root.session.editController.setSelectedWordSpacing(points)
                        }
                        onLineSpacingChangeRequested: factor => {
                            root.session.editController.setSelectedLineSpacing(factor)
                        }
                        onParagraphSpaceBeforeChangeRequested: points => {
                            root.session.editController.setSelectedParagraphSpaceBefore(points)
                        }
                        onParagraphSpaceAfterChangeRequested: points => {
                            root.session.editController.setSelectedParagraphSpaceAfter(points)
                        }
                        onFontBoldChangeRequested: enabled => {
                            root.applyInlineStyleChange("bold", enabled)
                        }
                        onFontItalicChangeRequested: enabled => {
                            root.applyInlineStyleChange("italic", enabled)
                        }
                        onFontUnderlineChangeRequested: enabled => {
                            root.applyInlineStyleChange("underline", enabled)
                        }
                        onFontColorChangeRequested: color => {
                            root.session.editController.setSelectedColor(color)
                        }
                        onParagraphAlignmentChangeRequested: alignment => {
                            root.session.editController.setSelectedParagraphAlignment(
                                alignment)
                        }
                        onCollapsedChangeRequested: collapsed => {
                            root.session.panelCollapsed = collapsed
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !imagePropertiesPanelLoader.active && !textFormatPanelLoader.active
            }
        }

        Loader {
            id: radioButtonOptionsPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && root.radioButtonOptionsPanelActive
            visible: active

            sourceComponent: Component {
                RadioButtonOptionsPanel {
                    theme: root.theme
                    controller: root.session.formDesignerController
                    fieldId: root.selectedFormFieldId
                    collapsed: root.session.panelCollapsed
                    onCollapsedChangeRequested: collapsed => {
                        root.session.panelCollapsed = collapsed
                    }
                }
            }
        }
    }
}
