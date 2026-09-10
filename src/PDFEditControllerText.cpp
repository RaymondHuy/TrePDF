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

// Keep PDF sizes in QTextDocument history independently of rounded display
// pixels, so local undo at any zoom restores the exact source size.
constexpr int kEditorFontSizeRatio = QTextFormat::UserProperty + 1;

constexpr int kEditorDisplayFontSizeRatio = QTextFormat::UserProperty + 2;

// Qt adds the insertion length and following format length when calculating
// the redo cursor. A final custom item makes it use the changed text bounds
// instead, without adding another user-visible undo step.
class EditorTypingCursorBoundary final : public QAbstractUndoItem
{
public:
    void undo() override {}
    void redo() override {}
};

// Native key insertion can be a single QTextDocument command, which
// joinPreviousEditBlock() cannot join. Start its block before Qt handles the
// input, then close it after the event so insertion and typing format undo together.
class EditorTypingEditBlock final : public QObject
{
public:
    EditorTypingEditBlock(QQuickTextDocument *document, std::function<bool()> enabled)
        : QObject(document), m_document(document), m_enabled(std::move(enabled))
    {
        setObjectName(QStringLiteral("trepdfTypingEditBlock"));
        if (document->parent()) {
            document->parent()->installEventFilter(this);
        }
    }

    ~EditorTypingEditBlock() override
    {
        finish();
    }

protected:
    bool eventFilter(QObject *editor, QEvent *event) override
    {
        bool insertsText = false;
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            insertsText = !key->text().isEmpty()
                && !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier));
        } else if (event->type() == QEvent::InputMethod) {
            insertsText = !static_cast<QInputMethodEvent *>(event)->commitString().isEmpty();
        }
        if (insertsText && m_enabled() && m_cursor.isNull()
            && m_document->textDocument()) {
            m_cursor = QTextCursor(m_document->textDocument());
            m_cursor.setPosition(qBound(0, editor->property("cursorPosition").toInt(),
                                         m_document->textDocument()->characterCount() - 1));
            m_cursor.beginEditBlock();
            QMetaObject::invokeMethod(this, [this]() { finish(); }, Qt::QueuedConnection);
        }
        return false;
    }

private:
    void finish()
    {
        if (!m_cursor.isNull()) {
            QTextCursor cursor = std::exchange(m_cursor, QTextCursor());
            cursor.endEditBlock();
        }
    }

    QQuickTextDocument *m_document;
    std::function<bool()> m_enabled;
    QTextCursor m_cursor;
};

constexpr qreal kInitialTextFrameSafetyPoints = 4.0;

constexpr qreal kTextEditMinimumPageMarginPoints = 12.0;

constexpr qreal kTextEditMaximumPageMarginRatio = 0.12;

constexpr qreal kTextEditMinimumExpansionPoints = 2.0;

constexpr qreal kTextEditRowToleranceRatio = 0.35;

bool equivalentFontFamilyName(const QString &left, const QString &right)
{
    const QString leftKey = fontFamilyMatchKey(left);
    return sameFontFamily(left, right)
        || (!leftKey.isEmpty() && leftKey == fontFamilyMatchKey(right));
}

QString matchingKnownFontFamilyName(const QString &family,
                                    const QStringList &knownFamilies)
{
    for (const QString &knownFamily : knownFamilies) {
        if (family.compare(knownFamily, Qt::CaseInsensitive) == 0) {
            return knownFamily;
        }
    }

    const QString key = fontFamilyMatchKey(family);
    for (const QString &knownFamily : knownFamilies) {
        if (!key.isEmpty() && key == fontFamilyMatchKey(knownFamily)) {
            return knownFamily;
        }
    }

    const QStringList nameParts = family.split(
        QLatin1Char('-'),
        Qt::SkipEmptyParts);
    for (const QString &namePart : nameParts) {
        const QString partKey = fontFamilyMatchKey(namePart);
        for (const QString &knownFamily : knownFamilies) {
            if (!partKey.isEmpty()
                && partKey == fontFamilyMatchKey(knownFamily)) {
                return knownFamily;
            }
        }
    }
    return {};
}

QString matchingBundledFamilyName(const QString &family)
{
    return matchingKnownFontFamilyName(
        family,
        ApplicationFonts::bundledFontFamilies());
}

QString matchingSystemFamilyName(const QString &family)
{
    return matchingKnownFontFamilyName(
        family,
        ApplicationFonts::systemFontFamilies());
}

bool isArialCompatibleFontFamily(const QString &family)
{
    const auto isCompatibleKey = [](const QString &key) {
        return key.contains(QStringLiteral("arial"))
            || key.contains(QStringLiteral("helvetica"))
            || key == QLatin1String("sans")
            || key == QLatin1String("sansserif");
    };
    if (isCompatibleKey(fontFamilyMatchKey(family))) {
        return true;
    }
    const QStringList nameParts = family.split(
        QLatin1Char('-'),
        Qt::SkipEmptyParts);
    return std::any_of(
        nameParts.cbegin(),
        nameParts.cend(),
        [&isCompatibleKey](const QString &namePart) {
            return isCompatibleKey(fontFamilyMatchKey(namePart));
        });
}

bool isGenericArialCompatibleFontFamily(const QString &family)
{
    const QString key = fontFamilyMatchKey(family);
    return key == QLatin1String("arial")
        || key == QLatin1String("helvetica")
        || key == QLatin1String("sans")
        || key == QLatin1String("sansserif");
}

bool containsNonAscii(const QString &text)
{
    return std::any_of(
        text.cbegin(),
        text.cend(),
        [](QChar character) {
            return character.unicode() > 0x7f;
        });
}

} // namespace

QStringList PDFEditController::bundledFontFamilies() const
{
    return ApplicationFonts::bundledFontFamilies();
}

QStringList PDFEditController::availableFontFamilies() const
{
    return ApplicationFonts::availableFontFamilies(
        selectedFontBold(),
        selectedFontItalic());
}

QStringList PDFEditController::fontFamiliesForFilter(FontFilter filter) const
{
    struct FontRequirement {
        bool bold = false;
        bool italic = false;
    };

    QVector<FontRequirement> requirements;
    const auto appendRequirement = [&requirements](
                                       const CharacterFormat &format) {
        const auto existing = std::find_if(
            requirements.begin(),
            requirements.end(),
            [&format](const FontRequirement &requirement) {
                return requirement.bold == format.fontBold
                    && requirement.italic == format.fontItalic;
            });
        if (existing == requirements.end()) {
            requirements.append(FontRequirement{
                format.fontBold,
                format.fontItalic});
        }
    };

    if (m_hasSelection) {
        const Draft draft = selectedDraftForDisplay();
        qsizetype rangeStart = 0;
        qsizetype rangeLength = 0;
        selectedTextRangeForDraft(draft, rangeStart, rangeLength);
        const qsizetype rangeEnd = rangeStart + rangeLength;
        for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
            const qsizetype spanStart = qMax<qsizetype>(rangeStart, span.start);
            const qsizetype spanEnd = qMin<qsizetype>(
                rangeEnd,
                span.start + span.length);
            if (spanEnd > spanStart) {
                appendRequirement(span.format);
            }
        }
        if (requirements.isEmpty()) {
            appendRequirement(primaryCharacterFormat(draft));
        }
        if (m_textEditActive
            && m_selectedTextRangeLength == 0
            && m_typingCharacterFormatActive) {
            appendRequirement(m_typingCharacterFormat);
        }
    } else {
        requirements.append(FontRequirement{
            selectedFontBold(),
            selectedFontItalic()});
    }

    const auto familiesForRequirement = [filter](
                                            const FontRequirement &requirement) {
        switch (filter) {
        case VietnameseFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Vietnamese,
                requirement.bold,
                requirement.italic));
        case EnglishFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::English,
                requirement.bold,
                requirement.italic));
        case ChineseFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Chinese,
                requirement.bold,
                requirement.italic));
        case JapaneseFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Japanese,
                requirement.bold,
                requirement.italic));
        case KoreanFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Korean,
                requirement.bold,
                requirement.italic));
        }
        return QStringList(ApplicationFonts::availableFontFamilies(
            requirement.bold,
            requirement.italic));
    };

    QStringList families;
    bool firstRequirement = true;
    for (const FontRequirement &requirement : std::as_const(requirements)) {
        const QStringList requirementFamilies = familiesForRequirement(
            requirement);
        if (firstRequirement) {
            families = requirementFamilies;
            firstRequirement = false;
            continue;
        }
        families.erase(
            std::remove_if(
                families.begin(),
                families.end(),
                [&requirementFamilies](const QString &family) {
                    return !requirementFamilies.contains(
                        family,
                        Qt::CaseInsensitive);
                }),
            families.end());
    }
    return families;
}

QVariantList PDFEditController::selectedCharacterSpans() const
{
    return m_hasSelection
        ? characterSpanMaps(selectedDraftForDisplay().characterSpans)
        : QVariantList();
}

QVariantList PDFEditController::selectedParagraphFormats() const
{
    return m_hasSelection
        ? paragraphFormatMaps(selectedDraftForDisplay().paragraphFormats)
        : QVariantList();
}

QString PDFEditController::selectedText() const
{
    return m_hasSelection ? m_selectedText : QString();
}

QString PDFEditController::selectedFontFamily() const
{
    return m_hasSelection
        ? selectedCharacterFormatForDisplay().fontFamily
        : QString();
}

