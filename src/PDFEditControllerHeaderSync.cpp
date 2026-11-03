#include "PDFEditController.h"
#include "PDFEditController_p.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QDebug>
#include <QRegularExpression>
#include <QRunnable>
#include <QTextLayout>
#include <QThreadPool>
#include <QtTranslation>

#include <algorithm>
#include <cmath>

using namespace PDFEditControllerPrivate;

namespace {
constexpr int maximumPages = 10000;
constexpr qsizetype maximumRetainedBytes = 64 * 1024 * 1024;
constexpr qsizetype maximumTextLength = 32768;
constexpr qsizetype maximumBatchTextLength = 1000000;

QThreadPool &headerScanPool()
{
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured)
    return pool;
}

QRectF inPoints(const QRectF &rect, const QSizeF &size)
{
    return {rect.x() * size.width(), rect.y() * size.height(),
            rect.width() * size.width(), rect.height() * size.height()};
}

QRectF fromPoints(const QRectF &rect, const QSizeF &size)
{
    return {rect.x() / size.width(), rect.y() / size.height(),
            rect.width() / size.width(), rect.height() / size.height()};
}

QRectF inMarginPoints(const QRectF &rect, const QSizeF &size, bool footer)
{
    return inPoints(rect, size).translated(0.0, footer ? -qMax(0.0, size.height() - 100.0) : 0.0);
}

QRectF fromMarginPoints(const QRectF &rect, const QSizeF &size, bool footer)
{
    return fromPoints(rect.translated(0.0, footer ? qMax(0.0, size.height() - 100.0) : 0.0), size);
}

bool isPageNumberRole(const QString &role)
{
    return role == QLatin1String("pageNumber") || role == QLatin1String("footerPageNumber");
}

bool withinTolerance(qreal a, qreal b, qreal tolerance = 2.0)
{
    return qIsFinite(a) && qIsFinite(b) && qAbs(a - b) <= tolerance;
}

bool validRect(const QRectF &rect)
{
    return qIsFinite(rect.x()) && qIsFinite(rect.y())
        && qIsFinite(rect.width()) && qIsFinite(rect.height())
        && rect.width() > 0.0 && rect.height() > 0.0;
}

// Only complete canonical decimal/Roman strings qualify for the external-PDF
// fallback. A matching value must advance with the PDF page index as well.
int numberValue(const QString &text, bool &roman)
{
    roman = false;
    static const QRegularExpression decimal(QStringLiteral("^[1-9][0-9]{0,8}$"));
    if (decimal.match(text).hasMatch())
        return text.toInt();
    static const QRegularExpression numeral(QStringLiteral(
        "^(?=.+$)m{0,3}(cm|cd|d?c{0,3})(xc|xl|l?x{0,3})(ix|iv|v?i{0,3})$"));
    if (!numeral.match(text.toLower()).hasMatch())
        return 0;
    const auto digit = [](QChar c) {
        switch (c.toLatin1()) {
        case 'i': return 1; case 'v': return 5; case 'x': return 10;
        case 'l': return 50; case 'c': return 100; case 'd': return 500;
        case 'm': return 1000; default: return 0;
        }
    };
    const QString lower = text.toLower();
    int value = 0;
    for (qsizetype i = 0; i < lower.size(); ++i) {
        const int current = digit(lower.at(i));
        value += i + 1 < lower.size() && current < digit(lower.at(i + 1))
            ? -current : current;
    }
    roman = true;
    return value;
}

bool selectedPages(const QString &expression, int count, QSet<int> &pages)
{
    if (expression.size() > 10000)
        return false;
    if (expression.trimmed().isEmpty())
        return true;
    static const QRegularExpression range(QStringLiteral("^([0-9]+)(?:\\s*-\\s*([0-9]+))?$"));
    int work = 0;
    for (const QString &part : expression.split(QLatin1Char(','))) {
        const auto match = range.match(part.trimmed());
        bool firstOk = false, lastOk = false;
        const int first = match.captured(1).toInt(&firstOk);
        const int last = match.captured(2).isEmpty()
            ? first : match.captured(2).toInt(&lastOk);
        if (!match.hasMatch() || !firstOk
            || (!match.captured(2).isEmpty() && !lastOk)
            || first < 1 || last < first || last > count || last - first > maximumPages)
            return false;
        if ((work += last - first + 1) > 100000)
            return false;
        for (int page = first; page <= last; ++page)
            pages.insert(page - 1);
    }
    return true;
}
} // namespace

bool PDFEditController::headerSyncAvailable() const
{
    return m_headerSyncValid && m_headerSelectionOnly && m_tool == SelectTool
        && m_document && m_document->isLoaded() && !m_saving;
}

QString PDFEditController::headerSyncKey() const
{
    return m_headerSyncValid
        ? draftKey(m_headerSyncBaseline.candidate.page, m_headerSyncBaseline.candidate.objectId)
        : QString();
}

int PDFEditController::headerSyncMatchCount() const
{
    return m_headerSyncReady ? m_headerSyncMatches.size() + 1 : 0;
}

