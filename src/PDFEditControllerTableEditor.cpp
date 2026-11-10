#include "PDFEditController.h"
#include "PDFTableCellFormat.h"
#include "PDFFontAliases.h"

#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QQuickTextDocument>
#include <QScopedValueRollback>
#include <QTextCursor>
#include <QTextDocument>
#include <QtTranslation>
#include <memory>
#include <utility>

namespace {
using namespace PDFTableCellFormat;
constexpr int SemanticFamilyProperty = QTextFormat::UserProperty + 421;
constexpr char CellRotationProperty[] = "trepdfTableCellRotation";

QTextCharFormat characterFormat(const Style &style)
{
    QTextCharFormat format;
    format.setFont(displayFont(style));
    format.setForeground(Qt::black);
    format.setProperty(SemanticFamilyProperty, style.family);
    return format;
}

class TypingCursorBoundary final : public QAbstractUndoItem
{
public:
    void undo() override {}
    void redo() override {}
};

// A table is the selected object; its temporary editor owns character formats
// independently of the ordinary selected-text editing session.
class TableCellEditor final : public QObject
{
public:
    explicit TableCellEditor(QQuickTextDocument *document)
        : QObject(document), m_document(document)
    {
        setObjectName(QStringLiteral("trepdfTableCellEditor"));
        resetChanges();
        connect(textDocument(), &QTextDocument::contentsChange, this,
            [this](int position, int removed, int added) {
                if (m_formatting || (removed == 0 && added == 0))
                    return;
                const int start = qBound(0, position, int(m_inserted.size()));
                m_inserted.remove(start, qBound(0, removed, int(m_inserted.size()) - start));
                m_inserted.insert(start, qMax(0, added), true);
                m_changed = true;
            });
        connect(textDocument(), &QTextDocument::undoCommandAdded, this, [this]() {
            if (!m_formatting)
                m_newCommand = true;
        });
        document->parent()->installEventFilter(this);
    }

    void initialize(const QString &text, const Cell &format, const QVariantMap &state)
    {
        synchronize();
        QScopedValueRollback<bool> formatting(m_formatting, true);
        auto *document = textDocument();
        const bool undoEnabled = document->isUndoRedoEnabled();
        document->setUndoRedoEnabled(false);
        m_defaultStyle = format.defaultStyle;
        document->setProperty(CellRotationProperty, format.rotation);
        document->setDefaultFont(displayFont(m_defaultStyle));
        if (document->toPlainText() != text)
            document->setPlainText(text);
        QTextCursor cursor(document);
        cursor.select(QTextCursor::Document);
        cursor.setCharFormat(characterFormat(m_defaultStyle));
        for (const Span &span : format.spans) {
            cursor.setPosition(span.start);
            cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
            cursor.setCharFormat(characterFormat(span.style));
        }
        document->setUndoRedoEnabled(undoEnabled);
        m_cursorPosition = cursorPosition();
        m_typingPosition = -1;
        Style stored;
        if (state.value(QStringLiteral("typingPosition"), -1).toInt() == m_cursorPosition
            && readStyle(QJsonObject::fromVariantMap(state.value(QStringLiteral("typingStyle")).toMap()), stored)) {
            m_typingStyle = stored;
            m_typingPosition = m_cursorPosition;
        }
        m_undoSteps = document->availableUndoSteps();
        resetChanges();
    }

    QVariantMap state()
    {
        synchronize();
        const QString text = textDocument()->toPlainText();
        Cell cell{m_defaultStyle, {}};
        cell.rotation = textDocument()->property(CellRotationProperty).toInt();
        if (text.isEmpty() && m_typingPosition >= 0)
            cell.defaultStyle = m_typingStyle;
        for (int start = 0; start < text.size();) {
            const Style style = styleAtPosition(start);
            int end = start + 1;
            while (end < text.size() && styleAtPosition(end) == style)
                ++end;
            cell.spans.append({start, end - start, style});
            start = end;
        }
        const Style selected = currentStyle();
        return {{QStringLiteral("text"), text},
                {QStringLiteral("format"), toVariant(cell)},
                {QStringLiteral("cursorPosition"), m_cursorPosition},
                {QStringLiteral("selectionStart"), selectionStart()},
                {QStringLiteral("selectionEnd"), selectionEnd()},
                {QStringLiteral("typingPosition"), m_typingPosition},
                {QStringLiteral("typingStyle"), styleJson(m_typingStyle).toVariantMap()},
                {QStringLiteral("fontFamily"), selected.family},
                {QStringLiteral("fontSize"), selected.fontSize},
                {QStringLiteral("rotation"), cell.rotation},
                {QStringLiteral("bold"), selected.bold},
                {QStringLiteral("italic"), selected.italic},
                {QStringLiteral("underline"), selected.underline}};
    }

