pragma ComponentBehavior: Bound

import QtQuick
import TrePDF

QtObject {
    id: root
    objectName: "headerImagePlacement"

    required property PDFDocument document
    required property PDFEditController editController
    readonly property bool footerMode: editController ? editController.footerMode : false
    property bool armed: false
    property bool choosing: false
    property bool preparing: false
    property bool submitting: false
    property int page: -1
    property real pageWidthPoints: 0
    property real headerHeightPoints: 0
    property real xPoints: 0
    property real yPoints: 0
    property real widthPoints: 0
    property real heightPoints: 0
    property int quarterTurns: 0
    property bool aspectRatioLocked: true
    property string previewSource: ""
    property string errorText: ""
    readonly property bool active: page >= 0 && previewSource.length > 0 && !choosing
    readonly property bool canSubmit: active && document.loaded
        && !editController.saving && !submitting
    readonly property bool canRotate: canSubmit
        && heightPoints <= pageWidthPoints && widthPoints <= headerHeightPoints

    signal fileRequested()
    signal focusRequested()

    function start() {
        if (!document.loaded || editController.saving || submitting || choosing)
            return
        if (active) {
            focusRequested()
            return
        }
        cancel()
        armed = true
    }

    function place(pageIndex, x, y) {
        if (!armed || !document.loaded || editController.saving
                || pageIndex < 0 || pageIndex >= document.pageCount
                || !Number.isFinite(x) || !Number.isFinite(y))
            return
        const size = document.pageSize(pageIndex)
        const headerHeight = Math.min(100, size.height)
        if (size.width <= 0 || headerHeight <= 0 || x < 0 || x >= size.width
                || y < 0 || y >= headerHeight)
            return
        xPoints = x
        yPoints = y
        pageWidthPoints = size.width
        headerHeightPoints = headerHeight
        page = pageIndex
        armed = false
        choosing = true
        fileRequested()
    }

    function acceptImage(source) {
        if (!choosing || !document.loaded || editController.saving)
            return false
        errorText = ""
        preparing = true
        const image = editController.prepareHeaderImage(source)
        preparing = false
        if (!image.previewSource || !Number.isFinite(image.pixelWidth)
                || !Number.isFinite(image.pixelHeight)
                || image.pixelWidth <= 0 || image.pixelHeight <= 0) {
            cancel()
            return false
        }
        const scale = Math.min(Math.min(pageWidthPoints * 0.25, 150) / image.pixelWidth,
            Math.min(80, headerHeightPoints) / image.pixelHeight)
        const width = image.pixelWidth * scale
        const height = image.pixelHeight * scale
        xPoints = Math.max(0, Math.min(pageWidthPoints - width, xPoints - width / 2))
        yPoints = Math.max(0, Math.min(headerHeightPoints - height, yPoints - height / 2))
        widthPoints = width
        heightPoints = height
        quarterTurns = 0
        aspectRatioLocked = true
        choosing = false
        previewSource = image.previewSource
        focusRequested()
        return true
    }

    function updateFrame(x, y, width, height) {
        if (!canSubmit || !Number.isFinite(x) || !Number.isFinite(y)
                || !Number.isFinite(width) || !Number.isFinite(height)
                || width <= 0 || height <= 0)
            return false
        widthPoints = Math.min(pageWidthPoints, width)
        heightPoints = Math.min(headerHeightPoints, height)
        xPoints = Math.max(0, Math.min(pageWidthPoints - widthPoints, x))
        yPoints = Math.max(0, Math.min(headerHeightPoints - heightPoints, y))
        errorText = ""
        return true
    }

    function resizeFrame(frame, horizontal, vertical, dx, dy) {
        if (!canSubmit || !Number.isFinite(dx) || !Number.isFinite(dy))
            return
        let width = horizontal === 0 ? frame.width : Math.max(1, frame.width + horizontal * dx)
        let height = vertical === 0 ? frame.height : Math.max(1, frame.height + vertical * dy)
        const centerX = frame.x + frame.width / 2
        const centerY = frame.y + frame.height / 2
        const maximumWidth = horizontal < 0 ? frame.x + frame.width
            : horizontal > 0 ? pageWidthPoints - frame.x
            : 2 * Math.min(centerX, pageWidthPoints - centerX)
        const maximumHeight = vertical < 0 ? frame.y + frame.height
            : vertical > 0 ? headerHeightPoints - frame.y
            : 2 * Math.min(centerY, headerHeightPoints - centerY)
        if (aspectRatioLocked) {
            const ratio = frame.width / frame.height
            if (horizontal === 0 || (vertical !== 0
                    && Math.abs(height / frame.height - 1) > Math.abs(width / frame.width - 1)))
                width = height * ratio
            else
                height = width / ratio
            const fit = Math.min(1, maximumWidth / width, maximumHeight / height)
            width *= fit
            height *= fit
        } else {
            width = Math.min(maximumWidth, width)
            height = Math.min(maximumHeight, height)
        }
        updateFrame(horizontal < 0 ? frame.x + frame.width - width
                : horizontal > 0 ? frame.x : centerX - width / 2,
            vertical < 0 ? frame.y + frame.height - height
                : vertical > 0 ? frame.y : centerY - height / 2, width, height)
    }

    function rotate(direction) {
        if (!canRotate || (direction !== -1 && direction !== 1))
            return false
        const centerX = xPoints + widthPoints / 2
        const centerY = yPoints + heightPoints / 2
        const width = heightPoints
        const height = widthPoints
        quarterTurns = (quarterTurns + direction + 4) % 4
        return updateFrame(centerX - width / 2, centerY - height / 2, width, height)
    }

    function cancel() {
        armed = false
        choosing = false
        page = -1
        previewSource = ""
        errorText = ""
        xPoints = 0
        yPoints = 0
        widthPoints = 0
        heightPoints = 0
        pageWidthPoints = 0
        headerHeightPoints = 0
        quarterTurns = 0
        if (editController)
            editController.clearHeaderImage()
    }

    function submit(pages) {
        if (!canSubmit)
            return
        errorText = ""
        submitting = true
        const succeeded = editController.addHeaderImage(pages, xPoints, yPoints,
            widthPoints, heightPoints, quarterTurns)
        submitting = false
        if (succeeded) {
            cancel()
        } else if (errorText.length === 0) {
            errorText = footerMode
                //% "Could not add the footer image."
                ? qsTrId("AddFooterImageFailed")
                //% "Could not add the header image."
                : qsTrId("AddHeaderImageFailed")
        }
    }

    onFooterModeChanged: cancel()
    onDocumentChanged: cancel()
    onEditControllerChanged: cancel()
    Component.onDestruction: {
        if (editController)
            editController.clearHeaderImage()
    }

    readonly property Connections documentConnections: Connections {
        target: root.document
        function onSourceChanged() { root.cancel() }
        function onPageStructureChanged() { root.cancel() }
        function onLoadedChanged() { root.cancel() }
    }

    readonly property Connections controllerConnections: Connections {
        target: root.editController
        function onHistoryStateChanged() {
            if (!root.submitting)
                root.cancel()
        }
        function onSavingChanged() {
            if (root.editController.saving)
                root.cancel()
        }
        function onStatusMessage(message) {
            if (root.submitting || root.preparing)
                root.errorText = message
        }
    }
}