qreal PDFEditController::selectedFontSizeRatio() const
{
    return m_hasSelection
        ? selectedCharacterFormatForDisplay().fontSizeRatio
        : 0.0;
}

qreal PDFEditController::selectedFontSize() const
{
    if (!m_document || !m_hasSelection || !isTextCandidateKind(m_selectedCandidate.kind)) {
        return 0.0;
    }
    const qreal pageHeight = m_document->pageSize(m_selectedCandidate.page).height();
    const CharacterFormat selectedFormat = selectedCharacterFormatForDisplay();
    const auto sizeRatio = [](const CharacterFormat &format) {
        return format.displayFontSizeRatio > 0.0
            ? format.displayFontSizeRatio : format.fontSizeRatio;
    };
    const qreal points = sizeRatio(selectedFormat) * pageHeight;
    if (!qIsFinite(points) || points <= 0.0) {
        return 0.0;
    }
    if (!m_textEditActive || m_selectedTextRangeLength > 0) {
        const Draft draft = selectedDraftForDisplay();
        qsizetype start = 0;
        qsizetype length = 0;
        selectedTextRangeForDraft(draft, start, length);
        for (const CharacterSpan &span : draft.characterSpans) {
            if (span.start < start + length && span.start + span.length > start
                && qAbs(sizeRatio(span.format) * pageHeight - points) > 0.01) {
                return 0.0;
            }
        }
    }
    return qRound(points);
}

qreal PDFEditController::selectedDisplayFontSizeRatio() const
{
    if (!m_hasSelection) {
        return 0.0;
    }
    const CharacterFormat format = selectedCharacterFormatForDisplay();
    return format.displayFontSizeRatio > 0.0
        ? format.displayFontSizeRatio
        : format.fontSizeRatio;
}

int PDFEditController::selectedFontStretch() const
{
    return m_hasSelection
        ? selectedCharacterFormatForDisplay().fontStretch
        : QFont::Unstretched;
}

bool PDFEditController::selectedFontBold() const
{
    return m_hasSelection && selectedCharacterFormatForDisplay().fontBold;
}

bool PDFEditController::selectedFontItalic() const
{
    return m_hasSelection && selectedCharacterFormatForDisplay().fontItalic;
}

bool PDFEditController::selectedFontUnderline() const
{
    return m_hasSelection && selectedCharacterFormatForDisplay().fontUnderline;
}

QColor PDFEditController::selectedColor() const
{
    return m_hasSelection
        ? selectedCharacterFormatForDisplay().color
        : QColor(Qt::black);
}

int PDFEditController::selectedParagraphAlignment() const
{
    if (!m_hasSelection || !isTextCandidateKind(m_selectedCandidate.kind)) {
        return -1;
    }

    const Draft draft = selectedDraftForDisplay();
    qsizetype first = -1;
    qsizetype last = -1;
    selectedParagraphRangeForDraft(draft, first, last);
    if (first < 0 || last < first) {
        return -1;
    }

    const ParagraphAlignment alignment = draft.paragraphFormats.at(first).alignment;
    for (qsizetype index = first + 1; index <= last; ++index) {
        if (draft.paragraphFormats.at(index).alignment != alignment) {
            return -1;
        }
    }
    return static_cast<int>(alignment);
}

QFont PDFEditController::fontForDisplay(const QString &fontFamily,
                                        int fontStretch,
                                        bool fontBold,
                                        bool fontItalic,
                                        bool fontUnderline,
                                        qreal pixelSize) const
{
    const int boundedPixelSize = qMax(1, qRound(pixelSize));
    QFont font;
    if (!fontFamily.isEmpty()) {
        font.setFamily(fontFamily);
    }
    font.setBold(fontBold);
    font.setItalic(fontItalic);
    font.setUnderline(fontUnderline);
    font.setPixelSize(boundedPixelSize);
    font.setStretch(displayFontStretch(
        fontFamily,
        fontStretch,
        fontBold,
        fontItalic,
        boundedPixelSize));
    return font;
}

int PDFEditController::displayFontStretch(const QString &fontFamily,
                                          int fontStretch,
                                          bool fontBold,
                                          bool fontItalic,
                                          qreal pixelSize) const
{
    return ApplicationFonts::calibratedFontStretch(
        fontFamily,
        fontStretch,
        fontBold,
        fontItalic,
        qMax(1, qRound(pixelSize)));
}

void PDFEditController::applyTypingFormatToEditorDocument(
    QObject *textDocument,
    int start,
    int length,
    qreal displayedPageHeight) const
{
    auto *quickTextDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickTextDocument
        ? quickTextDocument->textDocument()
        : nullptr;
    if (!document || !m_typingCharacterFormatActive || length <= 0) {
        return;
    }

    const int maximumPosition = qMax(0, document->characterCount() - 1);
    const int rangeStart = qBound(0, start, maximumPosition);
    const int rangeEnd = qBound(
        rangeStart,
        rangeStart + length,
        maximumPosition);
    if (rangeEnd <= rangeStart) {
        return;
    }

    QTextCharFormat format;
    format.setFontWeight(
        m_typingCharacterFormat.fontBold ? QFont::Bold : QFont::Normal);
    format.setFontItalic(m_typingCharacterFormat.fontItalic);
    format.setFontUnderline(m_typingCharacterFormat.fontUnderline);
    format.setProperty(kEditorFontSizeRatio, m_typingCharacterFormat.fontSizeRatio);
    format.setProperty(kEditorDisplayFontSizeRatio,
                       m_typingCharacterFormat.displayFontSizeRatio);
    if (qIsFinite(displayedPageHeight) && displayedPageHeight > 0.0) {
        const qreal ratio = m_typingCharacterFormat.displayFontSizeRatio > 0.0
            ? m_typingCharacterFormat.displayFontSizeRatio
            : m_typingCharacterFormat.fontSizeRatio;
        const int pixels = qMax(1, qRound(ratio * displayedPageHeight));
        format.setProperty(QTextFormat::FontPixelSize, pixels);
        format.setFontStretch(displayFontStretch(
            m_typingCharacterFormat.fontFamily, m_typingCharacterFormat.fontStretch,
            m_typingCharacterFormat.fontBold, m_typingCharacterFormat.fontItalic, pixels));
    }

    QTextCursor cursor(document);
    const auto properties = format.properties();
    bool needsFormat = false;
    for (int position = rangeStart; position < rangeEnd && !needsFormat; ++position) {
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat current = cursor.charFormat();
        for (auto property = properties.cbegin(); property != properties.cend(); ++property) {
            if (current.property(property.key()) != property.value()) {
                needsFormat = true;
                break;
            }
        }
    }
    // Redo already restores the inserted character's format. Do not append a
    // formatting command while Qt is restoring its cursor and history.
    if (!needsFormat) {
        return;
    }
    cursor.setPosition(rangeStart);
    cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
    cursor.joinPreviousEditBlock();
    cursor.mergeCharFormat(format);
    document->appendUndoItem(new EditorTypingCursorBoundary);
    cursor.endEditBlock();
}

