pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root

    required property AppTheme theme
    property bool openEnabled: true

    signal openRequested()

    spacing: 14

    Label {
        //% "No document open"
        text: qsTrId("NoDocumentOpen")
        font.pixelSize: 22
        color: "#33251d"
        Layout.alignment: Qt.AlignHCenter
    }

    OrangeButton {
        theme: root.theme
        enabled: root.openEnabled
        //% "Open PDF"
        text: qsTrId("OpenPdf")
        onClicked: root.openRequested()
        Layout.alignment: Qt.AlignHCenter
    }
}
