import QtQuick
import TrePDF

QtObject {
    id: root
    readonly property bool busy: PlatformService.nativeFilesBusy
    signal filesAccepted(var urls)
    signal errorOccurred(string message)
    function candidates(urls) { return PlatformService.nativePdfFiles(urls) }
    function accept(urls) { return PlatformService.importNativePdfFiles(urls) }
    property Connections serviceConnection: Connections {
        target: PlatformService
        function onErrorOccurred(message) { root.errorOccurred(message) }
    }
}
