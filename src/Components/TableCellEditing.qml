pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

QtObject {
    id: root
    objectName: "tableCellEditingState"

    required property PDFEditController controller
    property bool active: false
    property bool submitting: false
    property int page: -1
    property string objectId: ""
    property int row: 0
    property int column: 0
    property int rows: 0
    property int columns: 0
    property string text: ""
    property string errorText: ""
    property var cellFormat: ({})
    property var editorState: ({})
    property bool bold: false
    property bool italic: false
    property bool underline: false
    property rect cellRect: Qt.rect(0, 0, 0, 0)
    property string fontFamily: ""
    property real fontSize: 12
    property int textRotation: 0
    property int focusGeneration: 0
    property bool inputFocused: false
    property TableCellEditor editorItem: null
    property bool formatInputValid: true

    signal focusRequested()
    signal formatCommitRequested()

    function begin(targetPage, targetObjectId, targetRow, targetColumn) {
        if (active && !commit())
            return false
        if (!controller || controller.saving
                || controller.selectedPage !== targetPage
                || controller.selectedObjectId !== targetObjectId)
            return false
        const draft = controller.selectedDraftPreview()
        const nextRows = Number(draft.tableRows || 0)
        const nextColumns = Number(draft.tableColumns || 0)
        const index = targetRow * nextColumns + targetColumn
        const rects = draft.tableCellRects || []
        if (draft.shapeType !== PDFEditController.TableShape || draft.tableFrameOnly
                || targetRow < 0 || targetRow >= nextRows
                || targetColumn < 0 || targetColumn >= nextColumns
                || index >= rects.length)
            return false
        page = targetPage
        objectId = targetObjectId
        row = targetRow
        column = targetColumn
        rows = nextRows
        columns = nextColumns
        cellRect = rects[index]
        fontFamily = draft.tableFontFamily || controller.headerTextFontFamily()
        fontSize = draft.tableFontSize || 12
        text = (draft.tableCellTexts || [])[index] || ""
        const formats = draft.tableCellFormats || []
        const plainStyle = { family: fontFamily, bold: false, italic: false,
            underline: false, fontSize: fontSize }
        cellFormat = formats.length > index ? formats[index] : {
            defaultStyle: plainStyle,
            spans: text.length > 0 ? [{ start: 0, length: text.length,
                family: fontFamily, bold: false, italic: false,
                underline: false, fontSize: fontSize }] : []
        }
        const initialStyle = cellFormat.defaultStyle
        textRotation = Number(cellFormat.rotation || 0)
        bold = initialStyle.bold
        italic = initialStyle.italic
        underline = initialStyle.underline
        fontFamily = initialStyle.family
        fontSize = initialStyle.fontSize || 12
        editorState = { cursorPosition: text.length, selectionStart: text.length,
            selectionEnd: text.length, typingPosition: -1 }
        errorText = ""
        formatInputValid = true
        ++focusGeneration
        active = true
        focusRequested()
        return true
    }

    function commit() {
        if (!active || submitting)
            return true
        if (!controller || controller.saving)
            return false
        // Qt exposes QInputMethod as QObject in QML type metadata.
        // qmllint disable missing-property
        Qt.inputMethod.commit()
        // qmllint enable missing-property
        formatCommitRequested()
        if (!formatInputValid)
            return false
        if (editorItem)
            editorItem.synchronize()
        submitting = true
        if (controller.selectedPage !== page || controller.selectedObjectId !== objectId) {
            if (!controller.selectCandidate(page, objectId)) {
                submitting = false
                //% "This cell is no longer available. Select the table again."
                errorText = qsTrId("TableCellUnavailable")
                focusRequested()
                return false
            }
        }
        const error = controller.setSelectedTableCellContent(row, column, text, cellFormat)
        submitting = false
        if (error.length > 0) {
            errorText = error
            focusRequested()
            return false
        }
        active = false
        errorText = ""
        inputFocused = false
        return true
    }

    function cancel() {
        active = false
        errorText = ""
        inputFocused = false
        text = ""
        cellFormat = ({})
        editorState = ({})
        formatInputValid = true
    }

    function advance(backward) {
        const targetPage = page
        const targetId = objectId
        const nextIndex = row * columns + column + (backward ? -1 : 1)
        const columnCount = columns
        const cellCount = rows * columns
        if (!commit())
            return false
        if (nextIndex >= 0 && nextIndex < cellCount)
            return begin(targetPage, targetId, Math.floor(nextIndex / columnCount), nextIndex % columnCount)
        return true
    }

    onControllerChanged: cancel()
    property Connections historyConnection: Connections {
        target: root.controller
        function onHistoryStateChanged() {
            if (root.active && !root.submitting)
                root.cancel()
        }
    }
}
