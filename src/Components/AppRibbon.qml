pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

Column {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    required property PDFSearchController searchController
    property PDFDocumentSession session: null
    property bool anySaving: false
    property bool hasPendingPanelEdits: false
    property bool ocrPanelVisible: false
    property bool headerTextToolSelected: false
    property bool headerImageToolSelected: false
    property bool headerPageNumberToolSelected: false
    readonly property bool headerPlacementPending: headerTextToolSelected
        || headerImageToolSelected || headerPageNumberToolSelected
    property bool hasPendingHeaderPlacement: false
    property int acceptedTabIndex: 0
    property var prepareHeaderEntry: function() { return !root.hasUnsavedChanges }
    property var prepareHeaderExit: function() { return !root.hasUnsavedChanges }
    property PDFEditController syncedEditController: null
    property int viewNavigationPage: document.currentPage
    property bool viewCanNavigateBackward: viewNavigationPage > 0
    property bool viewCanNavigateForward:
        document.loaded && viewNavigationPage + 1 < document.pageCount
    readonly property bool hasUnsavedChanges:
        editController.hasDrafts || hasPendingPanelEdits
    property bool textFieldToolSelected: false
    property bool checkboxToolSelected: false
    property bool radioButtonToolSelected: false
    property bool dropdownToolSelected: false
    property bool listBoxToolSelected: false
    property bool combFieldToolSelected: false
    property bool commentsPanelVisible: true
    property bool searchFocusPending: false
    readonly property bool viewTabActive: root.acceptedTabIndex === 0
    readonly property bool editTabActive: root.acceptedTabIndex === 1
    readonly property bool commentTabActive: root.acceptedTabIndex === 2
    readonly property bool formsTabActive: root.acceptedTabIndex === 3
    readonly property bool signTabActive: root.acceptedTabIndex === 4
    readonly property bool protectTabActive: root.acceptedTabIndex === 5
    readonly property bool exportTabActive: root.acceptedTabIndex === 6
    readonly property bool headerTabActive: root.acceptedTabIndex === 7
    readonly property bool footerTabActive: root.acceptedTabIndex === 8
    readonly property bool settingsTabActive: root.acceptedTabIndex === 9
    readonly property bool informationTabActive: root.acceptedTabIndex === 10
    readonly property bool marginTabActive: headerTabActive || footerTabActive
    readonly property bool textFieldToolActive: formsTabActive
        && textFieldToolSelected
        && document.loaded
    readonly property bool checkboxToolActive: formsTabActive
        && checkboxToolSelected
        && document.loaded
    readonly property bool radioButtonToolActive: formsTabActive
        && radioButtonToolSelected
        && document.loaded
    readonly property bool dropdownToolActive: formsTabActive
        && dropdownToolSelected
        && document.loaded
    readonly property bool listBoxToolActive: formsTabActive
        && listBoxToolSelected
        && document.loaded
    readonly property bool combFieldToolActive: formsTabActive
        && combFieldToolSelected
        && document.loaded

    signal openRequested()
    signal saveRequested()
    signal saveAsRequested()
    signal excelExportRequested()
    signal ocrRequested()
    signal mergePdfRequested()
    signal fontSettingsRequested()
    signal pageRequested(int page)
    signal pageStepRequested(int direction)
    signal searchResultRequested(int page,
                                 real normalizedX,
                                 real normalizedY,
                                 real resultsRevision)
    signal deletePageRequested(int page)
    signal zoomOutRequested()
    signal zoomInRequested()
    signal fitWidthRequested()
    signal rotateRequested(int quarterTurns)
    signal pageDisplayModeRequested(int mode)
    signal separateCoverPageToggleRequested()
    signal splitViewModeRequested(int mode)
    signal splitViewToggleRequested()
    signal toolRequested(string toolName)
    signal signatureRequested(string mode)
    signal setPasswordRequested()
    signal watermarkRequested(string mode)
    signal headerSelectRequested()
    signal headerTextRequested()
    signal headerImageRequested()
    signal headerPageNumbersRequested()
    signal undoRequested()
    signal redoRequested()

    spacing: 0

    function showViewTab() {
        // A document switch has already changed the session; do not run the
        // previous Header's exit guard against the newly active document.
        acceptedTabIndex = 0
        ribbonTabs.setCurrentIndex(0)
        syncEditTool()
    }

    function acceptTabChange() {
        const requestedIndex = ribbonTabs.currentIndex
        if (requestedIndex === acceptedTabIndex)
            return
        if (marginTabActive && document.loaded && !prepareHeaderExit(footerTabActive)) {
            ribbonTabs.setCurrentIndex(acceptedTabIndex)
            return
        }
        if ((requestedIndex === 7 || requestedIndex === 8) && document.loaded
                && !prepareHeaderEntry(requestedIndex === 8)) {
            ribbonTabs.setCurrentIndex(acceptedTabIndex)
            return
        }
        acceptedTabIndex = requestedIndex
        syncEditTool()
    }

    function focusSearch() {
        if (!document.loaded || anySaving)
            return
        ribbonTabs.setCurrentIndex(0)
        searchFocusPending = viewTabActive
        applySearchFocus()
    }

    function applySearchFocus() {
        if (!searchFocusPending || !viewTabActive || ribbonLoader.status !== Loader.Ready)
            return
        searchFocusPending = false
        const viewRibbon = ribbonLoader.item as ViewRibbon
        if (viewRibbon)
            viewRibbon.focusSearch()
    }

    onViewTabActiveChanged: {
        if (!viewTabActive)
            searchFocusPending = false
    }

    function isCommentTool(tool) {
        return tool === PDFEditController.CommentSelectTool
            || tool === PDFEditController.HighlightTool
            || tool === PDFEditController.UnderlineTool
            || tool === PDFEditController.SquigglyTool
            || tool === PDFEditController.StrikeoutTool
            || tool === PDFEditController.FreeTextTool
            || tool === PDFEditController.StickyNoteTool
            || tool === PDFEditController.SquareTool
            || tool === PDFEditController.CircleTool
            || tool === PDFEditController.LineTool
            || tool === PDFEditController.PolygonTool
            || tool === PDFEditController.PolyLineTool
            || tool === PDFEditController.InkTool
            || tool === PDFEditController.StampTool
    }

    function drawingCommentSelected() {
        const kind = editController.selectedKind
        return kind === "square"
            || kind === "circle"
            || kind === "line"
            || kind === "polygon"
            || kind === "polyLine"
            || kind === "ink"
    }

    function revealSelectedDrawingComment() {
        if (acceptedTabIndex === 2 && drawingCommentSelected())
            commentsPanelVisible = true
    }

    function syncEditTool() {
        const activeTabIndex = acceptedTabIndex
        const editActive = activeTabIndex === 1
        const signActive = activeTabIndex === 4
        const commentActive = activeTabIndex === 2
        const formsActive = activeTabIndex === 3
        const marginActive = activeTabIndex === 7 || activeTabIndex === 8

        if (syncedEditController && syncedEditController !== editController)
            syncedEditController.headerSelectionOnly = false
        syncedEditController = editController
        editController.footerMode = activeTabIndex === 8
        editController.headerSelectionOnly = document.loaded && marginActive

        if (!document.loaded || !formsActive) {
            textFieldToolSelected = false
            checkboxToolSelected = false
            radioButtonToolSelected = false
            dropdownToolSelected = false
            listBoxToolSelected = false
            combFieldToolSelected = false
        }

        if (!document.loaded) {
            editController.tool = PDFEditController.BrowseTool
        } else if (marginActive) {
            editController.tool = headerPlacementPending
                ? PDFEditController.BrowseTool : PDFEditController.SelectTool
        } else if (commentActive) {
            editController.tool = PDFEditController.CommentSelectTool
            editController.requestCommentIndex()
        } else if (signActive) {
            editController.tool = PDFEditController.SelectTool
        } else if (editActive
                   && (editController.tool === PDFEditController.BrowseTool
                       || isCommentTool(editController.tool))) {
            editController.tool = PDFEditController.SelectTool
        } else if (!editActive) {
            editController.tool = PDFEditController.BrowseTool
        }
        revealSelectedDrawingComment()
    }

    Component.onCompleted: syncEditTool()
    onHeaderPlacementPendingChanged: {
        if (marginTabActive)
            syncEditTool()
    }
    onDocumentChanged: {
        searchFocusPending = false
        textFieldToolSelected = false
        checkboxToolSelected = false
        radioButtonToolSelected = false
        dropdownToolSelected = false
        listBoxToolSelected = false
        combFieldToolSelected = false
        syncEditTool()
    }
    onEditControllerChanged: {
        textFieldToolSelected = false
        checkboxToolSelected = false
        radioButtonToolSelected = false
        dropdownToolSelected = false
        listBoxToolSelected = false
        combFieldToolSelected = false
        syncEditTool()
    }

    Connections {
        target: root.document

        function onLoadedChanged() {
            root.syncEditTool()
        }
    }

    Connections {
        target: root.editController

        function onSelectionChanged() {
            root.revealSelectedDrawingComment()
        }
    }

    Connections {
        target: root.searchController

        function onResultActivated(page,
                                   normalizedX,
                                   normalizedY,
                                   resultsRevision) {
            if (root.viewTabActive) {
                root.searchResultRequested(
                    page,
                    normalizedX,
                    normalizedY,
                    resultsRevision)
            }
        }
    }

    Component {
        id: viewRibbonComponent

        ViewRibbon {
            theme: root.theme
            document: root.document
            editController: root.editController
            searchController: root.searchController
            session: root.session
            hasPendingPanelEdits: root.hasPendingPanelEdits
            navigationPage: root.viewNavigationPage
            canNavigateBackward: root.viewCanNavigateBackward
            canNavigateForward: root.viewCanNavigateForward
            onOpenRequested: root.openRequested()
            onSaveRequested: root.saveRequested()
            onSaveAsRequested: root.saveAsRequested()
            onPageRequested: page => root.pageRequested(page)
            onPageStepRequested: direction =>
                root.pageStepRequested(direction)
            onZoomOutRequested: root.zoomOutRequested()
            onZoomInRequested: root.zoomInRequested()
            onFitWidthRequested: root.fitWidthRequested()
            onRotateRequested: quarterTurns => root.rotateRequested(quarterTurns)
            onPageDisplayModeRequested: mode =>
                root.pageDisplayModeRequested(mode)
            onSeparateCoverPageToggleRequested:
                root.separateCoverPageToggleRequested()
            onSplitViewModeRequested: mode =>
                root.splitViewModeRequested(mode)
            onSplitViewToggleRequested: root.splitViewToggleRequested()
        }
    }

    Component {
        id: editRibbonComponent

        EditRibbon {
            enabled: !root.editController.watermarkChangesPending
            theme: root.theme
            document: root.document
            editController: root.editController
            hasPendingPanelEdits: root.hasPendingPanelEdits
            onPageRequested: page => root.pageRequested(page)
            onDeletePageRequested: page => root.deletePageRequested(page)
            onMergePdfRequested: root.mergePdfRequested()
            onUndoRequested: root.undoRequested()
            onRedoRequested: root.redoRequested()
            onToolRequested: toolName => root.toolRequested(toolName)
        }
    }

    Component {
        id: commentRibbonComponent

        CommentRibbon {
            theme: root.theme
            document: root.document
            editController: root.editController
            commentsPanelVisible: root.commentsPanelVisible
            hasPendingPanelEdits: root.hasPendingPanelEdits
            onPageRequested: page => root.pageRequested(page)
            onToolRequested: toolName => root.toolRequested(toolName)
            onCommentsPanelToggleRequested: {
                root.commentsPanelVisible = !root.commentsPanelVisible
            }
            onUndoRequested: root.undoRequested()
            onRedoRequested: root.redoRequested()
        }
    }

    Component {
        id: formRibbonComponent

        FormRibbon {
            theme: root.theme
            document: root.document
            textFieldToolSelected: root.textFieldToolSelected
            checkboxToolSelected: root.checkboxToolSelected
            radioButtonToolSelected: root.radioButtonToolSelected
            dropdownToolSelected: root.dropdownToolSelected
            listBoxToolSelected: root.listBoxToolSelected
            combFieldToolSelected: root.combFieldToolSelected
            onSelectRequested: {
                root.textFieldToolSelected = false
                root.checkboxToolSelected = false
                root.radioButtonToolSelected = false
                root.dropdownToolSelected = false
                root.listBoxToolSelected = false
                root.combFieldToolSelected = false
            }
            onTextFieldToolRequested: {
                root.textFieldToolSelected = !root.textFieldToolSelected
                root.checkboxToolSelected = false
                root.radioButtonToolSelected = false
                root.dropdownToolSelected = false
                root.listBoxToolSelected = false
                root.combFieldToolSelected = false
            }
            onCheckboxToolRequested: {
                root.checkboxToolSelected = !root.checkboxToolSelected
                root.textFieldToolSelected = false
                root.radioButtonToolSelected = false
                root.dropdownToolSelected = false
                root.listBoxToolSelected = false
                root.combFieldToolSelected = false
            }
            onRadioButtonToolRequested: {
                root.radioButtonToolSelected = !root.radioButtonToolSelected
                root.textFieldToolSelected = false
                root.checkboxToolSelected = false
                root.dropdownToolSelected = false
                root.listBoxToolSelected = false
                root.combFieldToolSelected = false
            }
            onDropdownToolRequested: {
                root.dropdownToolSelected = !root.dropdownToolSelected
                root.textFieldToolSelected = false
                root.checkboxToolSelected = false
                root.radioButtonToolSelected = false
                root.listBoxToolSelected = false
                root.combFieldToolSelected = false
            }
            onListBoxToolRequested: {
                root.listBoxToolSelected = !root.listBoxToolSelected
                root.textFieldToolSelected = false
                root.checkboxToolSelected = false
                root.radioButtonToolSelected = false
                root.dropdownToolSelected = false
                root.combFieldToolSelected = false
            }
            onCombFieldToolRequested: {
                root.combFieldToolSelected = !root.combFieldToolSelected
                root.textFieldToolSelected = false
                root.checkboxToolSelected = false
                root.radioButtonToolSelected = false
                root.dropdownToolSelected = false
                root.listBoxToolSelected = false
            }
        }
    }

    Component {
        id: signRibbonComponent

        SignRibbon {
            theme: root.theme
            document: root.document
            editController: root.editController
            hasPendingPanelEdits: root.hasPendingPanelEdits
            onSignatureRequested: root.signatureRequested("signature")
            onInitialsRequested: root.signatureRequested("initials")
            onDateRequested: root.signatureRequested("date")
            onCheckRequested: root.signatureRequested("check")
            onSelectRequested: root.editController.tool = PDFEditController.SelectTool
            onUndoRequested: root.undoRequested()
            onRedoRequested: root.redoRequested()
            onSaveAsRequested: root.saveAsRequested()
        }
    }

    Component {
        id: protectRibbonComponent

        ProtectRibbon {
            theme: root.theme
            document: root.document
            editController: root.editController
            hasPendingPanelEdits: root.hasPendingPanelEdits
            anySaving: root.anySaving
            onSetPasswordRequested: root.setPasswordRequested()
            onWatermarkRequested: mode => root.watermarkRequested(mode)
            onSaveRequested: root.saveRequested()
            onSaveAsRequested: root.saveAsRequested()
        }
    }

    Component {
        id: exportRibbonComponent

        ExportRibbon {
            theme: root.theme
            session: root.session
            anySaving: root.anySaving
            hasPendingPanelEdits: root.hasPendingPanelEdits
            ocrPanelVisible: root.ocrPanelVisible
            onExportRequested: root.excelExportRequested()
            onOcrRequested: root.ocrRequested()
        }
    }

    Component {
        id: headerRibbonComponent

        Row {
            x: 8
            height: 76

            RibbonGroup {
                theme: root.theme
                title: root.footerTabActive
                    //% "Footer"
                    ? qsTrId("FooterTab")
                    //% "Header"
                    : qsTrId("HeaderTab")

                RibbonButton {
                    objectName: root.footerTabActive ? "footerSelectButton" : "headerSelectButton"
                    theme: root.theme
                    documentLoaded: root.document.loaded
                    enabled: root.document.loaded && !root.anySaving
                    primary: root.document.loaded && root.marginTabActive
                        && !root.headerPlacementPending
                        && root.editController.tool === PDFEditController.SelectTool
                    //% "Select"
                    text: qsTrId("Select")
                    iconSource: "../assets/icons/mouse-pointer-2.svg"
                    onClicked: root.headerSelectRequested()
                }

                RibbonButton {
                    objectName: root.footerTabActive ? "footerAddTextButton" : "headerAddTextButton"
                    theme: root.theme
                    documentLoaded: root.document.loaded
                    enabled: root.document.loaded && !root.anySaving
                    primary: root.document.loaded && root.marginTabActive
                        && root.headerTextToolSelected
                    //% "Add Text"
                    text: qsTrId("AddText")
                    iconSource: "../assets/icons/type.svg"
                    onClicked: root.headerTextRequested()
                }

                RibbonButton {
                    objectName: root.footerTabActive ? "footerAddImageButton" : "headerAddImageButton"
                    theme: root.theme
                    documentLoaded: root.document.loaded
                    enabled: root.document.loaded && !root.anySaving
                    primary: root.document.loaded && root.marginTabActive
                        && root.headerImageToolSelected
                    //% "Image"
                    text: qsTrId("Image")
                    iconSource: "../assets/icons/image-plus.svg"
                    onClicked: root.headerImageRequested()
                }

                RibbonButton {
                    objectName: root.footerTabActive ? "footerPageNumbersButton" : "headerPageNumbersButton"
                    theme: root.theme
                    documentLoaded: root.document.loaded
                    enabled: root.document.loaded && !root.anySaving
                    primary: root.document.loaded && root.marginTabActive
                        && root.headerPageNumberToolSelected
                    //% "Page numbers"
                    text: qsTrId("HeaderPageNumbers")
                    iconSource: "../assets/icons/page-single.svg"
                    onClicked: root.headerPageNumbersRequested()
                }
            }

            RibbonGroup {
                theme: root.theme
                //% "File"
                title: qsTrId("File")

                RibbonButton {
                    objectName: root.footerTabActive ? "footerSaveButton" : "headerSaveButton"
                    readonly property bool hasChanges: root.hasUnsavedChanges
                        || root.hasPendingHeaderPlacement || root.editController.textEditActive
                    theme: root.theme
                    documentLoaded: root.document.loaded
                    enabled: root.document.loaded && hasChanges && !root.anySaving
                    primary: hasChanges
                    //% "Save"
                    text: qsTrId("Save")
                    //% "Save PDF"
                    toolTipText: qsTrId("SavePdf")
                    iconSource: "../assets/icons/save.svg"
                    onClicked: root.saveRequested()
                }
            }
        }
    }

    Component {
        id: settingsRibbonComponent

        Row {
            x: 8
            height: 76

            RibbonGroup {
                theme: root.theme
                //% "Settings"
                title: qsTrId("SettingsTab")

                RibbonButton {
                    objectName: "settingsFontButton"
                    theme: root.theme
                    documentLoaded: root.document.loaded
                    requiresDocument: false
                    //% "Font"
                    text: qsTrId("SettingsFontButton")
                    iconSource: "../assets/icons/type.svg"
                    enabled: !root.anySaving
                    onClicked: root.fontSettingsRequested()
                }
            }
        }
    }

    Component {
        id: informationRibbonComponent

        InformationRibbon {
            theme: root.theme
        }
    }

    RibbonTabStrip {
        id: ribbonTabs

        width: parent.width
        theme: root.theme
        anySaving: root.anySaving
        hasUnsavedChanges: root.hasUnsavedChanges
        onCurrentIndexChanged: root.acceptTabChange()
    }

    RibbonSurface {
        width: parent.width
        theme: root.theme

        Loader {
            id: ribbonLoader
            anchors.fill: parent
            onLoaded: Qt.callLater(root.applySearchFocus)
            enabled: !root.anySaving || root.exportTabActive
            sourceComponent: root.acceptedTabIndex === 0
                ? viewRibbonComponent
                : root.acceptedTabIndex === 1
                    ? editRibbonComponent
                    : root.acceptedTabIndex === 2
                        ? commentRibbonComponent
                        : root.acceptedTabIndex === 3
                            ? formRibbonComponent
                            : root.acceptedTabIndex === 4
                                ? signRibbonComponent
                                : root.acceptedTabIndex === 5
                                    ? protectRibbonComponent
                                    : root.acceptedTabIndex === 6
                                        ? exportRibbonComponent
                                        : root.marginTabActive
                                            ? headerRibbonComponent
                                            : root.settingsTabActive
                                                ? settingsRibbonComponent
                                                : root.informationTabActive
                                                    ? informationRibbonComponent
                                                    : null
        }
    }
}
