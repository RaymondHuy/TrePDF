#include "PDFEditController.h"

#include "PDFDocument.h"

#include <QFontMetricsF>
#include <QTextBoundaryFinder>

#include <cmath>
#include <utility>

namespace {

constexpr qsizetype kMaximumProjectedCharacters = 32768;
constexpr qreal kMaximumVirtualGapPoints = 512.0;

bool isParagraphSeparator(QChar character)
{
    return character == QLatin1Char('\n') || character == QLatin1Char('\r')
        || character == QChar::ParagraphSeparator || character == QChar::LineSeparator;
}

QVector<bool> graphemeBoundaries(const QString &text)
{
    QVector<bool> boundaries(text.size() + 1, false);
    boundaries[0] = true;
    boundaries[text.size()] = true;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    for (qsizetype position = finder.toNextBoundary(); position >= 0;
         position = finder.toNextBoundary()) {
        boundaries[position] = true;
    }
    return boundaries;
}

template<typename Draft, typename Format>
QVector<Format> characterFormats(const Draft &draft, const Format &fallback)
{
    QVector<Format> formats(draft.text.size(), fallback);
    for (const auto &span : draft.characterSpans) {
        const qsizetype start = qBound<qsizetype>(0, span.start, draft.text.size());
        const qsizetype end = qBound<qsizetype>(start, span.start + span.length,
                                              draft.text.size());
        for (qsizetype position = start; position < end; ++position)
            formats[position] = span.format;
    }
    return formats;
}

// Both directions use UTF-16 boundary maps, including the end-of-text boundary.
// Paragraph lengths exclude their newline; sourceLineBreaks identify the newline
// itself. Mapping those boundaries preserves both conventions without counting
// spaces or guessing positions from the resulting string.
template<typename Draft>
void remapTextPositions(Draft &draft, const QVector<qsizetype> &boundaries)
{
    const qsizetype lastBoundary = boundaries.size() - 1;
    for (auto &paragraph : draft.paragraphFormats) {
        const qsizetype start = qBound<qsizetype>(0, paragraph.start, lastBoundary);
        const qsizetype end = qBound<qsizetype>(start, paragraph.start + paragraph.length,
                                              lastBoundary);
        paragraph.start = boundaries.at(start);
        paragraph.length = boundaries.at(end) - paragraph.start;
    }
    for (qsizetype &position : draft.sourceLineBreaks)
        position = boundaries.at(qBound<qsizetype>(0, position, lastBoundary));
}

} // namespace

