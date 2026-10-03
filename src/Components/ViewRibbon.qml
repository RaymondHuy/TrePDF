import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    required property PDFSearchController searchController
    property PDFDocumentSession session: null
    property bool hasPendingPanelEdits: false
    property int navigationPage: document.currentPage
    property bool canNavigateBackward: navigationPage > 0
    property bool canNavigateForward:
        document.loaded && navigationPage + 1 < document.pageCount
    readonly property bool hasUnsavedChanges:
        editController.hasDrafts || hasPendingPanelEdits

    signal openRequested()
    signal saveRequested()
    signal saveAsRequested()
    signal pageRequested(int page)
    signal pageStepRequested(int direction)
    signal zoomOutRequested()
    signal zoomInRequested()
    signal fitWidthRequested()
    signal rotateRequested(int quarterTurns)
    signal pageDisplayModeRequested(int mode)
    signal separateCoverPageToggleRequested()
    signal splitViewModeRequested(int mode)
    signal splitViewToggleRequested()

    clip: true
    contentWidth: ribbonRow.implicitWidth + 16
    contentHeight: height
    flickableDirection: Flickable.HorizontalFlick
    boundsBehavior: Flickable.StopAtBounds

    function focusSearch() {
        ribbonRow.forceLayout()
        const left = searchControl.mapToItem(contentItem, 0, 0).x
        contentX = Math.max(0, Math.min(contentWidth - width,
            left + searchControl.width + 8 - width))
        searchControl.focusSearch()
    }

    Row {
        id: ribbonRow

        x: 8
        height: 76

        RibbonGroup {
            theme: root.theme
            //% "File"
            title: qsTrId("File")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Open"
                text: qsTrId("Open")
                //% "Open PDF"
                toolTipText: qsTrId("OpenPdf")
                iconSource: "../assets/icons/folder-open.svg"
                requiresDocument: false
                primary: true
                onClicked: root.openRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Save"
                text: qsTrId("Save")
                //% "Save PDF"
                toolTipText: qsTrId("SavePdf")
                iconSource: "../assets/icons/save.svg"
                enabled: root.document.loaded
                    && root.hasUnsavedChanges
                    && !root.editController.saving
                primary: root.hasUnsavedChanges
                onClicked: root.saveRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Save As"
                text: qsTrId("SaveAs")
                //% "Save PDF As"
                toolTipText: qsTrId("SavePdfAs")
                iconSource: "../assets/icons/save.svg"
                enabled: root.document.loaded && !root.editController.saving
                onClicked: root.saveAsRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Navigation"
            title: qsTrId("Navigation")

            RibbonButton {
                objectName: "previousViewPageButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Previous"
                text: qsTrId("Previous")
                //% "Previous Page"
                toolTipText: qsTrId("PreviousPage")
                iconSource: "../assets/icons/chevron-left.svg"
                enabled: root.canNavigateBackward
                onClicked: root.pageStepRequested(-1)
            }

            RibbonButton {
                objectName: "nextViewPageButton"
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Next"
                text: qsTrId("Next")
                //% "Next Page"
                toolTipText: qsTrId("NextPage")
                iconSource: "../assets/icons/chevron-right.svg"
                enabled: root.canNavigateForward
                onClicked: root.pageStepRequested(1)
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Zoom"
            title: qsTrId("Zoom")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Zoom Out"
                text: qsTrId("ZoomOut")
                iconSource: "../assets/icons/zoom-out.svg"
                onClicked: root.zoomOutRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Zoom In"
                text: qsTrId("ZoomIn")
                iconSource: "../assets/icons/zoom-in.svg"
                onClicked: root.zoomInRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Fit Width"
                text: qsTrId("FitWidth")
                iconSource: "../assets/icons/maximize-2.svg"
                onClicked: root.fitWidthRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Page"
            title: qsTrId("Page")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Rotate Left"
                text: qsTrId("RotateLeft")
                iconSource: "../assets/icons/rotate-ccw.svg"
                onClicked: root.rotateRequested(-1)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Rotate Right"
                text: qsTrId("RotateRight")
                iconSource: "../assets/icons/rotate-cw.svg"
                onClicked: root.rotateRequested(1)
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Page Display"
            title: qsTrId("PageDisplay")

            PageDisplayControl {
                theme: root.theme
                documentLoaded: root.document.loaded
                pageDisplayMode: root.session
                    ? root.session.pageDisplayMode
                    : PDFDocumentSession.ContinuousMode
                separateCoverPage: root.session
                    ? root.session.separateCoverPage
                    : false
                splitViewEnabled: root.session
                    ? root.session.splitViewEnabled
                    : false
                splitViewMode: root.session
                    ? root.session.splitViewMode
                    : PDFDocumentSession.NoSplit
                onPageDisplayModeRequested: mode =>
                    root.pageDisplayModeRequested(mode)
                onSeparateCoverPageToggleRequested:
                    root.separateCoverPageToggleRequested()
                onSplitViewModeRequested: mode =>
                    root.splitViewModeRequested(mode)
                onSplitViewToggleRequested: root.splitViewToggleRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Search"
            title: qsTrId("Search")

            SearchRibbonControl {
                id: searchControl
                theme: root.theme
                document: root.document
                controller: root.searchController
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
