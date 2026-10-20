pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

EditRibbonTools {
    id: root

    required property PDFDocument document
    required property PDFEditController editController

    documentLoaded: document.loaded
    documentContext: document
    pageCount: document.pageCount
    currentPage: document.currentPage
    currentTool: toolName(editController.tool)
    currentShape: shapeTypeName(editController.shapeType)
    saving: editController.saving
    canUndo: editController.canUndo
    canRedo: editController.canRedo
    textEditActive: editController.textEditActive
    creatingWebLinks: editController.creatingWebLinks
    removingWebLinks: editController.removingWebLinks
    loadingEditableLinks: editController.loadingEditableLinks
    mergeVisible: Qt.platform.os !== "ios"
    splitAvailable: Qt.platform.os !== "ios"
    splitVisible: Qt.platform.os !== "ios"

    onSelectToolRequested: toolName => {
        switch (toolName) {
        case "select": editController.tool = PDFEditController.SelectTool; break
        case "editText": editController.tool = PDFEditController.EditTextTool; break
        case "addText": editController.tool = PDFEditController.AddTextTool; break
        case "addImage": editController.tool = PDFEditController.AddImageTool; break
        }
    }
    onShapeRequested: shapeName => {
        switch (shapeName) {
        case "rectangle": editController.shapeType = PDFEditController.RectangleShape; break
        case "ellipse": editController.shapeType = PDFEditController.EllipseShape; break
        case "line": editController.shapeType = PDFEditController.LineShape; break
        case "arrow": editController.shapeType = PDFEditController.ArrowShape; break
        case "triangle": editController.shapeType = PDFEditController.TriangleShape; break
        case "diamond": editController.shapeType = PDFEditController.DiamondShape; break
        default: return
        }
        editController.tool = PDFEditController.AddShapeTool
    }
    onAddBlankPageRequested: {
        const page = editController.addBlankPage()
        if (page >= 0)
            root.pageRequested(page)
    }
    onBeginLinkEditingRequested: editController.beginLinkEditing()
    onAutoCreateLinksRequested: editController.autoCreateWebLinks()
    onRemoveLinksRequested: editController.removeWebLinks()
    onCancelLinksRequested: editController.cancelWebLinkCreation()

    function toolName(tool: int): string {
        switch (tool) {
        case PDFEditController.SelectTool: return "select"
        case PDFEditController.EditTextTool: return "editText"
        case PDFEditController.AddTextTool: return "addText"
        case PDFEditController.AddImageTool: return "addImage"
        case PDFEditController.AddShapeTool: return "addShape"
        case PDFEditController.LinkTool: return "link"
        }
        return ""
    }

    function shapeTypeName(shape: int): string {
        switch (shape) {
        case PDFEditController.RectangleShape: return "rectangle"
        case PDFEditController.EllipseShape: return "ellipse"
        case PDFEditController.LineShape: return "line"
        case PDFEditController.ArrowShape: return "arrow"
        case PDFEditController.TriangleShape: return "triangle"
        case PDFEditController.DiamondShape: return "diamond"
        }
        return ""
    }
}
