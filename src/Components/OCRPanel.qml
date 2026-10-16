pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TrePDF

Rectangle {
    id: root
    objectName: "ocrPanel"

    required property AppTheme theme
    required property PDFDocumentSession session
    property int currentPage: 0
    property bool anySaving: false
    property bool outputDialogOpen: false
    property bool hasPendingPanelEdits: false
    readonly property PDFOCRController controller: session ? session.ocrController : null
    readonly property bool busy: controller && controller.busy
    readonly property bool hasUnsavedChanges: (session && session.hasDrafts)
        || hasPendingPanelEdits
    readonly property string selectedLanguage: languageSelector.currentIndex === 1
        ? "vie" : languageSelector.currentIndex === 2 ? "eng" : "eng+vie"
    readonly property string selectedPageRange: pageSelector.currentIndex === 1
        ? String(currentPage + 1)
        : pageSelector.currentIndex === 2 ? pageRangeInput.text.trim() : ""
    readonly property bool validPageRange: pageSelector.currentIndex === 0
        ? (session && session.document.pageCount > 0 && session.document.pageCount <= 1000)
        : pageSelector.currentIndex === 1
            ? currentPage >= 0 && session && currentPage < session.document.pageCount
            : validatePageRange(pageRangeInput.text, session ? session.document.pageCount : 0)

    signal startRequested(string language, string pageRange)
    signal closeRequested()

    implicitWidth: 320
    color: theme.ribbonSurface

    function validatePageRange(value, pageCount) {
        const parts = value.trim().split(",")
        if (value.length > 4096 || parts.length === 0 || pageCount < 1)
            return false
        const selected = new Set()
        for (const part of parts) {
            const match = /^\s*([1-9][0-9]*)\s*(?:-\s*([1-9][0-9]*)\s*)?$/.exec(part)
            if (!match)
                return false
            const first = Number(match[1])
            const last = match[2] === undefined ? first : Number(match[2])
            if (last < first || last > pageCount || last - first >= 1000)
                return false
            for (let page = first; page <= last; ++page) {
                selected.add(page)
                if (selected.size > 1000)
                    return false
            }
        }
        return true
    }

    onSessionChanged: {
        if (languageSelector)
            languageSelector.currentIndex = 0
        if (pageSelector)
            pageSelector.currentIndex = 0
        if (pageRangeInput)
            pageRangeInput.clear()
    }

    component FieldLabel: Label {
        Layout.fillWidth: true
        color: root.theme.ribbonTextColor
        wrapMode: Text.Wrap
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: root.theme.ribbonBorder
        Accessible.ignored: true
    }

    ScrollView {
        id: panelScroll
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: panelScroll.availableWidth
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                FieldLabel {
                    //% "Recognize Text (OCR)"
                    text: qsTrId("RecognizeTextOCR")
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                ToolButton {
                    objectName: "closeOCRPanelButton"
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    icon.source: "../assets/icons/cancel.svg"
                    icon.width: 16
                    icon.height: 16
                    //% "Close"
                    Accessible.name: qsTrId("Close")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onClicked: root.closeRequested()
                }
            }

            FieldLabel {
                //% "Create a copy of this PDF with searchable, selectable text. The original page appearance is preserved."
                text: qsTrId("OCROutputDescription")
                font.pixelSize: 12
            }

            FieldLabel {
                id: languageLabel
                //% "Recognition language"
                text: qsTrId("OCRLanguage")
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: languageSelector
                objectName: "ocrLanguageSelector"
                Layout.fillWidth: true
                enabled: !root.busy && !root.outputDialogOpen
                Accessible.name: languageLabel.text
                model: [
                    //% "Vietnamese and English"
                    qsTrId("OCRLanguageVietnameseEnglish"),
                    //% "Vietnamese"
                    qsTrId("OCRLanguageVietnamese"),
                    //% "English"
                    qsTrId("OCRLanguageEnglish")
                ]
            }

            FieldLabel {
                id: pageRangeLabel
                //% "Pages to recognize"
                text: qsTrId("OCRPageRange")
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: pageSelector
                objectName: "ocrPageSelector"
                Layout.fillWidth: true
                enabled: !root.busy && !root.outputDialogOpen
                Accessible.name: pageRangeLabel.text
                model: [
                    //% "All pages"
                    qsTrId("OCRAllPages"),
                    //% "Current page (%1)"
                    qsTrId("OCRCurrentPage").arg(root.currentPage + 1),
                    //% "Custom page range"
                    qsTrId("OCRCustomPages")
                ]
            }
            TextField {
                id: pageRangeInput
                objectName: "ocrPageRangeInput"
                Layout.fillWidth: true
                visible: pageSelector.currentIndex === 2
                enabled: !root.busy && !root.outputDialogOpen
                maximumLength: 4096
                //% "For example: 1-3, 5"
                placeholderText: qsTrId("OCRPageRangePlaceholder")
                Accessible.name: pageRangeLabel.text
                selectByMouse: true
            }
            FieldLabel {
                visible: pageSelector.currentIndex === 2
                //% "Use PDF page numbers from 1 to %1. Separate pages or ranges with commas."
                text: qsTrId("OCRPageRangeHint").arg(root.session
                    ? root.session.document.pageCount : 0)
                color: pageRangeInput.text.length > 0 && !root.validPageRange
                    ? "#b3261e" : root.theme.ribbonTextColor
                font.pixelSize: 11
            }
            FieldLabel {
                visible: root.session && root.session.document.pageCount > 1000
                //% "Select up to 1,000 pages per OCR run. Use a custom range for larger documents."
                text: qsTrId("OCRPageLimitHint")
                color: root.validPageRange ? root.theme.ribbonTextColor : "#b3261e"
                font.pixelSize: 11
            }

            FieldLabel {
                //% "Pages that already contain text are skipped to avoid duplicate text. Mixed pages containing both text and scanned images are also skipped."
                text: qsTrId("OCRSkipExistingTextHint")
                font.pixelSize: 12
            }
            FieldLabel {
                objectName: "ocrSaveFirstHint"
                visible: root.hasUnsavedChanges && !root.busy
                //% "Save or undo your PDF changes before starting OCR."
                text: qsTrId("OCRSaveFirstHint")
                color: "#b3261e"
                font.pixelSize: 12
            }

            ProgressBar {
                objectName: "ocrProgressBar"
                Layout.fillWidth: true
                visible: root.busy
                from: 0
                to: Math.max(1, root.controller ? root.controller.totalPages : 0)
                value: root.controller ? root.controller.completedPages : 0
                indeterminate: root.busy && root.controller.totalPages === 0
                //% "OCR progress"
                Accessible.name: qsTrId("OCRProgress")
            }
            FieldLabel {
                objectName: "ocrProgressLabel"
                visible: root.busy && root.controller.totalPages > 0
                //% "%1 of %2 pages processed"
                text: qsTrId("OCRProgressPages")
                    .arg(root.controller ? root.controller.completedPages : 0)
                    .arg(root.controller ? root.controller.totalPages : 0)
                font.pixelSize: 12
            }
            FieldLabel {
                objectName: "ocrStatusLabel"
                text: root.controller ? root.controller.statusText : ""
                visible: text.length > 0
                font.pixelSize: 12
            }
            FieldLabel {
                objectName: "ocrPageSummary"
                visible: root.controller && root.controller.completedPages > 0
                //% "Recognized: %1 · Skipped: %2 · No text found: %3"
                text: qsTrId("OCRPageSummary")
                    .arg(root.controller ? root.controller.recognizedPages : 0)
                    .arg(root.controller ? root.controller.skippedPages : 0)
                    .arg(root.controller ? root.controller.emptyPages : 0)
                font.pixelSize: 11
            }

            OrangeButton {
                objectName: "startOCRButton"
                Layout.fillWidth: true
                theme: root.theme
                visible: !root.busy
                enabled: root.controller && root.controller.available
                    && root.session.document.loaded && !root.anySaving
                    && !root.outputDialogOpen && !root.hasUnsavedChanges
                    && root.validPageRange
                //% "Create OCR PDF…"
                text: qsTrId("OCRCreatePDF")
                onClicked: root.startRequested(root.selectedLanguage, root.selectedPageRange)
            }
            Button {
                objectName: "cancelOCRButton"
                Layout.fillWidth: true
                visible: root.busy
                //% "Cancel"
                text: qsTrId("Cancel")
                onClicked: {
                    if (root.controller)
                        root.controller.cancel()
                }
            }
        }
    }
}
