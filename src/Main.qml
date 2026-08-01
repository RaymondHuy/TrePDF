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
    //% "TrePDF"
    title: qsTrId("ApplicationTitle")

    property string transientStatus: ""
    property bool allowWindowClose: false
    property string pendingDraftAction: ""
    property bool openingFiles: false
    property PDFDocumentSession pendingSession: null
    property PDFDocumentSession saveAsSession: null
    property PDFDocumentSession workspaceSession: null
    property PDFDocumentSession deletePageSession: null
    property int deletePageIndex: -1
    property PDFDocumentSession externalLinkSession: null
    property var externalLinkRequestId: 0
    property string externalLinkAuthority: ""
    property string externalLinkDisplayUrl: ""
    property bool externalLinkInsecureHttp: false
    property PDFEditController imageInsertController: null
    property int imageInsertPage: -1
    property real imageInsertX: 0
    property real imageInsertY: 0
    readonly property PDFDocumentSession activeSession: tabModel.currentSession
    readonly property PDFDocument activeDocument: activeSession
        ? activeSession.document
        : emptyDocument
    readonly property PDFEditController activeEditController: activeSession
        ? activeSession.editController
        : emptyEditController
    readonly property PDFFormDesignerController activeFormDesignerController:
        activeSession
            ? activeSession.formDesignerController
            : emptyFormDesignerController

    palette.highlight: appTheme.foxitOrange
    palette.button: appTheme.foxitPaleOrange
    palette.buttonText: "#32251c"
    palette.window: "#f7f2ee"

    function activeWorkspace() {
        return workspaceLoader.status === Loader.Ready
            ? workspaceLoader.item
            : null
    }

    function prepareActiveWorkspace() {
        const workspace = activeWorkspace()
        if (workspace)
            workspace.prepareForDeactivation()
        else if (activeSession)
            activeSession.prepareForDeactivation()
    }

    function rebuildWorkspace() {
        const workspace = activeWorkspace()
        if (workspace)
            workspace.prepareForDeactivation()
        workspaceLoader.active = false
        workspaceSession = activeSession
        workspaceLoader.active = workspaceSession !== null
    }

    function setZoom(value) {
        if (activeSession)
            activeSession.zoom = Math.max(0.1, Math.min(5.0, value))
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

        prepareActiveWorkspace()
        tabModel.currentIndex = index
    }

    function requestOpen() {
        if (tabModel.anySaving) {
            //% "Please wait for the PDF to finish saving."
            showStatus(qsTrId("WaitForPdfSave"))
            return
        }

        openDialog.open()
    }

    function openFiles(urls) {
        prepareActiveWorkspace()
        openingFiles = true
        for (let index = 0; index < urls.length; ++index)
            tabModel.open(urls[index])
        openingFiles = false
        rebuildWorkspace()
    }

    function requestSave() {
        if (activeSession) {
            prepareActiveWorkspace()
            tabModel.requestSave(activeSession)
        }
    }

    function requestSaveAs() {
        if (!activeSession || tabModel.anySaving)
            return

        saveAsSession = activeSession
        saveAsDialog.selectedFile = saveAsSession.document.source
        saveAsDialog.open()
    }

    function requestImageFile(controller, page, x, y) {
        imageInsertController = controller
        imageInsertPage = page
        imageInsertX = x
        imageInsertY = y
        imageDialog.open()
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
            if (session === activeSession)
                prepareActiveWorkspace()
            tabModel.requestSaveAs(session, url)
        }
    }

    function requestDeletePage(page) {
        if (!activeSession
            || tabModel.anySaving
            || page < 0
            || page >= activeDocument.pageCount
            || activeDocument.pageCount <= 1) {
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
            || session.document.pageCount <= 1) {
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

        if (session === activeSession)
            prepareActiveWorkspace()
        else
            session.prepareForDeactivation()

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
        prepareActiveWorkspace()
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

    PDFFormController {
        id: emptyFormController

        document: emptyDocument
    }

    PDFFormDesignerController {
        id: emptyFormDesignerController
    }

    PDFTabModel {
        id: tabModel

        onStatusMessage: message => window.showStatus(message)
        onSessionSaveFinished: (session, succeeded) => {
            window.finishPendingDraftAction(session, succeeded)
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

        function onImageFileRequested(page, x, y) {
            window.requestImageFile(
                window.activeEditController,
                page,
                x,
                y)
        }
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: window.activeSession !== null
            && window.activeEditController.canUndo
            && !window.activeEditController.textEditActive
            && !window.activeFormDesignerController.textFieldFocused
            && !appRibbon.formsTabActive
            && !tabModel.anySaving
        onActivated: {
            const page = window.activeEditController.undo()
            if (page >= 0)
                window.goToPage(page)
        }
    }

    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: window.activeSession !== null
            && window.activeEditController.canRedo
            && !window.activeEditController.textEditActive
            && !window.activeFormDesignerController.textFieldFocused
            && !appRibbon.formsTabActive
            && !tabModel.anySaving
        onActivated: {
            const page = window.activeEditController.redo()
            if (page >= 0)
                window.goToPage(page)
        }
    }

    Shortcut {
        sequences: [StandardKey.Save]
        enabled: window.activeDocument.loaded
            && window.activeEditController.hasDrafts
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

    FileDialog {
        id: openDialog

        //% "Open PDFs"
        title: qsTrId("OpenPdfs")
        fileMode: FileDialog.OpenFiles
        //% "PDF files (*.pdf)"
        nameFilters: [qsTrId("PdfFileFilter")]
        onAccepted: window.openFiles(selectedFiles)
    }

    FileDialog {
        id: imageDialog

        //% "Select Image"
        title: qsTrId("SelectImage")
        fileMode: FileDialog.OpenFile
        //% "Image files (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)"
        nameFilters: [qsTrId("ImageFileFilter")]
        onAccepted: window.insertSelectedImage(selectedFile)
        onRejected: window.clearImageInsertRequest()
    }

    FileDialog {
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

    Popup {
        id: externalLinkPopup

        x: Math.round((window.width - width) / 2)
        y: Math.round((window.height - height) / 2)
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
        y: Math.round((window.height - height) / 2)
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
        y: Math.round((window.height - height) / 2)
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

        AppRibbon {
            id: appRibbon

            width: parent.width
            theme: appTheme
            document: window.activeDocument
            editController: window.activeEditController
            enabled: !tabModel.anySaving
            onOpenRequested: window.requestOpen()
            onSaveRequested: window.requestSave()
            onSaveAsRequested: window.requestSaveAs()
            onPageRequested: page => window.goToPage(page)
            onDeletePageRequested: page => window.requestDeletePage(page)
            onZoomOutRequested: {
                window.setZoom(window.activeSession
                    ? window.activeSession.zoom / 1.15
                    : 1.0)
            }
            onZoomInRequested: {
                window.setZoom(window.activeSession
                    ? window.activeSession.zoom * 1.15
                    : 1.0)
            }
            onFitWidthRequested: window.fitToWidth()
            onRotateRequested: quarterTurns => window.rotateView(quarterTurns)
            onToolRequested: toolName => window.showComingSoon(toolName)
        }

        DocumentTabBar {
            width: parent.width
            theme: appTheme
            tabModel: tabModel
            enabled: !tabModel.anySaving
            onTabRequested: index => window.switchToTab(index)
            onCloseRequested: session => window.requestCloseSession(session)
            onOpenRequested: window.requestOpen()
        }
    }

    DocumentView {
        anchors.fill: parent
        visible: window.activeSession === null
        theme: appTheme
        document: emptyDocument
        editController: emptyEditController
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
        dropdownToolActive: appRibbon.dropdownToolActive
        transientStatus: window.transientStatus
        onOpenRequested: window.requestOpen()
    }

    Loader {
        id: workspaceLoader

        anchors.fill: parent
        active: false
        asynchronous: true
        sourceComponent: Component {
            DocumentWorkspace {
                theme: appTheme
                session: window.workspaceSession
                formsTabActive: appRibbon.formsTabActive
                editTabActive: appRibbon.editTabActive
                formFieldsVisible: window.activeDocument.loaded
                formFieldsInteractive: appRibbon.viewTabActive
                    || appRibbon.editTabActive
                    || appRibbon.formsTabActive
                textFieldToolActive: appRibbon.textFieldToolActive
                checkboxToolActive: appRibbon.checkboxToolActive
                dropdownToolActive: appRibbon.dropdownToolActive
                transientStatus: window.transientStatus
                enabled: !tabModel.anySaving
                onOpenRequested: window.requestOpen()
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