QString PDFEditController::headerSyncMatchPages() const
{
    if (!m_headerSyncReady)
        return {};
    QList<int> pages{m_headerSyncBaseline.candidate.page + 1};
    for (const Draft &draft : m_headerSyncMatches)
        pages.append(draft.candidate.page + 1);
    std::sort(pages.begin(), pages.end());
    QStringList parts;
    for (qsizetype i = 0; i < pages.size(); ++i) {
        const int first = pages.at(i);
        int last = first;
        while (i + 1 < pages.size() && pages.at(i + 1) == last + 1)
            last = pages.at(++i);
        parts.append(first == last ? QString::number(first)
            : QStringLiteral("%1-%2").arg(first).arg(last));
    }
    return parts.join(QStringLiteral(", "));
}

PDFEditController::Draft PDFEditController::currentHeaderSyncDraft() const
{
    const Candidate &candidate = m_headerSyncBaseline.candidate;
    if (m_textEditActive && m_hasSelection
        && m_selectedCandidate.page == candidate.page
        && m_selectedCandidate.objectId == candidate.objectId)
        return selectedDraftForDisplay();
    const auto draft = m_drafts.constFind(draftKey(candidate.page, candidate.objectId));
    if (draft != m_drafts.cend())
        return *draft;
    Draft result = initialDraftForCandidate(candidate);
    if (candidate.added)
        result.deleted = true;
    return result;
}

bool PDFEditController::headerSyncCanApply() const
{
    return headerSyncAvailable() && m_headerSyncReady && !m_headerSyncLoading
        && !m_headerSyncMatches.isEmpty() && headerSyncHistoryCurrent()
        && !sameDraftContent(m_headerSyncBaseline, currentHeaderSyncDraft());
}

void PDFEditController::cancelHeaderSync()
{
    if (m_headerSyncCancellation)
        m_headerSyncCancellation->store(true, std::memory_order_relaxed);
    m_headerSyncCancellation.reset();
    m_headerSyncRequests.clear();
    m_headerSyncMatches.clear();
    m_headerSyncLoading = false;
    m_headerSyncReady = false;
    m_headerSyncError.clear();
    emit headerSyncChanged();
}

void PDFEditController::resetHeaderSync()
{
    m_headerSyncValid = false;
    m_headerSyncBaseline = Draft();
    m_headerSyncUndoIndex = 0;
    cancelHeaderSync();
}

bool PDFEditController::headerSyncDraftEligible(const Draft &draft, bool includeContentBounds) const
{
    const Candidate &candidate = draft.candidate;
    if (!m_document || !candidate.editable || candidate.headerMetadataInvalid
        || (!candidate.headerGroupId.isEmpty()
            && candidate.headerRole.startsWith(QLatin1String("footer")) != m_footerMode)
        || candidate.textMarkupHitTestOnly || draft.deleted
        || !(candidate.kind == QLatin1String("text")
             || candidate.kind == QLatin1String("addedText")
             || candidate.kind == QLatin1String("image")
             || candidate.kind == QLatin1String("addedImage")))
        return false;
    if (isTextCandidateKind(candidate.kind)
        && (draft.text.isEmpty() || draft.text.size() > maximumTextLength
            || draft.characterSpans.size() > maximumTextLength
            || draft.paragraphFormats.size() > 64))
        return false;
    const QSizeF size = m_document->pageSize(candidate.page);
    if (!qIsFinite(size.width()) || !qIsFinite(size.height()) || size.isEmpty()
        || !validRect(draft.frameRect) || (includeContentBounds && !validRect(draft.contentBounds)))
        return false;
    const QRectF bounds = inMarginPoints(includeContentBounds
        ? draft.frameRect.united(draft.contentBounds) : draft.frameRect, size, m_footerMode);
    return qIsFinite(bounds.x()) && qIsFinite(bounds.y())
        && qIsFinite(bounds.width()) && qIsFinite(bounds.height())
        && bounds.width() > 0 && bounds.height() > 0
        && bounds.left() >= -0.001 && bounds.top() >= -0.001
        && bounds.right() <= size.width() + 0.001
        && bounds.bottom() <= qMin(100.0, size.height()) + 0.001;
}

