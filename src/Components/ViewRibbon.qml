pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

ViewRibbonTools {
    id: root

    required property PDFDocument document
    required property PDFEditController editController
    required property PDFSearchController searchController
    property PDFDocumentSession session: null
    property bool hasPendingPanelEdits: false
    property int navigationPage: document.currentPage

    documentLoaded: document.loaded
    hasUnsavedChanges: editController.hasDrafts || hasPendingPanelEdits
    saving: editController.saving
    canNavigateBackward: navigationPage > 0
    canNavigateForward: document.loaded && navigationPage + 1 < document.pageCount
    pageDisplayMode: session ? session.pageDisplayMode : ReaderModes.ContinuousMode
    separateCoverPage: session ? session.separateCoverPage : false
    splitViewEnabled: session ? session.splitViewEnabled : false
    splitViewMode: session ? session.splitViewMode : ReaderModes.NoSplit

    searchContent: Component {
        SearchRibbonControl {
            id: searchControl
            theme: root.theme
            document: root.document
            controller: root.searchController

            Connections {
                target: root
                function onSearchFocusRequested() {
                    searchControl.focusSearch()
                }
            }
        }
    }
}
