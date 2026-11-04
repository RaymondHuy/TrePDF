#include "PDFEditController.h"
#include "PDFEditController_p.h"
#include "ApplicationFonts.h"
#include "PDFAnnotationUtils.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFSaveWriter.h"
#include "PDFiumSynchronization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMutexLocker>
#include <QPolygonF>
#include <QQuickTextDocument>
#include <QRunnable>
#include <QSet>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QThreadPool>
#include <QTimeZone>
#include <QUuid>
#include <QVariantMap>
#include <QtTranslation>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>

using namespace PDFEditControllerPrivate;

namespace {

int commentAnnotationSubtype(const QString &kind)
{
    if (kind == QLatin1String("underline")) {
        return FPDF_ANNOT_UNDERLINE;
    }
    if (kind == QLatin1String("squiggly")) {
        return FPDF_ANNOT_SQUIGGLY;
    }
    if (kind == QLatin1String("strikeout")) {
        return FPDF_ANNOT_STRIKEOUT;
    }
    if (kind == QLatin1String("stickyNote")) {
        return FPDF_ANNOT_TEXT;
    }
    if (kind == QLatin1String("freeText")) {
        return FPDF_ANNOT_FREETEXT;
    }
    if (kind == QLatin1String("stamp")) {
        return FPDF_ANNOT_STAMP;
    }
    if (kind == QLatin1String("square")) {
        return FPDF_ANNOT_SQUARE;
    }
    if (kind == QLatin1String("circle")) {
        return FPDF_ANNOT_CIRCLE;
    }
    if (kind == QLatin1String("line")) {
        return FPDF_ANNOT_LINE;
    }
    if (kind == QLatin1String("polygon")) {
        return FPDF_ANNOT_POLYGON;
    }
    if (kind == QLatin1String("polyLine")) {
        return FPDF_ANNOT_POLYLINE;
    }
    if (kind == QLatin1String("ink")) {
        return FPDF_ANNOT_INK;
    }
    return kind == QLatin1String("highlight")
        ? FPDF_ANNOT_HIGHLIGHT
        : FPDF_ANNOT_UNKNOWN;
}

bool draftSuppressesSourceObjects(const QString &kind, bool deleted)
{
    return kind == QLatin1String("text")
        || kind == QLatin1String("sourceShape")
        || (kind == QLatin1String("image") && deleted);
}

} // namespace

quint64 PDFEditController::draftRevisionForPage(int page) const
{
    return m_pageDraftRevisions.value(page, 0);
}

QVector<QVector<int>> PDFEditController::draftedTextObjectPathsForPage(int page) const
{
    QVector<QVector<int>> objectPaths;
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        if (candidate.page == page
            && draftSuppressesSourceObjects(candidate.kind, draft->deleted)
            && !isSourceTextMoveOnly(*draft)
            && candidate.renderSuppressible) {
            if (!candidate.parts.isEmpty()) {
                for (const CandidatePart &part : candidate.parts) {
                    if (!part.objectPath.isEmpty()) {
                        objectPaths.append(part.objectPath);
                    }
                    if (!part.underlineObjectPath.isEmpty()) {
                        objectPaths.append(part.underlineObjectPath);
                    }
                }
            } else if (!candidate.objectPath.isEmpty()) {
                objectPaths.append(candidate.objectPath);
            }
        }
    }

    std::sort(objectPaths.begin(), objectPaths.end(), [](const QVector<int> &left,
                                                         const QVector<int> &right) {
        return std::lexicographical_compare(
            left.cbegin(), left.cend(), right.cbegin(), right.cend());
    });
    objectPaths.erase(std::unique(objectPaths.begin(), objectPaths.end()), objectPaths.end());
    return objectPaths;
}

QVector<PDFEditController::DraftObjectMove>
PDFEditController::draftedObjectMovesForPage(int page) const
{
    QVector<DraftObjectMove> moves;
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        if (candidate.page != page || draft->deleted
            || !candidate.renderSuppressible) {
            continue;
        }

        if (candidate.kind == QLatin1String("image")
            && candidate.objectPath.size() == 1
            && (candidate.geometry.sourceRect != draft->frameRect
                || draft->imageQuarterTurns != 0)) {
            moves.append(DraftObjectMove{
                DraftObjectMove::Kind::Image,
                candidate.objectPath,
                candidate.geometry.sourceRect,
                draft->frameRect,
                draft->imageQuarterTurns});
            continue;
        }

        if (!isSourceTextMoveOnly(*draft)) {
            continue;
        }

        const QRectF initialFrameRect
            = (candidate.initialDraftFrameRect.isEmpty()
                   ? candidate.geometry.sourceRect
                   : candidate.initialDraftFrameRect)
                  .normalized();
        const QRectF movedFrameRect = draft->frameRect.normalized();
        const auto appendTextMove = [
            &moves,
            &initialFrameRect,
            &movedFrameRect](const QVector<int> &objectPath,
                            DraftObjectMove::Kind kind = DraftObjectMove::Kind::Text) {
            if (objectPath.size() != 1) {
                return;
            }
            moves.append(DraftObjectMove{
                kind,
                objectPath,
                initialFrameRect,
                movedFrameRect});
        };
        if (candidate.parts.isEmpty()) {
            appendTextMove(candidate.objectPath);
        } else {
            for (const CandidatePart &part : candidate.parts) {
                appendTextMove(part.objectPath);
                appendTextMove(part.underlineObjectPath, DraftObjectMove::Kind::Path);
            }
        }
    }

    std::sort(moves.begin(), moves.end(), [](const DraftObjectMove &left,
                                             const DraftObjectMove &right) {
        return std::lexicographical_compare(
            left.objectPath.cbegin(), left.objectPath.cend(),
            right.objectPath.cbegin(), right.objectPath.cend());
    });
    return moves;
}