bool PDFEditController::measureHeaderSyncText(Draft &draft) const
{
    // A resized peer still has its previous content extent until this reflow.
    // Validate the new frame first, then check the measured content below.
    if (!headerSyncDraftEligible(draft, false) || draft.characterSpans.isEmpty())
        return false;
    const QSizeF size = m_document->pageSize(draft.candidate.page);
    qsizetype covered = 0;
    for (const CharacterSpan &span : draft.characterSpans) {
        const CharacterFormat &format = span.format;
        if (span.start != covered || span.length <= 0 || span.length > draft.text.size() - covered
            || format.fontFamily.isEmpty() || !format.color.isValid()
            || !qIsFinite(format.fontSizeRatio) || format.fontSizeRatio <= 0
            || format.fontSizeRatio * size.height() > 512.001
            || !qIsFinite(format.displayFontSizeRatio) || format.displayFontSizeRatio < 0
            || format.displayFontSizeRatio * size.height() > 512.001
            || format.fontStretch < 1 || format.fontStretch > 4000
            || !qIsFinite(format.letterSpacingRatio) || !qIsFinite(format.wordSpacingRatio)
            || !qIsFinite(format.sourceAdvanceRatio)
            || qAbs(format.letterSpacingRatio * size.height()) > 512
            || qAbs(format.wordSpacingRatio * size.height()) > 512
            || qAbs(format.sourceAdvanceRatio * size.height()) > 512)
            return false;
        covered += span.length;
    }
    if (covered != draft.text.size())
        return false;
    const auto face = [&](const CharacterFormat &format) {
        return fontForDisplay(format.fontFamily, format.fontStretch,
            format.fontBold, format.fontItalic, format.fontUnderline,
            format.fontSizeRatio * size.height(),
            (format.letterSpacingRatio + format.sourceAdvanceRatio) * size.height(),
            format.wordSpacingRatio * size.height(), !qFuzzyIsNull(format.sourceAdvanceRatio));
    };
    QRectF frame = inPoints(draft.frameRect, size);
    const QPointF origin = layoutOriginForDraft(draft);
    const bool sourceBaseline = draft.candidate.hasBaseline && !draft.candidate.added;
    const qreal left = sourceBaseline ? origin.x() * size.width() : frame.x();
    // Numbers have different glyph widths. Keep each value on a single line
    // when increasing its size, without extending beyond the page edge.
    if (m_headerSyncPreserveNumbers && !draft.text.contains(QLatin1Char('\n'))) {
        const qreal minimumWidth = QFontMetricsF(face(primaryCharacterFormat(draft)))
            .horizontalAdvance(draft.text) + 1.0;
        frame.setRight(qMax(frame.right(), left + minimumWidth));
        draft.frameRect = fromPoints(frame, size);
    }
    const qreal width = frame.right() - left;
    if (!qIsFinite(left) || left < -0.001 || !qIsFinite(width) || width <= 0)
        return false;
    qreal height = 0.0;
    qreal firstBaseline = -1.0;
    qreal previousAfter = 0.0;
    qsizetype offset = 0;
    const auto paragraphAt = [&draft](qsizetype position) {
        for (const ParagraphFormat &format : draft.paragraphFormats) {
            if (position >= format.start && position <= format.start + format.length)
                return format;
        }
        return draft.paragraphFormats.isEmpty() ? ParagraphFormat() : draft.paragraphFormats.constLast();
    };
    for (const QString &text : draft.text.split(QLatin1Char('\n'))) {
        const ParagraphFormat paragraph = paragraphAt(offset);
        const auto validMargin = [&](qreal ratio) {
            return qIsFinite(ratio) && ratio >= 0.0 && ratio * size.height() <= 144.001;
        };
        if (!qIsFinite(paragraph.lineSpacing) || paragraph.lineSpacing < 0.5
            || paragraph.lineSpacing > 3.0 || !validMargin(paragraph.spaceBeforeRatio)
            || !validMargin(paragraph.spaceAfterRatio) || !validMargin(paragraph.firstLineIndentRatio))
            return false;
        if (offset > 0)
            height += std::floor(qMax(previousAfter, paragraph.spaceBeforeRatio * size.height()) * 64.0) / 64.0;
        QTextLayout layout(text.isEmpty() ? QStringLiteral(" ") : text,
            face(characterFormatAtPosition(draft, offset, false)));
        QList<QTextLayout::FormatRange> formats;
        for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
            const qsizetype start = qMax(offset, span.start);
            const qsizetype end = qMin(offset + text.size(), span.start + span.length);
            if (end <= start)
                continue;
            QTextCharFormat format;
            format.setFont(face(span.format));
            formats.append({int(start - offset), int(end - start), format});
        }
        layout.setFormats(formats);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        switch (paragraph.alignment) {
        case CenterAlignment: option.setAlignment(Qt::AlignHCenter); break;
        case RightAlignment: option.setAlignment(Qt::AlignRight); break;
        case JustifyAlignment: option.setAlignment(Qt::AlignJustify); break;
        default: option.setAlignment(Qt::AlignLeft); break;
        }
        layout.setTextOption(option);
        layout.beginLayout();
        bool first = true;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            const qreal indent = first ? paragraph.firstLineIndentRatio * size.height() : 0.0;
            if (width <= indent) {
                layout.endLayout();
                return false;
            }
            line.setLineWidth(width - indent);
            if (firstBaseline < 0.0)
                firstBaseline = height + line.ascent();
            height += std::floor(line.height() * paragraph.lineSpacing * 64.0) / 64.0;
            if (!qIsFinite(height) || height > 612.0
                || (!text.isEmpty() && line.naturalTextWidth() > width - indent + 0.01)) {
                layout.endLayout();
                return false;
            }
            first = false;
        }
        layout.endLayout();
        previousAfter = paragraph.spaceAfterRatio * size.height();
        offset += text.size() + 1;
    }
    // Match the writer's baseline anchoring and collapsed paragraph margins.
    const qreal top = (sourceBaseline ? origin.y() * size.height() - firstBaseline : frame.y() + 1.0)
        + paragraphAt(0).spaceBeforeRatio * size.height();
    const QRectF content(left, top, width, height + previousAfter);
    if (!validRect(content))
        return false;
    draft.contentBounds = fromPoints(content, size);
    return headerSyncDraftEligible(draft);
}

