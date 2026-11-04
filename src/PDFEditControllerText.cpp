#include "PDFEditController.h"
#include "PDFFontAliases.h"
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
#include <QClipboard>
#include <QGuiApplication>
#include <QDataStream>
#include <QMimeData>
#include <QTextDocumentFragment>
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QPolygonF>
#include <QQuickTextDocument>
#include <QRawFont>
#include <QRunnable>
#include <QScopedValueRollback>
#include <QSet>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBoundaryFinder>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextLayout>
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
constexpr int kEditorLetterSpacingRatio = QTextFormat::UserProperty + 3;
constexpr int kEditorWordSpacingRatio = QTextFormat::UserProperty + 4;
constexpr int kEditorLineSpacing = QTextFormat::UserProperty + 5;
constexpr int kEditorParagraphSpaceBeforeRatio = QTextFormat::UserProperty + 6;
constexpr int kEditorParagraphSpaceAfterRatio = QTextFormat::UserProperty + 7;
constexpr int kEditorSourceAdvanceRatio = QTextFormat::UserProperty + 8;
// The rendered face may change when a private, single-face PDF family is
// restyled. Retain the model family/stretch so toggling back restores its face
// and metrics, including through local undo/redo.
constexpr int kEditorFontFamily = QTextFormat::UserProperty + 9;
constexpr int kEditorFontStretch = QTextFormat::UserProperty + 10;
constexpr int kEditorFirstLineIndentRatio = QTextFormat::UserProperty + 11;
constexpr int kEditorVirtualSpaceRatio = QTextFormat::UserProperty + 12;
constexpr int kEditorTextShear = QTextFormat::UserProperty + 13;

// Keep a cell's physical width independent of its font, zoom and user tracking.
// The marker participates in QTextDocument history; a literal space has zero.
void applyVirtualSpaceWidth(QTextCharFormat &format, qreal ratio, qreal pageHeight)
{
    format.setProperty(kEditorVirtualSpaceRatio, ratio);
    if (ratio <= 0.0 || pageHeight <= 0.0)
        return;
    QFont font = format.font();
#ifdef Q_OS_WIN
    // Character formats omit the editor's inherited hinting preference. Measure
    // the same fractional advances as fontForDisplay(), not rounded pixels.
    font.setHintingPreference(QFont::PreferNoHinting);
#endif
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
    font.setWordSpacing(0.0);
    font.setKerning(false);
    const qreal natural = QFontMetricsF(font).horizontalAdvance(QLatin1Char(' '));
    format.setFontKerning(false);
    format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    const qreal spacing = std::round((ratio * pageHeight - natural) * 64.0) / 64.0;
    font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    // QFont's fixed-point conversion rounds negative tracking toward zero.
    // Compensate the measured conversion, not the semantic cell width.
    format.setFontLetterSpacing(spacing + spacing - font.letterSpacing());
    format.setFontWordSpacing(0.0);
}

// A virtual cell has no independent PDF ink; its underline follows the real
// glyph carrying the advance. Keep that true after typing or deleting anchors.
void normalizeVirtualSpaceUnderline(QTextDocument *document, QTextCursor &cursor, bool &merging)
{
    bool underline = false;
    for (int position = 0; position < document->characterCount() - 1; ++position) {
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat current = cursor.charFormat();
        const QChar character = document->characterAt(position);
        if (character == QLatin1Char(' ')
            && current.doubleProperty(kEditorVirtualSpaceRatio) > 0.0) {
            if (current.fontUnderline() != underline) {
                if (!merging) {
                    cursor.joinPreviousEditBlock();
                    merging = true;
                }
                QTextCharFormat decoration;
                decoration.setFontUnderline(underline);
                cursor.mergeCharFormat(decoration);
            }
        } else {
            underline = character != QChar::ParagraphSeparator
                && character != QChar::LineSeparator && current.fontUnderline();
        }
    }
}

// Qt adds the insertion length and following format length when calculating
// the redo cursor. A final custom item makes it use the changed text bounds
// instead, without adding another user-visible undo step.
class EditorTypingCursorBoundary final : public QAbstractUndoItem
{
public:
    void undo() override {}
    void redo() override {}
};

// Qt places the undo cursor one code unit past a block-format command. That
// skips an empty paragraph, and an edit block begun at position zero can jump
// to the first paragraph. Restore the user's range after Qt finishes its own
// cursor update, without replacing the document or creating another undo step.
class EditorParagraphSelectionBoundary final : public QAbstractUndoItem
{
public:
    EditorParagraphSelectionBoundary(QQuickTextDocument *document,
                                     const PDFEditController *controller)
        : m_document(document), m_controller(controller),
          m_objectId(controller->selectedObjectId())
    {
        const QObject *editor = document->parent();
        m_start = editor->property("selectionStart").toInt();
        m_end = editor->property("selectionEnd").toInt();
        if (editor->property("cursorPosition").toInt() == m_start) {
            std::swap(m_start, m_end);
        }
    }

    void undo() override { restore(); }
    void redo() override { restore(); }

private:
    void restore()
    {
        if (!m_document || !m_document->parent()) {
            return;
        }
        const QPointer<QQuickTextDocument> document = m_document;
        const QPointer<const PDFEditController> controller = m_controller;
        const QString objectId = m_objectId;
        const int start = m_start;
        const int end = m_end;
        QMetaObject::invokeMethod(m_document->parent(),
            [document, controller, objectId, start, end]() {
                if (!document || !document->parent() || !document->textDocument()
                    || !controller || !controller->textEditActive()
                    || controller->selectedObjectId() != objectId) {
                    return;
                }
                const int maximum = qMax(0, document->textDocument()->characterCount() - 1);
                QMetaObject::invokeMethod(document->parent(), "select",
                    Q_ARG(int, qBound(0, start, maximum)),
                    Q_ARG(int, qBound(0, end, maximum)));
            }, Qt::QueuedConnection);
    }

    QPointer<QQuickTextDocument> m_document;
    QPointer<const PDFEditController> m_controller;
    QString m_objectId;
    int m_start = 0;
    int m_end = 0;
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
        connect(document->textDocument(), &QTextDocument::undoCommandAdded, this, [this]() {
            if (!m_closingNativeBlock) {
                m_newCommand = true;
            }
        });
        if (document->parent()) {
            document->parent()->installEventFilter(this);
        }
    }

    void setPageHeight(qreal height) { m_pageHeight = height; }
    bool insertedAt(int position) const {
        return position >= 0 && position < m_inserted.size() && m_inserted.at(position);
    }
    qreal pastedVirtualRatio(int position) const {
        const int relative = position - m_pasteStart;
        return relative >= 0 && relative < m_pastedWidths.size() && m_pageHeight > 0
            ? m_pastedWidths.at(relative) / m_pageHeight : 0.0;
    }

    // QQuickTextEdit destroys its QTextDocument before QObject children. Never
    // dereference the document from this child's destructor.
    ~EditorTypingEditBlock() override = default;

    QTextCharFormat pastedFormat(int position) const {
        const int relative = position - m_pasteStart;
        return relative >= 0 && relative < m_pastedFormats.size()
            ? m_pastedFormats.at(relative) : QTextCharFormat();
    }
    bool hasPastedFormats() const { return !m_pastedFormats.isEmpty(); }

    bool takePendingTextEdit(bool *nativeEdit = nullptr)
    {
        finish();
        // A queued format synchronization can leave undoCommandAdded pending
        // until after Undo. A new edit clears redo history; Undo must instead
        // retain the character properties restored by QTextDocument.
        const bool pending = m_nativeEdit
            || (m_newCommand && !m_document->textDocument()->isRedoAvailable());
        if (nativeEdit)
            *nativeEdit = m_nativeEdit;
        if (!m_nativeEdit)
            m_inserted.clear();
        m_nativeEdit = false;
        m_newCommand = false;
        return pending;
    }

