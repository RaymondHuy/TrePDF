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
    property bool anySaving: false
    readonly property bool ready: document.loaded && !editController.saving && !anySaving

    signal setPasswordRequested()
    signal watermarkRequested(string mode)
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
                text: root.document.passwordProtected
                    //% "Change password"
                    ? qsTrId("ChangePdfPassword")
                    //% "Set password"
                    : qsTrId("SetPdfPassword")
                iconSource: "../assets/icons/shield-check.svg"
                enabled: root.ready && root.editController.canSetOpenPassword
                onClicked: root.setPasswordRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Watermark"
            title: qsTrId("Watermark")

            RibbonButton {
                id: watermarkButton
                objectName: "protectWatermarkButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Watermark"
                text: qsTrId("Watermark")
                iconSource: "../assets/icons/stamp.svg"
                menuIndicatorVisible: true
                menuToolTipText: text
                enabled: root.ready
                primary: watermarkMenuLoader.active
                onClicked: watermarkMenuLoader.active = true
                onMenuRequested: watermarkMenuLoader.active = true
                onEnabledChanged: {
                    if (!enabled)
                        watermarkMenuLoader.active = false
                }
                onVisibleChanged: {
                    if (!visible)
                        watermarkMenuLoader.active = false
                }

                Connections {
                    target: root
                    function onDocumentChanged() { watermarkMenuLoader.active = false }
                }

                Loader {
                    id: watermarkMenuLoader
                    active: false
                    onLoaded: {
                        if (status === Loader.Ready && item)
                            (item as Menu).open()
                    }
                    sourceComponent: Menu {
                        parent: watermarkButton
                        y: watermarkButton.height
                        width: 230
                        padding: 4
                        popupType: Popup.Item
                        focus: true
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                        onClosed: watermarkMenuLoader.active = false

                        delegate: MenuItem {
                            id: watermarkMenuItem
                            implicitHeight: 32
                            leftPadding: 12
                            rightPadding: 12
                            font.pixelSize: 12
                            palette.text: root.theme.ribbonTextColor
                            palette.highlightedText: root.theme.ribbonTextColor
                            background: Rectangle {
                                radius: 4
                                color: watermarkMenuItem.highlighted
                                    ? root.theme.foxitPaleOrange : "transparent"
                            }
                        }

                        background: Rectangle {
                            radius: 7
                            color: root.theme.ribbonSurface
                            border.width: 1
                            border.color: root.theme.ribbonBorder
                        }

                        Action {
                            //% "Add watermark"
                            text: qsTrId("AddWatermark")
                            onTriggered: root.watermarkRequested("add")
                        }
                        Action {
                            //% "Update watermark"
                            text: qsTrId("UpdateWatermark")
                            onTriggered: root.watermarkRequested("update")
                        }
                        Action {
                            //% "Remove watermark"
                            text: qsTrId("RemoveWatermark")
                            onTriggered: root.watermarkRequested("remove")
                        }
                    }
                }
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
            objectName: "protectWatermarkStatus"
            width: 260
            height: ribbonRow.height
            visible: root.document.loaded && root.editController.watermarkChangesPending
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
            color: root.theme.foxitDarkOrange
            //% "Save or discard watermark changes before editing page content."
            text: qsTrId("WatermarkPendingSaveHint")
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
                : root.document.loaded && !root.document.allowsPasswordChange
                    //% "Changing the password is not supported for PDFs with permission restrictions."
                    ? qsTrId("PdfPasswordChangeRestricted")
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