    QString apply(const QString &property, const QVariant &value)
    {
        synchronize();
        if (property == QLatin1String("rotation")) {
            bool numeric = false;
            const double rotation = value.toDouble(&numeric);
            if (!numeric || (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270))
                return qtTrId("TableCellInvalidFormat");
            textDocument()->setProperty(CellRotationProperty, static_cast<int>(rotation));
            return {};
        }
        if (property != QLatin1String("family") && property != QLatin1String("bold")
            && property != QLatin1String("italic") && property != QLatin1String("underline")
            && property != QLatin1String("fontSize"))
            return {};
        if (property == QLatin1String("fontSize") && !validFontSize(value.toDouble())) {
            //% "The cell formatting is invalid or too complex. Use fewer font or style changes."
            return qtTrId("TableCellInvalidFormat");
        }
        const auto update = [&](Style style) {
            if (property == QLatin1String("family")) style.family = value.toString();
            else if (property == QLatin1String("fontSize")) style.fontSize = value.toDouble();
            else if (property == QLatin1String("bold")) style.bold = value.toBool();
            else if (property == QLatin1String("italic")) style.italic = value.toBool();
            else style.underline = value.toBool();
            return style;
        };
        const QString text = textDocument()->toPlainText();
        const int start = selectionStart();
        const int end = selectionEnd();
        Cell proposed{update(currentStyle()), {}};
        for (int position = start; position < end;) {
            const Style style = update(styleAtPosition(position));
            int stop = position + 1;
            while (stop < end && update(styleAtPosition(stop)) == style)
                ++stop;
            proposed.spans.append({position, stop - position, style});
            position = stop;
        }
        const auto selectable = [](const Style &style, const QString &characters) {
            return validStyle(style)
                && ApplicationFonts::isFontFaceSelectable(style.family, style.bold, style.italic)
                && (characters.isEmpty() || ApplicationFonts::fontFaceSupportsText(
                    style.family, style.bold, style.italic, characters));
        };
        bool supported = selectable(proposed.defaultStyle, {});
        for (const auto &span : proposed.spans)
            supported = supported && selectable(span.style, text.sliced(span.start, span.length));
        if (!supported) {
            //% "The table font is unavailable or does not support this text. Install the font in Settings > Font."
            return qtTrId("TableCellFontUnavailable");
        }
        if (start < end) {
            QScopedValueRollback<bool> formatting(m_formatting, true);
            QTextCursor cursor(textDocument());
            cursor.beginEditBlock();
            for (const auto &span : proposed.spans) {
                cursor.setPosition(span.start);
                cursor.setPosition(span.start + span.length, QTextCursor::KeepAnchor);
                cursor.setCharFormat(characterFormat(span.style));
            }
            cursor.endEditBlock();
            m_undoSteps = textDocument()->availableUndoSteps();
            resetChanges();
        }
        m_typingStyle = proposed.defaultStyle;
        m_typingPosition = m_cursorPosition;
        return {};
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease
            && event->type() != QEvent::InputMethod)
            return false;
        synchronize();
        bool changesText = false;
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            changesText = (!key->text().isEmpty()
                && !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)))
                || key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete
                || key->matches(QKeySequence::Paste) || key->matches(QKeySequence::Cut);
            if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo))
                m_typingPosition = -1;
        } else if (event->type() == QEvent::InputMethod) {
            const auto *input = static_cast<QInputMethodEvent *>(event);
            changesText = !input->commitString().isEmpty() || input->replacementLength() > 0;
        }
        if (changesText) {
            m_nativeEdit = true;
            m_nativeBefore = textDocument()->toPlainText();
            m_nativeStart = selectionStart();
            m_nativeEnd = selectionEnd();
            if (event->type() == QEvent::InputMethod) {
                const auto *input = static_cast<QInputMethodEvent *>(event);
                if (input->replacementStart() != 0 || input->replacementLength() != 0) {
                    m_nativeStart = qBound(0, cursorPosition() + input->replacementStart(),
                                          int(m_nativeBefore.size()));
                    m_nativeEnd = qBound(m_nativeStart,
                        m_nativeStart + input->replacementLength(), int(m_nativeBefore.size()));
                }
            }
            m_nativeSnapshot = true;
            // Preserve Qt's native coalescing for ordinary typing. An explicit
            // block only joins a pending caret format to the inserted text.
            if (m_typingPosition >= 0) {
                m_editCursor = QTextCursor(textDocument());
                m_editCursor.setPosition(cursorPosition());
                m_editCursor.beginEditBlock();
            }
            QMetaObject::invokeMethod(this, [this]() { synchronize(); }, Qt::QueuedConnection);
        }
        return false;
    }