protected:
    bool eventFilter(QObject *editor, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease
            || event->type() == QEvent::InputMethod) {
            finish();
        }
        bool insertsText = false;
        bool cut = false;
        bool paste = false;
        int replacementStart = editor->property("selectionStart").toInt();
        int replacementEnd = editor->property("selectionEnd").toInt();
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->matches(QKeySequence::Copy)) {
                copySelection(editor);
                return true;
            }
            cut = key->matches(QKeySequence::Cut);
            paste = key->matches(QKeySequence::Paste);
            insertsText = (!key->text().isEmpty()
                && !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)))
                || key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete
                || key->matches(QKeySequence::Paste) || key->matches(QKeySequence::Cut);
        } else if (event->type() == QEvent::InputMethod) {
            const auto *input = static_cast<QInputMethodEvent *>(event);
            insertsText = !input->commitString().isEmpty() || input->replacementLength() > 0;
            if (replacementStart == replacementEnd) {
                replacementStart += input->replacementStart();
                replacementEnd = replacementStart + input->replacementLength();
            }
        }
        if (insertsText) {
            m_nativeEdit = true;
            m_nativeChangeStart = replacementStart;
            m_nativeChangeEnd = replacementEnd;
            m_inserted.fill(false, qMax(0, m_document->textDocument()->characterCount() - 1));
            m_pastedWidths.clear();
            m_pastedFormats.clear();
            m_pasteStart = editor->property("selectionStart").toInt();
            if (paste) {
                const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
                const QByteArray payload = mime ? mime->data(virtualSpaceMime()) : QByteArray();
                // Decode only a bounded self-contained payload; clipboard data is untrusted.
                if (!payload.isEmpty() && payload.size() <= 1024 * 1024) {
                    QDataStream stream(payload);
                    const QString text = mime->text();
                    quint32 count = 0;
                    stream >> count;
                    char digest[32];
                    const int digestSize = stream.readRawData(digest, sizeof(digest));
                    if (count == quint32(text.size()) && count <= 32768
                        && payload.size() == 4 + 32 + qint64(count) * 8
                        && digestSize == 32
                        && QByteArray(digest, 32) == QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256)) {
                        QVector<qreal> widths;
                        for (quint32 index = 0; index < count; ++index) {
                            double width = 0.0;
                            stream >> width;
                            if (!qIsFinite(width) || width < 0.0 || width > 512.0
                                || (width > 0.0 && text.at(index) != QLatin1Char(' '))) {
                                widths.clear();
                                break;
                            }
                            widths.append(width);
                        }
                        if (stream.status() == QDataStream::Ok && widths.size() == text.size())
                            m_pastedWidths = widths;
                    }
                }
                if (!m_pastedWidths.isEmpty()) {
                    const QByteArray data = mime->data(characterFormatMime());
                    if (data.size() <= 4 * 1024 * 1024) {
                        const QJsonArray formats = QJsonDocument::fromJson(data).array();
                        if (formats.size() == m_pastedWidths.size()) {
                            for (const QJsonValue &value : formats) {
                                const QJsonObject record = value.toObject();
                                const QString family = record.value(QStringLiteral("family")).toString();
                                const double size = record.value(QStringLiteral("size")).toDouble(-1);
                                const double displaySize = record.value(QStringLiteral("displaySize")).toDouble(-1);
                                const double letter = record.value(QStringLiteral("letter")).toDouble(1000);
                                const double word = record.value(QStringLiteral("word")).toDouble(1000);
                                const double shear = record.value(QStringLiteral("textShear")).toDouble();
                                const int stretch = record.value(QStringLiteral("stretch")).toInt(-1);
                                const QColor color(record.value(QStringLiteral("color")).toString());
                                if (family.isEmpty() || family.size() > 1024
                                    || !qIsFinite(size) || size <= 0 || size > 512
                                    || !qIsFinite(displaySize) || displaySize < 0 || displaySize > 512
                                    || !qIsFinite(letter) || qAbs(letter) > 512
                                    || !qIsFinite(word) || qAbs(word) > 512
                                    || !qIsFinite(shear) || qAbs(shear) > 0.5
                                    || stretch < 1 || stretch > 4000 || !color.isValid()) {
                                    m_pastedFormats.clear();
                                    break;
                                }
                                QTextCharFormat format;
                                const bool bold = record.value(QStringLiteral("bold")).toBool();
                                const bool italic = record.value(QStringLiteral("italic")).toBool();
                                const QString face = ApplicationFonts::displayFamilyForStyle(family, bold, italic);
                                const int pixels = qMax(1, qRound(displaySize > 0 ? displaySize : size));
                                format.setFontFamilies({face.isEmpty() ? family : face});
                                format.setFontWeight(bold ? QFont::Bold : QFont::Normal);
                                format.setFontItalic(italic);
                                format.setProperty(kEditorTextShear, italic ? 0.0 : shear);
                                format.setFontUnderline(record.value(QStringLiteral("underline")).toBool());
                                format.setForeground(color);
                                format.setProperty(QTextFormat::FontPixelSize, pixels);
                                format.setFontStretch(ApplicationFonts::calibratedFontStretch(face, stretch, bold, italic, pixels));
                                format.setProperty(kEditorFontFamily, family);
                                format.setProperty(kEditorFontStretch, stretch);
                                format.setProperty(kEditorFontSizeRatio, size / m_pageHeight);
                                format.setProperty(kEditorDisplayFontSizeRatio, displaySize / m_pageHeight);
                                format.setProperty(kEditorLetterSpacingRatio, letter / m_pageHeight);
                                format.setProperty(kEditorWordSpacingRatio, word / m_pageHeight);
                                m_pastedFormats.append(format);
                            }
                        }
                    }
                }
            }
            m_nativeEventPending = true;
            m_nativeTextBefore = m_document->textDocument()
                ? m_document->textDocument()->toPlainText() : QString();
            QMetaObject::invokeMethod(this, [this]() { finish(); }, Qt::QueuedConnection);
        }
        if (insertsText && m_enabled() && m_cursor.isNull()
            && m_document->textDocument()) {
            m_cursor = QTextCursor(m_document->textDocument());
            m_cursor.setPosition(qBound(0, editor->property("cursorPosition").toInt(),
                                         m_document->textDocument()->characterCount() - 1));
            m_cursor.beginEditBlock();
        }
        if (cut) {
            copySelection(editor);
            QTextCursor cursor(m_document->textDocument());
            const int start = editor->property("selectionStart").toInt();
            cursor.setPosition(start);
            cursor.setPosition(editor->property("selectionEnd").toInt(), QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            editor->setProperty("cursorPosition", start);
            return true;
        }
        return false;
    }

private:
    static QString characterFormatMime() { return QStringLiteral("application/x-trepdf-character-formats-v1"); }
    static QString virtualSpaceMime() { return QStringLiteral("application/x-trepdf-virtual-spaces-v1"); }

    void copySelection(QObject *editor)
    {
        const int start = editor->property("selectionStart").toInt();
        const int end = editor->property("selectionEnd").toInt();
        if (start >= end)
            return;
        QTextCursor cursor(m_document->textDocument());
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        const QTextDocumentFragment fragment(cursor);
        const QString text = fragment.toPlainText();
        auto *mime = new QMimeData;
        mime->setText(text);
        mime->setHtml(fragment.toHtml());
        if (text.size() <= 32768 && m_pageHeight > 0.0) {
            QByteArray payload;
            QDataStream stream(&payload, QIODevice::WriteOnly);
            stream << quint32(text.size());
            const QByteArray digest = QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256);
            stream.writeRawData(digest.constData(), digest.size());
            QJsonArray formats;
            for (int position = start; position < end; ++position) {
                cursor.setPosition(position);
                cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
                const QTextCharFormat format = cursor.charFormat();
                stream << double(text.at(position - start) == QLatin1Char(' ')
                    ? format.doubleProperty(kEditorVirtualSpaceRatio) * m_pageHeight : 0.0);
                formats.append(QJsonObject{
                    {QStringLiteral("family"), format.stringProperty(kEditorFontFamily)},
                    {QStringLiteral("size"), format.doubleProperty(kEditorFontSizeRatio) * m_pageHeight},
                    {QStringLiteral("displaySize"), format.doubleProperty(kEditorDisplayFontSizeRatio) * m_pageHeight},
                    {QStringLiteral("letter"), format.doubleProperty(kEditorLetterSpacingRatio) * m_pageHeight},
                    {QStringLiteral("word"), format.doubleProperty(kEditorWordSpacingRatio) * m_pageHeight},
                    {QStringLiteral("stretch"), format.intProperty(kEditorFontStretch)},
                    {QStringLiteral("bold"), format.fontWeight() >= QFont::Bold},
                    {QStringLiteral("italic"), format.fontItalic()},
                    {QStringLiteral("textShear"), format.doubleProperty(kEditorTextShear)},
                    {QStringLiteral("underline"), format.fontUnderline()},
                    {QStringLiteral("color"), format.foreground().color().name(QColor::HexArgb)}});
            }
            mime->setData(virtualSpaceMime(), payload);
            mime->setData(characterFormatMime(), QJsonDocument(formats).toJson(QJsonDocument::Compact));
        }
        QGuiApplication::clipboard()->setMimeData(mime);
    }

    void finish()
    {
        if (!m_cursor.isNull()) {
            QTextCursor cursor = std::exchange(m_cursor, QTextCursor());
            // Preedit can queue QML synchronization before the native block
            // closes. Its final undoCommandAdded is not another text edit:
            // otherwise the next undo would invalidate restored spacing again.
            QScopedValueRollback<bool> closing(m_closingNativeBlock, true);
            cursor.endEditBlock();
        }
        if (m_nativeEventPending) {
            const QString after = m_document->textDocument()->toPlainText();
            const int start = qBound(0, m_nativeChangeStart, int(m_nativeTextBefore.size()));
            const int end = qBound(start, m_nativeChangeEnd, int(m_nativeTextBefore.size()));
            const int inserted = after.size() - (m_nativeTextBefore.size() - (end - start));
            m_inserted.fill(false, after.size());
            // Preedit can report a whole-block contentsChange even though only
            // its commit changed text. Verify the native replacement boundary
            // against both immutable sides, never mark that broad signal range.
            if (inserted >= 0 && start + inserted <= after.size()
                && after.first(start) == m_nativeTextBefore.first(start)
                && after.sliced(start + inserted) == m_nativeTextBefore.sliced(end)) {
                for (int position = start; position < start + inserted; ++position)
                    m_inserted[position] = true;
            }
            // Boundary Delete/Backspace and empty paste produce no textChanged
            // signal. Do not let their pending flag invalidate the next undo.
            if (m_document->textDocument()
                && start == end
                && after == m_nativeTextBefore) {
                m_nativeEdit = false;
                m_newCommand = false;
            }
            m_nativeEventPending = false;
            m_nativeTextBefore.clear();
        }
    }

    QQuickTextDocument *m_document;
    std::function<bool()> m_enabled;
    QTextCursor m_cursor;
    bool m_nativeEdit = false;
    bool m_newCommand = false;
    bool m_closingNativeBlock = false;
    bool m_nativeEventPending = false;
    QString m_nativeTextBefore;
    QVector<bool> m_inserted;
    QVector<qreal> m_pastedWidths;
    QVector<QTextCharFormat> m_pastedFormats;
    qreal m_pageHeight = 0.0;
    int m_pasteStart = 0;
    int m_nativeChangeStart = 0;
    int m_nativeChangeEnd = 0;
};

// Header input is temporary and has no selected Draft. Keep its typing color
// with the editor, while actual character colors live in QTextDocument history.
// In particular, QTextCursor::mergeCharFormat at an empty selection must never
// be forwarded through QQuickTextEdit's word-formatting cursorSelection API.
class HeaderTextEditor final : public QObject
{
public:
    explicit HeaderTextEditor(QQuickTextDocument *document)
        : QObject(document), m_document(document)
    {
        setObjectName(QStringLiteral("trepdfHeaderTextEditor"));
        resetChanges();
        m_cursorPosition = currentCursorPosition();
        m_undoSteps = textDocument()->availableUndoSteps();
        connect(textDocument(), &QTextDocument::contentsChange, this,
                [this](int position, int removed, int added) {
            if (m_formatting || (removed == 0 && added == 0)) {
                return;
            }
            // Track the actual changed range, not a string diff: replacing a
            // selected "a" with another "a" still inserts a new colored glyph.
            const int start = qBound(0, position, int(m_inserted.size()));
            const int count = qBound(0, removed, int(m_inserted.size()) - start);
            m_inserted.remove(start, count);
            m_inserted.insert(start, qMax(0, added), true);
            m_changed = true;
        });
        connect(textDocument(), &QTextDocument::undoCommandAdded, this, [this]() {
            if (!m_formatting) {
                m_newCommand = true;
            }
        });
        if (document->parent()) {
            document->parent()->installEventFilter(this);
        }
    }