QVector<PDFEditController::DraftAnnotationDeletion>
PDFEditController::deletedSourceCommentAnnotationsForPage(int page) const
{
    QVector<DraftAnnotationDeletion> deletions;
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        const bool movesGeometryComment
            = isGeometryCommentCandidateKind(candidate.kind)
            && draft->frameRect
                != initialDraftForCandidate(candidate).frameRect;
        const bool changesDrawingProperties
            = isDrawingCommentCandidateKind(candidate.kind)
            && draft->commentProperties != candidate.commentProperties;
        const bool suppressesSourceAppearance
            = draft->deleted
            || candidate.kind == QLatin1String("freeText")
            || movesGeometryComment
            || changesDrawingProperties;
        if (candidate.page != page
            || candidate.added
            || !suppressesSourceAppearance
            || !isCommentCandidateKind(candidate.kind)
            || candidate.sourceAnnotationIndex < 0) {
            continue;
        }

        const int subtype = commentAnnotationSubtype(candidate.kind);
        if (subtype != FPDF_ANNOT_UNKNOWN) {
            deletions.append(DraftAnnotationDeletion{
                candidate.sourceAnnotationIndex,
                subtype});
        }
    }
    if (m_frameResizeActive
        && m_hasSelection
        && m_selectedCandidate.page == page
        && isGeometryCommentCandidateKind(m_selectedCandidate.kind)
        && !m_selectedCandidate.added
        && m_selectedCandidate.sourceAnnotationIndex >= 0) {
        deletions.append(DraftAnnotationDeletion{
            m_selectedCandidate.sourceAnnotationIndex,
            commentAnnotationSubtype(m_selectedCandidate.kind)});
    }

    std::sort(
        deletions.begin(),
        deletions.end(),
        [](const DraftAnnotationDeletion &left,
           const DraftAnnotationDeletion &right) {
            return left.annotationIndex < right.annotationIndex;
        });
    deletions.erase(
        std::unique(
            deletions.begin(),
            deletions.end(),
            [](const DraftAnnotationDeletion &left,
               const DraftAnnotationDeletion &right) {
                return left.annotationIndex == right.annotationIndex;
            }),
        deletions.end());
    return deletions;
}

bool PDFEditController::allDraftedTextObjectsSuppressibleForPage(int page) const
{
    for (auto draft = m_drafts.cbegin(); draft != m_drafts.cend(); ++draft) {
        const Candidate &candidate = draft->candidate;
        if (candidate.page == page
            && draftSuppressesSourceObjects(candidate.kind, draft->deleted)
            && !isSourceTextMoveOnly(*draft)
            && !candidate.renderSuppressible) {
            return false;
        }
    }
    return true;
}

QVariantList PDFEditController::draftsForPage(int page) const
{
    QVariantList result;
    const auto pageCandidates = m_pageCandidates.constFind(page);
    if (pageCandidates != m_pageCandidates.cend()) {
        for (const Candidate &candidate : pageCandidates.value()) {
            if (candidate.isReplyAnnotation) {
                continue;
            }
            const auto draft = m_drafts.constFind(draftKey(page, candidate.objectId));
            if (draft == m_drafts.cend()) {
                continue;
            }

            const Draft originalDraft = initialDraftForCandidate(candidate);
            QVariantMap map = candidateMap(*draft);
            map.insert(QStringLiteral("originalText"), originalDraft.text);
            map.insert(
                QStringLiteral("originalCharacterSpans"),
                characterSpanMaps(originalDraft.characterSpans));
            map.insert(
                QStringLiteral("originalParagraphFormats"),
                paragraphFormatMaps(originalDraft.paragraphFormats));
            map.insert(QStringLiteral("originalFrameRect"), originalDraft.frameRect);
            map.insert(QStringLiteral("originalContentBounds"), originalDraft.contentBounds);
            if (isDrawingCommentCandidateKind(candidate.kind)
                || isShapeCandidateKind(candidate.kind)) {
                map.insert(
                    QStringLiteral("originalPreview"),
                    candidateMap(originalDraft));
            }
            result.append(map);
        }
    }

    for (const Candidate &candidate : m_addedCandidates) {
        if (candidate.page != page) {
            continue;
        }
        if (candidate.isReplyAnnotation) {
            continue;
        }

        const auto draft = m_drafts.constFind(draftKey(page, candidate.objectId));
        if (draft == m_drafts.cend()) {
            continue;
        }
        const Draft originalDraft = initialDraftForCandidate(candidate);
        QVariantMap map = candidateMap(*draft);
        map.insert(QStringLiteral("originalText"), originalDraft.text);
        map.insert(
            QStringLiteral("originalCharacterSpans"),
            characterSpanMaps(originalDraft.characterSpans));
        map.insert(
            QStringLiteral("originalParagraphFormats"),
            paragraphFormatMaps(originalDraft.paragraphFormats));
        map.insert(QStringLiteral("originalFrameRect"), originalDraft.frameRect);
        map.insert(QStringLiteral("originalContentBounds"), originalDraft.contentBounds);
        result.append(map);
    }
    return result;
}

QVariantMap PDFEditController::selectedDraftPreview() const
{
    return m_hasSelection
        ? candidateMap(selectedDraftForDisplay())
        : QVariantMap();
}

