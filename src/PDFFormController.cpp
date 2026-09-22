#include "PDFFormController.h"

#include "PDFiumSynchronization.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QMetaObject>
#include <QTemporaryFile>

#include <atomic>

namespace {

quint64 nextFormSessionId()
{
    static std::atomic<quint64> nextId{1};
    return nextId.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

PDFFormController::PDFFormController(QObject *parent)
    : QObject(parent)
    , m_sessionId(nextFormSessionId())
{
}

PDFFormController::~PDFFormController()
{
    ++m_generation;
    PDFFormWorker::destroySession(m_sessionId);
}

PDFDocument *PDFFormController::document() const
{
    return m_document;
}

void PDFFormController::setDocument(PDFDocument *document)
{
    if (m_document == document) {
        return;
    }
    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
    }

    m_document = document;
    if (m_document) {
        connect(
            m_document,
            &PDFDocument::sourceChanged,
            this,
            &PDFFormController::refreshDocumentState);
        connect(
            m_document,
            &PDFDocument::loadedChanged,
            this,
            &PDFFormController::refreshDocumentState);
        connect(
            m_document,
            &PDFDocument::currentPageChanged,
            this,
            &PDFFormController::submitCurrentPageChange);
        connect(
            m_document,
            &PDFDocument::pageStructureChanged,
            this,
            &PDFFormController::handlePageStructureChange);
        connect(m_document, &QObject::destroyed, this, [this]() {
            m_document = nullptr;
            refreshDocumentState();
            emit documentChanged();
        });
    }

    refreshDocumentState();
    emit documentChanged();
}

bool PDFFormController::interactive() const
{
    return m_interactive;
}

bool PDFFormController::hasDrafts() const
{
    return m_hasDrafts || m_pendingMutationCount > 0;
}

bool PDFFormController::hasFocus() const
{
    return m_hasFocus;
}

bool PDFFormController::acceptsTextInput() const
{
    return m_acceptsTextInput;
}

int PDFFormController::focusedPage() const
{
    return m_focusedPage;
}

int PDFFormController::focusedAnnotationIndex() const
{
    return m_focusedAnnotationIndex;
}

bool PDFFormController::canUndo() const
{
    return m_canUndo;
}

bool PDFFormController::canRedo() const
{
    return m_canRedo;
}

bool PDFFormController::focusRequestPending() const
{
    return m_pendingFocusRequestCount > 0;
}

int PDFFormController::cursorType() const
{
    return m_cursorType;
}

bool PDFFormController::javascriptEnabled() const
{
    return m_javascriptEnabled;
}

void PDFFormController::setJavaScriptEnabled(bool enabled)
{
    if (m_javascriptEnabled == enabled) {
        return;
    }
    if (enabled && !documentKey().isValid()) {
        return;
    }

    if (!enabled) {
        clearPendingExternalLink(true);
    }
    m_javascriptEnabled = enabled;
    emit javascriptEnabledChanged();
    queueJavaScriptState(enabled);
}

bool PDFFormController::javascriptTransitionPending() const
{
    return m_javascriptTransitionPending;
}

quint64 PDFFormController::formRevisionForPage(int page) const
{
    return m_pageFormRevisions.value(page, m_allPagesFormRevision);
}

const QSet<int> &PDFFormController::changedPages() const
{
    return m_changedPages;
}

bool PDFFormController::allPagesChanged() const
{
    return m_allPagesChanged;
}

PDFFormWorker::DocumentKey PDFFormController::documentKey() const
{
    PDFFormWorker::DocumentKey key;
    key.sessionId = m_sessionId;
    if (m_document && m_document->isLoaded()) {
        key.filePath = m_document->renderFilePath();
        key.revision = m_document->renderRevision();
        key.password = m_document->documentPassword();
    }
    return key;
}

void PDFFormController::pointerEvent(
    int page,
    PDFFormWorker::PointerEventType type,
    int modifiers,
    const QPointF &position,
    const QSize &deviceSize)
{
    if (!m_interactive || !m_document || !m_document->isLoaded()) {
        return;
    }
    const int sourcePage = m_document->sourcePageIndex(page);
    if (sourcePage < 0) {
        return;
    }

    const PDFFormWorker::DocumentKey key = documentKey();
    const quint64 generation = m_generation;
    const bool pendingMutation
        = type != PDFFormWorker::PointerEventType::Move;
    const bool pendingFocusRequest
        = type == PDFFormWorker::PointerEventType::Press;
    if (pendingMutation) {
        beginPendingMutation();
    }
    if (pendingFocusRequest) {
        ++m_pendingFocusRequestCount;
    }
    PDFFormWorker::submitPointerEvent(
        key,
        page,
        sourcePage,
        type,
        modifiers,
        position,
        deviceSize,
        interactionCallback(
            key,
            generation,
            false,
            pendingMutation,
            pendingFocusRequest),
        m_removedWebLinks.value(sourcePage));
}

void PDFFormController::keyEvent(
    int virtualKey,
    int modifiers,
    const QString &text)
{
    if (!m_interactive) {
        return;
    }
    const PDFFormWorker::DocumentKey key = documentKey();
    const quint64 generation = m_generation;
    beginPendingMutation();
    PDFFormWorker::submitKeyEvent(
        key,
        virtualKey,
        modifiers,
        text,
        interactionCallback(key, generation, false, true));
}

void PDFFormController::selectAll()
{
    submitEditCommand(PDFFormWorker::EditCommand::SelectAll);
}

void PDFFormController::copySelection(bool cut)
{
    submitEditCommand(
        cut ? PDFFormWorker::EditCommand::Cut
            : PDFFormWorker::EditCommand::Copy,
        {},
        true);
}

void PDFFormController::replaceSelection(const QString &text)
{
    submitEditCommand(PDFFormWorker::EditCommand::Paste, text);
}

void PDFFormController::undo()
{
    submitEditCommand(PDFFormWorker::EditCommand::Undo);
}

void PDFFormController::redo()
{
    submitEditCommand(PDFFormWorker::EditCommand::Redo);
}

void PDFFormController::killFocus()
{
    if (!m_interactive) {
        return;
    }
    submitEditCommand(PDFFormWorker::EditCommand::KillFocus);
}

void PDFFormController::discardChanges()
{
    if (!hasDrafts() && !m_hasFocus && !m_canUndo && !m_canRedo) {
        return;
    }

    ++m_generation;
    ++m_navigationEpoch;
    clearPendingExternalLink(true);
    const bool historyChanged = hasDrafts() || m_canUndo || m_canRedo;
    const bool inputChanged = m_hasFocus
        || m_acceptsTextInput
        || m_focusedPage >= 0
        || m_focusedAnnotationIndex >= 0
        || m_pendingFocusRequestCount > 0
        || m_cursorType != FXCT_ARROW;
    m_hasDrafts = false;
    m_changedPages.clear();
    m_allPagesChanged = false;
    m_hasFocus = false;
    m_acceptsTextInput = false;
    m_focusedPage = -1;
    m_focusedAnnotationIndex = -1;
    m_canUndo = false;
    m_canRedo = false;
    m_pendingFocusRequestCount = 0;
    m_cursorType = FXCT_ARROW;
    m_pendingMutationCount = 0;
    PDFFormWorker::resetSession(m_sessionId);
    if (m_javascriptEnabled) {
        queueJavaScriptState(true);
    }
    invalidateFormPages(-1);
    if (historyChanged) {
        emit historyStateChanged();
    }
    if (inputChanged) {
        emit inputStateChanged();
    }
}

void PDFFormController::exportSnapshot(
    const SnapshotCompletion &completion)
{
    QString error;
    const QString filePath = createSnapshotPath(error);
    if (filePath.isEmpty()) {
        if (completion) {
            completion(false, {}, error);
        }
        return;
    }

    const PDFFormWorker::DocumentKey key = documentKey();
    const quint64 generation = m_generation;
    QPointer<PDFFormController> controller(this);
    PDFFormWorker::exportSnapshot(
        key,
        filePath,
        [controller,
         key,
         generation,
         filePath,
         completion](PDFFormWorker::SnapshotResult result) {
            if (!controller
                || controller->m_generation != generation
                || !sameDocumentKey(controller->documentKey(), key)) {
                QFile::remove(filePath);
                if (completion) {
                    completion(
                        false,
                        {},
                        QStringLiteral("The source PDF changed before saving"));
                }
                return;
            }

            controller->applyInteractionResult(result.interaction);
            if (!result.succeeded) {
                QFile::remove(filePath);
            }
            if (completion) {
                completion(
                    result.succeeded,
                    result.succeeded ? filePath : QString(),
                    result.error);
            }
        });
}

bool PDFFormController::exportSnapshotBlocking(
    QString &filePath,
    QString &error)
{
    filePath = createSnapshotPath(error);
    if (filePath.isEmpty()) {
        return false;
    }

    const PDFFormWorker::DocumentKey key = documentKey();
    const PDFFormWorker::SnapshotResult result
        = PDFFormWorker::exportSnapshotBlocking(key, filePath);
    applyInteractionResult(result.interaction);
    if (!result.succeeded) {
        QFile::remove(filePath);
        filePath.clear();
        error = result.error;
        return false;
    }
    return true;
}

void PDFFormController::requestChoiceFields(
    int page,
    const ChoiceFieldsCompletion &completion)
{
    if (!completion) {
        return;
    }
    if (!m_interactive || !m_document || !m_document->isLoaded()) {
        completion({});
        return;
    }
    const int sourcePage = m_document->sourcePageIndex(page);
    if (sourcePage < 0) {
        completion({});
        return;
    }

    const PDFFormWorker::DocumentKey key = documentKey();
    const quint64 generation = m_generation;
    QPointer<PDFFormController> controller(this);
    PDFFormWorker::inspectChoiceFields(
        key,
        page,
        sourcePage,
        [controller,
         key,
         generation,
         page,
         sourcePage,
         completion](PDFFormWorker::ChoiceFieldsResult result) {
            if (!controller
                || controller->m_generation != generation
                || !sameDocumentKey(controller->documentKey(), key)
                || !controller->m_document
                || controller->m_document->sourcePageIndex(page)
                    != sourcePage
                || !result.succeeded
                || result.page != page) {
                completion({});
                return;
            }
            completion(std::move(result.fields));
        });
}

void PDFFormController::requestTrePDFTextFields(
    const TrePDFTextFieldsCompletion &completion)
{
    if (!completion) {
        return;
    }
    const quint64 requestSerial = ++m_trePDFTextFieldsRequestSerial;
    QPointer<PDFFormController> controller(this);
    QMetaObject::invokeMethod(
        this,
        [controller, requestSerial, completion]() {
            if (!controller
                || controller->m_trePDFTextFieldsRequestSerial
                    != requestSerial
                || !controller->m_document
                || !controller->m_document->isLoaded()) {
                completion(false, {});
                return;
            }

            const PDFFormWorker::DocumentKey key
                = controller->documentKey();
            if (!key.isValid()) {
                completion(false, {});
                return;
            }
            const quint64 generation = controller->m_generation;
            PDFFormWorker::inspectTrePDFTextFields(
                key,
                [controller,
                 key,
                 generation,
                 requestSerial,
                 completion](
                    PDFFormWorker::TrePDFTextFieldsResult result) {
                    if (!controller
                        || controller->m_trePDFTextFieldsRequestSerial
                            != requestSerial
                        || controller->m_generation != generation
                        || !sameDocumentKey(
                            controller->documentKey(),
                            key)
                        || !controller->m_document
                        || !controller->m_document->isLoaded()
                        || !result.succeeded) {
                        completion(false, {});
                        return;
                    }
                    completion(true, std::move(result.fields));
                });
        },
        Qt::QueuedConnection);
}

void PDFFormController::selectChoiceOption(
    int page,
    int annotationIndex,
    int optionIndex,
    bool selected,
    int expectedSourcePage,
    bool toggle,
    const QString &expectedIdentityToken)
{
    if (!m_interactive || !m_document || !m_document->isLoaded()) {
        return;
    }
    const int sourcePage = m_document->sourcePageIndex(page);
    if (sourcePage < 0
        || (expectedSourcePage >= 0
            && sourcePage != expectedSourcePage)
        || annotationIndex < 0
        || optionIndex < 0) {
        return;
    }

    const PDFFormWorker::DocumentKey key = documentKey();
    const quint64 generation = m_generation;
    beginPendingMutation();
    ++m_pendingFocusRequestCount;
    PDFFormWorker::submitChoiceSelection(
        key,
        page,
        sourcePage,
        annotationIndex,
        optionIndex,
        selected,
        toggle,
        expectedIdentityToken,
        interactionCallback(key, generation, false, true, true));
}

void PDFFormController::focusChoiceField(
    int page,
    int annotationIndex,
    int expectedSourcePage,
    const QString &expectedIdentityToken,
    const ChoiceFocusCompletion &completion)
{
    if (!m_interactive || !m_document || !m_document->isLoaded()) {
        if (completion) {
            completion(false);
        }
        return;
    }
    const int sourcePage = m_document->sourcePageIndex(page);
    if (sourcePage < 0
        || (expectedSourcePage >= 0
            && sourcePage != expectedSourcePage)
        || annotationIndex < 0) {
        if (completion) {
            completion(false);
        }
        return;
    }

    const PDFFormWorker::DocumentKey key = documentKey();
    const quint64 generation = m_generation;
    beginPendingMutation();
    ++m_pendingFocusRequestCount;
    QPointer<PDFFormController> controller(this);
    PDFFormWorker::InteractionCallback interaction
        = interactionCallback(key, generation, false, true, true);
    PDFFormWorker::submitChoiceFocus(
        key,
        page,
        sourcePage,
        annotationIndex,
        expectedIdentityToken,
        [interaction = std::move(interaction),
         controller,
         completion,
         key,
         generation,
         page,
         annotationIndex](PDFFormWorker::InteractionResult result) mutable {
            const bool focused
                = controller
                && controller->m_generation == generation
                && sameDocumentKey(controller->documentKey(), key)
                && result.hasFocus
                && result.focusedPage == page
                && result.focusedAnnotationIndex == annotationIndex;
            interaction(std::move(result));
            if (completion) {
                completion(focused);
            }
        });
}

bool PDFFormController::confirmExternalLink(quint64 requestId)
{
    if (requestId == 0
        || requestId != m_pendingExternalLinkRequestId) {
        return false;
    }

    const bool requestIsCurrent
        = m_pendingExternalLinkGeneration == m_generation
        && sameDocumentKey(
            m_pendingExternalLinkKey,
            documentKey());
    if (!requestIsCurrent) {
        clearPendingExternalLink(true);
        return false;
    }

    const QUrl url = m_pendingExternalLinkUrl;
    clearPendingExternalLink(false);
    assertPDFiumProcessLockNotHeld();
    return QDesktopServices::openUrl(url);
}

quint64 PDFFormController::requestExternalWebLink(const QString &url)
{
    if (!m_document || !m_document->isLoaded()
        || m_pendingExternalLinkRequestId != 0
        || validatedExternalUrl(url.toUtf8()).isEmpty()) {
        return 0;
    }
    PDFFormWorker::InteractionResult result;
    result.externalUri = url.toUtf8();
    processHostActions(result, m_navigationEpoch);
    return m_pendingExternalLinkRequestId;
}

void PDFFormController::setRemovedWebLinks(
    const QHash<int, QSet<int>> &removedWebLinks)
{
    if (m_removedWebLinks == removedWebLinks) {
        return;
    }
    m_removedWebLinks = removedWebLinks;
    clearPendingExternalLink(true);
    if (m_cursorType == FXCT_HAND) {
        m_cursorType = FXCT_ARROW;
        emit inputStateChanged();
    }
}

void PDFFormController::cancelExternalLink(quint64 requestId)
{
    if (requestId == 0
        || requestId != m_pendingExternalLinkRequestId) {
        return;
    }
    clearPendingExternalLink(true);
}

void PDFFormController::refreshDocumentState()
{
    ++m_generation;
    ++m_navigationEpoch;
    ++m_javascriptRequestSerial;
    ++m_pageChangeRequestSerial;
    clearPendingExternalLink(true);
    m_removedWebLinks.clear();
    m_ignoreNextCurrentPageChange = false;
    PDFFormWorker::resetSession(m_sessionId);

    const PDFFormWorker::DocumentKey key = documentKey();
    const bool interactive = m_document
        && m_document->isLoaded()
        && m_document->allowsFormFilling();
    const bool didInteractiveChange = m_interactive != interactive;
    const bool didJavaScriptChange = m_javascriptEnabled;
    const bool didJavaScriptPendingChange
        = m_javascriptTransitionPending != key.isValid();
    const bool historyChanged = hasDrafts() || m_canUndo || m_canRedo;
    const bool inputChanged = m_hasFocus
        || m_acceptsTextInput
        || m_focusedPage >= 0
        || m_focusedAnnotationIndex >= 0
        || m_pendingFocusRequestCount > 0
        || m_cursorType != FXCT_ARROW;
    m_interactive = interactive;
    m_hasDrafts = false;
    m_changedPages.clear();
    m_allPagesChanged = false;
    m_hasFocus = false;
    m_acceptsTextInput = false;
    m_focusedPage = -1;
    m_focusedAnnotationIndex = -1;
    m_canUndo = false;
    m_canRedo = false;
    m_pendingFocusRequestCount = 0;
    m_cursorType = FXCT_ARROW;
    m_pendingMutationCount = 0;
    m_javascriptEnabled = false;
    m_javascriptTransitionPending = key.isValid();
    invalidateFormPages(-1);
    if (didInteractiveChange) {
        emit interactiveChanged();
    }
    if (historyChanged) {
        emit historyStateChanged();
    }
    if (inputChanged) {
        emit inputStateChanged();
    }
    if (didJavaScriptChange) {
        emit javascriptEnabledChanged();
    }
    if (didJavaScriptPendingChange) {
        emit javascriptTransitionPendingChanged();
    }

    const quint64 generation = m_generation;
    if (key.isValid()) {
        // PDFDocument emits sourceChanged and loadedChanged in one state
        // update. Defer automatic form scripting until both signals settle so
        // document/open actions run only once for the current document.
        QMetaObject::invokeMethod(
            this,
            [this, key, generation]() {
                if (m_generation != generation
                    || !sameDocumentKey(documentKey(), key)) {
                    return;
                }
                setJavaScriptEnabled(true);
            },
            Qt::QueuedConnection);
    }
}

void PDFFormController::handlePageStructureChange(
    bool currentPageMappingChanged)
{
    ++m_navigationEpoch;
    clearPendingExternalLink(true);
    invalidateFormPages(-1);
    if (currentPageMappingChanged) {
        submitCurrentPageChange();
    }
}

void PDFFormController::submitEditCommand(
    PDFFormWorker::EditCommand command,
    const QString &text,
    bool copyResultToClipboard)
{
    if (!m_interactive) {
        return;
    }
    const PDFFormWorker::DocumentKey key = documentKey();
    const quint64 generation = m_generation;
    const bool pendingMutation
        = command == PDFFormWorker::EditCommand::Cut
        || command == PDFFormWorker::EditCommand::Paste
        || command == PDFFormWorker::EditCommand::Undo
        || command == PDFFormWorker::EditCommand::Redo;
    if (pendingMutation) {
        beginPendingMutation();
    }
    PDFFormWorker::submitEditCommand(
        key,
        command,
        text,
        interactionCallback(
            key,
            generation,
            copyResultToClipboard,
            pendingMutation));
}

PDFFormWorker::InteractionCallback PDFFormController::interactionCallback(
    const PDFFormWorker::DocumentKey &key,
    quint64 generation,
    bool copyResultToClipboard,
    bool pendingMutation,
    bool pendingFocusRequest)
{
    const quint64 navigationEpoch = m_navigationEpoch;
    QPointer<PDFFormController> controller(this);
    return [controller,
            key,
            generation,
            navigationEpoch,
            copyResultToClipboard,
            pendingMutation,
            pendingFocusRequest](
               PDFFormWorker::InteractionResult result) {
        if (!controller
            || controller->m_generation != generation
            || !sameDocumentKey(controller->documentKey(), key)) {
            return;
        }
        const bool hadDrafts = controller->hasDrafts();
        if (pendingMutation && controller->m_pendingMutationCount > 0) {
            --controller->m_pendingMutationCount;
        }
        if (pendingFocusRequest
            && controller->m_pendingFocusRequestCount > 0) {
            --controller->m_pendingFocusRequestCount;
        }
        controller->applyInteractionResult(
            result,
            copyResultToClipboard);
        if (controller->m_navigationEpoch == navigationEpoch) {
            controller->processHostActions(result, navigationEpoch);
        }
        if (hadDrafts != controller->hasDrafts()) {
            emit controller->historyStateChanged();
        }
    };
}

void PDFFormController::beginPendingMutation()
{
    const bool hadDrafts = hasDrafts();
    ++m_pendingMutationCount;
    if (hadDrafts != hasDrafts()) {
        emit historyStateChanged();
    }
}

void PDFFormController::queueJavaScriptState(bool enabled)
{
    ++m_pageChangeRequestSerial;
    const PDFFormWorker::DocumentKey key = documentKey();
    if (!key.isValid()) {
        if (m_javascriptEnabled) {
            m_javascriptEnabled = false;
            emit javascriptEnabledChanged();
        }
        return;
    }

    const quint64 generation = m_generation;
    const quint64 requestSerial = ++m_javascriptRequestSerial;
    const quint64 pageChangeSerial = m_pageChangeRequestSerial;
    if (!m_javascriptTransitionPending) {
        m_javascriptTransitionPending = true;
        emit javascriptTransitionPendingChanged();
    }

    const int page = m_document ? m_document->currentPage() : -1;
    const int sourcePage = m_document
        ? m_document->sourcePageIndex(page)
        : -1;
    QPointer<PDFFormController> controller(this);
    PDFFormWorker::setJavaScriptEnabled(
        key,
        enabled,
        page,
        sourcePage,
        [controller,
         key,
         generation,
         requestSerial,
         pageChangeSerial,
         enabled](PDFFormWorker::JavaScriptResult result) {
            if (!controller
                || controller->m_generation != generation
                || controller->m_javascriptRequestSerial != requestSerial
                || !sameDocumentKey(controller->documentKey(), key)) {
                return;
            }

            controller->m_javascriptTransitionPending = false;
            emit controller->javascriptTransitionPendingChanged();
            controller->applyInteractionResult(result.interaction);
            if (!result.succeeded || result.enabled != enabled) {
                if (controller->m_javascriptEnabled != result.enabled) {
                    controller->m_javascriptEnabled = result.enabled;
                    emit controller->javascriptEnabledChanged();
                }
                if (controller->m_pageChangeRequestSerial
                    == pageChangeSerial) {
                    controller->rollbackCurrentPage(result.activePage);
                }
                emit controller->javascriptEnableFailed();
            }
        });
}

void PDFFormController::submitCurrentPageChange()
{
    if (m_ignoreNextCurrentPageChange) {
        m_ignoreNextCurrentPageChange = false;
        return;
    }
    ++m_navigationEpoch;
    if (!m_javascriptEnabled
        || !m_document
        || !m_document->isLoaded()) {
        return;
    }

    const PDFFormWorker::DocumentKey key = documentKey();
    const quint64 generation = m_generation;
    const quint64 requestSerial = ++m_pageChangeRequestSerial;
    const int page = m_document->currentPage();
    const int sourcePage = m_document->sourcePageIndex(page);
    QPointer<PDFFormController> controller(this);
    PDFFormWorker::submitPageChange(
        key,
        page,
        sourcePage,
        [controller,
         key,
         generation,
         requestSerial](PDFFormWorker::PageChangeResult result) {
            if (!controller
                || controller->m_generation != generation
                || !sameDocumentKey(controller->documentKey(), key)) {
                return;
            }
            controller->applyInteractionResult(result.interaction);
            if (controller->m_pageChangeRequestSerial
                != requestSerial) {
                return;
            }
            if (!result.succeeded) {
                controller->rollbackCurrentPage(result.activePage);
            }
        });
}

void PDFFormController::rollbackCurrentPage(int page)
{
    if (!m_document
        || page < 0
        || page >= m_document->pageCount()) {
        return;
    }

    // PDFDocument changes first and the form transition follows on the worker.
    // Ask the view to cancel any pending callLater navigation and move its
    // viewport too. The direct document update remains as a non-QML fallback.
    m_ignoreNextCurrentPageChange = true;
    emit pageRollbackRequested(page);
    if (m_document && m_document->currentPage() != page) {
        m_document->setCurrentPage(page);
    }
    // setCurrentPage() emits synchronously on the GUI thread when it changes.
    // Clear the guard explicitly when the model was already on this page.
    m_ignoreNextCurrentPageChange = false;
}

void PDFFormController::invalidateFormPages(int page)
{
    ++m_formRevisionSerial;
    if (page < 0) {
        m_allPagesFormRevision = m_formRevisionSerial;
        m_pageFormRevisions.clear();
    } else {
        m_pageFormRevisions.insert(page, m_formRevisionSerial);
    }
    emit formRenderStateChanged(page, m_formRevisionSerial);
}

void PDFFormController::processHostActions(
    const PDFFormWorker::InteractionResult &result,
    quint64 navigationEpoch)
{
    if (m_navigationEpoch != navigationEpoch
        || !m_document
        || !m_document->isLoaded()) {
        return;
    }

    const bool removedWebLink = result.webLinkSourcePage >= 0
        && result.webLinkAnnotationIndex >= 0
        && m_removedWebLinks.value(result.webLinkSourcePage)
               .contains(result.webLinkAnnotationIndex);
    if (!result.externalUri.isEmpty()
        && !removedWebLink
        && m_pendingExternalLinkRequestId == 0) {
        const QUrl url = validatedExternalUrl(result.externalUri);
        const PDFFormWorker::DocumentKey key = documentKey();
        if (!url.isEmpty() && key.isValid()) {
            do {
                ++m_nextExternalLinkRequestId;
            } while (m_nextExternalLinkRequestId == 0);

            m_pendingExternalLinkRequestId
                = m_nextExternalLinkRequestId;
            m_pendingExternalLinkGeneration = m_generation;
            m_pendingExternalLinkKey = key;
            m_pendingExternalLinkUrl = url;
            const QString scheme = url.scheme().toLower();
            assertPDFiumProcessLockNotHeld();
            emit externalLinkRequested(
                m_pendingExternalLinkRequestId,
                url.authority(QUrl::FullyEncoded),
                QString::fromLatin1(
                    url.toEncoded(QUrl::FullyEncoded)),
                scheme == QStringLiteral("http"));
        }
    }

    if (!result.goTo || removedWebLink
        || m_navigationEpoch != navigationEpoch
        || !m_document
        || !m_document->isLoaded()) {
        return;
    }

    const int page = m_document->displayPageIndexForSourcePage(
        result.goTo->sourcePage);
    if (page < 0) {
        return;
    }

    assertPDFiumProcessLockNotHeld();
    emit pageNavigationRequested(page);
    if (m_document && m_document->currentPage() != page) {
        m_document->setCurrentPage(page);
    }
}

void PDFFormController::clearPendingExternalLink(bool notify)
{
    const quint64 requestId = m_pendingExternalLinkRequestId;
    m_pendingExternalLinkRequestId = 0;
    m_pendingExternalLinkGeneration = 0;
    m_pendingExternalLinkKey = {};
    m_pendingExternalLinkUrl = QUrl();
    if (notify && requestId != 0) {
        emit externalLinkRequestCancelled(requestId);
    }
}

void PDFFormController::applyInteractionResult(
    const PDFFormWorker::InteractionResult &result,
    bool copyResultToClipboard)
{
    const bool previousHasDrafts = m_hasDrafts;
    const bool previousCanUndo = m_canUndo;
    const bool previousCanRedo = m_canRedo;
    const bool previousHasFocus = m_hasFocus;
    const bool previousAcceptsTextInput = m_acceptsTextInput;
    const int previousFocusedPage = m_focusedPage;
    const int previousFocusedAnnotationIndex
        = m_focusedAnnotationIndex;
    const int previousCursorType = m_cursorType;

    if (result.changed) {
        if (result.page < 0) {
            m_allPagesChanged = true;
            m_changedPages.clear();
        } else if (!m_allPagesChanged) {
            m_changedPages.insert(result.page);
        }
    }
    m_hasDrafts = m_hasDrafts || result.changed;
    m_canUndo = result.canUndo;
    m_canRedo = result.canRedo;
    m_hasFocus = result.hasFocus;
    m_acceptsTextInput = result.hasFocus && result.acceptsTextInput;
    m_focusedPage = result.hasFocus ? result.focusedPage : -1;
    m_focusedAnnotationIndex
        = result.hasFocus ? result.focusedAnnotationIndex : -1;
    const bool removedWebLink = result.webLinkSourcePage >= 0
        && result.webLinkAnnotationIndex >= 0
        && m_removedWebLinks.value(result.webLinkSourcePage)
               .contains(result.webLinkAnnotationIndex);
    m_cursorType = removedWebLink && result.cursorType == FXCT_HAND
        ? FXCT_ARROW : result.cursorType;

    if (copyResultToClipboard && !result.selectedText.isEmpty()) {
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(result.selectedText);
        }
    }

