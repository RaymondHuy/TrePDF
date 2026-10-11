pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Shapes
import TrePDF

FocusScope {
    id: root

    required property AppTheme theme
    required property PDFEditController controller
    required property int page
    required property real pagePointScale
    required property bool pooled
    required property bool interactive

    readonly property bool editing: interactive && !pooled
        && !controller.saving && controller.tool === PDFEditController.LinkTool
        && !controller.loadingEditableLinks
    readonly property bool browsing: interactive && !pooled
        && !controller.saving && controller.tool === PDFEditController.BrowseTool
    readonly property var links: {
        controller.webLinksRevision
        controller.loadingEditableLinks
        return pooled ? [] : controller.editableLinksForPage(page)
            .filter(link => editing || !link.automatic)
    }
    readonly property var selectedLink: links.find(link => link.id === selectedId) ?? null
    readonly property rect selectedRect: gestureId === selectedId && gestureMode.length > 0
        ? gestureRect : selectedLink ? selectedLink.rect : Qt.rect(0, 0, 0, 0)

    property string selectedId: ""
    property bool applyingChange: false
    property string gestureMode: ""
    property string gestureId: ""
    property rect gestureStartRect: Qt.rect(0, 0, 0, 0)
    property rect gestureRect: Qt.rect(0, 0, 0, 0)
    property point gestureStart: Qt.point(0, 0)
    property bool gestureMoved: false
    property int resizeHorizontal: 0
    property int resizeVertical: 0
    property var gestureProperties: ({})
    property bool creating: false
    property point creationStart: Qt.point(0, 0)
    property rect creationRect: Qt.rect(0, 0, 0, 0)
    property string dialogLinkId: ""
    property rect dialogRect: Qt.rect(0, 0, 0, 0)
    property var dialogProperties: ({})

    function prepareForSessionChange() {
        creating = false
        gestureMode = ""
        gestureId = ""
        selectedId = ""
        propertiesLoader.active = false
        contextMenuLoader.active = false
    }

    function normalizedPoint(point) {
        return Qt.point(Math.max(0, Math.min(1, point.x / Math.max(1, width))),
            Math.max(0, Math.min(1, point.y / Math.max(1, height))))
    }

    function openProperties(id, rect) {
        if (!editing)
            return
        const properties = id.length > 0 ? controller.linkProperties(page, id) : {
            url: "", targetPage: -1, visible: false, color: "#000000",
            width: 1, lineStyle: 0, highlight: 1
        }
        if (id.length > 0 && (!properties || properties.editable === false))
            return
        gestureMode = ""
        dialogLinkId = id
        dialogRect = rect
        dialogProperties = properties
        contextMenuLoader.active = false
        propertiesLoader.active = true
    }

    function submitProperties(properties) {
        if (!editing || propertiesLoader.status !== Loader.Ready)
            return
        applyingChange = true
        let id = dialogLinkId
        const previousIds = links.map(link => link.id)
        const accepted = id.length > 0
            ? controller.updateLink(page, id, dialogRect, properties)
            : (id = controller.addLink(page, dialogRect, properties)).length > 0
        applyingChange = false
        if (!accepted) {
            if (propertiesLoader.status === Loader.Ready)
                (propertiesLoader.item as LinkPropertiesDialog).showSaveError()
            return
        }
        propertiesLoader.active = false
        selectedId = selectionAfterChange(id, previousIds)
        forceActiveFocus()
    }

    function selectionAfterChange(id, previousIds) {
        const materializedId = links.find(link => !previousIds.includes(link.id)
            && !link.automatic)?.id ?? ""
        if (id.startsWith("auto:") && materializedId.length > 0)
            return materializedId
        if (links.some(link => link.id === id))
            return id
        return materializedId
    }

    function deleteSelection() {
        if (!editing || !selectedLink || !selectedLink.editable)
            return false
        const removed = controller.deleteLink(page, selectedId)
        if (removed)
            selectedId = ""
        return removed
    }

    function beginGesture(id, rect, point, horizontal, vertical) {
        if (!editing)
            return
        gestureId = id
        gestureStartRect = rect
        gestureRect = rect
        gestureStart = point
        gestureMoved = false
        resizeHorizontal = horizontal
        resizeVertical = vertical
        gestureProperties = controller.linkProperties(page, id)
        gestureMode = horizontal || vertical ? "resize" : "move"
    }

    function resizedRect(rect, horizontal, vertical, dx, dy) {
        const minimumWidth = Math.min(1, 6 / Math.max(1, width))
        const minimumHeight = Math.min(1, 6 / Math.max(1, height))
        let left = rect.x
        let top = rect.y
        let right = rect.x + rect.width
        let bottom = rect.y + rect.height
        if (horizontal < 0)
            left = Math.max(0, Math.min(right - minimumWidth, left + dx))
        else if (horizontal > 0)
            right = Math.min(1, Math.max(left + minimumWidth, right + dx))
        if (vertical < 0)
            top = Math.max(0, Math.min(bottom - minimumHeight, top + dy))
        else if (vertical > 0)
            bottom = Math.min(1, Math.max(top + minimumHeight, bottom + dy))
        return Qt.rect(left, top, right - left, bottom - top)
    }

    function updateGesture(point) {
        if (!editing || gestureMode.length === 0)
            return
        const dx = (point.x - gestureStart.x) / Math.max(1, width)
        const dy = (point.y - gestureStart.y) / Math.max(1, height)
        if (!gestureMoved && Math.hypot(point.x - gestureStart.x,
                point.y - gestureStart.y) < 3)
            return
        gestureMoved = true
        const rect = gestureStartRect
        gestureRect = gestureMode === "resize"
            ? resizedRect(rect, resizeHorizontal, resizeVertical, dx, dy)
            : Qt.rect(Math.max(0, Math.min(1 - rect.width, rect.x + dx)),
                Math.max(0, Math.min(1 - rect.height, rect.y + dy)),
                rect.width, rect.height)
    }

    function finishGesture() {
        if (gestureMode.length === 0)
            return
        const id = gestureId
        const rect = gestureRect
        const changed = gestureMoved
        gestureMode = ""
        gestureId = ""
        if (changed && editing) {
            const previousIds = links.map(link => link.id)
            applyingChange = true
            controller.updateLink(page, id, rect, gestureProperties)
            applyingChange = false
            selectedId = selectionAfterChange(id, previousIds)
        }
    }

    function moveByKey(dx, dy) {
        if (!editing || !selectedLink || !selectedLink.editable)
            return false
        beginGesture(selectedId, selectedRect, Qt.point(0, 0), 0, 0)
        gestureMoved = true
        updateGesture(Qt.point(dx, dy))
        finishGesture()
        return true
    }

    onPooledChanged: prepareForSessionChange()
    onPageChanged: prepareForSessionChange()
    onControllerChanged: prepareForSessionChange()
    onEditingChanged: {
        if (!editing)
            prepareForSessionChange()
    }
    onActiveFocusChanged: {
        if (!activeFocus && !propertiesLoader.active && !contextMenuLoader.active)
            selectedId = ""
    }

    Connections {
        target: root.controller
        function onWebLinksChanged() {
            if (!root.applyingChange)
                root.prepareForSessionChange()
        }
        function onDocumentChanged() { root.prepareForSessionChange() }
    }

    Connections {
        target: root.controller.document
        function onPageStructureChanged() { root.prepareForSessionChange() }
    }

    Keys.onPressed: event => {
        if (!root.editing)
            return
        if (event.key === Qt.Key_Escape) {
            root.prepareForSessionChange()
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            event.accepted = root.deleteSelection()
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                && root.selectedLink && root.selectedLink.editable) {
            root.openProperties(root.selectedId, root.selectedRect)
            event.accepted = true
        } else {
            const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
            if (event.key === Qt.Key_Left)
                event.accepted = root.moveByKey(-step, 0)
            else if (event.key === Qt.Key_Right)
                event.accepted = root.moveByKey(step, 0)
            else if (event.key === Qt.Key_Up)
                event.accepted = root.moveByKey(0, -step)
            else if (event.key === Qt.Key_Down)
                event.accepted = root.moveByKey(0, step)
        }
    }

    MouseArea {
        id: creationArea

        anchors.fill: parent
        enabled: root.editing && !propertiesLoader.active
        visible: enabled
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        cursorShape: Qt.CrossCursor
        onPressed: mouse => {
            root.forceActiveFocus()
            root.selectedId = ""
            root.creationStart = root.normalizedPoint(Qt.point(mouse.x, mouse.y))
            root.creationRect = Qt.rect(root.creationStart.x, root.creationStart.y, 0, 0)
            root.creating = true
        }
        onPositionChanged: mouse => {
            if (!pressed || !root.creating)
                return
            const point = root.normalizedPoint(Qt.point(mouse.x, mouse.y))
            root.creationRect = Qt.rect(Math.min(point.x, root.creationStart.x),
                Math.min(point.y, root.creationStart.y),
                Math.abs(point.x - root.creationStart.x),
                Math.abs(point.y - root.creationStart.y))
        }
        onReleased: {
            const rect = root.creationRect
            const wasCreating = root.creating
            root.creating = false
            if (wasCreating && rect.width * root.width >= 6
                    && rect.height * root.height >= 6)
                root.openProperties("", rect)
        }
        onCanceled: root.creating = false
        onWheel: wheel => wheel.accepted = false
    }

    Repeater {
        model: root.links

        delegate: Item {
            id: linkRegion

            required property var modelData
            readonly property rect linkRect: root.gestureId === modelData.id
                    && root.gestureMode.length > 0
                ? root.gestureRect : modelData.rect
            readonly property bool editable: Boolean(modelData.editable)
            readonly property bool receivesBrowse: root.browsing && modelData.draft
            readonly property string destination: modelData.targetPage >= 0
                //% "Go to page %1"
                ? qsTrId("ManualLinkGoToPageNumber").arg(modelData.targetPage + 1)
                : modelData.url

            x: linkRect.x * root.width
            y: linkRect.y * root.height
            width: linkRect.width * root.width
            height: linkRect.height * root.height
            visible: root.editing || modelData.draft

            Shape {
                anchors.fill: parent
                visible: Boolean(linkRegion.modelData.draft && linkRegion.modelData.visible)
                preferredRendererType: Shape.CurveRenderer
                Accessible.ignored: true

                ShapePath {
                    id: borderPath

                    readonly property bool underline: linkRegion.modelData.lineStyle === 2
                    fillColor: "transparent"
                    strokeColor: linkRegion.modelData.color
                    strokeWidth: linkRegion.modelData.width * root.pagePointScale
                    strokeStyle: linkRegion.modelData.lineStyle === 1
                        ? ShapePath.DashLine : ShapePath.SolidLine
                    dashPattern: [3, 3]
                    joinStyle: ShapePath.MiterJoin
                    startX: 0
                    startY: underline ? linkRegion.height : 0
                    PathLine { x: linkRegion.width; y: borderPath.underline ? linkRegion.height : 0 }
                    PathLine { x: linkRegion.width; y: linkRegion.height }
                    PathLine { x: 0; y: linkRegion.height }
                    PathLine { x: 0; y: borderPath.underline ? linkRegion.height : 0 }
                }
            }

            Rectangle {
                anchors.fill: parent
                color: linkPointer.pressed && linkRegion.receivesBrowse
                        && linkRegion.modelData.highlight === 1
                    ? "#30000000" : "transparent"
                border.width: root.editing || linkPointer.activeFocus
                    || (linkPointer.pressed && linkRegion.modelData.highlight >= 2) ? 1 : 0
                border.color: root.editing
                    ? linkRegion.editable ? root.theme.foxitDarkOrange : "#888888"
                    : "#333333"
                Accessible.ignored: true
            }

            MouseArea {
                id: linkPointer

                anchors.fill: parent
                enabled: root.editing || linkRegion.receivesBrowse
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                hoverEnabled: true
                preventStealing: true
                activeFocusOnTab: true
                cursorShape: root.editing
                    ? linkRegion.editable ? Qt.SizeAllCursor : Qt.ArrowCursor
                    : Qt.PointingHandCursor
                Accessible.role: Accessible.Link
                Accessible.name: linkRegion.destination
                Accessible.onPressAction: {
                    if (root.editing) {
                        root.selectedId = linkRegion.modelData.id
                        if (linkRegion.editable)
                            root.openProperties(root.selectedId, linkRegion.linkRect)
                    } else if (linkRegion.receivesBrowse) {
                        root.controller.activateManualLink(root.page,
                            linkRegion.modelData.id, linkRegion.modelData.revision)
                    }
                }
                onActiveFocusChanged: {
                    if (activeFocus && root.editing)
                        root.selectedId = linkRegion.modelData.id
                }
                onPressed: mouse => {
                    forceActiveFocus()
                    if (!root.editing)
                        return
                    root.selectedId = linkRegion.modelData.id
                    if (mouse.button === Qt.RightButton) {
                        contextMenuLoader.active = true
                    } else if (linkRegion.editable) {
                        root.beginGesture(root.selectedId, linkRegion.linkRect,
                            mapToItem(root, mouse.x, mouse.y), 0, 0)
                    }
                }
                onPositionChanged: mouse => {
                    if (pressed && root.gestureId === linkRegion.modelData.id)
                        root.updateGesture(mapToItem(root, mouse.x, mouse.y))
                }
                onReleased: mouse => {
                    root.finishGesture()
                    if (linkRegion.receivesBrowse && mouse.button === Qt.LeftButton
                            && containsMouse)
                        root.controller.activateManualLink(root.page,
                            linkRegion.modelData.id, linkRegion.modelData.revision)
                }
                onDoubleClicked: mouse => {
                    if (mouse.button === Qt.LeftButton && root.editing && linkRegion.editable)
                        root.openProperties(linkRegion.modelData.id, linkRegion.linkRect)
                }
                onCanceled: root.gestureMode = ""
                onWheel: wheel => wheel.accepted = false
                Keys.onPressed: event => {
                    if (linkRegion.receivesBrowse && (event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter || event.key === Qt.Key_Space)) {
                        root.controller.activateManualLink(root.page,
                            linkRegion.modelData.id, linkRegion.modelData.revision)
                        event.accepted = true
                    }
                }
                ToolTip.visible: containsMouse && !pressed
                ToolTip.text: linkRegion.destination
                ToolTip.delay: 500
            }
        }
    }

    Rectangle {
        x: root.creationRect.x * root.width
        y: root.creationRect.y * root.height
        width: root.creationRect.width * root.width
        height: root.creationRect.height * root.height
        visible: root.creating
        color: "#18d97a32"
        border.width: 1
        border.color: root.theme.foxitDarkOrange
        Accessible.ignored: true
    }

    Item {
        id: selectionFrame

        x: root.selectedRect.x * root.width
        y: root.selectedRect.y * root.height
        width: root.selectedRect.width * root.width
        height: root.selectedRect.height * root.height
        visible: root.editing && root.selectedLink !== null
            && !propertiesLoader.active && !root.creating

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.width: 2
            border.color: root.theme.foxitDarkOrange
            Accessible.ignored: true
        }

        Repeater {
            model: [
                {h: -1, v: -1, cursor: Qt.SizeFDiagCursor},
                {h: 0, v: -1, cursor: Qt.SizeVerCursor},
                {h: 1, v: -1, cursor: Qt.SizeBDiagCursor},
                {h: -1, v: 0, cursor: Qt.SizeHorCursor},
                {h: 1, v: 0, cursor: Qt.SizeHorCursor},
                {h: -1, v: 1, cursor: Qt.SizeBDiagCursor},
                {h: 0, v: 1, cursor: Qt.SizeVerCursor},
                {h: 1, v: 1, cursor: Qt.SizeFDiagCursor}
            ]

            delegate: MouseArea {
                id: resizeHandle

                required property var modelData
                width: 18
                height: 18
                x: (modelData.h + 1) / 2 * selectionFrame.width - width / 2
                y: (modelData.v + 1) / 2 * selectionFrame.height - height / 2
                visible: root.selectedLink && root.selectedLink.editable
                enabled: visible
                cursorShape: modelData.cursor
                acceptedButtons: Qt.LeftButton
                preventStealing: true
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                //% "Resize link"
                Accessible.name: qsTrId("ResizeManualLink")
                onPressed: mouse => {
                    forceActiveFocus()
                    root.beginGesture(root.selectedId, root.selectedRect,
                        mapToItem(root, mouse.x, mouse.y), modelData.h, modelData.v)
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        root.updateGesture(mapToItem(root, mouse.x, mouse.y))
                }
                onReleased: root.finishGesture()
                onCanceled: root.gestureMode = ""
                onWheel: wheel => wheel.accepted = false
                Keys.onPressed: event => {
                    const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
                    const dx = event.key === Qt.Key_Left ? -step
                        : event.key === Qt.Key_Right ? step : 0
                    const dy = event.key === Qt.Key_Up ? -step
                        : event.key === Qt.Key_Down ? step : 0
                    if ((dx && modelData.h) || (dy && modelData.v)) {
                        root.beginGesture(root.selectedId, root.selectedRect,
                            Qt.point(0, 0), modelData.h, modelData.v)
                        root.gestureMoved = true
                        root.updateGesture(Qt.point(dx, dy))
                        root.finishGesture()
                        event.accepted = true
                    }
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: 7
                    height: 7
                    color: resizeHandle.activeFocus ? root.theme.foxitOrange : "white"
                    border.color: root.theme.foxitDarkOrange
                    border.width: 1
                    Accessible.ignored: true
                }
            }
        }

        Row {
            x: Math.max(-selectionFrame.x,
                Math.min(0, root.width - selectionFrame.x - width))
            y: selectionFrame.y >= height + 12 ? -height - 12
                : selectionFrame.height + 12
            spacing: 4
            visible: root.gestureMode.length === 0 && root.selectedLink
                && root.selectedLink.editable

            Button {
                //% "Link properties"
                text: qsTrId("ManualLinkProperties")
                onClicked: root.openProperties(root.selectedId, root.selectedRect)
            }
            Button {
                //% "Delete"
                text: qsTrId("Delete")
                onClicked: root.deleteSelection()
            }
        }
    }

    Loader {
        id: propertiesLoader

        active: false
        sourceComponent: LinkPropertiesDialog {
            theme: root.theme
            pageCount: root.controller.document ? root.controller.document.pageCount : 1
            initialProperties: root.dialogProperties
            creating: root.dialogLinkId.length === 0
            onSubmitRequested: properties => root.submitProperties(properties)
            onClosed: {
                propertiesLoader.active = false
                root.forceActiveFocus()
            }
        }
        onLoaded: {
            if (status === Loader.Ready)
                (item as LinkPropertiesDialog).open()
        }
    }

    Loader {
        id: contextMenuLoader

        active: false
        sourceComponent: Menu {
            parent: root
            popupType: Popup.Item
            onClosed: {
                contextMenuLoader.active = false
                root.forceActiveFocus()
            }
            Action {
                //% "Link properties"
                text: qsTrId("ManualLinkProperties")
                enabled: root.selectedLink && root.selectedLink.editable
                onTriggered: root.openProperties(root.selectedId, root.selectedRect)
            }
            Action {
                //% "Delete"
                text: qsTrId("Delete")
                enabled: root.selectedLink && root.selectedLink.editable
                onTriggered: root.deleteSelection()
            }
        }
        onLoaded: {
            if (status === Loader.Ready)
                (item as Menu).popup()
        }
    }
}