void PDFEditController::initializeEditorFontSizes(
    QObject *textDocument, qreal displayedPageHeight) const
{
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickDocument ? quickDocument->textDocument() : nullptr;
    if (!document || !m_textEditActive || !m_hasSelection) {
        return;
    }
    const Draft draft = selectedDraftForDisplay();
    if (document->toPlainText() != draft.text) {
        return;
    }
    if (!quickDocument->findChild<QObject *>(QStringLiteral("trepdfTypingEditBlock"),
                                            Qt::FindDirectChildrenOnly)) {
        new EditorTypingEditBlock(quickDocument, [controller = QPointer<const PDFEditController>(this)]() {
            return controller && controller->m_textEditActive
                && controller->m_typingCharacterFormatActive;
        });
    }
    // Called only immediately after loading a new rich document, before user
    // edits. Metadata must not become a visible undo step.
    const bool undoEnabled = document->isUndoRedoEnabled();
    document->setUndoRedoEnabled(false);
    QTextCursor cursor(document);
    for (const CharacterSpan &span : draft.characterSpans) {
        const int start = qBound<qsizetype>(0, span.start, draft.text.size());
        const int end = qBound<qsizetype>(start, span.start + span.length, draft.text.size());
        if (end <= start) {
            continue;
        }
        QTextCharFormat format;
        format.setProperty(kEditorFontSizeRatio, span.format.fontSizeRatio);
        format.setProperty(kEditorDisplayFontSizeRatio, span.format.displayFontSizeRatio);
        if (qIsFinite(displayedPageHeight) && displayedPageHeight > 0.0) {
            const qreal ratio = span.format.displayFontSizeRatio > 0.0
                ? span.format.displayFontSizeRatio : span.format.fontSizeRatio;
            const int pixels = qMax(1, qRound(ratio * displayedPageHeight));
            format.setProperty(QTextFormat::FontPixelSize, pixels);
            format.setFontStretch(displayFontStretch(
                span.format.fontFamily, span.format.fontStretch,
                span.format.fontBold, span.format.fontItalic, pixels));
        }
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    document->setUndoRedoEnabled(undoEnabled);
}

bool PDFEditController::applyFontSizeToEditorDocument(
    QObject *textDocument, int start, int length,
    qreal fontSizeRatio, qreal displayedPageHeight) const
{
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickDocument ? quickDocument->textDocument() : nullptr;
    if (!document || !m_textEditActive || !m_hasSelection || length <= 0
        || !qIsFinite(fontSizeRatio) || fontSizeRatio <= 0.0
        || !qIsFinite(displayedPageHeight) || displayedPageHeight <= 0.0) {
        return false;
    }
    const Draft draft = selectedDraftForDisplay();
    if (document->toPlainText() != draft.text) {
        return false;
    }
    const int maximum = qMax(0, document->characterCount() - 1);
    const int first = qBound(0, start, maximum);
    const int last = static_cast<int>(qBound(qint64(first), qint64(start) + length,
                                            qint64(maximum)));
    if (last <= first) {
        return false;
    }
    const int pixels = qMax(1, qRound(fontSizeRatio * displayedPageHeight));
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (const CharacterSpan &span : draft.characterSpans) {
        const int rangeStart = qMax<qsizetype>(first, span.start);
        const int rangeEnd = qMin<qsizetype>(last, span.start + span.length);
        if (rangeEnd <= rangeStart) {
            continue;
        }
        QTextCharFormat format;
        format.setProperty(kEditorFontSizeRatio, fontSizeRatio);
        format.setProperty(kEditorDisplayFontSizeRatio, fontSizeRatio);
        format.setProperty(QTextFormat::FontPixelSize, pixels);
        format.setFontStretch(displayFontStretch(
            span.format.fontFamily, span.format.fontStretch,
            span.format.fontBold, span.format.fontItalic, pixels));
        cursor.setPosition(rangeStart);
        cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    cursor.endEditBlock();
    return true;
}

bool PDFEditController::applyFontStyleToEditorDocument(
    QObject *textDocument,
    int start,
    int length,
    int style,
    bool enabled) const
{
    auto *quickTextDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickTextDocument
        ? quickTextDocument->textDocument()
        : nullptr;
    if (!document || length <= 0
        || style < static_cast<int>(FontStyle::Bold)
        || style > static_cast<int>(FontStyle::Underline)) {
        return false;
    }

    const int maximumPosition = qMax(0, document->characterCount() - 1);
    const int rangeStart = qBound(0, start, maximumPosition);
    const int rangeEnd = qBound(
        rangeStart,
        rangeStart + length,
        maximumPosition);
    if (rangeEnd <= rangeStart) {
        return false;
    }

    QTextCharFormat format;
    switch (static_cast<FontStyle>(style)) {
    case FontStyle::Bold:
        format.setFontWeight(enabled ? QFont::Bold : QFont::Normal);
        break;
    case FontStyle::Italic:
        format.setFontItalic(enabled);
        break;
    case FontStyle::Underline:
        format.setFontUnderline(enabled);
        break;
    }

    QTextCursor cursor(document);
    cursor.setPosition(rangeStart);
    cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
    cursor.beginEditBlock();
    cursor.mergeCharFormat(format);
    cursor.endEditBlock();
    return true;
}

bool PDFEditController::syncEditorFontStyles(QObject *textDocument)
{
    auto *quickTextDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickTextDocument
        ? quickTextDocument->textDocument()
        : nullptr;
    if (!document || !m_textEditActive || !m_hasSelection) {
        return false;
    }

    Draft draft = selectedDraftForDisplay();
    if (document->toPlainText() != draft.text || draft.text.isEmpty()) {
        return false;
    }

    const qsizetype textLength = draft.text.size();
    const CharacterFormat fallbackFormat = primaryCharacterFormat(draft);
    QVector<CharacterFormat> formats(textLength, fallbackFormat);
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qBound<qsizetype>(0, span.start, textLength);
        const qsizetype spanEnd = qBound<qsizetype>(
            spanStart,
            span.start + span.length,
            textLength);
        for (qsizetype position = spanStart; position < spanEnd; ++position) {
            formats[position] = span.format;
        }
    }

    QTextCursor cursor(document);
    for (qsizetype position = 0; position < textLength; ++position) {
        cursor.setPosition(static_cast<int>(position));
        cursor.setPosition(static_cast<int>(position + 1), QTextCursor::KeepAnchor);
        const QTextCharFormat editorFormat = cursor.charFormat();
        CharacterFormat &format = formats[position];
        format.fontBold = editorFormat.fontWeight() >= QFont::Bold;
        format.fontItalic = editorFormat.fontItalic();
        format.fontUnderline = editorFormat.fontUnderline();
        if (editorFormat.hasProperty(kEditorFontSizeRatio)
            && editorFormat.hasProperty(kEditorDisplayFontSizeRatio)) {
            const qreal ratio = editorFormat.doubleProperty(kEditorFontSizeRatio);
            const qreal displayRatio = editorFormat.doubleProperty(kEditorDisplayFontSizeRatio);
            if (qIsFinite(ratio) && ratio > 0.0
                && qIsFinite(displayRatio) && displayRatio >= 0.0) {
                format.fontSizeRatio = ratio;
                format.displayFontSizeRatio = displayRatio;
            }
        }
    }

    QVector<CharacterSpan> spans;
    qsizetype spanStart = 0;
    CharacterFormat spanFormat = formats.constFirst();
    for (qsizetype position = 1; position <= formats.size(); ++position) {
        if (position < formats.size()
            && sameCharacterFormat(spanFormat, formats.at(position))) {
            continue;
        }
        spans.append(CharacterSpan{
            spanStart,
            position - spanStart,
            spanFormat});
        if (position < formats.size()) {
            spanStart = position;
            spanFormat = formats.at(position);
        }
    }
    if (sameCharacterSpans(draft.characterSpans, spans)) {
        return false;
    }

    draft.characterSpans = std::move(spans);
    updateSelectedDraft(draft);
    return true;
}

void PDFEditController::beginSelectedTextEdit()
{
    if (m_hasSelection
        && !isTextCandidateKind(m_selectedCandidate.kind)) {
        return;
    }
    beginSelectedTextEditSession(true);
}

void PDFEditController::beginSelectedTextEditSession(bool expandFrame)
{
    if (m_textEditActive || !m_hasSelection || !m_selectedCandidate.editable) {
        return;
    }

    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    m_textEditCommand = Command();
    m_textEditCommand.key = key;
    m_textEditCommand.page = m_selectedCandidate.page;
    m_textEditCommand.objectId = m_selectedCandidate.objectId;
    m_textEditCommand.hadBefore = existingDraft != m_drafts.cend();
    m_textEditCommand.before = m_textEditCommand.hadBefore
        ? *existingDraft
        : initialDraftForCandidate(m_selectedCandidate);
    m_textEditDisplayDraft = m_textEditCommand.before;
    m_textEditDisplayDraftActive = true;
    m_textEditFrameAutomaticallyExpanded = false;
    m_textEditGeometryExplicitlyChanged = false;
    if (expandFrame) {
        const QRectF expandedFrame = expandedTextEditFrame(m_textEditDisplayDraft);
        if (expandedFrame != m_textEditDisplayDraft.frameRect) {
            const qreal contentBottom = qBound<qreal>(
                expandedFrame.bottom(),
                qMax(
                    m_textEditDisplayDraft.frameRect.bottom(),
                    m_textEditDisplayDraft.contentBounds.bottom()),
                1.0);
            m_textEditDisplayDraft.frameRect = expandedFrame;
            m_textEditDisplayDraft.contentBounds = QRectF(
                expandedFrame.x(),
                expandedFrame.y(),
                expandedFrame.width(),
                contentBottom - expandedFrame.y());
            m_textEditFrameAutomaticallyExpanded = true;
        }
    }
    m_textEditInitialDisplayFrameRect = m_textEditDisplayDraft.frameRect;
    resetTypingFormat();
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditActive = true;
    emit textEditActiveChanged();
    emit selectionChanged();
}

void PDFEditController::updateSelectedTextPreview(const QString &text,
                                                  int cursorPosition)
{
    if (!m_textEditActive) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }
    Draft draft = selectedDraftForDisplay();
    if (draft.text == text) {
        return;
    }
    const TextChange change = setDraftText(
        draft,
        text,
        m_typingCharacterFormatActive ? &m_typingCharacterFormat : nullptr,
        m_typingCharacterFormatPosition,
        static_cast<qsizetype>(cursorPosition));
    bool shouldSyncEditorFormat = false;
    if (m_typingCharacterFormatActive) {
        const bool typingChange = change.start == m_typingCharacterFormatPosition
            || change.start + change.removedLength
                == m_typingCharacterFormatPosition;
        if (typingChange) {
            shouldSyncEditorFormat = change.insertedLength > 0;
            m_typingCharacterFormatPosition = change.start + change.insertedLength;
            m_selectedTextRangeStart = m_typingCharacterFormatPosition;
            m_selectedTextRangeLength = 0;
        } else {
            resetTypingFormat();
        }
    }
    updateSelectedDraft(draft);
    if (shouldSyncEditorFormat) {
        emit typingFormatApplied(
            static_cast<int>(change.start),
            static_cast<int>(change.insertedLength));
    }
}