    const bool historyChanged = previousHasDrafts != m_hasDrafts
        || previousCanUndo != m_canUndo
        || previousCanRedo != m_canRedo;
    const bool inputChanged = previousHasFocus != m_hasFocus
        || previousAcceptsTextInput != m_acceptsTextInput
        || previousFocusedPage != m_focusedPage
        || previousFocusedAnnotationIndex
            != m_focusedAnnotationIndex
        || previousCursorType != m_cursorType;
    if (result.invalidated
        || result.changed
        || previousHasFocus != m_hasFocus) {
        invalidateFormPages(result.changed ? -1 : result.page);
    }
    if (historyChanged) {
        emit historyStateChanged();
    }
    if (inputChanged || result.page >= 0) {
        emit inputStateChanged();
    }
}

QUrl PDFFormController::validatedExternalUrl(
    const QByteArray &encodedUrl)
{
    static constexpr qsizetype kMaximumUriBytes = 8192;
    if (encodedUrl.isEmpty()
        || encodedUrl.size() > kMaximumUriBytes) {
        return {};
    }

    for (const char character : encodedUrl) {
        const unsigned char byte
            = static_cast<unsigned char>(character);
        if (byte <= 0x20 || byte == 0x7f) {
            return {};
        }
    }

    const QUrl url = QUrl::fromEncoded(
        encodedUrl,
        QUrl::StrictMode);
    if (!url.isValid()
        || url.isEmpty()
        || url.isRelative()
        || url.host().isEmpty()) {
        return {};
    }

    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http")
        && scheme != QStringLiteral("https")) {
        return {};
    }
    if (url.authority(QUrl::FullyEncoded).contains(
            QLatin1Char('@'))) {
        return {};
    }
    if (url.toEncoded(QUrl::FullyEncoded).size()
        > kMaximumUriBytes) {
        return {};
    }
    return url;
}

bool PDFFormController::sameDocumentKey(
    const PDFFormWorker::DocumentKey &left,
    const PDFFormWorker::DocumentKey &right)
{
    return left.sessionId == right.sessionId
        && left.filePath == right.filePath
        && left.revision == right.revision
        && left.password == right.password;
}

QString PDFFormController::createSnapshotPath(QString &error) const
{
    if (!documentKey().isValid()) {
        error = QStringLiteral("No PDF is open");
        return {};
    }

    QTemporaryFile temporaryFile(
        QDir::tempPath() + QStringLiteral("/TrePDF-form-XXXXXX.pdf"));
    temporaryFile.setAutoRemove(false);
    if (!temporaryFile.open()) {
        error = temporaryFile.errorString();
        return {};
    }
    const QString filePath = temporaryFile.fileName();
    temporaryFile.close();
    return filePath;
}
