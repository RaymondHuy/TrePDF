#include "PDFFormWorker.h"

#include "PDFiumSynchronization.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>
#include <QRunnable>
#include <QSaveFile>
#include <QSemaphore>
#include <QSet>
#include <QThreadPool>
#include <QVector>

#include <cmath>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>

#include <fpdf_annot.h>
#include <fpdf_fwlevent.h>
#include <fpdf_save.h>

namespace {

QThreadPool &formRenderThreadPool()
{
    static QThreadPool pool;
    static const bool configured = []() {
        pool.setMaxThreadCount(1);
        // Form focus and field values live on the worker-owned PDFium handle.
        // Keeping the sole worker alive prevents thread-local form state from
        // disappearing between ordinary viewing and interaction requests.
        pool.setExpiryTimeout(-1);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

class FormRenderWorkQueue
{
public:
    void submitRender(const void *owner,
                      quint64 sessionId,
                      std::function<void()> work)
    {
        bool startWorker = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_destroyedSessions.contains(sessionId)) {
                return;
            }
            if (!m_pendingRenders.contains(owner)) {
                m_renderOrder.enqueue(owner);
            }
            m_pendingRenders.insert(
                owner,
                PendingRender{sessionId, std::move(work)});
            startWorker = activateWorkerLocked();
        }
        startWorkerIfNeeded(startWorker);
    }

    void cancelRender(const void *owner)
    {
        QMutexLocker locker(&m_mutex);
        m_pendingRenders.remove(owner);
        m_renderOrder.removeAll(owner);
    }

    bool submitCommand(quint64 sessionId,
                       std::function<void()> work,
                       bool coalescibleMove = false,
                       int page = -1)
    {
        bool startWorker = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_destroyedSessions.contains(sessionId)) {
                return false;
            }
            if (coalescibleMove
                && !m_commands.isEmpty()
                && m_commands.back().coalescibleMove
                && m_commands.back().sessionId == sessionId
                && m_commands.back().page == page) {
                m_commands.back().work = std::move(work);
            } else {
                m_commands.enqueue(PendingCommand{
                    sessionId,
                    coalescibleMove,
                    page,
                    std::move(work)});
            }
            startWorker = activateWorkerLocked();
        }
        startWorkerIfNeeded(startWorker);
        return true;
    }

    void resetSession(quint64 sessionId, std::function<void()> work)
    {
        bool startWorker = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_destroyedSessions.contains(sessionId)) {
                return;
            }
            purgeSessionLocked(sessionId);
            m_commands.enqueue(PendingCommand{
                sessionId,
                false,
                -1,
                std::move(work)});
            startWorker = activateWorkerLocked();
        }
        startWorkerIfNeeded(startWorker);
    }

    void destroySession(quint64 sessionId, std::function<void()> work)
    {
        bool startWorker = false;
        {
            QMutexLocker locker(&m_mutex);
            m_destroyedSessions.insert(sessionId);
            purgeSessionLocked(sessionId);
            // This cleanup command is intentionally accepted after the
            // tombstone. Future render/input submissions for the session are
            // rejected until cleanup. The controller is already being
            // destroyed, so no owner can submit that unique id afterward.
            m_commands.enqueue(PendingCommand{
                sessionId,
                false,
                -1,
                [this,
                 sessionId,
                 work = std::move(work)]() mutable {
                    work();
                    QMutexLocker locker(&m_mutex);
                    m_destroyedSessions.remove(sessionId);
                }});
            startWorker = activateWorkerLocked();
        }
        startWorkerIfNeeded(startWorker);
    }

private:
    struct PendingRender {
        quint64 sessionId = 0;
        std::function<void()> work;
    };

    struct PendingCommand {
        quint64 sessionId = 0;
        bool coalescibleMove = false;
        int page = -1;
        std::function<void()> work;
    };

    bool activateWorkerLocked()
    {
        if (m_workerActive) {
            return false;
        }
        m_workerActive = true;
        return true;
    }

    void startWorkerIfNeeded(bool startWorker)
    {
        if (!startWorker) {
            return;
        }
        formRenderThreadPool().start(QRunnable::create([this]() {
            drain();
        }));
    }

    void drain()
    {
        while (true) {
            std::function<void()> work;
            {
                QMutexLocker locker(&m_mutex);
                // Input is intentionally prioritized over queued page renders.
                // A render already in progress still completes normally and is
                // rejected by the GUI if its form revision became stale.
                if (!m_commands.isEmpty()) {
                    work = std::move(m_commands.dequeue().work);
                } else {
                    while (!m_renderOrder.isEmpty()) {
                        const void *owner = m_renderOrder.dequeue();
                        auto pending = m_pendingRenders.find(owner);
                        if (pending == m_pendingRenders.end()) {
                            continue;
                        }
                        work = std::move(pending->work);
                        m_pendingRenders.erase(pending);
                        break;
                    }
                }

                if (!work) {
                    m_workerActive = false;
                    return;
                }
            }
            work();
        }
    }

    void purgeSessionLocked(quint64 sessionId)
    {
        for (auto render = m_pendingRenders.begin();
             render != m_pendingRenders.end();) {
            if (render->sessionId == sessionId) {
                m_renderOrder.removeAll(render.key());
                render = m_pendingRenders.erase(render);
            } else {
                ++render;
            }
        }

        QQueue<PendingCommand> retainedCommands;
        while (!m_commands.isEmpty()) {
            PendingCommand command = std::move(m_commands.dequeue());
            if (command.sessionId != sessionId) {
                retainedCommands.enqueue(std::move(command));
            }
        }
        m_commands.swap(retainedCommands);
    }

    QMutex m_mutex;
    QHash<const void *, PendingRender> m_pendingRenders;
    QQueue<const void *> m_renderOrder;
    QQueue<PendingCommand> m_commands;
    QSet<quint64> m_destroyedSessions;
    bool m_workerActive = false;
};

FormRenderWorkQueue &workQueue()
{
    static FormRenderWorkQueue queue;
    return queue;
}

template<typename Result, typename Callback>
void postResult(Result result, const Callback &callback)
{
    if (!callback) {
        return;
    }
    QObject *app = QCoreApplication::instance();
    if (!app) {
        return;
    }
    QMetaObject::invokeMethod(
        app,
        [callback, result = std::move(result)]() mutable {
            callback(std::move(result));
        },
        Qt::QueuedConnection);
}

class SnapshotDeliveryGuard
{
public:
    explicit SnapshotDeliveryGuard(QString filePath)
        : m_filePath(std::move(filePath))
    {
    }