void PDFEditController::setSelectedTextRange(int start, int length)
{
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    const qsizetype textLength = m_selectedText.size();
    const qsizetype rangeStart = qBound<qsizetype>(
        0,
        static_cast<qsizetype>(start),
        textLength);
    const qsizetype rangeLength = qBound<qsizetype>(
        0,
        static_cast<qsizetype>(length),
        textLength - rangeStart);
    const bool rangeChanged = rangeStart != m_selectedTextRangeStart
        || rangeLength != m_selectedTextRangeLength;
    bool typingFormatChanged = false;
    if (m_typingCharacterFormatActive
        && (rangeLength > 0
            || rangeStart != m_typingCharacterFormatPosition)) {
        resetTypingFormat();
        typingFormatChanged = true;
    }
    m_selectedTextRangeStart = rangeStart;
    m_selectedTextRangeLength = rangeLength;
    if (rangeChanged || typingFormatChanged) {
        emit textFormatStateChanged();
    }
}

void PDFEditController::setSelectedFontFamily(const QString &fontFamily)
{
    if (!m_hasSelection
        || !m_selectedCandidate.editable
        || fontFamily.trimmed().isEmpty()
        || !QFontDatabase::hasFamily(fontFamily)) {
        return;
    }

    Draft draft = selectedDraftForDisplay();
    qsizetype rangeStart = 0;
    qsizetype rangeLength = 0;
    selectedTextRangeForDraft(draft, rangeStart, rangeLength);
    const qsizetype rangeEnd = rangeStart + rangeLength;
    bool checkedSpan = false;
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qMax<qsizetype>(0, span.start);
        const qsizetype spanEnd = qMin<qsizetype>(
            draft.text.size(),
            span.start + span.length);
        if (spanEnd <= rangeStart || spanStart >= rangeEnd) {
            continue;
        }
        checkedSpan = true;
        if (!ApplicationFonts::isFontFaceSelectable(
                fontFamily,
                span.format.fontBold,
                span.format.fontItalic)) {
            return;
        }
    }
    if (!checkedSpan) {
        const CharacterFormat format = primaryCharacterFormat(draft);
        if (!ApplicationFonts::isFontFaceSelectable(
                fontFamily,
                format.fontBold,
                format.fontItalic)) {
            return;
        }
    }
    if (m_textEditActive
        && m_selectedTextRangeLength == 0
        && m_typingCharacterFormatActive
        && !ApplicationFonts::isFontFaceSelectable(
            fontFamily,
            m_typingCharacterFormat.fontBold,
            m_typingCharacterFormat.fontItalic)) {
        return;
    }

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    draft = selectedDraftForDisplay();
    const bool changed = updateCharacterFormats(
        draft,
        rangeStart,
        rangeLength,
        [&fontFamily](CharacterFormat &format) {
            if (sameFontFamily(format.fontFamily, fontFamily)
                && format.fontStretch == QFont::Unstretched) {
                return;
            }
            format.fontFamily = fontFamily;
            format.fontStretch = QFont::Unstretched;
        });
    if (!changed) {
        if (finishImmediately) {
            finishSelectedTextEdit();
        }
        return;
    }
    rebaseTypingFormat(draft);
    updateSelectedDraft(draft);

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::setSelectedFontSize(qreal points)
{
    if (m_saving || !m_document || !m_hasSelection || !m_selectedCandidate.editable
        || !isTextCandidateKind(m_selectedCandidate.kind)
        || !qIsFinite(points) || points < 1.0 || points > 512.0
        || points != std::floor(points)) {
        return;
    }
    const qreal pageHeight = m_document->pageSize(m_selectedCandidate.page).height();
    if (!qIsFinite(pageHeight) || pageHeight <= 0.0) {
        return;
    }
    const qreal ratio = points / pageHeight;
    const auto changeSize = [points, ratio, pageHeight](CharacterFormat &format) {
        const qreal current = (format.displayFontSizeRatio > 0.0
            ? format.displayFontSizeRatio : format.fontSizeRatio) * pageHeight;
        // Candidate scan rounds through device coordinates. Re-selecting the
        // displayed integral point size should not create a source-text draft.
        if (qAbs(current - points) <= 0.01) {
            return;
        }
        format.fontSizeRatio = ratio;
        format.displayFontSizeRatio = ratio;
    };
    Draft draft = selectedDraftForDisplay();
    if (m_textEditActive && m_selectedTextRangeLength == 0) {
        const CharacterFormat current = selectedCharacterFormatForDisplay();
        CharacterFormat typing = current;
        changeSize(typing);
        if (sameCharacterFormat(current, typing)) {
            return;
        }
        const qsizetype caret = qBound<qsizetype>(
            0, m_selectedTextRangeStart, draft.text.size());
        if (sameCharacterFormat(typing, characterFormatAtPosition(draft, caret, true))) {
            resetTypingFormat();
        } else {
            m_typingCharacterFormat = typing;
            m_typingCharacterFormatPosition = caret;
            m_typingCharacterFormatActive = true;
        }
        emit textFormatStateChanged();
        return;
    }
    qsizetype start = 0;
    qsizetype length = 0;
    selectedTextRangeForDraft(draft, start, length);
    if (!updateCharacterFormats(draft, start, length, changeSize)) {
        return;
    }
    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEditSession(false);
    }
    if (!m_textEditActive) {
        return;
    }
    if (!finishImmediately && length > 0) {
        emit selectionFontSizeApplied(static_cast<int>(start), static_cast<int>(length), ratio);
    }
    updateSelectedDraft(draft);
    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::setSelectedFontBold(bool enabled)
{
    setSelectedFontStyle(FontStyle::Bold, enabled);
}

void PDFEditController::setSelectedFontItalic(bool enabled)
{
    setSelectedFontStyle(FontStyle::Italic, enabled);
}

void PDFEditController::setSelectedFontUnderline(bool enabled)
{
    setSelectedFontStyle(FontStyle::Underline, enabled);
}

