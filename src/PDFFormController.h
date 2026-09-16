#pragma once

#include "PDFDocument.h"
#include "PDFFormWorker.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <functional>

class PDFFormController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(PDFDocument *document READ document WRITE setDocument
                   NOTIFY documentChanged FINAL)
    Q_PROPERTY(bool interactive READ interactive NOTIFY interactiveChanged FINAL)
    Q_PROPERTY(bool hasDrafts READ hasDrafts NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool hasFocus READ hasFocus NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(bool acceptsTextInput READ acceptsTextInput
                   NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(int focusedPage READ focusedPage
                   NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(int focusedAnnotationIndex READ focusedAnnotationIndex
                   NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyStateChanged FINAL)
    Q_PROPERTY(int cursorType READ cursorType NOTIFY inputStateChanged FINAL)
    Q_PROPERTY(bool javascriptEnabled READ javascriptEnabled
                   WRITE setJavaScriptEnabled
                   NOTIFY javascriptEnabledChanged FINAL)
    Q_PROPERTY(bool javascriptTransitionPending
                   READ javascriptTransitionPending
                   NOTIFY javascriptTransitionPendingChanged FINAL)

public:
    using SnapshotCompletion
        = std::function<void(bool, const QString &, const QString &)>;
    using ChoiceFieldsCompletion
        = std::function<void(QVector<PDFFormWorker::ChoiceField>)>;
    using TrePDFTextFieldsCompletion = std::function<void(
        bool,
        QVector<PDFFormWorker::TrePDFTextField>)>;
    using ChoiceFocusCompletion = std::function<void(bool)>;

    explicit PDFFormController(QObject *parent = nullptr);
    ~PDFFormController() override;

    PDFDocument *document() const;
    void setDocument(PDFDocument *document);

    bool interactive() const;
    bool hasDrafts() const;
    bool hasFocus() const;
    bool acceptsTextInput() const;
    int focusedPage() const;
    int focusedAnnotationIndex() const;
    bool canUndo() const;
    bool canRedo() const;
    bool focusRequestPending() const;
    int cursorType() const;
    bool javascriptEnabled() const;
    void setJavaScriptEnabled(bool enabled);
    bool javascriptTransitionPending() const;
    quint64 formRevisionForPage(int page) const;
    const QSet<int> &changedPages() const;
    bool allPagesChanged() const;
    PDFFormWorker::DocumentKey documentKey() const;

    void pointerEvent(int page,
                      PDFFormWorker::PointerEventType type,
                      int modifiers,
                      const QPointF &position,
                      const QSize &deviceSize);
    void keyEvent(int virtualKey,
                  int modifiers,
                  const QString &text);
    void selectAll();
    void copySelection(bool cut);
    void replaceSelection(const QString &text);
    void undo();
    void redo();
    void killFocus();
    void discardChanges();
    void exportSnapshot(const SnapshotCompletion &completion);
    bool exportSnapshotBlocking(QString &filePath, QString &error);
    void requestChoiceFields(int page,
                             const ChoiceFieldsCompletion &completion);
    void requestTrePDFTextFields(
        const TrePDFTextFieldsCompletion &completion);
    void selectChoiceOption(int page,
                            int annotationIndex,
                            int optionIndex,
                            bool selected,
                            int expectedSourcePage = -1,
                            bool toggle = false,
                            const QString &expectedIdentityToken = {});
    void focusChoiceField(int page,
                          int annotationIndex,
                          int expectedSourcePage = -1,
                          const QString &expectedIdentityToken = {},
                          const ChoiceFocusCompletion &completion = {});
    Q_INVOKABLE bool confirmExternalLink(quint64 requestId);
    quint64 requestExternalWebLink(const QString &url);
    Q_INVOKABLE void cancelExternalLink(quint64 requestId);

signals:
    void documentChanged();
    void interactiveChanged();
    void historyStateChanged();
    void inputStateChanged();
    void formRenderStateChanged(int page, quint64 revision);
    void javascriptEnabledChanged();
    void javascriptTransitionPendingChanged();
    void javascriptEnableFailed();
    void pageRollbackRequested(int page);
    void pageNavigationRequested(int page);
    void externalLinkRequested(quint64 requestId,
                               const QString &authority,
                               const QString &displayUrl,
                               bool insecureHttp);
    void externalLinkRequestCancelled(quint64 requestId);

private:
    void refreshDocumentState();
    void handlePageStructureChange(bool currentPageMappingChanged);
    void submitEditCommand(PDFFormWorker::EditCommand command,
                           const QString &text = {},
                           bool copyResultToClipboard = false);
    PDFFormWorker::InteractionCallback interactionCallback(
        const PDFFormWorker::DocumentKey &key,
        quint64 generation,
        bool copyResultToClipboard = false,
        bool pendingMutation = false,
        bool pendingFocusRequest = false);
    void beginPendingMutation();
    void queueJavaScriptState(bool enabled);
    void submitCurrentPageChange();
    void rollbackCurrentPage(int page);
    void invalidateFormPages(int page);
    void processHostActions(
        const PDFFormWorker::InteractionResult &result,
        quint64 navigationEpoch);
    void clearPendingExternalLink(bool notify);
    void applyInteractionResult(
        const PDFFormWorker::InteractionResult &result,
        bool copyResultToClipboard = false);
    static QUrl validatedExternalUrl(const QByteArray &encodedUrl);
    static bool sameDocumentKey(const PDFFormWorker::DocumentKey &left,
                                const PDFFormWorker::DocumentKey &right);
    QString createSnapshotPath(QString &error) const;

    QPointer<PDFDocument> m_document;
    const quint64 m_sessionId;
    quint64 m_generation = 0;
    quint64 m_formRevisionSerial = 0;
    quint64 m_allPagesFormRevision = 0;
    QHash<int, quint64> m_pageFormRevisions;
    bool m_interactive = false;
    bool m_hasDrafts = false;
    bool m_hasFocus = false;
    bool m_acceptsTextInput = false;
    int m_focusedPage = -1;
    int m_focusedAnnotationIndex = -1;
    bool m_canUndo = false;
    bool m_canRedo = false;
    int m_pendingFocusRequestCount = 0;
    int m_cursorType = FXCT_ARROW;
    int m_pendingMutationCount = 0;
    QSet<int> m_changedPages;
    bool m_allPagesChanged = false;
    quint64 m_javascriptRequestSerial = 0;
    quint64 m_pageChangeRequestSerial = 0;
    quint64 m_trePDFTextFieldsRequestSerial = 0;
    quint64 m_navigationEpoch = 0;
    quint64 m_nextExternalLinkRequestId = 0;
    quint64 m_pendingExternalLinkRequestId = 0;
    quint64 m_pendingExternalLinkGeneration = 0;
    PDFFormWorker::DocumentKey m_pendingExternalLinkKey;
    QUrl m_pendingExternalLinkUrl;
    bool m_ignoreNextCurrentPageChange = false;
    bool m_javascriptEnabled = false;
    bool m_javascriptTransitionPending = false;
};