void PDFEditController::captureHeaderSyncSelection()
{
    if (m_headerSyncApplying)
        return;
    if (!m_headerSelectionOnly || m_tool != SelectTool || m_saving) {
        resetHeaderSync();
        return;
    }
    if (m_headerSyncValid && m_hasSelection
        && m_selectedCandidate.page == m_headerSyncBaseline.candidate.page
        && m_selectedCandidate.objectId == m_headerSyncBaseline.candidate.objectId) {
        emit headerSyncChanged();
        return;
    }
    // Keep an edited header's snapshot after clicking outside its frame, so
    // Save can still propagate the edit even without a visible selection.
    if (m_headerSyncValid && !m_hasSelection) {
        const Draft current = currentHeaderSyncDraft();
        if (draftRepresentsDeletion(current.candidate.kind, current.text, current.deleted)
            || ((m_headerSyncReady || m_headerSyncLoading || !m_headerSyncError.isEmpty())
                && !sameDraftContent(current, m_headerSyncBaseline))) {
            emit headerSyncChanged();
            return;
        }
    }
    resetHeaderSync();
    if (!m_hasSelection)
        return;
    const Draft draft = selectedDraftForDisplay();
    if (!headerSyncDraftEligible(draft))
        return;
    m_headerSyncBaseline = draft;
    m_headerSyncHadBefore = m_drafts.contains(draftKey(draft.candidate.page, draft.candidate.objectId));
    m_headerSyncUndoIndex = m_undoStack.size();
    m_headerSyncValid = true;
    emit headerSyncChanged();
}

bool PDFEditController::headerSyncHistoryCurrent() const
{
    if (!m_headerSyncValid || m_undoStack.size() < m_headerSyncUndoIndex)
        return false;
    const QString key = headerSyncKey();
    for (qsizetype i = m_headerSyncUndoIndex; i < m_undoStack.size(); ++i) {
        const Command &command = m_undoStack.at(i);
        if (command.kind != Command::Kind::ObjectDraft || command.key != key)
            return false;
    }
    return true;
}

void PDFEditController::validateHeaderSyncHistory()
{
    if (m_headerSyncApplying)
        return;
    if (m_headerSyncValid && !headerSyncHistoryCurrent())
        resetHeaderSync();
    else
        emit headerSyncChanged();
}

