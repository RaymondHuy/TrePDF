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
    required property var displayedSuppressedCommentObjectIds
    required property AppTheme theme
    required property PDFEditController controller
    required property real pagePointScale
    // Lay out in PDF points, then scale the result with the page. Rounding the
    // font separately at each zoom otherwise changes wrapping on editor entry.
    readonly property real textLayoutScale: Math.max(0.01, pagePointScale)
    readonly property real textLayoutPageHeight: height / textLayoutScale
    readonly property real selectionBottomRatio: controller.headerSelectionOnly
        && controller.tool === PDFEditController.SelectTool
        ? Math.min(1, 100 / Math.max(1, textLayoutPageHeight)) : 1
    property int viewRotation: 0

    property var drafts: []
    property var contentDrafts: []
    property var highlightDrafts: []
    property var stickyNoteDrafts: []
    property var freeTextDrafts: []
    property var drawingCommentDrafts: []
    property var inkDrafts: []
    property var stampDrafts: []
    property var hoverCandidate: ({})
    property bool editing: false
    property string editingObjectId: ""
    property int editFocusGeneration: 0
    property PDFEditController textEditPreparationController: null
    property int textEditPreparationPage: -1
    property string textEditPreparationObjectId: ""
    property bool loadingEditorText: false
    property bool syncingEditorPreview: false
    property bool syncingEditorGeometry: false
    property bool applyingEditorDocumentFormat: false
    property bool suppressNextEditorReload: false
    property bool editorLayoutDirty: false
    property bool resizingFrame: false
    property string resizingObjectId: ""
    property var frameResizePreview: ({})
    readonly property string frameResizePreviewKind:
        frameResizePreview.kind === undefined
            ? ""
            : String(frameResizePreview.kind)
    readonly property rect frameResizePreviewSourceRect:
        frameResizePreview.frameRect === undefined
            ? Qt.rect(0, 0, 0, 0)
            : frameResizePreview.frameRect
    readonly property var frameResizePreviewVertices:
        normalizedPointsForFrame(
            frameResizePreview.annotationPoints === undefined
                ? []
                : frameResizePreview.annotationPoints,
            frameResizePreviewSourceRect)
    readonly property var frameResizePreviewStrokes:
        normalizedStrokesForFrame(
            frameResizePreview.inkStrokes === undefined
                ? []
                : frameResizePreview.inkStrokes,
            frameResizePreviewSourceRect)
    readonly property var frameResizePreviewRects:
        normalizedRectsForFrame(
            frameResizePreview.highlightRects === undefined
                ? []
                : frameResizePreview.highlightRects,
            frameResizePreviewSourceRect)
    property bool editingSourceTextMoveOnly: false
    property real editingSourceTranslationX: 0
    property real editingSourceTranslationY: 0
    property string selectionActionsObjectId: ""
    property real selectCursorX: 0
    property real selectCursorY: 0
    property bool selectCursorVisible: false
    property bool highlighting: false
    property point highlightStart: Qt.point(0, 0)
    property point highlightCurrent: Qt.point(0, 0)
    property var textMarkupPreviewRects: []
    property bool textMarkupPreviewUpdatePending: false
    property bool drawingShape: false
    property point shapeStart: Qt.point(0, 0)
    property point shapeCurrent: Qt.point(0, 0)
    property bool placingFreeText: false
    property point freeTextStart: Qt.point(0, 0)
    property point freeTextCurrent: Qt.point(0, 0)
    property bool placingStamp: false
    property point stampStart: Qt.point(0, 0)
    property point stampCurrent: Qt.point(0, 0)
    property bool drawingCommentDragging: false
    property int drawingInteractionPage: -1
    property point drawingCommentStart: Qt.point(0, 0)
    property point drawingCommentCurrent: Qt.point(0, 0)
    property bool drawingPathActive: false
    property var drawingPathPoints: []
    property point drawingPathHover: Qt.point(0, 0)
    property bool inkDrawing: false
    property int inkInteractionPage: -1
    property var inkStrokePoints: []
    property var inkPreviewStrokePoints: []
    property bool inkPreviewUpdatePending: false
    readonly property var drawingPreviewVertices: {
        if (drawingCommentDragging && width > 0 && height > 0) {
            return [
                Qt.point(
                    drawingCommentStart.x / width,
                    drawingCommentStart.y / height),
                Qt.point(
                    drawingCommentCurrent.x / width,
                    drawingCommentCurrent.y / height)
            ]
        }
        if (!drawingPathActive)
            return []

        const points = drawingPathPoints.slice(0)
        points.push(drawingPathHover)
        return points
    }
    property bool suppressNextPlacementClick: false
    readonly property real fallbackBleed: 2
    readonly property real editBoxRadius: 4
    readonly property color editBoxColor: "#f5a55a"
    readonly property real editBoxLeftBleed: 3
    readonly property real editBoxRightBleed: 3
    readonly property real editBoxVerticalBleed: 3
    readonly property real boundingBoxPadding: 3
    readonly property real minimumTextPixelSize: 1
    readonly property real resizeHandleSize: 8
    readonly property real resizeHandleHitSize: 24
    readonly property real minimumResizePixelSize: 24
    readonly property real minimumTextMarkupDragSize: 3
    readonly property real minimumFreeTextDragSize: 3
    readonly property real minimumStampDragSize: 3
    readonly property real minimumInkSampleDistance: 1
    readonly property int maximumInkPoints: 4096
    readonly property real defaultFreeTextWidthPoints: 160
    readonly property real defaultFreeTextHeightPoints: 64
    readonly property real defaultStampWidthPoints: 160
    readonly property real defaultStampHeightPoints: 50
    readonly property rect activeTextMarkupDragRect: Qt.rect(
        Math.min(highlightStart.x, highlightCurrent.x),
        Math.min(highlightStart.y, highlightCurrent.y),
        Math.abs(highlightCurrent.x - highlightStart.x),
        Math.abs(highlightCurrent.y - highlightStart.y))
    readonly property rect activeTextMarkupSelectionRect: {
        const selectionRect = activeTextMarkupDragRect
        if (selectionRect.height >= minimumTextMarkupDragSize || height <= 0)
            return selectionRect

        const selectionHeight = Math.min(
            height,
            minimumTextMarkupDragSize)
        const selectionCenterY = selectionRect.y + selectionRect.height / 2
        const selectionY = Math.max(
            0,
            Math.min(
                height - selectionHeight,
                selectionCenterY - selectionHeight / 2))
        return Qt.rect(
            selectionRect.x,
            selectionY,
            selectionRect.width,
            selectionHeight)
    }
    readonly property string activeTextMarkupKind: {
        switch (controller.tool) {
        case PDFEditController.HighlightTool:
            return "highlight"
        case PDFEditController.UnderlineTool:
            return "underline"
        case PDFEditController.SquigglyTool:
            return "squiggly"
        case PDFEditController.StrikeoutTool:
            return "strikeout"
        default:
            return ""
        }
    }
    readonly property color activeTextMarkupColor: {
        switch (controller.tool) {
        case PDFEditController.HighlightTool:
            return controller.highlightColor
        case PDFEditController.UnderlineTool:
            return controller.underlineColor
        case PDFEditController.SquigglyTool:
            return controller.squigglyColor
        case PDFEditController.StrikeoutTool:
            return controller.strikeoutColor
        default:
            return "transparent"
        }
    }
    readonly property rect activeShapeRect: Qt.rect(
        Math.min(shapeStart.x, shapeCurrent.x),
        Math.min(shapeStart.y, shapeCurrent.y),
        Math.max(1, Math.abs(shapeCurrent.x - shapeStart.x)),
        Math.max(1, Math.abs(shapeCurrent.y - shapeStart.y)))
    readonly property bool freeTextDragIsSized:
        Math.abs(freeTextCurrent.x - freeTextStart.x)
            >= minimumFreeTextDragSize
        && Math.abs(freeTextCurrent.y - freeTextStart.y)
            >= minimumFreeTextDragSize
    readonly property rect activeFreeTextRect: {
        if (freeTextDragIsSized) {
            return Qt.rect(
                Math.min(freeTextStart.x, freeTextCurrent.x),
                Math.min(freeTextStart.y, freeTextCurrent.y),
                Math.abs(freeTextCurrent.x - freeTextStart.x),
                Math.abs(freeTextCurrent.y - freeTextStart.y))
        }

        const defaultWidth = Math.min(
            width,
            defaultFreeTextWidthPoints * pagePointScale)
        const defaultHeight = Math.min(
            height,
            defaultFreeTextHeightPoints * pagePointScale)
        return Qt.rect(
            Math.max(0, Math.min(width - defaultWidth, freeTextStart.x)),
            Math.max(0, Math.min(height - defaultHeight, freeTextStart.y)),
            defaultWidth,
            defaultHeight)
    }
    readonly property bool stampDragIsSized:
        Math.abs(stampCurrent.x - stampStart.x)
            >= minimumStampDragSize
        && Math.abs(stampCurrent.y - stampStart.y)
            >= minimumStampDragSize
    readonly property rect activeStampRect: {
        if (stampDragIsSized) {
            return Qt.rect(
                Math.min(stampStart.x, stampCurrent.x),
                Math.min(stampStart.y, stampCurrent.y),
                Math.abs(stampCurrent.x - stampStart.x),
                Math.abs(stampCurrent.y - stampStart.y))
        }

        const defaultWidth = Math.min(
            width,
            defaultStampWidthPoints * pagePointScale)
        const defaultHeight = Math.min(
            height,
            defaultStampHeightPoints * pagePointScale)
        return Qt.rect(
            Math.max(0, Math.min(width - defaultWidth, stampStart.x)),
            Math.max(0, Math.min(height - defaultHeight, stampStart.y)),
            defaultWidth,
            defaultHeight)
    }
    readonly property bool interactionActive: !pooled
        && !controller.saving
        && controller.tool !== PDFEditController.BrowseTool
    readonly property bool editorUsesCleanTexture: draftTextureReady
        && hasActiveDraft(editingObjectId)
        && !(editingSourceTextMoveOnly
            && activeDraft(editingObjectId).sourceTextMoveOnly === true)
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
    signal deleteCommentRequested(int page, string objectId)
    signal pointerInteractionStarted()

    visible: !pooled && (interactionActive || drafts.length > 0)
    enabled: interactionActive

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Backspace
                && root.drawingPathActive) {
            root.removeLastDrawingPathPoint()
            event.accepted = true
        } else if (event.key === Qt.Key_Backspace
                   || event.key === Qt.Key_Delete) {
            event.accepted = root.deleteSelectedNonText()
        }
    }

    function reloadData() {
        if (pooled) {
            drafts = []
            contentDrafts = []
            highlightDrafts = []
            stickyNoteDrafts = []
            freeTextDrafts = []
            drawingCommentDrafts = []
            inkDrafts = []
            stampDrafts = []
            hoverCandidate = ({})
            frameResizePreview = ({})
            cancelHighlight()
            cancelShape()
            cancelFreeTextPlacement()
            cancelStampPlacement()
            cancelDrawingComment()
            cancelInkDrawing()
            hideSelectCursor()
            return
        }

        if (interactionActive)
            controller.requestCandidatesForPage(page)

        const previousDrafts = drafts
        const currentDrafts = controller.draftsForPage(page)
        const displayedDrafts = []
        for (let index = 0; index < currentDrafts.length; ++index) {
            const currentDraft = currentDrafts[index]
            displayedDrafts.push(currentDraft)
            const drawingDraft = currentDraft.kind === "ink"
                || isDrawingCommentKind(currentDraft.kind)
            if (drawingDraft
                    && currentDraft.added !== true
                    && displayedTextureSuppressesComment(
                        String(currentDraft.objectId))
                    && !drawingAppearanceSuppressed(currentDraft)) {
                displayedDrafts.push(restorationOverlay(currentDraft))
            }
        }

        for (let index = 0; index < previousDrafts.length; ++index) {
            const previousDraft = previousDrafts[index]
            const currentDraft = draftForObject(
                currentDrafts,
                previousDraft.objectId)
            const drawingDraft = previousDraft.kind === "ink"
                || isDrawingCommentKind(previousDraft.kind)
            if (drawingDraft) {
                if (!currentDraft
                        && previousDraft.added !== true
                        && displayedTextureSuppressesComment(
                            String(previousDraft.objectId))) {
                    displayedDrafts.push(
                        previousDraft.restoration === true
                            ? previousDraft
                            : restorationOverlay(previousDraft))
                }
                continue
            }

            if (draftTextureCurrent || currentDraft)
                continue
            if (previousDraft.added === true
                    || previousDraft.kind === "image") {
                continue
            }
            if (previousDraft.restoration === true)
                displayedDrafts.push(previousDraft)
            else if (previousDraft.originalText !== undefined
                    || previousDraft.originalPreview !== undefined)
                displayedDrafts.push(restorationOverlay(previousDraft))
        }
        const displayedContentDrafts = []
        const displayedHighlightDrafts = []
        const displayedStickyNoteDrafts = []
        const displayedFreeTextDrafts = []
        const displayedDrawingCommentDrafts = []
        const displayedInkDrafts = []
        const displayedStampDrafts = []
        for (let index = 0; index < displayedDrafts.length; ++index) {
            const draft = displayedDrafts[index]
            if (isTextMarkupKind(draft.kind))
                displayedHighlightDrafts.push(draft)
            else if (draft.kind === "stickyNote")
                displayedStickyNoteDrafts.push(draft)
            else if (draft.kind === "freeText")
                displayedFreeTextDrafts.push(draft)
            else if (draft.kind === "ink")
                displayedInkDrafts.push(draft)
            else if (draft.kind === "stamp")
                displayedStampDrafts.push(draft)
            else if (isDrawingCommentKind(draft.kind))
                displayedDrawingCommentDrafts.push(draft)
            else
                displayedContentDrafts.push(draft)
        }
        drafts = displayedDrafts
        contentDrafts = displayedContentDrafts
        highlightDrafts = displayedHighlightDrafts
        stickyNoteDrafts = displayedStickyNoteDrafts
        freeTextDrafts = displayedFreeTextDrafts
        drawingCommentDrafts = displayedDrawingCommentDrafts
        inkDrafts = displayedInkDrafts
        stampDrafts = displayedStampDrafts
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

    function draftForObject(items, objectId) {
        for (let index = 0; index < items.length; ++index) {
            if (items[index].objectId === objectId)
                return items[index]
        }
        return null
    }

    function drawingAppearanceSuppressed(draft) {
        return draft !== null
            && (draft.kind === "ink" || isDrawingCommentKind(draft.kind))
            && (draft.deleted === true
                || draft.geometryChanged === true
                || draft.styleChanged === true)
    }

    function displayedTextureSuppressesComment(objectId) {
        if (objectId.length === 0
                || displayedSuppressedCommentObjectIds === undefined
                || displayedSuppressedCommentObjectIds === null) {
            return false
        }
        for (let index = 0;
             index < displayedSuppressedCommentObjectIds.length;
             ++index) {
            if (String(displayedSuppressedCommentObjectIds[index])
                    === objectId) {
                return true
            }
        }
        return false
    }

    function drawingPreviewCanRender(draft) {
        return draft.added === true
            || displayedTextureSuppressesComment(String(draft.objectId))
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

    function activeDraft(objectId) {
        if (objectId.length === 0)
            return ({})

        for (let index = 0; index < drafts.length; ++index) {
            if (drafts[index].objectId === objectId
                    && drafts[index].restoration !== true) {
                return drafts[index]
            }
        }
        return ({})
    }

    function restorationOverlay(draft) {
        if (draft.originalPreview !== undefined) {
            const restored = ({})
            const original = draft.originalPreview
            for (const key in original)
                restored[key] = original[key]
            restored.restoration = true
            // Keep the source appearance covered until the refreshed PDF
            // texture containing its original appearance is ready.
            restored.geometryChanged = true
            restored.styleChanged = true
            return restored
        }

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
            letterSpacingRatio: primaryFormat.letterSpacingRatio,
            wordSpacingRatio: primaryFormat.wordSpacingRatio,
            sourceAdvanceRatio: primaryFormat.sourceAdvanceRatio,
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
        const displayRatio = Number(format.displayFontSizeRatio) > 0
            ? Number(format.displayFontSizeRatio)
            : Number(format.fontSizeRatio)
        const relativeSize = displayRatio > 0 && baseRatio > 0
            ? displayRatio / baseRatio
            : 1
        const fontBold = format.fontBold === true
        const fontItalic = format.fontItalic === true
        const fontUnderline = format.fontUnderline === true
        const effectiveBasePixelSize = Number(basePixelSize) > 0
            ? Number(basePixelSize)
            : Math.max(1, baseRatio * root.height)
        const displayPixelSize = Math.max(
            1,
            Math.round(relativeSize * effectiveBasePixelSize))
        const displayFont = controller.fontForDisplay(
            rawFamily,
            Number(format.fontStretch || 100),
            fontBold,
            fontItalic,
            fontUnderline,
            displayPixelSize,
            (Number(format.letterSpacingRatio || 0) + Number(format.sourceAdvanceRatio || 0)) * effectiveBasePixelSize / baseRatio,
            Number(format.wordSpacingRatio || 0) * effectiveBasePixelSize / baseRatio,
            Math.abs(Number(format.sourceAdvanceRatio || 0)) > 1e-12)
        const family = String(displayFont.family || rawFamily)
            .replace(/\\/g, "\\\\")
            .replace(/'/g, "\\'")
        const displayStretch = controller.displayFontStretch(
            rawFamily,
            Number(format.fontStretch || 100),
            fontBold,
            fontItalic,
            displayPixelSize)
        let style = "font-size:" + relativeSize.toFixed(6) + "em;"
            + "font-kerning:" + (Math.abs(Number(format.sourceAdvanceRatio || 0)) > 1e-12 ? "none" : "normal") + ";"
            + "font-stretch:" + displayStretch + "%;"
            + "letter-spacing:"
            + ((Number(format.letterSpacingRatio || 0) + Number(format.sourceAdvanceRatio || 0)) * effectiveBasePixelSize / baseRatio).toFixed(6) + "px;"
            + "word-spacing:"
            + (Number(format.wordSpacingRatio || 0) * effectiveBasePixelSize / baseRatio).toFixed(6) + "px;"
            + "font-weight:" + (fontBold ? "700" : "400") + ";"
            + "font-style:" + (fontItalic ? "italic" : "normal") + ";"
            + "text-decoration:"
            + (fontUnderline ? "underline" : "none") + ";"
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
            const layoutHeight = Number(basePixelSize) > 0
                ? Number(basePixelSize) / baseRatio : root.height
            const lineSpacing = Math.max(0.5, Math.min(3,
                Number(paragraph.lineSpacing) || 1))
            const before = Math.max(0, Number(paragraph.spaceBeforeRatio) || 0)
                * layoutHeight
            const after = Math.max(0, Number(paragraph.spaceAfterRatio) || 0)
                * layoutHeight
            const firstLineIndent = Math.max(0, Number(paragraph.firstLineIndentRatio) || 0)
                * layoutHeight
            const paragraphStyle = "margin:0;white-space:pre-wrap;"
                + (start === end ? "-qt-paragraph-type:empty;" : "")
                + "line-height:" + (lineSpacing * 100).toFixed(6) + "%;"
                + "margin-top:" + before.toFixed(6) + "px;"
                + "margin-bottom:" + after.toFixed(6) + "px;"
                + "text-indent:" + firstLineIndent.toFixed(6) + "px;"
            // Qt Rich Text supports block alignment through the align attribute.
            html += "<p align=\"" + escapeHtmlAttribute(alignment)
                + "\" style=\"" + escapeHtmlAttribute(paragraphStyle) + "\">"
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
            letterSpacingRatio: Number(controller.selectedLetterSpacing || 0) / root.textLayoutPageHeight,
            wordSpacingRatio: Number(controller.selectedWordSpacing || 0) / root.textLayoutPageHeight,
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
        if (!editing || loadingEditorText
                || editorPlainText() !== controller.selectedText)
            return

        const start = Math.min(editor.selectionStart, editor.selectionEnd)
        const end = Math.max(editor.selectionStart, editor.selectionEnd)
        controller.setSelectedTextRange(start, end - start)
    }

    function syncEditorTextPreview() {
        if (!editing || loadingEditorText)
            return

        syncingEditorPreview = true
        controller.normalizeEditorSourceSpacing(editor.textDocument, root.textLayoutPageHeight)
        controller.updateSelectedTextPreview(
            editorPlainText(), editor.cursorPosition)
        syncingEditorPreview = false
        syncEditorSelection()
        syncEditorDocumentFontStyles()
        Qt.callLater(root.syncEditorContentBounds)
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
        controller.initializeEditorFontSizes(editor.textDocument, root.textLayoutPageHeight)
        controller.initializeEditorParagraphFormats(editor.textDocument, root.textLayoutPageHeight)
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
                || candidate.kind === "addedText")
    }

    function isImageKind(kind) {
        return kind === "image" || kind === "addedImage"
    }

    function isTextMarkupKind(kind) {
        return kind === "highlight"
            || kind === "underline"
            || kind === "squiggly"
            || kind === "strikeout"
    }

    function isDrawingCommentKind(kind) {
        return kind === "square"
            || kind === "circle"
            || kind === "line"
            || kind === "polygon"
            || kind === "polyLine"
    }

    function isCommentKind(kind) {
        return isTextMarkupKind(kind)
            || isDrawingCommentKind(kind)
            || kind === "comment"
            || kind === "stickyNote"
            || kind === "freeText"
            || kind === "ink"
            || kind === "stamp"
    }

    function isSelectionTool() {
        return controller.tool === PDFEditController.SelectTool
            || controller.tool === PDFEditController.CommentSelectTool
    }

    function candidateMatchesSelectionTool(candidate) {
        if (controller.headerSelectionOnly
                && (!hasCandidate(candidate)
                    || !controller.candidateAllowedInSelection(page, candidate.objectId)))
            return false
        return controller.tool !== PDFEditController.CommentSelectTool
            || (hasCandidate(candidate) && isCommentKind(candidate.kind))
    }

    function isGeometryCommentKind(kind) {
        return isTextMarkupKind(kind)
            || isDrawingCommentKind(kind)
            || kind === "stickyNote"
            || kind === "freeText"
            || kind === "ink"
            || kind === "stamp"
    }

    function resizeHandleHitExtent(overlayExtent) {
        return Math.min(
            resizeHandleHitSize,
            Math.max(2, (Number(overlayExtent) - 8) / 2))
    }

    function resizeCursorForHandle(horizontal, vertical) {
        const rotation = ((viewRotation % 360) + 360) % 360
        const quarterTurn = rotation === 90 || rotation === 270
        if (horizontal === 0)
            return quarterTurn ? Qt.SizeHorCursor : Qt.SizeVerCursor
        if (vertical === 0)
            return quarterTurn ? Qt.SizeVerCursor : Qt.SizeHorCursor

        const forwardDiagonal = horizontal === vertical
        return forwardDiagonal !== quarterTurn
            ? Qt.SizeFDiagCursor
            : Qt.SizeBDiagCursor
    }

    function directionalResizeAccessibilityName(baseName,
                                                  horizontal,
                                                  vertical) {
        if (horizontal < 0 && vertical < 0) {
            //% "%1 from top-left"
            const pattern = qsTrId("ResizeFromTopLeft")
            return formattedResizeAccessibilityName(pattern, baseName)
        }
        if (horizontal === 0 && vertical < 0) {
            //% "%1 from top"
            const pattern = qsTrId("ResizeFromTop")
            return formattedResizeAccessibilityName(pattern, baseName)
        }
        if (horizontal > 0 && vertical < 0) {
            //% "%1 from top-right"
            const pattern = qsTrId("ResizeFromTopRight")
            return formattedResizeAccessibilityName(pattern, baseName)
        }
        if (horizontal > 0 && vertical === 0) {
            //% "%1 from right"
            const pattern = qsTrId("ResizeFromRight")
            return formattedResizeAccessibilityName(pattern, baseName)
        }
        if (horizontal > 0 && vertical > 0) {
            //% "%1 from bottom-right"
            const pattern = qsTrId("ResizeFromBottomRight")
            return formattedResizeAccessibilityName(pattern, baseName)
        }
        if (horizontal === 0 && vertical > 0) {
            //% "%1 from bottom"
            const pattern = qsTrId("ResizeFromBottom")
            return formattedResizeAccessibilityName(pattern, baseName)
        }
        if (horizontal < 0 && vertical > 0) {
            //% "%1 from bottom-left"
            const pattern = qsTrId("ResizeFromBottomLeft")
            return formattedResizeAccessibilityName(pattern, baseName)
        }
        if (horizontal < 0 && vertical === 0) {
            //% "%1 from left"
            const pattern = qsTrId("ResizeFromLeft")
            return formattedResizeAccessibilityName(pattern, baseName)
        }
        return baseName
    }

    function formattedResizeAccessibilityName(pattern, baseName) {
        return pattern.indexOf("%1") >= 0
            ? pattern.arg(baseName)
            : pattern
    }

    function isTextMarkupTool() {
        return controller.tool === PDFEditController.HighlightTool
            || controller.tool === PDFEditController.UnderlineTool
            || controller.tool === PDFEditController.SquigglyTool
            || controller.tool === PDFEditController.StrikeoutTool
    }

    function isDrawingCommentTool() {
        return controller.tool === PDFEditController.SquareTool
            || controller.tool === PDFEditController.CircleTool
            || controller.tool === PDFEditController.LineTool
            || controller.tool === PDFEditController.PolygonTool
            || controller.tool === PDFEditController.PolyLineTool
    }

    function isInkTool() {
        return controller.tool === PDFEditController.InkTool
    }

    function isStampTool() {
        return controller.tool === PDFEditController.StampTool
    }

    function isDrawingPathTool() {
        return controller.tool === PDFEditController.PolygonTool
            || controller.tool === PDFEditController.PolyLineTool
    }

    function drawingKindForTool() {
        if (controller.tool === PDFEditController.CircleTool)
            return "circle"
        if (controller.tool === PDFEditController.LineTool)
            return "line"
        if (controller.tool === PDFEditController.PolygonTool)
            return "polygon"
        if (controller.tool === PDFEditController.PolyLineTool)
            return "polyLine"
        return "square"
    }

    function isCommentCreationTool() {
        return isTextMarkupTool()
            || isDrawingCommentTool()
            || isInkTool()
            || isStampTool()
            || controller.tool === PDFEditController.StickyNoteTool
            || controller.tool === PDFEditController.FreeTextTool
    }

    function isShapeKind(kind) {
        return kind === "addedShape" || kind === "sourceShape"
    }

    function isMovableKind(kind) {
        return isImageKind(kind)
            || isShapeKind(kind)
            || isGeometryCommentKind(kind)
            || kind === "text"
            || kind === "addedText"
    }

    function isImageCandidate(candidate) {
        return hasCandidate(candidate) && isImageKind(candidate.kind)
    }

    function isShapeCandidate(candidate) {
        return hasCandidate(candidate) && isShapeKind(candidate.kind)
    }

    function shapeDraft(objectId) {
        for (let index = 0; index < contentDrafts.length; ++index) {
            if (contentDrafts[index].objectId === objectId)
                return contentDrafts[index]
        }
        if (!pooled
                && controller.selectedPage === page
                && controller.selectedKind === "sourceShape") {
            const candidates = controller.candidatesForPage(page)
            for (let index = 0; index < candidates.length; ++index) {
                if (candidates[index].objectId === objectId)
                    return candidates[index]
            }
        }
        return ({})
    }

    function distanceToSegment(point, start, end) {
        const deltaX = end.x - start.x
        const deltaY = end.y - start.y
        const lengthSquared = deltaX * deltaX + deltaY * deltaY
        if (lengthSquared <= 0)
            return Math.hypot(point.x - start.x, point.y - start.y)

        const projection = Math.max(
            0,
            Math.min(
                1,
                ((point.x - start.x) * deltaX
                    + (point.y - start.y) * deltaY)
                    / lengthSquared))
        return Math.hypot(
            point.x - (start.x + projection * deltaX),
            point.y - (start.y + projection * deltaY))
    }

    function polygonContainsPoint(points, point, tolerance) {
        let inside = false
        for (let index = 0, previous = points.length - 1;
                index < points.length;
                previous = index++) {
            if (distanceToSegment(
                    point,
                    points[previous],
                    points[index]) <= tolerance) {
                return true
            }

            const current = points[index]
            const prior = points[previous]
            if ((current.y > point.y) !== (prior.y > point.y)
                    && point.x < (prior.x - current.x)
                        * (point.y - current.y)
                        / (prior.y - current.y)
                        + current.x) {
                inside = !inside
            }
        }
        return inside
    }

    function shapeContainsPoint(candidate, point, shapeWidth, shapeHeight) {
        if (!isShapeCandidate(candidate)
                || shapeWidth <= 0
                || shapeHeight <= 0) {
            return false
        }

        const strokeRatio = candidate.shapeStrokeWidthRatio !== undefined
            ? Number(candidate.shapeStrokeWidthRatio)
            : 0
        const strokeWidth = Math.max(1.25, strokeRatio * root.height)
        const inset = Math.min(
            strokeWidth / 2,
            shapeWidth / 4,
            shapeHeight / 4)
        const left = inset
        const top = inset
        const right = shapeWidth - inset
        const bottom = shapeHeight - inset
        const centerX = (left + right) / 2
        const centerY = (top + bottom) / 2
        const tolerance = root.controller.shapeHitToleranceRatio(
            root.page,
            strokeRatio) * root.height
        const boundedRatio = value => Math.max(
            0,
            Math.min(1, Number(value)))
        const lineRatioPoint = ratio => Qt.point(
            boundedRatio(ratio.x) * shapeWidth,
            boundedRatio(ratio.y) * shapeHeight)
        const startRatio = candidate.shapeStart !== undefined
            ? candidate.shapeStart
            : Qt.point(0, 0)
        const endRatio = candidate.shapeEnd !== undefined
            ? candidate.shapeEnd
            : Qt.point(1, 1)

        // Match controller hover/selection: closed interiors also accept
        // dragging when the shape has no fill or is fully transparent.
        switch (Number(candidate.shapeType)) {
        case PDFEditController.RectangleShape:
            return polygonContainsPoint([
                Qt.point(left, top),
                Qt.point(right, top),
                Qt.point(right, bottom),
                Qt.point(left, bottom)
            ], point, tolerance)
        case PDFEditController.EllipseShape: {
            const radiusX = (right - left) / 2
            const radiusY = (bottom - top) / 2
            const deltaX = point.x - centerX
            const deltaY = point.y - centerY
            const outerX = deltaX / (radiusX + tolerance)
            const outerY = deltaY / (radiusY + tolerance)
            return outerX * outerX + outerY * outerY <= 1
        }
        case PDFEditController.LineShape:
            return distanceToSegment(
                point,
                lineRatioPoint(startRatio),
                lineRatioPoint(endRatio)) <= tolerance
        case PDFEditController.ArrowShape: {
            const start = lineRatioPoint(startRatio)
            const end = lineRatioPoint(endRatio)
            if (distanceToSegment(point, start, end) <= tolerance)
                return true

            const deltaX = end.x - start.x
            const deltaY = end.y - start.y
            const length = Math.hypot(deltaX, deltaY)
            if (length <= 0)
                return false

            const unitX = deltaX / length
            const unitY = deltaY / length
            const headLength = Math.min(
                Math.max(6, strokeWidth * 4),
                length * 0.35)
            const halfWidth = headLength * 0.55
            const baseX = end.x - unitX * headLength
            const baseY = end.y - unitY * headLength
            const headLeft = Qt.point(
                baseX - unitY * halfWidth,
                baseY + unitX * halfWidth)
            const headRight = Qt.point(
                baseX + unitY * halfWidth,
                baseY - unitX * halfWidth)
            return distanceToSegment(point, headLeft, end) <= tolerance
                || distanceToSegment(point, end, headRight) <= tolerance
        }
        case PDFEditController.TriangleShape:
            return polygonContainsPoint([
                Qt.point(centerX, top),
                Qt.point(right, bottom),
                Qt.point(left, bottom)
            ], point, tolerance)
        case PDFEditController.DiamondShape:
            return polygonContainsPoint([
                Qt.point(centerX, top),
                Qt.point(right, centerY),
                Qt.point(centerX, bottom),
                Qt.point(left, centerY)
            ], point, tolerance)
        }
        return false
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
        if (isImageKind(controller.selectedKind)
                && controller.imageAspectRatioLocked) {
            return aspectLockedImageFrameRect(
                startFrameRect, horizontal, vertical, deltaX, deltaY,
                pageWidth, pageHeight, minimumWidth, minimumHeight)
        }

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
                selectionBottomRatio,
                Math.max(top + minimumHeight, bottom + deltaY / pageHeight))
        }

        return Qt.rect(left, top, right - left, bottom - top)
    }

    function aspectLockedImageFrameRect(startFrameRect, horizontal, vertical,
                                        deltaX, deltaY, pageWidth, pageHeight,
                                        minimumWidth, minimumHeight) {
        const frameWidth = Number(startFrameRect.width)
        const frameHeight = Number(startFrameRect.height)
        if (frameWidth <= 0 || frameHeight <= 0)
            return startFrameRect

        const left = Number(startFrameRect.x)
        const top = Number(startFrameRect.y)
        const right = left + frameWidth
        const bottom = top + frameHeight
        const centerX = left + frameWidth / 2
        const centerY = top + frameHeight / 2
        const pixelWidth = frameWidth * pageWidth
        const pixelHeight = frameHeight * pageHeight
        let requestedScale = 1
        if (horizontal === 0) {
            requestedScale += vertical * deltaY / pixelHeight
        } else if (vertical === 0) {
            requestedScale += horizontal * deltaX / pixelWidth
        } else {
            // Project the pointer onto the diagonal through the fixed corner.
            requestedScale += (horizontal * deltaX * pixelWidth
                + vertical * deltaY * pixelHeight)
                / (pixelWidth * pixelWidth + pixelHeight * pixelHeight)
        }

        const availableWidth = horizontal < 0
            ? right
            : horizontal > 0 ? 1 - left : 2 * Math.min(centerX, 1 - centerX)
        const availableHeight = vertical < 0
            ? bottom
            : vertical > 0 ? selectionBottomRatio - top
                : 2 * Math.min(centerY, selectionBottomRatio - centerY)
        const maximumScale = Math.min(
            availableWidth / frameWidth, availableHeight / frameHeight)
        if (maximumScale <= 0)
            return startFrameRect

        const minimumScale = Math.max(
            minimumWidth / frameWidth, minimumHeight / frameHeight)
        const scale = Math.min(
            maximumScale, Math.max(minimumScale, requestedScale))
        const width = frameWidth * scale
        const height = frameHeight * scale
        return Qt.rect(
            horizontal < 0 ? right - width
                : horizontal > 0 ? left : centerX - width / 2,
            vertical < 0 ? bottom - height
                : vertical > 0 ? top : centerY - height / 2,
            width, height)
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
                selectionBottomRatio - frameHeight,
                Number(startFrameRect.y) + deltaY / pageHeight))
        return Qt.rect(left, top, frameWidth, frameHeight)
    }

    function normalizedPointsForFrame(points, frameRect) {
        const frameWidth = Number(frameRect.width)
        const frameHeight = Number(frameRect.height)
        if (frameWidth <= 0 || frameHeight <= 0)
            return []

        const normalizedPoints = []
        for (let index = 0; index < points.length; ++index) {
            const point = points[index]
            normalizedPoints.push(Qt.point(
                (Number(point.x) - Number(frameRect.x)) / frameWidth,
                (Number(point.y) - Number(frameRect.y)) / frameHeight))
        }
        return normalizedPoints
    }

    function normalizedStrokesForFrame(strokes, frameRect) {
        const normalizedStrokes = []
        for (let index = 0; index < strokes.length; ++index) {
            normalizedStrokes.push(normalizedPointsForFrame(
                strokes[index],
                frameRect))
        }
        return normalizedStrokes
    }

    function normalizedRectsForFrame(rects, frameRect) {
        const frameWidth = Number(frameRect.width)
        const frameHeight = Number(frameRect.height)
        if (frameWidth <= 0 || frameHeight <= 0)
            return []

        const normalizedRects = []
        for (let index = 0; index < rects.length; ++index) {
            const rect = rects[index]
            normalizedRects.push(Qt.rect(
                (Number(rect.x) - Number(frameRect.x)) / frameWidth,
                (Number(rect.y) - Number(frameRect.y)) / frameHeight,
                Number(rect.width) / frameWidth,
                Number(rect.height) / frameHeight))
        }
        return normalizedRects
    }

    function startFrameResize() {
        controller.beginSelectedFrameResize()
        resizingFrame = true
        resizingObjectId = controller.selectedObjectId
        const selectedKind = controller.selectedKind
        frameResizePreview = isGeometryCommentKind(selectedKind)
            ? controller.selectedDraftPreview()
            : ({})
    }

    function finishFrameResize() {
        if (!resizingFrame)
            return

        controller.finishSelectedFrameResize()
        resizingFrame = false
        resizingObjectId = ""
        frameResizePreview = ({})
        reloadData()
    }

    function cancelFrameResize() {
        if (!resizingFrame)
            return

        controller.cancelSelectedFrameResize()
        resizingFrame = false
        resizingObjectId = ""
        frameResizePreview = ({})
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
                || controller.tool === PDFEditController.AddShapeTool
                || isCommentCreationTool()
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

    function beginShape(x, y) {
        if (controller.tool !== PDFEditController.AddShapeTool
                || width <= 0
                || height <= 0) {
            return
        }

        if (editing)
            commitEditing()
        selectionActions.close()
        controller.clearSelection()
        hoverCandidate = ({})
        hideSelectCursor()
        shapeStart = Qt.point(
            Math.max(0, Math.min(width, x)),
            Math.max(0, Math.min(height, y)))
        shapeCurrent = shapeStart
        drawingShape = true
    }

    function updateShape(x, y) {
        if (!drawingShape)
            return

        const boundedCurrent = Qt.point(
            Math.max(0, Math.min(width, x)),
            Math.max(0, Math.min(height, y)))
        const constrainedEnd = controller.constrainedShapeEnd(
            page,
            Qt.point(shapeStart.x / width, shapeStart.y / height),
            Qt.point(
                boundedCurrent.x / width,
                boundedCurrent.y / height))
        shapeCurrent = Qt.point(
            constrainedEnd.x * width,
            constrainedEnd.y * height)
    }

    function finishShape(x, y) {
        if (!drawingShape)
            return

        updateShape(x, y)
        const normalizedStart = Qt.point(
            shapeStart.x / width,
            shapeStart.y / height)
        const normalizedEnd = Qt.point(
            shapeCurrent.x / width,
            shapeCurrent.y / height)
        drawingShape = false
        if (!controller.shapeDragIsValid(
                page,
                normalizedStart,
                normalizedEnd)) {
            return
        }

        controller.addShape(
            page,
            normalizedStart,
            normalizedEnd)
        reloadData()
    }

    function cancelShape() {
        drawingShape = false
    }

    function beginFreeTextPlacement(x, y) {
        if (controller.tool !== PDFEditController.FreeTextTool
                || width <= 0
                || height <= 0) {
            return
        }

        if (editing)
            commitEditing()
        selectionActions.close()
        controller.clearSelection()
        hoverCandidate = ({})
        hideSelectCursor()
        freeTextStart = Qt.point(
            Math.max(0, Math.min(width, x)),
            Math.max(0, Math.min(height, y)))
        freeTextCurrent = freeTextStart
        placingFreeText = true
    }

    function updateFreeTextPlacement(x, y) {
        if (!placingFreeText)
            return

        freeTextCurrent = Qt.point(
            Math.max(0, Math.min(width, x)),
            Math.max(0, Math.min(height, y)))
    }

    function finishFreeTextPlacement(x, y) {
        if (!placingFreeText)
            return

        updateFreeTextPlacement(x, y)
        const placementRect = activeFreeTextRect
        placingFreeText = false
        suppressNextPlacementClick = true
        Qt.callLater(function() {
            root.suppressNextPlacementClick = false
        })
        if (placementRect.width <= 0 || placementRect.height <= 0)
            return

        controller.addFreeText(page, Qt.rect(
            placementRect.x / width,
            placementRect.y / height,
            placementRect.width / width,
            placementRect.height / height))
        reloadData()
    }

    function cancelFreeTextPlacement() {
        placingFreeText = false
    }

    function beginStampPlacement(x, y) {
        if (!isStampTool() || width <= 0 || height <= 0)
            return

        if (editing)
            commitEditing()
        selectionActions.close()
        controller.clearSelection()
        hoverCandidate = ({})
        hideSelectCursor()
        stampStart = Qt.point(
            Math.max(0, Math.min(width, x)),
            Math.max(0, Math.min(height, y)))
        stampCurrent = stampStart
        placingStamp = true
    }

    function updateStampPlacement(x, y) {
        if (!placingStamp)
            return

        stampCurrent = Qt.point(
            Math.max(0, Math.min(width, x)),
            Math.max(0, Math.min(height, y)))
    }

    function finishStampPlacement(x, y) {
        if (!placingStamp)
            return

        updateStampPlacement(x, y)
        const placementRect = activeStampRect
        placingStamp = false
        suppressNextPlacementClick = true
        Qt.callLater(function() {
            root.suppressNextPlacementClick = false
        })
        if (placementRect.width <= 0 || placementRect.height <= 0)
            return

        controller.addStamp(page, Qt.rect(
            placementRect.x / width,
            placementRect.y / height,
            placementRect.width / width,
            placementRect.height / height))
        reloadData()
    }

    function cancelStampPlacement() {
        placingStamp = false
    }

    function resolveTextMarkupPreview() {
        textMarkupPreviewUpdatePending = false
        if (!highlighting || !isTextMarkupTool() || width <= 0
                || height <= 0) {
            textMarkupPreviewRects = []
            return
        }

        const selectionRect = activeTextMarkupSelectionRect
        textMarkupPreviewRects = controller.textMarkupRectsForSelection(
            page,
            Qt.rect(
                selectionRect.x / width,
                selectionRect.y / height,
                selectionRect.width / width,
                selectionRect.height / height))
    }

    function scheduleTextMarkupPreviewUpdate() {
        if (!highlighting || !isTextMarkupTool()) {
            textMarkupPreviewUpdatePending = false
            if (textMarkupPreviewRects.length > 0)
                textMarkupPreviewRects = []
            return
        }

        textMarkupPreviewUpdatePending = true
    }

    function beginHighlight(x, y) {
        if (!isTextMarkupTool()
                || width <= 0
                || height <= 0) {
            return
        }

        if (editing)
            commitEditing()
        selectionActions.close()
        controller.clearSelection()
        hoverCandidate = ({})
        hideSelectCursor()
        highlightStart = Qt.point(
            Math.max(0, Math.min(width, x)),
            Math.max(0, Math.min(height, y)))
        highlightCurrent = highlightStart
        highlighting = true
        scheduleTextMarkupPreviewUpdate()
    }

    function updateHighlight(x, y) {
        if (!highlighting)
            return

        highlightCurrent = Qt.point(
            Math.max(0, Math.min(width, x)),
            Math.max(0, Math.min(height, y)))
        scheduleTextMarkupPreviewUpdate()
    }

    function finishHighlight(x, y) {
        if (!highlighting)
            return

        updateHighlight(x, y)
        const selectionRect = activeTextMarkupSelectionRect
        highlighting = false
        textMarkupPreviewUpdatePending = false
        textMarkupPreviewRects = []
        if (selectionRect.width < minimumTextMarkupDragSize)
            return

        controller.addHighlight(page, Qt.rect(
            selectionRect.x / width,
            selectionRect.y / height,
            selectionRect.width / width,
            selectionRect.height / height))
        reloadData()
    }

    function cancelHighlight() {
        highlighting = false
        textMarkupPreviewUpdatePending = false
        textMarkupPreviewRects = []
    }

    function boundedDrawingPoint(x, y) {
        return Qt.point(
            Math.max(0, Math.min(width, x)),
            Math.max(0, Math.min(height, y)))
    }

    function normalizedDrawingPoint(x, y) {
        const point = boundedDrawingPoint(x, y)
        return Qt.point(point.x / width, point.y / height)
    }

    function beginDrawingComment(x, y) {
        if (!isDrawingCommentTool()
                || isDrawingPathTool()
                || width <= 0
                || height <= 0) {
            return
        }
        if (!controller.claimDrawingCommentInteraction(page))
            return

        if (editing)
            commitEditing()
        selectionActions.close()
        controller.clearSelection()
        hoverCandidate = ({})
        hideSelectCursor()
        drawingInteractionPage = page
        drawingCommentStart = boundedDrawingPoint(x, y)
        drawingCommentCurrent = drawingCommentStart
        drawingCommentDragging = true
    }

    function updateDrawingComment(x, y) {
        if (drawingCommentDragging)
            drawingCommentCurrent = boundedDrawingPoint(x, y)
        if (drawingPathActive)
            drawingPathHover = normalizedDrawingPoint(x, y)
    }

    function finishDrawingComment(x, y) {
        if (!drawingCommentDragging)
            return

        updateDrawingComment(x, y)
        const points = drawingPreviewVertices.slice(0)
        const targetPage = drawingInteractionPage
        drawingCommentDragging = false
        drawingInteractionPage = -1
        controller.releaseDrawingCommentInteraction(targetPage)
        controller.addDrawingComment(targetPage, points)
        reloadData()
    }

    function addDrawingPathPoint(x, y) {
        if (!isDrawingPathTool() || width <= 0 || height <= 0)
            return false
        if (!drawingPathActive) {
            if (!controller.claimDrawingCommentInteraction(page))
                return false
            drawingInteractionPage = page
        }

        if (editing)
            commitEditing()
        selectionActions.close()
        controller.clearSelection()
        hoverCandidate = ({})
        hideSelectCursor()

        const point = normalizedDrawingPoint(x, y)
        const points = drawingPathPoints.slice(0)
        if (points.length > 0) {
            const previous = points[points.length - 1]
            const distance = Math.hypot(
                (point.x - previous.x) * width,
                (point.y - previous.y) * height)
            if (distance < 2) {
                drawingPathHover = point
                return false
            }
        }
        points.push(point)
        drawingPathPoints = points
        drawingPathHover = point
        drawingPathActive = true
        return true
    }

    function finishDrawingPath() {
        if (!drawingPathActive)
            return

        const points = drawingPathPoints.slice(0)
        const minimumPoints = controller.tool
                === PDFEditController.PolygonTool ? 3 : 2
        if (points.length < minimumPoints)
            return

        const targetPage = drawingInteractionPage
        drawingPathActive = false
        drawingPathPoints = []
        drawingInteractionPage = -1
        controller.releaseDrawingCommentInteraction(targetPage)
        controller.addDrawingComment(targetPage, points)
        reloadData()
    }

    function removeLastDrawingPathPoint() {
        if (!drawingPathActive)
            return
        const points = drawingPathPoints.slice(0)
        points.pop()
        drawingPathPoints = points
        if (points.length === 0) {
            cancelDrawingComment()
        } else {
            drawingPathHover = points[points.length - 1]
        }
    }

    function cancelDrawingComment(releaseOwner) {
        const targetPage = drawingInteractionPage
        drawingCommentDragging = false
        drawingPathActive = false
        drawingPathPoints = []
        drawingPathHover = Qt.point(0, 0)
        drawingInteractionPage = -1
        if (releaseOwner !== false && targetPage >= 0)
            controller.releaseDrawingCommentInteraction(targetPage)
    }

    function beginInkDrawing(x, y) {
        if (!isInkTool() || width <= 0 || height <= 0 || inkDrawing)
            return
        if (!controller.claimDrawingCommentInteraction(page))
            return

        if (editing)
            commitEditing()
        selectionActions.close()
        controller.clearSelection()
        hoverCandidate = ({})
        hideSelectCursor()
        inkInteractionPage = page
        const point = normalizedDrawingPoint(x, y)
        inkStrokePoints = [point]
        inkPreviewStrokePoints = [point]
        inkPreviewUpdatePending = false
        inkDrawing = true
    }

    function updateInkDrawing(x, y) {
        if (!inkDrawing || width <= 0 || height <= 0) {
            return
        }

        const point = normalizedDrawingPoint(x, y)
        const previous = inkStrokePoints[inkStrokePoints.length - 1]
        const distance = Math.hypot(
            (point.x - previous.x) * width,
            (point.y - previous.y) * height)
        if (distance < minimumInkSampleDistance)
            return

        if (inkStrokePoints.length >= maximumInkPoints)
            inkStrokePoints[inkStrokePoints.length - 1] = point
        else
            inkStrokePoints.push(point)
        inkPreviewUpdatePending = true
    }

    function publishInkPreview() {
        inkPreviewUpdatePending = false
        if (!inkDrawing) {
            inkPreviewStrokePoints = []
            return
        }

        inkPreviewStrokePoints = inkStrokePoints.slice(0)
    }

    function finishInkDrawing(x, y) {
        if (!inkDrawing)
            return

        updateInkDrawing(x, y)
        const points = inkStrokePoints
        const targetPage = inkInteractionPage
        inkDrawing = false
        inkInteractionPage = -1
        inkStrokePoints = []
        inkPreviewStrokePoints = []
        inkPreviewUpdatePending = false
        controller.releaseDrawingCommentInteraction(targetPage)
        if (points.length >= 2)
            controller.addDrawingComment(targetPage, points)
        reloadData()
    }

    function cancelInkDrawing(releaseOwner) {
        const targetPage = inkInteractionPage
        inkDrawing = false
        inkInteractionPage = -1
        inkStrokePoints = []
        inkPreviewStrokePoints = []
        inkPreviewUpdatePending = false
        if (releaseOwner !== false && targetPage >= 0)
            controller.releaseDrawingCommentInteraction(targetPage)
    }

    function addStickyNote(x, y) {
        if (controller.tool !== PDFEditController.StickyNoteTool
                || width <= 0
                || height <= 0) {
            return
        }

        if (editing)
            commitEditing()
        selectionActions.close()
        hoverCandidate = ({})
        hideSelectCursor()
        controller.addStickyNote(
            page,
            Qt.point(
                Math.max(0, Math.min(width, x)) / width,
                Math.max(0, Math.min(height, y)) / height))
        reloadData()
    }

    function selectCandidate(candidate) {
        if (editing && editingObjectId !== candidate.objectId)
            commitEditing()

        return controller.selectCandidate(page, candidate.objectId)
    }

    function showSelectionActions(x, y, candidate) {
        if (!isSelectionTool()
                || (!candidate.editable && !candidate.deletable)
                || !selectCandidate(candidate)) {
            selectionActions.close()
            return
        }

        selectionActionsObjectId = candidate.objectId
        hideSelectCursor()
        selectionActions.openAt(x, y)
    }

    function clearTextEditPreparation(cancelRequest) {
        const pendingController = textEditPreparationController
        const pendingPage = textEditPreparationPage
        const pendingObjectId = textEditPreparationObjectId
        textEditPreparationController = null
        textEditPreparationPage = -1
        textEditPreparationObjectId = ""
        if (cancelRequest && pendingController
                && pendingController.selectedPage === pendingPage
                && pendingController.selectedObjectId === pendingObjectId) {
            pendingController.cancelTextEditPreparation()
        }
    }

    function beginEditing(candidate) {
        beginEditingWithStyle(candidate, "")
    }

    function beginEditingWithStyle(candidate, styleAction) {
        if (pooled || !interactionActive
                || !isTextBoxCandidate(candidate)
                || !candidate.editable
                || (controller.tool !== PDFEditController.EditTextTool
                    && controller.tool !== PDFEditController.SelectTool)) {
            return
        }

        if (!controller.selectCandidate(page, candidate.objectId))
            return

        const action = String(styleAction || "")
        textEditPreparationController = controller
        textEditPreparationPage = page
        textEditPreparationObjectId = candidate.objectId
        if (controller.requestSelectedTextEdit(action))
            activatePreparedTextEdit(page, candidate.objectId, action)
    }

    function activatePreparedTextEdit(expectedPage, objectId, styleAction) {
        if (pooled || !interactionActive || editing
                || page !== expectedPage
                || controller.selectedPage !== expectedPage
                || controller.selectedObjectId !== objectId
                || !controller.selectedEditable
                || (controller.tool !== PDFEditController.EditTextTool
                    && controller.tool !== PDFEditController.SelectTool)) {
            return
        }

        clearTextEditPreparation(false)
        const currentDraft = activeDraft(objectId)
        editingSourceTextMoveOnly
            = currentDraft.sourceTextMoveOnly === true
        if (editingSourceTextMoveOnly
                && currentDraft.originalFrameRect !== undefined) {
            editingSourceTranslationX = currentDraft.frameRect.x
                - currentDraft.originalFrameRect.x
            editingSourceTranslationY = currentDraft.frameRect.y
                - currentDraft.originalFrameRect.y
        } else {
            editingSourceTranslationX = 0
            editingSourceTranslationY = 0
        }
        hideSelectCursor()
        controller.beginSelectedTextEdit()
        if (!controller.textEditActive)
            return
        editingObjectId = objectId
        editorLayoutDirty = false
        editing = true
        loadEditorRichText(-1, 0, 0)
        if (styleAction === "bold")
            controller.setSelectedFontBold(!controller.selectedFontBold)
        else if (styleAction === "italic")
            controller.setSelectedFontItalic(!controller.selectedFontItalic)
        else if (styleAction === "underline")
            controller.setSelectedFontUnderline(!controller.selectedFontUnderline)
        else if (styleAction.startsWith("fontFamily:"))
            controller.setSelectedFontFamily(styleAction.slice(11))
        const focusGeneration = ++editFocusGeneration
        const expectedController = controller
        Qt.callLater(function() {
            if (root.editFocusGeneration !== focusGeneration
                    || root.controller !== expectedController
                    || root.pooled || !root.interactionActive || !root.editing
                    || root.page !== expectedPage
                    || root.editingObjectId !== objectId
                    || !expectedController.textEditActive
                    || expectedController.selectedPage !== expectedPage
                    || expectedController.selectedObjectId !== objectId) {
                return
            }
            editor.forceActiveFocus()
        })
    }

    function commitEditing() {
        if (!editing)
            return

        ++editFocusGeneration

        if (resizingFrame) {
            controller.finishSelectedFrameResize()
            resizingFrame = false
            resizingObjectId = ""
            frameResizePreview = ({})
        }

        syncingEditorPreview = true
        controller.normalizeEditorSourceSpacing(editor.textDocument, root.textLayoutPageHeight)
        controller.updateSelectedTextPreview(
            editorPlainText(),
            editor.cursorPosition)
        controller.syncEditorFontStyles(editor.textDocument)
        syncingEditorPreview = false
        editing = false
        editingObjectId = ""
        editingSourceTextMoveOnly = false
        editingSourceTranslationX = 0
        editingSourceTranslationY = 0
        editorLayoutDirty = false
        controller.finishSelectedTextEdit()
        reloadData()
    }

    function cancelEditing() {
        ++editFocusGeneration
        clearTextEditPreparation(true)
        editing = false
        editingObjectId = ""
        editingSourceTextMoveOnly = false
        editingSourceTranslationX = 0
        editingSourceTranslationY = 0
        editorLayoutDirty = false
        cancelFrameResize()
        controller.cancelSelectedTextEdit()
    }

    function prepareForSessionChange() {
        ++editFocusGeneration
        clearTextEditPreparation(true)
        selectionActions.close()
        cancelHighlight()
        cancelShape()
        cancelFreeTextPlacement()
        cancelStampPlacement()
        cancelDrawingComment()
        cancelInkDrawing()
        hideSelectCursor()
        hoverCandidate = ({})
        if (editing) {
            commitEditing()
        } else if (resizingFrame) {
            controller.finishSelectedFrameResize()
            resizingFrame = false
            resizingObjectId = ""
            frameResizePreview = ({})
        }
    }

    function beginSelectedEditing() {
        beginSelectedEditingWithStyle("")
    }

    function beginSelectedEditingWithStyle(styleAction) {
        if (controller.selectedPage !== page || !controller.selectedEditable)
            return

        selectionActions.close()
        beginEditingWithStyle({
            objectId: controller.selectedObjectId,
            kind: controller.selectedKind,
            editable: controller.selectedEditable
        }, styleAction)
    }

    function deleteSelectedObject() {
        if (controller.selectedPage !== page || !controller.selectedDeletable)
            return false

        if (isCommentKind(controller.selectedKind)) {
            deleteCommentRequested(
                controller.selectedPage,
                controller.selectedObjectId)
            return true
        }

        controller.deleteSelectedObject()
        controller.clearSelection()
        hoverCandidate = ({})
        hideSelectCursor()
        reloadData()
        return true
    }

    function deleteSelectedNonText() {
        if (editing
                || (!isImageKind(controller.selectedKind)
                    && !isCommentKind(controller.selectedKind)
                    && !isShapeKind(controller.selectedKind))) {
            return false
        }

        selectionActions.close()
        return deleteSelectedObject()
    }

    function selectRelativeCandidate(direction) {
        if (!isSelectionTool()
                && controller.tool !== PDFEditController.EditTextTool) {
            return false
        }

        const candidates = controller.candidatesForPage(page)
        const selectable = []
        for (let index = 0; index < candidates.length; ++index) {
            if (candidateMatchesSelectionTool(candidates[index])
                    && (controller.tool !== PDFEditController.EditTextTool
                        || (candidates[index].editable
                            && isTextBoxCandidate(candidates[index])))) {
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
        ++editFocusGeneration
        clearTextEditPreparation(true)
        selectionActions.close()
        cancelHighlight()
        cancelShape()
        cancelFreeTextPlacement()
        cancelStampPlacement()
        cancelDrawingComment()
        cancelInkDrawing()
        cancelFrameResize()
        if (editing)
            commitEditing()
        reloadData()
    }

    onPooledChanged: {
        if (pooled) {
            ++editFocusGeneration
            clearTextEditPreparation(true)
        }
        selectionActions.close()
        cancelHighlight()
        cancelShape()
        cancelFreeTextPlacement()
        cancelStampPlacement()
        cancelDrawingComment()
        cancelInkDrawing()
        if (pooled)
            cancelFrameResize()
        if (pooled && editing)
            commitEditing()
        reloadData()
    }

    onInteractionActiveChanged: {
        if (!interactionActive) {
            ++editFocusGeneration
            clearTextEditPreparation(true)
            cancelHighlight()
            cancelShape()
            cancelFreeTextPlacement()
            cancelStampPlacement()
            cancelDrawingComment()
            cancelInkDrawing()
            cancelFrameResize()
        }
        reloadData()
    }
    onDraftTextureCurrentChanged: {
        if (draftTextureCurrent)
            reloadData()
    }
    onDisplayedSuppressedCommentObjectIdsChanged: reloadData()
    Component.onCompleted: reloadData()
    Component.onDestruction: clearTextEditPreparation(true)

    FrameAnimation {
        running: root.textMarkupPreviewUpdatePending

        // Pointer events can outpace frames; resolve from the latest position.
        onTriggered: root.resolveTextMarkupPreview()
    }

    FrameAnimation {
        running: root.inkPreviewUpdatePending

        // Pointer events can outpace frames; publish the latest captured stroke.
        onTriggered: root.publishInkPreview()
    }

    Connections {
        target: root.controller

        function onTextEditPreparationReady(readyPage, objectId, styleAction) {
            if (root.textEditPreparationController !== root.controller
                    || root.textEditPreparationPage !== readyPage
                    || root.textEditPreparationObjectId !== objectId) {
                return
            }
            root.activatePreparedTextEdit(readyPage, objectId, styleAction)
        }

        function onPageDataChanged(changedPage) {
            if (changedPage < 0 || changedPage === root.page)
                root.reloadData()
        }

        function onPageCandidatesReady(readyPage) {
            if (readyPage === root.page
                    && root.highlighting
                    && root.isTextMarkupTool()) {
                root.scheduleTextMarkupPreviewUpdate()
            }
            if (readyPage === root.page
                    && root.interactionActive
                    && hitArea.containsMouse) {
                root.updateHoverAtPosition(hitArea.mouseX, hitArea.mouseY)
            }
        }

        function onHeaderSelectionOnlyChanged() {
            root.prepareForSessionChange()
            root.reloadData()
        }

        function onToolChanged() {
            ++root.editFocusGeneration
            root.clearTextEditPreparation(true)
            selectionActions.close()
            root.cancelHighlight()
            root.cancelShape()
            root.cancelFreeTextPlacement()
            root.cancelStampPlacement()
            root.cancelDrawingComment()
            root.cancelInkDrawing()
            root.cancelFrameResize()
            if (root.editing)
                root.commitEditing()
            root.hoverCandidate = ({})
            root.hideSelectCursor()
            root.reloadData()
        }

        function onPendingDrawingCommentPageChanged() {
            if (root.drawingInteractionPage >= 0
                    && root.controller.pendingDrawingCommentPage
                        !== root.drawingInteractionPage) {
                root.cancelDrawingComment(false)
            }
            if (root.inkInteractionPage >= 0
                    && root.controller.pendingDrawingCommentPage
                        !== root.inkInteractionPage) {
                root.cancelInkDrawing(false)
            }
        }

        function onSelectionChanged() {
            if (root.resizingFrame
                    && (root.controller.selectedPage !== root.page
                        || root.controller.selectedObjectId
                            !== root.resizingObjectId)) {
                root.resizingFrame = false
                root.resizingObjectId = ""
                root.frameResizePreview = ({})
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
                    length,
                    root.textLayoutPageHeight)
            }
        }

        function onSelectionFontSizeApplied(start, length, fontSizeRatio) {
            if (!root.editing)
                return

            root.suppressNextEditorReload = true
            root.applyingEditorDocumentFormat = true
            const applied = root.controller.applyFontSizeToEditorDocument(
                editor.textDocument,
                start,
                length,
                fontSizeRatio,
                root.textLayoutPageHeight)
            root.applyingEditorDocumentFormat = false
            if (!applied)
                root.suppressNextEditorReload = false
        }

        function onSelectionTextSpacingApplied(start, length, letterSpacingRatio, wordSpacingRatio) {
            if (!root.editing)
                return

            root.suppressNextEditorReload = true
            root.applyingEditorDocumentFormat = true
            const applied = root.controller.applyTextSpacingToEditorDocument(
                editor.textDocument, start, length,
                letterSpacingRatio, wordSpacingRatio, root.textLayoutPageHeight)
            root.applyingEditorDocumentFormat = false
            if (!applied)
                root.suppressNextEditorReload = false
        }

        function onSelectionParagraphSpacingApplied() {
            if (!root.editing)
                return

            root.suppressNextEditorReload = true
            root.applyingEditorDocumentFormat = true
            const applied = root.controller.applyParagraphFormatsToEditorDocument(
                editor.textDocument, root.textLayoutPageHeight)
            root.applyingEditorDocumentFormat = false
            if (!applied)
                root.suppressNextEditorReload = false
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
        model: root.highlightDrafts

        delegate: Item {
            id: highlightItem

            required property var modelData

            width: root.width
            height: root.height
            z: 0
            visible: !(root.resizingFrame
                && String(modelData.objectId) === root.resizingObjectId)

            Repeater {
                model: highlightItem.modelData.highlightRects

                delegate: Rectangle {
                    id: markupRect

                    required property var modelData

                    readonly property string markupKind:
                        String(highlightItem.modelData.kind)
                    x: modelData.x * root.width
                    y: modelData.y * root.height
                    width: Math.max(1, modelData.width * root.width)
                    height: Math.max(1, modelData.height * root.height)
                    color: markupKind === "highlight"
                        ? highlightItem.modelData.highlightColor
                        : "transparent"
                    visible: highlightItem.modelData.deleted !== true
                    Accessible.ignored: true

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: markupRect.markupKind === "underline"
                            ? parent.bottom
                            : undefined
                        anchors.verticalCenter:
                            markupRect.markupKind === "strikeout"
                                ? parent.verticalCenter
                                : undefined
                        height: Math.max(1, Math.min(3, parent.height * 0.12))
                        visible: markupRect.markupKind !== "highlight"
                            && markupRect.markupKind !== "squiggly"
                        color: highlightItem.modelData.highlightColor
                        Accessible.ignored: true
                    }

                    SquigglyLine {
                        anchors.fill: parent
                        waveColor: highlightItem.modelData.highlightColor
                        pagePointScale: root.pagePointScale
                        visible: markupRect.markupKind === "squiggly"
                        Accessible.ignored: true
                    }
                }
            }
        }
    }

    Repeater {
        model: root.drawingCommentDrafts

        delegate: CommentDrawingView {
            required property var modelData

            width: root.width
            height: root.height
            drawingKind: String(modelData.kind)
            vertices: modelData.annotationPoints
            strokeColor: modelData.strokeColor
            interiorColor: modelData.fillColor
            hasInterior: modelData.drawingHasInterior === true
            borderWidth: Number(modelData.strokeWidthRatio) * root.height
            borderStyle: Number(modelData.borderStyle)
            dashPattern: modelData.dashPattern
            dashScale: root.pagePointScale
            borderEffect: Number(modelData.borderEffect)
            lineStartStyle: Number(modelData.lineStartStyle)
            lineEndStyle: Number(modelData.lineEndStyle)
            drawingOpacity: Number(modelData.opacity)
            visible: (modelData.added === true
                    || modelData.geometryChanged === true
                    || modelData.styleChanged === true)
                && root.drawingPreviewCanRender(modelData)
                && modelData.deleted !== true
                && !(root.resizingFrame
                    && String(modelData.objectId)
                        === root.resizingObjectId)
            z: 1
            Accessible.ignored: true
        }
    }

    Repeater {
        model: root.inkDrafts

        delegate: InkDrawingView {
            required property var modelData

            width: root.width
            height: root.height
            strokes: modelData.inkStrokes
            strokeColor: modelData.strokeColor
            strokeWidth: Number(modelData.strokeWidthRatio) * root.height
            drawingOpacity: Number(modelData.opacity)
            visible: (modelData.added === true
                    || modelData.geometryChanged === true
                    || modelData.styleChanged === true)
                && root.drawingPreviewCanRender(modelData)
                && modelData.deleted !== true
                && !(root.resizingFrame
                    && String(modelData.objectId)
                        === root.resizingObjectId)
            z: 1
            Accessible.ignored: true
        }
    }

    CommentDrawingView {
        readonly property rect frameRect: root.controller.selectedFrameRect

        x: frameRect.x * root.width
        y: frameRect.y * root.height
        width: Math.max(1, frameRect.width * root.width)
        height: Math.max(1, frameRect.height * root.height)
        drawingKind: root.frameResizePreviewKind
        vertices: root.frameResizePreviewVertices
        strokeColor: root.frameResizePreview.strokeColor === undefined
            ? "#000000"
            : root.frameResizePreview.strokeColor
        interiorColor: root.frameResizePreview.fillColor === undefined
            ? "transparent"
            : root.frameResizePreview.fillColor
        hasInterior:
            root.frameResizePreview.drawingHasInterior === true
        borderWidth: Number(
            root.frameResizePreview.strokeWidthRatio === undefined
                ? 0
                : root.frameResizePreview.strokeWidthRatio) * root.height
        borderStyle: Number(
            root.frameResizePreview.borderStyle === undefined
                ? 0
                : root.frameResizePreview.borderStyle)
        dashPattern: root.frameResizePreview.dashPattern === undefined
            ? []
            : root.frameResizePreview.dashPattern
        dashScale: root.pagePointScale
        borderEffect: Number(
            root.frameResizePreview.borderEffect === undefined
                ? 0
                : root.frameResizePreview.borderEffect)
        lineStartStyle: Number(
            root.frameResizePreview.lineStartStyle === undefined
                ? 0
                : root.frameResizePreview.lineStartStyle)
        lineEndStyle: Number(
            root.frameResizePreview.lineEndStyle === undefined
                ? 0
                : root.frameResizePreview.lineEndStyle)
        drawingOpacity: Number(
            root.frameResizePreview.opacity === undefined
                ? 1
                : root.frameResizePreview.opacity)
        visible: root.resizingFrame
            && root.isDrawingCommentKind(root.frameResizePreviewKind)
            && root.frameResizePreviewVertices.length >= 2
            && root.drawingPreviewCanRender(root.frameResizePreview)
        z: 2
        Accessible.ignored: true
    }

    InkDrawingView {
        readonly property rect frameRect: root.controller.selectedFrameRect

        x: frameRect.x * root.width
        y: frameRect.y * root.height
        width: Math.max(1, frameRect.width * root.width)
        height: Math.max(1, frameRect.height * root.height)
        strokes: root.frameResizePreviewStrokes
        strokeColor: root.frameResizePreview.strokeColor === undefined
            ? "#000000"
            : root.frameResizePreview.strokeColor
        strokeWidth: Number(
            root.frameResizePreview.strokeWidthRatio === undefined
                ? 0
                : root.frameResizePreview.strokeWidthRatio) * root.height
        drawingOpacity: Number(
            root.frameResizePreview.opacity === undefined
                ? 1
                : root.frameResizePreview.opacity)
        visible: root.resizingFrame
            && root.frameResizePreviewKind === "ink"
            && root.frameResizePreviewStrokes.length > 0
            && root.drawingPreviewCanRender(root.frameResizePreview)
        z: 2
        Accessible.ignored: true
    }

    Item {
        id: textMarkupFrameResizePreview

        readonly property rect frameRect: root.controller.selectedFrameRect
        readonly property string markupKind: root.frameResizePreviewKind

        x: frameRect.x * root.width
        y: frameRect.y * root.height
        width: Math.max(1, frameRect.width * root.width)
        height: Math.max(1, frameRect.height * root.height)
        visible: root.resizingFrame
            && root.isTextMarkupKind(markupKind)
            && root.frameResizePreviewRects.length > 0
        z: 2
        Accessible.ignored: true

        Repeater {
            model: root.frameResizePreviewRects

            delegate: Rectangle {
                id: previewMarkupRect

                required property var modelData

                x: modelData.x * textMarkupFrameResizePreview.width
                y: modelData.y * textMarkupFrameResizePreview.height
                width: Math.max(
                    1,
                    modelData.width * textMarkupFrameResizePreview.width)
                height: Math.max(
                    1,
                    modelData.height * textMarkupFrameResizePreview.height)
                color: textMarkupFrameResizePreview.markupKind === "highlight"
                    ? (root.frameResizePreview.highlightColor !== undefined
                        ? root.frameResizePreview.highlightColor
                        : "#ffd740")
                    : "transparent"
                Accessible.ignored: true

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom:
                        textMarkupFrameResizePreview.markupKind === "underline"
                            ? parent.bottom
                            : undefined
                    anchors.verticalCenter:
                        textMarkupFrameResizePreview.markupKind === "strikeout"
                            ? parent.verticalCenter
                            : undefined
                    height: Math.max(
                        1,
                        Math.min(3, parent.height * 0.12))
                    visible: textMarkupFrameResizePreview.markupKind
                            !== "highlight"
                        && textMarkupFrameResizePreview.markupKind
                            !== "squiggly"
                    color: root.frameResizePreview.highlightColor !== undefined
                        ? root.frameResizePreview.highlightColor
                        : "#ffd740"
                    Accessible.ignored: true
                }

                SquigglyLine {
                    anchors.fill: parent
                    waveColor: root.frameResizePreview.highlightColor !== undefined
                        ? root.frameResizePreview.highlightColor
                        : "#ffd740"
                    pagePointScale: root.pagePointScale
                    visible: textMarkupFrameResizePreview.markupKind
                        === "squiggly"
                    Accessible.ignored: true
                }
            }
        }
    }

    StampView {
        readonly property rect frameRect: root.controller.selectedFrameRect

        x: frameRect.x * root.width
        y: frameRect.y * root.height
        width: Math.max(1, frameRect.width * root.width)
        height: Math.max(1, frameRect.height * root.height)
        stampName: String(root.frameResizePreview.stampName || "")
        stampText: String(root.frameResizePreview.stampText || "")
        stampArtwork: String(root.frameResizePreview.stampArtwork || "")
        stampColor: root.frameResizePreview.stampColor !== undefined
            ? root.frameResizePreview.stampColor
            : "#c62828"
        visible: root.resizingFrame
            && root.frameResizePreviewKind === "stamp"
        z: 2
        Accessible.ignored: true
    }

    Rectangle {
        id: stickyNoteFrameResizePreview

        readonly property rect frameRect: root.controller.selectedFrameRect

        x: frameRect.x * root.width
        y: frameRect.y * root.height
        width: Math.max(18, frameRect.width * root.width)
        height: Math.max(18, frameRect.height * root.height)
        radius: 4
        color: root.frameResizePreview.highlightColor !== undefined
            ? root.frameResizePreview.highlightColor
            : "#ffeb3b"
        border.width: 1
        border.color: "#b8a316"
        visible: root.resizingFrame
            && root.frameResizePreviewKind === "stickyNote"
        z: 2
        Accessible.ignored: true

        Image {
            anchors.fill: parent
            anchors.margins: Math.max(
                3,
                Math.min(parent.width, parent.height) * 0.2)
            source: "../assets/icons/comment.svg"
            sourceSize.width: width
            sourceSize.height: height
            fillMode: Image.PreserveAspectFit
            smooth: true
            Accessible.ignored: true
        }
    }

    Rectangle {
        id: freeTextFrameResizePreview

        readonly property rect frameRect: root.controller.selectedFrameRect
        readonly property real fontPixelSize: Math.max(
            1,
            (Number(root.frameResizePreview.displayFontSizeRatio) > 0
                ? Number(root.frameResizePreview.displayFontSizeRatio)
                : 0.018) * root.height)
        readonly property font displayFont:
            root.controller.fontForDisplay(
                String(root.frameResizePreview.fontFamily || "Helvetica"),
                Number(root.frameResizePreview.fontStretch || 100),
                root.frameResizePreview.fontBold === true,
                root.frameResizePreview.fontItalic === true,
                root.frameResizePreview.fontUnderline === true,
                fontPixelSize,
                (Number(root.frameResizePreview.letterSpacingRatio || 0) + Number(root.frameResizePreview.sourceAdvanceRatio || 0)) * root.height,
                Number(root.frameResizePreview.wordSpacingRatio || 0) * root.height,
                Math.abs(Number(root.frameResizePreview.sourceAdvanceRatio || 0)) > 1e-12)
        readonly property real contentMargin: Math.min(
            Math.max(2, 4 * root.pagePointScale),
            Math.max(0, Math.min(width, height) / 4))

        x: frameRect.x * root.width
        y: frameRect.y * root.height
        width: Math.max(1, frameRect.width * root.width)
        height: Math.max(1, frameRect.height * root.height)
        color: "#ffffff"
        border.width: Math.max(1, root.pagePointScale)
        border.color: "#000000"
        clip: true
        visible: root.resizingFrame
            && root.frameResizePreviewKind === "freeText"
        z: 2
        Accessible.ignored: true

        Text {
            anchors.fill: parent
            anchors.margins: freeTextFrameResizePreview.contentMargin
            text: String(root.frameResizePreview.text || "")
            textFormat: Text.PlainText
            color: root.frameResizePreview.color !== undefined
                ? root.frameResizePreview.color
                : "#202124"
            font: freeTextFrameResizePreview.displayFont
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            elide: Text.ElideNone
            verticalAlignment: Text.AlignTop
            Accessible.ignored: true
        }
    }

    Repeater {
        model: root.stampDrafts

        delegate: StampView {
            required property var modelData

            readonly property rect frameRect: modelData.frameRect

            x: frameRect.x * root.width
            y: frameRect.y * root.height
            width: Math.max(1, frameRect.width * root.width)
            height: Math.max(1, frameRect.height * root.height)
            stampName: String(modelData.stampName || "")
            stampText: String(modelData.stampText || "")
            stampArtwork: String(modelData.stampArtwork || "")
            stampColor: modelData.stampColor !== undefined
                ? modelData.stampColor
                : "#c62828"
            visible: (modelData.added === true
                    || modelData.geometryChanged === true)
                && modelData.deleted !== true
                && !(root.resizingFrame
                    && String(modelData.objectId)
                        === root.resizingObjectId)
            z: 1
            Accessible.ignored: true
        }
    }

    Repeater {
        model: root.stickyNoteDrafts

        delegate: Rectangle {
            id: stickyNoteItem

            required property var modelData

            readonly property rect frameRect: modelData.frameRect
            x: frameRect.x * root.width
            y: frameRect.y * root.height
            width: Math.max(18, frameRect.width * root.width)
            height: Math.max(18, frameRect.height * root.height)
            radius: 4
            color: modelData.highlightColor
            border.width: 1
            border.color: "#b8a316"
            visible: modelData.deleted !== true
                && !(root.resizingFrame
                    && String(modelData.objectId)
                        === root.resizingObjectId)
            z: 1
            Accessible.ignored: true

            Image {
                anchors.fill: parent
                anchors.margins: Math.max(
                    3,
                    Math.min(parent.width, parent.height) * 0.2)
                source: "../assets/icons/comment.svg"
                sourceSize.width: width
                sourceSize.height: height
                fillMode: Image.PreserveAspectFit
                smooth: true
                Accessible.ignored: true
            }
        }
    }

    Repeater {
        model: root.freeTextDrafts

        delegate: Rectangle {
            id: freeTextItem

            required property var modelData

            readonly property rect frameRect: modelData.frameRect
            readonly property real fontPixelSize: Math.max(
                1,
                (Number(modelData.displayFontSizeRatio) > 0
                    ? Number(modelData.displayFontSizeRatio)
                    : 0.018) * root.height)
            readonly property font displayFont:
                root.controller.fontForDisplay(
                    String(modelData.fontFamily || "Helvetica"),
                    Number(modelData.fontStretch || 100),
                    modelData.fontBold === true,
                    modelData.fontItalic === true,
                    modelData.fontUnderline === true,
                    fontPixelSize,
                    (Number(modelData.letterSpacingRatio || 0) + Number(modelData.sourceAdvanceRatio || 0)) * root.height,
                    Number(modelData.wordSpacingRatio || 0) * root.height,
                    Math.abs(Number(modelData.sourceAdvanceRatio || 0)) > 1e-12)
            readonly property real contentMargin: Math.min(
                Math.max(2, 4 * root.pagePointScale),
                Math.max(0, Math.min(width, height) / 4))

            x: frameRect.x * root.width
            y: frameRect.y * root.height
            width: Math.max(1, frameRect.width * root.width)
            height: Math.max(1, frameRect.height * root.height)
            color: "#ffffff"
            border.width: Math.max(1, root.pagePointScale)
            border.color: "#000000"
            clip: true
            visible: modelData.deleted !== true
                && !(root.resizingFrame
                    && String(modelData.objectId)
                        === root.resizingObjectId)
            z: 1
            Accessible.ignored: true

            Text {
                anchors.fill: parent
                anchors.margins: freeTextItem.contentMargin
                text: String(freeTextItem.modelData.text || "")
                textFormat: Text.PlainText
                color: freeTextItem.modelData.color !== undefined
                    ? freeTextItem.modelData.color
                    : "#202124"
                font: freeTextItem.displayFont
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                elide: Text.ElideNone
                verticalAlignment: Text.AlignTop
                Accessible.ignored: true
            }
        }
    }

    Repeater {
        model: root.contentDrafts

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
            readonly property var paragraphFormats: modelData.paragraphFormats || []
            readonly property real firstParagraphSpaceBefore: paragraphFormats.length > 0
                ? Number(paragraphFormats[0].spaceBeforeRatio || 0) * root.height : 0
            readonly property real lastParagraphSpaceAfter: paragraphFormats.length > 0
                ? Number(paragraphFormats[paragraphFormats.length - 1].spaceAfterRatio || 0)
                    * root.height : 0
            readonly property real textBaselineY: modelData.baselineY * root.height
                + firstParagraphSpaceBefore
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
                modelData.displayFontSizeRatio * root.textLayoutPageHeight)
            readonly property font displayFont: root.controller.fontForDisplay(
                modelData.fontFamily,
                modelData.fontStretch,
                modelData.fontBold,
                modelData.fontItalic,
                modelData.fontUnderline,
                fontPixelSize,
                (Number(modelData.letterSpacingRatio || 0) + Number(modelData.sourceAdvanceRatio || 0)) * root.textLayoutPageHeight,
                Number(modelData.wordSpacingRatio || 0) * root.textLayoutPageHeight,
                Math.abs(Number(modelData.sourceAdvanceRatio || 0)) > 1e-12)
            readonly property real desiredTextPageY: usesBaseline
                ? Math.min(
                    frameY - root.fallbackBleed,
                    textBaselineY - draftText.baselineOffset * root.textLayoutScale)
                : frameY - root.fallbackBleed
            readonly property real textPageY: Math.max(0, desiredTextPageY)
            readonly property real textTop: usesBaseline
                ? Math.max(
                    0,
                    textBaselineY - textPageY - draftText.baselineOffset * root.textLayoutScale)
                : root.fallbackBleed + firstParagraphSpaceBefore
            readonly property real frameBottom: frameY
                + frameHeight
                + root.fallbackBleed
                - textPageY
            readonly property real contentBottom: textTop
                + (isImage || isShape ? 0 : draftText.implicitHeight * root.textLayoutScale
                    + lastParagraphSpaceAfter)
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
            readonly property bool isShape: root.isShapeCandidate(modelData)
            readonly property bool sourceTextMoveOnly:
                modelData.sourceTextMoveOnly === true
            readonly property bool hasText: modelData.text.length > 0
            readonly property bool hasContent:
                modelData.deleted !== true && (isImage || isShape || hasText)
            readonly property bool selected: root.controller.selectedPage === root.page
                && root.controller.selectedObjectId === modelData.objectId

            function syncSelectedContentBounds() {
                if (!selected
                        || isImage
                        || isShape
                        || modelData.restoration === true
                        || root.controller.selectedObjectId !== modelData.objectId) {
                    return
                }

                const selectedFrameRect = root.controller.selectedFrameRect
                const laidOutBottom = Math.min(
                    root.height,
                    draftItem.textPageY
                        + draftItem.textTop
                        + draftText.implicitHeight * root.textLayoutScale
                        + draftItem.lastParagraphSpaceAfter
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
                width: draftItem.width / root.textLayoutScale
                height: implicitHeight
                scale: root.textLayoutScale
                transformOrigin: Item.TopLeft
                leftPadding: Math.max(
                    0,
                    draftItem.contentLeftPageX - draftItem.x) / root.textLayoutScale
                rightPadding: Math.max(
                    0,
                    draftItem.x + draftItem.width - draftItem.contentRightPageX) / root.textLayoutScale
                text: draftItem.richText
                textFormat: Text.RichText
                color: draftItem.modelData.color
                font: draftItem.displayFont
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                elide: Text.ElideNone
                visible: !draftItem.isImage
                    && !draftItem.isShape
                    && !(draftItem.sourceTextMoveOnly
                        && root.draftTextureReady)
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
                imageQuarterTurns: draftItem.modelData.imageQuarterTurns ?? 0
                smooth: true
                visible: draftItem.isImage
                    && draftItem.modelData.deleted !== true
                Accessible.ignored: true
            }

            ShapeDraftView {
                x: draftItem.frameX - draftItem.x
                y: draftItem.frameY - draftItem.y
                width: draftItem.frameWidth
                height: draftItem.frameHeight
                shapeType: draftItem.modelData.shapeType !== undefined
                    ? draftItem.modelData.shapeType
                    : PDFEditController.RectangleShape
                startRatio: draftItem.modelData.shapeStart !== undefined
                    ? draftItem.modelData.shapeStart
                    : Qt.point(0, 0)
                endRatio: draftItem.modelData.shapeEnd !== undefined
                    ? draftItem.modelData.shapeEnd
                    : Qt.point(1, 1)
                fillColor: draftItem.modelData.shapeFillColor !== undefined
                    ? draftItem.modelData.shapeFillColor
                    : "transparent"
                strokeColor: draftItem.modelData.shapeStrokeColor !== undefined
                    ? draftItem.modelData.shapeStrokeColor
                    : "transparent"
                strokeWidth: Math.max(
                    1.25,
                    Number(
                        draftItem.modelData.shapeStrokeWidthRatio
                            !== undefined
                            ? draftItem.modelData.shapeStrokeWidthRatio
                            : 0) * root.height)
                visible: draftItem.isShape
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
                    && root.candidateMatchesSelectionTool(draftItem.modelData)
                    && draftItem.hovered
                    && !draftItem.selected
            }

            MouseArea {
                anchors.fill: parent
                enabled: root.interactionActive
                    && draftItem.modelData.restoration !== true
                    && draftItem.hasContent
                    && !draftItem.isShape
                    && root.controller.tool !== PDFEditController.AddTextTool
                    && root.controller.tool !== PDFEditController.AddImageTool
                    && root.controller.tool !== PDFEditController.AddShapeTool
                    && !root.isCommentCreationTool()
                    && root.candidateMatchesSelectionTool(draftItem.modelData)
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                preventStealing: false
                cursorShape: draftItem.isImage || draftItem.isShape
                    ? Qt.ArrowCursor
                    : root.isSelectionTool()
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
                onPressed: {
                    root.pointerInteractionStarted()
                    hitArea.forceActiveFocus()
                }
                onClicked: mouse => {
                    if (root.isSelectionTool()) {
                        root.showSelectionActions(
                            draftItem.x + mouse.x,
                            draftItem.y + mouse.y,
                            draftItem.modelData)
                    } else {
                        root.selectCandidate(draftItem.modelData)
                    }
                }
                onDoubleClicked: {
                    if (!draftItem.isImage && !draftItem.isShape)
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
        preventStealing: root.isTextMarkupTool()
            || root.isDrawingCommentTool()
            || root.isInkTool()
            || root.isStampTool()
            || root.controller.tool === PDFEditController.AddShapeTool
            || root.controller.tool === PDFEditController.FreeTextTool
        cursorShape: root.selectCursorActive || root.addTextCursorActive
            ? Qt.BlankCursor
            : root.isTextMarkupTool()
                    || root.isDrawingCommentTool()
                    || root.isInkTool()
                    || root.isStampTool()
                    || root.controller.tool === PDFEditController.AddShapeTool
                    || root.controller.tool === PDFEditController.FreeTextTool
                ? Qt.CrossCursor
            : root.controller.tool === PDFEditController.StickyNoteTool
                ? Qt.PointingHandCursor
            : root.controller.tool === PDFEditController.AddTextTool
                    || root.controller.tool === PDFEditController.AddImageTool
            ? Qt.IBeamCursor
            : root.hasCandidate(root.hoverCandidate)
            ? !root.hoverCandidate.editable
                    && !root.hoverCandidate.deletable
                ? Qt.ForbiddenCursor
                : root.isImageCandidate(root.hoverCandidate)
                        || root.isShapeCandidate(root.hoverCandidate)
                    ? Qt.ArrowCursor
                    : root.isCommentKind(root.hoverCandidate.kind)
                        ? Qt.PointingHandCursor
                    : Qt.IBeamCursor
            : Qt.ArrowCursor
        activeFocusOnTab: true
        Accessible.role: Accessible.Pane
        //% "PDF edit surface"
        Accessible.name: qsTrId("PdfEditSurface")
        //% "Use Tab or arrow keys to select content, Enter to edit text, drag selected content to move it, draw with Shapes, markup, or drawing comment tools, and use Enter or double-click to finish polygon comments"
        Accessible.description: qsTrId("PdfEditSurfaceDescriptionWithDrawingComments")

        onEntered: root.updateHoverAtPosition(mouseX, mouseY)
        onPositionChanged: mouse => {
            if (root.highlighting)
                root.updateHighlight(mouse.x, mouse.y)
            else if (root.drawingShape)
                root.updateShape(mouse.x, mouse.y)
            else if (root.placingFreeText)
                root.updateFreeTextPlacement(mouse.x, mouse.y)
            else if (root.placingStamp)
                root.updateStampPlacement(mouse.x, mouse.y)
            else if (root.inkDrawing)
                root.updateInkDrawing(mouse.x, mouse.y)
            else if (root.drawingCommentDragging
                     || root.drawingPathActive)
                root.updateDrawingComment(mouse.x, mouse.y)
            else
                root.updateHoverAtPosition(mouse.x, mouse.y)
        }
        onExited: {
            root.hideSelectCursor()
            root.hoverCandidate = ({})
        }
        onPressed: mouse => {
            root.pointerInteractionStarted()
            forceActiveFocus()
            if (root.isTextMarkupTool()) {
                root.beginHighlight(mouse.x, mouse.y)
                mouse.accepted = true
            } else if (root.controller.tool
                    === PDFEditController.AddShapeTool) {
                root.beginShape(mouse.x, mouse.y)
                mouse.accepted = true
            } else if (root.controller.tool
                    === PDFEditController.FreeTextTool) {
                root.beginFreeTextPlacement(mouse.x, mouse.y)
                mouse.accepted = true
            } else if (root.isStampTool()) {
                root.beginStampPlacement(mouse.x, mouse.y)
                mouse.accepted = true
            } else if (root.isInkTool()) {
                root.beginInkDrawing(mouse.x, mouse.y)
                mouse.accepted = true
            } else if (root.isDrawingCommentTool()
                       && !root.isDrawingPathTool()) {
                root.beginDrawingComment(mouse.x, mouse.y)
                mouse.accepted = true
            }
        }
        onReleased: mouse => {
            root.finishHighlight(mouse.x, mouse.y)
            root.finishShape(mouse.x, mouse.y)
            root.finishFreeTextPlacement(mouse.x, mouse.y)
            root.finishStampPlacement(mouse.x, mouse.y)
            root.finishDrawingComment(mouse.x, mouse.y)
            root.finishInkDrawing(mouse.x, mouse.y)
        }
        onCanceled: {
            root.cancelHighlight()
            root.cancelShape()
            root.cancelFreeTextPlacement()
            root.cancelStampPlacement()
            root.cancelDrawingComment()
            root.cancelInkDrawing()
        }
        onClicked: mouse => {
            if (root.suppressNextPlacementClick) {
                root.suppressNextPlacementClick = false
                return
            }
            if (root.isTextMarkupTool()
                    || root.controller.tool
                        === PDFEditController.AddShapeTool
                    || root.controller.tool
                        === PDFEditController.FreeTextTool
                    || root.isStampTool()
                    || root.isInkTool()) {
                return
            }
            if (root.isDrawingPathTool()) {
                root.addDrawingPathPoint(mouse.x, mouse.y)
                return
            }
            if (root.controller.tool === PDFEditController.StickyNoteTool) {
                root.addStickyNote(mouse.x, mouse.y)
                return
            }
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
                if (root.isSelectionTool()
                        && (root.hoverCandidate.editable
                            || root.hoverCandidate.deletable)) {
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
            if (root.isDrawingPathTool()) {
                root.addDrawingPathPoint(mouse.x, mouse.y)
                root.finishDrawingPath()
                return
            }
            if (root.controller.tool === PDFEditController.AddTextTool
                    || root.controller.tool === PDFEditController.AddImageTool
                    || root.controller.tool === PDFEditController.AddShapeTool
                    || root.isCommentCreationTool())
                return

            root.hoverCandidate = root.candidateAtPosition(mouse.x, mouse.y)
            if (root.hasCandidate(root.hoverCandidate))
                root.beginEditing(root.hoverCandidate)
        }

        Keys.onReturnPressed: event => {
            if (root.drawingPathActive)
                root.finishDrawingPath()
            else
                root.beginSelectedEditing()
            event.accepted = true
        }
        Keys.onEnterPressed: event => {
            if (root.drawingPathActive)
                root.finishDrawingPath()
            else
                root.beginSelectedEditing()
            event.accepted = true
        }
        Keys.onEscapePressed: event => {
            if (root.highlighting) {
                root.cancelHighlight()
                event.accepted = true
            } else if (root.drawingShape) {
                root.cancelShape()
                event.accepted = true
            } else if (root.placingFreeText) {
                root.cancelFreeTextPlacement()
                event.accepted = true
            } else if (root.placingStamp) {
                root.cancelStampPlacement()
                event.accepted = true
            } else if (root.inkDrawing) {
                root.cancelInkDrawing()
                event.accepted = true
            } else if (root.drawingCommentDragging
                       || root.drawingPathActive) {
                root.cancelDrawingComment()
                event.accepted = true
            }
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

    Repeater {
        model: root.textMarkupPreviewRects

        delegate: Item {
            required property var modelData

            x: modelData.x * root.width
            y: modelData.y * root.height
            width: Math.max(1, modelData.width * root.width)
            height: Math.max(1, modelData.height * root.height)
            visible: root.highlighting && root.activeTextMarkupKind !== ""
            z: 5
            Accessible.ignored: true

            Rectangle {
                anchors.fill: parent
                color: root.activeTextMarkupColor
                visible: root.activeTextMarkupKind === "highlight"
                Accessible.ignored: true
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: root.activeTextMarkupKind === "underline"
                    ? parent.bottom
                    : undefined
                anchors.verticalCenter:
                    root.activeTextMarkupKind === "strikeout"
                        ? parent.verticalCenter
                        : undefined
                height: Math.max(1, Math.min(3, parent.height * 0.12))
                color: root.activeTextMarkupColor
                visible: root.activeTextMarkupKind === "underline"
                    || root.activeTextMarkupKind === "strikeout"
                Accessible.ignored: true
            }

            SquigglyLine {
                anchors.fill: parent
                waveColor: root.activeTextMarkupColor
                pagePointScale: root.pagePointScale
                visible: root.activeTextMarkupKind === "squiggly"
                Accessible.ignored: true
            }
        }
    }

    CommentDrawingView {
        width: root.width
        height: root.height
        drawingKind: root.drawingPathActive
                && root.controller.tool === PDFEditController.PolygonTool
            ? "polyLine"
            : root.drawingKindForTool()
        vertices: root.drawingPreviewVertices
        strokeColor: root.controller.drawingCommentColor
        hasInterior: false
        borderWidth: 1.5 * root.pagePointScale
        drawingOpacity: root.controller.drawingCommentColor.a
        visible: root.drawingCommentDragging || root.drawingPathActive
        z: 5
        Accessible.ignored: true
    }

    InkDrawingView {
        width: root.width
        height: root.height
        strokes: root.inkDrawing ? [root.inkPreviewStrokePoints] : []
        strokeColor: root.controller.drawingCommentColor
        strokeWidth: 1.5 * root.pagePointScale
        drawingOpacity: root.controller.drawingCommentColor.a
        visible: root.inkDrawing
        z: 5
        Accessible.ignored: true
    }

    Rectangle {
        x: root.activeFreeTextRect.x
        y: root.activeFreeTextRect.y
        width: Math.max(1, root.activeFreeTextRect.width)
        height: Math.max(1, root.activeFreeTextRect.height)
        color: "#ffffff"
        border.width: Math.max(1, root.pagePointScale)
        border.color: "#000000"
        visible: root.placingFreeText
            && root.controller.tool === PDFEditController.FreeTextTool
        z: 5
        Accessible.ignored: true
    }

    StampView {
        x: root.activeStampRect.x
        y: root.activeStampRect.y
        width: Math.max(1, root.activeStampRect.width)
        height: Math.max(1, root.activeStampRect.height)
        stampName: root.controller.stampName
        stampText: root.controller.stampText
        stampArtwork: root.controller.stampArtwork
        stampColor: root.controller.stampColor
        visible: root.placingStamp
            && root.controller.tool === PDFEditController.StampTool
        z: 5
        Accessible.ignored: true
    }

    ShapeDraftView {
        x: root.activeShapeRect.x
        y: root.activeShapeRect.y
        width: root.activeShapeRect.width
        height: root.activeShapeRect.height
        shapeType: root.controller.shapeType
        startRatio: Qt.point(
            (root.shapeStart.x - root.activeShapeRect.x)
                / root.activeShapeRect.width,
            (root.shapeStart.y - root.activeShapeRect.y)
                / root.activeShapeRect.height)
        endRatio: Qt.point(
            (root.shapeCurrent.x - root.activeShapeRect.x)
                / root.activeShapeRect.width,
            (root.shapeCurrent.y - root.activeShapeRect.y)
                / root.activeShapeRect.height)
        fillColor: "transparent"
        strokeColor: "#000000"
        strokeWidth: 2
        visible: root.drawingShape
        z: 5
        Accessible.ignored: true
    }

    Popup {
        id: selectionActions

        readonly property bool compactSelection:
            root.isImageKind(root.controller.selectedKind)
            || root.isShapeKind(root.controller.selectedKind)
            || root.isCommentKind(root.controller.selectedKind)

        parent: root
        width: compactSelection ? 71 : 136
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

        contentItem: Column {
            id: selectionActionsContent

            spacing: 2
            focus: true

            Keys.onPressed: event => {
                if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete)
                    event.accepted = root.deleteSelectedNonText()
            }

            Row {
                x: Math.max(0, (selectionActionsContent.width - width) / 2)
                spacing: 2

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
                    visible: !selectionActions.compactSelection

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
                    enabled: root.controller.selectedDeletable

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
        border.color: root.hasCandidate(root.hoverCandidate)
                && (root.hoverCandidate.editable
                    || root.hoverCandidate.deletable)
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
            && !root.controller.selectedCommentReply
            && root.controller.selectedKind !== "addedText"
            && !root.controller.selectedEditable
            && !root.hasActiveDraft(root.controller.selectedObjectId)
    }

    Repeater {
        model: root.controller.selectedSourceRects

        delegate: Rectangle {
            id: editorSourceMask

            required property var modelData

            readonly property real sourceX: (
                modelData.x + root.editingSourceTranslationX) * root.width
            readonly property real sourceY: (
                modelData.y + root.editingSourceTranslationY) * root.height
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
                : Number(baseFormat.fontSizeRatio)) * root.textLayoutPageHeight)
        readonly property font displayFont: root.controller.fontForDisplay(
            String(baseFormat.fontFamily || ""),
            Number(baseFormat.fontStretch || 100),
            baseFormat.fontBold === true,
            baseFormat.fontItalic === true,
            baseFormat.fontUnderline === true,
            baseFontPixelSize,
            (Number(baseFormat.letterSpacingRatio || 0) + Number(baseFormat.sourceAdvanceRatio || 0)) * root.textLayoutPageHeight,
            Number(baseFormat.wordSpacingRatio || 0) * root.textLayoutPageHeight,
            Math.abs(Number(baseFormat.sourceAdvanceRatio || 0)) > 1e-12)
        readonly property bool usesBaseline: !editsAddedText
            && root.controller.selectedHasBaseline
        readonly property real textOriginX: root.controller.selectedOriginX * root.width
        readonly property var paragraphFormats: root.controller.selectedParagraphFormats
        readonly property real firstParagraphSpaceBefore: paragraphFormats.length > 0
            ? Number(paragraphFormats[0].spaceBeforeRatio || 0) * root.height : 0
        readonly property real lastParagraphSpaceAfter: paragraphFormats.length > 0
            ? Number(paragraphFormats[paragraphFormats.length - 1].spaceAfterRatio || 0)
                * root.height : 0
        readonly property real textBaselineY: root.controller.selectedBaselineY * root.height
            + firstParagraphSpaceBefore
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
                textBaselineY - editor.baselineOffset * root.textLayoutScale)
            : frameY - verticalBleed
        readonly property real contentTop: usesBaseline
            ? Math.max(0, textBaselineY - y - editor.baselineOffset * root.textLayoutScale)
            : inputMargin + firstParagraphSpaceBefore
        readonly property real frameBottom: frameY
            + Math.max(1, frameHeight)
            + verticalBleed
            - y
        readonly property real editorBottom: contentTop
            + editor.contentHeight * root.textLayoutScale
            + lastParagraphSpaceAfter
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
            width: parent.width / root.textLayoutScale
            height: Math.max(1, contentHeight)
            scale: root.textLayoutScale
            transformOrigin: Item.TopLeft
            leftPadding: Math.max(
                0,
                editorFrame.contentLeftPageX - editorFrame.x) / root.textLayoutScale
            rightPadding: Math.max(
                0,
                editorFrame.x + editorFrame.width - editorFrame.contentRightPageX) / root.textLayoutScale
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
            onTextChanged: Qt.callLater(root.syncEditorTextPreview)
            onTextEdited: {
                if (root.loadingEditorText)
                    return

                root.editorLayoutDirty = true
                // The native typing block closes after this input event.
                Qt.callLater(root.syncEditorTextPreview)
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

        readonly property var selectedDraft:
            root.shapeDraft(root.controller.selectedObjectId)
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
            && !root.controller.selectedCommentReply
            && root.controller.selectedEditable
            && (root.editing
                || (root.controller.tool !== PDFEditController.AddTextTool
                    && root.controller.tool !== PDFEditController.AddImageTool
                    && root.controller.tool !== PDFEditController.AddShapeTool
                    && !root.isCommentCreationTool()))

        Rectangle {
            anchors.fill: parent
            radius: root.editBoxRadius
            color: "#14d97a32"
            border.width: 2
            border.color: root.editBoxColor
            Accessible.ignored: true
        }

        MouseArea {
            id: moveObjectMouse

            property rect dragStartFrameRect: Qt.rect(0, 0, 0, 0)
            property point dragStartPosition: Qt.point(0, 0)

            anchors.fill: parent
            enabled: !root.editing
                && root.isMovableKind(root.controller.selectedKind)
            visible: enabled
            containmentMask: QtObject {
                function contains(point: point): bool {
                    if (!root.isShapeKind(root.controller.selectedKind))
                        return true

                    return root.shapeContainsPoint(
                        resizeOverlay.selectedDraft,
                        Qt.point(
                            point.x - root.boundingBoxPadding,
                            point.y - root.boundingBoxPadding),
                        Math.max(
                            1,
                            resizeOverlay.width
                                - 2 * root.boundingBoxPadding),
                        Math.max(
                            1,
                            resizeOverlay.height
                                - 2 * root.boundingBoxPadding))
                }
            }
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeAllCursor
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: {
                if (root.isImageKind(root.controller.selectedKind)) {
                    //% "Move image"
                    return qsTrId("MoveImage")
                }
                if (root.isShapeKind(root.controller.selectedKind)) {
                    //% "Move shape"
                    return qsTrId("MoveShape")
                }
                if (root.isDrawingCommentKind(
                        root.controller.selectedKind)) {
                    //% "Move drawing comment"
                    return qsTrId("MoveDrawingComment")
                }
                if (root.controller.selectedKind === "ink") {
                    //% "Move ink comment"
                    return qsTrId("MoveInkComment")
                }
                if (root.controller.selectedKind === "stamp") {
                    //% "Move stamp comment"
                    return qsTrId("MoveStampComment")
                }
                if (root.isGeometryCommentKind(
                        root.controller.selectedKind)) {
                    //% "Move comment"
                    return qsTrId("MoveComment")
                }
                //% "Move text"
                return qsTrId("MoveText")
            }

            function commitKeyboardMove(deltaX, deltaY) {
                const startFrameRect = resizeOverlay.layoutFrameRect
                root.startFrameResize()
                root.controller.updateSelectedFrameRect(
                    root.movedFrameRect(startFrameRect, deltaX, deltaY))
                root.finishFrameResize()
            }

            onPressed: mouse => {
                root.pointerInteractionStarted()
                forceActiveFocus()
                selectionActions.close()
                dragStartFrameRect = resizeOverlay.layoutFrameRect
                dragStartPosition = mapToItem(root, mouse.x, mouse.y)
                root.startFrameResize()
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

                root.finishFrameResize()
            }
            onCanceled: root.cancelFrameResize()
            onDoubleClicked: mouse => {
                if (root.controller.selectedKind !== "text"
                        && root.controller.selectedKind !== "addedText") {
                    return
                }

                root.cancelFrameResize()
                root.beginSelectedEditing()
                mouse.accepted = true
            }

            Keys.onPressed: event => {
                if (event.key === Qt.Key_Escape) {
                    root.cancelFrameResize()
                    event.accepted = true
                    return
                }
                if ((event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter)
                        && (root.controller.selectedKind === "text"
                            || root.controller.selectedKind === "addedText")) {
                    root.beginSelectedEditing()
                    event.accepted = true
                    return
                }

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
            model: {
                const selectedKind = root.controller.selectedKind
                if (root.isImageKind(selectedKind)
                        && !root.controller.selectedImageResizable) {
                    return []
                }
                if (root.isImageKind(selectedKind)
                        || root.isGeometryCommentKind(selectedKind)
                        || root.isShapeKind(selectedKind)) {
                    return [
                        { horizontal: -1, vertical: -1 },
                        { horizontal: 0, vertical: -1 },
                        { horizontal: 1, vertical: -1 },
                        { horizontal: 1, vertical: 0 },
                        { horizontal: 1, vertical: 1 },
                        { horizontal: 0, vertical: 1 },
                        { horizontal: -1, vertical: 1 },
                        { horizontal: -1, vertical: 0 }
                    ]
                }
                return [
                    { horizontal: 0, vertical: -1 },
                    { horizontal: 1, vertical: 0 },
                    { horizontal: 0, vertical: 1 },
                    { horizontal: -1, vertical: 0 }
                ]
            }

            delegate: Item {
                id: resizeHandle

                required property var modelData
                property rect dragStartFrameRect: Qt.rect(0, 0, 0, 0)
                property point dragStartPosition: Qt.point(0, 0)
                readonly property string baseAccessibilityName: {
                    if (root.isImageKind(root.controller.selectedKind)) {
                        //% "Resize image"
                        return qsTrId("ResizeImage")
                    }
                    if (root.isShapeKind(root.controller.selectedKind)) {
                        //% "Resize shape"
                        return qsTrId("ResizeShape")
                    }
                    if (root.isDrawingCommentKind(
                            root.controller.selectedKind)) {
                        //% "Resize drawing comment"
                        return qsTrId("ResizeDrawingComment")
                    }
                    if (root.controller.selectedKind === "ink") {
                        //% "Resize ink comment"
                        return qsTrId("ResizeInkComment")
                    }
                    if (root.controller.selectedKind === "stamp") {
                        //% "Resize stamp comment"
                        return qsTrId("ResizeStampComment")
                    }
                    if (root.isGeometryCommentKind(
                            root.controller.selectedKind)) {
                        //% "Resize comment"
                        return qsTrId("ResizeComment")
                    }
                    //% "Resize text box"
                    return qsTrId("ResizeTextBox")
                }
                readonly property string accessibilityName:
                    root.directionalResizeAccessibilityName(
                        baseAccessibilityName,
                        Number(modelData.horizontal),
                        Number(modelData.vertical))

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
                width: root.resizeHandleHitExtent(resizeOverlay.width)
                height: root.resizeHandleHitExtent(resizeOverlay.height)

                Rectangle {
                    anchors.centerIn: parent
                    width: Math.min(root.resizeHandleSize, parent.width)
                    height: Math.min(root.resizeHandleSize, parent.height)
                    radius: Math.min(width, height) / 2
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
                    cursorShape: root.resizeCursorForHandle(
                        Number(resizeHandle.modelData.horizontal),
                        Number(resizeHandle.modelData.vertical))
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: resizeHandle.accessibilityName

                    onPressed: mouse => {
                        root.pointerInteractionStarted()
                        forceActiveFocus()
                        selectionActions.close()
                        resizeHandle.dragStartFrameRect
                            = resizeOverlay.frameRect
                        if (root.editing)
                            root.editorLayoutDirty = true
                        resizeHandle.dragStartPosition = mapToItem(
                            root,
                            mouse.x,
                            mouse.y)
                        root.startFrameResize()
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
                        root.finishFrameResize()
                    }
                    onCanceled: root.cancelFrameResize()

                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_Escape) {
                            root.cancelFrameResize()
                            event.accepted = true
                            return
                        }

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
                        root.startFrameResize()
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
                        root.finishFrameResize()
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
            && !root.controller.selectedCommentReply
            && (!root.controller.selectedEditable
                || (root.isImageKind(root.controller.selectedKind)
                    && !root.controller.selectedImageResizable))

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
