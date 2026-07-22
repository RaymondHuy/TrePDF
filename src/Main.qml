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

    PDFTabModel {
        id: tabModel

        onStatusMessage: message => window.showStatus(message)
        onSessionSaveFinished: (session, succeeded) => {
            window.finishPendingDraftAction(session, succeeded)
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
            && !tabModel.anySaving
        onActivated: window.activeEditController.undo()
    }

    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: window.activeSession !== null
            && window.activeEditController.canRedo
            && !window.activeEditController.textEditActive
            && !tabModel.anySaving
        onActivated: window.activeEditController.redo()
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

    header: Column {
        width: window.width

        AppRibbon {
            width: parent.width
            theme: appTheme
            document: window.activeDocument
            editController: window.activeEditController
            enabled: !tabModel.anySaving
            onOpenRequested: window.requestOpen()
            onSaveRequested: window.requestSave()
            onSaveAsRequested: window.requestSaveAs()
            onPageRequested: page => window.goToPage(page)
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