bool PDFEditController::headerSyncDraftMatches(const Draft &draft, bool &pageNumber) const
{
    pageNumber = false;
    if (!headerSyncDraftEligible(draft))
        return false;
    const Draft &anchor = m_headerSyncBaseline;
    const bool text = isTextCandidateKind(anchor.candidate.kind);
    if (text != isTextCandidateKind(draft.candidate.kind))
        return false;
    // A valid group is authoritative and cannot accidentally absorb nearby
    // external content with identical appearance.
    if (!anchor.candidate.headerGroupId.isEmpty()) {
        pageNumber = isPageNumberRole(anchor.candidate.headerRole);
        return anchor.candidate.headerGroupId == draft.candidate.headerGroupId
            && anchor.candidate.headerRole == draft.candidate.headerRole;
    }
    if (!draft.candidate.headerGroupId.isEmpty())
        return false;
    const QSizeF anchorSize = m_document->pageSize(anchor.candidate.page);
    const QSizeF targetSize = m_document->pageSize(draft.candidate.page);
    const QRectF a = inMarginPoints(anchor.frameRect, anchorSize, m_footerMode);
    const QRectF b = inMarginPoints(draft.frameRect, targetSize, m_footerMode);
    bool romanA = false, romanB = false;
    const int numberA = text ? numberValue(anchor.text, romanA) : 0;
    const int numberB = text ? numberValue(draft.text, romanB) : 0;
    pageNumber = numberA > 0 && numberB > 0 && romanA == romanB
        && numberB - numberA == draft.candidate.page - anchor.candidate.page;
    if (!withinTolerance(a.x(), b.x()) || !withinTolerance(a.y(), b.y()) || !withinTolerance(a.height(), b.height())
        || (!pageNumber && !withinTolerance(a.width(), b.width())))
        return false;
    if (!text)
        return !anchor.candidate.headerImageDigest.isEmpty()
            && anchor.candidate.headerImageDigest == draft.candidate.headerImageDigest
            && anchor.imageQuarterTurns == draft.imageQuarterTurns;
    if (!pageNumber && anchor.text != draft.text)
        return false;
    if (anchor.characterSpans.isEmpty() || draft.characterSpans.isEmpty())
        return false;
    const auto sameFormat = [&](const CharacterFormat &left, const CharacterFormat &right) {
        return sameFontFamily(left.fontFamily, right.fontFamily)
            && withinTolerance(left.fontSizeRatio * anchorSize.height(), right.fontSizeRatio * targetSize.height(), 0.1)
            && left.textShear == right.textShear
            && left.fontBold == right.fontBold && left.fontItalic == right.fontItalic
            && left.fontUnderline == right.fontUnderline && left.color == right.color
            && withinTolerance(left.letterSpacingRatio * anchorSize.height(), right.letterSpacingRatio * targetSize.height(), 0.01)
            && withinTolerance(left.wordSpacingRatio * anchorSize.height(), right.wordSpacingRatio * targetSize.height(), 0.01)
            && left.fontStretch == right.fontStretch;
    };
    if (anchor.paragraphFormats.size() != draft.paragraphFormats.size())
        return false;
    for (qsizetype index = 0; index < anchor.paragraphFormats.size(); ++index) {
        const ParagraphFormat &aParagraph = anchor.paragraphFormats.at(index);
        const ParagraphFormat &bParagraph = draft.paragraphFormats.at(index);
        if (aParagraph.alignment != bParagraph.alignment
            || !withinTolerance(aParagraph.lineSpacing, bParagraph.lineSpacing, 0.001)
            || !withinTolerance(aParagraph.spaceBeforeRatio * anchorSize.height(), bParagraph.spaceBeforeRatio * targetSize.height(), 0.1)
            || !withinTolerance(aParagraph.spaceAfterRatio * anchorSize.height(), bParagraph.spaceAfterRatio * targetSize.height(), 0.1)
            || !withinTolerance(aParagraph.firstLineIndentRatio * anchorSize.height(), bParagraph.firstLineIndentRatio * targetSize.height(), 0.1)
            || aParagraph.listStyle != bParagraph.listStyle || aParagraph.listLevel != bParagraph.listLevel
            || aParagraph.listStart != bParagraph.listStart)
            return false;
    }
    if (pageNumber)
        return anchor.characterSpans.size() == 1 && draft.characterSpans.size() == 1
            && sameFormat(anchor.characterSpans.first().format, draft.characterSpans.first().format);
    qsizetype left = 0, right = 0;
    while (left < anchor.characterSpans.size() && right < draft.characterSpans.size()) {
        const CharacterSpan &aSpan = anchor.characterSpans.at(left);
        const CharacterSpan &bSpan = draft.characterSpans.at(right);
        if (!sameFormat(aSpan.format, bSpan.format))
            return false;
        const qsizetype aEnd = aSpan.start + aSpan.length;
        const qsizetype bEnd = bSpan.start + bSpan.length;
        if (aEnd <= bEnd) ++left;
        if (bEnd <= aEnd) ++right;
    }
    return left == anchor.characterSpans.size() && right == draft.characterSpans.size();
}

void PDFEditController::collectHeaderMatches(QVector<Candidate> candidates, int page)
{
    QVector<Draft> matches;
    bool preservesNumber = false;
    for (const Candidate &candidate : std::as_const(m_addedCandidates)) {
        if (candidate.page == page)
            candidates.append(candidate);
    }
    for (const Candidate &candidate : std::as_const(candidates)) {
        const auto existing = m_drafts.constFind(draftKey(page, candidate.objectId));
        const Draft draft = existing == m_drafts.cend() ? initialDraftForCandidate(candidate) : *existing;
        bool pageNumber = false;
        if (headerSyncDraftMatches(draft, pageNumber)) {
            matches.append(draft);
            preservesNumber = preservesNumber || pageNumber;
            if (matches.size() > 1)
                return; // Ambiguity never picks an arbitrary object on this page.
        }
    }
    if (matches.size() != 1)
        return;
    m_headerSyncRetainedBytes += estimatedCommentCandidateBytes(matches.first().candidate)
        + matches.first().text.size() * sizeof(QChar)
        + matches.first().characterSpans.size() * sizeof(CharacterSpan);
    if (m_headerSyncRetainedBytes > maximumRetainedBytes) {
        cancelHeaderSync();
        m_headerSyncError = (m_footerMode
            //% "There are too many matching footers. Narrow the document before trying again."
            ? qtTrId("FooterSyncLimit")
            //% "There are too many matching headers. Narrow the document before trying again."
            : qtTrId("HeaderSyncLimit"));
        emit headerSyncChanged();
        return;
    }
    m_headerSyncPreserveNumbers = m_headerSyncPreserveNumbers || preservesNumber;
    m_headerSyncMatches.append(matches.first());
}

void PDFEditController::findSimilarHeaders()
{
    if (!headerSyncAvailable())
        return;
    cancelHeaderSync();
    if (!headerSyncHistoryCurrent()) {
        resetHeaderSync();
        return;
    }
    if (m_document->pageCount() > maximumPages) {
        m_headerSyncError = (m_footerMode
            //% "There are too many matching footers. Narrow the document before trying again."
            ? qtTrId("FooterSyncLimit")
            //% "There are too many matching headers. Narrow the document before trying again."
            : qtTrId("HeaderSyncLimit"));
        emit headerSyncChanged();
        return;
    }
    m_headerSyncRetainedBytes = 0;
    m_headerSyncRequestIndex = 0;
    m_headerSyncPreserveNumbers = isPageNumberRole(m_headerSyncBaseline.candidate.headerRole);
    m_headerSyncCancellation = std::make_shared<std::atomic_bool>(false);
    m_headerSyncLoading = true;
    for (int page = 0; page < m_document->pageCount(); ++page) {
        if (!m_headerSyncLoading)
            break;
        if (page == m_headerSyncBaseline.candidate.page)
            continue;
        CandidateScanRequest request = candidateScanRequest(page);
        if (m_document->isTransientBlankPage(page)) {
            collectHeaderMatches({}, page);
        } else if (request.page >= 0) {
            m_headerSyncRequests.append(std::move(request));
        }
    }
    emit headerSyncChanged();
    scanNextHeaderPage();
}

