import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    required property PDFSearchController searchController
    property bool hasPendingPanelEdits: false
    readonly property bool hasUnsavedChanges:
        editController.hasDrafts || hasPendingPanelEdits

    signal openRequested()
    signal saveRequested()
    signal saveAsRequested()
    signal pageRequested(int page)
    signal zoomOutRequested()
    signal zoomInRequested()
    signal fitWidthRequested()
    signal rotateRequested(int quarterTurns)

    clip: true
    contentWidth: ribbonRow.implicitWidth + 16
    contentHeight: height
    flickableDirection: Flickable.HorizontalFlick
    boundsBehavior: Flickable.StopAtBounds

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
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Previous"
                text: qsTrId("Previous")
                //% "Previous Page"
                toolTipText: qsTrId("PreviousPage")
                iconSource: "../assets/icons/chevron-left.svg"
                enabled: root.document.currentPage > 0
                onClicked: root.pageRequested(root.document.currentPage - 1)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Next"
                text: qsTrId("Next")
                //% "Next Page"
                toolTipText: qsTrId("NextPage")
                iconSource: "../assets/icons/chevron-right.svg"
                enabled: root.document.loaded && root.document.currentPage + 1 < root.document.pageCount
                onClicked: root.pageRequested(root.document.currentPage + 1)
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
            //% "Search"
            title: qsTrId("Search")

            SearchRibbonControl {
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
