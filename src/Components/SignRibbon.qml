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
            //% "Sign"
            title: qsTrId("Sign")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Fill && Sign"
                text: qsTrId("FillAndSign")
                iconSource: "../assets/icons/pen-line.svg"
                signing: true
                primary: true
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Signature"
                text: qsTrId("Signature")
                //% "Add Signature"
                toolTipText: qsTrId("AddSignature")
                iconSource: "../assets/icons/signature.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Initials"
                text: qsTrId("Initials")
                //% "Add Initials"
                toolTipText: qsTrId("AddInitials")
                iconSource: "../assets/icons/signature.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Fields"
            title: qsTrId("Fields")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Date"
                text: qsTrId("Date")
                //% "Add Date"
                toolTipText: qsTrId("AddDate")
                iconSource: "../assets/icons/calendar-days.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Check"
                text: qsTrId("CheckMark")
                //% "Add Check Mark"
                toolTipText: qsTrId("AddCheckMark")
                iconSource: "../assets/icons/square-check-big.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Workflow"
            title: qsTrId("Workflow")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Request"
                text: qsTrId("Request")
                //% "Request Signatures"
                toolTipText: qsTrId("RequestSignatures")
                iconSource: "../assets/icons/send.svg"
                signing: true
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Validate"
                text: qsTrId("Validate")
                //% "Validate Signatures"
                toolTipText: qsTrId("ValidateSignatures")
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
