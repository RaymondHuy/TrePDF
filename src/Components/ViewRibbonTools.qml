pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Flickable {
    id: root

    required property AppTheme theme
    required property bool documentLoaded
    property bool hasUnsavedChanges: false
    property bool saving: false
    property bool canNavigateBackward: false
    property bool canNavigateForward: false
    property int pageDisplayMode: ReaderModes.ContinuousMode
    property bool separateCoverPage: false
    property bool splitViewEnabled: false
    property int splitViewMode: ReaderModes.NoSplit
    property bool openAvailable: true
    property bool saveAvailable: true
    property bool saveAsAvailable: true
    property bool rotationAvailable: true
    property bool pageLayoutAvailable: true
    property bool searchAvailable: true
    property var allowedPageDisplayModes: [ReaderModes.SinglePageMode,
        ReaderModes.ContinuousMode, ReaderModes.FacingMode,
        ReaderModes.ContinuousFacingMode]
    property bool splitViewSupported: true
    property Component searchContent

    signal searchFocusRequested()
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
        if (!searchAvailable || searchLoader.status !== Loader.Ready)
            return
        ribbonRow.forceLayout()
        const left = searchLoader.mapToItem(contentItem, 0, 0).x
        contentX = Math.max(0, Math.min(contentWidth - width,
            left + searchLoader.width + 8 - width))
        searchFocusRequested()
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
                documentLoaded: root.documentLoaded
                //% "Open"
                text: qsTrId("Open")
                //% "Open PDF"
                toolTipText: qsTrId("OpenPdf")
                iconSource: "../assets/icons/folder-open.svg"
                requiresDocument: false
                enabled: root.openAvailable
                primary: true
                onClicked: root.openRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Save"
                text: qsTrId("Save")
                //% "Save PDF"
                toolTipText: qsTrId("SavePdf")
                iconSource: "../assets/icons/save.svg"
                enabled: root.documentLoaded && root.saveAvailable
                    && root.hasUnsavedChanges
                    && !root.saving
                primary: root.hasUnsavedChanges
                onClicked: root.saveRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Save As"
                text: qsTrId("SaveAs")
                //% "Save PDF As"
                toolTipText: qsTrId("SavePdfAs")
                iconSource: "../assets/icons/save.svg"
                enabled: root.documentLoaded && root.saveAsAvailable && !root.saving
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
                documentLoaded: root.documentLoaded
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
                documentLoaded: root.documentLoaded
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
                documentLoaded: root.documentLoaded
                //% "Zoom Out"
                text: qsTrId("ZoomOut")
                iconSource: "../assets/icons/zoom-out.svg"
                onClicked: root.zoomOutRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Zoom In"
                text: qsTrId("ZoomIn")
                iconSource: "../assets/icons/zoom-in.svg"
                onClicked: root.zoomInRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
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
                documentLoaded: root.documentLoaded
                //% "Rotate Left"
                text: qsTrId("RotateLeft")
                iconSource: "../assets/icons/rotate-ccw.svg"
                enabled: root.documentLoaded && root.rotationAvailable
                onClicked: root.rotateRequested(-1)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Rotate Right"
                text: qsTrId("RotateRight")
                iconSource: "../assets/icons/rotate-cw.svg"
                enabled: root.documentLoaded && root.rotationAvailable
                onClicked: root.rotateRequested(1)
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Page Display"
            title: qsTrId("PageDisplay")

            PageDisplayControl {
                theme: root.theme
                documentLoaded: root.documentLoaded
                enabled: root.documentLoaded && root.pageLayoutAvailable
                pageDisplayMode: root.pageDisplayMode
                separateCoverPage: root.separateCoverPage
                splitViewEnabled: root.splitViewEnabled
                splitViewMode: root.splitViewMode
                allowedPageDisplayModes: root.allowedPageDisplayModes
                splitViewSupported: root.splitViewSupported
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

            visible: root.searchContent !== null
            enabled: root.searchAvailable

            Loader {
                id: searchLoader
                sourceComponent: root.searchContent
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
