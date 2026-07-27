import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController

    signal pageRequested(int page)
    signal deletePageRequested(int page)
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
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Shapes"
                text: qsTrId("Shapes")
                iconSource: "../assets/icons/shapes.svg"
                onClicked: root.toolRequested(text)
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
                theme: root.theme
                documentLoaded: root.document.loaded
                //% "Highlight"
                text: qsTrId("Highlight")
                iconSource: "../assets/icons/highlight.svg"
                preserveIconColors: true
                enabled: root.document.loaded
                    && !root.editController.saving
                primary: root.editController.tool
                    === PDFEditController.HighlightTool
                onClicked: root.editController.tool
                    = PDFEditController.HighlightTool
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
}