    void initialize(const QVariantList &colors, int typingPosition,
                    const QColor &typingColor, const QVariantList &families,
                    int typingFamilyPosition, const QString &typingFamily)
    {
        synchronize();
        QScopedValueRollback<bool> formatting(m_formatting, true);
        QTextDocument *document = textDocument();
        const bool undoEnabled = document->isUndoRedoEnabled();
        document->setUndoRedoEnabled(false);
        const int length = document->toPlainText().size();
        m_defaultFamily = PDFFontAliases::semanticFamily(document->defaultFont().family());
        QTextCursor cursor(document);
        for (int start = 0; start < length;) {
            const QColor color = storedColor(colors, start);
            const QString family = start < families.size()
                ? families.at(start).toString() : m_defaultFamily;
            int end = start + 1;
            while (end < length && storedColor(colors, end) == color
                   && (end < families.size() ? families.at(end).toString() : m_defaultFamily) == family) {
                ++end;
            }
            QTextCharFormat format;
            format.setForeground(color);
            format.setFontFamilies({family});
            cursor.setPosition(start);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(format);
            start = end;
        }
        document->setUndoRedoEnabled(undoEnabled);
        m_cursorPosition = currentCursorPosition();
        m_typingPosition = typingPosition == m_cursorPosition
            && validColor(typingColor) ? typingPosition : -1;
        m_typingColor = validColor(typingColor) ? typingColor : QColor(Qt::black);
        m_typingFamilyPosition = typingFamilyPosition == m_cursorPosition
            && !typingFamily.isEmpty() ? typingFamilyPosition : -1;
        m_typingFamily = typingFamily;
        m_undoSteps = document->availableUndoSteps();
        resetChanges();
    }

    QVariantMap state()
    {
        synchronize();
        const QString text = textDocument()->toPlainText();
        QVariantList colors;
        QVariantList families;
        colors.reserve(text.size());
        families.reserve(text.size());
        for (int position = 0; position < text.size(); ++position) {
            colors.append(colorAt(position));
            families.append(familyAt(position));
        }
        return {{QStringLiteral("text"), text},
                {QStringLiteral("colors"), colors},
                {QStringLiteral("fontFamilies"), families},
                {QStringLiteral("fontFamily"), m_typingFamilyPosition >= 0
                    ? m_typingFamily : familyAt(qMax(0, m_cursorPosition - 1))},
                {QStringLiteral("typingFamilyPosition"), m_typingFamilyPosition},
                {QStringLiteral("cursorPosition"), m_cursorPosition},
                {QStringLiteral("typingPosition"), m_typingPosition},
                {QStringLiteral("color"), m_typingPosition >= 0
                    ? m_typingColor : colorAt(qMax(0, m_cursorPosition - 1))}};
    }

    void setColor(const QColor &color)
    {
        if (!validColor(color)) {
            return;
        }
        synchronize();
        m_typingColor = color;
        m_typingPosition = m_cursorPosition;
    }

    void setFontFamily(const QString &family)
    {
        if (family.isEmpty())
            return;
        synchronize();
        const int length = textDocument()->toPlainText().size();
        const int start = qBound(0, m_document->parent()->property("selectionStart").toInt(), length);
        const int end = qBound(start, m_document->parent()->property("selectionEnd").toInt(), length);
        if (start < end) {
            QScopedValueRollback<bool> formatting(m_formatting, true);
            QTextCursor cursor(textDocument());
            cursor.setPosition(start);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
            QTextCharFormat format;
            format.setFontFamilies({family});
            cursor.mergeCharFormat(format);
            m_undoSteps = textDocument()->availableUndoSteps();
            resetChanges();
        }
        m_typingFamily = family;
        m_typingFamilyPosition = m_cursorPosition;
    }

protected:
    bool eventFilter(QObject *editor, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease
            && event->type() != QEvent::InputMethod) {
            return false;
        }
        synchronize();
        bool changesText = false;
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            changesText = (!key->text().isEmpty()
                && !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)))
                || key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete
                || key->matches(QKeySequence::Paste) || key->matches(QKeySequence::Cut);
            if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)) {
                m_typingPosition = -1;
                m_typingFamilyPosition = -1;
            }
        } else if (event->type() == QEvent::InputMethod) {
            const auto *input = static_cast<QInputMethodEvent *>(event);
            changesText = !input->commitString().isEmpty() || input->replacementLength() > 0;
        }
        if (changesText) {
            m_nativeEdit = true;
            m_editCursor = QTextCursor(textDocument());
            m_editCursor.setPosition(currentCursorPosition());
            m_editCursor.beginEditBlock();
            QMetaObject::invokeMethod(this, [this]() { synchronize(); }, Qt::QueuedConnection);
        }
        Q_UNUSED(editor);
        return false;
    }

private:
    QTextDocument *textDocument() const { return m_document->textDocument(); }

    static bool validColor(const QColor &color)
    {
        return color.isValid() && color.alpha() == 255;
    }

    static QColor storedColor(const QVariantList &colors, int position)
    {
        const QColor color = position < colors.size()
            ? colors.at(position).value<QColor>() : QColor();
        return validColor(color) ? color : QColor(Qt::black);
    }

    int currentCursorPosition() const
    {
        return qBound(0, m_document->parent()->property("cursorPosition").toInt(),
                      qMax(0, textDocument()->characterCount() - 1));
    }

    QColor colorAt(int position) const
    {
        const int maximum = textDocument()->characterCount() - 1;
        if (position < 0 || position >= maximum) {
            return QColor(Qt::black);
        }
        QTextCursor cursor(textDocument());
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QBrush foreground = cursor.charFormat().foreground();
        return foreground.style() != Qt::NoBrush && validColor(foreground.color())
            ? foreground.color() : QColor(Qt::black);
    }

    QString familyAt(int position) const
    {
        const int maximum = textDocument()->characterCount() - 1;
        if (position < 0 || position >= maximum)
            return m_defaultFamily;
        QTextCursor cursor(textDocument());
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QStringList families = cursor.charFormat().fontFamilies().toStringList();
        return families.isEmpty() ? m_defaultFamily : PDFFontAliases::semanticFamily(families.constFirst());
    }

    void resetChanges()
    {
        m_inserted.fill(false, qMax(0, textDocument()->characterCount() - 1));
        m_changed = false;
        m_newCommand = false;
    }

    void synchronize()
    {
        if (m_synchronizing) {
            return;
        }
        QScopedValueRollback<bool> synchronizing(m_synchronizing, true);
        // Closing the native block emits its aggregated contentsChange. Keep
        // the native marker set until those exact insertion ranges are known.
        if (!m_editCursor.isNull()) {
            QTextCursor cursor = std::exchange(m_editCursor, QTextCursor());
            // Bound native pastes before publishing textChanged. Trimming in
            // that signal would create a separate undo step after this block.
            const QString text = textDocument()->toPlainText();
            if (text.size() > 700) {
                const int limit = text.at(699).isHighSurrogate() ? 699 : 700;
                cursor.setPosition(limit);
                cursor.setPosition(text.size(), QTextCursor::KeepAnchor);
                cursor.removeSelectedText();
            }
            cursor.endEditBlock();
        }
        const bool edited = m_changed && (m_nativeEdit || m_newCommand);
        const int currentPosition = currentCursorPosition();
        if (edited && (m_typingPosition >= 0 || m_typingFamilyPosition >= 0)) {
            QTextDocument *document = textDocument();
            const int maximum = qMin(int(m_inserted.size()), document->characterCount() - 1);
            bool needsFormat = false;
            for (int position = 0; position < maximum && !needsFormat; ++position) {
                needsFormat = m_inserted.at(position)
                    && ((m_typingPosition >= 0 && colorAt(position) != m_typingColor)
                        || (m_typingFamilyPosition >= 0 && familyAt(position) != m_typingFamily));
            }
            if (needsFormat) {
                QScopedValueRollback<bool> formatting(m_formatting, true);
                QTextCursor cursor(document);
                cursor.joinPreviousEditBlock();
                QTextCharFormat format;
                if (m_typingPosition >= 0)
                    format.setForeground(m_typingColor);
                if (m_typingFamilyPosition >= 0)
                    format.setFontFamilies({m_typingFamily});
                for (int start = 0; start < maximum;) {
                    if (!m_inserted.at(start)) {
                        ++start;
                        continue;
                    }
                    int end = start + 1;
                    while (end < maximum && m_inserted.at(end)) {
                        ++end;
                    }
                    cursor.setPosition(start);
                    cursor.setPosition(end, QTextCursor::KeepAnchor);
                    cursor.mergeCharFormat(format);
                    start = end;
                }
                document->appendUndoItem(new EditorTypingCursorBoundary);
                cursor.endEditBlock();
            }
            if (m_typingPosition >= 0)
                m_typingPosition = currentPosition;
            if (m_typingFamilyPosition >= 0)
                m_typingFamilyPosition = currentPosition;
        } else if ((!edited && textDocument()->availableUndoSteps() != m_undoSteps)
                   || currentPosition != m_cursorPosition) {
            m_typingPosition = -1;
            m_typingFamilyPosition = -1;
        }
        m_cursorPosition = currentPosition;
        m_undoSteps = textDocument()->availableUndoSteps();
        m_nativeEdit = false;
        resetChanges();
    }

    QQuickTextDocument *m_document;
    QTextCursor m_editCursor;
    QVector<bool> m_inserted;
    QColor m_typingColor = Qt::black;
    QString m_defaultFamily;
    QString m_typingFamily;
    int m_typingFamilyPosition = -1;
    int m_cursorPosition = 0;
    int m_typingPosition = -1;
    int m_undoSteps = 0;
    bool m_nativeEdit = false;
    bool m_changed = false;
    bool m_newCommand = false;
    bool m_formatting = false;
    bool m_synchronizing = false;
};

