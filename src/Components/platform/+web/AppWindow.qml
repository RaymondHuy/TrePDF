import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    width: Screen.width
    height: Screen.height
    minimumWidth: 0
    minimumHeight: 0
    // Browser fonts default to 16px; match the desktop control density.
    font.pixelSize: 13
    visible: true
    //% "TrePDF"
    title: qsTrId("ApplicationTitle")
}