    ~SnapshotDeliveryGuard()
    {
        if (!m_released && !m_filePath.isEmpty()) {
            QFile::remove(m_filePath);
        }
    }

    void release()
    {
        m_released = true;
    }

private:
    QString m_filePath;
    bool m_released = false;
};

class PDFiumFormDocument;
void recordJavaScriptGoToAction(IPDF_JSPLATFORM *platform, int sourcePage);

struct FormFillInfo {
    FPDF_FORMFILLINFO api = {};
    PDFiumFormDocument *owner = nullptr;
};

static_assert(offsetof(FormFillInfo, api) == 0);

struct JavaScriptPlatform {
    IPDF_JSPLATFORM api = {};

    JavaScriptPlatform()
    {
        api.version = 2;
        api.app_alert = [](IPDF_JSPLATFORM *,
                           FPDF_WIDESTRING,
                           FPDF_WIDESTRING,
                           int,
                           int) {
            // Scripts may notify their own control flow, but never open a
            // host dialog while PDFium owns the worker and process lock.
            return JSPLATFORM_ALERT_RETURN_OK;
        };
        api.app_beep = [](IPDF_JSPLATFORM *, int) {
            // PDF scripts cannot make the host emit sound.
        };
        api.app_response = [](IPDF_JSPLATFORM *,
                              FPDF_WIDESTRING,
                              FPDF_WIDESTRING,
                              FPDF_WIDESTRING,
                              FPDF_WIDESTRING,
                              FPDF_BOOL,
                              void *response,
                              int length) {
            if (response && length >= 2) {
                auto *bytes = static_cast<unsigned char *>(response);
                bytes[0] = 0;
                bytes[1] = 0;
            }
            return 0;
        };
        api.Doc_getFilePath = [](IPDF_JSPLATFORM *,
                                 void *filePath,
                                 int length) {
            if (filePath && length > 0) {
                static_cast<char *>(filePath)[0] = '\0';
            }
            return 1;
        };
        api.Doc_mail = [](IPDF_JSPLATFORM *,
                          void *,
                          int,
                          FPDF_BOOL,
                          FPDF_WIDESTRING,
                          FPDF_WIDESTRING,
                          FPDF_WIDESTRING,
                          FPDF_WIDESTRING,
                          FPDF_WIDESTRING) {
            // Sending mail or exporting the document is blocked.
        };
        api.Doc_print = [](IPDF_JSPLATFORM *,
                           FPDF_BOOL,
                           int,
                           int,
                           FPDF_BOOL,
                           FPDF_BOOL,
                           FPDF_BOOL,
                           FPDF_BOOL,
                           FPDF_BOOL) {
            // Printing initiated by a PDF script is blocked.
        };
        api.Doc_submitForm = [](IPDF_JSPLATFORM *,
                                void *,
                                int,
                                FPDF_WIDESTRING) {
            // Form submission and all network transfer are blocked.
        };
        api.Doc_gotoPage = [](IPDF_JSPLATFORM *platform, int sourcePage) {
            recordJavaScriptGoToAction(platform, sourcePage);
        };
        api.Field_browse = [](IPDF_JSPLATFORM *,
                              void *filePath,
                              int length) {
            if (filePath && length > 0) {
                static_cast<char *>(filePath)[0] = '\0';
            }
            return 1;
        };
        api.m_isolate = nullptr;
        api.m_v8EmbedderSlot = 0;
    }
};

PDFiumFormDocument *formDocument(FPDF_FORMFILLINFO *info)
{
    auto *wrapper = reinterpret_cast<FormFillInfo *>(info);
    return wrapper ? wrapper->owner : nullptr;
}

class PDFiumFormDocument
{
    Q_DISABLE_COPY_MOVE(PDFiumFormDocument)

public:
    PDFiumFormDocument()
    {
        m_formFillInfo.owner = this;
        FPDF_FORMFILLINFO &info = m_formFillInfo.api;
        m_javaScriptPlatform.api.m_pFormfillinfo = &info;
        info.version = 2;
        info.xfa_disabled = true;
        info.FFI_Invalidate = [](FPDF_FORMFILLINFO *self,
                                 FPDF_PAGE,
                                 double,
                                 double,
                                 double,
                                 double) {
            if (PDFiumFormDocument *document = formDocument(self)) {
                document->m_eventInvalidated = true;
            }
        };
        info.FFI_OutputSelectedRect = [](FPDF_FORMFILLINFO *self,
                                         FPDF_PAGE,
                                         double,
                                         double,
                                         double,
                                         double) {
            if (PDFiumFormDocument *document = formDocument(self)) {
                document->m_eventInvalidated = true;
            }
        };
        info.FFI_SetCursor = [](FPDF_FORMFILLINFO *self, int cursorType) {
            if (PDFiumFormDocument *document = formDocument(self)) {
                document->m_cursorType = cursorType;
            }
        };
        info.FFI_SetTimer = [](FPDF_FORMFILLINFO *,
                               int,
                               TimerCallback) {
            // The render worker has no event loop. Returning zero tells PDFium
            // that caret/JavaScript timers are unavailable; editing itself
            // remains fully functional and every input event triggers repaint.
            return 0;
        };
        info.FFI_KillTimer = [](FPDF_FORMFILLINFO *, int) {};
        info.FFI_GetLocalTime = [](FPDF_FORMFILLINFO *) {
            const QDateTime now = QDateTime::currentDateTime();
            const QDate date = now.date();
            const QTime time = now.time();
            FPDF_SYSTEMTIME result = {};
            result.wYear = static_cast<unsigned short>(date.year());
            result.wMonth = static_cast<unsigned short>(date.month());
            result.wDayOfWeek = static_cast<unsigned short>(date.dayOfWeek() % 7);
            result.wDay = static_cast<unsigned short>(date.day());
            result.wHour = static_cast<unsigned short>(time.hour());
            result.wMinute = static_cast<unsigned short>(time.minute());
            result.wSecond = static_cast<unsigned short>(time.second());
            result.wMilliseconds = static_cast<unsigned short>(time.msec());
            return result;
        };
        info.FFI_OnChange = [](FPDF_FORMFILLINFO *self) {
            if (PDFiumFormDocument *document = formDocument(self)) {
                document->m_eventChanged = true;
                document->m_eventInvalidated = true;
            }
        };
        info.FFI_GetPage = [](FPDF_FORMFILLINFO *self,
                              FPDF_DOCUMENT document,
                              int pageIndex) -> FPDF_PAGE {
            PDFiumFormDocument *form = formDocument(self);
            return form && form->m_document == document
                ? form->pageForJavaScriptLocked(pageIndex)
                : nullptr;
        };
        info.FFI_GetCurrentPage = [](FPDF_FORMFILLINFO *self,
                                     FPDF_DOCUMENT document) -> FPDF_PAGE {
            PDFiumFormDocument *form = formDocument(self);
            if (!form || form->m_document != document) {
                return nullptr;
            }
            return form->m_activePage
                ? form->m_activePage
                : form->pageForJavaScriptLocked(
                      form->m_currentSourcePageHint);
        };
        info.FFI_GetRotation = [](FPDF_FORMFILLINFO *, FPDF_PAGE) {
            return 0;
        };
        info.FFI_ExecuteNamedAction = [](FPDF_FORMFILLINFO *,
                                         FPDF_BYTESTRING) {
            // Navigation and application actions are deliberately not handed
            // to the host application.
        };
        info.FFI_SetTextFieldFocus = [](FPDF_FORMFILLINFO *self,
                                        FPDF_WIDESTRING,
                                        FPDF_DWORD,
                                        FPDF_BOOL focused) {
            if (PDFiumFormDocument *document = formDocument(self)) {
                document->m_textInputFocused = focused;
                document->m_eventInvalidated = true;
            }
        };
        info.FFI_DoURIAction = [](FPDF_FORMFILLINFO *self,
                                  FPDF_BYTESTRING uri) {
            if (PDFiumFormDocument *document = formDocument(self)) {
                document->recordExternalUriActionLocked(uri);
            }
        };
        info.FFI_DoGoToAction = [](FPDF_FORMFILLINFO *self,
                                   int sourcePage,
                                   int zoomMode,
                                   float *positions,
                                   int positionCount) {
            if (PDFiumFormDocument *document = formDocument(self)) {
                document->recordGoToActionLocked(
                    sourcePage,
                    zoomMode,
                    positions,
                    positionCount);
            }
        };
        info.FFI_DoURIActionWithKeyboardModifier
            = [](FPDF_FORMFILLINFO *self,
                 FPDF_BYTESTRING uri,
                 int) {
                if (PDFiumFormDocument *document = formDocument(self)) {
                    document->recordExternalUriActionLocked(uri);
                }
            };
        info.m_pJsPlatform = nullptr;
    }

