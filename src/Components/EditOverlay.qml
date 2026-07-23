pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import TrePDF

Item {
    id: root

    required property int page
    required property bool pooled
    required property bool draftTextureCurrent
    required property bool draftTextureReady
    required property AppTheme theme
    required property PDFEditController controller

    property var drafts: []
    property var hoverCandidate: ({})
    property bool editing: false
    property string editingObjectId: ""
    property bool loadingEditorText: false
    property bool syncingEditorPreview: false
    property bool syncingEditorGeometry: false
    property bool applyingEditorDocumentFormat: false
    property bool suppressNextEditorReload: false
    property bool editorLayoutDirty: false
    property bool resizingFrame: false
    property string resizingObjectId: ""
    property string selectionActionsObjectId: ""
    property real selectCursorX: 0
    property real selectCursorY: 0
    property bool selectCursorVisible: false
    readonly property real fallbackBleed: 2
    readonly property real editBoxRadius: 4
    readonly property color editBoxColor: "#f5a55a"
    readonly property real editBoxLeftBleed: 3
    readonly property real editBoxRightBleed: 3
    readonly property real editBoxVerticalBleed: 3
    readonly property real boundingBoxPadding: 3
    readonly property real minimumTextPixelSize: 6
    readonly property real resizeHandleSize: 8
    readonly property real resizeHandleHitSize: 24
    readonly property real minimumResizePixelSize: 24
    readonly property bool interactionActive: !pooled
        && controller.tool !== PDFEditController.BrowseTool
    readonly property bool editorUsesCleanTexture: draftTextureReady
        && hasActiveDraft(editingObjectId)
    readonly property bool selectCursorReady: selectCursorImage.status === Image.Ready
    readonly property bool selectCursorActive: selectCursorReady
        && selectCursorVisible
        && !editing
    readonly property bool addTextCursorReady: addTextCursorImage.status === Image.Ready
    readonly property bool addTextCursorActive: addTextCursorReady
        && interactionActive
        && !editing
        && (controller.tool === PDFEditController.AddTextTool
            || controller.tool === PDFEditController.AddImageTool)
        && hitArea.containsMouse

    signal selectedDraftContentBoundsSyncRequested()

    visible: !pooled && (interactionActive || drafts.length > 0)
    enabled: interactionActive

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete)
            event.accepted = root.deleteSelectedImage()
    }

    function reloadData() {
        if (pooled) {
            drafts = []
            hoverCandidate = ({})
            hideSelectCursor()
            return
        }

        if (interactionActive)
            controller.candidatesForPage(page)

        const previousDrafts = drafts
        const currentDrafts = controller.draftsForPage(page)
        const displayedDrafts = []
        for (let index = 0; index < currentDrafts.length; ++index)
            displayedDrafts.push(currentDrafts[index])

        if (!draftTextureCurrent) {
            for (let index = 0; index < previousDrafts.length; ++index) {
                const previousDraft = previousDrafts[index]
                if (containsObject(currentDrafts, previousDraft.objectId))
                    continue

                if (previousDraft.added === true)
                    continue
                else if (previousDraft.kind === "image")
                    continue
                else if (previousDraft.restoration === true)
                    displayedDrafts.push(previousDraft)
                else if (previousDraft.originalText !== undefined)
                    displayedDrafts.push(restorationOverlay(previousDraft))
            }
        }
        drafts = displayedDrafts
        if (!interactionActive) {
            hoverCandidate = ({})
            hideSelectCursor()
        }
    }

    function containsObject(items, objectId) {
        for (let index = 0; index < items.length; ++index) {
            if (items[index].objectId === objectId)
                return true
        }
        return false
    }

    function hasActiveDraft(objectId) {
        if (objectId.length === 0)
            return false

        for (let index = 0; index < drafts.length; ++index) {
            if (drafts[index].objectId === objectId
                    && drafts[index].restoration !== true) {
                return true
            }
        }
        return false
    }

    function restorationOverlay(draft) {
        const characterSpans = draft.originalCharacterSpans !== undefined
            ? draft.originalCharacterSpans
            : draft.characterSpans
        const paragraphFormats = draft.originalParagraphFormats !== undefined
            ? draft.originalParagraphFormats
            : draft.paragraphFormats
        const primaryFormat = characterSpans.length > 0
            ? characterSpans[0]
            : draft
        return {
            objectId: draft.objectId,
            sourceRect: draft.sourceRect,
            frameRect: draft.originalFrameRect !== undefined
                ? draft.originalFrameRect
                : draft.frameRect,
            contentBounds: draft.originalContentBounds !== undefined
                ? draft.originalContentBounds
                : draft.contentBounds,
            sourceRects: draft.sourceRects,
            grouped: draft.grouped,
            text: draft.originalText,
            originalText: draft.originalText,
            characterSpans: characterSpans,
            paragraphFormats: paragraphFormats,
            fontFamily: primaryFormat.fontFamily,
            fontSizeRatio: primaryFormat.fontSizeRatio,
            displayFontSizeRatio: primaryFormat.displayFontSizeRatio,
            fontStretch: primaryFormat.fontStretch,
            fontBold: primaryFormat.fontBold,
            fontItalic: primaryFormat.fontItalic,
            fontUnderline: primaryFormat.fontUnderline,
            hasBaseline: draft.hasBaseline,
            originX: draft.originX,
            baselineY: draft.baselineY,
            color: primaryFormat.color,
            restoration: true
        }
    }

    function escapeHtml(value) {
        return String(value)
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
    }

    function escapeHtmlAttribute(value) {
        return escapeHtml(value).replace(/"/g, "&quot;")
    }

    function cssColor(value) {
        if (value === undefined || value === null)
            return "rgba(0,0,0,1)"

        return "rgba("
            + Math.round(value.r * 255) + ","
            + Math.round(value.g * 255) + ","
            + Math.round(value.b * 255) + ","
            + Number(value.a).toFixed(3) + ")"
    }

    function characterStyle(format, baseRatio, basePixelSize) {
        const rawFamily = format.fontFamily === undefined
            ? ""
            : String(format.fontFamily)
        const family = rawFamily
            .replace(/\\/g, "\\\\")
            .replace(/'/g, "\\'")
        const displayRatio = Number(format.displayFontSizeRatio) > 0
            ? Number(format.displayFontSizeRatio)
            : Number(format.fontSizeRatio)
        const relativeSize = displayRatio > 0 && baseRatio > 0
            ? displayRatio / baseRatio
            : 1
        const fontBold = format.fontBold === true
        const fontItalic = format.fontItalic === true
        const effectiveBasePixelSize = Number(basePixelSize) > 0
            ? Number(basePixelSize)
            : Math.max(1, baseRatio * root.height)
        const displayPixelSize = Math.max(
            1,
            Math.round(relativeSize * effectiveBasePixelSize))
        const displayStretch = controller.displayFontStretch(
            rawFamily,
            Number(format.fontStretch || 100),
            fontBold,
            fontItalic,
            displayPixelSize)
        let style = "font-size:" + relativeSize.toFixed(6) + "em;"
            + "font-stretch:" + displayStretch + "%;"
            + "font-weight:" + (fontBold ? "700" : "400") + ";"
            + "font-style:" + (fontItalic ? "italic" : "normal") + ";"
            + "text-decoration:"
            + (format.fontUnderline === true ? "underline" : "none") + ";"
            + "color:" + cssColor(format.color) + ";"
        if (family.length > 0)
            style += "font-family:'" + family + "';"
        return style
    }

    function richRangeHtml(text, start, end, spans, fallbackFormat,
                           baseRatio, basePixelSize) {
        let html = ""
        let cursor = start
        for (let index = 0; index < spans.length; ++index) {
            const span = spans[index]
            const spanStart = Math.max(start, Number(span.start))
            const spanEnd = Math.min(
                end,
                span.end !== undefined
                    ? Number(span.end)
                    : Number(span.start) + Number(span.length))
            if (spanEnd <= spanStart)
                continue

            if (spanStart > cursor) {
                html += "<span style=\""
                    + escapeHtmlAttribute(characterStyle(
                        fallbackFormat, baseRatio, basePixelSize))
                    + "\">" + escapeHtml(text.slice(cursor, spanStart)) + "</span>"
            }
            html += "<span style=\""
                + escapeHtmlAttribute(characterStyle(
                    span, baseRatio, basePixelSize))
                + "\">" + escapeHtml(text.slice(spanStart, spanEnd)) + "</span>"
            cursor = Math.max(cursor, spanEnd)
        }
        if (cursor < end) {
            html += "<span style=\""
                + escapeHtmlAttribute(characterStyle(
                    fallbackFormat, baseRatio, basePixelSize))
                + "\">" + escapeHtml(text.slice(cursor, end)) + "</span>"
        }
        return html
    }

    function richTextHtml(text, spans, paragraphFormats, fallbackFormat,
                          basePixelSize) {
        const sourceText = text === undefined ? "" : String(text)
        const sourceSpans = spans === undefined || spans === null ? [] : spans
        let paragraphs = paragraphFormats === undefined || paragraphFormats === null
            ? []
            : paragraphFormats
        if (paragraphs.length === 0) {
            paragraphs = []
            let start = 0
            while (start <= sourceText.length) {
                const newline = sourceText.indexOf("\n", start)
                const end = newline < 0 ? sourceText.length : newline
                paragraphs.push({
                    start: start,
                    length: end - start,
                    alignment: "left"
                })
                if (newline < 0)
                    break
                start = newline + 1
            }
        }

        const baseRatio = Number(fallbackFormat.displayFontSizeRatio) > 0
            ? Number(fallbackFormat.displayFontSizeRatio)
            : Math.max(Number(fallbackFormat.fontSizeRatio), 0.000001)
        let html = "<html><body style=\"margin:0;\">"
        for (let index = 0; index < paragraphs.length; ++index) {
            const paragraph = paragraphs[index]
            const start = Math.max(0, Math.min(
                sourceText.length,
                Number(paragraph.start)))
            const end = Math.max(start, Math.min(
                sourceText.length,
                paragraph.end !== undefined
                    ? Number(paragraph.end)
                    : start + Number(paragraph.length)))
            const alignment = paragraph.alignment === "center"
                    || paragraph.alignment === "right"
                    || paragraph.alignment === "justify"
                ? paragraph.alignment
                : "left"
            const content = richRangeHtml(
                sourceText,
                start,
                end,
                sourceSpans,
                fallbackFormat,
                baseRatio,
                basePixelSize)
            const paragraphStyle = "margin:0;white-space:pre-wrap;text-align:"
                + alignment + ";"
            html += "<p style=\"" + escapeHtmlAttribute(paragraphStyle) + "\">"
                + content + "</p>"
        }
        return html + "</body></html>"
    }

    function selectedFallbackFormat() {
        return {
            fontFamily: controller.selectedFontFamily,
            fontSizeRatio: controller.selectedFontSizeRatio,
            displayFontSizeRatio: controller.selectedDisplayFontSizeRatio,
            fontStretch: controller.selectedFontStretch,
            fontBold: controller.selectedFontBold,
            fontItalic: controller.selectedFontItalic,
            fontUnderline: controller.selectedFontUnderline,
            color: controller.selectedColor
        }
    }

    function selectedBaseFormat() {
        const spans = controller.selectedCharacterSpans
        return spans.length > 0 ? spans[0] : selectedFallbackFormat()
    }

    function editorPlainText() {
        return editor.getText(0, editor.length)
            .replace(/\u2028/g, "\n")
            .replace(/\u2029/g, "\n")
    }

    function syncEditorContentBounds() {
        if (!editing
                || loadingEditorText
                || syncingEditorGeometry
                || !editorLayoutDirty
                || root.height <= 0) {
            return
        }

        const frameRect = controller.selectedFrameRect
        if (frameRect.width <= 0 || frameRect.height <= 0)
            return

        const frameTop = frameRect.y * root.height
        const laidOutBottom = Math.min(
            root.height,
            editorFrame.y + editorFrame.editorBottom)
        const contentHeight = Math.min(
            1 - frameRect.y,
            Math.max(
                frameRect.height,
                (laidOutBottom - frameTop) / root.height))
        const currentBounds = controller.selectedContentBounds
        const epsilon = 0.25 / root.height
        if (Math.abs(currentBounds.x - frameRect.x) <= epsilon
                && Math.abs(currentBounds.y - frameRect.y) <= epsilon
                && Math.abs(currentBounds.width - frameRect.width) <= epsilon
                && Math.abs(currentBounds.height - contentHeight) <= epsilon) {
            return
        }

        syncingEditorGeometry = true
        controller.updateSelectedContentBounds(Qt.rect(
            frameRect.x,
            frameRect.y,
            frameRect.width,
            contentHeight))
        syncingEditorGeometry = false
    }

    function syncSelectedDraftContentBounds() {
        if (editing || !resizingFrame || root.height <= 0)
            return

        selectedDraftContentBoundsSyncRequested()
    }

    function syncEditorSelection() {
        if (!editing || loadingEditorText)
            return

        const start = Math.min(editor.selectionStart, editor.selectionEnd)
        const end = Math.max(editor.selectionStart, editor.selectionEnd)
        controller.setSelectedTextRange(start, end - start)
    }

    function syncEditorDocumentFontStyles() {
        if (!editing
                || loadingEditorText
                || syncingEditorPreview
                || applyingEditorDocumentFormat
                || editorPlainText() !== controller.selectedText) {
            return
        }

        suppressNextEditorReload = true
        const changed = controller.syncEditorFontStyles(editor.textDocument)
        if (!changed)
            suppressNextEditorReload = false
    }

    function loadEditorRichText(cursorPosition, selectionStart, selectionEnd) {
        loadingEditorText = true
        editor.text = richTextHtml(
            controller.selectedText,
            controller.selectedCharacterSpans,
            controller.selectedParagraphFormats,
            selectedBaseFormat(),
            editorFrame.baseFontPixelSize)
        const maximumPosition = editor.length
        const start = Math.max(0, Math.min(maximumPosition, selectionStart))
        const end = Math.max(0, Math.min(maximumPosition, selectionEnd))
        if (start !== end) {
            editor.select(start, end)
        } else {
            editor.deselect()
            editor.cursorPosition = cursorPosition < 0
                ? maximumPosition
                : Math.max(0, Math.min(maximumPosition, cursorPosition))
        }
        loadingEditorText = false
        syncEditorSelection()
    }

    function hasCandidate(candidate) {
        return candidate !== null
            && candidate.objectId !== undefined
            && candidate.objectId.length > 0
    }

    function isTextBoxCandidate(candidate) {
        return hasCandidate(candidate)
            && (candidate.kind === "text"
                || candidate.kind === "addedText"
                || candidate.kind === "freeText")
    }

    function isImageKind(kind) {
        return kind === "image" || kind === "addedImage"
    }

    function isImageCandidate(candidate) {
        return hasCandidate(candidate) && isImageKind(candidate.kind)
    }

    function updateSelectCursor(x, y, candidate) {
        if (controller.tool !== PDFEditController.SelectTool
                || !isTextBoxCandidate(candidate)) {
            hideSelectCursor()
            return
        }

        selectCursorX = x
        selectCursorY = y
        selectCursorVisible = true
    }

    function hideSelectCursor() {
        selectCursorVisible = false
    }

    function resizedFrameRect(startFrameRect, horizontal, vertical, deltaX, deltaY) {
        const pageWidth = Math.max(1, root.width)
        const pageHeight = Math.max(1, root.height)
        const minimumWidth = Math.min(
            Number(startFrameRect.width),
            root.minimumResizePixelSize / pageWidth)
        const minimumHeight = Math.min(
            Number(startFrameRect.height),
            root.minimumResizePixelSize / pageHeight)
        let left = Number(startFrameRect.x)
        let top = Number(startFrameRect.y)
        let right = left + Number(startFrameRect.width)
        let bottom = top + Number(startFrameRect.height)

        if (horizontal < 0) {
            left = Math.max(
                0,
                Math.min(right - minimumWidth, left + deltaX / pageWidth))
        } else if (horizontal > 0) {
            right = Math.min(
                1,
                Math.max(left + minimumWidth, right + deltaX / pageWidth))
        }

        if (vertical < 0) {
            top = Math.max(
                0,
                Math.min(bottom - minimumHeight, top + deltaY / pageHeight))
        } else if (vertical > 0) {
            bottom = Math.min(
                1,
                Math.max(top + minimumHeight, bottom + deltaY / pageHeight))
        }

        return Qt.rect(left, top, right - left, bottom - top)
    }

    function movedFrameRect(startFrameRect, deltaX, deltaY) {
        const pageWidth = Math.max(1, root.width)
        const pageHeight = Math.max(1, root.height)
        const frameWidth = Number(startFrameRect.width)
        const frameHeight = Number(startFrameRect.height)
        const left = Math.max(
            0,
            Math.min(
                1 - frameWidth,
                Number(startFrameRect.x) + deltaX / pageWidth))
        const top = Math.max(
            0,
            Math.min(
                1 - frameHeight,
                Number(startFrameRect.y) + deltaY / pageHeight))
        return Qt.rect(left, top, frameWidth, frameHeight)
    }

    function cancelFrameResize() {
        if (!resizingFrame)
            return

        controller.cancelSelectedFrameResize()
        resizingFrame = false
        resizingObjectId = ""
        reloadData()
    }

    function updateHoverAtPosition(x, y) {
        const candidate = candidateAtPosition(x, y)
        hoverCandidate = candidate
        updateSelectCursor(x, y, candidate)
    }

    function candidateAtPosition(x, y) {
        if (!interactionActive
                || controller.tool === PDFEditController.AddTextTool
                || controller.tool === PDFEditController.AddImageTool
                || width <= 0
                || height <= 0) {
            return ({})
        }

        return controller.candidateAt(page, x / width, y / height)
    }

    function beginAddingText(x, y) {
        if (controller.tool !== PDFEditController.AddTextTool
                || width <= 0
                || height <= 0) {
            return
        }

        if (editing)
            commitEditing()

        const candidate = controller.addText(page, x / width, y / height)
        if (!hasCandidate(candidate))
            return

        editingObjectId = candidate.objectId
        editorLayoutDirty = false
        editing = true
        loadEditorRichText(-1, 0, 0)
        Qt.callLater(function() {
            editor.forceActiveFocus()
        })
    }

    function beginAddingImage(x, y) {
        if (controller.tool !== PDFEditController.AddImageTool
                || width <= 0
                || height <= 0) {
            return
        }

        if (editing)
            commitEditing()
        controller.requestAddImage(page, x / width, y / height)
    }

    function selectCandidate(candidate) {
        if (editing && editingObjectId !== candidate.objectId)
            commitEditing()

        return controller.selectCandidate(page, candidate.objectId)
    }

    function showSelectionActions(x, y, candidate) {
        if (controller.tool !== PDFEditController.SelectTool
                || !candidate.editable
                || !selectCandidate(candidate)) {
            selectionActions.close()
            return
        }

        selectionActionsObjectId = candidate.objectId
        hideSelectCursor()
        selectionActions.openAt(x, y)
    }

    function beginEditing(candidate) {
        if (!isTextBoxCandidate(candidate)
                || !candidate.editable
                || (controller.tool !== PDFEditController.EditTextTool
                    && controller.tool !== PDFEditController.SelectTool)) {
            return
        }

        if (!controller.selectCandidate(page, candidate.objectId))
            return

        hideSelectCursor()
        controller.beginSelectedTextEdit()
        editingObjectId = candidate.objectId
        editorLayoutDirty = false
        editing = true
        loadEditorRichText(-1, 0, 0)
        Qt.callLater(function() {
            editor.forceActiveFocus()
            editor.deselect()
            editor.cursorPosition = editor.length
            root.syncEditorSelection()
        })
    }

    function commitEditing() {
        if (!editing)
            return

        if (resizingFrame) {
            controller.finishSelectedFrameResize()
            resizingFrame = false
            resizingObjectId = ""
        }

        syncingEditorPreview = true
        controller.updateSelectedTextPreview(
            editorPlainText(),
            editor.cursorPosition)
        syncingEditorPreview = false
        editing = false
        editingObjectId = ""
        editorLayoutDirty = false
        controller.finishSelectedTextEdit()
        reloadData()
    }

    function cancelEditing() {
        editing = false
        editingObjectId = ""
        editorLayoutDirty = false
        cancelFrameResize()
        controller.cancelSelectedTextEdit()
    }

    function prepareForSessionChange() {
        selectionActions.close()
        hideSelectCursor()
        hoverCandidate = ({})
        if (editing) {
            commitEditing()
        } else if (resizingFrame) {
            controller.finishSelectedFrameResize()
            resizingFrame = false
            resizingObjectId = ""
        }
    }

    function beginSelectedEditing() {
        if (controller.selectedPage !== page || !controller.selectedEditable)
            return

        selectionActions.close()
        beginEditing({
            objectId: controller.selectedObjectId,
            kind: controller.selectedKind,
            editable: controller.selectedEditable
        })
    }

    function deleteSelectedObject() {
        if (controller.selectedPage !== page || !controller.selectedEditable)
            return false

        controller.deleteSelectedObject()
        controller.clearSelection()
        hoverCandidate = ({})
        hideSelectCursor()
        reloadData()
        return true
    }

    function deleteSelectedImage() {
        if (editing || !isImageKind(controller.selectedKind))
            return false

        selectionActions.close()
        return deleteSelectedObject()
    }

    function selectRelativeCandidate(direction) {
        const candidates = controller.candidatesForPage(page)
        const selectable = []
        for (let index = 0; index < candidates.length; ++index) {
            if (controller.tool !== PDFEditController.EditTextTool
                    || (candidates[index].editable
                        && !isImageCandidate(candidates[index]))) {
                selectable.push(candidates[index])
            }
        }
        if (selectable.length === 0)
            return false

        let currentIndex = -1
        for (let index = 0; index < selectable.length; ++index) {
            if (controller.selectedPage === page
                    && selectable[index].objectId === controller.selectedObjectId) {
                currentIndex = index
                break
            }
        }

        const targetIndex = currentIndex < 0
            ? direction > 0 ? 0 : selectable.length - 1
            : currentIndex + direction
        if (targetIndex < 0 || targetIndex >= selectable.length)
            return false

        return controller.selectCandidate(page, selectable[targetIndex].objectId)
    }

    onPageChanged: {
        selectionActions.close()
        cancelFrameResize()
        if (editing)
            commitEditing()
        reloadData()
    }

    onPooledChanged: {
        selectionActions.close()
        if (pooled)
            cancelFrameResize()
        if (pooled && editing)
            commitEditing()
        reloadData()
    }

    onInteractionActiveChanged: reloadData()
    onDraftTextureCurrentChanged: {
        if (draftTextureCurrent)
            reloadData()
    }
    Component.onCompleted: reloadData()

    Connections {
        target: root.controller

        function onPageDataChanged(changedPage) {
            if (changedPage < 0 || changedPage === root.page)
                root.reloadData()
        }

        function onPageCandidatesReady(readyPage) {
            if (readyPage === root.page
                    && root.interactionActive
                    && hitArea.containsMouse) {
                root.updateHoverAtPosition(hitArea.mouseX, hitArea.mouseY)
            }
        }

        function onToolChanged() {
            selectionActions.close()
            root.cancelFrameResize()
            if (root.editing)
                root.commitEditing()
            root.hoverCandidate = ({})
            root.hideSelectCursor()
            root.reloadData()
        }

        function onSelectionChanged() {
            if (root.resizingFrame
                    && (root.controller.selectedPage !== root.page
                        || root.controller.selectedObjectId
                            !== root.resizingObjectId)) {
                root.resizingFrame = false
                root.resizingObjectId = ""
            }
            if (selectionActions.opened
                    && (root.controller.selectedPage !== root.page
                        || root.controller.selectedObjectId
                            !== root.selectionActionsObjectId)) {
                selectionActions.close()
            }
            if (root.editing
                    && (root.controller.selectedPage !== root.page
                        || root.controller.selectedObjectId !== root.editingObjectId)) {
                root.editing = false
                root.editingObjectId = ""
                root.editorLayoutDirty = false
                root.suppressNextEditorReload = false
                return
            }
            if (root.editing
                    && !root.syncingEditorPreview
                    && !root.syncingEditorGeometry
                    && !root.loadingEditorText
                    && !root.resizingFrame) {
                if (root.suppressNextEditorReload) {
                    root.suppressNextEditorReload = false
                    root.editorLayoutDirty = true
                    Qt.callLater(root.syncEditorContentBounds)
                    return
                }
                const cursorPosition = editor.cursorPosition
                const selectionStart = editor.selectionStart
                const selectionEnd = editor.selectionEnd
                root.loadEditorRichText(
                    cursorPosition,
                    selectionStart,
                    selectionEnd)
                root.editorLayoutDirty = true
                Qt.callLater(root.syncEditorContentBounds)
            }
        }

        function onTypingFormatApplied(start, length) {
            if (root.editing) {
                root.controller.applyTypingFormatToEditorDocument(
                    editor.textDocument,
                    start,
                    length)
            }
        }

        function onSelectionFontStyleApplied(start, length, style, enabled) {
            if (!root.editing)
                return

            root.suppressNextEditorReload = true
            root.applyingEditorDocumentFormat = true
            const applied = root.controller.applyFontStyleToEditorDocument(
                editor.textDocument,
                start,
                length,
                style,
                enabled)
            root.applyingEditorDocumentFormat = false
            if (!applied)
                root.suppressNextEditorReload = false
        }
    }

    Repeater {
        model: root.drafts

        delegate: Item {
            id: draftItem

            required property var modelData

            readonly property rect frameRect: modelData.frameRect
            readonly property rect contentBounds: modelData.contentBounds
            readonly property real frameX: frameRect.x * root.width
            readonly property real frameY: frameRect.y * root.height
            readonly property real frameWidth: Math.max(1, frameRect.width * root.width)
            readonly property real frameHeight: Math.max(1, frameRect.height * root.height)
            readonly property real boxBottom: Math.max(
                frameRect.y + frameRect.height,
                contentBounds.y + contentBounds.height) * root.height
            readonly property real boxHeight: Math.max(
                frameHeight,
                boxBottom - frameY)
            readonly property bool usesBaseline: modelData.hasBaseline === true
                && modelData.added !== true
            readonly property real textOriginX: modelData.originX * root.width
            readonly property real textBaselineY: modelData.baselineY * root.height
            readonly property real contentLeftPageX: usesBaseline
                ? textOriginX
                : frameX
            readonly property real contentRightPageX: frameX + frameWidth
            readonly property real desiredTextPageX: usesBaseline
                ? Math.min(frameX, textOriginX)
                : frameX
            readonly property real layoutContentWidth: Math.max(
                1,
                frameX + frameWidth - desiredTextPageX)
            readonly property real visibleTextWidth: Math.min(
                root.width,
                layoutContentWidth)
            readonly property real contentPageX: Math.max(
                0,
                Math.min(desiredTextPageX, root.width - visibleTextWidth))
            readonly property real leftBoxBleed: Math.min(
                root.editBoxLeftBleed,
                contentPageX)
            readonly property real rightBoxBleed: Math.min(
                root.editBoxRightBleed,
                Math.max(0, root.width - contentPageX - visibleTextWidth))
            readonly property real textPageX: contentPageX - leftBoxBleed
            readonly property real textWidth: leftBoxBleed
                + visibleTextWidth
                + rightBoxBleed
            readonly property real fontPixelSize: Math.max(
                modelData.added === true ? root.minimumTextPixelSize : 1,
                modelData.displayFontSizeRatio * root.height)
            readonly property font displayFont: root.controller.fontForDisplay(
                modelData.fontFamily,
                modelData.fontStretch,
                modelData.fontBold,
                modelData.fontItalic,
                modelData.fontUnderline,
                fontPixelSize)
            readonly property real desiredTextPageY: usesBaseline
                ? Math.min(
                    frameY - root.fallbackBleed,
                    textBaselineY - draftText.baselineOffset)
                : frameY - root.fallbackBleed
            readonly property real textPageY: Math.max(0, desiredTextPageY)
            readonly property real textTop: usesBaseline
                ? Math.max(
                    0,
                    textBaselineY - textPageY - draftText.baselineOffset)
                : root.fallbackBleed
            readonly property real frameBottom: frameY
                + frameHeight
                + root.fallbackBleed
                - textPageY
            readonly property real contentBottom: textTop
                + (isImage ? 0 : draftText.implicitHeight)
                + root.fallbackBleed
            readonly property real textHeight: Math.max(
                1,
                Math.min(
                    root.height - textPageY,
                    Math.max(frameBottom, contentBottom)))
            readonly property string richText: root.richTextHtml(
                modelData.text,
                modelData.characterSpans,
                modelData.paragraphFormats,
                modelData,
                fontPixelSize)
            readonly property bool hovered: root.hasCandidate(root.hoverCandidate)
                && root.hoverCandidate.objectId === modelData.objectId
            readonly property bool isImage: root.isImageCandidate(modelData)
            readonly property bool hasText: modelData.text.length > 0
            readonly property bool hasContent:
                modelData.deleted !== true && (isImage || hasText)
            readonly property bool selected: root.controller.selectedPage === root.page
                && root.controller.selectedObjectId === modelData.objectId

            function syncSelectedContentBounds() {
                if (!selected
                        || isImage
                        || modelData.restoration === true
                        || root.controller.selectedObjectId !== modelData.objectId) {
                    return
                }

                const selectedFrameRect = root.controller.selectedFrameRect
                const laidOutBottom = Math.min(
                    root.height,
                    draftItem.textPageY
                        + draftItem.textTop
                        + draftText.implicitHeight
                        + root.fallbackBleed)
                const contentHeight = Math.min(
                    1 - selectedFrameRect.y,
                    Math.max(
                        selectedFrameRect.height,
                        (laidOutBottom - selectedFrameRect.y * root.height)
                            / root.height))
                root.controller.updateSelectedContentBounds(Qt.rect(
                    selectedFrameRect.x,
                    selectedFrameRect.y,
                    selectedFrameRect.width,
                    contentHeight))
            }

            Connections {
                target: root

                function onSelectedDraftContentBoundsSyncRequested() {
                    draftItem.syncSelectedContentBounds()
                }
            }

            x: textPageX
            y: textPageY
            width: textWidth
            height: textHeight
            visible: !(root.editing && root.editingObjectId === modelData.objectId)
            z: modelData.restoration === true ? 0 : 1

            Repeater {
                model: draftItem.modelData.sourceRects

                delegate: Rectangle {
                    id: draftSourceMask

                    required property var modelData

                    readonly property real sourceX: modelData.x * root.width
                    readonly property real sourceY: modelData.y * root.height
                    readonly property real sourceWidth: Math.max(
                        1,
                        modelData.width * root.width)
                    readonly property real sourceHeight: Math.max(
                        1,
                        modelData.height * root.height)
                    readonly property real maskedX: Math.max(
                        0,
                        sourceX - root.fallbackBleed)
                    readonly property real maskedY: Math.max(
                        0,
                        sourceY - root.fallbackBleed)
                    readonly property real maskedRight: Math.min(
                        root.width,
                        sourceX + sourceWidth + root.fallbackBleed)
                    readonly property real maskedBottom: Math.min(
                        root.height,
                        sourceY + sourceHeight + root.fallbackBleed)

                    x: maskedX - draftItem.x
                    y: maskedY - draftItem.y
                    width: Math.max(1, maskedRight - maskedX)
                    height: Math.max(1, maskedBottom - maskedY)
                    color: "#fffdfb"
                    visible: !root.draftTextureReady
                        && draftItem.modelData.added !== true
                }
            }

            Text {
                id: draftText

                x: 0
                y: draftItem.textTop
                width: draftItem.width
                height: implicitHeight
                leftPadding: Math.max(
                    0,
                    draftItem.contentLeftPageX - draftItem.x)
                rightPadding: Math.max(
                    0,
                    draftItem.x + draftItem.width - draftItem.contentRightPageX)
                text: draftItem.richText
                textFormat: Text.RichText
                color: draftItem.modelData.color
                font: draftItem.displayFont
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                elide: Text.ElideNone
                visible: !draftItem.isImage
                onImplicitHeightChanged: {
                    if (draftItem.selected && root.resizingFrame)
                        Qt.callLater(root.syncSelectedDraftContentBounds)
                }
            }

            DraftImageView {
                x: draftItem.frameX - draftItem.x
                y: draftItem.frameY - draftItem.y
                width: draftItem.frameWidth
                height: draftItem.frameHeight
                sourceImage: draftItem.modelData.image
                smooth: true
                visible: draftItem.isImage
                    && draftItem.modelData.deleted !== true
                Accessible.ignored: true
            }

            Rectangle {
                x: draftItem.frameX - draftItem.x - root.boundingBoxPadding
                y: draftItem.frameY - draftItem.y - root.boundingBoxPadding
                width: draftItem.frameWidth + 2 * root.boundingBoxPadding
                height: draftItem.boxHeight + 2 * root.boundingBoxPadding
                radius: root.editBoxRadius
                color: "transparent"
                border.width: 2
                border.color: root.editBoxColor
                visible: root.interactionActive
                    && !root.editing
                    && draftItem.modelData.restoration !== true
                    && draftItem.hasContent
                    && draftItem.hovered
                    && !draftItem.selected
            }

            MouseArea {
                anchors.fill: parent
                enabled: root.interactionActive
                    && draftItem.modelData.restoration !== true
                    && draftItem.hasContent
                    && root.controller.tool !== PDFEditController.AddTextTool
                    && root.controller.tool !== PDFEditController.AddImageTool
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                preventStealing: false
                cursorShape: draftItem.isImage
                    ? Qt.ArrowCursor
                    : root.controller.tool === PDFEditController.SelectTool
                    ? root.selectCursorActive ? Qt.BlankCursor : Qt.ArrowCursor
                    : Qt.IBeamCursor
                onEntered: {
                    root.updateSelectCursor(
                        draftItem.x + mouseX,
                        draftItem.y + mouseY,
                        draftItem.modelData)
                    root.hoverCandidate = draftItem.modelData
                }
                onPositionChanged: mouse => {
                    root.updateSelectCursor(
                        draftItem.x + mouse.x,
                        draftItem.y + mouse.y,
                        draftItem.modelData)
                    root.hoverCandidate = draftItem.modelData
                }
                onExited: {
                    root.hideSelectCursor()
                    if (root.hasCandidate(root.hoverCandidate)
                            && root.hoverCandidate.objectId
                                === draftItem.modelData.objectId) {
                        root.hoverCandidate = ({})
                    }
                }
                onPressed: hitArea.forceActiveFocus()
                onClicked: mouse => {
                    if (root.controller.tool === PDFEditController.SelectTool) {
                        root.showSelectionActions(
                            draftItem.x + mouse.x,
                            draftItem.y + mouse.y,
                            draftItem.modelData)
                    } else {
                        root.selectCandidate(draftItem.modelData)
                    }
                }
                onDoubleClicked: {
                    if (!draftItem.isImage)
                        root.beginEditing(draftItem.modelData)
                }
            }
        }
    }

    MouseArea {
        id: hitArea

        anchors.fill: parent
        enabled: root.interactionActive
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        preventStealing: false
        cursorShape: root.selectCursorActive || root.addTextCursorActive
            ? Qt.BlankCursor
            : root.controller.tool === PDFEditController.AddTextTool
                    || root.controller.tool === PDFEditController.AddImageTool
            ? Qt.IBeamCursor
            : root.hasCandidate(root.hoverCandidate)
            ? !root.hoverCandidate.editable
                ? Qt.ForbiddenCursor
                : root.isImageCandidate(root.hoverCandidate)
                    ? Qt.ArrowCursor
                    : Qt.IBeamCursor
            : Qt.ArrowCursor
        activeFocusOnTab: true
        Accessible.role: Accessible.Pane
        //% "PDF edit surface"
        Accessible.name: qsTrId("PdfEditSurface")
        //% "Use Tab or arrow keys to select content, Enter to edit text, drag images to move, and Backspace or Delete to remove images"
        Accessible.description: qsTrId("PdfEditSurfaceDescription")

        onEntered: root.updateHoverAtPosition(mouseX, mouseY)
        onPositionChanged: mouse => root.updateHoverAtPosition(mouse.x, mouse.y)
        onExited: {
            root.hideSelectCursor()
            root.hoverCandidate = ({})
        }
        onPressed: forceActiveFocus()
        onClicked: mouse => {
            if (root.controller.tool === PDFEditController.AddTextTool) {
                root.beginAddingText(mouse.x, mouse.y)
                return
            }
            if (root.controller.tool === PDFEditController.AddImageTool) {
                root.beginAddingImage(mouse.x, mouse.y)
                return
            }

            root.hoverCandidate = root.candidateAtPosition(mouse.x, mouse.y)
            if (root.hasCandidate(root.hoverCandidate)) {
                if (root.controller.tool === PDFEditController.SelectTool
                        && root.hoverCandidate.editable) {
                    root.showSelectionActions(
                        mouse.x,
                        mouse.y,
                        root.hoverCandidate)
                } else {
                    selectionActions.close()
                    root.selectCandidate(root.hoverCandidate)
                }
            } else {
                selectionActions.close()
                root.controller.clearSelection()
            }
        }
        onDoubleClicked: mouse => {
            if (root.controller.tool === PDFEditController.AddTextTool
                    || root.controller.tool === PDFEditController.AddImageTool)
                return

            root.hoverCandidate = root.candidateAtPosition(mouse.x, mouse.y)
            if (root.hasCandidate(root.hoverCandidate))
                root.beginEditing(root.hoverCandidate)
        }

        Keys.onReturnPressed: event => {
            root.beginSelectedEditing()
            event.accepted = true
        }
        Keys.onEnterPressed: event => {
            root.beginSelectedEditing()
            event.accepted = true
        }
        Keys.onTabPressed: event => {
            event.accepted = root.selectRelativeCandidate(1)
        }
        Keys.onBacktabPressed: event => {
            event.accepted = root.selectRelativeCandidate(-1)
        }
        Keys.onDownPressed: event => {
            event.accepted = root.selectRelativeCandidate(1)
        }
        Keys.onUpPressed: event => {
            event.accepted = root.selectRelativeCandidate(-1)
        }
    }

    Popup {
        id: selectionActions

        readonly property bool imageSelection:
            root.isImageKind(root.controller.selectedKind)

        parent: root
        width: imageSelection ? 71 : 136
        height: 62
        padding: 4
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        function openAt(pointX, pointY) {
            const margin = 4
            const gap = 8
            x = Math.max(
                margin,
                Math.min(root.width - width - margin, pointX - width / 2))
            const aboveY = pointY - height - gap
            y = aboveY >= margin
                ? aboveY
                : Math.max(
                    margin,
                    Math.min(root.height - height - margin, pointY + gap))
            open()
        }

        onClosed: root.selectionActionsObjectId = ""

        background: Rectangle {
            radius: 6
            color: root.theme.ribbonSurface
            border.width: 1
            border.color: root.theme.ribbonBorder
        }

        contentItem: Row {
            spacing: 2
            focus: true

            Keys.onPressed: event => {
                if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete)
                    event.accepted = root.deleteSelectedImage()
            }

            ToolButton {
                id: editAction

                width: 63
                height: 54
                display: AbstractButton.TextUnderIcon
                spacing: 2
                //% "Edit"
                text: qsTrId("Edit")
                icon.source: "../assets/icons/edit-text.svg"
                icon.width: 24
                icon.height: 24
                icon.color: "transparent"
                font.pixelSize: 10
                visible: !selectionActions.imageSelection

                background: Rectangle {
                    radius: 4
                    color: editAction.down
                        ? "#f5dfcf"
                        : editAction.hovered
                            ? root.theme.foxitPaleOrange
                            : "transparent"
                }

                onClicked: {
                    selectionActions.close()
                    root.beginSelectedEditing()
                }
            }

            ToolButton {
                id: deleteAction

                width: 63
                height: 54
                display: AbstractButton.TextUnderIcon
                spacing: 2
                //% "Delete"
                text: qsTrId("Delete")
                icon.source: "../assets/icons/remove-text.svg"
                icon.width: 24
                icon.height: 24
                icon.color: "transparent"
                font.pixelSize: 10

                background: Rectangle {
                    radius: 4
                    color: deleteAction.down
                        ? "#f4d9d7"
                        : deleteAction.hovered
                            ? "#fff0ef"
                            : "transparent"
                }

                onClicked: {
                    selectionActions.close()
                    root.deleteSelectedObject()
                }
            }
        }
    }

    Image {
        id: selectCursorImage

        x: root.selectCursorX - 3
        y: root.selectCursorY - 3
        width: 22
        height: 22
        source: "../assets/icons/mouse-pointer-filled-orange.svg"
        sourceSize.width: 22
        sourceSize.height: 22
        fillMode: Image.PreserveAspectFit
        smooth: true
        visible: root.selectCursorActive
            && root.interactionActive
            && root.controller.tool === PDFEditController.SelectTool
        z: 4
        Accessible.ignored: true
    }

    Image {
        id: addTextCursorImage

        x: hitArea.mouseX - width / 2
        y: hitArea.mouseY - height / 2
        width: 22
        height: 22
        source: "../assets/icons/add.svg"
        sourceSize.width: 22
        sourceSize.height: 22
        fillMode: Image.PreserveAspectFit
        smooth: true
        visible: root.addTextCursorActive
        z: 4
        Accessible.ignored: true
    }

    Rectangle {
        readonly property rect frameRect: root.hasCandidate(root.hoverCandidate)
            ? root.hoverCandidate.frameRect
            : Qt.rect(0, 0, 0, 0)
        readonly property rect contentBounds: root.hasCandidate(root.hoverCandidate)
            ? root.hoverCandidate.contentBounds
            : frameRect
        readonly property real frameX: root.hasCandidate(root.hoverCandidate)
            ? frameRect.x * root.width
            : 0
        readonly property real frameY: root.hasCandidate(root.hoverCandidate)
            ? frameRect.y * root.height
            : 0
        readonly property real frameWidth: root.hasCandidate(root.hoverCandidate)
            ? Math.max(1, frameRect.width * root.width)
            : 0
        readonly property real frameHeight: root.hasCandidate(root.hoverCandidate)
            ? Math.max(1, frameRect.height * root.height)
            : 0
        readonly property real boxHeight: root.hasCandidate(root.hoverCandidate)
            ? Math.max(
                frameHeight,
                (contentBounds.y + contentBounds.height - frameRect.y)
                    * root.height)
            : 0

        x: frameX - root.boundingBoxPadding
        y: frameY - root.boundingBoxPadding
        width: frameWidth + 2 * root.boundingBoxPadding
        height: boxHeight + 2 * root.boundingBoxPadding
        radius: root.editBoxRadius
        color: "transparent"
        border.width: 2
        border.color: root.hasCandidate(root.hoverCandidate) && root.hoverCandidate.editable
            ? root.editBoxColor
            : "#a5a7aa"
        z: 2
        visible: root.interactionActive
            && !root.editing
            && root.hasCandidate(root.hoverCandidate)
            && root.hoverCandidate.added !== true
            && (root.controller.selectedPage !== root.page
                || root.controller.selectedObjectId
                    !== root.hoverCandidate.objectId)
            && !root.hasActiveDraft(root.hoverCandidate.objectId)
    }

    Rectangle {
        readonly property rect frameRect: root.controller.selectedFrameRect
        readonly property rect contentBounds: root.controller.selectedContentBounds
        readonly property real frameX: frameRect.x * root.width
        readonly property real frameY: frameRect.y * root.height
        readonly property real frameWidth: frameRect.width * root.width
        readonly property real frameHeight: frameRect.height * root.height
        readonly property real boxHeight: Math.max(
            frameHeight,
            (contentBounds.y + contentBounds.height - frameRect.y)
                * root.height)

        x: frameX - root.boundingBoxPadding
        y: frameY - root.boundingBoxPadding
        width: Math.max(1, frameWidth) + 2 * root.boundingBoxPadding
        height: Math.max(1, boxHeight) + 2 * root.boundingBoxPadding
        radius: root.editBoxRadius
        color: "#14d97a32"
        border.width: 2
        border.color: root.controller.selectedEditable
            ? root.editBoxColor
            : "#8a8d92"
        z: 2
        visible: root.interactionActive
            && !root.editing
            && root.controller.selectedPage === root.page
            && root.controller.selectedObjectId.length > 0
            && root.controller.selectedKind !== "addedText"
            && !root.controller.selectedEditable
            && !root.hasActiveDraft(root.controller.selectedObjectId)
    }

    Repeater {
        model: root.controller.selectedSourceRects

        delegate: Rectangle {
            id: editorSourceMask

            required property var modelData

            readonly property real sourceX: modelData.x * root.width
            readonly property real sourceY: modelData.y * root.height
            readonly property real sourceWidth: Math.max(1, modelData.width * root.width)
            readonly property real sourceHeight: Math.max(1, modelData.height * root.height)
            readonly property real maskedX: Math.max(0, sourceX - root.fallbackBleed)
            readonly property real maskedY: Math.max(0, sourceY - root.fallbackBleed)
            readonly property real maskedRight: Math.min(
                root.width,
                sourceX + sourceWidth + root.fallbackBleed)
            readonly property real maskedBottom: Math.min(
                root.height,
                sourceY + sourceHeight + root.fallbackBleed)

            x: maskedX
            y: maskedY
            width: Math.max(1, maskedRight - maskedX)
            height: Math.max(1, maskedBottom - maskedY)
            color: "#fffdfb"
            z: 2
            visible: editorFrame.visible
                && root.controller.selectedKind !== "addedText"
                && !root.editorUsesCleanTexture
        }
    }

    Item {
        id: editorFrame

        readonly property rect frameRect: root.controller.selectedFrameRect
        readonly property rect contentBounds: root.controller.selectedContentBounds
        readonly property real frameX: frameRect.x * root.width
        readonly property real frameY: frameRect.y * root.height
        readonly property real frameWidth: frameRect.width * root.width
        readonly property real frameHeight: frameRect.height * root.height
        readonly property real boxHeight: Math.max(
            frameHeight,
            (contentBounds.y + contentBounds.height - frameRect.y)
                * root.height)
        readonly property bool editsAddedText: root.controller.selectedKind === "addedText"
        readonly property bool editsGroupedText: root.controller.selectedSourceRects.length > 1
        readonly property var baseFormat: root.selectedBaseFormat()
        readonly property real leftBleed: editsAddedText
            ? 0
            : root.editBoxLeftBleed
        readonly property real rightBleed: editsAddedText
            ? 0
            : root.editBoxRightBleed
        readonly property real verticalBleed: editsAddedText
            ? 0
            : root.editBoxVerticalBleed
        readonly property real baseFontPixelSize: Math.max(
            editsAddedText ? root.minimumTextPixelSize : 1,
            (Number(baseFormat.displayFontSizeRatio) > 0
                ? Number(baseFormat.displayFontSizeRatio)
                : Number(baseFormat.fontSizeRatio)) * root.height)
        readonly property font displayFont: root.controller.fontForDisplay(
            String(baseFormat.fontFamily || ""),
            Number(baseFormat.fontStretch || 100),
            baseFormat.fontBold === true,
            baseFormat.fontItalic === true,
            baseFormat.fontUnderline === true,
            baseFontPixelSize)
        readonly property bool usesBaseline: !editsAddedText
            && root.controller.selectedHasBaseline
        readonly property real textOriginX: root.controller.selectedOriginX * root.width
        readonly property real textBaselineY: root.controller.selectedBaselineY * root.height
        readonly property real contentLeftPageX: usesBaseline ? textOriginX : frameX
        readonly property real contentRightPageX: frameX + Math.max(1, frameWidth)
        readonly property real inputMargin: editsAddedText ? 1 : 2
        readonly property real desiredContentPageX: usesBaseline
            ? Math.min(frameX, textOriginX)
            : frameX
        readonly property real layoutContentWidth: Math.max(
            1,
            frameX + Math.max(1, frameWidth) - desiredContentPageX)
        readonly property real visibleContentWidth: Math.min(
            root.width,
            layoutContentWidth)
        readonly property real contentPageX: Math.max(
            0,
            Math.min(desiredContentPageX, root.width - visibleContentWidth))
        readonly property real visibleLeftBleed: Math.min(leftBleed, contentPageX)
        readonly property real visibleRightBleed: Math.min(
            rightBleed,
            Math.max(0, root.width - contentPageX - visibleContentWidth))
        readonly property real desiredPageY: usesBaseline
            ? Math.min(
                frameY - verticalBleed,
                textBaselineY - editor.baselineOffset)
            : frameY - verticalBleed
        readonly property real contentTop: usesBaseline
            ? Math.max(0, textBaselineY - y - editor.baselineOffset)
            : inputMargin
        readonly property real frameBottom: frameY
            + Math.max(1, frameHeight)
            + verticalBleed
            - y
        readonly property real editorBottom: contentTop
            + editor.contentHeight
            + (usesBaseline ? verticalBleed : inputMargin)
        readonly property real desiredHeight: Math.max(
            editsAddedText ? 1 : 17,
            frameBottom,
            editorBottom)

        x: contentPageX - visibleLeftBleed
        y: Math.max(0, desiredPageY)
        width: visibleLeftBleed + visibleContentWidth + visibleRightBleed
        height: Math.max(1, Math.min(root.height - y, desiredHeight))
        z: 2
        visible: root.editing
            && root.controller.selectedPage === root.page
            && root.controller.selectedEditable

        Rectangle {
            x: editorFrame.frameX - editorFrame.x - root.boundingBoxPadding
            y: editorFrame.frameY - editorFrame.y - root.boundingBoxPadding
            width: Math.max(1, editorFrame.frameWidth)
                + 2 * root.boundingBoxPadding
            height: Math.max(1, editorFrame.boxHeight)
                + 2 * root.boundingBoxPadding
            radius: root.editBoxRadius
            color: editorFrame.editsAddedText
                    || editorFrame.editsGroupedText
                    || root.editorUsesCleanTexture
                ? "transparent"
                : "#fffdfb"
            Accessible.ignored: true
        }

        TextEdit {
            id: editor

            x: 0
            y: editorFrame.contentTop
            width: parent.width
            height: Math.max(1, contentHeight)
            leftPadding: Math.max(
                0,
                editorFrame.contentLeftPageX - editorFrame.x)
            rightPadding: Math.max(
                0,
                editorFrame.x + editorFrame.width - editorFrame.contentRightPageX)
            topPadding: 0
            bottomPadding: 0
            textMargin: 0
            color: editorFrame.baseFormat.color
            selectionColor: "#DCEBFF"
            selectedTextColor: root.theme.ribbonTextColor
            font: editorFrame.displayFont
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
            selectByMouse: true
            selectByKeyboard: true
            persistentSelection: true
            verticalAlignment: TextEdit.AlignTop
            clip: true
            onTextChanged: Qt.callLater(root.syncEditorDocumentFontStyles)
            onTextEdited: {
                if (root.loadingEditorText)
                    return

                root.editorLayoutDirty = true
                root.syncingEditorPreview = true
                root.controller.updateSelectedTextPreview(
                    root.editorPlainText(),
                    editor.cursorPosition)
                root.syncingEditorPreview = false
                root.syncEditorSelection()
                Qt.callLater(root.syncEditorContentBounds)
            }
            onContentHeightChanged: {
                if (root.editorLayoutDirty)
                    Qt.callLater(root.syncEditorContentBounds)
            }
            onSelectionStartChanged: Qt.callLater(root.syncEditorSelection)
            onSelectionEndChanged: Qt.callLater(root.syncEditorSelection)

            Keys.onEscapePressed: event => {
                root.cancelEditing()
                event.accepted = true
            }
            Keys.onPressed: event => {
                const applyModifier = (event.modifiers & Qt.ControlModifier)
                    || (event.modifiers & Qt.MetaModifier)
                if (applyModifier
                        && (event.key === Qt.Key_Return
                            || event.key === Qt.Key_Enter)) {
                    root.commitEditing()
                    event.accepted = true
                }
            }
        }
    }

    Item {
        id: resizeOverlay

        readonly property rect layoutFrameRect: root.controller.selectedFrameRect
        readonly property rect contentBounds: root.controller.selectedContentBounds
        readonly property rect frameRect: Qt.rect(
            layoutFrameRect.x,
            layoutFrameRect.y,
            layoutFrameRect.width,
            Math.max(
                layoutFrameRect.height,
                contentBounds.y + contentBounds.height - layoutFrameRect.y))

        x: frameRect.x * root.width - root.boundingBoxPadding
        y: frameRect.y * root.height - root.boundingBoxPadding
        width: Math.max(1, frameRect.width * root.width)
            + 2 * root.boundingBoxPadding
        height: Math.max(1, frameRect.height * root.height)
            + 2 * root.boundingBoxPadding
        z: 6
        visible: root.interactionActive
            && root.controller.selectedPage === root.page
            && root.controller.selectedObjectId.length > 0
            && root.controller.selectedEditable
            && (root.editing
                || (root.controller.tool !== PDFEditController.AddTextTool
                    && root.controller.tool !== PDFEditController.AddImageTool)
                || root.controller.selectedKind === "addedImage")

        Rectangle {
            anchors.fill: parent
            radius: root.editBoxRadius
            color: "#14d97a32"
            border.width: 2
            border.color: root.editBoxColor
            Accessible.ignored: true
        }

        MouseArea {
            id: moveImageMouse

            property rect dragStartFrameRect: Qt.rect(0, 0, 0, 0)
            property point dragStartPosition: Qt.point(0, 0)

            anchors.fill: parent
            enabled: !root.editing
                && root.isImageKind(root.controller.selectedKind)
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeAllCursor
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            //% "Move image"
            Accessible.name: qsTrId("MoveImage")

            function commitKeyboardMove(deltaX, deltaY) {
                const startFrameRect = resizeOverlay.layoutFrameRect
                root.controller.beginSelectedFrameResize()
                root.resizingFrame = true
                root.resizingObjectId = root.controller.selectedObjectId
                root.controller.updateSelectedFrameRect(
                    root.movedFrameRect(startFrameRect, deltaX, deltaY))
                root.controller.finishSelectedFrameResize()
                root.resizingFrame = false
                root.resizingObjectId = ""
                root.reloadData()
            }

            onPressed: mouse => {
                forceActiveFocus()
                selectionActions.close()
                dragStartFrameRect = resizeOverlay.layoutFrameRect
                dragStartPosition = mapToItem(root, mouse.x, mouse.y)
                root.controller.beginSelectedFrameResize()
                root.resizingFrame = true
                root.resizingObjectId = root.controller.selectedObjectId
                mouse.accepted = true
            }
            onPositionChanged: mouse => {
                if (!pressed || !root.resizingFrame)
                    return

                const currentPosition = mapToItem(root, mouse.x, mouse.y)
                root.controller.updateSelectedFrameRect(root.movedFrameRect(
                    dragStartFrameRect,
                    currentPosition.x - dragStartPosition.x,
                    currentPosition.y - dragStartPosition.y))
            }
            onReleased: {
                if (!root.resizingFrame)
                    return

                root.controller.finishSelectedFrameResize()
                root.resizingFrame = false
                root.resizingObjectId = ""
                root.reloadData()
            }
            onCanceled: root.cancelFrameResize()

            Keys.onPressed: event => {
                const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
                let deltaX = 0
                let deltaY = 0
                if (event.key === Qt.Key_Left)
                    deltaX = -step
                else if (event.key === Qt.Key_Right)
                    deltaX = step
                else if (event.key === Qt.Key_Up)
                    deltaY = -step
                else if (event.key === Qt.Key_Down)
                    deltaY = step
                else
                    return

                commitKeyboardMove(deltaX, deltaY)
                event.accepted = true
            }
        }

        Repeater {
            model: root.controller.selectedKind === "image" ? [] : [
                { horizontal: 0, vertical: -1, cursor: Qt.SizeVerCursor },
                { horizontal: 1, vertical: 0, cursor: Qt.SizeHorCursor },
                { horizontal: 0, vertical: 1, cursor: Qt.SizeVerCursor },
                { horizontal: -1, vertical: 0, cursor: Qt.SizeHorCursor }
            ]

            delegate: Item {
                id: resizeHandle

                required property var modelData
                property rect dragStartFrameRect: Qt.rect(0, 0, 0, 0)
                property point dragStartPosition: Qt.point(0, 0)
                readonly property string accessibilityName: {
                    if (root.controller.selectedKind === "addedImage") {
                        //% "Resize image"
                        return qsTrId("ResizeImage")
                    }
                    //% "Resize text box"
                    return qsTrId("ResizeTextBox")
                }

                x: modelData.horizontal < 0
                    ? -width / 2
                    : modelData.horizontal > 0
                        ? resizeOverlay.width - width / 2
                        : (resizeOverlay.width - width) / 2
                y: modelData.vertical < 0
                    ? -height / 2
                    : modelData.vertical > 0
                        ? resizeOverlay.height - height / 2
                        : (resizeOverlay.height - height) / 2
                width: root.resizeHandleHitSize
                height: root.resizeHandleHitSize

                Rectangle {
                    anchors.centerIn: parent
                    width: root.resizeHandleSize
                    height: root.resizeHandleSize
                    radius: width / 2
                    color: root.editBoxColor
                    border.width: 1
                    border.color: "#fffdfb"
                    Accessible.ignored: true
                }

                MouseArea {
                    id: resizeMouse

                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: resizeHandle.modelData.cursor
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: resizeHandle.accessibilityName

                    onPressed: mouse => {
                        forceActiveFocus()
                        resizeHandle.dragStartFrameRect
                            = resizeOverlay.frameRect
                        if (root.editing)
                            root.editorLayoutDirty = true
                        resizeHandle.dragStartPosition = mapToItem(
                            root,
                            mouse.x,
                            mouse.y)
                        root.controller.beginSelectedFrameResize()
                        root.resizingFrame = true
                        root.resizingObjectId
                            = root.controller.selectedObjectId
                        mouse.accepted = true
                    }
                    onPositionChanged: mouse => {
                        if (!pressed || !root.resizingFrame)
                            return

                        const currentPosition = mapToItem(root, mouse.x, mouse.y)
                        root.controller.updateSelectedFrameRect(
                            root.resizedFrameRect(
                                resizeHandle.dragStartFrameRect,
                                resizeHandle.modelData.horizontal,
                                resizeHandle.modelData.vertical,
                                currentPosition.x
                                    - resizeHandle.dragStartPosition.x,
                                currentPosition.y
                                    - resizeHandle.dragStartPosition.y))
                        if (root.editing)
                            Qt.callLater(root.syncEditorContentBounds)
                        else
                            Qt.callLater(root.syncSelectedDraftContentBounds)
                    }
                    onReleased: {
                        if (!root.resizingFrame)
                            return

                        if (root.editing)
                            root.syncEditorContentBounds()
                        else
                            root.syncSelectedDraftContentBounds()
                        root.controller.finishSelectedFrameResize()
                        root.resizingFrame = false
                        root.resizingObjectId = ""
                        root.reloadData()
                    }
                    onCanceled: root.cancelFrameResize()

                    Keys.onPressed: event => {
                        const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
                        let deltaX = 0
                        let deltaY = 0
                        if (resizeHandle.modelData.horizontal !== 0) {
                            if (event.key === Qt.Key_Left)
                                deltaX = -step
                            else if (event.key === Qt.Key_Right)
                                deltaX = step
                        }
                        if (resizeHandle.modelData.vertical !== 0) {
                            if (event.key === Qt.Key_Up)
                                deltaY = -step
                            else if (event.key === Qt.Key_Down)
                                deltaY = step
                        }
                        if (deltaX === 0 && deltaY === 0)
                            return

                        const startFrameRect
                            = resizeOverlay.frameRect
                        if (root.editing)
                            root.editorLayoutDirty = true
                        root.controller.beginSelectedFrameResize()
                        root.resizingFrame = true
                        root.resizingObjectId
                            = root.controller.selectedObjectId
                        root.controller.updateSelectedFrameRect(
                            root.resizedFrameRect(
                                startFrameRect,
                                resizeHandle.modelData.horizontal,
                                resizeHandle.modelData.vertical,
                                deltaX,
                                deltaY))
                        if (root.editing)
                            root.syncEditorContentBounds()
                        else
                            root.syncSelectedDraftContentBounds()
                        root.controller.finishSelectedFrameResize()
                        root.resizingFrame = false
                        root.resizingObjectId = ""
                        root.reloadData()
                        if (root.editing)
                            Qt.callLater(root.syncEditorContentBounds)
                        event.accepted = true
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.left: resizeOverlay.left
        anchors.bottom: resizeOverlay.top
        anchors.bottomMargin: 4
        width: editorHint.implicitWidth + 14
        height: 22
        radius: 3
        color: "#e63a3b3f"
        z: 7
        visible: editorFrame.visible

        Label {
            id: editorHint

            anchors.centerIn: parent
            //% "Ctrl/Cmd+Enter to apply · Esc to cancel"
            text: qsTrId("ApplyOrCancelEditHint")
            color: "white"
            font.pixelSize: 10
        }
    }

    Rectangle {
        x: Math.min(Math.max(0, root.controller.selectedFrameRect.x * root.width),
                    Math.max(0, root.width - width))
        y: Math.max(
            0,
            root.controller.selectedFrameRect.y * root.height - height - 5)
        width: Math.min(root.width, selectionReason.implicitWidth + 18)
        height: 26
        radius: 3
        color: "#e65d6065"
        z: 2
        visible: root.interactionActive
            && root.controller.selectedPage === root.page
            && root.controller.selectedObjectId.length > 0
            && !root.controller.selectedEditable

        Label {
            id: selectionReason

            anchors.fill: parent
            anchors.leftMargin: 9
            anchors.rightMargin: 9
            text: root.controller.selectedReason
            color: "white"
            font.pixelSize: 11
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
    }
}
