import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Flickable {
    id: root

    required property AppTheme theme
    required property PDFDocument document
    required property PDFEditController editController

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
            title: qsTr("Content")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Select")
                iconSource: "../assets/icons/mouse-pointer-2.svg"
                primary: root.editController.tool === PDFEditController.SelectTool
                onClicked: root.editController.tool = PDFEditController.SelectTool
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Edit Text")
                iconSource: "../assets/icons/type.svg"
                primary: root.editController.tool === PDFEditController.EditTextTool
                onClicked: root.editController.tool = PDFEditController.EditTextTool
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Add Text")
                iconSource: "../assets/icons/type.svg"
                onClicked: root.toolRequested(text)
            }
        }

        RibbonGroup {
            theme: root.theme
            title: qsTr("Objects")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Image")
                toolTipText: qsTr("Add Image")
                iconSource: "../assets/icons/image-plus.svg"
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Link")
                toolTipText: qsTr("Add Link")
                iconSource: "../assets/icons/link-2.svg"
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Shapes")
                iconSource: "../assets/icons/shapes.svg"
                onClicked: root.toolRequested(text)
            }
        }

        RibbonGroup {
            theme: root.theme
            title: qsTr("Markup")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Highlight")
                iconSource: "../assets/icons/highlighter.svg"
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Underline")
                iconSource: "../assets/icons/underline.svg"
                onClicked: root.toolRequested(text)
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Strikeout")
                iconSource: "../assets/icons/strikethrough.svg"
                onClicked: root.toolRequested(text)
            }
        }

        RibbonGroup {
            theme: root.theme
            title: qsTr("History")

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Undo")
                iconSource: "../assets/icons/undo-2.svg"
                enabled: root.document.loaded
                    && root.editController.canUndo
                    && !root.editController.textEditActive
                onClicked: root.editController.undo()
            }

            RibbonButton {
                theme: root.theme
                documentLoaded: root.document.loaded
                text: qsTr("Redo")
                iconSource: "../assets/icons/redo-2.svg"
                enabled: root.document.loaded
                    && root.editController.canRedo
                    && !root.editController.textEditActive
                onClicked: root.editController.redo()
            }
        }
    }

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