    ~PDFiumFormDocument()
    {
        if (pdfiumProcessLockOwnedByCurrentThread()) {
            closeLocked();
        } else {
            PDFiumProcessLock pdfiumLock;
            closeLocked();
        }
    }

    void recordGoToActionLocked(int sourcePage,
                                int zoomMode,
                                const float *positions,
                                int positionCount)
    {
        assertPDFiumProcessLockHeld();
        if (!m_eventActive
            || !m_hostActionsAllowed
            || !m_document
            || sourcePage < 0
            || sourcePage >= FPDF_GetPageCount(m_document)) {
            return;
        }

        PDFFormWorker::GoToRequest request;
        request.sourcePage = sourcePage;
        request.zoomMode = zoomMode;
        const int boundedPositionCount = positions && positionCount > 0
            ? qMin<int>(
                  positionCount,
                  static_cast<int>(request.positions.size()))
            : 0;
        for (int index = 0; index < boundedPositionCount; ++index) {
            if (!std::isfinite(positions[index])) {
                request.positionCount = 0;
                m_eventGoTo = request;
                return;
            }
            request.positions[static_cast<size_t>(index)] = positions[index];
        }
        request.positionCount = boundedPositionCount;
        m_eventGoTo = request;
    }

    void recordExternalUriActionLocked(FPDF_BYTESTRING uri)
    {
        assertPDFiumProcessLockHeld();
        static constexpr qsizetype kMaximumUriBytes = 8192;
        if (!m_eventActive
            || !m_hostActionsAllowed
            || !m_eventExternalUri.isEmpty()
            || !uri) {
            return;
        }

        qsizetype length = 0;
        while (length <= kMaximumUriBytes && uri[length] != '\0') {
            ++length;
        }
        if (length <= kMaximumUriBytes) {
            m_eventExternalUri = QByteArray(uri, length);
        }
    }

    FPDF_DOCUMENT documentLocked(const PDFFormWorker::DocumentKey &key)
    {
        assertPDFiumProcessLockHeld();
        if (m_document
            && m_formHandle
            && m_filePath == key.filePath
            && m_revision == key.revision) {
            return m_document;
        }

        closeLocked();
        if (!key.isValid()) {
            return nullptr;
        }

        const QByteArray encodedPath = QFile::encodeName(key.filePath);
        m_document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
        if (!m_document) {
            return nullptr;
        }
        m_formHandle = FPDFDOC_InitFormFillEnvironment(
            m_document,
            &m_formFillInfo.api);
        if (!m_formHandle) {
            FPDF_CloseDocument(m_document);
            m_document = nullptr;
            return nullptr;
        }
        m_filePath = key.filePath;
        m_revision = key.revision;
        m_javascriptEnabled = false;
        return m_document;
    }

    PDFFormWorker::JavaScriptResult setJavaScriptEnabledLocked(
        const PDFFormWorker::DocumentKey &key,
        bool enabled,
        int page,
        int sourcePage)
    {
        assertPDFiumProcessLockHeld();
        PDFFormWorker::JavaScriptResult result;
        result.interaction.page = page;
        if (!documentLocked(key)) {
            result.activePage = m_activePageIndex;
            result.error = QStringLiteral(
                "Could not open the JavaScript form environment");
            return result;
        }

        if (m_javascriptEnabled == enabled) {
            beginEvent(page);
            finishEvent(result.interaction);
            result.succeeded = true;
            result.enabled = enabled;
            result.activePage = m_activePageIndex;
            return result;
        }

        const bool hasRequestedPage = page >= 0 || sourcePage >= 0;
        const int restoreSourcePage = hasRequestedPage
            ? sourcePage
            : m_activeSourcePage;
        const int restorePage = page >= 0 ? page : m_activePageIndex;
        beginEvent(restorePage);
        if (!closeActivePageLocked(true)) {
            finishEvent(result.interaction);
            result.enabled = m_javascriptEnabled;
            result.activePage = m_activePageIndex;
            result.error = QStringLiteral(
                "The active form field could not commit its value");
            return result;
        }
        closeJavaScriptPagesLocked();
        if (m_formHandle) {
            FPDFDOC_ExitFormFillEnvironment(m_formHandle);
            m_formHandle = nullptr;
        }

        if (!createFormEnvironmentLocked(enabled)) {
            // A failed enable must never leave the document in a partially
            // enabled state. Restore the ordinary no-JavaScript form
            // environment when possible.
            const bool restoredSafeEnvironment
                = createFormEnvironmentLocked(false);
            if (!restoredSafeEnvironment) {
                closeLocked();
            }
            if (m_formHandle && restoreSourcePage >= 0) {
                m_currentSourcePageHint = restoreSourcePage;
                ensureActivePageLocked(restoreSourcePage, restorePage);
            }
            finishEvent(result.interaction);
            result.interaction.invalidated = true;
            result.enabled = false;
            result.activePage = m_activePageIndex;
            result.error = QStringLiteral(
                "PDFium could not change the JavaScript form environment");
            return result;
        }

        m_currentSourcePageHint = restoreSourcePage;
        if (enabled) {
            // These calls initialize document helpers and then execute the
            // catalog open action. The platform below exposes no URL, file,
            // mail, print, submit, browse, or host-navigation capability.
            FORM_DoDocumentJSAction(m_formHandle);
            FORM_DoDocumentOpenAction(m_formHandle);
        }
        if (restoreSourcePage >= 0) {
            ensureActivePageLocked(restoreSourcePage, restorePage);
        }

        finishEvent(result.interaction);
        result.interaction.invalidated = true;
        result.succeeded = true;
        result.enabled = enabled;
        result.activePage = m_activePageIndex;
        return result;
    }