private:
    QTextDocument *textDocument() const { return m_document->textDocument(); }
    int cursorPosition() const {
        return qBound(0, m_document->parent()->property("cursorPosition").toInt(),
                      textDocument()->characterCount() - 1);
    }
    int selectionStart() const {
        return qBound(0, m_document->parent()->property("selectionStart").toInt(),
                      textDocument()->characterCount() - 1);
    }
    int selectionEnd() const {
        return qBound(selectionStart(), m_document->parent()->property("selectionEnd").toInt(),
                      textDocument()->characterCount() - 1);
    }
    Style styleAtPosition(int position) const {
        if (position < 0 || position >= textDocument()->characterCount() - 1)
            return m_defaultStyle;
        QTextCursor cursor(textDocument());
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat format = cursor.charFormat();
        QString family = format.stringProperty(SemanticFamilyProperty);
        if (family.isEmpty()) {
            const QStringList families = format.fontFamilies().toStringList();
            family = families.isEmpty() ? m_defaultStyle.family
                : PDFFontAliases::semanticFamily(families.constFirst());
        }
        const int pixels = format.font().pixelSize();
        return {family, format.fontWeight() >= QFont::Bold,
                format.fontItalic(), format.fontUnderline(),
                pixels > 0 ? qreal(pixels) : m_defaultStyle.fontSize};
    }
    Style currentStyle() const {
        if (selectionStart() < selectionEnd())
            return styleAtPosition(selectionStart());
        return m_typingPosition == cursorPosition() ? m_typingStyle
            : styleAtPosition(qMax(0, cursorPosition() - 1));
    }
    void resetChanges() {
        m_inserted.fill(false, qMax(0, textDocument()->characterCount() - 1));
        m_changed = false;
        m_newCommand = false;
    }
    void synchronize() {
        if (m_synchronizing)
            return;
        QScopedValueRollback<bool> synchronizing(m_synchronizing, true);
        if (!m_editCursor.isNull()) {
            QTextCursor cursor = std::exchange(m_editCursor, QTextCursor());
            cursor.endEditBlock();
        }
        if (m_nativeSnapshot) {
            // IME composition can report an entire paragraph as replaced. Only
            // newly committed characters inherit the explicit typing style.
            const QString after = textDocument()->toPlainText();
            int start = m_nativeStart;
            int end = m_nativeEnd;
            int inserted = int(after.size()) - (int(m_nativeBefore.size()) - (end - start));
            if (inserted < 0 || start + inserted > after.size()
                || after.first(start) != m_nativeBefore.first(start)
                || after.sliced(start + inserted) != m_nativeBefore.sliced(end)) {
                start = 0;
                while (start < after.size() && start < m_nativeBefore.size()
                       && after.at(start) == m_nativeBefore.at(start))
                    ++start;
                end = int(m_nativeBefore.size());
                int afterEnd = int(after.size());
                while (end > start && afterEnd > start
                       && m_nativeBefore.at(end - 1) == after.at(afterEnd - 1)) {
                    --end;
                    --afterEnd;
                }
                inserted = afterEnd - start;
            }
            m_inserted.fill(false, after.size());
            for (int position = start; position < start + inserted; ++position)
                m_inserted[position] = true;
            m_nativeSnapshot = false;
            m_nativeBefore.clear();
        }
        const bool edited = m_changed && (m_nativeEdit || m_newCommand);
        const int currentPosition = cursorPosition();
        if (edited && m_typingPosition >= 0) {
            const int maximum = qMin(int(m_inserted.size()), textDocument()->characterCount() - 1);
            bool needsFormat = false;
            for (int position = 0; position < maximum && !needsFormat; ++position)
                needsFormat = m_inserted.at(position) && styleAtPosition(position) != m_typingStyle;
            if (needsFormat) {
                QScopedValueRollback<bool> formatting(m_formatting, true);
                QTextCursor cursor(textDocument());
                cursor.joinPreviousEditBlock();
                for (int start = 0; start < maximum;) {
                    if (!m_inserted.at(start)) { ++start; continue; }
                    int end = start + 1;
                    while (end < maximum && m_inserted.at(end)) ++end;
                    cursor.setPosition(start);
                    cursor.setPosition(end, QTextCursor::KeepAnchor);
                    cursor.setCharFormat(characterFormat(m_typingStyle));
                    start = end;
                }
                auto boundary = std::make_unique<TypingCursorBoundary>();
                textDocument()->appendUndoItem(boundary.release());
                cursor.endEditBlock();
            }
            m_typingPosition = currentPosition;
        } else if ((!edited && textDocument()->availableUndoSteps() != m_undoSteps)
                   || currentPosition != m_cursorPosition) {
            m_typingPosition = -1;
        }
        m_cursorPosition = currentPosition;
        m_undoSteps = textDocument()->availableUndoSteps();
        m_nativeEdit = false;
        resetChanges();
    }

    QQuickTextDocument *m_document;
    QTextCursor m_editCursor;
    QVector<bool> m_inserted;
    Style m_defaultStyle;
    Style m_typingStyle;
    int m_cursorPosition = 0;
    int m_typingPosition = -1;
    int m_undoSteps = 0;
    QString m_nativeBefore;
    int m_nativeStart = 0;
    int m_nativeEnd = 0;
    bool m_nativeSnapshot = false;
    bool m_nativeEdit = false;
    bool m_changed = false;
    bool m_newCommand = false;
    bool m_formatting = false;
    bool m_synchronizing = false;
};

