pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root

    required property AppTheme theme
    property bool openEnabled: true
    property bool showCreateButton: false

    signal openRequested()
    signal createRequested()

    spacing: 14

    Label {
        //% "No document open"
        text: qsTrId("NoDocumentOpen")
        font.pixelSize: 22
        color: "#33251d"
        Layout.alignment: Qt.AlignHCenter
    }

    RowLayout {
        Layout.alignment: Qt.AlignHCenter
        spacing: 8

        OrangeButton {
            theme: root.theme
            enabled: root.openEnabled
            //% "Open PDF"
            text: qsTrId("OpenPdf")
            onClicked: root.openRequested()
        }

        OrangeButton {
            objectName: "createPdfButton"
            theme: root.theme
            visible: root.showCreateButton
            enabled: root.openEnabled
            //% "New PDF"
            text: qsTrId("NewPdf")
            onClicked: root.createRequested()
        }
    }
}
