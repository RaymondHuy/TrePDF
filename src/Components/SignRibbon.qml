import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document

    signal toolRequested(string toolName)

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
            title: qsTr("Sign")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Fill && Sign")
                iconSource: "../assets/icons/pen-line.svg"
                signing: true
                primary: true
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Signature")
                toolTipText: qsTr("Add Signature")
                iconSource: "../assets/icons/signature.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Initials")
                toolTipText: qsTr("Add Initials")
                iconSource: "../assets/icons/signature.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }
        }

        RibbonGroup {
            theme: root.theme
            title: qsTr("Fields")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Date")
                toolTipText: qsTr("Add Date")
                iconSource: "../assets/icons/calendar-days.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Check")
                toolTipText: qsTr("Add Check Mark")
                iconSource: "../assets/icons/square-check-big.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }
        }

        RibbonGroup {
            theme: root.theme
            title: qsTr("Workflow")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Request")
                toolTipText: qsTr("Request Signatures")
                iconSource: "../assets/icons/send.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Validate")
                toolTipText: qsTr("Validate Signatures")
                iconSource: "../assets/icons/shield-check.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