void PDFEditController::scanNextHeaderPage()
{
    if (!m_headerSyncLoading || !m_headerSyncCancellation)
        return;
    if (m_headerSyncRequestIndex >= m_headerSyncRequests.size()) {
        m_headerSyncLoading = false;
        m_headerSyncReady = true;
        m_headerSyncRequests.clear();
        if (m_headerSyncMatches.isEmpty()) {
            m_headerSyncError = (m_footerMode
                //% "No unambiguous matching footers were found on other pages."
                ? qtTrId("FooterSyncNoMatches")
                //% "No unambiguous matching headers were found on other pages."
                : qtTrId("HeaderSyncNoMatches"));
        }
        emit headerSyncChanged();
        return;
    }
    const CandidateScanRequest request = m_headerSyncRequests.at(m_headerSyncRequestIndex++);
    const auto cancellation = m_headerSyncCancellation;
    const QPointer<PDFEditController> controller(this);
    headerScanPool().start(QRunnable::create([controller, request, cancellation]() {
        QVector<Candidate> candidates = scanPageFromFile(request, cancellation);
        QObject *app = QCoreApplication::instance();
        if (!app || cancellation->load(std::memory_order_relaxed))
            return;
        QMetaObject::invokeMethod(app, [controller, request, cancellation,
                                      candidates = std::move(candidates)]() mutable {
            if (!controller || controller->m_headerSyncCancellation != cancellation
                || cancellation->load(std::memory_order_relaxed))
                return;
            PDFDocument *document = controller->m_document;
            if (!document || !document->isLoaded() || document->renderRevision() != request.revision
                || document->renderFilePath() != request.filePath
                || document->sourcePageIndex(request.page) != request.sourcePage
                || document->pageSize(request.page) != request.pageSize) {
                controller->resetHeaderSync();
                return;
            }
            candidates = controller->finalizePageCandidates(std::move(candidates), request.page, request.pageSize);
            controller->collectHeaderMatches(std::move(candidates), request.page);
            controller->scanNextHeaderPage();
        }, Qt::QueuedConnection);
    }));
}