TableCellEditor *tableEditor(QQuickTextDocument *document)
{
    if (!document || !document->textDocument() || !document->parent())
        return nullptr;
    return static_cast<TableCellEditor *>(document->findChild<QObject *>(
        QStringLiteral("trepdfTableCellEditor"), Qt::FindDirectChildrenOnly));
}
} // namespace

void PDFEditController::initializeTableCellEditor(QQuickTextDocument *document,
    const QString &text, const QVariantMap &format, const QVariantMap &editorState)
{
    if (!document || !document->textDocument() || !document->parent())
        return;
    PDFTableCellFormat::Cell cell;
    // Pending input may exceed the persistence run limit while the user fixes
    // a validation error. Pooling must retain those formats, not flatten them.
    if (!PDFTableCellFormat::fromVariant(format, text, cell, 2048))
        cell = PDFTableCellFormat::plain(text, {headerTextFontFamily(), false, false, false});
    auto *editor = tableEditor(document);
    if (!editor) {
        auto owned = std::make_unique<TableCellEditor>(document);
        editor = owned.release(); // QQuickTextDocument owns the QObject child.
    }
    editor->initialize(text, cell, editorState);
}

QVariantMap PDFEditController::tableCellEditorState(QQuickTextDocument *document) const
{
    auto *editor = tableEditor(document);
    return editor ? editor->state() : QVariantMap();
}

QString PDFEditController::applyTableCellEditorFormat(QQuickTextDocument *document,
    const QString &property, const QVariant &value)
{
    auto *editor = tableEditor(document);
    return editor ? editor->apply(property, value) : QString();
}
