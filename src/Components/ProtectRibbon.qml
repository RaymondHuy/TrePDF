pragma ComponentBehavior: Bound

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

    signal setPasswordRequested()
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
        spacing: 12

        RibbonGroup {
            theme: root.theme
            //% "Protect"
            title: qsTrId("ProtectTab")

            RibbonButton {
                objectName: "protectSetPasswordButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Set password"
                text: qsTrId("SetPdfPassword")
                iconSource: "../assets/icons/shield-check.svg"
                enabled: root.ready && root.editController.canSetOpenPassword
                onClicked: root.setPasswordRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "File"
            title: qsTrId("File")

            RibbonButton {
                objectName: "protectSaveButton"
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
                objectName: "protectSaveAsButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Save As"
                text: qsTrId("SaveAs")
                iconSource: "../assets/icons/save.svg"
                enabled: root.ready
                onClicked: root.saveAsRequested()
            }
        }

        Label {
            objectName: "protectPasswordStatus"
            width: 310
            height: ribbonRow.height
            visible: root.document.loaded
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
            color: root.editController.passwordChangePending
                ? root.theme.foxitDarkOrange : root.theme.ribbonTextColor
            text: root.editController.passwordChangePending
                //% "Password pending. Save the PDF to apply it."
                ? qsTrId("PdfPasswordPending")
                : root.document.passwordProtected
                    //% "This PDF already has password protection."
                    ? qsTrId("PdfPasswordProtected")
                    //% "This PDF has no password protection."
                    : qsTrId("PdfPasswordNotProtected")
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