HeaderTextEditor *headerTextEditor(QObject *textDocument)
{
    auto *document = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!document || !document->textDocument() || !document->parent()) {
        return nullptr;
    }
    return static_cast<HeaderTextEditor *>(document->findChild<QObject *>(
        QStringLiteral("trepdfHeaderTextEditor"), Qt::FindDirectChildrenOnly));
}

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
        m_hasSelection && selectedCharacterFormatForDisplay().fontItalic);
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

    if (m_hasSelection && m_textEditActive && m_selectedTextRangeLength == 0) {
        appendRequirement(selectedCharacterFormatForDisplay());
    } else if (m_hasSelection) {
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
        case SymbolFonts:
            return QStringList(ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Symbol,
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
        ? PDFFontAliases::semanticFamily(selectedCharacterFormatForDisplay().fontFamily)
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
    if (m_selectedCandidate.preservePhysicalFontSizes || selectedFormat.textShear != 0.0)
        return std::round(points * 100.0) / 100.0;
    return qRound(points);
}

qreal PDFEditController::selectedLetterSpacing() const
{
    if (!m_document || !m_hasSelection || !isTextCandidateKind(m_selectedCandidate.kind)) {
        return 0.0;
    }
    const qreal pageHeight = m_document->pageSize(m_selectedCandidate.page).height();
    const qreal points = selectedCharacterFormatForDisplay().letterSpacingRatio * pageHeight;
    if (!m_textEditActive || m_selectedTextRangeLength > 0) {
        const Draft draft = selectedDraftForDisplay();
        qsizetype start = 0;
        qsizetype length = 0;
        selectedTextRangeForDraft(draft, start, length);
        for (const CharacterSpan &span : draft.characterSpans) {
            if (span.start < start + length && span.start + span.length > start
                && qAbs(span.format.letterSpacingRatio * pageHeight - points) > 0.001) {
                return std::numeric_limits<qreal>::quiet_NaN();
            }
        }
    }
    return points;
}

qreal PDFEditController::selectedWordSpacing() const
{
    if (!m_document || !m_hasSelection || !isTextCandidateKind(m_selectedCandidate.kind)) {
        return 0.0;
    }
    const qreal pageHeight = m_document->pageSize(m_selectedCandidate.page).height();
    const qreal points = selectedCharacterFormatForDisplay().wordSpacingRatio * pageHeight;
    if (!m_textEditActive || m_selectedTextRangeLength > 0) {
        const Draft draft = selectedDraftForDisplay();
        qsizetype start = 0;
        qsizetype length = 0;
        selectedTextRangeForDraft(draft, start, length);
        for (const CharacterSpan &span : draft.characterSpans) {
            if (span.start < start + length && span.start + span.length > start
                && qAbs(span.format.wordSpacingRatio * pageHeight - points) > 0.001) {
                return std::numeric_limits<qreal>::quiet_NaN();
            }
        }
    }
    return points;
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
    if (!m_hasSelection)
        return false;
    const auto format = selectedCharacterFormatForDisplay();
    return format.fontItalic || format.textShear != 0.0;
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

qreal PDFEditController::selectedLineSpacing() const
{
    return selectedParagraphSpacing(ParagraphSpacing::Line);
}

qreal PDFEditController::selectedParagraphSpaceBefore() const
{
    return selectedParagraphSpacing(ParagraphSpacing::Before);
}

qreal PDFEditController::selectedParagraphSpaceAfter() const
{
    return selectedParagraphSpacing(ParagraphSpacing::After);
}

qreal PDFEditController::selectedParagraphSpacing(ParagraphSpacing spacing) const
{
    if (!m_document || !m_hasSelection || !isTextCandidateKind(m_selectedCandidate.kind)) {
        return spacing == ParagraphSpacing::Line ? 1.0 : 0.0;
    }
    const Draft draft = selectedDraftForDisplay();
    qsizetype first = -1;
    qsizetype last = -1;
    selectedParagraphRangeForDraft(draft, first, last);
    expandSourceParagraphRange(draft, first, last);
    if (first < 0 || last < first) {
        return spacing == ParagraphSpacing::Line ? 1.0 : 0.0;
    }
    const qreal pageHeight = m_document->pageSize(m_selectedCandidate.page).height();
    const auto valueFor = [spacing, pageHeight](const ParagraphFormat &format) {
        if (spacing == ParagraphSpacing::Line) {
            return format.lineSpacing;
        }
        return (spacing == ParagraphSpacing::Before
                    ? format.spaceBeforeRatio : format.spaceAfterRatio) * pageHeight;
    };
    const qreal value = valueFor(draft.paragraphFormats.at(first));
    for (qsizetype index = first + 1; index <= last; ++index) {
        if (qAbs(valueFor(draft.paragraphFormats.at(index)) - value) > 0.00001) {
            return std::numeric_limits<qreal>::quiet_NaN();
        }
    }
    return value;
}

QFont PDFEditController::fontForDisplay(const QString &fontFamily,
                                        int fontStretch,
                                        bool fontBold,
                                        bool fontItalic,
                                        bool fontUnderline,
                                        qreal pixelSize,
                                        qreal letterSpacing,
                                        qreal wordSpacing,
                                        bool sourceSpacing) const
{
    const int boundedPixelSize = qMax(1, qRound(pixelSize));
    const QString displayFamily = ApplicationFonts::displayFamilyForStyle(
        fontFamily, fontBold, fontItalic);
    QFont font;
    if (!displayFamily.isEmpty()) {
        font.setFamily(displayFamily);
    }
    font.setBold(fontBold);
    font.setItalic(fontItalic);
    font.setUnderline(fontUnderline);
    font.setPixelSize(boundedPixelSize);
#ifdef Q_OS_WIN
    // The whole layout is scaled to the page zoom. Rounding each advance to
    // screen pixels here changes the source line width before that scaling.
    font.setHintingPreference(QFont::PreferNoHinting);
#endif
    font.setStretch(displayFontStretch(
        fontFamily,
        fontStretch,
        fontBold,
        fontItalic,
        boundedPixelSize));
    font.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
    font.setWordSpacing(wordSpacing);
    font.setKerning(!sourceSpacing);
    return font;
}

int PDFEditController::displayFontStretch(const QString &fontFamily,
                                          int fontStretch,
                                          bool fontBold,
                                          bool fontItalic,
                                          qreal pixelSize) const
{
    return ApplicationFonts::calibratedFontStretch(
        ApplicationFonts::displayFamilyForStyle(fontFamily, fontBold, fontItalic),
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
    const auto *typingBlock = static_cast<EditorTypingEditBlock *>(quickTextDocument->findChild<QObject *>(
        QStringLiteral("trepdfTypingEditBlock"), Qt::FindDirectChildrenOnly));
    if (typingBlock && typingBlock->hasPastedFormats())
        return;

    const int maximumPosition = qMax(0, document->characterCount() - 1);
    const int rangeStart = qBound(0, start, maximumPosition);
    const int rangeEnd = qBound(
        rangeStart,
        rangeStart + length,
        maximumPosition);
    if (rangeEnd <= rangeStart) {
        return;
    }

    const QString displayFamily = ApplicationFonts::displayFamilyForStyle(
        m_typingCharacterFormat.fontFamily, m_typingCharacterFormat.fontBold,
        m_typingCharacterFormat.fontItalic,
        document->toPlainText().sliced(rangeStart, rangeEnd - rangeStart));
    QTextCharFormat format;
    if (!displayFamily.isEmpty()) {
        format.setFontFamilies({displayFamily});
    }
    format.setProperty(kEditorFontFamily, m_typingCharacterFormat.fontFamily);
    format.setProperty(kEditorFontStretch, m_typingCharacterFormat.fontStretch);
    format.setFontWeight(
        m_typingCharacterFormat.fontBold ? QFont::Bold : QFont::Normal);
    format.setFontItalic(m_typingCharacterFormat.fontItalic);
    format.setProperty(kEditorTextShear, m_typingCharacterFormat.textShear);
    format.setFontUnderline(m_typingCharacterFormat.fontUnderline);
    format.setForeground(m_typingCharacterFormat.color);
    format.setProperty(kEditorFontSizeRatio, m_typingCharacterFormat.fontSizeRatio);
    format.setProperty(kEditorDisplayFontSizeRatio,
                       m_typingCharacterFormat.displayFontSizeRatio);
    format.setProperty(kEditorLetterSpacingRatio, m_typingCharacterFormat.letterSpacingRatio);
    format.setProperty(kEditorWordSpacingRatio, m_typingCharacterFormat.wordSpacingRatio);
    format.setProperty(kEditorSourceAdvanceRatio, 0.0);
    format.setProperty(kEditorVirtualSpaceRatio, 0.0);
    format.setFontKerning(true);
    if (qIsFinite(displayedPageHeight) && displayedPageHeight > 0.0) {
        const qreal ratio = m_typingCharacterFormat.displayFontSizeRatio > 0.0
            ? m_typingCharacterFormat.displayFontSizeRatio
            : m_typingCharacterFormat.fontSizeRatio;
        const int pixels = qMax(1, qRound(ratio * displayedPageHeight));
        format.setProperty(QTextFormat::FontPixelSize, pixels);
        format.setFontStretch(displayFontStretch(
            displayFamily, m_typingCharacterFormat.fontStretch,
            m_typingCharacterFormat.fontBold, m_typingCharacterFormat.fontItalic, pixels));
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(m_typingCharacterFormat.letterSpacingRatio * displayedPageHeight);
        format.setFontWordSpacing(m_typingCharacterFormat.wordSpacingRatio * displayedPageHeight);
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
    bool merging = true;
    normalizeVirtualSpaceUnderline(document, cursor, merging);
    document->appendUndoItem(new EditorTypingCursorBoundary);
    cursor.endEditBlock();
}

void PDFEditController::initializeHeaderTextEditor(
    QObject *textDocument, const QVariantList &colors,
    int typingPosition, const QColor &typingColor, const QVariantList &families,
    int typingFamilyPosition, const QString &typingFamily) const
{
    auto *document = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!document || !document->textDocument() || !document->parent()) {
        return;
    }
    HeaderTextEditor *editor = headerTextEditor(document);
    if (!editor) {
        editor = new HeaderTextEditor(document);
    }
    editor->initialize(colors, typingPosition, typingColor, families,
                       typingFamilyPosition, typingFamily);
}

QVariantMap PDFEditController::headerTextEditorState(QObject *textDocument) const
{
    HeaderTextEditor *editor = headerTextEditor(textDocument);
    return editor ? editor->state() : QVariantMap();
}

void PDFEditController::setHeaderTextEditorColor(
    QObject *textDocument, const QColor &color) const
{
    if (HeaderTextEditor *editor = headerTextEditor(textDocument)) {
        editor->setColor(color);
    }
}

void PDFEditController::setHeaderTextEditorFontFamily(
    QObject *textDocument, const QString &family) const
{
    if (HeaderTextEditor *editor = headerTextEditor(textDocument)) {
        editor->setFontFamily(family);
    }
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
            if (!controller || !controller->m_textEditActive) {
                return false;
            }
            const Draft current = controller->selectedDraftForDisplay();
            return controller->m_typingCharacterFormatActive
                || std::any_of(current.characterSpans.cbegin(), current.characterSpans.cend(),
                               [](const CharacterSpan &span) {
                    return span.format.textShear != 0.0
                        || !qFuzzyIsNull(span.format.sourceAdvanceRatio)
                        || span.format.virtualSpaceRatio > 0.0;
                });
        });
    }
    static_cast<EditorTypingEditBlock *>(quickDocument->findChild<QObject *>(
        QStringLiteral("trepdfTypingEditBlock"), Qt::FindDirectChildrenOnly))->setPageHeight(displayedPageHeight);
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
        format.setProperty(kEditorFontFamily, span.format.fontFamily);
        format.setProperty(kEditorFontStretch, span.format.fontStretch);
        format.setProperty(kEditorFontSizeRatio, span.format.fontSizeRatio);
        format.setProperty(kEditorDisplayFontSizeRatio, span.format.displayFontSizeRatio);
        format.setProperty(kEditorLetterSpacingRatio, span.format.letterSpacingRatio);
        format.setProperty(kEditorWordSpacingRatio, span.format.wordSpacingRatio);
        format.setProperty(kEditorSourceAdvanceRatio, span.format.sourceAdvanceRatio);
        // HTML carries these styles on visible runs, but omits the paragraph
        // separator. Seed its complete style along with the exact metrics.
        format.setFontWeight(span.format.fontBold ? QFont::Bold : QFont::Normal);
        format.setFontItalic(span.format.fontItalic);
        format.setProperty(kEditorTextShear, span.format.textShear);
        format.setFontUnderline(span.format.fontUnderline);
        format.setForeground(span.format.color);
        format.setFontKerning(qFuzzyIsNull(span.format.sourceAdvanceRatio));
        if (qIsFinite(displayedPageHeight) && displayedPageHeight > 0.0) {
            const qreal ratio = span.format.displayFontSizeRatio > 0.0
                ? span.format.displayFontSizeRatio : span.format.fontSizeRatio;
            const int pixels = qMax(1, qRound(ratio * displayedPageHeight));
            format.setProperty(QTextFormat::FontPixelSize, pixels);
            format.setFontStretch(displayFontStretch(
                span.format.fontFamily, span.format.fontStretch,
                span.format.fontBold, span.format.fontItalic, pixels));
            format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            format.setFontLetterSpacing((span.format.letterSpacingRatio
                                         + span.format.sourceAdvanceRatio) * displayedPageHeight);
            format.setFontWordSpacing(span.format.wordSpacingRatio * displayedPageHeight);
        }
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QTextCharFormat complete = cursor.charFormat();
        complete.merge(format);
        applyVirtualSpaceWidth(complete, span.format.virtualSpaceRatio, displayedPageHeight);
        cursor.setCharFormat(complete);
    }
    document->setUndoRedoEnabled(undoEnabled);
}

