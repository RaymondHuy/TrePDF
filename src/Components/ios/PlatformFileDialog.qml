import QtQuick
import QtQuick.Dialogs
import TrePDF

QtObject {
    id: root
    property alias title: dialog.title
    property alias fileMode: dialog.fileMode
    property alias nameFilters: dialog.nameFilters
    property alias defaultSuffix: dialog.defaultSuffix
    property alias selectedFile: dialog.selectedFile
    readonly property alias selectedFiles: dialog.selectedFiles
    readonly property alias visible: dialog.visible
    readonly property bool writePending: false
    signal accepted()
    signal rejected()

    function open() { dialog.open() }
    function close() { dialog.close() }
    function reject() { dialog.reject() }
    function trackWrite(session: PDFDocumentSession, target: url) {}
    function finishWrite(session: PDFDocumentSession, succeeded: bool, target: url) {}

    property FileDialog dialog: FileDialog {
        id: dialog
        onAccepted: root.accepted()
        onRejected: root.rejected()
    }
}
