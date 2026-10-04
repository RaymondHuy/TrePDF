import QtQuick
import TrePDF

Item {
    required property AppTheme theme
    required property bool documentLoaded
    required property bool allowsTextCopy
    required property bool hasDrafts
    required property bool saving
    required property bool textEditing
    required property bool formTextFocused
    required property bool viewTabActive
    required property real zoom
    required property PDFTextSelectionController selectionController
    readonly property bool textSelectionEnabled: true

    signal zoomRequested(real value)
    signal activated()
}