void PDFEditController::prepareEditorVirtualSpaces()
{
    if (m_editorVirtualSpacesPrepared || !m_textEditActive
        || !m_textEditDisplayDraftActive || !m_document || !m_hasSelection) {
        return;
    }
    m_editorVirtualSpacesPrepared = true;
    const Draft &source = m_textEditDisplayDraft;
    if (source.text.isEmpty() || source.text.size() >= kMaximumProjectedCharacters)
        return;

    const qreal pageHeight = m_document->pageSize(source.candidate.page).height();
    if (!qIsFinite(pageHeight) || pageHeight <= 0.0)
        return;
    const QVector<CharacterFormat> formats = characterFormats(source,
                                                               primaryCharacterFormat(source));
    for (const CharacterFormat &format : formats) {
        // A reload of a partially edited projection must not subdivide its
        // remaining gaps again (for example after changing the font family).
        if (format.virtualSpaceRatio > 0.0)
            return;
    }

    const QVector<bool> graphemes = graphemeBoundaries(source.text);
    QVector<qsizetype> boundaries(source.text.size() + 1, 0);
    Draft projected = source;
    projected.text.clear();
    projected.characterSpans.clear();
    const auto append = [&](QChar character, const CharacterFormat &format) {
        const qsizetype position = projected.text.size();
        projected.text.append(character);
        if (!projected.characterSpans.isEmpty()
            && sameCharacterFormat(projected.characterSpans.constLast().format, format)) {
            ++projected.characterSpans.last().length;
        } else {
            projected.characterSpans.append(CharacterSpan{position, 1, format});
        }
    };

    bool expanded = false;
    for (qsizetype start = 0; start < source.text.size();) {
        qsizetype end = start + 1;
        while (end < source.text.size() && !graphemes.at(end))
            ++end;
        CharacterFormat format = formats.at(start);
        // A retained gap can become trailing after the user deletes the next
        // character or inserts a paragraph break. Keep it traversable on the
        // next edit, just like a gap between two visible characters.
        bool eligible = !isParagraphSeparator(source.text.at(start))
            && qIsFinite(format.sourceAdvanceRatio) && format.sourceAdvanceRatio > 0.0
            && format.sourceAdvanceRatio * pageHeight <= kMaximumVirtualGapPoints;
        for (qsizetype position = start + 1; eligible && position < end; ++position)
            eligible = sameCharacterFormat(format, formats.at(position));

        qreal stepRatio = 0.0;
        qsizetype steps = 0;
        if (eligible) {
            const qreal sizeRatio = format.displayFontSizeRatio > 0.0
                ? format.displayFontSizeRatio : format.fontSizeRatio;
            const QFont font = fontForDisplay(format.fontFamily, format.fontStretch,
                format.fontBold, format.fontItalic, format.fontUnderline,
                sizeRatio * pageHeight, 0.0, 0.0, true);
            stepRatio = QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')) / pageHeight;
            if (qIsFinite(stepRatio) && stepRatio > 0.0
                && format.sourceAdvanceRatio >= 2.0 * stepRatio) {
                const qreal count = std::ceil(format.sourceAdvanceRatio / stepRatio);
                const qsizetype available = kMaximumProjectedCharacters
                    - projected.text.size() - (source.text.size() - start);
                if (qIsFinite(count) && count <= available)
                    steps = static_cast<qsizetype>(count);
            }
        }

        const qreal gapRatio = format.sourceAdvanceRatio;
        if (steps > 0)
            format.sourceAdvanceRatio = 0.0;
        for (qsizetype position = start; position < end; ++position) {
            boundaries[position] = projected.text.size();
            append(source.text.at(position), steps > 0 ? format : formats.at(position));
        }
        if (steps > 0) {
            CharacterFormat virtualFormat = format;
            qreal accumulated = 0.0;
            for (qsizetype step = 0; step < steps; ++step) {
                const qreal remaining = gapRatio - accumulated;
                // ceil(gap / step) can overcount an exact multiple by one ULP.
                // A zero-width U+0020 would have no marker and become a real
                // PDF space. Fold rounding dust into the last positive cell;
                // collapsing those same widths recovers the exact source gap.
                if (remaining <= 0.0)
                    break;
                virtualFormat.virtualSpaceRatio = step + 1 == steps
                        || remaining <= stepRatio * (1.0 + 1e-9)
                    ? remaining : stepRatio;
                accumulated += virtualFormat.virtualSpaceRatio;
                append(QLatin1Char(' '), virtualFormat);
            }
            expanded = true;
        }
        boundaries[end] = projected.text.size();
        start = end;
    }
    if (!expanded)
        return;

    remapTextPositions(projected, boundaries);
    const qsizetype selectionStart = qBound<qsizetype>(0, m_selectedTextRangeStart,
                                                       source.text.size());
    const qsizetype selectionEnd = qBound<qsizetype>(selectionStart,
        m_selectedTextRangeStart + m_selectedTextRangeLength, source.text.size());
    m_selectedTextRangeStart = boundaries.at(selectionStart);
    m_selectedTextRangeLength = boundaries.at(selectionEnd) - m_selectedTextRangeStart;
    m_textEditDisplayDraft = std::move(projected);
    m_selectedText = m_textEditDisplayDraft.text;
    emit selectionChanged();
}

