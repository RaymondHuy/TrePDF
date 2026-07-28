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

    signal pageRequested(int page)
    signal deletePageRequested(int page)
    signal toolRequested(string toolName)

    readonly property real highlightOpacity: 96 / 255
    readonly property var highlightColors: [
        "#ffeb3b", "#ffc107", "#ff9800", "#ff7043", "#ef5350",
        "#ec407a", "#ab47bc", "#42a5f5", "#26c6da", "#66bb6a",
        "#fff176", "#b39ddb", "#f48fb1", "#90caf9", "#a5d6a7"
    ]
    readonly property var shapeOptions: [
        {
            type: PDFEditController.RectangleShape,
            icon: "../assets/icons/rectangle.svg"
        },
        {
            type: PDFEditController.EllipseShape,
            icon: "../assets/icons/oval.svg"
        },
        {
            type: PDFEditController.LineShape,
            icon: "../assets/icons/line.svg"
        },
        {
            type: PDFEditController.ArrowShape,
            icon: "../assets/icons/arrow.svg"
        },
        {
            type: PDFEditController.TriangleShape,
            icon: "../assets/icons/triangle.svg"
        },
        {
            type: PDFEditController.DiamondShape,
            icon: "../assets/icons/diamond.svg"
        }
    ]

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

    function highlightColorWithOpacity(color) {
        return Qt.rgba(
            color.r,
            color.g,
            color.b,
            highlightOpacity)
    }

    function highlightCheckColor(color) {
        const luminance = color.r * 0.299
            + color.g * 0.587
            + color.b * 0.114
        return luminance > 0.62 ? "#35363a" : "#ffffff"
    }

    function selectHighlightColor(color) {
        highlightColorPopup.close()
        editController.highlightColor = highlightColorWithOpacity(color)
        editController.tool = PDFEditController.HighlightTool
    }

    function shapeName(shapeType) {
        switch (shapeType) {
        case PDFEditController.RectangleShape:
            //% "Rectangle"
            return qsTrId("RectangleShape")
        case PDFEditController.EllipseShape:
            //% "Oval"
            return qsTrId("OvalShape")
        case PDFEditController.LineShape:
            //% "Line"
            return qsTrId("LineShape")
        case PDFEditController.ArrowShape:
            //% "Arrow"
            return qsTrId("ArrowShape")
        case PDFEditController.TriangleShape:
            //% "Triangle"
            return qsTrId("TriangleShape")
        case PDFEditController.DiamondShape:
            //% "Diamond"
            return qsTrId("DiamondShape")
        }
        return ""
    }

    function selectShape(shapeType) {
        shapePopup.close()
        editController.shapeType = shapeType
        editController.tool = PDFEditController.AddShapeTool
    }

    component HighlightColorSwatch: ToolButton {
        id: swatch

        required property color swatchColor

        width: 38
        height: 26
        padding: 3
        checkable: true
        checked: root.sameHighlightColor(
            root.editController.highlightColor,
            swatchColor)
        focusPolicy: Qt.TabFocus
        //% "Choose highlight color"
        Accessible.name: qsTrId("ChooseHighlightColor")
        onClicked: root.selectHighlightColor(swatchColor)

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

    component ShapeOptionButton: ToolButton {
        id: shapeOption

        required property int optionType
        required property url optionIconSource

        width: 92
        height: 66
        padding: 5
        checkable: true
        checked: root.editController.tool === PDFEditController.AddShapeTool
            && root.editController.shapeType === optionType
        focusPolicy: Qt.TabFocus
        Accessible.name: root.shapeName(optionType)
        onClicked: root.selectShape(optionType)

        contentItem: Column {
            spacing: 3

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 46
                height: 30
                source: shapeOption.optionIconSource
                sourceSize: Qt.size(width * 3, height * 3)
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
                Accessible.ignored: true
            }

            Label {
                width: parent.width
                text: root.shapeName(shapeOption.optionType)
                color: root.theme.ribbonTextColor
                font.pixelSize: 10
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
            }
        }

        background: Rectangle {
            radius: 5
            color: shapeOption.down
                ? "#f5dfcf"
                : shapeOption.hovered
                        || shapeOption.visualFocus
                        || shapeOption.checked
                    ? root.theme.foxitPaleOrange
                    : "transparent"
            border.width: shapeOption.checked
                ? 2
                : shapeOption.hovered || shapeOption.visualFocus ? 1 : 0
            border.color: shapeOption.checked
                ? root.theme.foxitDarkOrange
                : root.theme.foxitBorderOrange
        }
    }

    Row {
        id: ribbonRow

        x: 8
        height: 76

        RibbonGroup {
            theme: root.theme
            //% "Content"
            title: qsTrId("Content")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Select"
                text: qsTrId("Select")
                iconSource: "../assets/icons/mouse-pointer-2.svg"
                primary: root.editController.tool === PDFEditController.SelectTool
                onClicked: root.editController.tool = PDFEditController.SelectTool
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Edit Text"
                text: qsTrId("EditText")
                iconSource: "../assets/icons/type.svg"
                primary: root.editController.tool === PDFEditController.EditTextTool
                onClicked: root.editController.tool = PDFEditController.EditTextTool
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Add Text"
                text: qsTrId("AddText")
                iconSource: "../assets/icons/type.svg"
                primary: root.editController.tool === PDFEditController.AddTextTool
                onClicked: root.editController.tool = PDFEditController.AddTextTool
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Objects"
            title: qsTrId("Objects")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Image"
                text: qsTrId("Image")
                //% "Add Image"
                toolTipText: qsTrId("AddImage")
                iconSource: "../assets/icons/image-plus.svg"
                primary: root.editController.tool === PDFEditController.AddImageTool
                onClicked: root.editController.tool = PDFEditController.AddImageTool
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Link"
                text: qsTrId("Link")
                //% "Add Link"
                toolTipText: qsTrId("AddLink")
                iconSource: "../assets/icons/link-2.svg"
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                id: shapeButton

                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Shapes"
                text: qsTrId("Shapes")
                iconSource: "../assets/icons/shapes.svg"
                menuIndicatorVisible: true
                //% "Choose shape"
                menuToolTipText: qsTrId("ChooseShape")
                enabled: root.document.loaded
                    && !root.editController.saving
                primary: root.editController.tool
                    === PDFEditController.AddShapeTool
                onClicked: shapePopup.open()
                onMenuRequested: shapePopup.open()
                onEnabledChanged: {
                    if (!enabled)
                        shapePopup.close()
                }

                Popup {
                    id: shapePopup

                    x: shapeButton.width - width
                    y: shapeButton.height
                    width: 300
                    padding: 8
                    focus: true
                    closePolicy: Popup.CloseOnEscape
                        | Popup.CloseOnPressOutsideParent

                    contentItem: Grid {
                        columns: 3
                        columnSpacing: 4
                        rowSpacing: 4

                        Repeater {
                            model: root.shapeOptions

                            delegate: ShapeOptionButton {
                                required property var modelData

                                optionType: modelData.type
                                optionIconSource: modelData.icon
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
        }

        RibbonGroup {
            theme: root.theme
            //% "Page"
            title: qsTrId("Page")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Add Page"
                text: qsTrId("AddPage")
                //% "Add Blank Page"
                toolTipText: qsTrId("AddBlankPage")
                iconSource: "../assets/icons/add-page.svg"
                preserveIconColors: true
                enabled: root.document.loaded
                    && !root.editController.saving
                onClicked: {
                    const page = root.editController.addBlankPage()
                    if (page >= 0)
                        root.pageRequested(page)
                }
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Delete Page"
                text: qsTrId("DeletePage")
                //% "Delete Current Page"
                toolTipText: qsTrId("DeleteCurrentPage")
                iconSource: "../assets/icons/delete-page.svg"
                preserveIconColors: true
                enabled: root.document.loaded
                    && root.document.pageCount > 1
                    && !root.editController.saving
                onClicked: root.deletePageRequested(root.document.currentPage)
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

                Popup {
                    id: highlightColorPopup

                    x: 0
                    y: highlightButton.height
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
                                model: root.highlightColors

                                delegate: HighlightColorSwatch {
                                    required property var modelData

                                    swatchColor: modelData
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
                            id: moreHighlightColorsButton

                            width: parent.width
                            height: 32
                            leftPadding: 7
                            rightPadding: 7
                            //% "More colors…"
                            text: qsTrId("HighlightMoreColors")
                            onClicked: {
                                highlightColorPopup.close()
                                highlightColorDialog.selectedColor
                                    = root.opaqueHighlightColor(
                                        root.editController.highlightColor)
                                highlightColorDialog.open()
                            }

                            contentItem: Row {
                                spacing: 8

                                Grid {
                                    y: Math.round(
                                        (parent.height - height) / 2)
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
                                    text: moreHighlightColorsButton.text
                                    color: root.theme.ribbonTextColor
                                    font.pixelSize: 11
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            background: Rectangle {
                                radius: 5
                                color: moreHighlightColorsButton.down
                                    ? "#f5dfcf"
                                    : moreHighlightColorsButton.hovered
                                        || moreHighlightColorsButton.visualFocus
                                        ? root.theme.foxitPaleOrange
                                        : "transparent"
                                border.width:
                                    moreHighlightColorsButton.visualFocus
                                        ? 1
                                        : 0
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
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Underline"
                text: qsTrId("Underline")
                iconSource: "../assets/icons/underline.svg"
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Strikeout"
                text: qsTrId("Strikeout")
                iconSource: "../assets/icons/strikethrough.svg"
                onClicked: root.toolRequested(text)
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
        id: highlightColorDialog

        //% "Choose highlight color"
        title: qsTrId("ChooseHighlightColor")
        onAccepted: root.selectHighlightColor(selectedColor)
    }
}