bool PDFEditController::applyHeaderChanges(const QString &pages)
{
    finishSelectedFrameResize();
    finishSelectedTextEdit();
    if (!headerSyncCanApply() || !documentAllowsEditing())
        return false;
    QSet<int> pageFilter;
    if (!selectedPages(pages, m_document->pageCount(), pageFilter)) {
        m_headerSyncError = (m_footerMode
            //% "Enter valid PDF pages, for example 1-3, 5."
            ? qtTrId("FooterSyncInvalidPages")
            //% "Enter valid PDF pages, for example 1-3, 5."
            : qtTrId("HeaderSyncInvalidPages"));
        emit headerSyncChanged();
        return false;
    }
    const bool all = pages.trimmed().isEmpty();
    Draft anchor = currentHeaderSyncDraft();
    const bool deleting = draftRepresentsDeletion(anchor.candidate.kind, anchor.text, anchor.deleted);
    const QSizeF anchorSize = m_document->pageSize(anchor.candidate.page);
    const QRectF oldFrame = inMarginPoints(m_headerSyncBaseline.frameRect, anchorSize, m_footerMode);
    QRectF newFrame = inMarginPoints(anchor.frameRect, anchorSize, m_footerMode);
    const bool contentChanged = anchor.text != m_headerSyncBaseline.text;
    const bool formatChanged = !sameCharacterSpans(anchor.characterSpans, m_headerSyncBaseline.characterSpans)
        || !sameParagraphFormats(anchor.paragraphFormats, m_headerSyncBaseline.paragraphFormats);
    const bool frameResized = !withinTolerance(newFrame.width(), oldFrame.width(), 0.001)
        || !withinTolerance(newFrame.height(), oldFrame.height(), 0.001);
    const auto fail = [this]() {
        m_headerSyncError = (m_footerMode
            //% "A matching footer changed or the result does not fit inside the 100pt footer. Find matches again or adjust the edit."
            ? qtTrId("FooterSyncCannotApply")
            //% "A matching header changed or the result does not fit inside the 100pt header. Find matches again or adjust the edit."
            : qtTrId("HeaderSyncCannotApply"));
        emit headerSyncChanged();
        return false;
    };
    // A number group copies appearance and placement only. An edited number
    // remains a local edit until the user restores its original value.
    if (!deleting && m_headerSyncPreserveNumbers && contentChanged) {
        m_headerSyncError = (m_footerMode
            //% "Page numbers keep their own values. Restore this number before applying formatting or position changes."
            ? qtTrId("FooterSyncNumberContent")
            //% "Page numbers keep their own values. Restore this number before applying formatting or position changes."
            : qtTrId("HeaderSyncNumberContent"));
        emit headerSyncChanged();
        return false;
    }
    const bool reflowAnchor = isTextCandidateKind(anchor.candidate.kind)
        && (contentChanged || formatChanged || frameResized);
    if (!deleting && !(reflowAnchor ? measureHeaderSyncText(anchor) : headerSyncDraftEligible(anchor)))
        return fail();
    newFrame = inMarginPoints(anchor.frameRect, anchorSize, m_footerMode);
    qsizetype textWork = anchor.text.size();
    Command command;
    command.kind = Command::Kind::HeaderDraftBatch;
    const auto anchorEntry = m_drafts.constFind(headerSyncKey());
    command.changedDrafts.append(DraftChange{headerSyncKey(), anchor.candidate.page,
        anchor.candidate.objectId, m_headerSyncHadBefore, m_headerSyncBaseline,
        anchorEntry != m_drafts.cend(), anchor});
    for (const Draft &before : std::as_const(m_headerSyncMatches)) {
        const int page = before.candidate.page;
        if (!all && !pageFilter.contains(page))
            continue;
        const QString key = draftKey(page, before.candidate.objectId);
        const auto existing = m_drafts.constFind(key);
        const Draft current = existing == m_drafts.cend()
            ? initialDraftForCandidate(before.candidate) : *existing;
        if (!sameDraftContent(current, before)
            || (before.candidate.added && existing == m_drafts.cend()))
            return fail();
        Draft after = before;
        if (deleting) {
            if (isTextCandidateKind(before.candidate.kind))
                setDraftText(after, QString());
            after.deleted = true;
        } else {
            const QSizeF size = m_document->pageSize(page);
            if (size.isEmpty())
                return fail();
            const QRectF previous = inMarginPoints(before.frameRect, size, m_footerMode);
            const bool text = isTextCandidateKind(before.candidate.kind);
            // Group members can retain different saved layout widths. Copy the
            // edited text's frame size so identical content wraps identically.
            const bool copyTextLayout = text && !m_headerSyncPreserveNumbers
                && (contentChanged || formatChanged || frameResized);
            const QSizeF targetSize = copyTextLayout ? newFrame.size()
                : QSizeF(previous.width() + newFrame.width() - oldFrame.width(),
                         previous.height() + newFrame.height() - oldFrame.height());
            QPointF targetOrigin = previous.topLeft() + newFrame.topLeft() - oldFrame.topLeft();
            // A saved Header group may contain members moved independently.
            // Moving the group sets the edited coordinates, rather than adding
            // its old offsets again and pushing those members out of the header.
            if (!anchor.candidate.headerGroupId.isEmpty()) {
                if (!withinTolerance(newFrame.x(), oldFrame.x(), 0.001))
                    targetOrigin.setX(newFrame.x());
                if (!withinTolerance(newFrame.y(), oldFrame.y(), 0.001))
                    targetOrigin.setY(newFrame.y());
            }
            const QRectF target(targetOrigin, targetSize);
            after.frameRect = fromMarginPoints(target, size, m_footerMode);
            after.contentBounds = before.contentBounds.translated(after.frameRect.topLeft() - before.frameRect.topLeft());
            if (text && (contentChanged || formatChanged)) {
                const qreal scale = anchorSize.height() / size.height();
                if (!m_headerSyncPreserveNumbers)
                    after.text = anchor.text;
                after.characterSpans = anchor.characterSpans;
                after.paragraphFormats = anchor.paragraphFormats;
                after.sourceLineBreaks = anchor.sourceLineBreaks;
                if (m_headerSyncPreserveNumbers) {
                    // Retain each number's own embedded face and calibration
                    // unless that property was actually changed on the anchor.
                    // A subset font containing "i" may not contain Arabic digits.
                    CharacterFormat format = primaryCharacterFormat(before);
                    const CharacterFormat oldFormat = primaryCharacterFormat(m_headerSyncBaseline);
                    const CharacterFormat newFormat = primaryCharacterFormat(anchor);
                    if (oldFormat.fontFamily != newFormat.fontFamily)
                        format.fontFamily = newFormat.fontFamily;
                    if (oldFormat.fontStretch != newFormat.fontStretch)
                        format.fontStretch = newFormat.fontStretch;
                    if (oldFormat.fontBold != newFormat.fontBold)
                        format.fontBold = newFormat.fontBold;
                    if (oldFormat.textShear != newFormat.textShear)
                        format.textShear = newFormat.textShear;
                    if (oldFormat.fontItalic != newFormat.fontItalic)
                        format.fontItalic = newFormat.fontItalic;
                    if (oldFormat.fontUnderline != newFormat.fontUnderline)
                        format.fontUnderline = newFormat.fontUnderline;
                    if (oldFormat.color != newFormat.color)
                        format.color = newFormat.color;
                    format.fontSizeRatio = newFormat.fontSizeRatio;
                    format.displayFontSizeRatio = newFormat.displayFontSizeRatio;
                    format.letterSpacingRatio = newFormat.letterSpacingRatio;
                    format.wordSpacingRatio = newFormat.wordSpacingRatio;
                    after.characterSpans = {{0, after.text.size(), format}};
                    after.paragraphFormats = paragraphFormatsForText(after.text,
                        anchor.paragraphFormats.isEmpty() ? ParagraphFormat() : anchor.paragraphFormats.first());
                    after.sourceLineBreaks.clear();
                }
                for (CharacterSpan &span : after.characterSpans) {
                    span.format.fontSizeRatio *= scale;
                    span.format.displayFontSizeRatio *= scale;
                    span.format.letterSpacingRatio *= scale;
                    span.format.wordSpacingRatio *= scale;
                    span.format.sourceAdvanceRatio = contentChanged || m_headerSyncPreserveNumbers
                        ? 0.0 : span.format.sourceAdvanceRatio * scale;
                }
                for (ParagraphFormat &paragraph : after.paragraphFormats) {
                    paragraph.spaceBeforeRatio *= scale;
                    paragraph.spaceAfterRatio *= scale;
                    paragraph.firstLineIndentRatio *= scale;
                }
                applyUnicodeFontFallback(after);
            }
            if (!text) {
                if ((target.size() != previous.size() || anchor.imageQuarterTurns != m_headerSyncBaseline.imageQuarterTurns)
                    && !before.candidate.added && !before.candidate.imageResizable)
                    return fail();
                after.imageQuarterTurns = (before.imageQuarterTurns + anchor.imageQuarterTurns
                    - m_headerSyncBaseline.imageQuarterTurns + 4) % 4;
                after.contentBounds = after.frameRect;
            } else if (contentChanged || formatChanged
                || !withinTolerance(target.width(), previous.width(), 0.001)
                || !withinTolerance(target.height(), previous.height(), 0.001)) {
                // Validate every reflow before making the batch visible.
                if (!measureHeaderSyncText(after))
                    return fail();
            }
            if (!headerSyncDraftEligible(after))
                return fail();
        }
        textWork += after.text.size();
        if (textWork > maximumBatchTextLength)
            return fail();
        const bool hadAfter = before.candidate.added ? !deleting
            : !sameDraftContent(after, initialDraftForCandidate(before.candidate));
        command.changedDrafts.append(DraftChange{key, page, before.candidate.objectId,
            existing != m_drafts.cend(), before, hadAfter, after});
    }
    if (command.changedDrafts.size() < 2) {
        m_headerSyncError = (m_footerMode
            //% "The selected range contains no matching footers on other pages."
            ? qtTrId("FooterSyncEmptyRange")
            //% "The selected range contains no matching headers on other pages."
            : qtTrId("HeaderSyncEmptyRange"));
        emit headerSyncChanged();
        return false;
    }
    m_headerSyncApplying = true;
    m_undoStack.resize(m_headerSyncUndoIndex);
    m_undoStack.append(command);
    m_redoStack.clear();
    applyHeaderDraftBatch(command, true, true);
    resetHeaderSync();
    m_headerSyncApplying = false;
    captureHeaderSyncSelection();
    emit historyStateChanged();
    return true;
}

