pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

QtObject {
    id: root
    objectName: "headerTextPlacement"

    required property PDFDocument document
    required property PDFEditController editController
    readonly property bool footerMode: editController ? editController.footerMode : false
    property bool armed: false
    readonly property bool active: page >= 0
    property int page: -1
    property real xPoints: 0
    property real yPoints: 0
    property real widthPoints: 0
    property real initialHeightPoints: 0
    property string text: ""
    property string fontFamily: ""
    property string baseFontFamily: ""
    property var fontFamilies: []
    property int typingFamilyPosition: -1
    property real fontSize: 12
    property bool fontBold: false
    property bool fontItalic: false
    property bool fontUnderline: false
    property color textColor: "black"
    property var colors: []
    property int cursorPosition: 0
    property int typingPosition: -1
    property string errorText: ""
    property bool submitting: false
    property bool inputFocused: false
    readonly property bool canSubmit: active && document.loaded
        && !editController.saving && !submitting && text.trim().length > 0

    signal focusRequested()
    signal synchronizeRequested()
    signal typingColorRequested(color value)
    signal typingFontFamilyRequested(string value)

    function compatibleFont(family, bold, italic) {
        const families = editController.headerTextFontFamilies(-1, bold, italic)
        const key = family.toLowerCase()
        return families.some(value => value.toLowerCase() === key)
    }

    function setFontFamily(value) {
        if (active && compatibleFont(value, fontBold, fontItalic)) {
            InputMethod.commit()
            synchronizeRequested()
            fontFamily = value
            typingFamilyPosition = cursorPosition
            typingFontFamilyRequested(value)
            errorText = ""
        }
    }

    function setFontSize(value) {
        if (active && Number.isInteger(value) && value >= 1 && value <= 512) {
            InputMethod.commit()
            fontSize = value
            errorText = ""
        }
    }

    function setFontBold(value) {
        if (active && compatibleFont(fontFamily, value, fontItalic)) {
            InputMethod.commit()
            fontBold = value
            errorText = ""
        }
    }

    function setFontItalic(value) {
        if (active && compatibleFont(fontFamily, fontBold, value)) {
            InputMethod.commit()
            fontItalic = value
            errorText = ""
        }
    }

    function setFontUnderline(value) {
        if (active) {
            InputMethod.commit()
            fontUnderline = value
            errorText = ""
        }
    }

    function setTextColor(value) {
        if (active) {
            InputMethod.commit()
            synchronizeRequested()
            textColor = value
            typingPosition = cursorPosition
            typingColorRequested(value)
            errorText = ""
        }
    }

    function start() {
        if (!document.loaded || editController.saving || submitting)
            return
        if (active) {
            focusRequested()
            return
        }
        armed = true
    }

    function place(pageIndex, x, y) {
        if (!armed || !document.loaded || editController.saving
                || pageIndex < 0 || pageIndex >= document.pageCount
                || !Number.isFinite(x) || !Number.isFinite(y)) {
            return
        }
        const size = document.pageSize(pageIndex)
        if (size.width < 48 || size.height < 18 || x < 0 || x >= size.width
                || y < 0 || y >= Math.min(100, size.height))
            return
        xPoints = Math.max(0, Math.min(size.width - 48, x))
        yPoints = Math.max(0, Math.min(Math.min(100, size.height) - 18, y))
        widthPoints = Math.min(Math.max(48, size.width * 0.25), size.width - xPoints)
        initialHeightPoints = Math.min(Math.max(18, size.height * 0.04),
            Math.min(100, size.height) - yPoints)
        text = ""
        colors = []
        fontFamilies = []
        typingFamilyPosition = -1
        cursorPosition = 0
        typingPosition = -1
        fontFamily = editController.headerTextFontFamily()
        baseFontFamily = fontFamily
        fontSize = 12
        fontBold = false
        fontItalic = false
        fontUnderline = false
        textColor = "black"
        errorText = ""
        armed = false
        page = pageIndex
        focusRequested()
    }

    function cancel() {
        armed = false
        page = -1
        text = ""
        colors = []
        fontFamilies = []
        typingFamilyPosition = -1
        cursorPosition = 0
        typingPosition = -1
        errorText = ""
        inputFocused = false
        xPoints = 0
        yPoints = 0
        widthPoints = 0
        initialHeightPoints = 0
    }

    function submit(pages) {
        InputMethod.commit()
        synchronizeRequested()
        if (!canSubmit)
            return
        errorText = ""
        submitting = true
        const succeeded = editController.addHeaderText(
            text, pages, xPoints, yPoints, widthPoints, {
                fontFamily: fontFamily,
                fontSize: fontSize,
                fontBold: fontBold,
                fontItalic: fontItalic,
                fontUnderline: fontUnderline,
                color: textColor,
                colors: colors,
                fontFamilies: fontFamilies
            })
        submitting = false
        if (succeeded) {
            cancel()
        } else if (errorText.length === 0) {
            errorText = footerMode
                //% "Could not add footer text."
                ? qsTrId("AddFooterTextFailed")
                //% "Could not add header text."
                : qsTrId("AddHeaderTextFailed")
        }
    }

    onFooterModeChanged: cancel()
    onDocumentChanged: cancel()
    onEditControllerChanged: cancel()
    onTextChanged: {
        if (text.length > 700) {
            const lastCodeUnit = text.charCodeAt(699)
            const limit = lastCodeUnit >= 0xD800 && lastCodeUnit <= 0xDBFF ? 699 : 700
            text = text.slice(0, limit)
        }
        errorText = ""
    }

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