void PDFEditController::setSelectedColor(const QColor &color)
{
    if (!m_hasSelection || !m_selectedCandidate.editable || !color.isValid()) {
        return;
    }

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    const QColor normalizedColor = color.toRgb();
    Draft draft = selectedDraftForDisplay();
    qsizetype rangeStart = 0;
    qsizetype rangeLength = 0;
    selectedTextRangeForDraft(draft, rangeStart, rangeLength);
    const bool changed = updateCharacterFormats(
        draft,
        rangeStart,
        rangeLength,
        [&normalizedColor](CharacterFormat &format) {
            format.color = normalizedColor;
        });
    if (!changed) {
        if (finishImmediately) {
            finishSelectedTextEdit();
        }
        return;
    }
    rebaseTypingFormat(draft);
    updateSelectedDraft(draft);

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::setSelectedParagraphAlignment(
    ParagraphAlignment alignment)
{
    switch (alignment) {
    case LeftAlignment:
    case RightAlignment:
    case CenterAlignment:
    case JustifyAlignment:
        break;
    default:
        return;
    }

    if (!m_hasSelection
        || !m_selectedCandidate.editable
        || !isTextCandidateKind(m_selectedCandidate.kind)) {
        return;
    }

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    Draft draft = selectedDraftForDisplay();
    qsizetype first = -1;
    qsizetype last = -1;
    if (finishImmediately) {
        first = 0;
        last = draft.paragraphFormats.size() - 1;
    } else {
        selectedParagraphRangeForDraft(draft, first, last);
    }

    bool changed = false;
    if (alignment == JustifyAlignment && first >= 0 && last >= first
        && !draft.sourceLineBreaks.isEmpty()) {
        // Source rows belong to one inferred paragraph. A user-entered newline
        // remains a paragraph boundary, including when only the caret is active.
        while (first > 0 && draft.sourceLineBreaks.contains(
                   draft.paragraphFormats.at(first).start - 1)) {
            --first;
        }
        while (last + 1 < draft.paragraphFormats.size()
               && draft.sourceLineBreaks.contains(
                   draft.paragraphFormats.at(last + 1).start - 1)) {
            ++last;
        }
        const qsizetype start = draft.paragraphFormats.at(first).start;
        const qsizetype end = draft.paragraphFormats.at(last).start
            + draft.paragraphFormats.at(last).length;
        for (qsizetype position : std::as_const(draft.sourceLineBreaks)) {
            if (position >= start && position < end) {
                // Keep UTF-16 positions stable for character styles and the
                // live editor selection while enabling normal word wrapping.
                draft.text[position] = QLatin1Char(' ');
                changed = true;
            }
        }
        if (changed) {
            draft.sourceLineBreaks.removeIf([start, end](qsizetype position) {
                return position >= start && position < end;
            });
            const auto oldFormats = draft.paragraphFormats;
            draft.paragraphFormats = paragraphFormatsForText(draft.text, ParagraphFormat());
            qsizetype previous = 0;
            for (qsizetype index = 0; index < draft.paragraphFormats.size(); ++index) {
                ParagraphFormat &format = draft.paragraphFormats[index];
                while (previous + 1 < oldFormats.size()
                       && oldFormats.at(previous + 1).start <= format.start) {
                    ++previous;
                }
                const qsizetype newStart = format.start;
                const qsizetype newLength = format.length;
                format = oldFormats.at(previous);
                format.start = newStart;
                format.length = newLength;
                if (format.start == start) {
                    first = index;
                }
                if (format.start <= end) {
                    last = index;
                }
            }
        }
    }
    for (qsizetype index = first; index >= 0 && index <= last; ++index) {
        ParagraphFormat &format = draft.paragraphFormats[index];
        if (format.alignment != alignment) {
            format.alignment = alignment;
            changed = true;
        }
    }
    if (changed) {
        updateSelectedDraft(draft);
    }

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::setSelectedFontStyle(FontStyle style, bool enabled)
{
    if (!m_hasSelection || !m_selectedCandidate.editable) {
        return;
    }

    const auto updatedStyle = [style, enabled](CharacterFormat format) {
        switch (style) {
        case FontStyle::Bold:
            format.fontBold = enabled;
            break;
        case FontStyle::Italic:
            format.fontItalic = enabled;
            break;
        case FontStyle::Underline:
            format.fontUnderline = enabled;
            break;
        }
        return format;
    };
    const auto faceIsSelectable = [](const CharacterFormat &format) {
        const bool knownFamily = ApplicationFonts::bundledFontFamilies().contains(
                format.fontFamily,
                Qt::CaseInsensitive)
            || ApplicationFonts::systemFontFamilies().contains(
                format.fontFamily,
                Qt::CaseInsensitive);
        return !knownFamily
            || ApplicationFonts::isFontFaceSelectable(
                format.fontFamily,
                format.fontBold,
                format.fontItalic);
    };

    Draft draft = selectedDraftForDisplay();
    if (m_textEditActive && m_selectedTextRangeLength == 0) {
        const qsizetype caretPosition = qBound<qsizetype>(
            0,
            m_selectedTextRangeStart,
            draft.text.size());
        const CharacterFormat currentFormat = selectedCharacterFormatForDisplay();
        const CharacterFormat typingFormat = updatedStyle(currentFormat);
        if ((style != FontStyle::Underline && !faceIsSelectable(typingFormat))
            || sameCharacterFormat(currentFormat, typingFormat)) {
            return;
        }

        const CharacterFormat inheritedFormat = characterFormatAtPosition(
            draft,
            caretPosition,
            true);
        if (sameCharacterFormat(typingFormat, inheritedFormat)) {
            resetTypingFormat();
        } else {
            m_typingCharacterFormat = typingFormat;
            m_typingCharacterFormatPosition = caretPosition;
            m_typingCharacterFormatActive = true;
        }
        emit textFormatStateChanged();
        return;
    }

    qsizetype rangeStart = 0;
    qsizetype rangeLength = 0;
    selectedTextRangeForDraft(draft, rangeStart, rangeLength);
    if (style != FontStyle::Underline) {
        const qsizetype rangeEnd = rangeStart + rangeLength;
        bool checkedSpan = false;
        for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
            const qsizetype spanStart = qMax<qsizetype>(0, span.start);
            const qsizetype spanEnd = qMin<qsizetype>(
                draft.text.size(),
                span.start + span.length);
            if (spanEnd <= rangeStart || spanStart >= rangeEnd) {
                continue;
            }
            checkedSpan = true;
            if (!faceIsSelectable(updatedStyle(span.format))) {
                return;
            }
        }
        if (!checkedSpan
            && !faceIsSelectable(updatedStyle(primaryCharacterFormat(draft)))) {
            return;
        }
    }

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    draft = selectedDraftForDisplay();
    const bool changed = updateCharacterFormats(
        draft,
        rangeStart,
        rangeLength,
        [&updatedStyle](CharacterFormat &format) {
            format = updatedStyle(format);
        });
    if (!changed) {
        if (finishImmediately) {
            finishSelectedTextEdit();
        }
        return;
    }
    if (m_textEditActive && rangeLength > 0) {
        emit selectionFontStyleApplied(
            static_cast<int>(rangeStart),
            static_cast<int>(rangeLength),
            static_cast<int>(style),
            enabled);
    }
    updateSelectedDraft(draft);

    if (finishImmediately) {
        finishSelectedTextEdit();
    }
}

void PDFEditController::finishSelectedTextEdit()
{
    if (!m_textEditActive) {
        return;
    }

    const Command baseline = m_textEditCommand;
    Draft finalizedDraft = selectedDraftForDisplay();
    const auto fontSizesUnchanged = [&]() {
        qsizetype left = 0;
        qsizetype right = 0;
        while (left < finalizedDraft.characterSpans.size()
               && right < baseline.before.characterSpans.size()) {
            const CharacterSpan &current = finalizedDraft.characterSpans.at(left);
            const CharacterSpan &previous = baseline.before.characterSpans.at(right);
            if (!qFuzzyCompare(current.format.fontSizeRatio, previous.format.fontSizeRatio)
                || !qFuzzyCompare(current.format.displayFontSizeRatio,
                                  previous.format.displayFontSizeRatio)) {
                return false;
            }
            const qsizetype currentEnd = current.start + current.length;
            const qsizetype previousEnd = previous.start + previous.length;
            if (currentEnd <= previousEnd) {
                ++left;
            }
            if (previousEnd <= currentEnd) {
                ++right;
            }
        }
        return left == finalizedDraft.characterSpans.size()
            && right == baseline.before.characterSpans.size();
    };
    if (m_textEditFrameAutomaticallyExpanded
        && !m_textEditGeometryExplicitlyChanged
        && finalizedDraft.text == baseline.before.text
        && fontSizesUnchanged()
        && sameParagraphFormats(
            finalizedDraft.paragraphFormats,
            baseline.before.paragraphFormats)) {
        finalizedDraft.frameRect = baseline.before.frameRect;
        finalizedDraft.contentBounds = baseline.before.contentBounds;
    }
    const bool draftChanged
        = !sameDraftContent(finalizedDraft, baseline.before);
    if (draftChanged) {
        applyUnicodeFontFallback(finalizedDraft);
        const bool movesGeometryComment
            = isGeometryCommentCandidateKind(
                  finalizedDraft.candidate.kind)
            && finalizedDraft.frameRect != baseline.before.frameRect;
        if (finalizedDraft.candidate.kind == QLatin1String("freeText")
            || movesGeometryComment) {
            finalizedDraft.candidate.commentModifiedDate
                = QDateTime::currentDateTime();
        }
    }
    updateSelectedDraft(finalizedDraft);
    resetFrameResizeState();
    m_textEditActive = false;
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditCommand = Command();
    resetTextEditDisplayState();
    emit textEditActiveChanged();
    emit selectionChanged();

    Command command = baseline;
    const auto currentDraft = m_drafts.constFind(command.key);
    command.hadAfter = currentDraft != m_drafts.cend();
    command.after = command.hadAfter
        ? *currentDraft
        : initialDraftForCandidate(command.before.candidate);
    const bool removedAddedCandidate = command.before.candidate.added
        && !command.hadAfter
        && removeAddedCandidate(command.page, command.objectId);
    if (removedAddedCandidate) {
        if (m_hasSelection
            && m_selectedCandidate.page == command.page
            && m_selectedCandidate.objectId == command.objectId) {
            setSelection(nullptr);
        }
        emit pageDataChanged(command.page);
        if (isCommentCandidateKind(command.before.candidate.kind)) {
            emit commentsChanged();
        }
    }
    requestDraftRender(command.page, true);
    const bool changed = command.hadBefore != command.hadAfter
        || !sameDraftContent(command.before, command.after);
    if (!changed) {
        return;
    }

    m_undoStack.append(command);
    m_redoStack.clear();
    emit historyStateChanged();
}

void PDFEditController::cancelSelectedTextEdit()
{
    if (!m_textEditActive) {
        return;
    }

    const Command baseline = m_textEditCommand;
    resetFrameResizeState();
    m_textEditActive = false;
    m_selectedTextRangeStart = 0;
    m_selectedTextRangeLength = 0;
    m_textEditCommand = Command();
    resetTextEditDisplayState();
    emit textEditActiveChanged();
    applyDraftState(baseline, false, true);
    emit historyStateChanged();
}

void PDFEditController::commitSelectedText(const QString &text)
{
    if (!m_hasSelection
        || !m_selectedCandidate.editable
        || !isTextCandidateKind(m_selectedCandidate.kind)) {
        if (m_hasSelection && !m_selectedCandidate.reason.isEmpty()) {
            emit statusMessage(m_selectedCandidate.reason);
        }
        return;
    }
    if (m_textEditActive) {
        updateSelectedTextPreview(text);
        finishSelectedTextEdit();
        return;
    }

    const QString key = draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId);
    const auto existingDraft = m_drafts.constFind(key);
    const bool hadBefore = existingDraft != m_drafts.cend();
    const Draft before = hadBefore
        ? *existingDraft
        : initialDraftForCandidate(m_selectedCandidate);
    if (before.text == text) {
        return;
    }

    Command command;
    command.key = key;
    command.page = m_selectedCandidate.page;
    command.objectId = m_selectedCandidate.objectId;
    command.hadBefore = hadBefore;
    command.before = before;
    command.after = before;
    setDraftText(command.after, text);
    applyUnicodeFontFallback(command.after);
    command.hadAfter = !sameDraftContent(
        command.after,
        initialDraftForCandidate(m_selectedCandidate));

    m_undoStack.append(command);
    m_redoStack.clear();
    applyDraftState(command, true, true);
    emit historyStateChanged();
}

void PDFEditController::setInitialGeometry(Candidate &candidate,
                                           const QRectF &sourceRect)
{
    candidate.geometry.sourceRect = sourceRect;
    candidate.initialDraftFrameRect = sourceRect;
}