void PDFEditController::deleteSelectedObject()
{
    if (m_saving || !selectedDeletable()) {
        return;
    }
    if (isCommentCandidateKind(m_selectedCandidate.kind)) {
        removeCommentCandidate(m_selectedCandidate);
        return;
    }
    if (isTextCandidateKind(m_selectedCandidate.kind)) {
        commitSelectedText(QString());
        return;
    }

    finishSelectedTextEdit();
    const QString key = draftKey(
        m_selectedCandidate.page,
        m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    if (m_selectedCandidate.kind == QLatin1String("addedImage")
        || m_selectedCandidate.kind == QLatin1String("addedShape")) {
        if (existingDraft == m_drafts.cend()) {
            return;
        }

        Command command;
        command.key = key;
        command.page = m_selectedCandidate.page;
        command.objectId = m_selectedCandidate.objectId;
        command.hadBefore = true;
        command.before = *existingDraft;
        command.hadAfter = false;
        command.after = command.before;
        m_undoStack.append(command);
        m_redoStack.clear();
        applyDraftState(command, true, true);
        emit historyStateChanged();
        return;
    }
    if (m_selectedCandidate.kind != QLatin1String("image")
        && m_selectedCandidate.kind != QLatin1String("sourceShape")) {
        return;
    }

    Command command;
    command.key = key;
    command.page = m_selectedCandidate.page;
    command.objectId = m_selectedCandidate.objectId;
    command.hadBefore = existingDraft != m_drafts.cend();
    command.before = command.hadBefore
        ? *existingDraft
        : initialDraftForCandidate(m_selectedCandidate);
    if (command.before.deleted) {
        return;
    }
    command.hadAfter = true;
    command.after = command.before;
    command.after.deleted = true;
    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
}

int PDFEditController::undo()
{
    resetHeaderSync();
    cancelWebLinkCreation();
    if (m_saving || m_textEditActive) {
        return -1;
    }
    if (m_undoStack.isEmpty()) {
        if (m_formController && m_formController->canUndo()) {
            m_formController->undo();
        }
        return -1;
    }

    const Command command = m_undoStack.takeLast();
    applyDraftState(command, false, true);
    m_redoStack.append(command);
    emit historyStateChanged();
    return command.kind == Command::Kind::PageStructure && m_document
        ? m_document->currentPage()
        : -1;
}

int PDFEditController::redo()
{
    resetHeaderSync();
    cancelWebLinkCreation();
    if (m_saving || m_textEditActive) {
        return -1;
    }
    if (m_redoStack.isEmpty()) {
        if (m_formController && m_formController->canRedo()) {
            m_formController->redo();
        }
        return -1;
    }

    const Command command = m_redoStack.takeLast();
    applyDraftState(command, true, true);
    m_undoStack.append(command);
    emit historyStateChanged();
    return command.kind == Command::Kind::PageStructure && m_document
        ? m_document->currentPage()
        : -1;
}

void PDFEditController::discardDrafts()
{
    resetHeaderSync();
    cancelWebLinkCreation();
    if (m_saving
        || (!m_textEditActive
            && m_pendingOpenPassword.isEmpty()
            && m_watermarkChanges.isEmpty()
            && m_drafts.isEmpty()
            && m_autoWebLinks.isEmpty()
            && m_removedWebLinks.isEmpty()
            && m_manualLinks.isEmpty()
            && m_pendingTextMarkupCreations.isEmpty()
            && m_pageInsertions.isEmpty()
            && m_deletedSourcePages.isEmpty()
            && m_undoStack.isEmpty()
            && m_redoStack.isEmpty()
            && (!m_formController
                || (!m_formController->hasDrafts()
                    && !m_formController->canUndo()
                    && !m_formController->canRedo()))
            && (!m_formDesignerController
                || !m_formDesignerController->hasDrafts()))) {
        return;
    }

    m_pendingOpenPassword.clear();
    emit passwordProtectionChanged();
    m_watermarkChanges.clear();
    invalidateWatermarks();
    QSet<int> affectedPages;
    for (auto revision = m_pageDraftRevisions.cbegin();
         revision != m_pageDraftRevisions.cend();
         ++revision) {
        affectedPages.insert(revision.key());
    }
    for (const PendingTextMarkupCreation &creation
         : std::as_const(m_pendingTextMarkupCreations)) {
        affectedPages.insert(creation.page);
    }
    const bool discardedPageStructure
        = !m_pageInsertions.isEmpty() || !m_deletedSourcePages.isEmpty();
    int deferredSourcePage = -1;
    if (discardedPageStructure && m_document) {
        deferredSourcePage = m_document->sourcePageIndex(
            m_deferredCandidateScanPage);
    }

    const bool wasEditing = m_textEditActive;
    resetFrameResizeState();
    m_textEditActive = false;
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditCommand = Command();
    resetTextEditDisplayState();
    if (discardedPageStructure) {
        if (m_candidateScanCancellation) {
            m_candidateScanCancellation->store(
                true,
                std::memory_order_relaxed);
        }
        m_deferredCandidateScanPage = -1;
        m_pageCandidates.clear();
        m_pageCandidateLru.clear();
        resetCommentIndex();
    }
    m_addedCandidates.clear();
    m_pendingTextMarkupCreations.clear();
    m_pageInsertions.clear();
    m_deletedSourcePages.clear();
    if (m_document) {
        m_document->setTransientPageStructure(
            m_deletedSourcePages,
            m_pageInsertions);
    }
    if (discardedPageStructure) {
        if (deferredSourcePage >= 0
            && m_document
            && m_deferredCandidateScanPage < 0) {
            m_deferredCandidateScanPage
                = m_document->displayPageIndexForSourcePage(
                    deferredSourcePage);
        }
    }
    m_drafts.clear();
    m_autoWebLinks.clear();
    m_removedWebLinks.clear();
    m_manualLinks.clear();
    invalidateWebLinks();
    m_undoStack.clear();
    m_redoStack.clear();
    if (m_formController) {
        m_formController->discardChanges();
    }
    if (m_formDesignerController) {
        m_formDesignerController->resetFromDocument();
    }
    setSelection(nullptr);
    for (int page : std::as_const(affectedPages)) {
        markDraftStateChanged(page, true);
    }
    if (wasEditing) {
        emit textEditActiveChanged();
    }
    emit pageDataChanged(-1);
    if (discardedPageStructure) {
        emit commentsChanged();
        startPendingCandidateScan();
    }
    emit historyStateChanged();
    //% "All draft changes were discarded."
    emit statusMessage(qtTrId("AllDraftChangesDiscarded"));
}

QVariantList PDFEditController::candidateSourceRects(const Candidate &candidate) const
{
    QVariantList sourceRects;
    const auto appendRect = [&sourceRects](const QRectF &rect) {
        QVariantMap map;
        map.insert(QStringLiteral("x"), rect.x());
        map.insert(QStringLiteral("y"), rect.y());
        map.insert(QStringLiteral("width"), rect.width());
        map.insert(QStringLiteral("height"), rect.height());
        sourceRects.append(map);
    };

    if (!candidate.highlightRects.isEmpty()) {
        sourceRects.reserve(candidate.highlightRects.size());
        for (const QRectF &rect : candidate.highlightRects) {
            appendRect(rect);
        }
    } else if (candidate.parts.isEmpty()) {
        appendRect(candidate.geometry.sourceRect);
    } else {
        sourceRects.reserve(candidate.parts.size());
        for (const CandidatePart &part : candidate.parts) {
            appendRect(part.sourceRect);
            if (!part.underlineRect.isEmpty()) {
                appendRect(part.underlineRect);
            }
        }
    }
    return sourceRects;
}

QVariantList PDFEditController::characterSpanMaps(
    const QVector<CharacterSpan> &spans) const
{
    QVariantList maps;
    maps.reserve(spans.size());
    for (const CharacterSpan &span : spans) {
        QVariantMap map;
        map.insert(QStringLiteral("start"), span.start);
        map.insert(QStringLiteral("length"), span.length);
        map.insert(QStringLiteral("end"), span.start + span.length);
        map.insert(QStringLiteral("fontFamily"), span.format.fontFamily);
        map.insert(QStringLiteral("fontSizeRatio"), span.format.fontSizeRatio);
        map.insert(
            QStringLiteral("displayFontSizeRatio"),
            span.format.displayFontSizeRatio > 0.0
                ? span.format.displayFontSizeRatio
                : span.format.fontSizeRatio);
        map.insert(QStringLiteral("fontStretch"), span.format.fontStretch);
        map.insert(QStringLiteral("letterSpacingRatio"), span.format.letterSpacingRatio);
        map.insert(QStringLiteral("wordSpacingRatio"), span.format.wordSpacingRatio);
        map.insert(QStringLiteral("sourceAdvanceRatio"), span.format.sourceAdvanceRatio);
        map.insert(QStringLiteral("virtualSpaceRatio"), span.format.virtualSpaceRatio);
        map.insert(QStringLiteral("fontBold"), span.format.fontBold);
        map.insert(QStringLiteral("textShear"), span.format.textShear);
        map.insert(QStringLiteral("fontItalic"), span.format.fontItalic);
        map.insert(QStringLiteral("fontUnderline"), span.format.fontUnderline);
        map.insert(QStringLiteral("color"), span.format.color);
        maps.append(map);
    }
    return maps;
}

QVariantList PDFEditController::paragraphFormatMaps(
    const QVector<ParagraphFormat> &formats) const
{
    const auto alignmentName = [](ParagraphAlignment alignment) {
        switch (alignment) {
        case LeftAlignment:
            return QStringLiteral("left");
        case CenterAlignment:
            return QStringLiteral("center");
        case RightAlignment:
            return QStringLiteral("right");
        case JustifyAlignment:
            return QStringLiteral("justify");
        }
        return QStringLiteral("left");
    };
    const auto listStyleName = [](ParagraphListStyle listStyle) {
        switch (listStyle) {
        case ParagraphListStyle::None:
            return QStringLiteral("none");
        case ParagraphListStyle::Bullet:
            return QStringLiteral("bullet");
        case ParagraphListStyle::Decimal:
            return QStringLiteral("decimal");
        }
        return QStringLiteral("none");
    };

    QVariantList maps;
    maps.reserve(formats.size());
    for (const ParagraphFormat &format : formats) {
        QVariantMap map;
        map.insert(QStringLiteral("start"), format.start);
        map.insert(QStringLiteral("length"), format.length);
        map.insert(QStringLiteral("end"), format.start + format.length);
        map.insert(QStringLiteral("alignment"), alignmentName(format.alignment));
        map.insert(QStringLiteral("lineSpacing"), format.lineSpacing);
        map.insert(QStringLiteral("spaceBeforeRatio"), format.spaceBeforeRatio);
        map.insert(QStringLiteral("spaceAfterRatio"), format.spaceAfterRatio);
        map.insert(QStringLiteral("firstLineIndentRatio"), format.firstLineIndentRatio);
        map.insert(QStringLiteral("listStyle"), listStyleName(format.listStyle));
        map.insert(QStringLiteral("listLevel"), format.listLevel);
        map.insert(QStringLiteral("listStart"), format.listStart);
        maps.append(map);
    }
    return maps;
}

QVariantMap PDFEditController::candidateMap(const Draft &draft) const
{
    const Candidate &candidate = draft.candidate;
    const PDFCommentAnnotation::CommonProperties &commentProperties
        = draft.commentProperties;
    const CharacterFormat format = primaryCharacterFormat(draft);
    const QPointF layoutOrigin = layoutOriginForDraft(draft);
    const QRectF initialFrameRect = candidate.initialDraftFrameRect.isEmpty()
        ? candidate.geometry.sourceRect
        : candidate.initialDraftFrameRect;
    QVariantMap map;
    map.insert(QStringLiteral("objectId"), candidate.objectId);
    map.insert(QStringLiteral("kind"), candidate.kind);
    map.insert(QStringLiteral("imageResizable"), candidate.editable
        && (candidate.kind == QLatin1String("addedImage")
            || (candidate.kind == QLatin1String("image") && candidate.imageResizable)));
    map.insert(QStringLiteral("imageQuarterTurns"), draft.imageQuarterTurns);
    map.insert(QStringLiteral("label"), candidate.label);
    map.insert(QStringLiteral("sourceRect"), candidate.geometry.sourceRect);
    map.insert(QStringLiteral("frameRect"), draft.frameRect);
    map.insert(QStringLiteral("contentBounds"), draft.contentBounds);
    map.insert(
        QStringLiteral("geometryChanged"),
        draft.frameRect != initialFrameRect || draft.imageQuarterTurns != 0);
    map.insert(
        QStringLiteral("styleChanged"),
        isDrawingCommentCandidateKind(candidate.kind)
            && commentProperties != candidate.commentProperties);
    map.insert(QStringLiteral("deleted"), draft.deleted);
    map.insert(QStringLiteral("sourceRects"), candidateSourceRects(candidate));
    map.insert(QStringLiteral("grouped"), candidate.parts.size() > 1);
    map.insert(QStringLiteral("hasBaseline"), candidate.hasBaseline);
    map.insert(QStringLiteral("originX"), layoutOrigin.x());
    map.insert(QStringLiteral("baselineY"), layoutOrigin.y());
    map.insert(QStringLiteral("text"), draft.text);
    map.insert(
        QStringLiteral("commentAuthor"),
        candidate.commentAuthor);
    map.insert(
        QStringLiteral("commentCreationDate"),
        candidate.commentCreationDate);
    map.insert(
        QStringLiteral("commentModifiedDate"),
        candidate.commentModifiedDate);
    map.insert(
        QStringLiteral("commentDate"),
        candidate.commentModifiedDate.isValid()
            ? candidate.commentModifiedDate
            : candidate.commentCreationDate);
    map.insert(
        QStringLiteral("commentStatus"),
        static_cast<int>(draft.commentStatus));
    map.insert(
        QStringLiteral("commentMarked"),
        draft.commentMarked);
    const bool stateEditable = commentStateEditable(candidate);
    map.insert(
        QStringLiteral("commentStateEditable"),
        stateEditable);
    QString commentStateEditReason;
    if (!stateEditable) {
        if (!documentAllowsAnnotations()) {
            //% "This PDF does not permit changing annotations."
            commentStateEditReason = qtTrId(
                "PdfAnnotationsNotPermitted");
        } else {
            //% "This comment is read-only."
            commentStateEditReason = qtTrId(
                "ReadOnlyCommentCannotBeEdited");
        }
    }
    map.insert(
        QStringLiteral("commentStateEditReason"),
        commentStateEditReason);
    map.insert(
        QStringLiteral("isReply"),
        !candidate.replyToObjectId.isEmpty());
    map.insert(
        QStringLiteral("parentObjectId"),
        candidate.replyToObjectId);
    map.insert(QStringLiteral("replyCount"), 0);
    map.insert(QStringLiteral("characterSpans"), characterSpanMaps(draft.characterSpans));
    map.insert(
        QStringLiteral("paragraphFormats"),
        paragraphFormatMaps(draft.paragraphFormats));
    map.insert(QStringLiteral("fontFamily"), format.fontFamily);
    map.insert(QStringLiteral("fontSizeRatio"), format.fontSizeRatio);
    map.insert(
        QStringLiteral("displayFontSizeRatio"),
        format.displayFontSizeRatio > 0.0
            ? format.displayFontSizeRatio
            : format.fontSizeRatio);
    map.insert(QStringLiteral("fontStretch"), format.fontStretch);
    map.insert(QStringLiteral("letterSpacingRatio"), format.letterSpacingRatio);
    map.insert(QStringLiteral("wordSpacingRatio"), format.wordSpacingRatio);
    map.insert(QStringLiteral("sourceAdvanceRatio"), format.sourceAdvanceRatio);
    map.insert(QStringLiteral("fontBold"), format.fontBold);
    map.insert(QStringLiteral("textShear"), format.textShear);
    map.insert(QStringLiteral("fontItalic"), format.fontItalic);
    map.insert(QStringLiteral("fontUnderline"), format.fontUnderline);
    map.insert(QStringLiteral("color"), format.color);
    map.insert(QStringLiteral("editable"), candidate.editable);
    map.insert(QStringLiteral("deletable"), candidateDeletable(candidate));
    map.insert(QStringLiteral("limited"), candidate.limited);
    map.insert(QStringLiteral("added"), candidate.added);
    map.insert(
        QStringLiteral("sourceTextMoveOnly"),
        isSourceTextMoveOnly(draft));
    map.insert(QStringLiteral("image"), candidate.image);
    map.insert(QStringLiteral("imagePixelWidth"), candidate.image.width());
    map.insert(QStringLiteral("imagePixelHeight"), candidate.image.height());
    map.insert(
        QStringLiteral("shapeType"),
        static_cast<int>(candidate.shapeType));
    map.insert(QStringLiteral("shapeStart"), candidate.shapeStartRatio);
    map.insert(QStringLiteral("shapeEnd"), candidate.shapeEndRatio);
    map.insert(QStringLiteral("tableRows"), candidate.tableRows);
    map.insert(QStringLiteral("tableColumns"), candidate.tableColumns);
    const auto shapeColor = [&commentProperties](const QColor &color) {
        QColor result = color.isValid() ? color.toRgb() : QColor(Qt::transparent);
        result.setAlphaF(result.alphaF() * commentProperties.opacity);
        return result;
    };
    map.insert(
        QStringLiteral("shapeFillColor"),
        shapeColor(commentProperties.interiorColor));
    map.insert(
        QStringLiteral("shapeStrokeColor"),
        shapeColor(commentProperties.strokeColor));
    map.insert(
        QStringLiteral("shapeStrokeWidthRatio"),
        candidate.shapeStrokeWidthRatio);
    const QVector<QRectF> displayedHighlightRects
        = highlightRectsForDraft(draft);
    QVariantList highlightRects;
    highlightRects.reserve(displayedHighlightRects.size());
    for (const QRectF &rect : displayedHighlightRects) {
        QVariantMap rectMap;
        rectMap.insert(QStringLiteral("x"), rect.x());
        rectMap.insert(QStringLiteral("y"), rect.y());
        rectMap.insert(QStringLiteral("width"), rect.width());
        rectMap.insert(QStringLiteral("height"), rect.height());
        highlightRects.append(rectMap);
    }
    map.insert(QStringLiteral("highlightRects"), highlightRects);
    map.insert(QStringLiteral("highlightColor"), candidate.highlightColor);
    map.insert(
        QStringLiteral("stampName"),
        candidate.commentProperties.name);
    map.insert(QStringLiteral("stampText"), candidate.stampText);
    map.insert(QStringLiteral("stampDetail"), candidate.stampDetail);
    map.insert(QStringLiteral("stampArtwork"), candidate.stampArtwork);
    map.insert(QStringLiteral("stampColor"), candidate.highlightColor);
    const QVector<QPointF> displayedDrawingVertices
        = drawingVerticesForDraft(draft);
    QVariantList drawingVertices;
    drawingVertices.reserve(displayedDrawingVertices.size());
    for (const QPointF &vertex : displayedDrawingVertices) {
        drawingVertices.append(vertex);
    }
    map.insert(QStringLiteral("drawingVertices"), drawingVertices);
    map.insert(QStringLiteral("annotationPoints"), drawingVertices);
    const QVector<QVector<QPointF>> displayedInkStrokes
        = inkStrokesForDraft(draft);
    QVariantList inkStrokes;
    inkStrokes.reserve(displayedInkStrokes.size());
    for (const QVector<QPointF> &stroke : displayedInkStrokes) {
        QVariantList strokePoints;
        strokePoints.reserve(stroke.size());
        for (const QPointF &point : stroke) {
            strokePoints.append(point);
        }
        inkStrokes.append(QVariant::fromValue(strokePoints));
    }
    map.insert(QStringLiteral("inkStrokes"), inkStrokes);
    map.insert(QStringLiteral("annotationStrokes"), inkStrokes);
    map.insert(
        QStringLiteral("drawingStrokeColor"),
        commentProperties.strokeColor);
    map.insert(
        QStringLiteral("strokeColor"),
        commentProperties.strokeColor);
    map.insert(
        QStringLiteral("drawingInteriorColor"),
        commentProperties.interiorColor);
    map.insert(
        QStringLiteral("fillColor"),
        commentProperties.interiorColor);
    map.insert(
        QStringLiteral("drawingHasInterior"),
        commentProperties.interiorColor.isValid());
    map.insert(
        QStringLiteral("drawingOpacity"),
        commentProperties.opacity);
    map.insert(
        QStringLiteral("opacity"),
        commentProperties.opacity);
    map.insert(
        QStringLiteral("drawingBorderWidth"),
        commentProperties.borderWidth);
    qreal strokeWidthRatio = 0.0;
    if (m_document
        && candidate.page >= 0
        && candidate.page < m_document->pageCount()) {
        const qreal pageHeight = m_document->pageSize(candidate.page).height();
        if (pageHeight > 0.0) {
            strokeWidthRatio = commentProperties.borderWidth / pageHeight;
        }
    }
    map.insert(QStringLiteral("strokeWidthRatio"), strokeWidthRatio);
    map.insert(
        QStringLiteral("drawingBorderStyle"),
        static_cast<int>(commentProperties.borderStyle));
    map.insert(
        QStringLiteral("borderStyle"),
        static_cast<int>(commentProperties.borderStyle));
    QVariantList dashPattern;
    dashPattern.reserve(commentProperties.dashPattern.size());
    for (qreal dash : commentProperties.dashPattern) {
        dashPattern.append(dash);
    }
    map.insert(QStringLiteral("drawingDashPattern"), dashPattern);
    map.insert(QStringLiteral("dashPattern"), dashPattern);
    map.insert(
        QStringLiteral("drawingBorderEffect"),
        static_cast<int>(commentProperties.borderEffect));
    map.insert(
        QStringLiteral("borderEffect"),
        static_cast<int>(commentProperties.borderEffect));
    map.insert(
        QStringLiteral("drawingStartLineEnding"),
        static_cast<int>(commentProperties.startLineEnding));
    map.insert(
        QStringLiteral("lineStartStyle"),
        static_cast<int>(commentProperties.startLineEnding));
    map.insert(
        QStringLiteral("drawingEndLineEnding"),
        static_cast<int>(commentProperties.endLineEnding));
    map.insert(
        QStringLiteral("lineEndStyle"),
        static_cast<int>(commentProperties.endLineEnding));
    map.insert(QStringLiteral("reason"), candidate.reason);
    return map;
}

QPointF PDFEditController::layoutOriginForDraft(const Draft &draft)
{
    const Candidate &candidate = draft.candidate;
    if (!candidate.hasBaseline) {
        return candidate.origin;
    }

    const QRectF initialFrameRect = candidate.initialDraftFrameRect.isEmpty()
        ? candidate.geometry.sourceRect
        : candidate.initialDraftFrameRect;
    return candidate.origin + QPointF(
        draft.frameRect.x() - initialFrameRect.x(),
        draft.frameRect.y() - initialFrameRect.y());
}

bool PDFEditController::sameCharacterFormat(const CharacterFormat &left,
                                            const CharacterFormat &right)
{
    return sameFontFamily(left.fontFamily, right.fontFamily)
        && left.fontSizeRatio == right.fontSizeRatio
        && left.displayFontSizeRatio == right.displayFontSizeRatio
        && left.fontStretch == right.fontStretch
        && left.letterSpacingRatio == right.letterSpacingRatio
        && left.wordSpacingRatio == right.wordSpacingRatio
        && left.sourceAdvanceRatio == right.sourceAdvanceRatio
        && left.virtualSpaceRatio == right.virtualSpaceRatio
        && left.fontBold == right.fontBold
        && left.textShear == right.textShear
        && left.fontItalic == right.fontItalic
        && left.fontUnderline == right.fontUnderline
        && left.color == right.color;
}

bool PDFEditController::sameCharacterSpans(const QVector<CharacterSpan> &left,
                                           const QVector<CharacterSpan> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (qsizetype index = 0; index < left.size(); ++index) {
        const CharacterSpan &leftSpan = left.at(index);
        const CharacterSpan &rightSpan = right.at(index);
        if (leftSpan.start != rightSpan.start
            || leftSpan.length != rightSpan.length
            || !sameCharacterFormat(leftSpan.format, rightSpan.format)) {
            return false;
        }
    }
    return true;
}

bool PDFEditController::sameParagraphFormats(
    const QVector<ParagraphFormat> &left,
    const QVector<ParagraphFormat> &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (qsizetype index = 0; index < left.size(); ++index) {
        const ParagraphFormat &leftFormat = left.at(index);
        const ParagraphFormat &rightFormat = right.at(index);
        if (leftFormat.start != rightFormat.start
            || leftFormat.length != rightFormat.length
            || leftFormat.alignment != rightFormat.alignment
            || leftFormat.lineSpacing != rightFormat.lineSpacing
            || leftFormat.spaceBeforeRatio != rightFormat.spaceBeforeRatio
            || leftFormat.spaceAfterRatio != rightFormat.spaceAfterRatio
            || leftFormat.firstLineIndentRatio != rightFormat.firstLineIndentRatio
            || leftFormat.listStyle != rightFormat.listStyle
            || leftFormat.listLevel != rightFormat.listLevel
            || leftFormat.listStart != rightFormat.listStart) {
            return false;
        }
    }
    return true;
}

bool PDFEditController::sameDraftContent(const Draft &left, const Draft &right)
{
    return left.text == right.text
        && left.commentProperties == right.commentProperties
        && left.commentStatus == right.commentStatus
        && left.commentMarked == right.commentMarked
        && sameCharacterSpans(left.characterSpans, right.characterSpans)
        && sameParagraphFormats(left.paragraphFormats, right.paragraphFormats)
        && left.sourceLineBreaks == right.sourceLineBreaks
        && left.frameRect == right.frameRect
        && left.contentBounds == right.contentBounds
        && left.imageQuarterTurns == right.imageQuarterTurns
        && left.deleted == right.deleted;
}

void PDFEditController::updateSelectedDraft(const Draft &draft)
{
    if (!m_hasSelection
        || draft.candidate.page != m_selectedCandidate.page
        || draft.candidate.objectId != m_selectedCandidate.objectId) {
        return;
    }

    Draft normalizedDraft = draft;
    const QDateTime commentModifiedDate
        = normalizedDraft.candidate.commentModifiedDate;
    const QString commentThreadId
        = normalizedDraft.candidate.commentThreadId;
    normalizedDraft.candidate = m_selectedCandidate;
    if (commentModifiedDate.isValid()) {
        normalizedDraft.candidate.commentModifiedDate
            = commentModifiedDate;
    }
    if (!commentThreadId.isEmpty()) {
        normalizedDraft.candidate.commentThreadId = commentThreadId;
    }
    if (m_textEditActive && m_textEditDisplayDraftActive) {
        m_textEditDisplayDraft = normalizedDraft;
    }
    const QString displayText = normalizedDraft.text;
    normalizedDraft = collapseEditorVirtualSpaces(normalizedDraft);
    const Draft originalDraft = initialDraftForCandidate(m_selectedCandidate);
    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    const bool matchesOriginal
        = m_selectedCandidate.kind != QLatin1String("addedImage")
        && m_selectedCandidate.kind != QLatin1String("addedShape")
        && !(m_selectedCandidate.added
             && m_selectedCandidate.kind == QLatin1String("freeText"))
        && sameDraftContent(normalizedDraft, originalDraft);
    bool changed = false;
    if (matchesOriginal) {
        changed = m_drafts.remove(key) > 0;
    } else if (existingDraft == m_drafts.cend()
               || !sameDraftContent(*existingDraft, normalizedDraft)
               || existingDraft->candidate.commentModifiedDate
                   != normalizedDraft.candidate.commentModifiedDate) {
        m_drafts.insert(key, normalizedDraft);
        changed = true;
    }

    m_selectedText = displayText;
    if (changed) {
        const bool reusesSourceCommentSuppressionTexture
            = m_frameResizeActive
            && isGeometryCommentCandidateKind(
                normalizedDraft.candidate.kind)
            && !normalizedDraft.candidate.added;
        if (reusesSourceCommentSuppressionTexture) {
            emit pageDataChanged(m_selectedCandidate.page);
        } else {
            const bool notifyComments = !m_frameResizeActive
                || !isCommentCandidateKind(
                    normalizedDraft.candidate.kind);
            markDraftStateChanged(
                m_selectedCandidate.page,
                false,
                notifyComments);
        }
        emit historyStateChanged();
    }
    emit selectionChanged();
}

void PDFEditController::markDraftStateChanged(int page,
                                              bool immediate,
                                              bool notifyComments)
{
    const quint64 revision = ++m_nextDraftRevision;
    m_pageDraftRevisions.insert(page, revision);
    emit draftRenderStateChanged(page, revision, immediate);
    emit pageDataChanged(page);
    if (notifyComments) {
        emit commentsChanged();
    }
}

void PDFEditController::requestDraftRender(int page, bool immediate)
{
    emit draftRenderStateChanged(page, draftRevisionForPage(page), immediate);
}

void PDFEditController::applyDraftState(const Command &command,
                                        bool useAfterState,
                                        bool immediate)
{
    if (command.kind == Command::Kind::HeaderDraftBatch) {
        applyHeaderDraftBatch(command, useAfterState, immediate);
        return;
    }
    if (command.kind == Command::Kind::ObjectDraftBatch) {
        QSet<int> changedPages;
        bool removesSelection = false;
        // Complete the entire mutation before notifying observers. In particular,
        // draftsForPage() must never expose a partly-applied multi-page header.
        for (const Draft &draft : command.addedDrafts) {
            const Candidate &candidate = draft.candidate;
            const QString key = draftKey(candidate.page, candidate.objectId);
            if (useAfterState) {
                const auto insertionPoint = std::lower_bound(
                    m_addedCandidates.begin(), m_addedCandidates.end(),
                    candidate.addedOrder,
                    [](const Candidate &existing, quint64 order) {
                        return existing.addedOrder < order;
                    });
                if (insertionPoint == m_addedCandidates.end()
                    || insertionPoint->addedOrder != candidate.addedOrder) {
                    m_addedCandidates.insert(insertionPoint, candidate);
                }
                m_drafts.insert(key, draft);
            } else {
                m_drafts.remove(key);
                removeAddedCandidate(candidate.page, candidate.objectId);
                removesSelection = removesSelection
                    || (m_hasSelection && m_selectedCandidate.page == candidate.page
                        && m_selectedCandidate.objectId == candidate.objectId);
            }
            changedPages.insert(candidate.page);
        }
        if (removesSelection) {
            setSelection(nullptr);
        }
        QList<int> pages = changedPages.values();
        std::sort(pages.begin(), pages.end());
        for (int page : pages) {
            markDraftStateChanged(page, immediate, false);
        }
        return;
    }
    if (command.kind == Command::Kind::Watermarks) {
        m_watermarkChanges = useAfterState ? command.watermarksAfter : command.watermarksBefore;
        if (watermarkChangesPending()) {
            clearSelection();
            setTool(BrowseTool);
        }
        invalidateWatermarks();
        return;
    }
    if (command.kind == Command::Kind::WebLinks) {
        m_autoWebLinks = useAfterState ? command.webLinksAfter : command.webLinksBefore;
        m_removedWebLinks = useAfterState ? command.removedWebLinksAfter
                                         : command.removedWebLinksBefore;
        m_manualLinks = useAfterState ? command.manualLinksAfter : command.manualLinksBefore;
        invalidateWebLinks();
        return;
    }
    if (command.kind == Command::Kind::PageStructure) {
        applyPageState(
            useAfterState
                ? command.pageStateAfter
                : command.pageStateBefore);
        return;
    }

    const bool shouldExist = useAfterState ? command.hadAfter : command.hadBefore;
    Draft draft = useAfterState ? command.after : command.before;
    Candidate candidate = draft.candidate;
    if (candidate.objectId.isEmpty()) {
        const Candidate *currentCandidate = findCandidate(command.page, command.objectId);
        if (currentCandidate) {
            candidate = *currentCandidate;
            draft.candidate = candidate;
        }
    }
    if (candidate.objectId.isEmpty()) {
        return;
    }

    bool candidateChanged = false;
    if (candidate.added && shouldExist && !findCandidate(command.page, command.objectId)) {
        const auto insertionPoint = std::lower_bound(
            m_addedCandidates.begin(),
            m_addedCandidates.end(),
            candidate.addedOrder,
            [](const Candidate &existing, quint64 addedOrder) {
                return existing.addedOrder < addedOrder;
            });
        m_addedCandidates.insert(insertionPoint, candidate);
        candidateChanged = true;
    }

    const auto existingDraft = m_drafts.constFind(command.key);
    const bool draftChanged = shouldExist
        ? existingDraft == m_drafts.cend()
            || !sameDraftContent(*existingDraft, draft)
        : existingDraft != m_drafts.cend();

    if (shouldExist && draftChanged) {
        m_drafts.insert(command.key, draft);
    } else if (draftChanged) {
        m_drafts.remove(command.key);
    }

    if (candidate.added && !shouldExist) {
        candidateChanged = removeAddedCandidate(command.page, command.objectId)
            || candidateChanged;
    }

    const bool selectedCandidateChanged = m_hasSelection
        && m_selectedCandidate.page == command.page
        && m_selectedCandidate.objectId == command.objectId;
    const bool selectionWasRemoved = candidate.added && !shouldExist;
    const bool selectionWasDeleted = shouldExist
        && draftRepresentsDeletion(candidate.kind, draft.text, draft.deleted);
    const bool selectedReplyWasHidden = m_hasSelection
        && isCommentCandidateKind(m_selectedCandidate.kind)
        && !m_selectedCandidate.replyToObjectId.isEmpty()
        && !commentReplyIsVisible(m_selectedCandidate);
    if ((selectedCandidateChanged
         && (selectionWasRemoved || selectionWasDeleted))
        || selectedReplyWasHidden) {
        setSelection(nullptr);
    } else {
        updateSelectedTextFromDraft();
    }

    const bool changed = candidateChanged || draftChanged;
    if (changed) {
        markDraftStateChanged(command.page, immediate);
    } else {
        requestDraftRender(command.page, immediate);
    }
}

PDFEditController::PageState PDFEditController::capturePageState() const
{
    PageState state;
    state.deletedSourcePages = m_deletedSourcePages;
    state.pageInsertions = m_pageInsertions;
    state.addedCandidates = m_addedCandidates;
    state.drafts = m_drafts;
    state.currentPage = m_document ? m_document->currentPage() : 0;
    state.hasSelection = m_hasSelection;
    state.selectedCandidate = m_selectedCandidate;
    return state;
}

void PDFEditController::applyPageState(const PageState &state)
{
    if (!m_document || !m_document->isLoaded()) {
        return;
    }

    const int deferredSourcePage
        = m_document->sourcePageIndex(m_deferredCandidateScanPage);
    if (m_candidateScanCancellation) {
        m_candidateScanCancellation->store(true, std::memory_order_relaxed);
    }
    m_deferredCandidateScanPage = -1;
    m_pageCandidates.clear();
    m_pageCandidateLru.clear();
    resetCommentIndex();
    m_pageDraftRevisions.clear();

    m_deletedSourcePages = state.deletedSourcePages;
    m_pageInsertions = state.pageInsertions;
    m_addedCandidates = state.addedCandidates;
    m_drafts = state.drafts;
    m_document->setTransientPageStructure(
        m_deletedSourcePages,
        m_pageInsertions,
        state.currentPage);
    if (deferredSourcePage >= 0
        && m_deferredCandidateScanPage < 0) {
        m_deferredCandidateScanPage
            = m_document->displayPageIndexForSourcePage(deferredSourcePage);
    }

    QVector<PendingTextMarkupCreation> remappedPendingCreations;
    remappedPendingCreations.reserve(m_pendingTextMarkupCreations.size());
    for (PendingTextMarkupCreation creation
         : std::as_const(m_pendingTextMarkupCreations)) {
        const int remappedPage
            = m_document->displayPageIndexForSourcePage(creation.sourcePage);
        if (remappedPage < 0) {
            // Deleting the source page cancels its unfinished annotation.
            continue;
        }

        const QSizeF remappedPageSize = m_document->pageSize(remappedPage);
        if (remappedPageSize.isEmpty()) {
            continue;
        }
        creation.page = remappedPage;
        creation.pageSize = remappedPageSize;
        if (!pendingTextMarkupCreationIsCurrent(creation)) {
            continue;
        }
        remappedPendingCreations.append(std::move(creation));
    }
    m_pendingTextMarkupCreations = std::move(remappedPendingCreations);

    if (state.hasSelection) {
        const Candidate selectedCandidate = state.selectedCandidate;
        setSelection(&selectedCandidate);
    } else {
        setSelection(nullptr);
    }
    emit pageDataChanged(-1);
    emit commentsChanged();
    startPendingCandidateScan();
}

bool PDFEditController::removeAddedCandidate(int page, const QString &objectId)
{
    for (qsizetype index = 0; index < m_addedCandidates.size(); ++index) {
        const Candidate &candidate = m_addedCandidates.at(index);
        if (candidate.page == page && candidate.objectId == objectId) {
            m_addedCandidates.removeAt(index);
            return true;
        }
    }
    return false;
}

void PDFEditController::updateSelectedTextFromDraft()
{
    if (!m_hasSelection) {
        return;
    }
    if (!candidateMatchesSelectionScope(m_selectedCandidate)) {
        setSelection(nullptr);
        return;
    }

    const auto draft = m_drafts.constFind(
        draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId));
    m_selectedText = draft == m_drafts.cend() ? m_selectedCandidate.text : draft->text;
    emit selectionChanged();
}
