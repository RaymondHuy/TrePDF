pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    property PDFDocumentSession session: null
    property bool anySaving: false
    property bool hasPendingPanelEdits: false
    property bool ocrPanelVisible: false
    readonly property PDFExcelExportController controller: session
        ? session.excelExportController
        : null
    readonly property bool busy: controller && controller.busy
    readonly property bool hasUnsavedChanges:
        (session && session.hasDrafts) || hasPendingPanelEdits

    signal exportRequested()
    signal ocrRequested()

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
            //% "Export"
            title: qsTrId("ExportTab")

            RibbonButton {
                objectName: "exportExcelButton"
                theme: root.theme
                documentLoaded: root.session && root.session.document.loaded
                enabled: documentLoaded && root.controller && !root.busy
                    && !root.anySaving && !root.hasUnsavedChanges
                //% "Export to Excel"
                text: qsTrId("ExportExcel")
                iconSource: "../assets/icons/spreadsheet.svg"
                primary: true
                onClicked: root.exportRequested()
            }

            RibbonButton {
                objectName: "cancelExcelExportButton"
                theme: root.theme
                documentLoaded: root.session && root.session.document.loaded
                visible: root.busy
                //% "Cancel"
                text: qsTrId("Cancel")
                iconSource: "../assets/icons/cancel.svg"
                onClicked: {
                    if (root.controller)
                        root.controller.cancel()
                }
            }
        }

        RibbonGroup {
            visible: root.session && root.session.ocrController.available
            theme: root.theme
            //% "Text recognition"
            title: qsTrId("OCRRibbonGroup")

            RibbonButton {
                objectName: "recognizeTextButton"
                theme: root.theme
                documentLoaded: root.session && root.session.document.loaded
                enabled: documentLoaded && !root.anySaving
                //% "Recognize Text (OCR)"
                text: qsTrId("RecognizeTextOCR")
                iconSource: "../assets/icons/type.svg"
                primary: root.ocrPanelVisible
                onClicked: root.ocrRequested()
            }
        }

    }
}
