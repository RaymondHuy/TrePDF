#pragma once

#include <QByteArray>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <functional>
#include <optional>

#include <fpdf_formfill.h>

namespace PDFFormWorker {

struct DocumentKey {
    quint64 sessionId = 0;
    QString filePath;
    int revision = -1;

    bool isValid() const
    {
        return !filePath.isEmpty() && revision >= 0;
    }
};

struct GoToRequest {
    int sourcePage = -1;
    int zoomMode = 0;
    std::array<float, 4> positions = {};
    int positionCount = 0;
};

struct InteractionResult {
    bool handled = false;
    bool changed = false;
    bool invalidated = false;
    bool hasFocus = false;
    bool acceptsTextInput = false;
    bool canUndo = false;
    bool canRedo = false;
    int page = -1;
    int focusedPage = -1;
    int focusedAnnotationIndex = -1;
    int cursorType = FXCT_ARROW;
    QString selectedText;
    std::optional<GoToRequest> goTo;
    QByteArray externalUri;
};

struct SnapshotResult {
    bool succeeded = false;
    QString error;
    InteractionResult interaction;
};

struct JavaScriptResult {
    bool succeeded = false;
    bool enabled = false;
    int activePage = -1;
    QString error;
    InteractionResult interaction;
};

struct PageChangeResult {
    bool succeeded = false;
    int activePage = -1;
    InteractionResult interaction;
};

struct ChoiceField {
    int annotationIndex = -1;
    int sourcePage = -1;
    int fieldType = FPDF_FORMFIELD_UNKNOWN;
    QRectF normalizedRect;
    QString accessibleName;
    QString identityToken;
    QStringList options;
    QVector<int> selectedIndices;
    bool readOnly = false;
    bool multiSelect = false;
};

struct ChoiceFieldsResult {
    bool succeeded = false;
    int page = -1;
    QVector<ChoiceField> fields;
};

enum class PointerEventType {
    Move,
    Press,
    Release,
    DoubleClick,
};

enum class EditCommand {
    SelectAll,
    Copy,
    Cut,
    Paste,
    Undo,
    Redo,
    KillFocus,
};

using InteractionCallback = std::function<void(InteractionResult)>;
using SnapshotCallback = std::function<void(SnapshotResult)>;
using JavaScriptCallback = std::function<void(JavaScriptResult)>;
using PageChangeCallback = std::function<void(PageChangeResult)>;
using ChoiceFieldsCallback = std::function<void(ChoiceFieldsResult)>;
using PageCallback = std::function<bool(FPDF_DOCUMENT,
                                        FPDF_FORMHANDLE,
                                        FPDF_PAGE,
                                        FPDF_PAGE)>;

void submitRender(const void *owner,
                  quint64 sessionId,
                  std::function<void()> work);
void cancelRender(const void *owner);

bool withPageLocked(const DocumentKey &key,
                    int sourcePage,
                    const PageCallback &callback);

void submitPointerEvent(const DocumentKey &key,
                        int page,
                        int sourcePage,
                        PointerEventType type,
                        int modifiers,
                        const QPointF &position,
                        const QSize &deviceSize,
                        const InteractionCallback &callback);
void submitKeyEvent(const DocumentKey &key,
                    int virtualKey,
                    int modifiers,
                    const QString &text,
                    const InteractionCallback &callback);
void submitEditCommand(const DocumentKey &key,
                       EditCommand command,
                       const QString &text,
                       const InteractionCallback &callback);
void setJavaScriptEnabled(const DocumentKey &key,
                          bool enabled,
                          int page,
                          int sourcePage,
                          const JavaScriptCallback &callback);
void submitPageChange(const DocumentKey &key,
                      int page,
                      int sourcePage,
                      const PageChangeCallback &callback);
void inspectChoiceFields(const DocumentKey &key,
                         int page,
                         int sourcePage,
                         const ChoiceFieldsCallback &callback);
void submitChoiceSelection(const DocumentKey &key,
                           int page,
                           int sourcePage,
                           int annotationIndex,
                           int optionIndex,
                           bool selected,
                           bool toggle,
                           const QString &expectedIdentityToken,
                           const InteractionCallback &callback);
void submitChoiceFocus(const DocumentKey &key,
                       int page,
                       int sourcePage,
                       int annotationIndex,
                       const QString &expectedIdentityToken,
                       const InteractionCallback &callback);
void resetSession(quint64 sessionId,
                  const InteractionCallback &callback = {});
void destroySession(quint64 sessionId);
void exportSnapshot(const DocumentKey &key,
                    const QString &filePath,
                    const SnapshotCallback &callback);
SnapshotResult exportSnapshotBlocking(const DocumentKey &key,
                                      const QString &filePath);

} // namespace PDFFormWorker