void PDFEditController::expandInitialTextFrame(Candidate &candidate,
                                               const QSizeF &pageSize) const
{
    const QRectF &sourceRect = candidate.geometry.sourceRect;
    if (candidate.kind != QLatin1String("text")
        || !candidate.hasBaseline
        || candidate.text.isEmpty()
        || sourceRect.isEmpty()
        || pageSize.isEmpty()) {
        return;
    }

    const qreal displayFontSizeRatio = candidate.displayFontSizeRatio > 0.0
        ? candidate.displayFontSizeRatio
        : candidate.fontSizeRatio;
    const qreal fontPixelSize = displayFontSizeRatio * pageSize.height();
    if (fontPixelSize <= 0.0) {
        return;
    }

    const QFont displayFont = fontForDisplay(
        candidate.fontFamily,
        candidate.fontStretch,
        candidate.fontBold,
        candidate.fontItalic,
        candidate.fontUnderline,
        fontPixelSize);
    const QFontMetricsF metrics(displayFont);
    qreal measuredWidth = 0.0;
    const QStringList lines = candidate.text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        measuredWidth = qMax(
            measuredWidth,
            qMax(metrics.horizontalAdvance(line), metrics.boundingRect(line).width()));
    }
    const qreal layoutLeft = qBound<qreal>(
        0.0,
        qMin(sourceRect.left(), candidate.origin.x()),
        1.0);
    const qreal measuredRight = candidate.origin.x()
        + (measuredWidth + kInitialTextFrameSafetyPoints) / pageSize.width();
    const qreal layoutRight = qBound<qreal>(
        layoutLeft,
        qMax(sourceRect.right(), measuredRight),
        1.0);
    candidate.initialDraftFrameRect = QRectF(
        layoutLeft,
        sourceRect.y(),
        layoutRight - layoutLeft,
        sourceRect.height());
}

PDFEditController::CharacterFormat PDFEditController::characterFormatForCandidate(
    const Candidate &candidate)
{
    CharacterFormat format;
    format.fontFamily = candidate.fontFamily;
    format.fontSizeRatio = candidate.fontSizeRatio;
    format.displayFontSizeRatio = candidate.displayFontSizeRatio;
    format.fontStretch = candidate.fontStretch;
    format.fontBold = candidate.fontBold;
    format.fontItalic = candidate.fontItalic;
    format.fontUnderline = candidate.fontUnderline;
    format.color = candidate.color;
    return format;
}

PDFEditController::CharacterFormat PDFEditController::primaryCharacterFormat(
    const Draft &draft)
{
    return draft.characterSpans.isEmpty()
        ? characterFormatForCandidate(draft.candidate)
        : draft.characterSpans.constFirst().format;
}

PDFEditController::CharacterFormat PDFEditController::characterFormatAtPosition(
    const Draft &draft,
    qsizetype position,
    bool preferPrevious)
{
    if (draft.characterSpans.isEmpty()) {
        return characterFormatForCandidate(draft.candidate);
    }

    const qsizetype textLength = draft.text.size();
    if (textLength <= 0) {
        return draft.characterSpans.constFirst().format;
    }

    const qsizetype boundedPosition = qBound<qsizetype>(0, position, textLength);
    const qsizetype characterIndex = preferPrevious && boundedPosition > 0
        ? boundedPosition - 1
        : qMin(boundedPosition, textLength - 1);
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qBound<qsizetype>(0, span.start, textLength);
        const qsizetype spanEnd = qBound<qsizetype>(
            spanStart,
            span.start + span.length,
            textLength);
        if (characterIndex >= spanStart && characterIndex < spanEnd) {
            return span.format;
        }
    }

    return primaryCharacterFormat(draft);
}

PDFEditController::CharacterFormat
PDFEditController::selectedCharacterFormatForDisplay() const
{
    if (!m_hasSelection) {
        return CharacterFormat();
    }

    const Draft draft = selectedDraftForDisplay();
    if (!m_textEditActive) {
        return primaryCharacterFormat(draft);
    }

    const qsizetype textLength = draft.text.size();
    const qsizetype rangeStart = qBound<qsizetype>(
        0,
        m_selectedTextRangeStart,
        textLength);
    const qsizetype rangeLength = qBound<qsizetype>(
        0,
        m_selectedTextRangeLength,
        textLength - rangeStart);
    if (rangeLength == 0
        && m_typingCharacterFormatActive
        && m_typingCharacterFormatPosition == rangeStart) {
        return m_typingCharacterFormat;
    }

    return characterFormatAtPosition(draft, rangeStart, rangeLength == 0);
}

QVector<PDFEditController::ParagraphFormat> PDFEditController::paragraphFormatsForText(
    const QString &text,
    const ParagraphFormat &prototype)
{
    QVector<ParagraphFormat> formats;
    qsizetype start = 0;
    while (start <= text.size()) {
        const qsizetype newline = text.indexOf(QLatin1Char('\n'), start);
        const qsizetype end = newline < 0 ? text.size() : newline;
        ParagraphFormat format = prototype;
        format.start = start;
        format.length = end - start;
        formats.append(format);
        if (newline < 0) {
            break;
        }
        start = newline + 1;
    }
    return formats;
}

PDFEditController::Draft PDFEditController::initialDraftForCandidate(
    const Candidate &candidate)
{
    Draft draft;
    draft.candidate = candidate;
    draft.commentProperties = candidate.commentProperties;
    draft.text = candidate.text;
    draft.characterSpans.append(CharacterSpan{
        0,
        candidate.text.size(),
        characterFormatForCandidate(candidate)});
    draft.paragraphFormats = paragraphFormatsForText(candidate.text, ParagraphFormat());
    if (candidate.objectId.startsWith(QLatin1String("paragraph:"))) {
        for (qsizetype index = 0; index < candidate.text.size(); ++index) {
            if (candidate.text.at(index) == QLatin1Char('\n')) {
                draft.sourceLineBreaks.append(index);
            }
        }
    }
    draft.frameRect = candidate.initialDraftFrameRect.isEmpty()
        ? candidate.geometry.sourceRect
        : candidate.initialDraftFrameRect;
    draft.contentBounds = draft.frameRect;
    draft.commentStatus = candidate.commentStatus;
    draft.commentMarked = candidate.commentMarked;
    draft.commentStateAuthor = candidate.commentStateAuthor.trimmed().isEmpty()
        ? currentCommentAuthor()
        : candidate.commentStateAuthor;
    return draft;
}

bool PDFEditController::isSourceTextMoveOnly(const Draft &draft)
{
    const Candidate &candidate = draft.candidate;
    if (candidate.kind != QLatin1String("text")
        || candidate.added
        || draft.deleted
        || !candidate.editable
        || !candidate.renderSuppressible) {
        return false;
    }

    const auto hasTopLevelSourcePath = [](const QVector<int> &objectPath) {
        return objectPath.size() == 1;
    };
    if (candidate.parts.isEmpty()) {
        if (!hasTopLevelSourcePath(candidate.objectPath)
            || candidate.geometry.sourceRect.isEmpty()) {
            return false;
        }
    } else {
        for (const CandidatePart &part : candidate.parts) {
            if (!hasTopLevelSourcePath(part.objectPath)
                || part.sourceRect.isEmpty()) {
                return false;
            }
        }
    }

    constexpr qreal geometryEpsilon = 1e-9;
    const auto nearlyEqual = [](qreal left, qreal right) {
        return qAbs(left - right) <= geometryEpsilon;
    };
    const QRectF initialFrameRect
        = (candidate.initialDraftFrameRect.isEmpty()
               ? candidate.geometry.sourceRect
               : candidate.initialDraftFrameRect)
              .normalized();
    if (initialFrameRect.isEmpty()
        || !nearlyEqual(draft.frameRect.width(), initialFrameRect.width())
        || !nearlyEqual(
            draft.frameRect.height(),
            initialFrameRect.height())) {
        return false;
    }

    const QPointF translation
        = draft.frameRect.topLeft() - initialFrameRect.topLeft();
    if (nearlyEqual(translation.x(), 0.0)
        && nearlyEqual(translation.y(), 0.0)) {
        return false;
    }

    const Draft initialDraft = initialDraftForCandidate(candidate);
    if (draft.text != initialDraft.text
        || !sameCharacterSpans(
            draft.characterSpans,
            initialDraft.characterSpans)
        || !sameParagraphFormats(
            draft.paragraphFormats,
            initialDraft.paragraphFormats)) {
        return false;
    }

    const QRectF expectedContentBounds
        = initialDraft.contentBounds.translated(translation);
    return nearlyEqual(
               draft.contentBounds.x(),
               expectedContentBounds.x())
        && nearlyEqual(
               draft.contentBounds.y(),
               expectedContentBounds.y())
        && nearlyEqual(
               draft.contentBounds.width(),
               expectedContentBounds.width())
        && nearlyEqual(
               draft.contentBounds.height(),
               expectedContentBounds.height());
}

