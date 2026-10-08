pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import TrePDF
import "Components/ios"

ApplicationWindow {
    id: window

    width: 1200
    height: 800
    minimumWidth: windowMetrics.minimumWidth
    minimumHeight: windowMetrics.minimumHeight
    visible: true
    //% "TrePDF"
    title: qsTrId("ApplicationTitle")

    WindowMetrics { id: windowMetrics; window: window }

    DocumentAccess {
        id: documentAccess
        onFilesAccepted: urls => window.openFiles(urls)
        onErrorOccurred: message => window.showStatus(message)
    }

    property string transientStatus: ""
    property bool allowWindowClose: false
    property string pendingDraftAction: ""
    property bool openingFiles: false
    property var pendingOpenFiles: []
    property var passwordRequests: []
    property bool passwordRetryRequested: false
    property PDFDocumentSession pendingSession: null
    property PDFEditController fontDownloadController: null
    property PDFDocumentSession saveAsSession: null
    property PDFDocumentSession excelExportSession: null
    property PDFDocumentSession workspaceSession: null
    property PDFDocumentSession deletePageSession: null
    property int deletePageIndex: -1
    property PDFDocumentSession externalLinkSession: null
    property var externalLinkRequestId: 0
    property string externalLinkAuthority: ""
    property string externalLinkDisplayUrl: ""
    property bool externalLinkInsecureHttp: false
    property HeaderImagePlacement headerImagePickerPlacement: null
    property PDFDocumentSession headerImagePickerSession: null
    property PDFEditController imageInsertController: null
    property int imageInsertPage: -1
    property real imageInsertX: 0
    property real imageInsertY: 0
    property PDFDocumentSession signatureSession: null
    property PDFDocumentSession passwordSetupSession: null
    property PDFDocumentSession watermarkSession: null
    property string watermarkMode: "add"
    property string signatureMode: "signature"
    property bool signaturePlacementPending: false
    property real signatureWidthPoints: 140
    property PDFDocumentSession pendingSearchNavigationSession: null
    property int pendingSearchNavigationPage: -1
    property real pendingSearchNavigationX: 0.5
    property real pendingSearchNavigationY: 0.0
    property real pendingSearchNavigationRevision: -1
    readonly property PDFDocumentSession activeSession: tabModel.currentSession
    readonly property DocumentWorkspace activeWorkspaceItem: {
        if (workspaceLoader.status !== Loader.Ready)
            return null
        const workspace = workspaceLoader.item as DocumentWorkspace
        return workspace && workspace.session === activeSession
            ? workspace
            : null
    }
    readonly property PDFDocument activeDocument: activeSession
        ? activeSession.document
        : emptyDocument
    readonly property PDFEditController activeEditController: activeSession
        ? activeSession.editController
        : emptyEditController
    readonly property PDFSearchController activeSearchController: activeSession
        ? activeSession.searchController
        : emptySearchController
    readonly property PDFFormDesignerController activeFormDesignerController:
        activeSession
            ? activeSession.formDesignerController
            : emptyFormDesignerController
    readonly property bool activeWorkspaceHasPendingEdits:
        activeWorkspaceItem ? activeWorkspaceItem.hasPendingPanelEdits : false
    readonly property bool modalOperationActive:
        passwordRequests.length > 0 || fontDownloadController !== null
        || signatureDialogLoader.active || setPasswordDialogLoader.active
        || watermarkDialogLoader.active || openDialog.visible
        || imageDialog.visible || headerImageDialog.visible || saveAsDialog.visible || excelExportDialog.visible
        || externalLinkPopup.visible || discardDraftPopup.visible || deletePagePopup.visible
        || documentAccess.busy || saveAsDialog.writePending || excelExportDialog.writePending
    readonly property bool fileOpenBlocked:
        tabModel.anySaving || openingFiles || modalOperationActive

    onFileOpenBlockedChanged: {
        if (!fileOpenBlocked)
            Qt.callLater(flushPendingOpenFiles)
    }
    onActiveWorkspaceHasPendingEditsChanged: {
        if (!activeWorkspaceHasPendingEdits)
            Qt.callLater(flushPendingOpenFiles)
    }

    palette.highlight: appTheme.foxitOrange
    palette.button: appTheme.foxitPaleOrange
    palette.buttonText: "#32251c"
    palette.window: "#f7f2ee"

    function activeWorkspace() {
        return activeWorkspaceItem
    }

    function prepareActiveWorkspace() {
        const workspace = activeWorkspace()
        if (workspace)
            return workspace.prepareForDeactivation()
        if (activeSession)
            activeSession.prepareForDeactivation()
        return true
    }

    function rebuildWorkspace() {
        const workspace = activeWorkspace()
        if (workspace && !workspace.prepareForDeactivation())
            return false
        workspaceLoader.active = false
        workspaceSession = activeSession
        workspaceLoader.active = workspaceSession !== null
        return true
    }

    function setZoom(value) {
        const workspace = activeWorkspace()
        if (workspace)
            workspace.setZoom(value)
        else if (activeSession)
            activeSession.zoom = Math.max(0.1, Math.min(5.0, value))
    }

    function zoomBy(factor) {
        const workspace = activeWorkspace()
        if (workspace)
            workspace.zoomBy(factor)
        else if (activeSession)
            setZoom(activeSession.zoom * factor)
    }

    function fitToWidth() {
        const workspace = activeWorkspace()
        if (workspace) {
            workspace.fitToWidth()
        } else if (activeSession) {
            activeSession.initialFitPending = true
        }
    }

    function goToPage(page) {
        const workspace = activeWorkspace()
        if (workspace)
            workspace.navigateToPage(page)
    }

    function goToActivePage(page) {
        const workspace = activeWorkspace()
        if (workspace) {
            workspace.navigateActivePaneToPage(page)
        } else if (activeDocument.loaded) {
            activeDocument.currentPage = page
        }
    }

    function setPageDisplayMode(mode) {
        const workspace = activeWorkspace()
        if (workspace)
            workspace.setPageDisplayMode(mode)
        else if (activeSession)
            activeSession.pageDisplayMode = mode
    }

    function clearPendingSearchNavigation() {
        pendingSearchNavigationSession = null
        pendingSearchNavigationPage = -1
        pendingSearchNavigationRevision = -1
    }

    function goToSearchResult(page,
                              normalizedX,
                              normalizedY,
                              resultsRevision) {
        if (!activeSession
                || !appRibbon.viewTabActive
                || !activeDocument.loaded
                || page < 0
                || page >= activeDocument.pageCount
                || activeSearchController.resultsRevision
                    !== resultsRevision) {
            return
        }

        const workspace = activeWorkspace()
        if (workspace) {
            clearPendingSearchNavigation()
            workspace.navigateToSearchResult(
                page,
                normalizedX,
                normalizedY,
                resultsRevision)
            return
        }

        pendingSearchNavigationSession = activeSession
        pendingSearchNavigationPage = page
        pendingSearchNavigationX = normalizedX
        pendingSearchNavigationY = normalizedY
        pendingSearchNavigationRevision = resultsRevision
    }

    function rotateView(quarterTurns) {
        if (!activeSession || !activeDocument.loaded)
            return

        const workspace = activeWorkspace()
        if (workspace)
            workspace.rotateView(quarterTurns)
        else
            activeSession.rotateView(quarterTurns)
    }

    function switchToTab(index) {
        if (index === tabModel.currentIndex || tabModel.anySaving)
            return

        if (!prepareActiveWorkspace())
            return
        tabModel.currentIndex = index
    }

    function requestOpen() {
        if (modalOperationActive)
            return
        if (tabModel.anySaving) {
            //% "Please wait for the PDF to finish saving."
            showStatus(qsTrId("WaitForPdfSave"))
            return
        }

        openDialog.open()
    }

    function openFiles(urls) {
        const files = FileOpenHandler.localPdfFiles(urls)
        const queued = pendingOpenFiles.slice()
        for (let index = 0; index < files.length; ++index) {
            const source = files[index].toString()
            if (!queued.some(file => file.toString() === source)
                    && !passwordRequests.some(request => request.source.toString() === source))
                queued.push(files[index])
        }
        pendingOpenFiles = queued
        flushPendingOpenFiles()
    }

    function flushPendingOpenFiles() {
        if (fileOpenBlocked || pendingOpenFiles.length === 0)
            return
        if (!prepareActiveWorkspace())
            return
        const urls = pendingOpenFiles
        pendingOpenFiles = []
        openingFiles = true
        for (let index = 0; index < urls.length; ++index)
            tabModel.open(urls[index])
        openingFiles = false
        rebuildWorkspace()
    }

    function receiveExternalFiles() {
        const files = FileOpenHandler.takePendingFiles()
        if (files.length === 0)
            return
        if (visibility === Window.Minimized)
            showNormal()
        raise()
        requestActivate()
        openFiles(files)
    }

    function requestFind() {
        if (!activeDocument.loaded || fileOpenBlocked || !prepareActiveWorkspace())
            return
        appRibbon.focusSearch()
    }

    Component.onCompleted: Qt.callLater(receiveExternalFiles)

    Connections {
        target: FileOpenHandler
        function onFilesPending() { window.receiveExternalFiles() }
    }

    function requestPassword(source, incorrectPassword) {
        const requests = passwordRequests.slice()
        const sourceString = source.toString()
        for (let index = 0; index < requests.length; ++index) {
            if (requests[index].source.toString() === sourceString) {
                requests[index] = { source: source, incorrect: incorrectPassword }
                passwordRequests = requests
                if (index === 0)
                    passwordRetryRequested = true
                return
            }
        }
        requests.push({ source: source, incorrect: incorrectPassword })
        passwordRequests = requests
    }

    function finishPasswordRequest() {
        passwordRequests = passwordRequests.slice(1)
    }

    function submitPassword(password) {
        if (passwordRequests.length === 0 || tabModel.anySaving)
            return
        if (!prepareActiveWorkspace())
            return
        passwordRetryRequested = false
        tabModel.open(passwordRequests[0].source, password)
        if (!passwordRetryRequested)
            finishPasswordRequest()
    }

    function requestSave() {
        if (activeSession && prepareActiveWorkspace()) {
            const workspace = activeWorkspace()
            if (workspace)
                workspace.closeHeaderPlacement()
            tabModel.requestSave(activeSession)
        }
    }

    function requestSaveAs() {
        if (!activeSession || tabModel.anySaving)
            return

        const workspace = activeWorkspace()
        if (workspace && !workspace.commitPendingPanelEdits())
            return

        if (workspace)
            workspace.closeHeaderPlacement()

        saveAsSession = activeSession
        saveAsDialog.selectedFile = saveAsSession.document.source
        saveAsDialog.open()
    }

    function requestExcelExport() {
        if (!activeSession || tabModel.anySaving
                || !activeDocument.loaded || !prepareActiveWorkspace()) {
            return
        }

        const session = activeSession
        if (session.hasDrafts) {
            //% "Save the PDF changes before exporting to Excel."
            showStatus(qsTrId("SaveBeforeExcelExport"))
            return
        }
        if (session.excelExportController.busy)
            return

        excelExportSession = session
        const source = session.document.source.toString()
        excelExportDialog.selectedFile = /\.pdf$/i.test(source)
            ? source.replace(/\.pdf$/i, ".xlsx")
            : source + ".xlsx"
        excelExportDialog.open()
    }

    function exportExcelTo(url) {
        const session = excelExportSession
        excelExportSession = null
        if (!session || tabModel.indexOf(session) < 0
                || !session.document.loaded || tabModel.anySaving
                || session.saving || session.excelExportController.busy) {
            return
        }
        if (session === activeSession && !prepareActiveWorkspace())
            return
        if (session.hasDrafts) {
            //% "Save the PDF changes before exporting to Excel."
            showStatus(qsTrId("SaveBeforeExcelExport"))
            return
        }
        excelExportDialog.trackWrite(session, url)
        if (!tabModel.requestExcelExport(session, url))
            excelExportDialog.finishWrite(session, false, url)
    }

    function commitActivePanelEdits() {
        const workspace = activeWorkspace()
        return !workspace || workspace.commitPendingPanelEdits()
    }

    function requestUndo() {
        if (!activeSession
                || tabModel.anySaving
                || activeEditController.textEditActive
                || activeFormDesignerController.textFieldFocused
                || appRibbon.formsTabActive
                || !commitActivePanelEdits()) {
            return false
        }
        if (!activeEditController.canUndo)
            return false

        const page = activeEditController.undo()
        if (page >= 0)
            goToPage(page)
        return true
    }

    function requestRedo() {
        if (!activeSession
                || tabModel.anySaving
                || activeEditController.textEditActive
                || activeFormDesignerController.textFieldFocused
                || appRibbon.formsTabActive
                || !commitActivePanelEdits()) {
            return false
        }
        if (!activeEditController.canRedo)
            return false

        const page = activeEditController.redo()
        if (page >= 0)
            goToPage(page)
        return true
    }

    function requestDeleteComment(page, objectId) {
        if (!activeSession
                || tabModel.anySaving
                || activeEditController.saving
                || page < 0
                || objectId.length === 0
                || !commitActivePanelEdits()) {
            return false
        }

        return activeEditController.removeComment(page, objectId)
    }

    function requestImageFile(controller, page, x, y) {
        if (appRibbon.signTabActive) {
            if (signaturePlacementPending && signatureSession === activeSession
                    && controller === activeEditController && !tabModel.anySaving) {
                signaturePlacementPending = false
                const placed = signatureLibrary.place(controller, page, x, y,
                                                      signatureWidthPoints)
                if (!placed.objectId && signatureLibrary.error.length > 0)
                    showStatus(signatureLibrary.error)
                cancelSignature()
            }
            return
        }
        imageInsertController = controller
        imageInsertPage = page
        imageInsertX = x
        imageInsertY = y
        imageDialog.open()
    }

    function requestHeaderText() {
        if (!activeSession || !activeDocument.loaded || tabModel.anySaving
                || !appRibbon.headerTabActive || !prepareActiveWorkspace()) {
            return
        }
        const workspace = activeWorkspace()
        if (workspace)
            workspace.openHeaderTextPanel()
    }

    function requestHeaderImage() {
        if (!activeSession || !activeDocument.loaded || tabModel.anySaving
                || !appRibbon.headerTabActive || !prepareActiveWorkspace())
            return
        const workspace = activeWorkspace()
        if (workspace)
            workspace.openHeaderImagePanel()
    }

    function requestHeaderImageFile(placement) {
        const workspace = activeWorkspace()
        if (!workspace || workspace.headerImagePlacement !== placement
                || !placement.choosing || !appRibbon.headerTabActive
                || tabModel.anySaving)
            return
        headerImagePickerPlacement = placement
        headerImagePickerSession = activeSession
        headerImageDialog.open()
    }

    function cancelHeaderImageFile() {
        const placement = headerImagePickerPlacement
        headerImagePickerPlacement = null
        headerImagePickerSession = null
        headerImageDialog.close()
        if (placement)
            placement.cancel()
    }

    function insertSelectedHeaderImage(source) {
        const placement = headerImagePickerPlacement
        const session = headerImagePickerSession
        headerImagePickerPlacement = null
        headerImagePickerSession = null
        const workspace = activeWorkspace()
        if (placement && session === activeSession && workspace
                && workspace.headerImagePlacement === placement
                && placement.choosing && appRibbon.headerTabActive
                && !tabModel.anySaving)
            placement.acceptImage(source)
        else if (placement)
            placement.cancel()
    }

    Connections {
        target: window.headerImagePickerPlacement
        function onChoosingChanged() {
            if (window.headerImagePickerPlacement
                    && !window.headerImagePickerPlacement.choosing)
                window.cancelHeaderImageFile()
        }
    }

    function requestSetPassword() {
        if (!activeSession || !activeDocument.loaded || tabModel.anySaving
                || !appRibbon.protectTabActive || !activeEditController.canSetOpenPassword
                || !prepareActiveWorkspace())
            return
        passwordSetupSession = activeSession
        setPasswordDialogLoader.active = true
    }

    function cancelPasswordSetup() {
        setPasswordDialogLoader.active = false
        passwordSetupSession = null
    }

    function requestWatermark(mode) {
        if (!activeSession || !activeDocument.loaded || tabModel.anySaving
                || !appRibbon.protectTabActive
                || !prepareActiveWorkspace())
            return
        cancelWatermark()
        watermarkSession = activeSession
        watermarkMode = mode
        watermarkDialogLoader.active = true
        watermarkSession.editController.loadWatermarks()
    }

    function cancelWatermark() {
        watermarkDialogLoader.active = false
        watermarkSession = null
    }

    function requestSignature(mode) {
        if (!activeSession || !activeDocument.loaded || tabModel.anySaving
                || !appRibbon.signTabActive || !prepareActiveWorkspace())
            return
        cancelSignature()
        activeEditController.tool = PDFEditController.SelectTool
        signatureSession = activeSession
        signatureMode = mode
        signatureDialogLoader.active = true
    }

    function armSignaturePlacement(widthPoints) {
        if (signatureSession !== activeSession || !activeDocument.loaded
                || tabModel.anySaving || !appRibbon.signTabActive
                || !signatureLibrary.hasPreview) {
            cancelSignature()
            return
        }
        const workspace = activeWorkspace()
        if (workspace)
            workspace.activePaneIndex = 0
        signatureWidthPoints = widthPoints
        signaturePlacementPending = true
        activeEditController.tool = PDFEditController.AddImageTool
        //% "Click the page to place your signature or mark. Press Escape to cancel."
        showStatus(qsTrId("ESignClickToPlace"))
    }

    function cancelSignature() {
        const session = signatureSession
        signaturePlacementPending = false
        signatureSession = null
        signatureDialogLoader.active = false
        if (session && session.editController.tool === PDFEditController.AddImageTool)
            session.editController.tool = PDFEditController.SelectTool
        signatureLibrary.clearPreview()
    }

    function clearImageInsertRequest() {
        imageInsertController = null
        imageInsertPage = -1
        imageInsertX = 0
        imageInsertY = 0
    }

    function insertSelectedImage(source) {
        const controller = imageInsertController
        const page = imageInsertPage
        const x = imageInsertX
        const y = imageInsertY
        clearImageInsertRequest()
        if (controller)
            controller.addImage(page, x, y, source)
    }

    function saveFileAs(url) {
        const session = saveAsSession
        saveAsSession = null
        if (session) {
            if (session === activeSession
                    && !prepareActiveWorkspace()) {
                return
            }
            saveAsDialog.trackWrite(session, url)
            if (!tabModel.requestSaveAs(session, url))
                saveAsDialog.finishWrite(session, false, url)
        }
    }

    function requestDeletePage(page) {
        if (!activeSession
            || tabModel.anySaving
            || page < 0
            || page >= activeDocument.pageCount
            || activeDocument.pageCount <= 1
            || !commitActivePanelEdits()) {
            return
        }

        deletePageSession = activeSession
        deletePageIndex = page
        deletePagePopup.open()
    }

    function confirmDeletePage() {
        const session = deletePageSession
        const page = deletePageIndex
        deletePagePopup.close()
        if (!session
            || session !== activeSession
            || session.saving
            || page < 0
            || page >= session.document.pageCount
            || session.document.pageCount <= 1
            || !commitActivePanelEdits()) {
            return
        }

        session.document.currentPage = page
        const nextPage = session.editController.deleteCurrentPage()
        if (nextPage >= 0)
            goToPage(nextPage)
    }

    function clearExternalLinkPrompt() {
        externalLinkSession = null
        externalLinkRequestId = 0
        externalLinkAuthority = ""
        externalLinkDisplayUrl = ""
        externalLinkInsecureHttp = false
    }

    function cancelExternalLinkPrompt() {
        const session = externalLinkSession
        const requestId = externalLinkRequestId
        clearExternalLinkPrompt()
        if (externalLinkPopup.opened)
            externalLinkPopup.close()
        if (session && Number(requestId) !== 0)
            session.formController.cancelExternalLink(requestId)
    }

    function requestExternalLink(session, requestId, authority,
                                 displayUrl, insecureHttp) {
        if (!session
            || session !== activeSession
            || Number(requestId) === 0
            || Number(externalLinkRequestId) !== 0) {
            if (session && Number(requestId) !== 0)
                session.formController.cancelExternalLink(requestId)
            return
        }

        externalLinkSession = session
        externalLinkRequestId = requestId
        externalLinkAuthority = authority
        externalLinkDisplayUrl = displayUrl
        externalLinkInsecureHttp = insecureHttp
        externalLinkPopup.open()
    }

    function finishExternalLinkRequest(session, requestId) {
        if (session !== externalLinkSession
            || Number(requestId) !== Number(externalLinkRequestId)) {
            return
        }

        clearExternalLinkPrompt()
        if (externalLinkPopup.opened)
            externalLinkPopup.close()
    }

    function confirmExternalLink() {
        const session = externalLinkSession
        const requestId = externalLinkRequestId
        clearExternalLinkPrompt()
        externalLinkPopup.close()
        if (!session
            || session !== activeSession
            || !session.document.loaded
            || Number(requestId) === 0) {
            if (session && Number(requestId) !== 0)
                session.formController.cancelExternalLink(requestId)
            return
        }

        if (!session.formController.confirmExternalLink(requestId)) {
            //% "The link could not be opened in your default browser."
            showStatus(qsTrId("ExternalLinkOpenFailed"))
        }
    }

    function beginDraftPrompt(action, session) {
        pendingDraftAction = action
        pendingSession = session
        discardDraftPopup.open()
    }

    function clearDraftPrompt() {
        pendingDraftAction = ""
        pendingSession = null
        discardDraftPopup.close()
    }

    function cancelDraftPrompt() {
        if (pendingSession && pendingSession.saving)
            return
        clearDraftPrompt()
    }

    function requestCloseSession(session) {
        if (!session || tabModel.anySaving) {
            if (tabModel.anySaving) {
                //% "Please wait for the PDF to finish saving."
                showStatus(qsTrId("WaitForPdfSave"))
            }
            return
        }

        if (session === activeSession) {
            if (!prepareActiveWorkspace())
                return
        } else {
            session.prepareForDeactivation()
        }

        if (session.hasDrafts) {
            beginDraftPrompt("close", session)
        } else {
            tabModel.closeSession(session)
        }
    }

    function discardPendingChanges() {
        const session = pendingSession
        const action = pendingDraftAction
        if (!session || session.saving)
            return

        session.editController.discardDrafts()
        clearDraftPrompt()
        if (action === "close") {
            tabModel.closeSession(session)
        } else if (action === "quit") {
            Qt.callLater(window.continueQuitSequence)
        }
    }

    function savePendingChanges() {
        if (pendingSession && !pendingSession.saving)
            tabModel.requestSave(pendingSession)
    }

    function finishPendingDraftAction(session, succeeded) {
        if (session !== pendingSession || !succeeded)
            return

        const action = pendingDraftAction
        clearDraftPrompt()
        if (action === "close") {
            tabModel.closeSession(session)
        } else if (action === "quit") {
            Qt.callLater(window.continueQuitSequence)
        }
    }

    function continueQuitSequence() {
        if (tabModel.anySaving) {
            //% "Please wait for the PDF to finish saving."
            showStatus(qsTrId("WaitForPdfSave"))
            return
        }

        const draftSession = tabModel.firstDraftSession()
        if (draftSession) {
            beginDraftPrompt("quit", draftSession)
            return
        }

        allowWindowClose = true
        Qt.callLater(function() { window.close() })
    }

    function showComingSoon(toolName) {
        //% "%1 is coming soon"
        showStatus(qsTrId("FeatureComingSoon").arg(toolName))
    }

    function showStatus(message) {
        transientStatus = message
        transientStatusTimer.restart()
    }

    onActiveSessionChanged: {
        cancelHeaderImageFile()
        cancelSignature()
        cancelPasswordSetup()
        cancelWatermark()
        if (pendingSearchNavigationSession
                && pendingSearchNavigationSession !== activeSession) {
            clearPendingSearchNavigation()
        }
        if (Number(externalLinkRequestId) !== 0
            && externalLinkSession !== activeSession) {
            cancelExternalLinkPrompt()
        }
        if (!openingFiles)
            rebuildWorkspace()
    }

    onClosing: close => {
        if (allowWindowClose)
            return

        close.accepted = false
        if (!prepareActiveWorkspace())
            return
        tabModel.finishAllEdits()
        if (tabModel.anySaving) {
            //% "Please wait for the PDF to finish saving."
            showStatus(qsTrId("WaitForPdfSave"))
        } else {
            continueQuitSequence()
        }
    }

    AppTheme {
        id: appTheme
    }

    Timer {
        id: transientStatusTimer

        interval: 2600
        onTriggered: window.transientStatus = ""
    }

    PDFDocument {
        id: emptyDocument
    }

    PDFEditController {
        id: emptyEditController

        document: emptyDocument
    }

    PDFSearchController {
        id: emptySearchController

        document: emptyDocument
    }

    PDFFormController {
        id: emptyFormController

        document: emptyDocument
    }

    PDFFormDesignerController {
        id: emptyFormDesignerController
    }

    PDFTabModel {
        id: tabModel
        objectName: "pdfTabModel"

        onStatusMessage: message => window.showStatus(message)
        onAnySavingChanged: {
            if (anySaving) {
                window.cancelPasswordSetup()
                window.cancelWatermark()
            }
        }
        onPasswordRequested: (source, incorrectPassword) => {
            window.requestPassword(source, incorrectPassword)
        }
        onSessionSaveFinished: (session, succeeded) => {
            saveAsDialog.finishWrite(session, succeeded, session.document.source)
            window.finishPendingDraftAction(session, succeeded)
        }
        onFontDownloadRequested: session => {
            window.fontDownloadController = session.editController
        }
        onExternalLinkRequested:
            (session, requestId, authority, displayUrl, insecureHttp) => {
                window.requestExternalLink(
                    session,
                    requestId,
                    authority,
                    displayUrl,
                    insecureHttp)
            }
        onExternalLinkRequestCancelled: (session, requestId) => {
            window.finishExternalLinkRequest(session, requestId)
        }
    }

    Connections {
        target: window.activeEditController

        function onToolChanged() {
            if (window.signaturePlacementPending
                    && window.activeEditController.tool !== PDFEditController.AddImageTool)
                window.cancelSignature()
        }

        function onHistoryStateChanged() {
            if (window.signaturePlacementPending)
                window.cancelSignature()
        }

        function onSavingChanged() {
            if (window.activeEditController.saving)
                window.cancelSignature()
        }

        function onImageFileRequested(page, x, y) {
            window.requestImageFile(
                window.activeEditController,
                page,
                x,
                y)
        }
    }

    SignatureLibrary {
        id: signatureLibrary
        objectName: "signatureLibrary"
    }

    Connections {
        target: window.signatureSession ? window.signatureSession.document : null
        function onSourceChanged() { window.cancelSignature() }
        function onPageStructureChanged() { window.cancelSignature() }
        function onLoadedChanged() { window.cancelSignature() }
    }

    Loader {
        id: signatureDialogLoader
        objectName: "signatureDialogLoader"
        active: false
        sourceComponent: SignatureDialog {
            id: signatureDialog
            theme: appTheme
            library: signatureLibrary
            mode: window.signatureMode
            parent: Overlay.overlay
            onPlacementRequested: widthPoints => window.armSignaturePlacement(widthPoints)
            onRejected: window.cancelSignature()
            onClosed: {
                if (signatureDialogLoader.item !== signatureDialog)
                    return
                signatureDialogLoader.active = false
                if (!window.signaturePlacementPending)
                    window.cancelSignature()
            }
        }
        onLoaded: {
            const dialog = item as SignatureDialog
            if (dialog)
                dialog.open()
        }
    }

    Connections {
        target: window.passwordSetupSession ? window.passwordSetupSession.document : null
        function onSourceChanged() { window.cancelPasswordSetup() }
        function onLoadedChanged() { window.cancelPasswordSetup() }
    }

    Loader {
        id: setPasswordDialogLoader
        objectName: "setPasswordDialogLoader"
        active: false
        sourceComponent: SetPasswordDialog {
            theme: appTheme
            editController: window.passwordSetupSession
                ? window.passwordSetupSession.editController : null
            onClosed: window.cancelPasswordSetup()
        }
        onLoaded: {
            if (status === Loader.Ready) {
                const dialog = item as SetPasswordDialog
                if (dialog)
                    dialog.open()
            }
        }
    }

    Connections {
        target: window.watermarkSession ? window.watermarkSession.document : null
        function onSourceChanged() { window.cancelWatermark() }
        function onLoadedChanged() { window.cancelWatermark() }
        function onPageStructureChanged() { window.cancelWatermark() }
    }

    Loader {
        id: watermarkDialogLoader
        objectName: "watermarkDialogLoader"
        active: false
        sourceComponent: WatermarkDialog {
            theme: appTheme
            editController: window.watermarkSession
                ? window.watermarkSession.editController : null
            mode: window.watermarkMode
            operationAllowed: window.watermarkSession !== null
                && window.watermarkSession === window.activeSession
                && tabModel.indexOf(window.watermarkSession) >= 0
                && window.watermarkSession.document.loaded
                && !window.watermarkSession.saving
                && !tabModel.anySaving && appRibbon.protectTabActive
            onClosed: window.cancelWatermark()
        }
        onLoaded: {
            if (status === Loader.Ready) {
                const dialog = item as WatermarkDialog
                if (dialog)
                    dialog.open()
            }
        }
    }

    Shortcut {
        objectName: "openPdfShortcut"
        sequences: [StandardKey.Open]
        autoRepeat: false
        enabled: !window.fileOpenBlocked
        onActivated: window.requestOpen()
    }

    Shortcut {
        objectName: "findPdfShortcut"
        sequences: [StandardKey.Find]
        autoRepeat: false
        enabled: window.activeDocument.loaded && !window.fileOpenBlocked
        onActivated: window.requestFind()
    }

    Shortcut {
        sequence: "Escape"
        enabled: window.signaturePlacementPending
        onActivated: window.cancelSignature()
    }

    Shortcut {
        sequence: "Escape"
        enabled: window.activeWorkspaceItem !== null
            && (window.activeWorkspaceItem.headerTextPlacement.armed
                || window.activeWorkspaceItem.headerTextPlacement.active
                || window.activeWorkspaceItem.headerImagePlacement.armed
                || window.activeWorkspaceItem.headerImagePlacement.active)
        onActivated: window.activeWorkspaceItem.closeHeaderPlacement()
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: window.activeSession !== null
            && window.activeEditController.canUndo
            && !(window.activeWorkspaceItem
                && (window.activeWorkspaceItem.headerTextInputFocused
                    || window.activeWorkspaceItem.headerImageInputFocused))
            && !window.activeEditController.textEditActive
            && !window.activeFormDesignerController.textFieldFocused
            && !appRibbon.formsTabActive
            && !tabModel.anySaving
        onActivated: window.requestUndo()
    }

    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: window.activeSession !== null
            && window.activeEditController.canRedo
            && !(window.activeWorkspaceItem
                && (window.activeWorkspaceItem.headerTextInputFocused
                    || window.activeWorkspaceItem.headerImageInputFocused))
            && !window.activeWorkspaceHasPendingEdits
            && !window.activeEditController.textEditActive
            && !window.activeFormDesignerController.textFieldFocused
            && !appRibbon.formsTabActive
            && !tabModel.anySaving
        onActivated: window.requestRedo()
    }

    Shortcut {
        sequences: [StandardKey.Save]
        enabled: window.activeDocument.loaded
            && (window.activeEditController.hasDrafts
                || window.activeWorkspaceHasPendingEdits)
            && !tabModel.anySaving
        onActivated: window.requestSave()
    }

    Shortcut {
        sequences: [StandardKey.SaveAs]
        enabled: window.activeDocument.loaded && !tabModel.anySaving
        onActivated: window.requestSaveAs()
    }

    Shortcut {
        sequences: [StandardKey.Close]
        enabled: window.activeSession !== null && !tabModel.anySaving
        onActivated: window.requestCloseSession(window.activeSession)
    }

    Shortcut {
        sequence: "Escape"
        enabled: discardDraftPopup.opened
            && (!window.pendingSession || !window.pendingSession.saving)
        onActivated: window.cancelDraftPrompt()
    }

    PlatformFileDialog {
        id: openDialog
        objectName: "openPdfDialog"

        //% "Open PDFs"
        title: qsTrId("OpenPdfs")
        fileMode: FileDialog.OpenFiles
        //% "PDF files (*.pdf)"
        nameFilters: [qsTrId("PdfFileFilter")]
        onAccepted: window.openFiles(selectedFiles)
    }

    Loader {
        id: passwordPromptLoader
        active: window.passwordRequests.length > 0
        sourceComponent: Popup {
            id: passwordPopup
            objectName: "pdfPasswordPopup"
            parent: Overlay.overlay
            x: Math.round((window.width - width) / 2)
            y: Math.max(windowMetrics.topInset,
                Math.round((window.height - windowMetrics.keyboardInset - height) / 2))
            width: Math.min(440, window.width - 32)
            padding: 20
            modal: true
            focus: true
            visible: true
            z: 200
            closePolicy: Popup.CloseOnEscape
            readonly property var request: window.passwordRequests.length > 0
                ? window.passwordRequests[0] : null
            readonly property string fileName: request
                ? decodeURIComponent(request.source.toString().split("/").pop()) : ""

            function submit() {
                if (passwordInput.text.length === 0 || tabModel.anySaving)
                    return
                const password = passwordInput.text
                passwordInput.clear()
                window.submitPassword(password)
            }

            onOpened: passwordInput.forceActiveFocus()
            onRequestChanged: {
                passwordInput.clear()
                passwordInput.forceActiveFocus()
            }
            onClosed: {
                if (window.passwordRequests.length > 0) {
                    window.finishPasswordRequest()
                    if (window.passwordRequests.length > 0)
                        open()
                }
            }

            background: Rectangle {
                radius: 6
                color: "#fffdfb"
                border.color: appTheme.ribbonBorder
            }

            contentItem: ColumnLayout {
                spacing: 16
                Label {
                    Layout.fillWidth: true
                    //% "Enter PDF password"
                    text: qsTrId("EnterPdfPasswordTitle")
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    color: appTheme.ribbonTextColor
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    //% "Enter the password to open “%1”."
                    text: qsTrId("EnterPdfPasswordForFile").arg(passwordPopup.fileName)
                    textFormat: Text.PlainText
                    color: appTheme.ribbonTextColor
                    wrapMode: Text.WrapAnywhere
                }
                TextField {
                    id: passwordInput
                    objectName: "pdfPasswordInput"
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                        | Qt.ImhNoAutoUppercase
                    selectByMouse: true
                    maximumLength: 1024
                    //% "Password"
                    placeholderText: qsTrId("PdfPasswordPlaceholder")
                    Accessible.name: placeholderText
                    onAccepted: passwordPopup.submit()
                }
                Label {
                    objectName: "pdfPasswordError"
                    Layout.fillWidth: true
                    visible: passwordPopup.request !== null
                        && passwordPopup.request.incorrect
                    //% "Incorrect password. Please try again."
                    text: qsTrId("IncorrectPdfPassword")
                    textFormat: Text.PlainText
                    color: "#b3261e"
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { Layout.fillWidth: true }
                    Button {
                        objectName: "cancelPdfPasswordButton"
                        //% "Cancel"
                        text: qsTrId("Cancel")
                        onClicked: passwordPopup.close()
                    }
                    OrangeButton {
                        objectName: "openPasswordPdfButton"
                        theme: appTheme
                        //% "Open"
                        text: qsTrId("OpenPasswordPdf")
                        enabled: passwordInput.text.length > 0 && !tabModel.anySaving
                        onClicked: passwordPopup.submit()
                    }
                }
            }
        }
    }

    Connections {
        target: window.fontDownloadController
        function onFontDownloadStateChanged() {
            if (window.fontDownloadController
                    && !window.fontDownloadController.fontDownloadPending)
                window.fontDownloadController = null
        }
    }

    Loader {
        active: window.fontDownloadController !== null
            && window.fontDownloadController.fontDownloadPending
        sourceComponent: Popup {
            id: fontDownloadPopup
            objectName: "fontDownloadPopup"
            parent: Overlay.overlay
            x: Math.round((window.width - width) / 2)
            y: Math.max(windowMetrics.topInset,
                Math.round((window.height - windowMetrics.keyboardInset - height) / 2))
            width: Math.min(480, window.width - 32)
            padding: 20
            modal: true
            focus: true
            visible: true
            z: 200
            closePolicy: Popup.NoAutoClose
            readonly property PDFEditController controller: window.fontDownloadController
            readonly property bool downloading: controller ? controller.downloadingFont : false
            readonly property bool forEdit: controller ? controller.fontDownloadForEdit : false
            readonly property string fontFamilies: controller
                ? controller.missingFontFamilies.join("\n") : ""

            Shortcut {
                sequence: "Escape"
                enabled: fontDownloadPopup.visible && fontDownloadPopup.forEdit
                onActivated: {
                    if (fontDownloadPopup.controller)
                        fontDownloadPopup.controller.cancelTextEditPreparation()
                }
            }

            background: Rectangle {
                radius: 6
                color: "#fffdfb"
                border.color: appTheme.ribbonBorder
            }
            contentItem: ColumnLayout {
                spacing: 16
                Label {
                    Layout.fillWidth: true
                    text: fontDownloadPopup.forEdit
                        //% "Prepare fonts for editing"
                        ? qsTrId("DownloadEditFontsTitle")
                        //% "Download font family"
                        : qsTrId("DownloadMissingFontTitle")
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    color: appTheme.ribbonTextColor
                    wrapMode: Text.Wrap
                }
                ScrollView {
                    id: fontDownloadMessageScroll
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(
                        fontDownloadMessage.implicitHeight,
                        Math.max(64, window.height - 260))
                    contentWidth: availableWidth
                    clip: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    Label {
                        id: fontDownloadMessage
                        width: fontDownloadMessageScroll.availableWidth
                        text: fontDownloadPopup.forEdit
                            ? (fontDownloadPopup.downloading
                                //% "Downloading fonts. Text editing will open automatically when the downloads finish."
                                ? qsTrId("EditFontDownloadInProgress")
                                //% "Download the following font families before editing this text?"
                                : qsTrId("EditFontDownloadConfirmation"))
                                + "\n\n" + fontDownloadPopup.fontFamilies
                            : fontDownloadPopup.downloading
                                //% "Downloading the available regular, bold, italic and bold italic styles of %1. Saving will continue automatically when all downloads finish."
                                ? qsTrId("FontDownloadInProgress").arg(
                                    fontDownloadPopup.controller ? fontDownloadPopup.controller.missingFontFamily : "")
                                //% "The application does not have the complete %1 font. Would you like it to download all available regular, bold, italic and bold italic styles of this family and continue saving?"
                                : qsTrId("FontDownloadConfirmation").arg(
                                    fontDownloadPopup.controller ? fontDownloadPopup.controller.missingFontFamily : "")
                        textFormat: Text.PlainText
                        color: appTheme.ribbonTextColor
                        wrapMode: Text.Wrap
                    }
                }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: fontDownloadPopup.downloading
                    indeterminate: fontDownloadPopup.downloading
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { Layout.fillWidth: true }
                    Button {
                        objectName: "cancelFontDownloadButton"
                        //% "Cancel"
                        text: qsTrId("Cancel")
                        onClicked: {
                            if (fontDownloadPopup.controller)
                                fontDownloadPopup.controller.cancelFontDownload()
                        }
                    }
                    OrangeButton {
                        objectName: "confirmFontDownloadButton"
                        theme: appTheme
                        //% "OK"
                        text: qsTrId("ConfirmFontDownload")
                        visible: !fontDownloadPopup.downloading
                        onClicked: {
                            if (fontDownloadPopup.controller)
                                fontDownloadPopup.controller.respondToFontDownload(true)
                        }
                    }
                }
            }
        }
    }

    PlatformFileDialog {
        id: imageDialog

        //% "Select Image"
        title: qsTrId("SelectImage")
        fileMode: FileDialog.OpenFile
        //% "Image files (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)"
        nameFilters: [qsTrId("ImageFileFilter")]
        onAccepted: window.insertSelectedImage(selectedFile)
        onRejected: window.clearImageInsertRequest()
    }

    PlatformFileDialog {
        id: headerImageDialog
        objectName: "headerImageFileDialog"
        //% "Select Image"
        title: qsTrId("SelectImage")
        fileMode: FileDialog.OpenFile
        //% "Image files (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)"
        nameFilters: [qsTrId("ImageFileFilter")]
        onAccepted: window.insertSelectedHeaderImage(selectedFile)
        onRejected: window.cancelHeaderImageFile()
    }

    PlatformFileDialog {
        id: saveAsDialog

        //% "Save PDF As"
        title: qsTrId("SavePdfAs")
        fileMode: FileDialog.SaveFile
        //% "PDF files (*.pdf)"
        nameFilters: [qsTrId("PdfFileFilter")]
        defaultSuffix: "pdf"
        onAccepted: window.saveFileAs(selectedFile)
        onRejected: window.saveAsSession = null
    }

    PlatformFileDialog {
        id: excelExportDialog

        //% "Export to Excel"
        title: qsTrId("ExportExcel")
        fileMode: FileDialog.SaveFile
        //% "Excel workbooks (*.xlsx)"
        nameFilters: [qsTrId("ExcelFileFilter")]
        defaultSuffix: "xlsx"
        onAccepted: window.exportExcelTo(selectedFile)
        onRejected: window.excelExportSession = null
    }

    Popup {
        id: externalLinkPopup

        x: Math.round((window.width - width) / 2)
        y: Math.max(windowMetrics.topInset,
                Math.round((window.height - windowMetrics.keyboardInset - height) / 2))
        width: Math.min(520, window.width - 32)
        height: externalLinkContent.implicitHeight + 32
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        onOpened: externalLinkCancelButton.forceActiveFocus()
        onClosed: {
            if (Number(window.externalLinkRequestId) !== 0)
                window.cancelExternalLinkPrompt()
        }

        background: Rectangle {
            radius: 6
            color: "#fffdfb"
            border.color: appTheme.ribbonBorder
            border.width: 1
        }

        contentItem: ColumnLayout {
            id: externalLinkContent

            spacing: 14

            Label {
                Layout.fillWidth: true
                //% "Open link to %1?"
                text: qsTrId("OpenExternalLinkQuestion").arg(
                    window.externalLinkAuthority)
                textFormat: Text.PlainText
                color: appTheme.ribbonTextColor
                font.pixelSize: 18
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                //% "“%1” wants to open this address in your default browser."
                text: qsTrId("ExternalLinkDocumentWantsToOpen").arg(
                    window.externalLinkSession
                        ? window.externalLinkSession.title
                        : "")
                textFormat: Text.PlainText
                color: "#656268"
                wrapMode: Text.Wrap
            }

            ScrollView {
                id: externalLinkUrlScroll

                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(
                    104,
                    Math.max(52, externalLinkUrlText.implicitHeight))
                clip: true

                TextArea {
                    id: externalLinkUrlText

                    width: externalLinkUrlScroll.availableWidth
                    text: window.externalLinkDisplayUrl
                    textFormat: TextEdit.PlainText
                    color: appTheme.ribbonTextColor
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    padding: 10

                    background: Rectangle {
                        color: "#f7f3ef"
                        border.color: appTheme.ribbonBorder
                        border.width: 1
                        radius: 4
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: window.externalLinkInsecureHttp
                //% "This link uses HTTP, so the connection may not be secure."
                text: qsTrId("InsecureHttpLinkWarning")
                textFormat: Text.PlainText
                color: "#9a4d16"
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item {
                    Layout.fillWidth: true
                }

                Button {
                    id: externalLinkCancelButton

                    //% "Cancel"
                    text: qsTrId("Cancel")
                    onClicked: window.cancelExternalLinkPrompt()
                }

                OrangeButton {
                    Layout.preferredWidth: 148
                    theme: appTheme
                    //% "Open in Browser"
                    text: qsTrId("OpenInBrowser")
                    onClicked: window.confirmExternalLink()
                }
            }
        }
    }

    Popup {
        id: discardDraftPopup

        x: Math.round((window.width - width) / 2)
        y: Math.max(windowMetrics.topInset,
                Math.round((window.height - windowMetrics.keyboardInset - height) / 2))
        width: Math.min(420, window.width - 32)
        height: discardDraftContent.implicitHeight + 32
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose

        background: Rectangle {
            radius: 6
            color: "#fffdfb"
            border.color: appTheme.ribbonBorder
            border.width: 1
        }

        contentItem: ColumnLayout {
            id: discardDraftContent

            spacing: 14

            Label {
                Layout.fillWidth: true
                //% "Save changes to “%1”?"
                text: qsTrId("SaveChangesToDocumentQuestion").arg(
                    window.pendingSession ? window.pendingSession.title : "")
                color: appTheme.ribbonTextColor
                font.pixelSize: 18
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            Label {
                Layout.fillWidth: true
                //% "Your edits exist only in this session and have not been saved to the PDF."
                text: qsTrId("UnsavedEditsDescription")
                color: "#656268"
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item {
                    Layout.fillWidth: true
                }

                Button {
                    //% "Cancel"
                    text: qsTrId("Cancel")
                    enabled: !window.pendingSession
                        || !window.pendingSession.saving
                    onClicked: window.cancelDraftPrompt()
                }

                Button {
                    //% "Save"
                    text: qsTrId("Save")
                    enabled: window.pendingSession !== null
                        && !window.pendingSession.saving
                    onClicked: window.savePendingChanges()
                }

                OrangeButton {
                    Layout.preferredWidth: 104
                    theme: appTheme
                    //% "Discard"
                    text: qsTrId("Discard")
                    enabled: window.pendingSession !== null
                        && !window.pendingSession.saving
                    onClicked: window.discardPendingChanges()
                }
            }
        }
    }

    Popup {
        id: deletePagePopup

        x: Math.round((window.width - width) / 2)
        y: Math.max(windowMetrics.topInset,
                Math.round((window.height - windowMetrics.keyboardInset - height) / 2))
        width: Math.min(400, window.width - 32)
        height: deletePageContent.implicitHeight + 32
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        onClosed: {
            window.deletePageSession = null
            window.deletePageIndex = -1
        }

        background: Rectangle {
            radius: 6
            color: "#fffdfb"
            border.color: appTheme.ribbonBorder
            border.width: 1
        }

        contentItem: ColumnLayout {
            id: deletePageContent

            spacing: 18

            Label {
                Layout.fillWidth: true
                //% "Do you want to delete page %1?"
                text: qsTrId("ConfirmDeletePageQuestion").arg(
                    window.deletePageIndex + 1)
                color: appTheme.ribbonTextColor
                font.pixelSize: 18
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item {
                    Layout.fillWidth: true
                }

                Button {
                    //% "Cancel"
                    text: qsTrId("Cancel")
                    onClicked: deletePagePopup.close()
                }

                OrangeButton {
                    Layout.preferredWidth: 104
                    theme: appTheme
                    //% "Delete"
                    text: qsTrId("Delete")
                    onClicked: window.confirmDeletePage()
                }
            }
        }
    }

    header: Column {
        width: window.width
        topPadding: windowMetrics.topInset
        leftPadding: windowMetrics.leftInset
        rightPadding: windowMetrics.rightInset

        AppRibbon {
            id: appRibbon
            objectName: "appRibbon"

            width: parent.width - parent.leftPadding - parent.rightPadding
            theme: appTheme
            document: window.activeDocument
            editController: window.activeEditController
            searchController: window.activeSearchController
            session: window.activeSession
            anySaving: tabModel.anySaving
            hasPendingPanelEdits: window.activeWorkspaceHasPendingEdits
            viewNavigationPage: window.activeWorkspaceItem
                ? window.activeWorkspaceItem.activePage
                : window.activeDocument.currentPage
            viewCanNavigateBackward: window.activeWorkspaceItem
                ? window.activeWorkspaceItem.canNavigateBackward
                : window.activeDocument.currentPage > 0
            viewCanNavigateForward: window.activeWorkspaceItem
                ? window.activeWorkspaceItem.canNavigateForward
                : window.activeDocument.loaded
                    && window.activeDocument.currentPage + 1
                        < window.activeDocument.pageCount
            onOpenRequested: window.requestOpen()
            onSaveRequested: window.requestSave()
            onSaveAsRequested: window.requestSaveAs()
            onExcelExportRequested: window.requestExcelExport()
            onPageRequested: page => window.goToPage(page)
            onPageStepRequested: direction => {
                const workspace = window.activeWorkspace()
                if (workspace)
                    workspace.navigateByStep(direction)
                else if (window.activeDocument.loaded)
                    window.goToPage(window.activeDocument.currentPage + direction)
            }
            onSearchResultRequested: (page,
                                      normalizedX,
                                      normalizedY,
                                      resultsRevision) =>
                window.goToSearchResult(
                    page,
                    normalizedX,
                    normalizedY,
                    resultsRevision)
            onDeletePageRequested: page => window.requestDeletePage(page)
            onZoomOutRequested: {
                window.zoomBy(1 / 1.15)
            }
            onZoomInRequested: {
                window.zoomBy(1.15)
            }
            onFitWidthRequested: window.fitToWidth()
            onRotateRequested: quarterTurns => window.rotateView(quarterTurns)
            onPageDisplayModeRequested: mode =>
                window.setPageDisplayMode(mode)
            onSeparateCoverPageToggleRequested: {
                const workspace = window.activeWorkspace()
                if (workspace) {
                    workspace.toggleSeparateCoverPage()
                } else if (window.activeSession) {
                    const mode = window.activeSession.pageDisplayMode
                    const facingMode = mode
                            === PDFDocumentSession.FacingMode
                        || mode
                            === PDFDocumentSession.ContinuousFacingMode
                    if (facingMode) {
                        window.activeSession.separateCoverPage
                            = !window.activeSession.separateCoverPage
                    }
                }
            }
            onSplitViewModeRequested: mode => {
                const workspace = window.activeWorkspace()
                if (workspace)
                    workspace.setSplitViewMode(mode)
                else if (window.activeSession)
                    window.activeSession.splitViewMode = mode
            }
            onSplitViewToggleRequested: {
                const workspace = window.activeWorkspace()
                if (workspace) {
                    workspace.toggleSplitView()
                } else if (window.activeSession) {
                    window.activeSession.splitViewEnabled
                        = !window.activeSession.splitViewEnabled
                }
            }
            onToolRequested: toolName => window.showComingSoon(toolName)
            onSignatureRequested: mode => window.requestSignature(mode)
            onSetPasswordRequested: window.requestSetPassword()
            onWatermarkRequested: mode => window.requestWatermark(mode)
            onHeaderTextRequested: window.requestHeaderText()
            onHeaderImageRequested: window.requestHeaderImage()
            onProtectTabActiveChanged: {
                if (!protectTabActive) {
                    window.cancelPasswordSetup()
                    window.cancelWatermark()
                }
            }
            onSignTabActiveChanged: {
                if (!signTabActive)
                    window.cancelSignature()
            }
            onUndoRequested: window.requestUndo()
            onRedoRequested: window.requestRedo()
        }

        DocumentTabBar {
            width: parent.width - parent.leftPadding - parent.rightPadding
            theme: appTheme
            tabModel: tabModel
            enabled: !tabModel.anySaving
            onTabRequested: index => window.switchToTab(index)
            onCloseRequested: session => window.requestCloseSession(session)
            onOpenRequested: window.requestOpen()
        }
    }

    footer: ReaderControlBar {
        id: readerControlBar
        bottomPadding: 4 + windowMetrics.bottomInset
        leftPadding: 6 + windowMetrics.leftInset
        rightPadding: 6 + windowMetrics.rightInset
        visible: windowMetrics.keyboardInset === 0

        theme: appTheme
        documentLoaded: window.activeDocument.loaded
        navigationPage: window.activeWorkspaceItem
            ? window.activeWorkspaceItem.activePage
            : window.activeDocument.currentPage
        pageCount: window.activeDocument.pageCount
        canNavigateBackward: window.activeWorkspaceItem
            ? window.activeWorkspaceItem.canNavigateBackward
            : window.activeDocument.currentPage > 0
        canNavigateForward: window.activeWorkspaceItem
            ? window.activeWorkspaceItem.canNavigateForward
            : window.activeDocument.loaded
                && window.activeDocument.currentPage + 1
                    < window.activeDocument.pageCount
        zoom: window.activeWorkspaceItem
            ? window.activeWorkspaceItem.activeZoom
            : window.activeSession
                ? window.activeSession.zoom
                : 1.0
        pageDisplayMode: window.activeSession
            ? window.activeSession.pageDisplayMode
            : PDFDocumentSession.ContinuousMode
        viewReady: window.activeWorkspaceItem
            ? window.activeWorkspaceItem.initialViewReady
            : !workspaceLoader.active && window.activeDocument.loaded
        viewContextOwner: window.activeSession
        viewContextPane: window.activeWorkspaceItem
            ? window.activeWorkspaceItem.activePaneIndex
            : 0
        enabled: !tabModel.anySaving
        onPageRequested: page => window.goToActivePage(page)
        onPageStepRequested: direction => {
            const workspace = window.activeWorkspace()
            if (workspace) {
                workspace.navigateByStep(direction)
            } else if (window.activeDocument.loaded) {
                window.goToActivePage(
                    window.activeDocument.currentPage + direction)
            }
        }
        onZoomRequested: value => window.setZoom(value)
        onPageDisplayModeRequested: mode =>
            window.setPageDisplayMode(mode)
        onViewRestoreRequested: (page, restoredZoom) => {
            const session = window.activeSession
            const pane = window.activeWorkspaceItem
                ? window.activeWorkspaceItem.activePaneIndex
                : 0
            window.setZoom(restoredZoom)
            Qt.callLater(function() {
                const workspace = window.activeWorkspace()
                const activePane = workspace ? workspace.activePaneIndex : 0
                if (window.activeSession === session && activePane === pane)
                    window.goToActivePage(page)
            })
        }
    }

    DocumentView {
        anchors.fill: parent
        visible: window.activeSession === null
        theme: appTheme
        document: emptyDocument
        editController: emptyEditController
        searchController: emptySearchController
        formController: emptyFormController
        formDesignerController: emptyFormDesignerController
        formsTabActive: appRibbon.formsTabActive
        editTabActive: appRibbon.editTabActive
        formFieldsVisible: window.activeDocument.loaded
        formFieldsInteractive: appRibbon.viewTabActive
            || appRibbon.editTabActive
            || appRibbon.formsTabActive
        textFieldToolActive: appRibbon.textFieldToolActive
        checkboxToolActive: appRibbon.checkboxToolActive
        radioButtonToolActive: appRibbon.radioButtonToolActive
        dropdownToolActive: appRibbon.dropdownToolActive
        listBoxToolActive: appRibbon.listBoxToolActive
        combFieldToolActive: appRibbon.combFieldToolActive
        transientStatus: window.transientStatus
        onOpenRequested: window.requestOpen()
    }

    Loader {
        id: workspaceLoader

        anchors.fill: parent
        anchors.bottomMargin: Math.max(0, windowMetrics.keyboardInset - windowMetrics.bottomInset)
        active: false
        asynchronous: true
        onStatusChanged: {
            if (status !== Loader.Ready
                    || window.pendingSearchNavigationPage < 0
                    || window.pendingSearchNavigationSession
                        !== window.activeSession) {
                return
            }

            const workspace = item as DocumentWorkspace
            if (!workspace)
                return
            const page = window.pendingSearchNavigationPage
            const normalizedX = window.pendingSearchNavigationX
            const normalizedY = window.pendingSearchNavigationY
            const resultsRevision
                = window.pendingSearchNavigationRevision
            const session = window.pendingSearchNavigationSession
            if (!appRibbon.viewTabActive
                    || session.searchController.resultsRevision
                        !== resultsRevision) {
                window.clearPendingSearchNavigation()
                return
            }
            window.clearPendingSearchNavigation()
            workspace.navigateToSearchResult(
                page,
                normalizedX,
                normalizedY,
                resultsRevision)
        }
        sourceComponent: Component {
            DocumentWorkspace {
                theme: appTheme
                session: window.workspaceSession
                viewTabActive: appRibbon.viewTabActive
                headerTabActive: appRibbon.headerTabActive
                formsTabActive: appRibbon.formsTabActive
                editTabActive: appRibbon.editTabActive
                signTabActive: appRibbon.signTabActive
                commentTabActive: appRibbon.commentTabActive
                commentsPanelVisible: appRibbon.commentsPanelVisible
                formFieldsVisible: window.activeDocument.loaded
                formFieldsInteractive: appRibbon.viewTabActive
                    || appRibbon.editTabActive
                    || appRibbon.formsTabActive
                textFieldToolActive: appRibbon.textFieldToolActive
                checkboxToolActive: appRibbon.checkboxToolActive
                radioButtonToolActive: appRibbon.radioButtonToolActive
                dropdownToolActive: appRibbon.dropdownToolActive
                listBoxToolActive: appRibbon.listBoxToolActive
                combFieldToolActive: appRibbon.combFieldToolActive
                transientStatus: window.transientStatus
                enabled: !tabModel.anySaving
                onOpenRequested: window.requestOpen()
                onHeaderImageFileRequested: placement => window.requestHeaderImageFile(placement)
                onDeleteCommentRequested: (page, objectId) => {
                    window.requestDeleteComment(page, objectId)
                }
            }
        }
    }

    DropArea {
        id: pdfFileDropArea
        objectName: "pdfFileDropArea"
        anchors.fill: parent
        anchors.topMargin: -window.header.height
        anchors.bottomMargin: -window.footer.height
        z: 100
        enabled: !window.fileOpenBlocked

        onEntered: drag => {
            drag.accepted = drag.hasUrls
                && (drag.supportedActions & Qt.CopyAction) !== 0
                && documentAccess.candidates(drag.urls).length > 0
        }
        onDropped: drop => {
            const files = documentAccess.candidates(drop.urls)
            if (window.fileOpenBlocked || files.length === 0
                    || (drop.supportedActions & Qt.CopyAction) === 0) {
                drop.accepted = false
                return
            }
            if (documentAccess.accept(files))
                drop.accept(Qt.CopyAction)
            else
                drop.accepted = false
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: 5
            visible: pdfFileDropArea.containsDrag
            color: "#eefdf7f1"
            border.color: appTheme.foxitOrange
            border.width: 2
            radius: 6
            Accessible.ignored: true

            Label {
                anchors.centerIn: parent
                //% "Drop PDF files here to open"
                text: qsTrId("DropPdfFilesToOpen")
                font.pixelSize: 20
                color: appTheme.foxitDarkOrange
            }
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        width: 40
        height: 40
        visible: workspaceLoader.active
            && workspaceLoader.status === Loader.Loading
        running: visible
    }
}