    PDFFormWorker::PageChangeResult pageChangeLocked(
        int page,
        int sourcePage)
    {
        assertPDFiumProcessLockHeld();
        PDFFormWorker::PageChangeResult result;
        const int previousSourcePage = m_activeSourcePage;
        const int previousPage = m_activePageIndex;
        const int previousSourcePageHint = m_currentSourcePageHint;
        beginEvent(page);
        bool transitionSucceeded = false;
        if (sourcePage >= 0) {
            transitionSucceeded = ensureActivePageLocked(sourcePage, page);
        } else {
            transitionSucceeded = closeActivePageLocked(true);
        }
        const bool pageChanged = transitionSucceeded
            && previousSourcePage != sourcePage;
        if (transitionSucceeded) {
            m_currentSourcePageHint = sourcePage;
        } else {
            m_currentSourcePageHint = previousSourcePageHint;
            if (m_activePage) {
                m_activePageIndex = previousPage;
            }
            m_currentEventPage = previousPage;
        }
        finishEvent(result.interaction);
        result.succeeded = transitionSucceeded;
        result.activePage = m_activePageIndex;
        if (pageChanged) {
            // Focus and page-open/page-close appearances can affect both the
            // previous and current pages.
            result.interaction.invalidated = true;
            result.interaction.page = -1;
        }
        return result;
    }

    bool withPageLocked(const PDFFormWorker::DocumentKey &key,
                        int sourcePage,
                        const PDFFormWorker::PageCallback &callback)
    {
        assertPDFiumProcessLockHeld();
        if (!documentLocked(key)
            || !m_formHandle
            || sourcePage < 0
            || !callback) {
            return false;
        }

        if (m_activePage && m_activeSourcePage == sourcePage) {
            return withPreviewPageLocked(
                sourcePage,
                m_activePage,
                callback);
        }

        const auto scriptPage = m_javaScriptPages.constFind(sourcePage);
        if (scriptPage != m_javaScriptPages.cend()) {
            return withPreviewPageLocked(
                sourcePage,
                scriptPage.value(),
                callback);
        }

        FPDF_PAGE formPage = FPDF_LoadPage(m_document, sourcePage);
        if (!formPage) {
            return false;
        }
        FORM_OnAfterLoadPage(formPage, m_formHandle);
        const bool previewReusable = withPreviewPageLocked(
            sourcePage,
            formPage,
            callback);
        FORM_OnBeforeClosePage(formPage, m_formHandle);
        FPDF_ClosePage(formPage);
        return previewReusable;
    }

    PDFFormWorker::InteractionResult pointerEventLocked(
        int page,
        int sourcePage,
        PDFFormWorker::PointerEventType type,
        int modifiers,
        const QPointF &position,
        const QSize &deviceSize)
    {
        assertPDFiumProcessLockHeld();
        PDFFormWorker::InteractionResult result;
        result.page = page;
        if (deviceSize.isEmpty()) {
            return result;
        }

        beginEvent(page);
        FPDF_PAGE eventPage = m_activePage;
        bool temporaryEventPage = false;
        if (type == PDFFormWorker::PointerEventType::Press) {
            if (!ensureActivePageLocked(sourcePage, page)) {
                finishEvent(result);
                return result;
            }
            eventPage = m_activePage;
        } else if (type == PDFFormWorker::PointerEventType::Move
                   && (!m_activePage
                       || m_activeSourcePage != sourcePage)) {
            eventPage = FPDF_LoadPage(m_document, sourcePage);
            if (!eventPage) {
                finishEvent(result);
                return result;
            }
            FORM_OnAfterLoadPage(eventPage, m_formHandle);
            temporaryEventPage = true;
        } else if (!m_activePage || m_activeSourcePage != sourcePage) {
            finishEvent(result);
            return result;
        }

        double pageX = 0.0;
        double pageY = 0.0;
        if (!FPDF_DeviceToPage(
                eventPage,
                0,
                0,
                deviceSize.width(),
                deviceSize.height(),
                0,
                qRound(position.x()),
                qRound(position.y()),
                &pageX,
                &pageY)) {
            if (temporaryEventPage) {
                FORM_OnBeforeClosePage(eventPage, m_formHandle);
                FPDF_ClosePage(eventPage);
            }
            finishEvent(result);
            return result;
        }

        m_hostActionsAllowed
            = type != PDFFormWorker::PointerEventType::Move;
        switch (type) {
        case PDFFormWorker::PointerEventType::Move:
            result.handled = FORM_OnMouseMove(
                m_formHandle,
                eventPage,
                modifiers,
                pageX,
                pageY);
            break;
        case PDFFormWorker::PointerEventType::Press: {
            const bool focused = FORM_OnFocus(
                m_formHandle,
                m_activePage,
                modifiers,
                pageX,
                pageY);
            const bool pressed = FORM_OnLButtonDown(
                m_formHandle,
                m_activePage,
                modifiers,
                pageX,
                pageY);
            result.handled = focused || pressed;
            break;
        }
        case PDFFormWorker::PointerEventType::Release:
            result.handled = FORM_OnLButtonUp(
                m_formHandle,
                m_activePage,
                modifiers,
                pageX,
                pageY);
            break;
        case PDFFormWorker::PointerEventType::DoubleClick:
            result.handled = FORM_OnLButtonDoubleClick(
                m_formHandle,
                m_activePage,
                modifiers,
                pageX,
                pageY);
            break;
        }
        finishEvent(result);
        if (temporaryEventPage) {
            FORM_OnBeforeClosePage(eventPage, m_formHandle);
            FPDF_ClosePage(eventPage);
        }
        return result;
    }