PDFEditController::TextChange PDFEditController::setDraftText(
    Draft &draft,
    const QString &text,
    const CharacterFormat *typingFormat,
    qsizetype typingPosition,
    qsizetype cursorAfterEdit)
{
    if (draft.text == text) {
        return TextChange();
    }

    const qsizetype oldLength = draft.text.size();
    const qsizetype newLength = text.size();
    qsizetype commonPrefix = 0;
    while (commonPrefix < oldLength
           && commonPrefix < newLength
           && draft.text.at(commonPrefix) == text.at(commonPrefix)) {
        ++commonPrefix;
    }

    qsizetype commonSuffix = 0;
    while (commonSuffix < oldLength - commonPrefix
           && commonSuffix < newLength - commonPrefix
           && draft.text.at(oldLength - commonSuffix - 1)
               == text.at(newLength - commonSuffix - 1)) {
        ++commonSuffix;
    }

    if (cursorAfterEdit >= 0
        && cursorAfterEdit <= newLength) {
        const qsizetype lengthDelta = newLength - oldLength;
        if (lengthDelta > 0) {
            const qsizetype anchoredStart = cursorAfterEdit - lengthDelta;
            if ((!typingFormat || anchoredStart == typingPosition)
                && anchoredStart >= 0
                && draft.text.first(anchoredStart) == text.first(anchoredStart)
                && draft.text.sliced(anchoredStart)
                    == text.sliced(cursorAfterEdit)) {
                commonPrefix = anchoredStart;
                commonSuffix = oldLength - anchoredStart;
            }
        } else if (lengthDelta < 0) {
            const qsizetype removedLength = -lengthDelta;
            const qsizetype anchoredStart = cursorAfterEdit;
            if ((!typingFormat || anchoredStart == typingPosition
                 || anchoredStart + removedLength == typingPosition)
                && anchoredStart + removedLength <= oldLength
                && draft.text.first(anchoredStart) == text.first(anchoredStart)
                && draft.text.sliced(anchoredStart + removedLength)
                    == text.sliced(anchoredStart)) {
                commonPrefix = anchoredStart;
                commonSuffix = oldLength - anchoredStart - removedLength;
            }
        }
    }

    TextChange change;
    change.start = commonPrefix;
    change.removedLength = oldLength - commonPrefix - commonSuffix;
    change.insertedLength = newLength - commonPrefix - commonSuffix;

    QVector<qsizetype> sourceLineBreaks;
    for (qsizetype position : std::as_const(draft.sourceLineBreaks)) {
        if (position < change.start) {
            sourceLineBreaks.append(position);
        } else if (position >= change.start + change.removedLength) {
            sourceLineBreaks.append(position + change.insertedLength - change.removedLength);
        }
    }
    draft.sourceLineBreaks = std::move(sourceLineBreaks);

    const QVector<ParagraphFormat> oldParagraphFormats = draft.paragraphFormats;
    const CharacterFormat fallbackFormat = primaryCharacterFormat(draft);
    QVector<CharacterFormat> oldFormats(oldLength, fallbackFormat);
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qBound<qsizetype>(0, span.start, oldLength);
        const qsizetype spanEnd = qBound<qsizetype>(
            spanStart,
            span.start + span.length,
            oldLength);
        for (qsizetype index = spanStart; index < spanEnd; ++index) {
            oldFormats[index] = span.format;
        }
    }

    CharacterFormat insertedFormat = fallbackFormat;
    if (commonPrefix > 0 && !oldFormats.isEmpty()) {
        insertedFormat = oldFormats.at(commonPrefix - 1);
    } else if (commonPrefix < oldFormats.size()) {
        insertedFormat = oldFormats.at(commonPrefix);
    }
    if (typingFormat
        && change.insertedLength > 0
        && (change.start == typingPosition
            || change.start + change.removedLength == typingPosition)) {
        insertedFormat = *typingFormat;
    }

    QVector<CharacterFormat> newFormats;
    newFormats.reserve(newLength);
    for (qsizetype index = 0; index < commonPrefix; ++index) {
        newFormats.append(oldFormats.at(index));
    }
    for (qsizetype index = 0; index < change.insertedLength; ++index) {
        newFormats.append(insertedFormat);
    }
    const qsizetype oldSuffixStart = oldLength - commonSuffix;
    for (qsizetype index = oldSuffixStart; index < oldLength; ++index) {
        newFormats.append(oldFormats.at(index));
    }

    QVector<CharacterSpan> newSpans;
    if (newFormats.isEmpty()) {
        newSpans.append(CharacterSpan{0, 0, insertedFormat});
    } else {
        qsizetype spanStart = 0;
        CharacterFormat spanFormat = newFormats.constFirst();
        for (qsizetype index = 1; index <= newFormats.size(); ++index) {
            if (index < newFormats.size()
                && sameCharacterFormat(spanFormat, newFormats.at(index))) {
                continue;
            }
            newSpans.append(CharacterSpan{spanStart, index - spanStart, spanFormat});
            if (index < newFormats.size()) {
                spanStart = index;
                spanFormat = newFormats.at(index);
            }
        }
    }

    QVector<ParagraphFormat> newParagraphFormats = paragraphFormatsForText(
        text,
        ParagraphFormat());
    const auto oldParagraphAt = [&oldParagraphFormats](qsizetype position) {
        for (const ParagraphFormat &format : oldParagraphFormats) {
            if (position >= format.start
                && position <= format.start + format.length) {
                return format;
            }
        }
        return oldParagraphFormats.isEmpty()
            ? ParagraphFormat()
            : oldParagraphFormats.constLast();
    };
    for (ParagraphFormat &format : newParagraphFormats) {
        qsizetype oldPosition = change.start;
        if (format.start < change.start) {
            oldPosition = format.start;
        } else if (format.start >= change.start + change.insertedLength) {
            oldPosition = format.start
                - change.insertedLength
                + change.removedLength;
        }
        ParagraphFormat inheritedFormat = oldParagraphAt(
            qBound<qsizetype>(0, oldPosition, oldLength));
        inheritedFormat.start = format.start;
        inheritedFormat.length = format.length;
        format = inheritedFormat;
    }
    draft.text = text;
    draft.characterSpans = std::move(newSpans);
    draft.paragraphFormats = std::move(newParagraphFormats);
    return change;
}

void PDFEditController::applyUnicodeFontFallback(Draft &draft)
{
    if (draft.text == draft.candidate.text || !containsNonAscii(draft.text)) {
        return;
    }

    const CharacterFormat originalFormat = characterFormatForCandidate(
        draft.candidate);
    const QString bundledArial = matchingBundledFamilyName(
        QStringLiteral("Arial"));
    for (CharacterSpan &span : draft.characterSpans) {
        const qsizetype start = qBound<qsizetype>(
            0,
            span.start,
            draft.text.size());
        const qsizetype length = qBound<qsizetype>(
            0,
            span.length,
            draft.text.size() - start);
        if (!containsNonAscii(draft.text.sliced(start, length))) {
            continue;
        }

        const QString bundledFamily = matchingBundledFamilyName(
            span.format.fontFamily);
        const QString systemFamily = matchingSystemFamilyName(
            span.format.fontFamily);
        if (!bundledFamily.isEmpty()) {
            span.format.fontFamily = bundledFamily;
        } else if (!systemFamily.isEmpty()
                   && !isGenericArialCompatibleFontFamily(
                       span.format.fontFamily)) {
            span.format.fontFamily = systemFamily;
        } else if (!bundledArial.isEmpty()
                   && equivalentFontFamilyName(
                       span.format.fontFamily,
                       originalFormat.fontFamily)
                   && isArialCompatibleFontFamily(span.format.fontFamily)) {
            span.format.fontFamily = bundledArial;
        } else if (!systemFamily.isEmpty()) {
            span.format.fontFamily = systemFamily;
        }
    }
}

bool PDFEditController::updateCharacterFormats(
    Draft &draft,
    qsizetype start,
    qsizetype length,
    const std::function<void(CharacterFormat &)> &update)
{
    const qsizetype textLength = draft.text.size();
    const CharacterFormat fallbackFormat = primaryCharacterFormat(draft);
    if (textLength == 0) {
        CharacterFormat updatedFormat = fallbackFormat;
        update(updatedFormat);
        if (sameCharacterFormat(fallbackFormat, updatedFormat)) {
            return false;
        }
        draft.characterSpans = {CharacterSpan{0, 0, updatedFormat}};
        return true;
    }

    const qsizetype rangeStart = qBound<qsizetype>(0, start, textLength);
    const qsizetype rangeEnd = qBound<qsizetype>(
        rangeStart,
        rangeStart + length,
        textLength);
    if (rangeStart == rangeEnd) {
        return false;
    }

    QVector<CharacterFormat> formats(textLength, fallbackFormat);
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        const qsizetype spanStart = qBound<qsizetype>(0, span.start, textLength);
        const qsizetype spanEnd = qBound<qsizetype>(
            spanStart,
            span.start + span.length,
            textLength);
        for (qsizetype index = spanStart; index < spanEnd; ++index) {
            formats[index] = span.format;
        }
    }

    bool changed = false;
    for (qsizetype index = rangeStart; index < rangeEnd; ++index) {
        const CharacterFormat previousFormat = formats.at(index);
        update(formats[index]);
        changed = changed
            || !sameCharacterFormat(previousFormat, formats.at(index));
    }
    if (!changed) {
        return false;
    }

    QVector<CharacterSpan> spans;
    qsizetype spanStart = 0;
    CharacterFormat spanFormat = formats.constFirst();
    for (qsizetype index = 1; index <= formats.size(); ++index) {
        if (index < formats.size()
            && sameCharacterFormat(spanFormat, formats.at(index))) {
            continue;
        }
        spans.append(CharacterSpan{spanStart, index - spanStart, spanFormat});
        if (index < formats.size()) {
            spanStart = index;
            spanFormat = formats.at(index);
        }
    }
    draft.characterSpans = std::move(spans);
    return true;
}

