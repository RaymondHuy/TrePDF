import QtQuick
import TrePDF

QtObject {
    readonly property bool busy: false
    signal filesAccepted(var urls)
    signal errorOccurred(string message)
    function candidates(urls) { return FileOpenHandler.localPdfFiles(urls) }
    function accept(urls) {
        const files = candidates(urls)
        if (files.length === 0)
            return false
        filesAccepted(files)
        return true
    }
}