    PDFFormWorker::InteractionResult keyEventLocked(
        int virtualKey,
        int modifiers,
        const QString &text)
    {
        assertPDFiumProcessLockHeld();
        PDFFormWorker::InteractionResult result;
        result.page = m_activePageIndex;
        if (!m_formHandle || !m_activePage) {
            return result;
        }

        const bool mayChangeText = !text.isEmpty()
            || virtualKey == FWL_VKEY_Back
            || virtualKey == FWL_VKEY_Delete
            || virtualKey == FWL_VKEY_Return
            || virtualKey == FWL_VKEY_NewLine;
        const QString textBefore = mayChangeText
            ? focusedTextLocked()
            : QString();
        const bool activatesButton
            = !m_textInputFocused
            && (virtualKey == FWL_VKEY_Return
                || virtualKey == FWL_VKEY_Space);
        beginEvent(m_activePageIndex, activatesButton);
        if (virtualKey != FWL_VKEY_Unknown) {
            result.handled = FORM_OnKeyDown(
                m_formHandle,
                m_activePage,
                virtualKey,
                modifiers);
        }
        if (!text.isEmpty()) {
            const QVector<uint> codePoints = text.toUcs4();
            for (uint codePoint : codePoints) {
                result.handled = FORM_OnChar(
                                     m_formHandle,
                                     m_activePage,
                                     static_cast<int>(codePoint),
                                     modifiers)
                    || result.handled;
            }
        }
        if (virtualKey != FWL_VKEY_Unknown) {
            FORM_OnKeyUp(
                m_formHandle,
                m_activePage,
                virtualKey,
                modifiers);
        }
        finishEvent(result);
        if (mayChangeText && focusedTextLocked() != textBefore) {
            result.changed = true;
            result.invalidated = true;
        }
        return result;
    }

    PDFFormWorker::InteractionResult editCommandLocked(
        PDFFormWorker::EditCommand command,
        const QString &text)
    {
        assertPDFiumProcessLockHeld();
        PDFFormWorker::InteractionResult result;
        result.page = m_activePageIndex;
        if (!m_formHandle) {
            return result;
        }

        const bool mayChangeText
            = command == PDFFormWorker::EditCommand::Cut
            || command == PDFFormWorker::EditCommand::Paste
            || command == PDFFormWorker::EditCommand::Undo
            || command == PDFFormWorker::EditCommand::Redo;
        const QString textBefore = mayChangeText
            ? focusedTextLocked()
            : QString();
        beginEvent(m_activePageIndex);
        switch (command) {
        case PDFFormWorker::EditCommand::SelectAll:
            if (m_activePage) {
                FORM_SelectAllText(m_formHandle, m_activePage);
                result.handled = true;
                m_eventInvalidated = true;
            }
            break;
        case PDFFormWorker::EditCommand::Copy:
            if (m_activePage) {
                result.selectedText = selectedTextLocked();
                result.handled = !result.selectedText.isEmpty();
            }
            break;
        case PDFFormWorker::EditCommand::Cut:
            if (m_activePage) {
                result.selectedText = selectedTextLocked();
                if (!result.selectedText.isEmpty()) {
                    const QVector<FPDF_WCHAR> replacement = wideString({});
                    FORM_ReplaceSelection(
                        m_formHandle,
                        m_activePage,
                        replacement.constData());
                    result.handled = true;
                }
            }
            break;
        case PDFFormWorker::EditCommand::Paste:
            if (m_activePage) {
                const QVector<FPDF_WCHAR> replacement = wideString(text);
                FORM_ReplaceSelection(
                    m_formHandle,
                    m_activePage,
                    replacement.constData());
                result.handled = true;
            }
            break;
        case PDFFormWorker::EditCommand::Undo:
            if (m_activePage) {
                result.handled = FORM_Undo(m_formHandle, m_activePage);
            }
            break;
        case PDFFormWorker::EditCommand::Redo:
            if (m_activePage) {
                result.handled = FORM_Redo(m_formHandle, m_activePage);
            }
            break;
        case PDFFormWorker::EditCommand::KillFocus:
            result.handled = FORM_ForceToKillFocus(m_formHandle);
            break;
        }
        finishEvent(result);
        if (mayChangeText && focusedTextLocked() != textBefore) {
            result.changed = true;
            result.invalidated = true;
        }
        return result;
    }

    PDFFormWorker::SnapshotResult exportSnapshotLocked(
        const PDFFormWorker::DocumentKey &key,
        const QString &filePath)
    {
        assertPDFiumProcessLockHeld();
        PDFFormWorker::SnapshotResult result;
        if (!documentLocked(key)) {
            result.error = QStringLiteral("Could not open the form document");
            return result;
        }

        bool hadFocusedAnnotation = false;
        if (m_formHandle) {
            int focusedPage = -1;
            FPDF_ANNOTATION focusedAnnotation = nullptr;
            hadFocusedAnnotation = FORM_GetFocusedAnnot(
                                       m_formHandle,
                                       &focusedPage,
                                       &focusedAnnotation)
                && focusedAnnotation != nullptr;
            if (focusedAnnotation) {
                FPDFPage_CloseAnnot(focusedAnnotation);
            }
        }

        beginEvent(m_activePageIndex);
        if (hadFocusedAnnotation
            && !FORM_ForceToKillFocus(m_formHandle)) {
            finishEvent(result.interaction);
            result.error = QStringLiteral(
                "The active form field could not commit its value");
            return result;
        }
        if (m_javascriptEnabled) {
            FORM_DoDocumentAAction(
                m_formHandle,
                FPDFDOC_AACTION_WS);
        }
        finishEvent(result.interaction);

        class SaveFileWriter final : public FPDF_FILEWRITE
        {
        public:
            explicit SaveFileWriter(const QString &path)
                : m_file(path)
            {
                version = 1;
                WriteBlock = &SaveFileWriter::writeBlock;
            }

            bool open()
            {
                return m_file.open(QIODevice::WriteOnly);
            }

            bool commit()
            {
                return m_file.commit();
            }

            QString errorString() const
            {
                return m_file.errorString();
            }

        private:
            static int writeBlock(FPDF_FILEWRITE *writer,
                                  const void *data,
                                  unsigned long size)
            {
                auto *self = static_cast<SaveFileWriter *>(writer);
                return self->m_file.write(
                           static_cast<const char *>(data),
                           static_cast<qint64>(size))
                        == static_cast<qint64>(size)
                    ? 1
                    : 0;
            }

            QSaveFile m_file;
        };

        SaveFileWriter writer(filePath);
        if (!writer.open()) {
            result.error = writer.errorString();
            return result;
        }
        if (!FPDF_SaveAsCopy(m_document, &writer, FPDF_NO_INCREMENTAL)) {
            result.error = writer.errorString();
            if (result.error.isEmpty()) {
                result.error = QStringLiteral("PDFium could not serialize the form");
            }
            return result;
        }
        if (!writer.commit()) {
            result.error = writer.errorString();
            return result;
        }
        result.succeeded = true;
        return result;
    }

