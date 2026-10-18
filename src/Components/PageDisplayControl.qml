pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

RibbonButton {
    id: root
    objectName: "pageDisplayControl"

    property int pageDisplayMode: ReaderModes.ContinuousMode
    property var allowedPageDisplayModes: [
        ReaderModes.SinglePageMode, ReaderModes.ContinuousMode,
        ReaderModes.FacingMode, ReaderModes.ContinuousFacingMode
    ]
    property bool separateCoverPage: false
    property bool splitViewEnabled: false
    property bool splitViewSupported: true
    property int splitViewMode: ReaderModes.NoSplit
    readonly property bool facingMode:
        pageDisplayMode === ReaderModes.FacingMode
        || pageDisplayMode === ReaderModes.ContinuousFacingMode
    readonly property url activeLayoutIcon: {
        if (pageDisplayMode === ReaderModes.SinglePageMode)
            return "../assets/icons/page-single.svg"
        if (pageDisplayMode === ReaderModes.FacingMode)
            return "../assets/icons/page-facing.svg"
        if (pageDisplayMode === ReaderModes.ContinuousFacingMode)
            return "../assets/icons/page-continuous-facing.svg"
        return "../assets/icons/page-continuous.svg"
    }
    readonly property url activeSplitIcon: {
        if (splitViewMode === ReaderModes.HorizontalSplit)
            return "../assets/icons/page-split-horizontal.svg"
        return "../assets/icons/page-split-vertical.svg"
    }

    signal pageDisplayModeRequested(int mode)
    signal separateCoverPageToggleRequested()
    signal splitViewModeRequested(int mode)
    signal splitViewToggleRequested()

    component DisplayOptionButton: ItemDelegate {
        id: optionButton

        required property AppTheme theme
        required property url optionIconSource
        property bool selected: false

        implicitWidth: 240
        implicitHeight: 36
        topPadding: 6
        bottomPadding: 6
        leftPadding: 10
        rightPadding: 10
        spacing: 10
        display: AbstractButton.TextBesideIcon
        checkable: true
        checked: selected
        font.pixelSize: 12
        palette.text: enabled
            ? theme.ribbonTextColor
            : "#a7a4a1"
        icon.source: optionIconSource
        icon.width: 18
        icon.height: 18
        icon.color: enabled ? "#4a4b50" : "#aaa8a6"

        background: Rectangle {
            radius: 5
            color: optionButton.down
                ? optionButton.theme.foxitPaleOrange
                : optionButton.hovered
                        || optionButton.visualFocus
                        || optionButton.selected
                    ? optionButton.theme.foxitPaleOrange
                    : "transparent"
            border.width: optionButton.selected ? 1 : 0
            border.color: optionButton.theme.foxitBorderOrange
        }
    }

    //% "Page Layout"
    text: qsTrId("PageLayout")
    //% "Choose page display mode"
    toolTipText: qsTrId("ChoosePageDisplayMode")
    //% "Choose page display mode"
    menuToolTipText: qsTrId("ChoosePageDisplayMode")
    iconSource: activeLayoutIcon
    menuIndicatorVisible: true
    primary: pageDisplayPopup.visible
    onClicked: pageDisplayPopup.open()
    onMenuRequested: pageDisplayPopup.open()
    onEnabledChanged: {
        if (!enabled) {
            splitMenu.close()
            pageDisplayPopup.close()
        }
    }

    Popup {
        id: pageDisplayPopup
        objectName: "pageDisplayPopup"

        x: root.width - width
        y: root.height
        width: 252
        padding: 6
        focus: true
        popupType: Popup.Item
        closePolicy: Popup.CloseOnEscape
            | Popup.CloseOnPressOutsideParent
        onOpened: singlePageButton.forceActiveFocus()
        onClosed: splitMenu.close()

        contentItem: Column {
            id: optionColumn

            spacing: 2

            DisplayOptionButton {
                id: singlePageButton
                objectName: "singlePageDisplayItem"
                theme: root.theme
                enabled: root.documentLoaded
                    && root.allowedPageDisplayModes.indexOf(ReaderModes.SinglePageMode) >= 0
                optionIconSource: "../assets/icons/page-single.svg"
                selected: root.pageDisplayMode
                    === ReaderModes.SinglePageMode
                //% "Single Page"
                text: qsTrId("PageDisplaySinglePage")
                KeyNavigation.up: splitMainButton
                KeyNavigation.down: continuousButton
                onClicked: {
                    root.pageDisplayModeRequested(
                        ReaderModes.SinglePageMode)
                    pageDisplayPopup.close()
                }
            }

            DisplayOptionButton {
                id: continuousButton
                objectName: "continuousPageDisplayItem"
                theme: root.theme
                enabled: root.documentLoaded
                    && root.allowedPageDisplayModes.indexOf(ReaderModes.ContinuousMode) >= 0
                optionIconSource: "../assets/icons/page-continuous.svg"
                selected: root.pageDisplayMode
                    === ReaderModes.ContinuousMode
                //% "Continuous"
                text: qsTrId("PageDisplayContinuous")
                KeyNavigation.up: singlePageButton
                KeyNavigation.down: facingButton
                onClicked: {
                    root.pageDisplayModeRequested(
                        ReaderModes.ContinuousMode)
                    pageDisplayPopup.close()
                }
            }

            DisplayOptionButton {
                id: facingButton
                objectName: "facingPageDisplayItem"
                theme: root.theme
                enabled: root.documentLoaded
                    && root.allowedPageDisplayModes.indexOf(ReaderModes.FacingMode) >= 0
                optionIconSource: "../assets/icons/page-facing.svg"
                selected: root.pageDisplayMode
                    === ReaderModes.FacingMode
                //% "Facing"
                text: qsTrId("PageDisplayFacing")
                KeyNavigation.up: continuousButton
                KeyNavigation.down: continuousFacingButton
                onClicked: {
                    root.pageDisplayModeRequested(
                        ReaderModes.FacingMode)
                    pageDisplayPopup.close()
                }
            }

            DisplayOptionButton {
                id: continuousFacingButton
                objectName: "continuousFacingPageDisplayItem"
                theme: root.theme
                enabled: root.documentLoaded
                    && root.allowedPageDisplayModes.indexOf(ReaderModes.ContinuousFacingMode) >= 0
                optionIconSource:
                    "../assets/icons/page-continuous-facing.svg"
                selected: root.pageDisplayMode
                    === ReaderModes.ContinuousFacingMode
                //% "Continuous Facing"
                text: qsTrId("PageDisplayContinuousFacing")
                KeyNavigation.up: facingButton
                KeyNavigation.down: separateCoverButton
                onClicked: {
                    root.pageDisplayModeRequested(
                        ReaderModes.ContinuousFacingMode)
                    pageDisplayPopup.close()
                }
            }

            Rectangle {
                width: optionColumn.width
                height: 1
                color: root.theme.ribbonBorder
                Accessible.ignored: true
            }

            DisplayOptionButton {
                id: separateCoverButton
                objectName: "separateCoverPageDisplayItem"
                theme: root.theme
                enabled: root.documentLoaded && root.facingMode
                    && root.allowedPageDisplayModes.indexOf(root.pageDisplayMode) >= 0
                optionIconSource: "../assets/icons/page-separate-cover.svg"
                selected: root.separateCoverPage && root.facingMode
                //% "Separate Cover Page"
                text: qsTrId("PageDisplaySeparateCoverPage")
                KeyNavigation.up: continuousFacingButton
                KeyNavigation.down: splitMainButton
                onClicked: {
                    if (root.facingMode)
                        root.separateCoverPageToggleRequested()
                    pageDisplayPopup.close()
                }
            }

            Item {
                id: splitRow

                width: 240
                height: 36

                Rectangle {
                    anchors.fill: parent
                    radius: 5
                    color: splitMainButton.down || splitMenuButton.down
                        ? root.theme.foxitPaleOrange
                        : splitMainButton.hovered
                                || splitMenuButton.hovered
                                || root.splitViewEnabled
                            ? root.theme.foxitPaleOrange
                            : "transparent"
                    border.width: root.splitViewEnabled ? 1 : 0
                    border.color: root.theme.foxitBorderOrange
                }

                ItemDelegate {
                    id: splitMainButton
                    objectName: "splitPageDisplayItem"

                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.right: splitMenuButton.left
                    enabled: root.documentLoaded && root.splitViewSupported
                    topPadding: 6
                    bottomPadding: 6
                    leftPadding: 10
                    rightPadding: 8
                    spacing: 10
                    display: AbstractButton.TextBesideIcon
                    checkable: true
                    checked: root.splitViewEnabled
                    font.pixelSize: 12
                    palette.text: enabled
                        ? root.theme.ribbonTextColor
                        : "#a7a4a1"
                    icon.source: root.activeSplitIcon
                    icon.width: 18
                    icon.height: 18
                    icon.color: enabled ? "#4a4b50" : "#aaa8a6"
                    //% "Split"
                    text: qsTrId("PageDisplaySplit")
                    KeyNavigation.up: separateCoverButton
                    KeyNavigation.down: singlePageButton
                    KeyNavigation.right: splitMenuButton
                    onClicked: {
                        root.splitViewToggleRequested()
                        pageDisplayPopup.close()
                    }

                    background: Item {}
                }

                ToolButton {
                    id: splitMenuButton
                    objectName: "splitPageDisplayMenuButton"

                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: 34
                    enabled: root.documentLoaded && root.splitViewSupported
                    focusPolicy: Qt.TabFocus
                    text: "\u25b8"
                    font.pixelSize: 16
                    Accessible.name: splitMenuToolTip
                    //% "Choose split view mode"
                    readonly property string splitMenuToolTip:
                        qsTrId("ChooseSplitViewMode")
                    KeyNavigation.up: separateCoverButton
                    KeyNavigation.down: singlePageButton
                    KeyNavigation.left: splitMainButton
                    onClicked: splitMenu.open()

                    contentItem: Label {
                        text: splitMenuButton.text
                        color: splitMenuButton.enabled
                            ? "#4a4b50"
                            : "#aaa8a6"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Item {
                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.topMargin: 6
                            anchors.bottomMargin: 6
                            width: 1
                            color: splitMenuButton.hovered
                                    || root.splitViewEnabled
                                ? root.theme.foxitBorderOrange
                                : "transparent"
                        }
                    }

                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: splitMenuToolTip
                }

                Menu {
                    id: splitMenu
                    objectName: "splitPageDisplayModeMenu"

                    x: splitRow.width
                    y: 0
                    width: 218
                    padding: 4
                    popupType: Popup.Item
                    closePolicy: Popup.CloseOnEscape
                        | Popup.CloseOnPressOutsideParent

                    MenuItem {
                        objectName: "verticalSplitPageDisplayItem"
                        enabled: root.documentLoaded && root.splitViewSupported
                        checkable: true
                        checked: root.splitViewMode
                            === ReaderModes.VerticalSplit
                        //% "Vertical Split"
                        text: qsTrId("PageDisplayVerticalSplit")
                        onTriggered: {
                            root.splitViewModeRequested(
                                ReaderModes.VerticalSplit)
                            pageDisplayPopup.close()
                        }
                    }

                    MenuItem {
                        objectName: "horizontalSplitPageDisplayItem"
                        enabled: root.documentLoaded && root.splitViewSupported
                        checkable: true
                        checked: root.splitViewMode
                            === ReaderModes.HorizontalSplit
                        //% "Horizontal Split"
                        text: qsTrId("PageDisplayHorizontalSplit")
                        onTriggered: {
                            root.splitViewModeRequested(
                                ReaderModes.HorizontalSplit)
                            pageDisplayPopup.close()
                        }
                    }

                    background: Rectangle {
                        radius: 7
                        color: root.theme.ribbonSurface
                        border.width: 1
                        border.color: root.theme.ribbonBorder
                    }
                }
            }
        }

        background: Rectangle {
            radius: 7
            color: root.theme.ribbonSurface
            border.width: 1
            border.color: root.theme.ribbonBorder
        }
    }
}