QRectF PDFEditController::expandedTextEditFrame(const Draft &draft)
{
    const Candidate &selectedCandidate = draft.candidate;
    const QRectF frameRect = draft.frameRect.normalized();
    if (!m_document
        || selectedCandidate.kind != QLatin1String("text")
        || selectedCandidate.added
        || frameRect.isEmpty()) {
        return draft.frameRect;
    }

    const QSizeF pageSize = m_document->pageSize(selectedCandidate.page);
    if (pageSize.isEmpty()) {
        return draft.frameRect;
    }

    const qreal minimumPageMargin = qBound<qreal>(
        0.0,
        kTextEditMinimumPageMarginPoints / pageSize.width(),
        kTextEditMaximumPageMarginRatio);
    const qreal minimumExpansion = kTextEditMinimumExpansionPoints
        / pageSize.width();
    const qreal currentRight = qBound<qreal>(
        frameRect.left(),
        frameRect.right(),
        1.0);
    const QRectF selectedSourceRect
        = selectedCandidate.geometry.sourceRect.normalized();
    const QRectF initialFrameRect
        = (selectedCandidate.initialDraftFrameRect.isEmpty()
               ? selectedSourceRect
               : selectedCandidate.initialDraftFrameRect)
              .normalized();
    const qreal boundaryAnchorRight = qBound<qreal>(
        frameRect.left(),
        qMax(selectedSourceRect.right(), initialFrameRect.right()),
        1.0);
    qreal inferredLeftMargin = 1.0;
    qreal observedRight = currentRight;
    qreal nearestRowObstacle = 2.0;
    // Paragraph bounds span multiple rows. Keep the extra obstacle tolerance
    // proportional to one row so nearby paragraphs do not act as column edges.
    const auto rowHeight = [](const Candidate &candidate, const QRectF &rect) {
        return rect.height() / (candidate.text.count(QLatin1Char('\n')) + 1);
    };
    const qreal selectedRowHeight = rowHeight(selectedCandidate, selectedSourceRect);

    const auto considerCandidate = [&](const Candidate &candidate) {
        if (candidate.textMarkupHitTestOnly
            || (candidate.kind != QLatin1String("text")
             && candidate.kind != QLatin1String("addedText"))
            || candidate.geometry.sourceRect.isEmpty()) {
            return;
        }

        const QRectF sourceRect = candidate.geometry.sourceRect.normalized();
        QRectF effectiveRect = sourceRect;
        const auto candidateDraft = m_drafts.constFind(
            draftKey(candidate.page, candidate.objectId));
        if (candidateDraft != m_drafts.cend()) {
            if (candidateDraft->text.isEmpty()) {
                return;
            }
            effectiveRect = candidateDraft->frameRect.normalized();
        } else if (candidate.added && candidate.text.isEmpty()) {
            return;
        }

        inferredLeftMargin = qMin(inferredLeftMargin, effectiveRect.left());
        observedRight = qMax(observedRight, effectiveRect.right());
        if (candidate.page != selectedCandidate.page
            || candidate.objectId == selectedCandidate.objectId
            || effectiveRect.left()
                <= selectedSourceRect.left() + minimumExpansion) {
            return;
        }

        const qreal rowTolerance = kTextEditRowToleranceRatio
            * qMax(selectedRowHeight, rowHeight(candidate, effectiveRect));
        const bool sharesVisualRow
            = effectiveRect.top() <= selectedSourceRect.bottom() + rowTolerance
            && effectiveRect.bottom() >= selectedSourceRect.top() - rowTolerance;
        if (sharesVisualRow) {
            nearestRowObstacle = qMin(
                nearestRowObstacle,
                effectiveRect.left());
        }
    };

    const QVector<Candidate> &pageCandidates = ensurePageCandidates(
        selectedCandidate.page);
    for (const Candidate &candidate : pageCandidates) {
        considerCandidate(candidate);
    }
    for (const Candidate &candidate : std::as_const(m_addedCandidates)) {
        if (candidate.page == selectedCandidate.page) {
            considerCandidate(candidate);
        }
    }

    const qreal inferredMargin = inferredLeftMargin < 1.0
        ? qBound<qreal>(
              minimumPageMargin,
              inferredLeftMargin,
              kTextEditMaximumPageMarginRatio)
        : minimumPageMargin;
    qreal pageRight = qMax(1.0 - inferredMargin, observedRight);
    pageRight = qBound<qreal>(currentRight, pageRight, 1.0);

    qreal targetRight = pageRight;
    if (nearestRowObstacle <= 1.0) {
        const qreal fixedColumnRight = nearestRowObstacle <= boundaryAnchorRight
            ? boundaryAnchorRight
            : boundaryAnchorRight
                + (nearestRowObstacle - boundaryAnchorRight) / 2.0;
        targetRight = qMax(
            currentRight,
            qMin(pageRight, fixedColumnRight));
    }
    if (targetRight - currentRight < minimumExpansion) {
        return draft.frameRect;
    }

    return QRectF(
        frameRect.x(),
        frameRect.y(),
        targetRight - frameRect.x(),
        frameRect.height());
}

PDFEditController::Draft PDFEditController::selectedDraftForDisplay() const
{
    if (!m_hasSelection) {
        return Draft();
    }

    if (m_textEditActive
        && m_textEditDisplayDraftActive
        && m_textEditDisplayDraft.candidate.page == m_selectedCandidate.page
        && m_textEditDisplayDraft.candidate.objectId
            == m_selectedCandidate.objectId) {
        return m_textEditDisplayDraft;
    }

    const auto draft = m_drafts.constFind(
        draftKey(m_selectedCandidate.page, m_selectedCandidate.objectId));
    return draft == m_drafts.cend()
        ? initialDraftForCandidate(m_selectedCandidate)
        : *draft;
}

void PDFEditController::selectedTextRangeForDraft(const Draft &draft,
                                                  qsizetype &start,
                                                  qsizetype &length) const
{
    const qsizetype textLength = draft.text.size();
    if (m_textEditActive && m_selectedTextRangeLength > 0) {
        start = qBound<qsizetype>(0, m_selectedTextRangeStart, textLength);
        length = qBound<qsizetype>(
            0,
            m_selectedTextRangeLength,
            textLength - start);
        return;
    }

    start = 0;
    length = textLength;
}

void PDFEditController::selectedParagraphRangeForDraft(
    const Draft &draft,
    qsizetype &first,
    qsizetype &last) const
{
    first = -1;
    last = -1;
    if (draft.paragraphFormats.isEmpty()) {
        return;
    }
    if (!m_textEditActive) {
        first = 0;
        last = draft.paragraphFormats.size() - 1;
        return;
    }

    const qsizetype textLength = draft.text.size();
    const qsizetype rangeStart = qBound<qsizetype>(
        0,
        m_selectedTextRangeStart,
        textLength);
    const qsizetype rangeLength = qBound<qsizetype>(
        0,
        m_selectedTextRangeLength,
        textLength - rangeStart);
    const qsizetype rangeEnd = rangeLength > 0
        ? rangeStart + rangeLength - 1
        : rangeStart;
    const auto paragraphAt = [&draft](qsizetype position) {
        for (qsizetype index = 0;
             index < draft.paragraphFormats.size();
             ++index) {
            const ParagraphFormat &format = draft.paragraphFormats.at(index);
            if (position >= format.start
                && position <= format.start + format.length) {
                return index;
            }
        }
        return draft.paragraphFormats.size() - 1;
    };

    first = paragraphAt(rangeStart);
    last = paragraphAt(rangeEnd);
    if (last < first) {
        std::swap(first, last);
    }
}

void PDFEditController::resetTextEditDisplayState()
{
    resetTypingFormat();
    m_textEditDisplayDraft = Draft();
    m_textEditDisplayDraftActive = false;
    m_textEditFrameAutomaticallyExpanded = false;
    m_textEditGeometryExplicitlyChanged = false;
    m_textEditInitialDisplayFrameRect = QRectF();
}

void PDFEditController::resetTypingFormat()
{
    m_typingCharacterFormat = CharacterFormat();
    m_typingCharacterFormatPosition = -1;
    m_typingCharacterFormatActive = false;
}

void PDFEditController::rebaseTypingFormat(const Draft &draft)
{
    if (!m_typingCharacterFormatActive) {
        return;
    }

    const bool fontBold = m_typingCharacterFormat.fontBold;
    const bool fontItalic = m_typingCharacterFormat.fontItalic;
    const bool fontUnderline = m_typingCharacterFormat.fontUnderline;
    const qreal fontSizeRatio = m_typingCharacterFormat.fontSizeRatio;
    const qreal displayFontSizeRatio = m_typingCharacterFormat.displayFontSizeRatio;
    m_typingCharacterFormat = characterFormatAtPosition(
        draft,
        m_typingCharacterFormatPosition,
        true);
    m_typingCharacterFormat.fontBold = fontBold;
    m_typingCharacterFormat.fontItalic = fontItalic;
    m_typingCharacterFormat.fontUnderline = fontUnderline;
    m_typingCharacterFormat.fontSizeRatio = fontSizeRatio;
    m_typingCharacterFormat.displayFontSizeRatio = displayFontSizeRatio;
}