    void closeLocked()
    {
        assertPDFiumProcessLockHeld();
        closeActivePageLocked();
        closeJavaScriptPagesLocked();
        closePreviewDocumentLocked();
        if (m_formHandle) {
            FPDFDOC_ExitFormFillEnvironment(m_formHandle);
        }
        if (m_document) {
            FPDF_CloseDocument(m_document);
        }
        m_formHandle = nullptr;
        m_document = nullptr;
        m_filePath.clear();
        m_revision = -1;
        m_cursorType = FXCT_ARROW;
        m_textInputFocused = false;
        m_currentSourcePageHint = -1;
        m_eventActive = false;
        m_hostActionsAllowed = false;
        m_eventGoTo.reset();
        m_eventExternalUri.clear();
        m_javascriptEnabled = false;
        m_formFillInfo.api.m_pJsPlatform = nullptr;
    }

private:
    bool createFormEnvironmentLocked(bool javascriptEnabled)
    {
        assertPDFiumProcessLockHeld();
        if (!m_document || m_formHandle) {
            return false;
        }

        m_formFillInfo.api.m_pJsPlatform = javascriptEnabled
            ? &m_javaScriptPlatform.api
            : nullptr;
        m_formHandle = FPDFDOC_InitFormFillEnvironment(
            m_document,
            &m_formFillInfo.api);
        if (!m_formHandle) {
            m_formFillInfo.api.m_pJsPlatform = nullptr;
            m_javascriptEnabled = false;
            return false;
        }
        m_javascriptEnabled = javascriptEnabled;
        return true;
    }

    FPDF_PAGE pageForJavaScriptLocked(int sourcePage)
    {
        assertPDFiumProcessLockHeld();
        if (!m_document
            || !m_formHandle
            || sourcePage < 0
            || sourcePage >= FPDF_GetPageCount(m_document)) {
            return nullptr;
        }
        if (m_activePage && m_activeSourcePage == sourcePage) {
            return m_activePage;
        }

        const auto existing = m_javaScriptPages.constFind(sourcePage);
        if (existing != m_javaScriptPages.cend()) {
            return existing.value();
        }

        FPDF_PAGE loadedPage = FPDF_LoadPage(m_document, sourcePage);
        if (!loadedPage) {
            return nullptr;
        }
        // Cache before notifying PDFium so a re-entrant FFI_GetPage() for the
        // same index cannot recursively load the page forever.
        m_javaScriptPages.insert(sourcePage, loadedPage);
        FORM_OnAfterLoadPage(loadedPage, m_formHandle);
        return loadedPage;
    }

    void closeJavaScriptPagesLocked()
    {
        assertPDFiumProcessLockHeld();
        while (!m_javaScriptPages.isEmpty()) {
            auto pageEntry = m_javaScriptPages.begin();
            FPDF_PAGE page = pageEntry.value();
            m_javaScriptPages.erase(pageEntry);
            if (m_formHandle) {
                FORM_OnBeforeClosePage(page, m_formHandle);
            }
            FPDF_ClosePage(page);
        }
    }

    bool withPreviewPageLocked(
        int sourcePage,
        FPDF_PAGE formPage,
        const PDFFormWorker::PageCallback &callback)
    {
        assertPDFiumProcessLockHeld();
        if (!m_previewDocument) {
            const QByteArray encodedPath = QFile::encodeName(m_filePath);
            m_previewDocument = FPDF_LoadDocument(
                encodedPath.constData(),
                nullptr);
        }
        if (!m_previewDocument) {
            return false;
        }

        FPDF_PAGE previewPage = FPDF_LoadPage(
            m_previewDocument,
            sourcePage);
        if (!previewPage) {
            return false;
        }
        const bool previewReusable = callback(
            m_document,
            m_formHandle,
            previewPage,
            formPage);
        FPDF_ClosePage(previewPage);
        if (!previewReusable) {
            closePreviewDocumentLocked();
        }
        return previewReusable;
    }

    void closePreviewDocumentLocked()
    {
        assertPDFiumProcessLockHeld();
        if (m_previewDocument) {
            FPDF_CloseDocument(m_previewDocument);
            m_previewDocument = nullptr;
        }
    }

    static QVector<FPDF_WCHAR> wideString(const QString &text)
    {
        QVector<FPDF_WCHAR> result;
        result.reserve(text.size() + 1);
        for (QChar character : text) {
            result.append(static_cast<FPDF_WCHAR>(character.unicode()));
        }
        result.append(0);
        return result;
    }

    QString selectedTextLocked() const
    {
        assertPDFiumProcessLockHeld();
        const unsigned long byteCount = FORM_GetSelectedText(
            m_formHandle,
            m_activePage,
            nullptr,
            0);
        if (byteCount <= sizeof(FPDF_WCHAR)) {
            return {};
        }
        QVector<FPDF_WCHAR> buffer(
            static_cast<qsizetype>(
                (byteCount + sizeof(FPDF_WCHAR) - 1)
                / sizeof(FPDF_WCHAR)));
        if (FORM_GetSelectedText(
                m_formHandle,
                m_activePage,
                buffer.data(),
                byteCount)
            != byteCount) {
            return {};
        }
        const qsizetype length = qMax<qsizetype>(
            0,
            static_cast<qsizetype>(byteCount / sizeof(FPDF_WCHAR)) - 1);
        return QString::fromUtf16(
            reinterpret_cast<const char16_t *>(buffer.constData()),
            length);
    }

