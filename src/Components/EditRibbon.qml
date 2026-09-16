pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController
    property bool hasPendingPanelEdits: false

    signal pageRequested(int page)
    signal deletePageRequested(int page)
    signal undoRequested()
    signal redoRequested()
    signal toolRequested(string toolName)

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
                id: linkButton

                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Link"
                text: qsTrId("Link")
                iconSource: "../assets/icons/link-2.svg"
                menuIndicatorVisible: true
                menuToolTipText: text
                enabled: root.document.loaded && !root.editController.saving
                primary: linkMenuLoader.active
                onClicked: linkMenuLoader.active = true
                onMenuRequested: linkMenuLoader.active = true
                onEnabledChanged: {
                    if (!enabled)
                        linkMenuLoader.active = false
                }
                onVisibleChanged: {
                    if (!visible)
                        linkMenuLoader.active = false
                }

                Connections {
                    target: root
                    function onDocumentChanged() {
                        linkMenuLoader.active = false
                    }
                }

                Loader {
                    id: linkMenuLoader

                    active: false
                    onLoaded: {
                        if (status === Loader.Ready && item)
                            (item as Menu).open()
                    }

                    sourceComponent: Menu {
                        parent: linkButton
                        y: linkButton.height
                        width: 280
                        padding: 4
                        popupType: Popup.Item
                        focus: true
                        closePolicy: Popup.CloseOnEscape
                            | Popup.CloseOnPressOutsideParent
                        onClosed: linkMenuLoader.active = false

                        delegate: MenuItem {
                            id: linkMenuItem

                            implicitHeight: 32
                            leftPadding: 12
                            rightPadding: 12
                            font.pixelSize: 12
                            palette.text: root.theme.ribbonTextColor
                            palette.highlightedText: root.theme.ribbonTextColor

                            background: Rectangle {
                                radius: 4
                                color: linkMenuItem.highlighted
                                    ? root.theme.foxitPaleOrange : "transparent"
                            }
                        }

                        Action {
                            //% "Add or edit a link"
                            text: qsTrId("AddOrEditLink")
                        }

                        Action {
                            text: root.editController.creatingWebLinks
                                //% "Cancel creating links"
                                ? qsTrId("CancelCreatingWebLinks")
                                //% "Auto-create links from URLs"
                                : qsTrId("AutoCreateLinksFromUrls")
                            enabled: root.editController.creatingWebLinks
                                || !root.hasPendingPanelEdits
                            onTriggered: {
                                if (root.editController.creatingWebLinks)
                                    root.editController.cancelWebLinkCreation()
                                else
                                    root.editController.autoCreateWebLinks()
                            }
                        }

                        Action {
                            //% "Remove web links"
                            text: qsTrId("RemoveWebLinks")
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
                onClicked: root.undoRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Redo"
                text: qsTrId("Redo")
                iconSource: "../assets/icons/redo-2.svg"
                enabled: root.document.loaded
                    && root.editController.canRedo
                    && !root.hasPendingPanelEdits
                    && !root.editController.saving
                    && !root.editController.textEditActive
                onClicked: root.redoRequested()
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }

}
