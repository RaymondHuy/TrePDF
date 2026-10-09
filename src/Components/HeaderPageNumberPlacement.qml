pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

QtObject {
    id: root
    objectName: "headerPageNumberPlacement"

    required property PDFDocument document
    required property PDFEditController editController
    property bool armed: false
    property int page: -1
    readonly property bool active: page >= 0
    property real xPoints: 0
    property real yPoints: 0
    property real widthPoints: 0
    property int firstPage: 1
    property int lastPage: 1
    property bool romanEnabled: false
    property int romanStartPage: 1
    property bool decimalEnabled: true
    property int decimalStartPage: 1
    property string fontFamily: ""
    property string errorText: ""
    property bool submitting: false
    readonly property int pageCount: document ? document.pageCount : 0
    readonly property int firstNumberedPage: romanEnabled ? romanStartPage : decimalStartPage
    readonly property string validationError: {
        if (!active)
            return ""
        if (firstPage < 1 || lastPage < firstPage || lastPage > pageCount
                || (!romanEnabled && !decimalEnabled)
                || (romanEnabled && (romanStartPage < firstPage || romanStartPage > lastPage))
                || (decimalEnabled && (decimalStartPage < firstPage || decimalStartPage > lastPage))
                || (romanEnabled && decimalEnabled && romanStartPage >= decimalStartPage)) {
            //% "Choose valid start and stop pages, with Roman numerals before Arabic numbers."
            return qsTrId("HeaderPageNumbersInvalidRange")
        }
        if (romanEnabled && (decimalEnabled ? decimalStartPage - 1 : lastPage)
                - romanStartPage + 1 > 3999) {
            //% "Roman numerals support at most 3,999 numbered pages."
            return qsTrId("HeaderPageNumbersRomanLimit")
        }
        if (lastPage - firstNumberedPage + 1 > 10000) {
            //% "This header batch is too large. Apply it to fewer pages."
            return qsTrId("HeaderTextBatchTooLarge")
        }
        return ""
    }
    readonly property bool canSubmit: active && !submitting && document.loaded
        && !editController.saving && validationError.length === 0

    signal focusRequested()

    function start() {
        if (!document.loaded || editController.saving)
            return
        if (active) {
            focusRequested()
            return
        }
        errorText = ""
        armed = true
    }

    function place(targetPage, x, y) {
        if (!armed || !document.loaded || editController.saving
                || targetPage < 0 || targetPage >= pageCount
                || !Number.isFinite(x) || !Number.isFinite(y))
            return
        const size = document.pageSize(targetPage)
        const headerHeight = Math.min(100, size.height)
        if (size.width <= 0 || headerHeight <= 0)
            return
        xPoints = Math.max(0, Math.min(x, Math.max(0, size.width - 48)))
        yPoints = Math.max(0, Math.min(y, Math.max(0, headerHeight - 18)))
        widthPoints = Math.min(Math.max(48, size.width * 0.25), size.width - xPoints)
        firstPage = 1
        lastPage = pageCount
        romanEnabled = false
        romanStartPage = 1
        decimalEnabled = true
        decimalStartPage = 1
        fontFamily = editController.headerTextFontFamily()
        errorText = ""
        armed = false
        page = targetPage
    }

    function labelForPage(targetPage) {
        if (!active)
            return ""
        return editController.headerPageNumberText(targetPage, firstPage, lastPage,
            romanEnabled ? romanStartPage : 0, decimalEnabled ? decimalStartPage : 0)
    }

    function cancel() {
        page = -1
        armed = false
        errorText = ""
    }

    function submit() {
        if (!canSubmit)
            return
        errorText = ""
        submitting = true
        const succeeded = editController.addHeaderPageNumbers(firstPage, lastPage,
            romanEnabled ? romanStartPage : 0, decimalEnabled ? decimalStartPage : 0,
            xPoints, yPoints, widthPoints, { fontFamily: fontFamily, fontSize: 12, color: "black" })
        submitting = false
        if (succeeded) {
            cancel()
        } else if (errorText.length === 0) {
            //% "Could not add page numbers."
            errorText = qsTrId("AddHeaderPageNumbersFailed")
        }
    }

    onFirstPageChanged: errorText = ""
    onLastPageChanged: errorText = ""
    onRomanEnabledChanged: errorText = ""
    onRomanStartPageChanged: errorText = ""
    onDecimalEnabledChanged: errorText = ""
    onDecimalStartPageChanged: errorText = ""
    onDocumentChanged: cancel()
    onEditControllerChanged: cancel()

    readonly property Connections documentConnections: Connections {
        target: root.document
        function onSourceChanged() { root.cancel() }
        function onPageStructureChanged() { root.cancel() }
        function onLoadedChanged() { root.cancel() }
    }
    readonly property Connections controllerConnections: Connections {
        target: root.editController
        function onHistoryStateChanged() {
            if (!root.submitting)
                root.cancel()
        }
        function onSavingChanged() {
            if (root.editController.saving)
                root.cancel()
        }
        function onStatusMessage(message) {
            if (root.submitting)
                root.errorText = message
        }
    }
}