void PDFEditController::normalizeEditorSourceSpacing(
    QObject *textDocument, qreal displayedPageHeight) const
{
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickDocument ? quickDocument->textDocument() : nullptr;
    if (!document || !m_textEditActive || !m_hasSelection
        || !qIsFinite(displayedPageHeight) || displayedPageHeight <= 0.0) {
        return;
    }
    auto *block = static_cast<EditorTypingEditBlock *>(quickDocument->findChild<QObject *>(
        QStringLiteral("trepdfTypingEditBlock"), Qt::FindDirectChildrenOnly));
    // Undo and redo restore their own custom properties. Reapplying the edit
    // invalidation there would erase the source spacing that undo just restored.
    bool nativeEdit = false;
    if (!block || !block->takePendingTextEdit(&nativeEdit)) {
        return;
    }
    Draft draft = selectedDraftForDisplay();
    const QString text = document->toPlainText();
    // A format command can still be pending when Undo restores its properties.
    // Only native replacement (including space-for-space) needs normalization
    // when the text is unchanged; do not turn a format undo into a new command.
    if (draft.text == text && !nativeEdit)
        return;
    const int position = quickDocument->parent()
        ? quickDocument->parent()->property("cursorPosition").toInt() : -1;
    setDraftText(draft, text, nullptr, -1, position);
    QTextCursor cursor(document);
    bool merging = false;
    for (const CharacterSpan &span : std::as_const(draft.characterSpans)) {
        for (int index = int(span.start); index < span.start + span.length; ++index) {
            cursor.setPosition(index);
            cursor.setPosition(index + 1, QTextCursor::KeepAnchor);
            QTextCharFormat previous = cursor.charFormat();
            const bool inserted = block->insertedAt(index);
            if (inserted)
                previous.merge(block->pastedFormat(index));
            const qreal virtualRatio = inserted ? block->pastedVirtualRatio(index) : span.format.virtualSpaceRatio;
            if (!inserted
                && previous.doubleProperty(kEditorSourceAdvanceRatio) == span.format.sourceAdvanceRatio
                && previous.doubleProperty(kEditorVirtualSpaceRatio) == virtualRatio) {
                continue;
            }
            if (!merging) {
                cursor.joinPreviousEditBlock();
                merging = true;
            }
            QTextCharFormat format;
            format.setProperty(kEditorSourceAdvanceRatio, span.format.sourceAdvanceRatio);
            format.setFontKerning(qFuzzyIsNull(span.format.sourceAdvanceRatio));
            const qreal letterSpacing = previous.hasProperty(kEditorLetterSpacingRatio)
                ? previous.doubleProperty(kEditorLetterSpacingRatio) : span.format.letterSpacingRatio;
            format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            format.setFontLetterSpacing(
                (letterSpacing + span.format.sourceAdvanceRatio) * displayedPageHeight);
            QTextCharFormat complete = previous;
            complete.merge(format);
            applyVirtualSpaceWidth(complete, virtualRatio, displayedPageHeight);
            if (virtualRatio <= 0.0)
                complete.setFontWordSpacing(previous.doubleProperty(kEditorWordSpacingRatio) * displayedPageHeight);
            cursor.setCharFormat(complete);
        }
    }
    normalizeVirtualSpaceUnderline(document, cursor, merging);
    if (merging) {
        document->appendUndoItem(new EditorTypingCursorBoundary);
        cursor.endEditBlock();
    }
    block->takePendingTextEdit();
}

bool PDFEditController::applyFontFamilyToEditorDocument(
    QObject *textDocument, int start, int length, const QString &family,
    qreal displayedPageHeight) const
{
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickDocument ? quickDocument->textDocument() : nullptr;
    if (!document || !m_textEditActive || !m_hasSelection || length <= 0
        || family.isEmpty() || !qIsFinite(displayedPageHeight) || displayedPageHeight <= 0.0)
        return false;
    const int maximum = document->characterCount() - 1;
    const int first = qBound(0, start, maximum);
    const int last = int(qBound(qint64(first), qint64(start) + length, qint64(maximum)));
    if (first == last)
        return false;
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (int position = first; position < last; ++position) {
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        QTextCharFormat format = cursor.charFormat();
        const bool bold = format.fontWeight() >= QFont::Bold;
        CharacterFormat face;
        face.fontFamily = format.stringProperty(kEditorFontFamily);
        face.fontBold = bold;
        face.fontItalic = format.fontItalic();
        face.textShear = format.doubleProperty(kEditorTextShear);
        changeFormatFamily(face, family);
        format.setFontItalic(face.fontItalic);
        format.setProperty(kEditorTextShear, face.textShear);
        const QString displayFamily = ApplicationFonts::displayFamilyForStyle(
            family, bold, face.fontItalic, cursor.selectedText());
        format.setFontFamilies({displayFamily.isEmpty() ? family : displayFamily});
        format.setProperty(kEditorFontFamily, family);
        format.setProperty(kEditorFontStretch, QFont::Unstretched);
        format.setProperty(kEditorSourceAdvanceRatio, 0.0);
        const int pixels = format.intProperty(QTextFormat::FontPixelSize);
        format.setFontStretch(displayFontStretch(family, QFont::Unstretched,
            bold, format.fontItalic(), pixels));
        format.setFontKerning(true);
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(format.doubleProperty(kEditorLetterSpacingRatio) * displayedPageHeight);
        format.setFontWordSpacing(format.doubleProperty(kEditorWordSpacingRatio) * displayedPageHeight);
        applyVirtualSpaceWidth(format, format.doubleProperty(kEditorVirtualSpaceRatio), displayedPageHeight);
        cursor.setCharFormat(format);
    }
    cursor.endEditBlock();
    return true;
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
        format.setProperty(kEditorSourceAdvanceRatio, 0.0);
        format.setFontKerning(true);
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(span.format.letterSpacingRatio * displayedPageHeight);
        format.setProperty(QTextFormat::FontPixelSize, pixels);
        format.setFontStretch(displayFontStretch(
            span.format.fontFamily, span.format.fontStretch,
            span.format.fontBold, span.format.fontItalic, pixels));
        cursor.setPosition(rangeStart);
        cursor.setPosition(rangeEnd, QTextCursor::KeepAnchor);
        QTextCharFormat complete = cursor.charFormat();
        complete.merge(format);
        applyVirtualSpaceWidth(complete, complete.doubleProperty(kEditorVirtualSpaceRatio), displayedPageHeight);
        cursor.setCharFormat(complete);
    }
    cursor.endEditBlock();
    return true;
}