    QString focusedTextLocked() const
    {
        assertPDFiumProcessLockHeld();
        if (!m_formHandle || !m_activePage) {
            return {};
        }
        const unsigned long byteCount = FORM_GetFocusedText(
            m_formHandle,
            m_activePage,
            nullptr,
            0);
        if (byteCount <= sizeof(FPDF_WCHAR)) {
            return {};
        }
        QVector<FPDF_WCHAR> buffer(
            static_cast<qsizetype>(
                (byteCount + sizeof(FPDF_WCHAR) - 1)
                / sizeof(FPDF_WCHAR)));
        if (FORM_GetFocusedText(
                m_formHandle,
                m_activePage,
                buffer.data(),
                byteCount)
            != byteCount) {
            return {};
        }
        const qsizetype length = qMax<qsizetype>(
            0,
            static_cast<qsizetype>(byteCount / sizeof(FPDF_WCHAR)) - 1);
        return QString::fromUtf16(
            reinterpret_cast<const char16_t *>(buffer.constData()),
            length);
    }

    bool ensureActivePageLocked(int sourcePage, int page)
    {
        assertPDFiumProcessLockHeld();
        if (!m_document || !m_formHandle || sourcePage < 0) {
            return false;
        }
        if (m_activePage && m_activeSourcePage == sourcePage) {
            m_activePageIndex = page;
            m_currentSourcePageHint = sourcePage;
            return true;
        }

        if (!closeActivePageLocked(true)) {
            return false;
        }
        m_activePage = m_javaScriptPages.take(sourcePage);
        const bool wasLoadedForJavaScript = m_activePage != nullptr;
        if (!m_activePage) {
            m_activePage = FPDF_LoadPage(m_document, sourcePage);
        }
        if (!m_activePage) {
            return false;
        }
        m_activeSourcePage = sourcePage;
        m_activePageIndex = page;
        m_currentSourcePageHint = sourcePage;
        if (!wasLoadedForJavaScript) {
            FORM_OnAfterLoadPage(m_activePage, m_formHandle);
        }
        if (m_javascriptEnabled) {
            FORM_DoPageAAction(
                m_activePage,
                m_formHandle,
                FPDFPAGE_AACTION_OPEN);
            m_activePageJavaScriptOpened = true;
        }
        return true;
    }

    bool closeActivePageLocked(bool requireFocusCommit = false)
    {
        assertPDFiumProcessLockHeld();
        if (!m_activePage) {
            return true;
        }
        if (m_formHandle) {
            bool focusCommitted = true;
            if (requireFocusCommit) {
                int focusedPage = -1;
                FPDF_ANNOTATION focusedAnnotation = nullptr;
                const bool hadFocusedAnnotation = FORM_GetFocusedAnnot(
                                                      m_formHandle,
                                                      &focusedPage,
                                                      &focusedAnnotation)
                    && focusedAnnotation != nullptr;
                if (focusedAnnotation) {
                    FPDFPage_CloseAnnot(focusedAnnotation);
                }
                focusCommitted = !hadFocusedAnnotation
                    || FORM_ForceToKillFocus(m_formHandle);
            } else {
                FORM_ForceToKillFocus(m_formHandle);
            }
            if (!focusCommitted) {
                return false;
            }
            if (m_javascriptEnabled
                && m_activePageJavaScriptOpened) {
                FORM_DoPageAAction(
                    m_activePage,
                    m_formHandle,
                    FPDFPAGE_AACTION_CLOSE);
            }
            FORM_OnBeforeClosePage(m_activePage, m_formHandle);
        }
        FPDF_ClosePage(m_activePage);
        m_activePage = nullptr;
        m_activeSourcePage = -1;
        m_activePageIndex = -1;
        m_activePageJavaScriptOpened = false;
        m_textInputFocused = false;
        return true;
    }

    void beginEvent(int page, bool allowHostActions = false)
    {
        m_eventChanged = false;
        m_eventInvalidated = false;
        m_currentEventPage = page;
        m_eventActive = true;
        m_hostActionsAllowed = allowHostActions;
        m_eventGoTo.reset();
        m_eventExternalUri.clear();
    }

    void finishEvent(PDFFormWorker::InteractionResult &result)
    {
        result.page = m_currentEventPage;
        result.changed = m_eventChanged;
        result.invalidated = m_eventInvalidated;
        result.cursorType = m_cursorType;
        result.acceptsTextInput = m_textInputFocused;
        result.goTo = std::move(m_eventGoTo);
        result.externalUri = std::move(m_eventExternalUri);
        m_eventActive = false;
        m_hostActionsAllowed = false;

        if (m_formHandle) {
            int focusedPage = -1;
            FPDF_ANNOTATION annotation = nullptr;
            if (FORM_GetFocusedAnnot(
                    m_formHandle,
                    &focusedPage,
                    &annotation)) {
                result.hasFocus = annotation != nullptr && focusedPage >= 0;
                if (annotation) {
                    const int fieldType = FPDFAnnot_GetFormFieldType(
                        m_formHandle,
                        annotation);
                    result.acceptsTextInput
                        = fieldType == FPDF_FORMFIELD_TEXTFIELD
                        || fieldType == FPDF_FORMFIELD_COMBOBOX;
                    FPDFPage_CloseAnnot(annotation);
                }
            }
        }
        if (!result.hasFocus) {
            result.acceptsTextInput = false;
        }
        if (m_formHandle && m_activePage) {
            result.canUndo = FORM_CanUndo(m_formHandle, m_activePage);
            result.canRedo = FORM_CanRedo(m_formHandle, m_activePage);
        }
    }

    FormFillInfo m_formFillInfo;
    JavaScriptPlatform m_javaScriptPlatform;
    FPDF_DOCUMENT m_document = nullptr;
    FPDF_DOCUMENT m_previewDocument = nullptr;
    FPDF_FORMHANDLE m_formHandle = nullptr;
    FPDF_PAGE m_activePage = nullptr;
    QHash<int, FPDF_PAGE> m_javaScriptPages;
    QString m_filePath;
    int m_revision = -1;
    int m_activeSourcePage = -1;
    int m_activePageIndex = -1;
    int m_currentSourcePageHint = -1;
    int m_currentEventPage = -1;
    int m_cursorType = FXCT_ARROW;
    bool m_textInputFocused = false;
    bool m_eventChanged = false;
    bool m_eventInvalidated = false;
    bool m_eventActive = false;
    bool m_hostActionsAllowed = false;
    std::optional<PDFFormWorker::GoToRequest> m_eventGoTo;
    QByteArray m_eventExternalUri;
    bool m_javascriptEnabled = false;
    bool m_activePageJavaScriptOpened = false;
};

void recordJavaScriptGoToAction(
    IPDF_JSPLATFORM *platform,
    int sourcePage)
{
    if (!platform || !platform->m_pFormfillinfo) {
        return;
    }
    if (PDFiumFormDocument *document = formDocument(
            static_cast<FPDF_FORMFILLINFO *>(
                platform->m_pFormfillinfo))) {
        document->recordGoToActionLocked(
            sourcePage,
            0,
            nullptr,
            0);
    }
}

