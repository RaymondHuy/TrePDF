pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    property bool commentsPanelVisible: false

    signal pageRequested(int page)
    signal toolRequested(string toolName)
    signal commentsPanelToggleRequested()
    signal previousCommentRequested()
    signal nextCommentRequested()

    readonly property real highlightOpacity: 96 / 255
    readonly property real lineMarkupOpacity: 192 / 255
    readonly property bool hasComments:
        editController.commentItems.length > 0
    readonly property var highlightColors: [
        "#ffeb3b", "#ffc107", "#ff9800", "#ff7043", "#ef5350",
        "#ec407a", "#ab47bc", "#42a5f5", "#26c6da", "#66bb6a",
        "#fff176", "#b39ddb", "#f48fb1", "#90caf9", "#a5d6a7"
    ]
    readonly property var lineMarkupColors: [
        "#c62828", "#ad1457", "#6a1b9a", "#283593", "#1565c0",
        "#0277bd", "#00838f", "#00695c", "#2e7d32", "#558b2f",
        "#9e9d24", "#f9a825", "#ef6c00", "#d84315", "#455a64"
    ]
    property int pendingColorTool: PDFEditController.HighlightTool
    property string pendingColorDialogTitle: ""

    clip: true
    contentWidth: ribbonRow.implicitWidth + 16
    contentHeight: height
    flickableDirection: Flickable.HorizontalFlick
    boundsBehavior: Flickable.StopAtBounds

    function sameHighlightColor(left, right) {
        return Math.abs(left.r - right.r) < 0.001
            && Math.abs(left.g - right.g) < 0.001
            && Math.abs(left.b - right.b) < 0.001
    }

    function opaqueHighlightColor(color) {
        return Qt.rgba(color.r, color.g, color.b, 1)
    }

    function markupColorWithOpacity(color, tool) {
        const opacity = tool === PDFEditController.UnderlineTool
                || tool === PDFEditController.SquigglyTool
            ? lineMarkupOpacity
            : highlightOpacity
        return Qt.rgba(
            color.r,
            color.g,
            color.b,
            opacity)
    }

    function highlightCheckColor(color) {
        const luminance = color.r * 0.299
            + color.g * 0.587
            + color.b * 0.114
        return luminance > 0.62 ? "#35363a" : "#ffffff"
    }

    function markupColor(tool) {
        if (tool === PDFEditController.UnderlineTool)
            return editController.underlineColor
        if (tool === PDFEditController.SquigglyTool)
            return editController.squigglyColor
        return editController.highlightColor
    }

    function selectMarkupColor(color, tool) {
        highlightColorPopup.close()
        underlineColorPopup.close()
        squigglyColorPopup.close()
        const selectedColor = markupColorWithOpacity(color, tool)
        if (tool === PDFEditController.UnderlineTool) {
            editController.underlineColor = selectedColor
        } else if (tool === PDFEditController.SquigglyTool) {
            editController.squigglyColor = selectedColor
        } else {
            editController.highlightColor = selectedColor
        }
        editController.tool = tool
    }

    function openMarkupColorDialog(tool, title) {
        highlightColorPopup.close()
        underlineColorPopup.close()
        squigglyColorPopup.close()
        pendingColorTool = tool
        pendingColorDialogTitle = title
        markupColorDialog.selectedColor = opaqueHighlightColor(
            markupColor(tool))
        markupColorDialog.open()
    }

    function closeColorPickers() {
        if (highlightColorPopup)
            highlightColorPopup.close()
        if (underlineColorPopup)
            underlineColorPopup.close()
        if (squigglyColorPopup)
            squigglyColorPopup.close()
        if (markupColorDialog)
            markupColorDialog.close()
    }

    function isCommentKind(kind) {
        return kind === "comment"
            || kind === "stickyNote"
            || kind === "note"
            || kind === "highlight"
            || kind === "underline"
            || kind === "squiggly"
            || kind === "strikeout"
            || kind === "freeText"
    }

    onDocumentChanged: closeColorPickers()
    onEditControllerChanged: closeColorPickers()
    Component.onDestruction: closeColorPickers()

    component MarkupColorSwatch: ToolButton {
        id: swatch

        required property color swatchColor
        required property int targetTool
        required property string accessibleName

        width: 38
        height: 26
        padding: 3
        checkable: true
        checked: root.sameHighlightColor(
            root.markupColor(swatch.targetTool),
            swatchColor)
        focusPolicy: Qt.TabFocus
        Accessible.name: swatch.accessibleName
        onClicked: root.selectMarkupColor(swatchColor, swatch.targetTool)

        contentItem: Rectangle {
            radius: 3
            color: swatch.swatchColor
            border.width: 1
            border.color: "#c9c3be"
            Accessible.ignored: true

            Label {
                anchors.centerIn: parent
                visible: swatch.checked
                text: "\u2713"
                color: root.highlightCheckColor(swatch.swatchColor)
                font.pixelSize: 13
                font.bold: true
                Accessible.ignored: true
            }
        }

        background: Rectangle {
            radius: 5
            color: swatch.down
                ? "#f5dfcf"
                : swatch.hovered || swatch.visualFocus || swatch.checked
                    ? root.theme.foxitPaleOrange
                    : "transparent"
            border.width: swatch.checked
                ? 2
                : swatch.hovered || swatch.visualFocus ? 1 : 0
            border.color: swatch.checked
                ? root.theme.foxitDarkOrange
                : root.theme.foxitBorderOrange
        }
    }

    component MarkupColorPopup: Popup {
        id: colorPopup

        required property Item anchorButton
        required property var paletteColors
        required property int targetTool
        required property string pickerTitle

        x: 0
        y: anchorButton.height
        width: 234
        padding: 12
        focus: true
        closePolicy: Popup.CloseOnEscape
            | Popup.CloseOnPressOutsideParent

        contentItem: Column {
            spacing: 10

            Grid {
                columns: 5
                columnSpacing: 5
                rowSpacing: 5

                Repeater {
                    model: colorPopup.paletteColors

                    delegate: MarkupColorSwatch {
                        required property var modelData

                        swatchColor: modelData
                        targetTool: colorPopup.targetTool
                        accessibleName: colorPopup.pickerTitle
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: "#eee8e4"
                Accessible.ignored: true
            }

            ToolButton {
                id: moreColorsButton

                width: parent.width
                height: 32
                leftPadding: 7
                rightPadding: 7
                //% "More colors…"
                text: qsTrId("HighlightMoreColors")
                onClicked: root.openMarkupColorDialog(
                    colorPopup.targetTool,
                    colorPopup.pickerTitle)

                contentItem: Row {
                    spacing: 8

                    Grid {
                        y: Math.round((parent.height - height) / 2)
                        columns: 2
                        columnSpacing: 1
                        rowSpacing: 1
                        Accessible.ignored: true

                        Repeater {
                            model: [
                                "#ffeb3b", "#ec407a",
                                "#42a5f5", "#66bb6a"
                            ]

                            delegate: Rectangle {
                                required property var modelData

                                width: 6
                                height: 6
                                radius: 1
                                color: modelData
                                Accessible.ignored: true
                            }
                        }
                    }

                    Label {
                        height: parent.height
                        text: moreColorsButton.text
                        color: root.theme.ribbonTextColor
                        font.pixelSize: 11
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                background: Rectangle {
                    radius: 5
                    color: moreColorsButton.down
                        ? "#f5dfcf"
                        : moreColorsButton.hovered
                            || moreColorsButton.visualFocus
                            ? root.theme.foxitPaleOrange
                            : "transparent"
                    border.width: moreColorsButton.visualFocus ? 1 : 0
                    border.color: root.theme.foxitBorderOrange
                }
            }
        }

        background: Rectangle {
            radius: 8
            color: root.theme.ribbonSurface
            border.width: 1
            border.color: root.theme.ribbonBorder
        }
    }

    Row {
        id: ribbonRow

        x: 8
        height: 76

        RibbonGroup {
            theme: root.theme
            //% "Notes"
            title: qsTrId("Notes")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Sticky Note"
                text: qsTrId("StickyNote")
                //% "Add sticky note"
                toolTipText: qsTrId("AddStickyNote")
                iconSource: "../assets/icons/comment.svg"
                enabled: root.document.loaded
                    && !root.editController.saving
                primary: root.editController.tool
                    === PDFEditController.StickyNoteTool
                onClicked: {
                    root.editController.tool
                        = PDFEditController.StickyNoteTool
                }
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Free Text"
                text: qsTrId("FreeText")
                //% "Add a free-text comment"
                toolTipText: qsTrId("AddFreeTextComment")
                iconSource: "../assets/icons/type.svg"
                enabled: root.document.loaded
                    && !root.editController.saving
                primary: root.editController.tool
                    === PDFEditController.FreeTextTool
                onClicked: {
                    if (!root.commentsPanelVisible)
                        root.commentsPanelToggleRequested()
                    root.editController.tool
                        = PDFEditController.FreeTextTool
                }
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Markup"
            title: qsTrId("Markup")

            RibbonButton {
                id: highlightButton

                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Highlight"
                text: qsTrId("Highlight")
                iconSource: "../assets/icons/highlight.svg"
                preserveIconColors: true
                menuIndicatorVisible: true
                spacing: 6
                //% "Choose highlight color"
                menuToolTipText: qsTrId("ChooseHighlightColor")
                enabled: root.document.loaded
                    && !root.editController.saving
                primary: root.editController.tool
                    === PDFEditController.HighlightTool
                onClicked: {
                    highlightColorPopup.close()
                    root.editController.tool
                        = PDFEditController.HighlightTool
                }
                onMenuRequested: highlightColorPopup.open()
                onEnabledChanged: {
                    if (!enabled)
                        highlightColorPopup.close()
                }

                Rectangle {
                    anchors.top: highlightButton.contentItem.top
                    anchors.horizontalCenter:
                        highlightButton.contentItem.horizontalCenter
                    anchors.topMargin:
                        (highlightButton.contentItem.height
                            - highlightButton.implicitContentHeight) / 2
                        + highlightButton.icon.height
                        + (highlightButton.spacing - height) / 2
                    width: highlightButton.icon.width
                    height: 5
                    radius: 1
                    color: root.opaqueHighlightColor(
                        root.editController.highlightColor)
                    Accessible.ignored: true
                }

                MarkupColorPopup {
                    id: highlightColorPopup

                    anchorButton: highlightButton
                    paletteColors: root.highlightColors
                    targetTool: PDFEditController.HighlightTool
                    //% "Choose highlight color"
                    pickerTitle: qsTrId("ChooseHighlightColor")
                }
            }

            RibbonButton {
                id: underlineButton

                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Underline"
                text: qsTrId("Underline")
                iconSource: "../assets/icons/underline.svg"
                menuIndicatorVisible: true
                spacing: 6
                //% "Choose underline color"
                menuToolTipText: qsTrId("ChooseUnderlineColor")
                enabled: root.document.loaded
                    && !root.editController.saving
                primary: root.editController.tool
                    === PDFEditController.UnderlineTool
                onClicked: {
                    underlineColorPopup.close()
                    root.editController.tool
                        = PDFEditController.UnderlineTool
                }
                onMenuRequested: underlineColorPopup.open()
                onEnabledChanged: {
                    if (!enabled)
                        underlineColorPopup.close()
                }

                Rectangle {
                    anchors.top: underlineButton.contentItem.top
                    anchors.horizontalCenter:
                        underlineButton.contentItem.horizontalCenter
                    anchors.topMargin:
                        (underlineButton.contentItem.height
                            - underlineButton.implicitContentHeight) / 2
                        + underlineButton.icon.height
                        + (underlineButton.spacing - height) / 2
                    width: underlineButton.icon.width
                    height: 5
                    radius: 1
                    color: root.opaqueHighlightColor(
                        root.editController.underlineColor)
                    Accessible.ignored: true
                }

                MarkupColorPopup {
                    id: underlineColorPopup

                    anchorButton: underlineButton
                    paletteColors: root.lineMarkupColors
                    targetTool: PDFEditController.UnderlineTool
                    //% "Choose underline color"
                    pickerTitle: qsTrId("ChooseUnderlineColor")
                }
            }

            RibbonButton {
                id: squigglyButton

                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Squiggly"
                text: qsTrId("Squiggly")
                iconSource: "../assets/icons/squiggly.svg"
                menuIndicatorVisible: true
                spacing: 6
                //% "Choose squiggly color"
                menuToolTipText: qsTrId("ChooseSquigglyColor")
                enabled: root.document.loaded
                    && !root.editController.saving
                primary: root.editController.tool
                    === PDFEditController.SquigglyTool
                onClicked: {
                    squigglyColorPopup.close()
                    root.editController.tool
                        = PDFEditController.SquigglyTool
                }
                onMenuRequested: squigglyColorPopup.open()
                onEnabledChanged: {
                    if (!enabled)
                        squigglyColorPopup.close()
                }

                Rectangle {
                    anchors.top: squigglyButton.contentItem.top
                    anchors.horizontalCenter:
                        squigglyButton.contentItem.horizontalCenter
                    anchors.topMargin:
                        (squigglyButton.contentItem.height
                            - squigglyButton.implicitContentHeight) / 2
                        + squigglyButton.icon.height
                        + (squigglyButton.spacing - height) / 2
                    width: squigglyButton.icon.width
                    height: 5
                    radius: 1
                    color: root.opaqueHighlightColor(
                        root.editController.squigglyColor)
                    Accessible.ignored: true
                }

                MarkupColorPopup {
                    id: squigglyColorPopup

                    anchorButton: squigglyButton
                    paletteColors: root.lineMarkupColors
                    targetTool: PDFEditController.SquigglyTool
                    //% "Choose squiggly color"
                    pickerTitle: qsTrId("ChooseSquigglyColor")
                }
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Strikeout"
                text: qsTrId("Strikeout")
                iconSource: "../assets/icons/strikethrough.svg"
                enabled: root.document.loaded
                    && !root.editController.saving
                primary: root.editController.tool
                    === PDFEditController.StrikeoutTool
                onClicked: {
                    root.editController.tool
                        = PDFEditController.StrikeoutTool
                }
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Manage"
            title: qsTrId("ManageComments")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "List"
                text: qsTrId("CommentList")
                //% "Show or hide the comments list"
                toolTipText: qsTrId("ToggleCommentsList")
                iconSource: "../assets/icons/panel-left.svg"
                enabled: root.document.loaded
                    && !root.editController.saving
                primary: root.commentsPanelVisible
                onClicked: root.commentsPanelToggleRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Previous"
                text: qsTrId("Previous")
                //% "Previous comment"
                toolTipText: qsTrId("PreviousComment")
                iconSource: "../assets/icons/chevron-left.svg"
                enabled: root.document.loaded
                    && !root.editController.saving
                    && root.hasComments
                onClicked: root.previousCommentRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Next"
                text: qsTrId("Next")
                //% "Next comment"
                toolTipText: qsTrId("NextComment")
                iconSource: "../assets/icons/chevron-right.svg"
                enabled: root.document.loaded
                    && !root.editController.saving
                    && root.hasComments
                onClicked: root.nextCommentRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Delete"
                text: qsTrId("Delete")
                //% "Delete selected comment"
                toolTipText: qsTrId("DeleteSelectedComment")
                iconSource: "../assets/icons/remove-text.svg"
                enabled: root.document.loaded
                    && !root.editController.saving
                    && root.isCommentKind(
                        root.editController.selectedKind)
                onClicked: {
                    if (root.isCommentKind(
                            root.editController.selectedKind)) {
                        root.editController.removeComment(
                            root.editController.selectedPage,
                            root.editController.selectedObjectId)
                    }
                }
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "History"
            title: qsTrId("History")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Undo"
                text: qsTrId("Undo")
                iconSource: "../assets/icons/undo-2.svg"
                enabled: root.document.loaded
                    && root.editController.canUndo
                    && !root.editController.saving
                    && !root.editController.textEditActive
                onClicked: {
                    const page = root.editController.undo()
                    if (page >= 0)
                        root.pageRequested(page)
                }
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Redo"
                text: qsTrId("Redo")
                iconSource: "../assets/icons/redo-2.svg"
                enabled: root.document.loaded
                    && root.editController.canRedo
                    && !root.editController.saving
                    && !root.editController.textEditActive
                onClicked: {
                    const page = root.editController.redo()
                    if (page >= 0)
                        root.pageRequested(page)
                }
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }

    ColorDialog {
        id: markupColorDialog

        title: root.pendingColorDialogTitle
        onAccepted: root.selectMarkupColor(
            selectedColor,
            root.pendingColorTool)
    }
}
