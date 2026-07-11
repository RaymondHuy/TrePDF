import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document

    signal openRequested()
    signal pageRequested(int page)
    signal zoomOutRequested()
    signal zoomInRequested()
    signal fitWidthRequested()
    signal toolRequested(string toolName)

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
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Rotate Right"
                text: qsTrId("RotateRight")
                iconSource: "../assets/icons/rotate-cw.svg"
                onClicked: root.toolRequested(text)
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Find & Panels"
            title: qsTrId("FindAndPanels")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Thumbnails"
                text: qsTrId("Thumbnails")
                iconSource: "../assets/icons/panel-left.svg"
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Search"
                text: qsTrId("Search")
                iconSource: "../assets/icons/search.svg"
                onClicked: root.toolRequested(text)
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