PDFEditController::Draft PDFEditController::collapseEditorVirtualSpaces(const Draft &draft)
{
    bool marked = false;
    for (const CharacterSpan &span : draft.characterSpans)
        marked = marked || span.format.virtualSpaceRatio > 0.0;
    if (!marked)
        return draft;
    const QVector<CharacterFormat> formats = characterFormats(draft,
                                                               primaryCharacterFormat(draft));
    bool hasVirtualSpaces = false;
    for (qsizetype position = 0; position < draft.text.size(); ++position) {
        if (draft.text.at(position) == QLatin1Char(' ')
            && qIsFinite(formats.at(position).virtualSpaceRatio)
            && formats.at(position).virtualSpaceRatio > 0.0) {
            hasVirtualSpaces = true;
            break;
        }
    }
    if (!hasVirtualSpaces)
        return draft;

    Draft collapsed = draft;
    collapsed.text.clear();
    collapsed.characterSpans.clear();
    QVector<CharacterFormat> semanticFormats;
    semanticFormats.reserve(draft.text.size());
    QVector<qsizetype> boundaries(draft.text.size() + 1, 0);
    const QVector<bool> graphemes = graphemeBoundaries(draft.text);
    QHash<qsizetype, qreal> leadingGaps;
    qsizetype paragraphStart = 0;
    qsizetype previousClusterStart = -1;
    for (qsizetype position = 0; position < draft.text.size(); ++position) {
        boundaries[position] = collapsed.text.size();
        const QChar character = draft.text.at(position);
        CharacterFormat format = formats.at(position);
        if (character == QLatin1Char(' ') && qIsFinite(format.virtualSpaceRatio)
            && format.virtualSpaceRatio > 0.0) {
            if (previousClusterStart >= paragraphStart) {
                for (qsizetype index = previousClusterStart; index < semanticFormats.size(); ++index)
                    semanticFormats[index].sourceAdvanceRatio += format.virtualSpaceRatio;
            } else {
                // Deleting the anchor at a paragraph start must keep the
                // remaining gap, without introducing a real PDF space glyph.
                leadingGaps[paragraphStart] += format.virtualSpaceRatio;
            }
            continue;
        }

        format.virtualSpaceRatio = 0.0;
        if (graphemes.at(position) || previousClusterStart < paragraphStart)
            previousClusterStart = collapsed.text.size();
        collapsed.text.append(character);
        semanticFormats.append(format);
        if (isParagraphSeparator(character)) {
            paragraphStart = collapsed.text.size();
            previousClusterStart = -1;
        }
    }
    boundaries[draft.text.size()] = collapsed.text.size();
    remapTextPositions(collapsed, boundaries);
    if (!leadingGaps.isEmpty()) {
        if (collapsed.paragraphFormats.isEmpty())
            collapsed.paragraphFormats = paragraphFormatsForText(collapsed.text, ParagraphFormat());
        for (ParagraphFormat &paragraph : collapsed.paragraphFormats)
            paragraph.firstLineIndentRatio += leadingGaps.value(paragraph.start);
    }

    for (qsizetype position = 0; position < semanticFormats.size(); ++position) {
        const CharacterFormat &format = semanticFormats.at(position);
        if (!collapsed.characterSpans.isEmpty()
            && sameCharacterFormat(collapsed.characterSpans.constLast().format, format)) {
            ++collapsed.characterSpans.last().length;
        } else {
            collapsed.characterSpans.append(CharacterSpan{position, 1, format});
        }
    }
    if (collapsed.characterSpans.isEmpty()) {
        CharacterFormat format = primaryCharacterFormat(draft);
        format.virtualSpaceRatio = 0.0;
        format.sourceAdvanceRatio = 0.0;
        collapsed.characterSpans.append(CharacterSpan{0, 0, format});
    }
    if (collapsed.text == draft.candidate.text) {
        const Draft original = initialDraftForCandidate(draft.candidate);
        const QVector<CharacterFormat> originalFormats = characterFormats(
            original, primaryCharacterFormat(original));
        bool unchangedFormats = originalFormats.size() == semanticFormats.size();
        for (qsizetype position = 0; unchangedFormats && position < semanticFormats.size();
             ++position) {
            unchangedFormats = sameCharacterFormat(semanticFormats.at(position),
                                                   originalFormats.at(position));
        }
        // Projection is free to coalesce equivalent neighboring spans, but a
        // no-op edit must not make the source dirty solely because its span
        // boundaries were more granular than the editor's representation.
        if (unchangedFormats)
            collapsed.characterSpans = original.characterSpans;
    }
    return collapsed;
}
