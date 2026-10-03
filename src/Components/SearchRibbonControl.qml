pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFSearchController controller
    property bool compositionPending: false
    property PDFSearchController compositionController: null

    implicitWidth: 400
    implicitHeight: 56

    function focusSearch() {
        if (!document.loaded)
            return
        searchField.forceActiveFocus(Qt.ShortcutFocusReason)
        searchField.selectAll()
    }

    onControllerChanged: {
        if (compositionPending && compositionController)
            compositionController.scheduleSearch(compositionController.query)
        compositionPending = false
        compositionController = null
        searchField.text = controller.query
    }

    Component.onDestruction: {
        if (compositionPending && compositionController
                && compositionController.query.length > 0) {
            compositionController.scheduleSearch(compositionController.query)
        }
    }

    function submitSearch() {
        compositionPending = false
        compositionController = null
        if (searchField.text.trim().length === 0) {
            controller.clearSearch()
            return
        }
        controller.search(searchField.text)
    }

    function clearSearch() {
        compositionPending = false
        compositionController = null
        controller.clearSearch()
        searchField.clear()
        searchField.forceActiveFocus()
    }

    function resultStatusText() {
        if (controller.state === PDFSearchController.Searching) {
            //% "Searching…"
            return qsTrId("SearchingDocument")
        }
        if (controller.state === PDFSearchController.Failed) {
            //% "Search failed"
            return qsTrId("DocumentSearchFailed")
        }
        if (controller.query.length === 0)
            return ""
        if (controller.state === PDFSearchController.Limited
                && controller.resultCount === 0) {
            //% "Search incomplete"
            return qsTrId("DocumentSearchIncomplete")
        }
        if (controller.resultCount === 0) {
            //% "No results"
            return qsTrId("NoSearchResults")
        }
        if (controller.state === PDFSearchController.Limited) {
            //% "%1 / %2+"
            return qsTrId("LimitedSearchResultPosition")
                .arg(controller.currentResultIndex + 1)
                .arg(controller.resultCount)
        }
        //% "%1 / %2"
        return qsTrId("SearchResultPosition")
            .arg(controller.currentResultIndex + 1)
            .arg(controller.resultCount)
    }

    Row {
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        TextField {
            id: searchField

            objectName: "documentSearchField"
            width: 190
            height: 34
            enabled: root.document.loaded
            maximumLength: 256
            selectByMouse: true
            activeFocusOnTab: true
            leftPadding: 29
            rightPadding: 8
            //% "Search document"
            placeholderText: qsTrId("SearchDocument")
            Accessible.name: placeholderText
            text: root.controller.query
            color: root.theme.ribbonTextColor
            selectionColor: root.theme.foxitOrange
            selectedTextColor: "white"
            onAccepted: root.submitSearch()
            onTextEdited: {
                if (text.trim().length === 0) {
                    root.compositionPending = false
                    root.compositionController = null
                    root.controller.clearSearch()
                } else if (inputMethodComposing) {
                    root.compositionPending = true
                    root.compositionController = root.controller
                    root.controller.prepareSearchText(text)
                } else {
                    root.compositionPending = false
                    root.compositionController = null
                    root.controller.scheduleSearch(text)
                }
            }
            onInputMethodComposingChanged: {
                if (!inputMethodComposing
                        && root.compositionPending
                        && text.trim().length > 0) {
                    root.compositionPending = false
                    root.compositionController = null
                    root.controller.scheduleSearch(text)
                }
            }

            Keys.onEscapePressed: event => {
                root.clearSearch()
                event.accepted = true
            }

            Image {
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 15
                height: 15
                source: "../assets/icons/search.svg"
                sourceSize: Qt.size(15, 15)
                opacity: searchField.enabled ? 0.8 : 0.4
                Accessible.ignored: true
            }

            background: Rectangle {
                radius: 4
                color: searchField.enabled ? "white" : "#f0eeec"
                border.width: searchField.activeFocus ? 2 : 1
                border.color: searchField.activeFocus
                    ? root.theme.foxitOrange
                    : root.theme.ribbonBorder
            }
        }

        Label {
            width: 82
            height: 34
            text: root.resultStatusText()
            color: root.controller.state === PDFSearchController.Failed
                ? "#b3261e"
                : "#625c58"
            font.pixelSize: 11
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        ToolButton {
            id: previousButton

            objectName: "previousSearchResultButton"
            width: 34
            height: 34
            enabled: root.document.loaded
                && root.controller.resultCount > 0
                && root.controller.state !== PDFSearchController.Searching
            icon.source: "../assets/icons/chevron-left.svg"
            icon.width: 17
            icon.height: 17
            icon.color: enabled ? "#4a4b50" : "#aaa8a6"
            //% "Previous search result"
            Accessible.name: qsTrId("PreviousSearchResult")
            onClicked: root.controller.previousResult()

            background: Rectangle {
                radius: 4
                color: previousButton.down
                    ? "#f5dfcf"
                    : previousButton.hovered
                        ? root.theme.foxitPaleOrange
                        : "transparent"
            }

            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: Accessible.name
        }

        ToolButton {
            id: nextButton

            objectName: "nextSearchResultButton"
            width: 34
            height: 34
            enabled: root.document.loaded
                && root.controller.resultCount > 0
                && root.controller.state !== PDFSearchController.Searching
            icon.source: "../assets/icons/chevron-right.svg"
            icon.width: 17
            icon.height: 17
            icon.color: enabled ? "#4a4b50" : "#aaa8a6"
            //% "Next search result"
            Accessible.name: qsTrId("NextSearchResult")
            onClicked: root.controller.nextResult()

            background: Rectangle {
                radius: 4
                color: nextButton.down
                    ? "#f5dfcf"
                    : nextButton.hovered
                        ? root.theme.foxitPaleOrange
                        : "transparent"
            }

            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: Accessible.name
        }

        ToolButton {
            id: clearButton

            objectName: "clearSearchButton"
            width: 34
            height: 34
            enabled: root.document.loaded
                && (searchField.text.length > 0
                    || root.controller.query.length > 0)
            text: "×"
            font.pixelSize: 20
            palette.buttonText: enabled ? "#4a4b50" : "#aaa8a6"
            //% "Clear search"
            Accessible.name: qsTrId("ClearDocumentSearch")
            onClicked: root.clearSearch()

            background: Rectangle {
                radius: 4
                color: clearButton.down
                    ? "#f5dfcf"
                    : clearButton.hovered
                        ? root.theme.foxitPaleOrange
                        : "transparent"
            }

            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: Accessible.name
        }
    }

    Connections {
        target: root.controller

        function onQueryChanged() {
            if ((root.controller.query.length === 0
                    || !searchField.activeFocus)
                    && searchField.text !== root.controller.query) {
                searchField.text = root.controller.query
            }
        }
    }
}