using FormRegistry = std::unordered_map<quint64, std::unique_ptr<PDFiumFormDocument>>;

FormRegistry &formRegistry()
{
    static thread_local FormRegistry registry;
    return registry;
}

PDFiumFormDocument &formDocumentForSession(quint64 sessionId)
{
    FormRegistry &registry = formRegistry();
    auto found = registry.find(sessionId);
    if (found == registry.end()) {
        found = registry.emplace(
            sessionId,
            std::make_unique<PDFiumFormDocument>()).first;
    }
    return *found->second;
}

} // namespace

namespace PDFFormWorker {

void submitRender(const void *owner,
                  quint64 sessionId,
                  std::function<void()> work)
{
    workQueue().submitRender(owner, sessionId, std::move(work));
}

void cancelRender(const void *owner)
{
    workQueue().cancelRender(owner);
}

bool withPageLocked(const DocumentKey &key,
                    int sourcePage,
                    const PageCallback &callback)
{
    assertPDFiumProcessLockHeld();
    return formDocumentForSession(key.sessionId).withPageLocked(
        key,
        sourcePage,
        callback);
}

void submitPointerEvent(const DocumentKey &key,
                        int page,
                        int sourcePage,
                        PointerEventType type,
                        int modifiers,
                        const QPointF &position,
                        const QSize &deviceSize,
                        const InteractionCallback &callback)
{
    workQueue().submitCommand(
        key.sessionId,
        [key,
         page,
         sourcePage,
         type,
         modifiers,
         position,
         deviceSize,
         callback]() {
            InteractionResult result;
            {
                PDFiumProcessLock pdfiumLock;
                PDFiumFormDocument &document = formDocumentForSession(
                    key.sessionId);
                if (document.documentLocked(key)) {
                    result = document.pointerEventLocked(
                        page,
                        sourcePage,
                        type,
                        modifiers,
                        position,
                        deviceSize);
                }
            }
            postResult(std::move(result), callback);
        },
        type == PointerEventType::Move,
        page);
}

void submitKeyEvent(const DocumentKey &key,
                    int virtualKey,
                    int modifiers,
                    const QString &text,
                    const InteractionCallback &callback)
{
    workQueue().submitCommand(
        key.sessionId,
        [key, virtualKey, modifiers, text, callback]() {
            InteractionResult result;
            {
                PDFiumProcessLock pdfiumLock;
                PDFiumFormDocument &document = formDocumentForSession(
                    key.sessionId);
                if (document.documentLocked(key)) {
                    result = document.keyEventLocked(
                        virtualKey,
                        modifiers,
                        text);
                }
            }
            postResult(std::move(result), callback);
        });
}

void submitEditCommand(const DocumentKey &key,
                       EditCommand command,
                       const QString &text,
                       const InteractionCallback &callback)
{
    workQueue().submitCommand(
        key.sessionId,
        [key, command, text, callback]() {
            InteractionResult result;
            {
                PDFiumProcessLock pdfiumLock;
                PDFiumFormDocument &document = formDocumentForSession(
                    key.sessionId);
                if (document.documentLocked(key)) {
                    result = document.editCommandLocked(command, text);
                }
            }
            postResult(std::move(result), callback);
        });
}

void setJavaScriptEnabled(const DocumentKey &key,
                          bool enabled,
                          int page,
                          int sourcePage,
                          const JavaScriptCallback &callback)
{
    workQueue().submitCommand(
        key.sessionId,
        [key, enabled, page, sourcePage, callback]() {
            JavaScriptResult result;
            {
                PDFiumProcessLock pdfiumLock;
                result = formDocumentForSession(key.sessionId)
                             .setJavaScriptEnabledLocked(
                                 key,
                                 enabled,
                                 page,
                                 sourcePage);
            }
            postResult(std::move(result), callback);
        });
}

void submitPageChange(const DocumentKey &key,
                      int page,
                      int sourcePage,
                      const PageChangeCallback &callback)
{
    workQueue().submitCommand(
        key.sessionId,
        [key, page, sourcePage, callback]() {
            PageChangeResult result;
            {
                PDFiumProcessLock pdfiumLock;
                PDFiumFormDocument &document = formDocumentForSession(
                    key.sessionId);
                if (document.documentLocked(key)) {
                    result = document.pageChangeLocked(
                        page,
                        sourcePage);
                }
            }
            postResult(std::move(result), callback);
        });
}

void resetSession(quint64 sessionId, const InteractionCallback &callback)
{
    workQueue().resetSession(sessionId, [sessionId, callback]() {
        {
            PDFiumProcessLock pdfiumLock;
            formRegistry().erase(sessionId);
        }
        InteractionResult result;
        result.invalidated = true;
        result.page = -1;
        postResult(std::move(result), callback);
    });
}

void destroySession(quint64 sessionId)
{
    workQueue().destroySession(sessionId, [sessionId]() {
        PDFiumProcessLock pdfiumLock;
        formRegistry().erase(sessionId);
    });
}

void exportSnapshot(const DocumentKey &key,
                    const QString &filePath,
                    const SnapshotCallback &callback)
{
    if (!callback) {
        QFile::remove(filePath);
        return;
    }
    const auto deliveryGuard
        = std::make_shared<SnapshotDeliveryGuard>(filePath);
    workQueue().submitCommand(
        key.sessionId,
        [key, filePath, callback, deliveryGuard]() {
        SnapshotResult result;
        {
            PDFiumProcessLock pdfiumLock;
            result = formDocumentForSession(key.sessionId)
                         .exportSnapshotLocked(key, filePath);
        }
        const SnapshotCallback delivery
            = [callback, deliveryGuard](SnapshotResult deliveredResult) {
                  deliveryGuard->release();
                  callback(std::move(deliveredResult));
              };
        postResult(std::move(result), delivery);
        });
}

SnapshotResult exportSnapshotBlocking(const DocumentKey &key,
                                      const QString &filePath)
{
    SnapshotResult result;
    QSemaphore completed;
    const bool submitted = workQueue().submitCommand(
        key.sessionId,
        [&result, &completed, key, filePath]() {
            {
                PDFiumProcessLock pdfiumLock;
                result = formDocumentForSession(key.sessionId)
                             .exportSnapshotLocked(key, filePath);
            }
            completed.release();
        });
    if (!submitted) {
        result.error = QStringLiteral("The form session is no longer available");
        return result;
    }
    completed.acquire();
    return result;
}

} // namespace PDFFormWorker