bool PDFEditController::applyTextSpacingToEditorDocument(
    QObject *textDocument, int start, int length,
    qreal letterSpacingRatio, qreal wordSpacingRatio, qreal displayedPageHeight) const
{
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickDocument ? quickDocument->textDocument() : nullptr;
    if (!document || !m_textEditActive || !m_hasSelection || length <= 0
        || (!qIsFinite(letterSpacingRatio) && !qIsFinite(wordSpacingRatio))
        || !qIsFinite(displayedPageHeight) || displayedPageHeight <= 0.0) {
        return false;
    }
    if (document->toPlainText() != selectedDraftForDisplay().text) {
        return false;
    }
    const int maximum = qMax(0, document->characterCount() - 1);
    const int first = qBound(0, start, maximum);
    const int last = static_cast<int>(qBound(qint64(first), qint64(start) + length,
                                            qint64(maximum)));
    if (last <= first) {
        return false;
    }
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (int position = first; position < last; ++position) {
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat previous = cursor.charFormat();
        QTextCharFormat format;
        format.setProperty(kEditorSourceAdvanceRatio, 0.0);
        format.setFontKerning(true);
        const qreal effectiveLetterSpacing = qIsFinite(letterSpacingRatio)
            ? letterSpacingRatio : previous.doubleProperty(kEditorLetterSpacingRatio);
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing(effectiveLetterSpacing * displayedPageHeight);
        if (qIsFinite(letterSpacingRatio)) {
            format.setProperty(kEditorLetterSpacingRatio, letterSpacingRatio);
        }
        if (qIsFinite(wordSpacingRatio)) {
            format.setProperty(kEditorWordSpacingRatio, wordSpacingRatio);
            format.setFontWordSpacing(wordSpacingRatio * displayedPageHeight);
        }
        QTextCharFormat complete = cursor.charFormat();
        complete.merge(format);
        applyVirtualSpaceWidth(complete, complete.doubleProperty(kEditorVirtualSpaceRatio), displayedPageHeight);
        cursor.setCharFormat(complete);
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
    cursor.beginEditBlock();
    if (style == static_cast<int>(FontStyle::Underline)) {
        for (int position = rangeStart; position < rangeEnd; ++position) {
            cursor.setPosition(position);
            cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
            if (cursor.charFormat().doubleProperty(kEditorVirtualSpaceRatio) <= 0.0)
                cursor.mergeCharFormat(format);
        }
        bool merging = true;
        normalizeVirtualSpaceUnderline(document, cursor, merging);
    } else {
        const qreal pageHeight = m_document
            ? m_document->pageSize(m_selectedCandidate.page).height() : 0.0;
        for (int position = rangeStart; position < rangeEnd; ++position) {
            cursor.setPosition(position);
            cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
            const QTextCharFormat previous = cursor.charFormat();
            QTextCharFormat updated = format;
            if (style == static_cast<int>(FontStyle::Italic)) {
                const qreal shear = previous.doubleProperty(kEditorTextShear);
                updated.setProperty(kEditorTextShear, enabled ? shear : 0.0);
                updated.setFontItalic(enabled && shear == 0.0);
            }
            QTextCharFormat styled = previous;
            styled.merge(updated);
            const QString family = previous.hasProperty(kEditorFontFamily)
                ? previous.stringProperty(kEditorFontFamily)
                : previous.font().family();
            const int stretch = previous.hasProperty(kEditorFontStretch)
                ? previous.intProperty(kEditorFontStretch)
                : (previous.fontStretch() > 0 ? previous.fontStretch() : 100);
            const QString displayFamily = ApplicationFonts::displayFamilyForStyle(
                family, styled.fontWeight() >= QFont::Bold, styled.fontItalic(),
                cursor.selectedText());
            if (!displayFamily.isEmpty()) {
                updated.setFontFamilies({displayFamily});
            }
            updated.setProperty(kEditorFontFamily, family);
            updated.setProperty(kEditorFontStretch, stretch);
            if (previous.hasProperty(QTextFormat::FontPixelSize)) {
                updated.setFontStretch(displayFontStretch(
                    displayFamily, stretch, styled.fontWeight() >= QFont::Bold,
                    styled.fontItalic(), previous.intProperty(QTextFormat::FontPixelSize)));
            }
            updated.setProperty(kEditorSourceAdvanceRatio, 0.0);
            updated.setFontKerning(true);
            updated.setFontLetterSpacingType(QFont::AbsoluteSpacing);
            updated.setFontLetterSpacing(previous.doubleProperty(kEditorLetterSpacingRatio) * pageHeight);
            QTextCharFormat complete = previous;
            complete.merge(updated);
            applyVirtualSpaceWidth(complete, complete.doubleProperty(kEditorVirtualSpaceRatio), pageHeight);
            cursor.setCharFormat(complete);
        }
    }
    cursor.endEditBlock();
    return true;
}

void PDFEditController::initializeEditorParagraphFormats(
    QObject *textDocument, qreal displayedPageHeight) const
{
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickDocument ? quickDocument->textDocument() : nullptr;
    if (!document || !m_textEditActive || !m_hasSelection
        || document->toPlainText() != selectedDraftForDisplay().text) {
        return;
    }
    // Initial metadata must be invisible to the editor's local undo history.
    const bool undoEnabled = document->isUndoRedoEnabled();
    document->setUndoRedoEnabled(false);
    applyParagraphFormatsToEditorDocument(textDocument, displayedPageHeight);
    document->setUndoRedoEnabled(undoEnabled);
}

bool PDFEditController::applyParagraphFormatsToEditorDocument(
    QObject *textDocument, qreal displayedPageHeight) const
{
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    QTextDocument *document = quickDocument ? quickDocument->textDocument() : nullptr;
    if (!document || !m_textEditActive || !m_hasSelection
        || !qIsFinite(displayedPageHeight) || displayedPageHeight <= 0.0) {
        return false;
    }
    const Draft draft = selectedDraftForDisplay();
    if (document->toPlainText() != draft.text) {
        return false;
    }
    QTextCursor cursor(document);
    if (quickDocument->parent()) {
        cursor.setPosition(qBound(0, quickDocument->parent()->property("cursorPosition").toInt(),
                                 qMax(0, document->characterCount() - 1)));
    }
    cursor.beginEditBlock();
    bool changed = false;
    for (const ParagraphFormat &paragraph : draft.paragraphFormats) {
        const QTextBlock block = document->findBlock(static_cast<int>(paragraph.start));
        if (!block.isValid()) {
            continue;
        }
        QTextBlockFormat format;
        format.setProperty(kEditorLineSpacing, paragraph.lineSpacing);
        format.setProperty(kEditorParagraphSpaceBeforeRatio, paragraph.spaceBeforeRatio);
        format.setProperty(kEditorParagraphSpaceAfterRatio, paragraph.spaceAfterRatio);
        format.setProperty(kEditorFirstLineIndentRatio, paragraph.firstLineIndentRatio);
        format.setLineHeight(paragraph.lineSpacing * 100.0, QTextBlockFormat::ProportionalHeight);
        format.setTopMargin(paragraph.spaceBeforeRatio * displayedPageHeight);
        format.setBottomMargin(paragraph.spaceAfterRatio * displayedPageHeight);
        format.setTextIndent(paragraph.firstLineIndentRatio * displayedPageHeight);
        switch (paragraph.alignment) {
        case LeftAlignment: format.setAlignment(Qt::AlignLeft); break;
        case RightAlignment: format.setAlignment(Qt::AlignRight); break;
        case CenterAlignment: format.setAlignment(Qt::AlignHCenter); break;
        case JustifyAlignment: format.setAlignment(Qt::AlignJustify); break;
        }
        const auto properties = format.properties();
        const QTextBlockFormat current = block.blockFormat();
        bool needsFormat = false;
        for (auto property = properties.cbegin(); property != properties.cend(); ++property) {
            if (current.property(property.key()) != property.value()) {
                needsFormat = true;
                break;
            }
        }
        if (needsFormat) {
            cursor.setPosition(block.position());
            cursor.mergeBlockFormat(format);
            changed = true;
        }
    }
    if (changed && document->isUndoRedoEnabled() && quickDocument->parent()) {
        document->appendUndoItem(new EditorParagraphSelectionBoundary(quickDocument, this));
    }
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
    if (document->toPlainText() != draft.text) {
        return false;
    }
    QVector<ParagraphFormat> paragraphs = draft.paragraphFormats;
    for (ParagraphFormat &paragraph : paragraphs) {
        const QTextBlock block = document->findBlock(static_cast<int>(paragraph.start));
        const QTextBlockFormat format = block.blockFormat();
        if (format.hasProperty(kEditorLineSpacing)) {
            const qreal value = format.doubleProperty(kEditorLineSpacing);
            if (qIsFinite(value) && value >= 0.5 && value <= 3.0) {
                paragraph.lineSpacing = value;
            }
        }
        if (format.hasProperty(kEditorParagraphSpaceBeforeRatio)) {
            const qreal value = format.doubleProperty(kEditorParagraphSpaceBeforeRatio);
            if (qIsFinite(value) && value >= 0.0) {
                paragraph.spaceBeforeRatio = value;
            }
        }
        if (format.hasProperty(kEditorParagraphSpaceAfterRatio)) {
            const qreal value = format.doubleProperty(kEditorParagraphSpaceAfterRatio);
            if (qIsFinite(value) && value >= 0.0) {
                paragraph.spaceAfterRatio = value;
            }
        }
        if (format.hasProperty(kEditorFirstLineIndentRatio)) {
            const qreal value = format.doubleProperty(kEditorFirstLineIndentRatio);
            if (qIsFinite(value) && value >= 0.0) {
                paragraph.firstLineIndentRatio = value;
            }
        }
    }
    const bool paragraphsChanged = !sameParagraphFormats(draft.paragraphFormats, paragraphs);
    draft.paragraphFormats = std::move(paragraphs);
    if (draft.text.isEmpty()) {
        if (paragraphsChanged) {
            updateSelectedDraft(draft);
        }
        return paragraphsChanged;
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
        // Selecting a paragraph separator makes QTextCursor::charFormat()
        // report the first character of the following block. Its own format
        // is stored on that block and can have a different size or style.
        const QTextCharFormat editorFormat = document->characterAt(static_cast<int>(position))
                == QChar::ParagraphSeparator
            ? document->findBlock(static_cast<int>(position + 1)).charFormat()
            : cursor.charFormat();
        CharacterFormat &format = formats[position];
        const qreal virtualRatio = editorFormat.doubleProperty(kEditorVirtualSpaceRatio);
        format.virtualSpaceRatio = draft.text.at(position) == QLatin1Char(' ')
                && qIsFinite(virtualRatio) && virtualRatio > 0.0 ? virtualRatio : 0.0;
        if (editorFormat.hasProperty(kEditorFontFamily))
            format.fontFamily = editorFormat.stringProperty(kEditorFontFamily);
        if (editorFormat.hasProperty(kEditorFontStretch))
            format.fontStretch = editorFormat.intProperty(kEditorFontStretch);
        format.fontBold = editorFormat.fontWeight() >= QFont::Bold;
        format.fontItalic = editorFormat.fontItalic();
        format.textShear = format.fontItalic ? 0.0 : editorFormat.doubleProperty(kEditorTextShear);
        format.fontUnderline = editorFormat.fontUnderline();
        if (editorFormat.hasProperty(QTextFormat::ForegroundBrush)
            && editorFormat.foreground().color().isValid()) {
            format.color = editorFormat.foreground().color().toRgb();
        }
        if (editorFormat.hasProperty(kEditorLetterSpacingRatio)) {
            const qreal ratio = editorFormat.doubleProperty(kEditorLetterSpacingRatio);
            if (qIsFinite(ratio)) {
                format.letterSpacingRatio = ratio;
            }
        }
        if (editorFormat.hasProperty(kEditorWordSpacingRatio)) {
            const qreal ratio = editorFormat.doubleProperty(kEditorWordSpacingRatio);
            if (qIsFinite(ratio)) {
                format.wordSpacingRatio = ratio;
            }
        }
        if (editorFormat.hasProperty(kEditorSourceAdvanceRatio)) {
            const qreal ratio = editorFormat.doubleProperty(kEditorSourceAdvanceRatio);
            if (qIsFinite(ratio)) {
                format.sourceAdvanceRatio = ratio;
            }
        }
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
    if (sameCharacterSpans(draft.characterSpans, spans) && !paragraphsChanged) {
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

void PDFEditController::changeFormatFamily(CharacterFormat &format, const QString &family)
{
    if (sameFontFamily(format.fontFamily, family) && format.fontStretch == QFont::Unstretched)
        return;
    if (format.textShear != 0.0 && !sameFontFamily(format.fontFamily, family)
        && ApplicationFonts::isFontFaceSelectable(family, format.fontBold, true)) {
        format.fontItalic = true;
        format.textShear = 0.0;
    }
    format.fontFamily = family;
    format.fontStretch = QFont::Unstretched;
    format.sourceAdvanceRatio = 0.0;
}

void PDFEditController::setSelectedFontFamily(const QString &fontFamily)
{
    if (m_saving || !m_hasSelection
        || !m_selectedCandidate.editable
        || !isTextCandidateKind(m_selectedCandidate.kind)
        || fontFamily.trimmed().isEmpty()
        || !QFontDatabase::hasFamily(fontFamily)) {
        return;
    }

    Draft draft = selectedDraftForDisplay();
    const auto faceIsSelectable = [&fontFamily](CharacterFormat format) {
        changeFormatFamily(format, fontFamily);
        return ApplicationFonts::isFontFaceSelectable(fontFamily, format.fontBold, format.fontItalic);
    };
    if (m_textEditActive && m_selectedTextRangeLength == 0) {
        const CharacterFormat current = selectedCharacterFormatForDisplay();
        if (!faceIsSelectable(current))
            return;
        CharacterFormat typing = current;
        changeFormatFamily(typing, fontFamily);
        if (sameCharacterFormat(current, typing))
            return;
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
        if (!faceIsSelectable(span.format)) {
            return;
        }
    }
    if (!checkedSpan) {
        const CharacterFormat format = primaryCharacterFormat(draft);
        if (!faceIsSelectable(format)) {
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
        [&fontFamily](CharacterFormat &format) {
            changeFormatFamily(format, fontFamily);
        });
    if (!changed) {
        if (finishImmediately) {
            finishSelectedTextEdit();
        }
        return;
    }
    rebaseTypingFormat(draft);
    if (!finishImmediately && rangeLength > 0)
        emit selectionFontFamilyApplied(int(rangeStart), int(rangeLength), fontFamily);
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
        format.sourceAdvanceRatio = 0.0;
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

void PDFEditController::setSelectedLetterSpacing(qreal points)
{
    setSelectedTextSpacing(points, false);
}

void PDFEditController::setSelectedWordSpacing(qreal points)
{
    setSelectedTextSpacing(points, true);
}

void PDFEditController::setSelectedTextSpacing(qreal points, bool wordSpacing)
{
    if (m_saving || !m_document || !m_hasSelection || !m_selectedCandidate.editable
        || !isTextCandidateKind(m_selectedCandidate.kind)
        || !qIsFinite(points) || points < -10.0 || points > 50.0) {
        return;
    }
    const qreal pageHeight = m_document->pageSize(m_selectedCandidate.page).height();
    if (!qIsFinite(pageHeight) || pageHeight <= 0.0) {
        return;
    }
    const qreal ratio = points / pageHeight;
    const auto changeSpacing = [ratio, wordSpacing](CharacterFormat &format) {
        qreal &current = wordSpacing ? format.wordSpacingRatio : format.letterSpacingRatio;
        if (!qFuzzyCompare(current + 1.0, ratio + 1.0)) {
            current = ratio;
            format.sourceAdvanceRatio = 0.0;
        }
    };
    Draft draft = selectedDraftForDisplay();
    if (m_textEditActive && m_selectedTextRangeLength == 0) {
        const CharacterFormat current = selectedCharacterFormatForDisplay();
        CharacterFormat typing = current;
        changeSpacing(typing);
        if (sameCharacterFormat(current, typing)) {
            return;
        }
        const qsizetype caret = qBound<qsizetype>(0, m_selectedTextRangeStart, draft.text.size());
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
    if (!updateCharacterFormats(draft, start, length, changeSpacing)) {
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
        const qreal unchanged = std::numeric_limits<qreal>::quiet_NaN();
        emit selectionTextSpacingApplied(static_cast<int>(start), static_cast<int>(length),
                                         wordSpacing ? unchanged : ratio,
                                         wordSpacing ? ratio : unchanged);
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

    const QColor normalizedColor = color.toRgb();
    Draft draft = selectedDraftForDisplay();
    if (m_textEditActive && m_selectedTextRangeLength == 0) {
        const CharacterFormat current = selectedCharacterFormatForDisplay();
        CharacterFormat typing = current;
        typing.color = normalizedColor;
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

    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEdit();
    }
    if (!m_textEditActive || !m_hasSelection) {
        return;
    }

    draft = selectedDraftForDisplay();
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

void PDFEditController::expandSourceParagraphRange(
    const Draft &draft, qsizetype &first, qsizetype &last)
{
    if (first < 0 || last < first || last >= draft.paragraphFormats.size()) {
        return;
    }
    while (first > 0 && draft.sourceLineBreaks.contains(
               draft.paragraphFormats.at(first).start - 1)) {
        --first;
    }
    while (last + 1 < draft.paragraphFormats.size() && draft.sourceLineBreaks.contains(
               draft.paragraphFormats.at(last + 1).start - 1)) {
        ++last;
    }
}

bool PDFEditController::normalizeSourceParagraphBreaks(
    Draft &draft, qsizetype &first, qsizetype &last)
{
    if (first < 0 || last < first || draft.sourceLineBreaks.isEmpty()) {
        return false;
    }
    expandSourceParagraphRange(draft, first, last);
    const qsizetype start = draft.paragraphFormats.at(first).start;
    const qsizetype end = draft.paragraphFormats.at(last).start
        + draft.paragraphFormats.at(last).length;
    bool changed = false;
    for (qsizetype position : std::as_const(draft.sourceLineBreaks)) {
        if (position >= start && position < end) {
            // A same-length substitution retains source styles and selection.
            // Explicit user paragraph breaks never occur in sourceLineBreaks.
            draft.text[position] = QLatin1Char(' ');
            changed = true;
        }
    }
    if (!changed) {
        return false;
    }
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
        if (newStart == start) {
            first = index;
        }
        if (newStart <= end) {
            last = index;
        }
    }
    return true;
}

void PDFEditController::setSelectedLineSpacing(qreal factor)
{
    setSelectedParagraphSpacing(factor, ParagraphSpacing::Line);
}

void PDFEditController::setSelectedParagraphSpaceBefore(qreal points)
{
    setSelectedParagraphSpacing(points, ParagraphSpacing::Before);
}

void PDFEditController::setSelectedParagraphSpaceAfter(qreal points)
{
    setSelectedParagraphSpacing(points, ParagraphSpacing::After);
}

void PDFEditController::setSelectedParagraphSpacing(qreal value, ParagraphSpacing spacing)
{
    if (m_saving || !m_document || !m_hasSelection || !m_selectedCandidate.editable
        || !isTextCandidateKind(m_selectedCandidate.kind) || !qIsFinite(value)
        || (spacing == ParagraphSpacing::Line ? value < 0.5 || value > 3.0
                                             : value < 0.0 || value > 144.0)) {
        return;
    }
    const qreal pageHeight = m_document->pageSize(m_selectedCandidate.page).height();
    if (!qIsFinite(pageHeight) || pageHeight <= 0.0) {
        return;
    }
    const qreal storedValue = spacing == ParagraphSpacing::Line ? value : value / pageHeight;
    const auto metric = [spacing](ParagraphFormat &paragraph) -> qreal & {
        if (spacing == ParagraphSpacing::Line) {
            return paragraph.lineSpacing;
        }
        return spacing == ParagraphSpacing::Before
            ? paragraph.spaceBeforeRatio : paragraph.spaceAfterRatio;
    };
    Draft draft = selectedDraftForDisplay();
    qsizetype first = -1;
    qsizetype last = -1;
    selectedParagraphRangeForDraft(draft, first, last);
    expandSourceParagraphRange(draft, first, last);
    bool changed = false;
    for (qsizetype index = first; index >= 0 && index <= last; ++index) {
        changed |= !qFuzzyCompare(metric(draft.paragraphFormats[index]) + 1.0, storedValue + 1.0);
    }
    // A no-op never normalizes source rows or creates an expanded edit frame.
    if (!changed) {
        return;
    }
    normalizeSourceParagraphBreaks(draft, first, last);
    for (qsizetype index = first; index >= 0 && index <= last; ++index) {
        metric(draft.paragraphFormats[index]) = storedValue;
    }
    const bool finishImmediately = !m_textEditActive;
    if (finishImmediately) {
        beginSelectedTextEditSession(false);
    }
    if (!m_textEditActive) {
        return;
    }
    if (!finishImmediately) {
        // The merge callback reads the new paragraph formats. Expose the draft
        // before notifying QML, then publish its normal state change afterwards.
        m_textEditDisplayDraft = draft;
        m_textEditDisplayDraftActive = true;
        emit selectionParagraphSpacingApplied();
    }
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
    if (alignment == JustifyAlignment) {
        changed = normalizeSourceParagraphBreaks(draft, first, last);
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
            if (format.fontBold != enabled) {
                format.sourceAdvanceRatio = 0.0;
            }
            format.fontBold = enabled;
            break;
        case FontStyle::Italic:
            if ((format.fontItalic || format.textShear != 0.0) != enabled) {
                format.sourceAdvanceRatio = 0.0;
            }
            if (!enabled)
                format.textShear = 0.0;
            format.fontItalic = enabled && format.textShear == 0.0;
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
        [&updatedStyle, style](CharacterFormat &format) {
            if (style == FontStyle::Underline && format.virtualSpaceRatio > 0.0)
                return;
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
    Draft finalizedDraft = collapseEditorVirtualSpaces(selectedDraftForDisplay());
    const auto textMetricsUnchanged = [&]() {
        qsizetype left = 0;
        qsizetype right = 0;
        while (left < finalizedDraft.characterSpans.size()
               && right < baseline.before.characterSpans.size()) {
            const CharacterSpan &current = finalizedDraft.characterSpans.at(left);
            const CharacterSpan &previous = baseline.before.characterSpans.at(right);
            if (!qFuzzyCompare(current.format.fontSizeRatio, previous.format.fontSizeRatio)
                || !qFuzzyCompare(current.format.displayFontSizeRatio,
                                  previous.format.displayFontSizeRatio)
                || current.format.letterSpacingRatio != previous.format.letterSpacingRatio
                || current.format.wordSpacingRatio != previous.format.wordSpacingRatio
                || current.format.sourceAdvanceRatio != previous.format.sourceAdvanceRatio) {
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
        && textMetricsUnchanged()
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
    if (m_hasSelection && !candidateMatchesSelectionScope(m_selectedCandidate)) {
        setSelection(nullptr);
    } else {
        emit selectionChanged();
    }

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
        fontPixelSize,
        candidate.letterSpacingRatio * pageSize.height(),
        candidate.wordSpacingRatio * pageSize.height());
    const QFontMetricsF metrics(displayFont);
    qreal measuredWidth = 0.0;
    qsizetype lineStart = 0;
    const QStringList lines = candidate.text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        qreal firstLineIndent = 0.0;
        for (const ParagraphFormat &paragraph : candidate.sourceParagraphFormats) {
            if (paragraph.start == lineStart) {
                firstLineIndent = paragraph.firstLineIndentRatio * pageSize.height();
                break;
            }
        }
        if (candidate.sourceCharacterSpans.isEmpty()) {
            measuredWidth = qMax(measuredWidth,
                firstLineIndent
                    + qMax(metrics.horizontalAdvance(line), metrics.boundingRect(line).width()));
        } else {
            QTextLayout layout(line, displayFont);
            QList<QTextLayout::FormatRange> formats;
            for (const CharacterSpan &span : candidate.sourceCharacterSpans) {
                const qsizetype start = qMax(lineStart, span.start);
                const qsizetype end = qMin(lineStart + line.size(), span.start + span.length);
                if (start >= end) {
                    continue;
                }
                const CharacterFormat &format = span.format;
                const qreal size = (format.displayFontSizeRatio > 0.0
                    ? format.displayFontSizeRatio : format.fontSizeRatio) * pageSize.height();
                QTextCharFormat textFormat;
                textFormat.setFont(fontForDisplay(format.fontFamily, format.fontStretch,
                    format.fontBold, format.fontItalic, format.fontUnderline, size,
                    (format.letterSpacingRatio + format.sourceAdvanceRatio) * pageSize.height(),
                    format.wordSpacingRatio * pageSize.height(),
                    !qFuzzyIsNull(format.sourceAdvanceRatio)));
                formats.append(QTextLayout::FormatRange{
                    int(start - lineStart), int(end - start), textFormat});
            }
            layout.setFormats(formats);
            QTextOption option;
            option.setWrapMode(QTextOption::NoWrap);
            layout.setTextOption(option);
            layout.beginLayout();
            QTextLine textLine = layout.createLine();
            if (textLine.isValid()) {
                // Tabs occupy several columns: using the UTF-16 length as a
                // column cap can measure only a prefix of the source line.
                textLine.setLineWidth(pageSize.width());
                measuredWidth = qMax(measuredWidth,
                    firstLineIndent
                        + qMax(textLine.horizontalAdvance(), textLine.naturalTextWidth()));
            }
            layout.endLayout();
        }
        lineStart += line.size() + 1;
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
    format.letterSpacingRatio = candidate.letterSpacingRatio;
    format.wordSpacingRatio = candidate.wordSpacingRatio;
    format.fontBold = candidate.fontBold;
    format.fontItalic = candidate.fontItalic;
    format.textShear = candidate.textShear;
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
    draft.tableRowStops = candidate.tableRowStops;
    draft.tableColumnStops = candidate.tableColumnStops;
    draft.commentProperties = candidate.commentProperties;
    draft.text = candidate.text;
    draft.characterSpans = candidate.sourceCharacterSpans;
    if (draft.characterSpans.isEmpty()) {
        draft.characterSpans.append(CharacterSpan{
            0,
            candidate.text.size(),
            characterFormatForCandidate(candidate)});
    }
    draft.paragraphFormats = candidate.sourceParagraphFormats.isEmpty()
        ? paragraphFormatsForText(candidate.text, ParagraphFormat())
        : candidate.sourceParagraphFormats;
    draft.sourceLineBreaks = candidate.sourceParagraphLineBreaks;
    if (candidate.sourceParagraphFormats.isEmpty()
        && candidate.objectId.startsWith(QLatin1String("paragraph:"))) {
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
    if (!candidate.sourceParagraphFormats.isEmpty()) {
        // A saved layout frame can be shorter than its laid-out paragraph.
        // Retain the actual source glyph extent when reopening, including the
        // final paragraph's trailing space, for hover, selection and movement.
        QRectF sourceContentBounds = candidate.geometry.sourceRect;
        sourceContentBounds.setBottom(qMin<qreal>(1.0, sourceContentBounds.bottom()
            + candidate.sourceParagraphFormats.constLast().spaceAfterRatio));
        draft.contentBounds = draft.contentBounds.united(sourceContentBounds);
    }
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

    // A source advance describes one original cluster boundary. New text must
    // never inherit that boundary's adjustment from the preceding character.
    insertedFormat.sourceAdvanceRatio = 0.0;
    insertedFormat.virtualSpaceRatio = 0.0;
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

    // Invalidate the changed graphemes and the boundary immediately before the
    // edit. A combining-mark insertion may change a cluster on either side.
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
    boundaries.setPosition(change.start);
    qsizetype invalidStart = boundaries.toPreviousBoundary();
    if (invalidStart < 0) {
        invalidStart = 0;
    }
    boundaries.setPosition(change.start + change.insertedLength);
    qsizetype invalidEnd = boundaries.isAtBoundary()
        ? change.start + change.insertedLength : boundaries.toNextBoundary();
    if (invalidEnd < 0) {
        invalidEnd = newLength;
    }
    for (qsizetype index = invalidStart; index < invalidEnd; ++index) {
        newFormats[index].sourceAdvanceRatio = 0.0;
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
    if (draft.text == draft.candidate.text) {
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
        const QString semanticFamily = PDFFontAliases::semanticFamily(span.format.fontFamily);
        const QString previousFamily = span.format.fontFamily;
        const QString spanText = draft.text.sliced(start, length);
        if (!containsNonAscii(spanText) && semanticFamily == span.format.fontFamily) {
            continue;
        }

        // Keep a usable embedded face and its original metrics. When an edit
        // introduces a missing glyph, resolve the complete local face through
        // the public family; private PDF aliases are never provider names.
        if (!PDFFontAliases::sourceFingerprint(span.format.fontFamily).isEmpty()) {
            QFont font(span.format.fontFamily);
            font.setBold(span.format.fontBold);
            font.setItalic(span.format.fontItalic);
            font.setPixelSize(16);
            font.setStyleStrategy(QFont::NoFontMerging);
            const QRawFont raw = QRawFont::fromFont(font);
            bool supportsText = raw.isValid()
                && raw.familyName() == span.format.fontFamily
                && (raw.weight() >= QFont::Bold) == span.format.fontBold
                && (raw.style() != QFont::StyleNormal) == span.format.fontItalic;
            for (uint character : spanText.toUcs4()) {
                if (!QChar::isSpace(static_cast<char32_t>(character))
                    && !raw.supportsCharacter(character)) {
                    supportsText = false;
                    break;
                }
            }
            if (supportsText)
                continue;
        }
        const QString bundledFamily = matchingBundledFamilyName(semanticFamily);
        const QString systemFamily = matchingSystemFamilyName(
            semanticFamily);
        const QString downloadedFamily = matchingKnownFontFamilyName(
            semanticFamily, ApplicationFonts::downloadedFontFamilies());
        if (!systemFamily.isEmpty()
            && ApplicationFonts::fontFaceSupportsText(systemFamily,
                span.format.fontBold, span.format.fontItalic, spanText,
                ApplicationFonts::FontSource::System)) {
            span.format.fontFamily = systemFamily;
        } else if (!downloadedFamily.isEmpty()
            && ApplicationFonts::fontFaceSupportsText(downloadedFamily,
                span.format.fontBold, span.format.fontItalic, spanText,
                ApplicationFonts::FontSource::Downloaded)) {
            span.format.fontFamily = downloadedFamily;
        } else if (!bundledFamily.isEmpty()) {
            span.format.fontFamily = bundledFamily;
        } else if (!systemFamily.isEmpty()
                   && !isGenericArialCompatibleFontFamily(
                       semanticFamily)) {
            span.format.fontFamily = systemFamily;
        } else if (!bundledArial.isEmpty()
                   && equivalentFontFamilyName(
                       semanticFamily,
                       PDFFontAliases::semanticFamily(originalFormat.fontFamily))
                   && isArialCompatibleFontFamily(semanticFamily)) {
            span.format.fontFamily = bundledArial;
        } else if (!systemFamily.isEmpty()) {
            span.format.fontFamily = systemFamily;
        }
        if (span.format.fontFamily != previousFamily) {
            span.format.sourceAdvanceRatio = 0.0;
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
    m_editorVirtualSpacesPrepared = false;
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

    const QString fontFamily = m_typingCharacterFormat.fontFamily;
    const int fontStretch = m_typingCharacterFormat.fontStretch;
    const qreal sourceAdvanceRatio = m_typingCharacterFormat.sourceAdvanceRatio;
    const bool fontBold = m_typingCharacterFormat.fontBold;
    const bool fontItalic = m_typingCharacterFormat.fontItalic;
    const qreal textShear = m_typingCharacterFormat.textShear;
    const bool fontUnderline = m_typingCharacterFormat.fontUnderline;
    const qreal fontSizeRatio = m_typingCharacterFormat.fontSizeRatio;
    const qreal displayFontSizeRatio = m_typingCharacterFormat.displayFontSizeRatio;
    const qreal letterSpacingRatio = m_typingCharacterFormat.letterSpacingRatio;
    const qreal wordSpacingRatio = m_typingCharacterFormat.wordSpacingRatio;
    const QColor color = m_typingCharacterFormat.color;
    m_typingCharacterFormat = characterFormatAtPosition(
        draft,
        m_typingCharacterFormatPosition,
        true);
    m_typingCharacterFormat.fontFamily = fontFamily;
    m_typingCharacterFormat.fontStretch = fontStretch;
    m_typingCharacterFormat.sourceAdvanceRatio = sourceAdvanceRatio;
    m_typingCharacterFormat.fontBold = fontBold;
    m_typingCharacterFormat.fontItalic = fontItalic;
    m_typingCharacterFormat.textShear = textShear;
    m_typingCharacterFormat.fontUnderline = fontUnderline;
    m_typingCharacterFormat.fontSizeRatio = fontSizeRatio;
    m_typingCharacterFormat.displayFontSizeRatio = displayFontSizeRatio;
    m_typingCharacterFormat.letterSpacingRatio = letterSpacingRatio;
    m_typingCharacterFormat.wordSpacingRatio = wordSpacingRatio;
    m_typingCharacterFormat.color = color;
}
