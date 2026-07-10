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
            title: qsTr("File")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Open")
                toolTipText: qsTr("Open PDF")
                iconSource: "../assets/icons/folder-open.svg"
                requiresDocument: false
                primary: true
                onClicked: root.openRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            title: qsTr("Navigation")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Previous")
                toolTipText: qsTr("Previous Page")
                iconSource: "../assets/icons/chevron-left.svg"
                enabled: root.document.currentPage > 0
                onClicked: root.pageRequested(root.document.currentPage - 1)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Next")
                toolTipText: qsTr("Next Page")
                iconSource: "../assets/icons/chevron-right.svg"
                enabled: root.document.loaded && root.document.currentPage + 1 < root.document.pageCount
                onClicked: root.pageRequested(root.document.currentPage + 1)
            }
        }

        RibbonGroup {
            theme: root.theme
            title: qsTr("Zoom")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Zoom Out")
                iconSource: "../assets/icons/zoom-out.svg"
                onClicked: root.zoomOutRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Zoom In")
                iconSource: "../assets/icons/zoom-in.svg"
                onClicked: root.zoomInRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Fit Width")
                iconSource: "../assets/icons/maximize-2.svg"
                onClicked: root.fitWidthRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            title: qsTr("Page")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Rotate Left")
                iconSource: "../assets/icons/rotate-ccw.svg"
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Rotate Right")
                iconSource: "../assets/icons/rotate-cw.svg"
                onClicked: root.toolRequested(text)
            }
        }

        RibbonGroup {
            theme: root.theme
            title: qsTr("Find & Panels")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Thumbnails")
                iconSource: "../assets/icons/panel-left.svg"
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Search")
                iconSource: "../assets/icons/search.svg"
                onClicked: root.toolRequested(text)
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
