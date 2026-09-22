import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    property bool hasPendingPanelEdits: false
    readonly property bool ready: document.loaded && !editController.saving

    signal signatureRequested()
    signal initialsRequested()
    signal dateRequested()
    signal checkRequested()
    signal selectRequested()
    signal undoRequested()
    signal redoRequested()
    signal saveRequested()
    signal saveAsRequested()

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
                objectName: "signSelectButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Select"
                text: qsTrId("Select")
                iconSource: "../assets/icons/mouse-pointer-2.svg"
                signing: true
                enabled: root.ready
                primary: root.editController.tool === PDFEditController.SelectTool
                onClicked: root.selectRequested()
            }
            RibbonButton {
                objectName: "signSignatureButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Signature"
                text: qsTrId("Signature")
                //% "Add Signature"
                toolTipText: qsTrId("AddSignature")
                iconSource: "../assets/icons/signature.svg"
                signing: true
                enabled: root.ready
                onClicked: root.signatureRequested()
            }
            RibbonButton {
                objectName: "signInitialsButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Initials"
                text: qsTrId("Initials")
                //% "Add Initials"
                toolTipText: qsTrId("AddInitials")
                iconSource: "../assets/icons/signature.svg"
                signing: true
                enabled: root.ready
                onClicked: root.initialsRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Fields"
            title: qsTrId("Fields")

            RibbonButton {
                objectName: "signDateButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Date"
                text: qsTrId("Date")
                //% "Add Date"
                toolTipText: qsTrId("AddDate")
                iconSource: "../assets/icons/calendar-days.svg"
                signing: true
                enabled: root.ready
                onClicked: root.dateRequested()
            }
            RibbonButton {
                objectName: "signCheckButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Check"
                text: qsTrId("CheckMark")
                //% "Add Check Mark"
                toolTipText: qsTrId("AddCheckMark")
                iconSource: "../assets/icons/square-check-big.svg"
                signing: true
                enabled: root.ready
                onClicked: root.checkRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "History"
            title: qsTrId("History")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Undo"
                text: qsTrId("Undo")
                iconSource: "../assets/icons/undo-2.svg"
                enabled: root.ready && root.editController.canUndo
                    && !root.editController.textEditActive
                onClicked: root.undoRequested()
            }
            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Redo"
                text: qsTrId("Redo")
                iconSource: "../assets/icons/redo-2.svg"
                enabled: root.ready && root.editController.canRedo
                    && !root.hasPendingPanelEdits
                    && !root.editController.textEditActive
                onClicked: root.redoRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "File"
            title: qsTrId("File")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Save"
                text: qsTrId("Save")
                iconSource: "../assets/icons/save.svg"
                enabled: root.ready
                    && (root.editController.hasDrafts || root.hasPendingPanelEdits)
                onClicked: root.saveRequested()
            }
            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Save As"
                text: qsTrId("SaveAs")
                iconSource: "../assets/icons/save.svg"
                enabled: root.ready
                onClicked: root.saveAsRequested()
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
