pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

FocusScope {
    id: root

    required property BrowserEditController controller
    required property int page
    readonly property var pageObjects: controller.objects.filter(object => object.page === page)
    property bool dragging: false
    property bool resizing: false
    property var resizeOriginal: null
    property var resizePreview: null
    property point resizeEdges: Qt.point(0, 0)
    property point resizeStart: Qt.point(0, 0)
    property int draggedId: 0
    property point startPoint: Qt.point(0, 0)
    property point endPoint: Qt.point(0, 0)
    property point originalPosition: Qt.point(0, 0)
    readonly property var pending: controller.workingObject
        && controller.workingObject.page === page ? controller.workingObject : null
    readonly property var imageFrame: resizePreview || (pending && pending.type === "image"
        && pending.editable !== false ? pending : null)
    readonly property bool editingText: controller.textEditing && pending !== null
        && pending.type === "text"
    readonly property real pageWidth: controller.document && controller.document.pages[page]
        ? controller.document.pages[page].width : 1

    visible: controller.active
    enabled: controller.editingAllowed
    onEnabledChanged: {
        if (!enabled) {
            dragging = false
            cancelResize()
        }
    }
    onPageChanged: { dragging = false; cancelResize() }
    onVisibleChanged: { if (!visible) cancelResize() }

    function cancelResize(): void {
        resizing = false
        resizeOriginal = null
        resizePreview = null
    }

    function beginResize(xEdge: real, yEdge: real, point: point): bool {
        if (!imageFrame || controller.hasPendingInput || !controller.editingAllowed)
            return false
        root.forceActiveFocus()
        resizeOriginal = Object.assign({}, imageFrame)
        resizePreview = Object.assign({}, imageFrame)
        resizeEdges = Qt.point(xEdge, yEdge)
        resizeStart = Qt.point(point.x / width, point.y / height)
        resizing = true
        return true
    }

    function updateResize(point: point): void {
        if (!resizing || !resizeOriginal)
            return
        const original = resizeOriginal
        const xEdge = resizeEdges.x
        const yEdge = resizeEdges.y
        const deltaX = point.x / width - resizeStart.x
        const deltaY = point.y / height - resizeStart.y
        const anchorX = original.x + original.width * (1 - xEdge) / 2
        const anchorY = original.y + original.height * (1 - yEdge) / 2
        const maxWidth = xEdge < 0 ? anchorX : xEdge > 0 ? 1 - anchorX
            : 2 * Math.min(anchorX, 1 - anchorX)
        const maxHeight = yEdge < 0 ? anchorY : yEdge > 0 ? 1 - anchorY
            : 2 * Math.min(anchorY, 1 - anchorY)
        let newWidth = original.width + deltaX * xEdge
        let newHeight = original.height + deltaY * yEdge
        if (controller.imageAspectLocked) {
            const widthScale = newWidth / original.width
            const heightScale = newHeight / original.height
            let scale = xEdge === 0 ? heightScale : yEdge === 0 ? widthScale
                : Math.abs(widthScale - 1) >= Math.abs(heightScale - 1) ? widthScale : heightScale
            scale = Math.max(Math.max(0.001 / original.width, 0.001 / original.height),
                Math.min(maxWidth / original.width, maxHeight / original.height, scale))
            newWidth = original.width * scale
            newHeight = original.height * scale
        } else {
            newWidth = Math.max(0.001, Math.min(maxWidth, newWidth))
            newHeight = Math.max(0.001, Math.min(maxHeight, newHeight))
        }
        resizePreview = Object.assign({}, original, {
            x: anchorX - newWidth * (1 - xEdge) / 2,
            y: anchorY - newHeight * (1 - yEdge) / 2,
            width: newWidth, height: newHeight
        })
    }

    function finishResize(): void {
        if (!resizing || !resizePreview)
            return
        const frame = resizePreview
        cancelResize()
        controller.resizeObject(frame.id, {
            x: frame.x, y: frame.y, width: frame.width, height: frame.height
        })
    }

    function hitObject(x: real, y: real): int {
        for (let index = pageObjects.length - 1; index >= 0; --index) {
            const object = pageObjects[index]
            if (controller.tool === "editText" && object.type !== "text")
                continue
            if (x >= object.x && x <= object.x + object.width
                    && y >= object.y && y <= object.y + object.height)
                return object.id
        }
        return 0
    }

    function editorValue(name: string, fallback: real): real {
        const value = pending ? Number(pending[name]) : NaN
        return Number.isFinite(value) ? value : fallback
    }

    Keys.onDeletePressed: event => {
        if (!root.editingText)
            controller.deleteSelection()
        event.accepted = true
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Backspace) {
            if (!root.editingText)
                controller.deleteSelection()
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            controller.cancelPending()
            controller.selectedId = 0
            dragging = false
            cancelResize()
            event.accepted = true
        }
    }

    Repeater {
        model: root.pageObjects
        delegate: Rectangle {
            required property var modelData
            readonly property bool selected: modelData.id === root.controller.selectedId
            readonly property bool moving: root.dragging && root.draggedId === modelData.id
            readonly property var frame: selected && root.resizePreview ? root.resizePreview
                : selected && root.pending ? root.pending : modelData
            x: (moving ? Math.max(0, Math.min(1 - frame.width,
                root.originalPosition.x + root.endPoint.x - root.startPoint.x)) : frame.x) * root.width
            y: (moving ? Math.max(0, Math.min(1 - frame.height,
                root.originalPosition.y + root.endPoint.y - root.startPoint.y)) : frame.y) * root.height
            width: frame.width * root.width
            height: frame.height * root.height
            color: selected ? "#14d97a32" : "transparent"
            border.width: selected ? 2 : 1
            border.color: selected ? (modelData.editable === false ? "#9b7770" : "#d97a32") : "#40777777"
            Accessible.ignored: true
        }
    }

    Rectangle {
        visible: root.controller.pendingObject !== null && root.pending !== null
        x: root.pending ? root.pending.x * root.width : 0
        y: root.pending ? root.pending.y * root.height : 0
        width: root.pending ? root.pending.width * root.width : 0
        height: root.pending ? root.pending.height * root.height : 0
        color: "#14d97a32"
        border.color: "#d97a32"
        border.width: 2
        Accessible.ignored: true
    }

    Rectangle {
        visible: root.dragging && (root.controller.tool === "addText" || root.controller.tool === "addShape")
        x: Math.min(root.startPoint.x, root.endPoint.x) * root.width
        y: Math.min(root.startPoint.y, root.endPoint.y) * root.height
        width: Math.abs(root.endPoint.x - root.startPoint.x) * root.width
        height: Math.abs(root.endPoint.y - root.startPoint.y) * root.height
        color: "#20d97a32"
        border.color: "#d97a32"
        border.width: 1
        Accessible.ignored: true
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        preventStealing: root.controller.tool !== "select" || root.draggedId > 0
        cursorShape: root.controller.tool === "select" ? Qt.ArrowCursor
            : root.controller.tool === "editText" ? Qt.IBeamCursor : Qt.CrossCursor
        onPressed: mouse => {
            root.forceActiveFocus()
            root.startPoint = Qt.point(mouse.x / root.width, mouse.y / root.height)
            root.endPoint = root.startPoint
            root.draggedId = 0
            if (root.controller.tool === "editText") {
                root.controller.beginTextEdit(root.hitObject(root.startPoint.x, root.startPoint.y))
                return
            }
            if (root.controller.tool === "select") {
                const id = root.hitObject(root.startPoint.x, root.startPoint.y)
                if (!root.controller.selectObject(id)) {
                    mouse.accepted = false
                    return
                }
                root.draggedId = root.controller.selectionEditable ? id : 0
                const object = root.controller.selectedObject
                if (object)
                    root.originalPosition = Qt.point(object.x, object.y)
            } else if (root.controller.hasPendingInput) {
                root.controller.pendingNotice()
                mouse.accepted = false
                return
            }
            root.dragging = true
        }
        onPositionChanged: mouse => {
            if (root.dragging)
                root.endPoint = Qt.point(Math.max(0, Math.min(1, mouse.x / root.width)),
                                        Math.max(0, Math.min(1, mouse.y / root.height)))
        }
        onReleased: mouse => {
            if (!root.dragging)
                return
            root.dragging = false
            if (root.controller.tool === "select") {
                if (root.draggedId > 0)
                    root.controller.moveObject(root.draggedId,
                        root.originalPosition.x + root.endPoint.x - root.startPoint.x,
                        root.originalPosition.y + root.endPoint.y - root.startPoint.y)
                return
            }
            const width = Math.abs(root.endPoint.x - root.startPoint.x)
            const height = Math.abs(root.endPoint.y - root.startPoint.y)
            const clicked = width * root.width < 5 && height * root.height < 5
            root.controller.prepareObject(root.page,
                Math.min(root.startPoint.x, root.endPoint.x),
                Math.min(root.startPoint.y, root.endPoint.y),
                clicked ? 0.25 : Math.max(0.01, width),
                clicked ? (root.controller.tool === "addText" ? 0.08 : 0.12) : Math.max(0.01, height))
        }
        onCanceled: root.dragging = false
        onWheel: wheel => wheel.accepted = false
        onDoubleClicked: mouse => {
            if (root.controller.tool === "select") {
                const id = root.hitObject(mouse.x / root.width, mouse.y / root.height)
                const object = root.controller.findObject(id)
                if (object && object.type === "text" && object.editable !== false) {
                    root.dragging = false
                    root.controller.beginTextEdit(id)
                }
            }
        }
    }

    Repeater {
        model: [Qt.point(-1, -1), Qt.point(0, -1), Qt.point(1, -1), Qt.point(1, 0),
                Qt.point(1, 1), Qt.point(0, 1), Qt.point(-1, 1), Qt.point(-1, 0)]
        delegate: Rectangle {
            id: resizeHandle
            required property int index
            required property point modelData
            objectName: "webImageResize" + index
            visible: root.imageFrame !== null && !root.dragging
            enabled: !root.controller.hasPendingInput
            x: root.imageFrame ? (root.imageFrame.x + root.imageFrame.width * (modelData.x + 1) / 2)
                * root.width - width / 2 : 0
            y: root.imageFrame ? (root.imageFrame.y + root.imageFrame.height * (modelData.y + 1) / 2)
                * root.height - height / 2 : 0
            width: 10
            height: 10
            radius: 2
            color: "white"
            border.color: "#d97a32"
            border.width: 1
            Accessible.ignored: true
            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                preventStealing: true
                cursorShape: resizeHandle.modelData.x === 0 ? Qt.SizeVerCursor
                    : resizeHandle.modelData.y === 0 ? Qt.SizeHorCursor
                    : resizeHandle.modelData.x === resizeHandle.modelData.y ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                onPressed: mouse => {
                    mouse.accepted = root.beginResize(resizeHandle.modelData.x, resizeHandle.modelData.y,
                        mapToItem(root, mouse.x, mouse.y))
                }
                onPositionChanged: mouse => root.updateResize(mapToItem(root, mouse.x, mouse.y))
                onReleased: root.finishResize()
                onCanceled: root.cancelResize()
            }
        }
    }

    Loader {
        id: textEditor
        active: root.visible && root.editingText
        x: Math.max(0, root.editorValue("x", 0)) * root.width
        y: Math.max(0, root.editorValue("y", 0)) * root.height
        width: Math.max(1, root.editorValue("width", 0.25) * root.width)
        height: Math.max(1, root.editorValue("height", 0.08) * root.height)
        sourceComponent: TextArea {
            id: input
            objectName: "webInlineTextInput"
            property bool initialized: false
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            persistentSelection: true
            clip: true
            padding: 0
            font.family: root.controller.textFontFamily
            font.pixelSize: Math.max(1, root.editorValue("fontSize", 16) * root.width / root.pageWidth)
            font.kerning: false
            color: root.pending && /^#[0-9a-fA-F]{6}$/.test(root.pending.color)
                ? root.pending.color : "#202124"
            selectionColor: "#d97a32"
            selectedTextColor: "white"
            placeholderText: qsTr("Nhập nội dung")
            Accessible.name: qsTr("Nội dung chữ trên trang PDF")
            background: Rectangle {
                color: "#fffdf9"
                border.color: "#d97a32"
                border.width: 1
            }
            Component.onCompleted: {
                text = root.pending ? root.pending.text : ""
                cursorPosition = text.length
                initialized = true
            }
            onTextChanged: {
                if (initialized)
                    root.controller.setProperty("text", text)
            }
            Keys.priority: Keys.BeforeItem
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Escape) {
                    root.controller.cancelPending()
                    root.forceActiveFocus()
                    event.accepted = true
                } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                           && (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier))) {
                    root.controller.finishTextEdit()
                    event.accepted = true
                }
            }
            // Editing keys must stop here instead of bubbling to object deletion.
            Keys.onShortcutOverride: event => {
                if (event.matches(StandardKey.Undo) || event.matches(StandardKey.Redo)
                        || event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace)
                    event.accepted = true
            }
        }
    }

    Connections {
        target: root.controller
        function onTextFocusRequested(page: int): void {
            if (page !== root.page)
                return
            Qt.callLater(function() {
                if (root.visible && textEditor.status === Loader.Ready) {
                    const input = textEditor.item as TextArea
                    if (input)
                        input.forceActiveFocus(Qt.MouseFocusReason)
                }
            })
        }
    }
}
