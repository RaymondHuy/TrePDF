pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property bool textFieldToolSelected

    signal textFieldToolRequested()

    clip: true
    contentWidth: ribbonRow.implicitWidth + 16
    contentHeight: height
    flickableDirection: Flickable.HorizontalFlick
    boundsBehavior: Flickable.StopAtBounds

    Row {
        id: ribbonRow

        x: 8
        height: 76

        RibbonGroup {
            theme: root.theme
            //% "Fields"
            title: qsTrId("Fields")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Text Field"
                text: qsTrId("TextFieldTool")
                //% "Drag on the page to add a text field"
                toolTipText: qsTrId("DragToAddTextField")
                iconSource: "../assets/icons/text-input.svg"
                icon.width: 32
                icon.height: 32
                preserveIconColors: true
                primary: root.textFieldToolSelected
                onClicked: root.textFieldToolRequested()
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
