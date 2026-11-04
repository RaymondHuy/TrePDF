pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Flickable {
    id: root

    required property AppTheme theme
    property bool documentLoaded: false
    property int pageCount: 0
    property int currentPage: 0
    property var documentContext: null
    property string currentTool: "select"
    property string currentShape: "rectangle"
    property bool saving: false
    property bool canUndo: false
    property bool canRedo: false
    property bool textEditActive: false
    property bool hasPendingPanelEdits: false
    property bool creatingWebLinks: false
    property bool removingWebLinks: false
    property bool loadingEditableLinks: false
    property bool selectAvailable: true
    property bool textEditingAvailable: true
    property bool textAddingAvailable: true
    property bool imageAddingAvailable: true
    property bool shapesAvailable: true
    property bool tablesAvailable: false
    property int tableRows: 3
    property int tableColumns: 3
    property list<string> allowedShapes: ["rectangle", "ellipse", "line", "arrow", "triangle", "diamond"]
    property bool linksAvailable: true
    property bool addPageAvailable: true
    property bool deletePageAvailable: true
    property bool mergeAvailable: true
    property bool mergeVisible: true
    property bool splitAvailable: false
    property bool splitVisible: false

    signal pageRequested(int page)
    signal selectToolRequested(string toolName)
    signal shapeRequested(string shapeName)
    signal tableRequested(int rows, int columns)
    signal addBlankPageRequested()
    signal beginLinkEditingRequested()
    signal autoCreateLinksRequested()
    signal removeLinksRequested()
    signal cancelLinksRequested()
    signal deletePageRequested(int page)
    signal mergePdfRequested()
    signal splitPdfRequested()
    signal undoRequested()
    signal redoRequested()
    signal toolRequested(string toolName)

    readonly property var shapeOptions: [
        {
            type: "rectangle",
            icon: "../assets/icons/rectangle.svg"
        },
        {
            type: "ellipse",
            icon: "../assets/icons/oval.svg"
        },
        {
            type: "line",
            icon: "../assets/icons/line.svg"
        },
        {
            type: "arrow",
            icon: "../assets/icons/arrow.svg"
        },
        {
            type: "triangle",
            icon: "../assets/icons/triangle.svg"
        },
        {
            type: "diamond",
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
        case "rectangle":
            //% "Rectangle"
            return qsTrId("RectangleShape")
        case "ellipse":
            //% "Oval"
            return qsTrId("OvalShape")
        case "line":
            //% "Line"
            return qsTrId("LineShape")
        case "arrow":
            //% "Arrow"
            return qsTrId("ArrowShape")
        case "triangle":
            //% "Triangle"
            return qsTrId("TriangleShape")
        case "diamond":
            //% "Diamond"
            return qsTrId("DiamondShape")
        }
        return ""
    }

    function selectShape(shapeType) {
        shapePopup.close()
        root.shapeRequested(shapeType)
    }

    component ShapeOptionButton: ToolButton {
        id: shapeOption

        required property string optionType
        required property url optionIconSource

        width: 92
        height: 66
        padding: 5
        enabled: root.allowedShapes.indexOf(optionType) >= 0
        checkable: true
        checked: root.currentTool === "addShape"
            && root.currentShape === optionType
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
                documentLoaded: root.documentLoaded
                //% "Select"
                text: qsTrId("Select")
                iconSource: "../assets/icons/mouse-pointer-2.svg"
                objectName: "editSelectButton"
                enabled: root.documentLoaded && root.selectAvailable
                primary: root.currentTool === "select"
                onClicked: root.selectToolRequested("select")
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Edit Text"
                text: qsTrId("EditText")
                iconSource: "../assets/icons/type.svg"
                objectName: "editTextButton"
                enabled: root.documentLoaded && root.textEditingAvailable
                primary: root.currentTool === "editText"
                onClicked: root.selectToolRequested("editText")
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Add Text"
                text: qsTrId("AddText")
                iconSource: "../assets/icons/type.svg"
                objectName: "editAddTextButton"
                enabled: root.documentLoaded && root.textAddingAvailable
                primary: root.currentTool === "addText"
                onClicked: root.selectToolRequested("addText")
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "Objects"
            title: qsTrId("Objects")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Image"
                text: qsTrId("Image")
                //% "Add Image"
                toolTipText: qsTrId("AddImage")
                iconSource: "../assets/icons/image-plus.svg"
                objectName: "editAddImageButton"
                enabled: root.documentLoaded && root.imageAddingAvailable
                primary: root.currentTool === "addImage"
                onClicked: root.selectToolRequested("addImage")
            }

            RibbonButton {
                id: linkButton
                objectName: "editLinkButton"

                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Link"
                text: qsTrId("Link")
                iconSource: "../assets/icons/link-2.svg"
                menuIndicatorVisible: true
                menuToolTipText: text
                enabled: root.documentLoaded && root.linksAvailable && !root.saving
                primary: linkMenuLoader.active
                    || root.currentTool === "link"
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
                    function onDocumentContextChanged() {
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
                        id: linkMenu

                        //% "Create a link: Choose this tool, then drag a rectangle around the text or area you want to make clickable. Enter a web address or choose a page in this PDF.\n\nEdit a link: Double-click an existing link to change its destination or appearance.\n\nMove or resize: Drag the link frame to move it, or drag an edge or corner handle to resize it.\n\nSave your changes, then switch to the View tab to try the link."
                        readonly property string linkHelpText: qsTrId("AddOrEditLinkHelp")

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
                            Accessible.description: action === addOrEditLinkAction
                                ? linkMenu.linkHelpText : ""

                            Loader {
                                active: linkMenu.opened
                                    && linkMenuItem.action === addOrEditLinkAction

                                sourceComponent: HelpToolTip {
                                    theme: root.theme
                                    parent: linkMenuItem
                                    title: addOrEditLinkAction.text
                                    visible: linkMenu.opened && linkMenuItem.enabled
                                        && !linkMenuItem.down
                                        && (linkMenuItem.hovered || linkMenuItem.visualFocus)
                                    text: linkMenu.linkHelpText
                                }
                            }

                            background: Rectangle {
                                radius: 4
                                color: linkMenuItem.highlighted
                                    ? root.theme.foxitPaleOrange : "transparent"
                            }
                        }

                        Action {
                            id: addOrEditLinkAction

                            //% "Add or edit a link"
                            text: qsTrId("AddOrEditLink")
                            enabled: !root.hasPendingPanelEdits
                                && !root.creatingWebLinks
                                && !root.removingWebLinks
                                && !root.loadingEditableLinks
                            onTriggered: root.beginLinkEditingRequested()
                        }

                        Action {
                            text: root.creatingWebLinks
                                //% "Cancel creating links"
                                ? qsTrId("CancelCreatingWebLinks")
                                //% "Auto-create links from URLs"
                                : qsTrId("AutoCreateLinksFromUrls")
                            enabled: root.creatingWebLinks
                                || (!root.hasPendingPanelEdits
                                    && !root.removingWebLinks)
                            onTriggered: {
                                if (root.creatingWebLinks)
                                    root.cancelLinksRequested()
                                else
                                    root.autoCreateLinksRequested()
                            }
                        }

                        Action {
                            text: root.removingWebLinks
                                //% "Cancel removing links"
                                ? qsTrId("CancelRemovingWebLinks")
                                //% "Remove web links"
                                : qsTrId("RemoveWebLinks")
                            enabled: root.removingWebLinks
                                || (!root.hasPendingPanelEdits
                                    && !root.creatingWebLinks)
                            onTriggered: {
                                if (root.removingWebLinks)
                                    root.cancelLinksRequested()
                                else
                                    root.removeLinksRequested()
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

            RibbonButton {
                id: shapeButton
                objectName: "editShapesButton"

                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Shapes"
                text: qsTrId("Shapes")
                iconSource: "../assets/icons/shapes.svg"
                menuIndicatorVisible: true
                //% "Choose shape"
                menuToolTipText: qsTrId("ChooseShape")
                enabled: root.documentLoaded && root.shapesAvailable
                    && root.allowedShapes.length > 0
                    && !root.saving
                primary: root.currentTool === "addShape"
                    && root.currentShape !== "table"
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

            RibbonButton {
                id: tableButton
                objectName: "editTableButton"

                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Table"
                text: qsTrId("TableShape")
                iconSource: "../assets/icons/table.svg"
                menuIndicatorVisible: true
                menuToolTipText: text
                visible: root.tablesAvailable
                enabled: root.documentLoaded && root.tablesAvailable && !root.saving
                primary: root.currentTool === "addShape" && root.currentShape === "table"
                onClicked: tablePopupLoader.active = true
                onMenuRequested: tablePopupLoader.active = true
                onEnabledChanged: {
                    if (!enabled)
                        tablePopupLoader.active = false
                }
                onVisibleChanged: {
                    if (!visible)
                        tablePopupLoader.active = false
                }

                Connections {
                    target: root
                    function onDocumentContextChanged() {
                        tablePopupLoader.active = false
                    }
                }

                Loader {
                    id: tablePopupLoader

                    active: false
                    onLoaded: {
                        if (status === Loader.Ready && item)
                            (item as Popup).open()
                    }

                    sourceComponent: Popup {
                        id: tablePopup
                        objectName: "tablePopup"

                        parent: tableButton
                        x: tableButton.width - width
                        y: tableButton.height
                        width: 280
                        padding: 12
                        popupType: Popup.Item
                        focus: true
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                        onClosed: tablePopupLoader.active = false

                        contentItem: ColumnLayout {
                            spacing: 10

                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: 12
                                rowSpacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    //% "Rows"
                                    text: qsTrId("TableRows")
                                    color: root.theme.ribbonTextColor
                                }
                                SpinBox {
                                    id: tableRowsInput
                                    objectName: "tableRowsSpinBox"
                                    from: 1
                                    to: 20
                                    value: root.tableRows
                                    editable: true
                                    live: true
                                    //% "Rows"
                                    Accessible.name: qsTrId("TableRows")
                                }
                                Label {
                                    Layout.fillWidth: true
                                    //% "Columns"
                                    text: qsTrId("TableColumns")
                                    color: root.theme.ribbonTextColor
                                }
                                SpinBox {
                                    id: tableColumnsInput
                                    objectName: "tableColumnsSpinBox"
                                    from: 1
                                    to: 20
                                    value: root.tableColumns
                                    editable: true
                                    live: true
                                    //% "Columns"
                                    Accessible.name: qsTrId("TableColumns")
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                //% "Choose rows and columns, then drag on the page to draw the table."
                                text: qsTrId("DrawTableHelp")
                                color: root.theme.ribbonTextColor
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }

                            Button {
                                objectName: "drawTableButton"
                                Layout.fillWidth: true
                                //% "Draw table"
                                text: qsTrId("DrawTable")
                                onClicked: {
                                    root.tableRequested(tableRowsInput.value, tableColumnsInput.value)
                                    tablePopup.close()
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
        }

        RibbonGroup {
            theme: root.theme
            //% "Page"
            title: qsTrId("Page")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Add Page"
                text: qsTrId("AddPage")
                //% "Add Blank Page"
                toolTipText: qsTrId("AddBlankPage")
                iconSource: "../assets/icons/add-page.svg"
                preserveIconColors: true
                objectName: "editAddPageButton"
                enabled: root.documentLoaded && root.addPageAvailable
                    && !root.saving
                onClicked: root.addBlankPageRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Delete Page"
                text: qsTrId("DeletePage")
                //% "Delete Current Page"
                toolTipText: qsTrId("DeleteCurrentPage")
                iconSource: "../assets/icons/delete-page.svg"
                preserveIconColors: true
                objectName: "editDeletePageButton"
                enabled: root.documentLoaded && root.deletePageAvailable
                    && root.pageCount > 1
                    && !root.saving
                onClicked: root.deletePageRequested(root.currentPage)
            }

            RibbonButton {
                objectName: "mergePdfButton"
                theme: root.theme
                documentLoaded: true
                visible: root.mergeVisible
                //% "Merge PDF"
                text: qsTrId("MergePdf")
                iconSource: "../assets/icons/merge-pdf.svg"
                enabled: root.mergeAvailable && !root.saving
                onClicked: root.mergePdfRequested()
            }

            RibbonButton {
                objectName: "splitPdfButton"
                theme: root.theme
                documentLoaded: true
                visible: root.splitVisible
                //% "Split PDF"
                text: qsTrId("SplitPdf")
                iconSource: "../assets/icons/split-pdf.svg"
                enabled: root.splitAvailable && !root.saving
                onClicked: root.splitPdfRequested()
            }
        }

        RibbonGroup {
            theme: root.theme
            //% "History"
            title: qsTrId("History")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Undo"
                text: qsTrId("Undo")
                iconSource: "../assets/icons/undo-2.svg"
                enabled: root.documentLoaded
                    && root.canUndo
                    && !root.saving
                    && !root.textEditActive
                objectName: "editUndoButton"
                onClicked: root.undoRequested()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.documentLoaded
                //% "Redo"
                text: qsTrId("Redo")
                iconSource: "../assets/icons/redo-2.svg"
                enabled: root.documentLoaded
                    && root.canRedo
                    && !root.hasPendingPanelEdits
                    && !root.saving
                    && !root.textEditActive
                objectName: "editRedoButton"
                onClicked: root.redoRequested()
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }

}