void PDFEditController::applyHeaderDraftBatch(const Command &command, bool useAfterState, bool immediate)
{
    QSet<int> pages;
    bool clearSelected = false;
    for (const DraftChange &change : command.changedDrafts) {
        const bool exists = useAfterState ? change.hadAfter : change.hadBefore;
        const Draft &draft = useAfterState ? change.after : change.before;
        const Candidate &candidate = draft.candidate;
        if (exists) {
            m_drafts.insert(change.key, draft);
            if (candidate.added) {
                const bool present = std::any_of(m_addedCandidates.cbegin(), m_addedCandidates.cend(),
                    [&](const Candidate &item) { return item.page == candidate.page && item.objectId == candidate.objectId; });
                if (!present) {
                    const auto position = std::lower_bound(m_addedCandidates.begin(), m_addedCandidates.end(),
                        candidate.addedOrder, [](const Candidate &item, quint64 order) { return item.addedOrder < order; });
                    m_addedCandidates.insert(position, candidate);
                }
            }
        } else {
            m_drafts.remove(change.key);
            if (candidate.added)
                removeAddedCandidate(candidate.page, candidate.objectId);
        }
        if (m_hasSelection && m_selectedCandidate.page == change.page && m_selectedCandidate.objectId == change.objectId)
            clearSelected = (candidate.added && !exists)
                || (exists && draftRepresentsDeletion(candidate.kind, draft.text, draft.deleted));
        pages.insert(change.page);
    }
    if (clearSelected)
        setSelection(nullptr);
    else
        updateSelectedTextFromDraft();
    QList<int> ordered = pages.values();
    std::sort(ordered.begin(), ordered.end());
    for (int page : ordered)
        markDraftStateChanged(page, immediate, false);
}
