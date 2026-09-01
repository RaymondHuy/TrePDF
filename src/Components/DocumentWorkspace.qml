pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFDocumentSession session
    property bool viewTabActive: false
    property bool formsTabActive: false
    property bool editTabActive: false
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
        editTabActive && drawingCommentSelected
    readonly property bool hasPendingPanelEdits: {
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
    signal deleteCommentRequested(int page, string objectId)

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

    function setZoom(value) {
        session.zoom = Math.max(0.1, Math.min(5.0, value))
    }

    function fitToWidth() {
        const maximumViewPageWidth = session.viewRotation % 180 === 0
            ? session.document.maximumPageWidth
            : session.document.maximumPageHeight
        if (!session.document.loaded
                || maximumViewPageWidth <= 0
                || documentView.viewportWidth <= 0) {
            return
        }

        setZoom((documentView.viewportWidth - 96)
                / maximumViewPageWidth)
    }

    function navigateToPage(page) {
        documentView.navigateToPage(page)
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

    function prepareInlineStyleChange() {
        return session.editController.textEditActive
            || documentView.beginSelectedEditing()
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

        documentView.prepareForSessionChange()
        session.scrollX = documentView.contentX
        session.scrollY = documentView.contentY
        session.prepareForDeactivation()
        return true
    }

    Component.onDestruction: prepareForDeactivation()

    onViewTabActiveChanged: {
        if (!viewTabActive)
            clearPendingSearchNavigation()
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

        DocumentView {
            id: documentView

            Layout.fillWidth: true
            Layout.fillHeight: true
            theme: root.theme
            document: root.session.document
            editController: root.session.editController
            searchController: root.session.searchController
            formController: root.session.formController
            formDesignerController: root.session.formDesignerController
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
            zoom: root.session.zoom
            viewRotation: root.session.viewRotation
            transientStatus: root.transientStatus
            onOpenRequested: root.openRequested()
            onNavigationSettled: generation =>
                root.finishInitialView(generation)
            onDeleteCommentRequested: (page, objectId) =>
                root.deleteCommentRequested(page, objectId)
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

        Loader {
            id: textFormatPanelLoader

            Layout.preferredWidth: active
                ? root.session.panelCollapsed ? 42 : 264
                : 0
            Layout.fillHeight: true
            active: root.session.document.loaded
                && root.editTabActive
                && !root.textFieldOptionsPanelActive
                && !root.combFieldOptionsPanelActive
                && !root.dropdownOptionsPanelActive
                && !root.listBoxOptionsPanelActive
                && !root.radioButtonOptionsPanelActive
                && !root.drawingCommentPropertiesPanelActive
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
                        root.session.editController.setSelectedFontFamily(fontFamily)
                    }
                    onFontBoldChangeRequested: enabled => {
                        const editWasActive = root.session.editController.textEditActive
                        if (root.prepareInlineStyleChange()) {
                            root.session.editController.setSelectedFontBold(
                                editWasActive
                                    ? enabled
                                    : !root.session.editController.selectedFontBold)
                        }
                    }
                    onFontItalicChangeRequested: enabled => {
                        const editWasActive = root.session.editController.textEditActive
                        if (root.prepareInlineStyleChange()) {
                            root.session.editController.setSelectedFontItalic(
                                editWasActive
                                    ? enabled
                                    : !root.session.editController.selectedFontItalic)
                        }
                    }
                    onFontUnderlineChangeRequested: enabled => {
                        const editWasActive = root.session.editController.textEditActive
                        if (root.prepareInlineStyleChange()) {
                            root.session.editController.setSelectedFontUnderline(
                                editWasActive
                                    ? enabled
                                    : !root.session.editController.selectedFontUnderline)
                        }
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
