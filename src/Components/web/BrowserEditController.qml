pragma ComponentBehavior: Bound

import QtQuick

QtObject {
    id: root

    required property var document
    property bool active: false
    property string tool: "select"
    property string shape: "rectangle"
    property int selectedId: 0
    property int editingTextId: 0
    property bool imageAspectLocked: true
    property var pendingObject: null
    property var propertyDraft: null
    property string message: ""
    readonly property var objects: document ? document.objects : []
    readonly property var selectedObject: findObject(selectedId)
    readonly property var workingObject: propertyDraft || pendingObject || selectedObject
    readonly property bool selectionEditable: selectedObject !== null && selectedObject.editable !== false
    readonly property bool hasPendingInput: pendingObject !== null || propertyDraft !== null
    readonly property bool textEditing: editingTextId > 0
        || (pendingObject !== null && pendingObject.type === "text")
    readonly property string textFontFamily: editingFont.name
    readonly property bool editingAllowed: active && document && document.ready
        && document.canEdit && !document.busy

    signal textFocusRequested(int page)

    property FontLoader editingFont: FontLoader {
        source: Qt.resolvedUrl("../../web/fonts/LiberationSans-Regular.ttf")
    }

    function findObject(id: int): var {
        for (const object of objects) {
            if (object.id === id)
                return object
        }
        return null
    }

    function pendingNotice(): bool {
        if (!hasPendingInput)
            return false
        message = qsTr("Hãy áp dụng hoặc hủy phần đang nhập trước.")
        return true
    }

    function selectTool(name: string): void {
        if (pendingNotice())
            return
        if (name !== "select" && name !== "editText" && name !== "addText" && name !== "addShape")
            return
        tool = name
        editingTextId = 0
        message = ""
    }

    function selectShape(name: string): void {
        if (pendingNotice())
            return
        if (["rectangle", "ellipse", "line", "arrow", "triangle", "diamond"].indexOf(name) < 0)
            return
        shape = name
        editingTextId = 0
        tool = "addShape"
        message = ""
    }

    function selectObject(id: int): bool {
        if (!editingAllowed || (id !== selectedId && pendingNotice()))
            return false
        if (selectedId !== id) {
            editingTextId = 0
            imageAspectLocked = true
        }
        selectedId = id
        message = selectedObject && selectedObject.editable === false
            ? selectedObject.readOnlyReason || qsTr("Đối tượng này chưa hỗ trợ chỉnh sửa trên web.") : ""
        return true
    }

    function beginTextEdit(id: int): void {
        if (!selectObject(id) || !selectionEditable || selectedObject.type !== "text")
            return
        editingTextId = id
        textFocusRequested(selectedObject.page)
    }

    function finishTextEdit(): void {
        Qt.inputMethod.commit()
        if (hasPendingInput)
            applyProperties()
        else
            editingTextId = 0
    }

    function prepareObject(page: int, x: real, y: real, width: real, height: real): void {
        if (!editingAllowed || pendingNotice() || page < 0 || page >= document.pageCount)
            return
        const w = Math.max(0.01, Math.min(1, width))
        const h = Math.max(0.01, Math.min(1, height))
        const object = {
            id: 0, type: tool === "addText" ? "text" : shape, page: page,
            x: Math.max(0, Math.min(1 - w, x)),
            y: Math.max(0, Math.min(1 - h, y)),
            width: w, height: h, text: "", fontSize: 16,
            color: "#202124", strokeWidth: 1
        }
        selectedId = 0
        editingTextId = 0
        propertyDraft = null
        message = ""
        if (tool === "addText") {
            pendingObject = object
            textFocusRequested(page)
        } else if (tool === "addShape") {
            document.applyEdit({action: "add", object: object})
        }
    }

    function setProperty(name: string, value: var): void {
        if (!editingAllowed || !workingObject || workingObject.editable === false)
            return
        const draft = Object.assign({}, workingObject)
        draft[name] = value
        if (draft.type === "image" && imageAspectLocked && Number.isFinite(Number(value))) {
            if (name === "width")
                draft.height = Number(value) * workingObject.height / workingObject.width
            else if (name === "height")
                draft.width = Number(value) * workingObject.width / workingObject.height
        }
        const original = pendingObject || selectedObject
        propertyDraft = JSON.stringify(draft) === JSON.stringify(original) ? null : draft
        message = ""
    }

    function applyProperties(): bool {
        if (!document || !document.ready || !document.canEdit || document.busy || !workingObject
                || workingObject.editable === false)
            return false
        Qt.inputMethod.commit()
        const object = Object.assign({}, workingObject)
        const numericFields = ["x", "y", "width", "height"]
        if (object.type === "text")
            numericFields.push("fontSize")
        else if (object.type !== "image")
            numericFields.push("strokeWidth")
        for (const key of numericFields)
            object[key] = Number(object[key])
        if (![object.x, object.y, object.width, object.height].every(Number.isFinite)
                || object.x < 0 || object.y < 0 || object.width < 0.001 || object.height < 0.001
                || object.x + object.width > 1.000001 || object.y + object.height > 1.000001) {
            message = qsTr("Đối tượng cần nằm trong trang, với chiều rộng và chiều cao lớn hơn 0.")
            return false
        }
        if (object.type !== "image" && !/^#[0-9a-fA-F]{6}$/.test(object.color)) {
            message = qsTr("Nhập mã màu, ví dụ #202124.")
            return false
        }
        const textChanged = object.type === "text" && (!object.source || !selectedObject
            || object.text !== selectedObject.text || object.fontSize !== selectedObject.fontSize
            || object.color !== selectedObject.color)
        if (textChanged && (!object.text.trim().length || object.text.length > 1024
                || !Number.isFinite(object.fontSize) || object.fontSize < 6 || object.fontSize > 144)) {
            message = qsTr("Nhập nội dung (tối đa 1.024 ký tự) và cỡ chữ từ 6 đến 144.")
            return false
        }
        if (object.type !== "text" && object.type !== "image" && (!Number.isFinite(object.strokeWidth)
                || object.strokeWidth < 0.5 || object.strokeWidth > 12)) {
            message = qsTr("Nhập độ dày nét từ 0,5 đến 12.")
            return false
        }
        message = ""
        return document.applyEdit({action: pendingObject ? "add" : "update", object: object})
    }

    function moveObject(id: int, x: real, y: real): void {
        if (!editingAllowed || pendingNotice())
            return
        const original = findObject(id)
        if (!original || original.editable === false)
            return
        const object = Object.assign({}, original)
        object.x = Math.max(0, Math.min(1 - object.width, x))
        object.y = Math.max(0, Math.min(1 - object.height, y))
        if (Math.abs(object.x - original.x) < 0.00001 && Math.abs(object.y - original.y) < 0.00001)
            return
        document.applyEdit({action: "update", object: object})
    }

    function resizeObject(id: int, frame: var): void {
        if (!editingAllowed || pendingNotice())
            return
        const original = findObject(id)
        if (!original || original.type !== "image" || original.editable === false)
            return
        const object = Object.assign({}, original, frame)
        if (["x", "y", "width", "height"].every(key => Math.abs(object[key] - original[key]) < 0.00001))
            return
        document.applyEdit({action: "update", object: object})
    }

    function rotateImage(turns: int): void {
        if (!editingAllowed || !selectionEditable || selectedObject.type !== "image" || pendingNotice())
            return
        const object = Object.assign({}, selectedObject)
        const pageSize = document.pages[object.page]
        const width = object.height * pageSize.height / pageSize.width
        const height = object.width * pageSize.width / pageSize.height
        const scale = Math.min(1, 1 / width, 1 / height)
        object.width = width * scale
        object.height = height * scale
        object.x = Math.max(0, Math.min(1 - object.width,
            selectedObject.x + (selectedObject.width - object.width) / 2))
        object.y = Math.max(0, Math.min(1 - object.height,
            selectedObject.y + (selectedObject.height - object.height) / 2))
        object.quarterTurns = ((Number(object.quarterTurns || 0) + turns) % 4 + 4) % 4
        document.applyEdit({action: "update", object: object})
    }

    function replaceImage(): void {
        if (!editingAllowed || !selectionEditable || selectedObject.type !== "image" || pendingNotice())
            return
        document.replaceImage(selectedId)
    }

    function deleteSelection(): void {
        if (!editingAllowed)
            return
        if (pendingObject) {
            cancelPending()
            return
        }
        if (selectionEditable && !pendingNotice())
            document.applyEdit({action: "remove", object: {id: selectedId}})
    }

    function cancelPending(): void {
        editingTextId = 0
        pendingObject = null
        propertyDraft = null
        message = ""
    }

    function reset(): void {
        cancelPending()
        selectedId = 0
        tool = "select"
    }

    property Connections documentConnections: Connections {
        target: root.document
        function onEdited(selectedId: int): void {
            root.cancelPending()
            root.selectedId = selectedId > 0 ? selectedId : 0
            root.tool = "select"
        }
        function onPagesChanged(): void { root.reset() }
    }
}
