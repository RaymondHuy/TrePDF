import QtQuick
import QtQuick.Dialogs
import TrePDF

QtObject {
    id: root
    property string title: ""
    property int fileMode: FileDialog.OpenFile
    property list<string> nameFilters: []
    property string defaultSuffix: ""
    property url selectedFile: ""
    property list<url> selectedFiles: []
    property bool visible: false
    readonly property bool writePending: pendingSession !== null
    property PDFDocumentSession pendingSession: null
    property url pendingTarget: ""
    signal accepted()
    signal rejected()

    function open() {
        if (visible || writePending || PlatformService.nativeFilesBusy)
            return
        if (fileMode === FileDialog.SaveFile) {
            const name = decodeURIComponent(selectedFile.toString().split("/").pop())
            const target = PlatformService.createLocalExportUrl(name)
            if (target.toString().length === 0) {
                rejected()
                return
            }
            selectedFile = target
            selectedFiles = [target]
            accepted()
            return
        }
        visible = true
        const started = fileMode === FileDialog.OpenFiles
            ? PlatformService.openNativePdfFiles() : PlatformService.openNativeImageFile()
        if (!started && visible) {
            visible = false
            rejected()
        }
    }

    function close() { visible = false }
    function reject() {
        close()
        rejected()
    }

    function trackWrite(session: PDFDocumentSession, target: url) {
        pendingTarget = target
        pendingSession = session
    }

    function finishWrite(session: PDFDocumentSession, succeeded: bool, target: url) {
        if (!session || session !== pendingSession)
            return
        const expected = pendingTarget
        pendingSession = null
        pendingTarget = ""
        if (succeeded && target.toString() === expected.toString())
            PlatformService.exportNativeFile(target)
    }

    property Connections exportConnection: Connections {
        target: root.defaultSuffix === "xlsx" && root.pendingSession
            ? root.pendingSession.excelExportController : null
        function onFinished(succeeded, target) {
            root.finishWrite(root.pendingSession, succeeded, target)
        }
    }

    property Connections pickerConnection: Connections {
        target: PlatformService
        function onDocumentsImportFinished() {
            if (root.visible && root.fileMode === FileDialog.OpenFiles)
                root.visible = false // Imported PDFs arrive through FileOpenHandler's queue.
        }
        function onImageFileImported(source) {
            if (!root.visible || root.fileMode !== FileDialog.OpenFile)
                return
            root.visible = false
            root.selectedFile = source
            root.selectedFiles = [source]
            root.accepted()
        }
        function onImageImportFinished() {
            if (root.visible && root.fileMode === FileDialog.OpenFile) {
                root.visible = false
                root.rejected()
            }
        }
    }
}
