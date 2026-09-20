#include "PDFFormWorker.h"

#include "PDFFormAppearance.h"
#include "PDFiumSynchronization.h"

#include <QCoreApplication>
#include <QCryptographicHash>
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
#include <QUuid>
#include <QVector>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_fwlevent.h>
#include <fpdf_save.h>

namespace {

// PDF 1.7, table 8.75. PDFium does not currently expose this choice-field
// flag in fpdf_annot.h.
constexpr int kChoiceCommitOnSelectionChange = 1 << 26;
// PDFium's public annotation header does not expose the PDF Comb flag.
constexpr int kTextFieldComb = 1 << 24;
constexpr int kMaximumChoiceOptions = 4096;
constexpr qsizetype kMaximumChoiceOptionCharacters = 1024 * 1024;
constexpr int kMaximumTextFieldLength = 1000000;

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

    bool submitBackgroundCommand(quint64 sessionId,
                                 std::function<void()> work)
    {
        bool startWorker = false;
        {
            QMutexLocker locker(&m_mutex);
            if (m_destroyedSessions.contains(sessionId)) {
                return false;
            }
            m_backgroundCommands.enqueue(PendingCommand{
                sessionId,
                false,
                -1,
                std::move(work)});
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
                // Input/reset commands stay responsive. Page renders and
                // background discovery then alternate so a long document scan
                // cannot leave the viewport waiting for every page to finish.
                // A render already in progress still completes normally and
                // is rejected by the GUI if its form revision became stale.
                if (!m_commands.isEmpty()) {
                    work = std::move(m_commands.dequeue().work);
                } else {
                    if (m_preferBackgroundCommand
                        && !m_backgroundCommands.isEmpty()) {
                        work = std::move(
                            m_backgroundCommands.dequeue().work);
                        m_preferBackgroundCommand = false;
                    } else {
                        while (!m_renderOrder.isEmpty()) {
                            const void *owner = m_renderOrder.dequeue();
                            auto pending = m_pendingRenders.find(owner);
                            if (pending == m_pendingRenders.end()) {
                                continue;
                            }
                            work = std::move(pending->work);
                            m_pendingRenders.erase(pending);
                            m_preferBackgroundCommand = true;
                            break;
                        }
                        if (!work
                            && !m_backgroundCommands.isEmpty()) {
                            work = std::move(
                                m_backgroundCommands.dequeue().work);
                            m_preferBackgroundCommand = false;
                        }
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

        QQueue<PendingCommand> retainedBackgroundCommands;
        while (!m_backgroundCommands.isEmpty()) {
            PendingCommand command = std::move(
                m_backgroundCommands.dequeue());
            if (command.sessionId != sessionId) {
                retainedBackgroundCommands.enqueue(
                    std::move(command));
            }
        }
        m_backgroundCommands.swap(retainedBackgroundCommands);
    }

    QMutex m_mutex;
    QHash<const void *, PendingRender> m_pendingRenders;
    QQueue<const void *> m_renderOrder;
    QQueue<PendingCommand> m_commands;
    QQueue<PendingCommand> m_backgroundCommands;
    QSet<quint64> m_destroyedSessions;
    bool m_preferBackgroundCommand = false;
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

struct TrePDFTextFieldsScanState {
    PDFFormWorker::DocumentKey key;
    PDFFormWorker::TrePDFTextFieldsCallback callback;
    PDFFormWorker::TrePDFTextFieldsResult result;
    QSet<QString> fieldIds;
    QMutex resultMutex;
    std::atomic_bool finished = false;
    int pageCount = -1;
    int nextSourcePage = 0;
};

using TrePDFTextFieldsScan
    = std::shared_ptr<TrePDFTextFieldsScanState>;

QMutex &trePDFTextFieldsScanRegistryMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<quint64, TrePDFTextFieldsScan> &trePDFTextFieldsScanRegistry()
{
    static QHash<quint64, TrePDFTextFieldsScan> registry;
    return registry;
}

void finishTrePDFTextFieldsScan(
    const TrePDFTextFieldsScan &scan,
    bool succeeded)
{
    if (!scan
        || scan->finished.exchange(
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    PDFFormWorker::TrePDFTextFieldsResult result;
    {
        QMutexLocker resultLocker(&scan->resultMutex);
        if (succeeded) {
            scan->result.succeeded = true;
            result = std::move(scan->result);
        }
    }
    {
        QMutexLocker registryLocker(
            &trePDFTextFieldsScanRegistryMutex());
        auto &registry = trePDFTextFieldsScanRegistry();
        const auto active = registry.constFind(scan->key.sessionId);
        if (active != registry.cend()
            && active.value() == scan) {
            registry.remove(scan->key.sessionId);
        }
    }
    postResult(std::move(result), scan->callback);
}

template<typename Getter>
QString formWideString(const Getter &getter)
{
    const unsigned long byteCount = getter(nullptr, 0);
    if (byteCount <= sizeof(FPDF_WCHAR)
        || byteCount > 1024UL * 1024UL
        || byteCount % sizeof(FPDF_WCHAR) != 0) {
        return {};
    }
    QVector<FPDF_WCHAR> buffer(
        static_cast<qsizetype>(byteCount / sizeof(FPDF_WCHAR)));
    if (getter(buffer.data(), byteCount) != byteCount) {
        return {};
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        buffer.size() - 1);
}

bool isTrePDFDesignerFieldId(const QString &id, int fieldType)
{
    const QString prefix = fieldType == FPDF_FORMFIELD_TEXTFIELD
        ? QStringLiteral("text-field-")
        : fieldType == FPDF_FORMFIELD_CHECKBOX
            ? QStringLiteral("checkbox-field-")
            : fieldType == FPDF_FORMFIELD_RADIOBUTTON
                ? QStringLiteral("radio-field-")
                : fieldType == FPDF_FORMFIELD_COMBOBOX
                    ? QStringLiteral("dropdown-field-")
                    : fieldType == FPDF_FORMFIELD_LISTBOX
                        ? QStringLiteral("listbox-field-")
                        : QString();
    constexpr qsizetype uuidLength = 36;
    if (prefix.isEmpty() || !id.startsWith(prefix)) {
        return false;
    }

    const QString suffix = id.sliced(prefix.size());
    if (suffix.size() == uuidLength) {
        const QUuid uuid = QUuid::fromString(suffix);
        if (!uuid.isNull()
            && uuid.toString(QUuid::WithoutBraces).compare(
                   suffix,
                   Qt::CaseInsensitive)
                == 0) {
            return true;
        }
    }
    if (fieldType != FPDF_FORMFIELD_TEXTFIELD) {
        return false;
    }

    // Early TrePDF form drafts used a positive sequence number as /NM.
    // Accept that exact legacy shape, but do not treat arbitrary annotations
    // sharing a designer prefix as TrePDF-owned widgets.
    if (suffix.isEmpty()
        || suffix.size() > 20
        || suffix.front() == QLatin1Char('0')) {
        return false;
    }
    for (const QChar character : suffix) {
        if (!character.isDigit()) {
            return false;
        }
    }
    bool converted = false;
    const qulonglong sequence = suffix.toULongLong(&converted);
    return converted && sequence > 0;
}

std::optional<QRectF> normalizedAnnotationRect(
    FPDF_PAGE page,
    FPDF_ANNOTATION annotation)
{
    constexpr int normalizedDeviceExtent = 1000000;
    FS_RECTF rect = {};
    if (!page
        || !annotation
        || !FPDFAnnot_GetRect(annotation, &rect)) {
        return std::nullopt;
    }

    const double pageXs[4] = {
        rect.left,
        rect.left,
        rect.right,
        rect.right,
    };
    const double pageYs[4] = {
        rect.bottom,
        rect.top,
        rect.bottom,
        rect.top,
    };
    int minimumX = normalizedDeviceExtent;
    int minimumY = normalizedDeviceExtent;
    int maximumX = 0;
    int maximumY = 0;
    for (int corner = 0; corner < 4; ++corner) {
        int deviceX = 0;
        int deviceY = 0;
        if (!FPDF_PageToDevice(
                page,
                0,
                0,
                normalizedDeviceExtent,
                normalizedDeviceExtent,
                0,
                pageXs[corner],
                pageYs[corner],
                &deviceX,
                &deviceY)) {
            return std::nullopt;
        }
        minimumX = qMin(minimumX, deviceX);
        minimumY = qMin(minimumY, deviceY);
        maximumX = qMax(maximumX, deviceX);
        maximumY = qMax(maximumY, deviceY);
    }

    QRectF normalizedRect(
        static_cast<qreal>(minimumX) / normalizedDeviceExtent,
        static_cast<qreal>(minimumY) / normalizedDeviceExtent,
        static_cast<qreal>(maximumX - minimumX)
            / normalizedDeviceExtent,
        static_cast<qreal>(maximumY - minimumY)
            / normalizedDeviceExtent);
    normalizedRect = normalizedRect.intersected(
        QRectF(0.0, 0.0, 1.0, 1.0));
    return normalizedRect.isEmpty()
        ? std::nullopt
        : std::optional<QRectF>(normalizedRect);
}

QString choiceFieldIdentityToken(
    FPDF_FORMHANDLE formHandle,
    FPDF_ANNOTATION annotation,
    int sourcePage)
{
    if (!formHandle || !annotation || sourcePage < 0) {
        return {};
    }
    const int optionCount = FPDFAnnot_GetOptionCount(
        formHandle,
        annotation);
    FS_RECTF rect = {};
    if (optionCount < 0
        || optionCount > 4096
        || !FPDFAnnot_GetRect(annotation, &rect)) {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto addBytes = [&hash](const QByteArray &value) {
        hash.addData(QByteArray::number(value.size()));
        hash.addData(QByteArrayLiteral(":"));
        hash.addData(value);
        hash.addData(QByteArrayLiteral(";"));
    };
    const auto addInteger = [&addBytes](int value) {
        addBytes(QByteArray::number(value));
    };
    const auto addReal = [&addBytes](float value) {
        addBytes(QByteArray::number(
            static_cast<double>(value),
            'g',
            17));
    };
    const auto addString = [&addBytes](const QString &value) {
        addBytes(value.toUtf8());
    };

    addInteger(sourcePage);
    addInteger(FPDFAnnot_GetSubtype(annotation));
    addInteger(FPDFAnnot_GetFlags(annotation));
    addInteger(FPDFAnnot_GetFormFieldType(formHandle, annotation));
    addInteger(FPDFAnnot_GetFormFieldFlags(formHandle, annotation));
    addReal(rect.left);
    addReal(rect.top);
    addReal(rect.right);
    addReal(rect.bottom);
    addString(formWideString(
        [formHandle, annotation](FPDF_WCHAR *buffer,
                                 unsigned long byteCount) {
            return FPDFAnnot_GetFormFieldName(
                formHandle,
                annotation,
                buffer,
                byteCount);
        }));
    addString(formWideString(
        [formHandle, annotation](FPDF_WCHAR *buffer,
                                 unsigned long byteCount) {
            return FPDFAnnot_GetFormFieldAlternateName(
                formHandle,
                annotation,
                buffer,
                byteCount);
        }));
    addInteger(optionCount);

    qsizetype totalOptionCharacters = 0;
    for (int optionIndex = 0;
         optionIndex < optionCount;
         ++optionIndex) {
        const QString label = formWideString(
            [formHandle,
             annotation,
             optionIndex](FPDF_WCHAR *buffer,
                          unsigned long byteCount) {
                return FPDFAnnot_GetOptionLabel(
                    formHandle,
                    annotation,
                    optionIndex,
                    buffer,
                    byteCount);
            });
        totalOptionCharacters += label.size();
        if (totalOptionCharacters > 1024 * 1024) {
            return {};
        }
        addString(label);
    }
    return QString::fromLatin1(hash.result().toHex());
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

    PDFFormWorker::ChoiceFieldsResult choiceFieldsLocked(
        const PDFFormWorker::DocumentKey &key,
        int page,
        int sourcePage)
    {
        assertPDFiumProcessLockHeld();
        PDFFormWorker::ChoiceFieldsResult result;
        result.page = page;
        if (!documentLocked(key)
            || !m_formHandle
            || sourcePage < 0
            || sourcePage >= FPDF_GetPageCount(m_document)) {
            return result;
        }

        FPDF_PAGE formPage = nullptr;
        bool temporaryPage = false;
        if (m_activePage && m_activeSourcePage == sourcePage) {
            formPage = m_activePage;
        } else {
            const auto scriptPage = m_javaScriptPages.constFind(sourcePage);
            if (scriptPage != m_javaScriptPages.cend()) {
                formPage = scriptPage.value();
            } else {
                formPage = FPDF_LoadPage(m_document, sourcePage);
                if (!formPage) {
                    return result;
                }
                FORM_OnAfterLoadPage(formPage, m_formHandle);
                temporaryPage = true;
            }
        }

        constexpr int kNormalizedDeviceExtent = 1000000;
        int focusedAnnotationIndex = -1;
        int focusedSourcePage = -1;
        QString focusedFieldName;
        QVector<bool> focusedSelections;
        if (m_activePage) {
            int focusedPage = -1;
            FPDF_ANNOTATION focusedAnnotation = nullptr;
            if (FORM_GetFocusedAnnot(
                    m_formHandle,
                    &focusedPage,
                    &focusedAnnotation)
                && focusedAnnotation
                && focusedPage == m_activeSourcePage) {
                focusedSourcePage = focusedPage;
                focusedAnnotationIndex = FPDFPage_GetAnnotIndex(
                    m_activePage,
                    focusedAnnotation);
                focusedFieldName = formWideString(
                    [this,
                     focusedAnnotation](FPDF_WCHAR *buffer,
                                        unsigned long byteCount) {
                        return FPDFAnnot_GetFormFieldName(
                            m_formHandle,
                            focusedAnnotation,
                            buffer,
                            byteCount);
                    });
                const int focusedOptionCount
                    = FPDFAnnot_GetOptionCount(
                        m_formHandle,
                        focusedAnnotation);
                if (focusedOptionCount >= 0
                    && focusedOptionCount
                        <= kMaximumChoiceOptions) {
                    focusedSelections.reserve(focusedOptionCount);
                    for (int optionIndex = 0;
                         optionIndex < focusedOptionCount;
                         ++optionIndex) {
                        focusedSelections.append(
                            FORM_IsIndexSelected(
                                m_formHandle,
                                m_activePage,
                                optionIndex));
                    }
                }
            }
            if (focusedAnnotation) {
                FPDFPage_CloseAnnot(focusedAnnotation);
            }
        }
        const int annotationCount = FPDFPage_GetAnnotCount(formPage);
        for (int annotationIndex = 0;
             annotationIndex < annotationCount;
             ++annotationIndex) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
                formPage,
                annotationIndex);
            if (!annotation) {
                continue;
            }

            const int annotationFlags = FPDFAnnot_GetFlags(annotation);
            const int fieldType = FPDFAnnot_GetFormFieldType(
                m_formHandle,
                annotation);
            const int fieldFlags = FPDFAnnot_GetFormFieldFlags(
                m_formHandle,
                annotation);
            const bool hidden
                = annotationFlags
                    & (FPDF_ANNOT_FLAG_INVISIBLE
                       | FPDF_ANNOT_FLAG_HIDDEN
                       | FPDF_ANNOT_FLAG_NOVIEW);
            const bool editableCombo
                = fieldType == FPDF_FORMFIELD_COMBOBOX
                && (fieldFlags & FPDF_FORMFLAG_CHOICE_EDIT);
            const bool commitOnSelectionChange
                = fieldFlags & kChoiceCommitOnSelectionChange;
            const int optionCount = FPDFAnnot_GetOptionCount(
                m_formHandle,
                annotation);
            FS_RECTF rect = {};
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_WIDGET
                || hidden
                || (fieldType != FPDF_FORMFIELD_COMBOBOX
                    && fieldType != FPDF_FORMFIELD_LISTBOX)
                || editableCombo
                || commitOnSelectionChange
                || FPDFAnnot_HasKey(annotation, "A")
                || FPDFAnnot_HasKey(annotation, "AA")
                || optionCount < 0
                || optionCount > kMaximumChoiceOptions
                || !FPDFAnnot_GetRect(annotation, &rect)) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            const double pageXs[4] = {
                rect.left,
                rect.left,
                rect.right,
                rect.right,
            };
            const double pageYs[4] = {
                rect.bottom,
                rect.top,
                rect.bottom,
                rect.top,
            };
            int minimumX = kNormalizedDeviceExtent;
            int minimumY = kNormalizedDeviceExtent;
            int maximumX = 0;
            int maximumY = 0;
            bool mapped = true;
            for (int corner = 0; corner < 4; ++corner) {
                int deviceX = 0;
                int deviceY = 0;
                if (!FPDF_PageToDevice(
                        formPage,
                        0,
                        0,
                        kNormalizedDeviceExtent,
                        kNormalizedDeviceExtent,
                        0,
                        pageXs[corner],
                        pageYs[corner],
                        &deviceX,
                        &deviceY)) {
                    mapped = false;
                    break;
                }
                minimumX = qMin(minimumX, deviceX);
                minimumY = qMin(minimumY, deviceY);
                maximumX = qMax(maximumX, deviceX);
                maximumY = qMax(maximumY, deviceY);
            }
            if (!mapped) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            QRectF normalizedRect(
                static_cast<qreal>(minimumX) / kNormalizedDeviceExtent,
                static_cast<qreal>(minimumY) / kNormalizedDeviceExtent,
                static_cast<qreal>(maximumX - minimumX)
                    / kNormalizedDeviceExtent,
                static_cast<qreal>(maximumY - minimumY)
                    / kNormalizedDeviceExtent);
            normalizedRect = normalizedRect.intersected(
                QRectF(0.0, 0.0, 1.0, 1.0));
            if (normalizedRect.isEmpty()) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            PDFFormWorker::ChoiceField field;
            field.annotationIndex = annotationIndex;
            field.sourcePage = sourcePage;
            field.fieldType = fieldType;
            field.normalizedRect = normalizedRect;
            field.readOnly
                = (fieldFlags & FPDF_FORMFLAG_READONLY)
                || (annotationFlags & FPDF_ANNOT_FLAG_READONLY);
            field.multiSelect
                = fieldType == FPDF_FORMFIELD_LISTBOX
                && (fieldFlags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
            const QString fieldName = formWideString(
                [this, annotation](FPDF_WCHAR *buffer,
                                   unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldName(
                        m_formHandle,
                        annotation,
                        buffer,
                        byteCount);
                });
            field.accessibleName = formWideString(
                [this, annotation](FPDF_WCHAR *buffer,
                                   unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldAlternateName(
                        m_formHandle,
                        annotation,
                        buffer,
                        byteCount);
                });
            if (field.accessibleName.isEmpty()) {
                field.accessibleName = fieldName;
            }
            field.identityToken = choiceFieldIdentityToken(
                m_formHandle,
                annotation,
                sourcePage);
            if (field.identityToken.isEmpty()) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }
            const bool usesFocusedSelection
                = optionCount == focusedSelections.size()
                && ((sourcePage == focusedSourcePage
                        && annotationIndex
                            == focusedAnnotationIndex)
                    || (!fieldName.isEmpty()
                        && fieldName == focusedFieldName));

            field.options.reserve(optionCount);
            qsizetype totalOptionCharacters = 0;
            bool optionsTooLarge = false;
            for (int optionIndex = 0;
                 optionIndex < optionCount;
                 ++optionIndex) {
                const QString label = formWideString(
                    [this,
                     annotation,
                     optionIndex](FPDF_WCHAR *buffer,
                                  unsigned long byteCount) {
                        return FPDFAnnot_GetOptionLabel(
                            m_formHandle,
                            annotation,
                            optionIndex,
                            buffer,
                            byteCount);
                    });
                totalOptionCharacters += label.size();
                if (totalOptionCharacters > 1024 * 1024) {
                    optionsTooLarge = true;
                    break;
                }
                field.options.append(label);
                const bool optionSelected
                    = usesFocusedSelection
                    ? focusedSelections.at(optionIndex)
                    : FPDFAnnot_IsOptionSelected(
                          m_formHandle,
                          annotation,
                          optionIndex);
                if (optionSelected) {
                    field.selectedIndices.append(optionIndex);
                }
            }
            if (optionsTooLarge) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }
            result.fields.append(std::move(field));
            FPDFPage_CloseAnnot(annotation);
        }

        if (temporaryPage) {
            FORM_OnBeforeClosePage(formPage, m_formHandle);
            FPDF_ClosePage(formPage);
        }
        result.succeeded = true;
        return result;
    }

    std::optional<int> beginTrePDFTextFieldsScanLocked(
        const PDFFormWorker::DocumentKey &key)
    {
        assertPDFiumProcessLockHeld();
        if (!documentLocked(key) || !m_formHandle) {
            return std::nullopt;
        }
        if (FPDF_GetFormType(m_document) == FORMTYPE_NONE) {
            return 0;
        }
        return FPDF_GetPageCount(m_document);
    }

    bool trePDFTextFieldsPageLocked(
        const PDFFormWorker::DocumentKey &key,
        int sourcePage,
        QVector<PDFFormWorker::TrePDFTextField> &fields)
    {
        assertPDFiumProcessLockHeld();
        fields.clear();
        if (!m_document
            || !m_formHandle
            || m_filePath != key.filePath
            || m_revision != key.revision
            || sourcePage < 0
            || sourcePage >= FPDF_GetPageCount(m_document)) {
            return false;
        }

        FPDF_PAGE formPage = nullptr;
        bool temporaryPage = false;
        if (m_activePage && m_activeSourcePage == sourcePage) {
            formPage = m_activePage;
        } else {
            const auto scriptPage = m_javaScriptPages.constFind(
                sourcePage);
            if (scriptPage != m_javaScriptPages.cend()) {
                formPage = scriptPage.value();
            } else {
                formPage = FPDF_LoadPage(m_document, sourcePage);
                if (!formPage) {
                    return false;
                }
                FORM_OnAfterLoadPage(formPage, m_formHandle);
                temporaryPage = true;
            }
        }

        const int annotationCount = FPDFPage_GetAnnotCount(formPage);
        for (int annotationIndex = 0;
             annotationIndex < annotationCount;
             ++annotationIndex) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
                formPage,
                annotationIndex);
            if (!annotation) {
                continue;
            }

            const int fieldType = FPDFAnnot_GetFormFieldType(
                m_formHandle,
                annotation);
            const bool designerWidget
                = FPDFAnnot_GetSubtype(annotation)
                      == FPDF_ANNOT_WIDGET
                && (fieldType == FPDF_FORMFIELD_TEXTFIELD
                    || fieldType == FPDF_FORMFIELD_CHECKBOX
                    || fieldType == FPDF_FORMFIELD_RADIOBUTTON
                    || fieldType == FPDF_FORMFIELD_COMBOBOX
                    || fieldType == FPDF_FORMFIELD_LISTBOX);
            if (!designerWidget) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            const QString id = formWideString(
                [annotation](FPDF_WCHAR *buffer,
                             unsigned long byteCount) {
                    return FPDFAnnot_GetStringValue(
                        annotation,
                        "NM",
                        buffer,
                        byteCount);
                });
            if (!isTrePDFDesignerFieldId(id, fieldType)) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            const std::optional<QRectF> normalizedRect
                = normalizedAnnotationRect(formPage, annotation);
            if (!normalizedRect) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            PDFFormWorker::TrePDFTextField field;
            field.id = id;
            field.type = fieldType;
            field.annotationIndex = annotationIndex;
            field.name = formWideString(
                [this,
                 annotation](FPDF_WCHAR *buffer,
                              unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldName(
                        m_formHandle,
                        annotation,
                        buffer,
                        byteCount);
                });
            if (field.name.isEmpty()) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }
            field.tooltip = formWideString(
                [this,
                 annotation](FPDF_WCHAR *buffer,
                              unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldAlternateName(
                        m_formHandle,
                        annotation,
                        buffer,
                        byteCount);
                });
            field.value = (fieldType == FPDF_FORMFIELD_CHECKBOX
                           || fieldType == FPDF_FORMFIELD_RADIOBUTTON)
                ? FPDFAnnot_IsChecked(m_formHandle, annotation)
                    ? QStringLiteral("Yes")
                    : QStringLiteral("Off")
                : formWideString(
                      [this,
                       annotation](FPDF_WCHAR *buffer,
                                    unsigned long byteCount) {
                          return FPDFAnnot_GetFormFieldValue(
                              m_formHandle,
                              annotation,
                              buffer,
                              byteCount);
                      });
            field.defaultValue = formWideString(
                [annotation](FPDF_WCHAR *buffer,
                             unsigned long byteCount) {
                    return FPDFAnnot_GetStringValue(
                        annotation,
                        "DV",
                        buffer,
                        byteCount);
                });
            if (fieldType == FPDF_FORMFIELD_RADIOBUTTON
                && field.defaultValue.isEmpty()) {
                FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(
                    annotation,
                    "Parent");
                if (parent) {
                    field.defaultValue = formWideString(
                        [parent](FPDF_WCHAR *buffer,
                                 unsigned long byteCount) {
                            return FPDFAnnot_GetStringValue(
                                parent,
                                "DV",
                                buffer,
                                byteCount);
                        });
                    FPDFPage_CloseAnnot(parent);
                }
            }
            if (fieldType == FPDF_FORMFIELD_RADIOBUTTON) {
                float defaultChecked = 0.0f;
                const bool hasDefaultChecked
                    = FPDFAnnot_GetNumberValue(
                        annotation,
                        PDFFormAppearance::kRadioButtonDefaultCheckedKey,
                        &defaultChecked)
                    && std::isfinite(defaultChecked)
                    && (defaultChecked == 0.0f
                        || defaultChecked == 1.0f);
                if (hasDefaultChecked) {
                    field.defaultValue = defaultChecked == 1.0f
                        ? QStringLiteral("Yes")
                        : QStringLiteral("Off");
                } else {
                    const QString exportValue = formWideString(
                        [this,
                         annotation](FPDF_WCHAR *buffer,
                                      unsigned long byteCount) {
                            return FPDFAnnot_GetFormFieldExportValue(
                                m_formHandle,
                                annotation,
                                buffer,
                                byteCount);
                        });
                    field.defaultValue
                        = !exportValue.isEmpty()
                            && field.defaultValue == exportValue
                        ? QStringLiteral("Yes")
                        : QStringLiteral("Off");
                }
            }
            if (fieldType == FPDF_FORMFIELD_COMBOBOX
                || fieldType == FPDF_FORMFIELD_LISTBOX) {
                const int choiceFlags = FPDFAnnot_GetFormFieldFlags(
                    m_formHandle,
                    annotation);
                const bool combo = choiceFlags
                    & FPDF_FORMFLAG_CHOICE_COMBO;
                if ((fieldType == FPDF_FORMFIELD_COMBOBOX) != combo
                    || (choiceFlags & FPDF_FORMFLAG_CHOICE_EDIT)
                    || (fieldType == FPDF_FORMFIELD_COMBOBOX
                        && (choiceFlags
                            & FPDF_FORMFLAG_CHOICE_MULTI_SELECT))
                    || (choiceFlags & kChoiceCommitOnSelectionChange)
                    || FPDFAnnot_HasKey(annotation, "A")
                    || FPDFAnnot_HasKey(annotation, "AA")) {
                    FPDFPage_CloseAnnot(annotation);
                    continue;
                }
                const int optionCount = FPDFAnnot_GetOptionCount(
                    m_formHandle,
                    annotation);
                qsizetype totalOptionCharacters = 0;
                QSet<QString> uniqueOptions;
                bool validOptions = optionCount > 0
                    && optionCount <= kMaximumChoiceOptions;
                if (validOptions) {
                    field.options.reserve(optionCount);
                    for (int optionIndex = 0;
                         optionIndex < optionCount;
                         ++optionIndex) {
                        const QString option = formWideString(
                            [this,
                             annotation,
                             optionIndex](FPDF_WCHAR *buffer,
                                          unsigned long byteCount) {
                                return FPDFAnnot_GetOptionLabel(
                                    m_formHandle,
                                    annotation,
                                    optionIndex,
                                    buffer,
                                    byteCount);
                            });
                        totalOptionCharacters += option.size();
                        if (option.isEmpty()
                            || totalOptionCharacters
                                > kMaximumChoiceOptionCharacters
                            || uniqueOptions.contains(option)) {
                            validOptions = false;
                            break;
                        }
                        uniqueOptions.insert(option);
                        field.options.append(option);
                        if (fieldType == FPDF_FORMFIELD_LISTBOX
                            && FPDFAnnot_IsOptionSelected(
                                m_formHandle,
                                annotation,
                                optionIndex)) {
                            field.selectedValues.append(option);
                        }
                    }
                }
                if (!validOptions
                    || (fieldType == FPDF_FORMFIELD_COMBOBOX
                        && !field.options.contains(field.value))
                    || !field.options.contains(field.defaultValue)) {
                    FPDFPage_CloseAnnot(annotation);
                    continue;
                }
                if (fieldType == FPDF_FORMFIELD_LISTBOX) {
                    field.value = field.selectedValues.value(0);
                }
                field.identityToken = choiceFieldIdentityToken(
                    m_formHandle,
                    annotation,
                    sourcePage);
                if (field.identityToken.isEmpty()) {
                    FPDFPage_CloseAnnot(annotation);
                    continue;
                }
            }
            float appearanceVersion = 0.0f;
            if (FPDFAnnot_GetNumberValue(
                    annotation,
                    PDFFormAppearance::kVersionKey,
                    &appearanceVersion)) {
                const double parsedAppearanceVersion
                    = static_cast<double>(appearanceVersion);
                if (std::isfinite(parsedAppearanceVersion)
                    && parsedAppearanceVersion >= 0.0
                    && parsedAppearanceVersion
                        <= static_cast<double>(
                            std::numeric_limits<int>::max())
                    && std::floor(parsedAppearanceVersion)
                        == parsedAppearanceVersion) {
                    field.appearanceVersion
                        = static_cast<int>(parsedAppearanceVersion);
                }
            }
            const int fieldFlags = FPDFAnnot_GetFormFieldFlags(
                m_formHandle,
                annotation);
            const int annotationFlags = FPDFAnnot_GetFlags(annotation);
            field.flags = fieldFlags
                & (FPDF_FORMFLAG_READONLY
                   | FPDF_FORMFLAG_REQUIRED);
            if (field.type == FPDF_FORMFIELD_TEXTFIELD) {
                field.flags |= fieldFlags
                    & (FPDF_FORMFLAG_TEXT_MULTILINE
                       | kTextFieldComb);
            }
            if (annotationFlags & FPDF_ANNOT_FLAG_READONLY) {
                field.flags |= FPDF_FORMFLAG_READONLY;
            }
            if (field.type == FPDF_FORMFIELD_TEXTFIELD) {
                float maxLength = 0.0f;
                if (FPDFAnnot_GetNumberValue(
                        annotation,
                        "MaxLen",
                        &maxLength)) {
                    const double parsedMaxLength
                        = static_cast<double>(maxLength);
                    if (std::isfinite(parsedMaxLength)
                        && parsedMaxLength > 0.0
                        && parsedMaxLength
                            <= static_cast<double>(
                                kMaximumTextFieldLength)
                        && std::floor(parsedMaxLength)
                            == parsedMaxLength) {
                        field.maxLength
                            = static_cast<int>(parsedMaxLength);
                    }
                }
                if ((field.flags & kTextFieldComb)
                    && (field.maxLength <= 0
                        || (field.flags
                            & FPDF_FORMFLAG_TEXT_MULTILINE))) {
                    field.flags &= ~kTextFieldComb;
                }
            }
            field.sourcePage = sourcePage;
            field.normalizedRect = *normalizedRect;
            fields.append(std::move(field));
            FPDFPage_CloseAnnot(annotation);
        }

        if (temporaryPage) {
            FORM_OnBeforeClosePage(formPage, m_formHandle);
            FPDF_ClosePage(formPage);
        }
        return true;
    }

    PDFFormWorker::InteractionResult focusChoiceFieldLocked(
        const PDFFormWorker::DocumentKey &key,
        int page,
        int sourcePage,
        int annotationIndex,
        const QString &expectedIdentityToken)
    {
        assertPDFiumProcessLockHeld();
        PDFFormWorker::InteractionResult result;
        result.page = page;
        beginEvent(page, true);
        if (!documentLocked(key)
            || !m_formHandle
            || annotationIndex < 0
            || !ensureActivePageLocked(sourcePage, page)) {
            finishEvent(result);
            return result;
        }

        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
            m_activePage,
            annotationIndex);
        if (!annotation) {
            finishEvent(result);
            return result;
        }
        const int fieldType = FPDFAnnot_GetFormFieldType(
            m_formHandle,
            annotation);
        const int fieldFlags = FPDFAnnot_GetFormFieldFlags(
            m_formHandle,
            annotation);
        const int annotationFlags = FPDFAnnot_GetFlags(annotation);
        const bool hidden
            = annotationFlags
                & (FPDF_ANNOT_FLAG_INVISIBLE
                   | FPDF_ANNOT_FLAG_HIDDEN
                   | FPDF_ANNOT_FLAG_NOVIEW);
        const bool editableCombo
            = fieldType == FPDF_FORMFIELD_COMBOBOX
            && (fieldFlags & FPDF_FORMFLAG_CHOICE_EDIT);
        const bool commitOnSelectionChange
            = fieldFlags & kChoiceCommitOnSelectionChange;
        const bool readOnly
            = (fieldFlags & FPDF_FORMFLAG_READONLY)
            || (annotationFlags & FPDF_ANNOT_FLAG_READONLY);
        const QString identityToken = choiceFieldIdentityToken(
            m_formHandle,
            annotation,
            sourcePage);
        if (!hidden
            && !editableCombo
            && !commitOnSelectionChange
            && !readOnly
            && !FPDFAnnot_HasKey(annotation, "A")
            && !FPDFAnnot_HasKey(annotation, "AA")
            && !identityToken.isEmpty()
            && (expectedIdentityToken.isEmpty()
                || identityToken == expectedIdentityToken)
            && (fieldType == FPDF_FORMFIELD_COMBOBOX
                || fieldType == FPDF_FORMFIELD_LISTBOX)) {
            result.handled = FORM_SetFocusedAnnot(
                m_formHandle,
                annotation);
        }
        FPDFPage_CloseAnnot(annotation);
        finishEvent(result);
        return result;
    }

    PDFFormWorker::InteractionResult selectChoiceOptionLocked(
        const PDFFormWorker::DocumentKey &key,
        int page,
        int sourcePage,
        int annotationIndex,
        int optionIndex,
        bool selected,
        bool toggle,
        const QString &expectedIdentityToken)
    {
        assertPDFiumProcessLockHeld();
        PDFFormWorker::InteractionResult result;
        result.page = page;
        beginEvent(page, true);
        if (!documentLocked(key)
            || !m_formHandle
            || annotationIndex < 0
            || optionIndex < 0
            || !ensureActivePageLocked(sourcePage, page)) {
            finishEvent(result);
            return result;
        }

        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
            m_activePage,
            annotationIndex);
        if (!annotation) {
            finishEvent(result);
            return result;
        }

        const int fieldType = FPDFAnnot_GetFormFieldType(
            m_formHandle,
            annotation);
        const int fieldFlags = FPDFAnnot_GetFormFieldFlags(
            m_formHandle,
            annotation);
        const int annotationFlags = FPDFAnnot_GetFlags(annotation);
        const int optionCount = FPDFAnnot_GetOptionCount(
            m_formHandle,
            annotation);
        const bool supportedType
            = fieldType == FPDF_FORMFIELD_COMBOBOX
            || fieldType == FPDF_FORMFIELD_LISTBOX;
        const bool hidden
            = annotationFlags
                & (FPDF_ANNOT_FLAG_INVISIBLE
                   | FPDF_ANNOT_FLAG_HIDDEN
                   | FPDF_ANNOT_FLAG_NOVIEW);
        const bool editableCombo
            = fieldType == FPDF_FORMFIELD_COMBOBOX
            && (fieldFlags & FPDF_FORMFLAG_CHOICE_EDIT);
        const bool commitOnSelectionChange
            = fieldFlags & kChoiceCommitOnSelectionChange;
        const bool readOnly
            = (fieldFlags & FPDF_FORMFLAG_READONLY)
            || (annotationFlags & FPDF_ANNOT_FLAG_READONLY);
        if (!supportedType
            || FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_WIDGET
            || hidden
            || editableCombo
            || commitOnSelectionChange
            || readOnly
            || FPDFAnnot_HasKey(annotation, "A")
            || FPDFAnnot_HasKey(annotation, "AA")
            || optionIndex >= optionCount) {
            FPDFPage_CloseAnnot(annotation);
            finishEvent(result);
            return result;
        }
        const QString identityToken = choiceFieldIdentityToken(
            m_formHandle,
            annotation,
            sourcePage);
        if (identityToken.isEmpty()
            || (!expectedIdentityToken.isEmpty()
                && identityToken != expectedIdentityToken)
            || !FORM_SetFocusedAnnot(m_formHandle, annotation)) {
            FPDFPage_CloseAnnot(annotation);
            finishEvent(result);
            return result;
        }

        int focusedPage = -1;
        FPDF_ANNOTATION focusedAnnotation = nullptr;
        if (!FORM_GetFocusedAnnot(
                m_formHandle,
                &focusedPage,
                &focusedAnnotation)
            || !focusedAnnotation
            || focusedPage != sourcePage
            || FPDFPage_GetAnnotIndex(
                   m_activePage,
                   focusedAnnotation)
                != annotationIndex) {
            if (focusedAnnotation) {
                FPDFPage_CloseAnnot(focusedAnnotation);
            }
            FPDFPage_CloseAnnot(annotation);
            finishEvent(result);
            return result;
        }
        const int focusedFieldType = FPDFAnnot_GetFormFieldType(
            m_formHandle,
            focusedAnnotation);
        const int focusedFieldFlags = FPDFAnnot_GetFormFieldFlags(
            m_formHandle,
            focusedAnnotation);
        const int focusedOptionCount = FPDFAnnot_GetOptionCount(
            m_formHandle,
            focusedAnnotation);
        const int focusedAnnotationFlags = FPDFAnnot_GetFlags(
            focusedAnnotation);
        const bool focusedHidden
            = focusedAnnotationFlags
                & (FPDF_ANNOT_FLAG_INVISIBLE
                   | FPDF_ANNOT_FLAG_HIDDEN
                   | FPDF_ANNOT_FLAG_NOVIEW);
        const bool focusedEditableCombo
            = focusedFieldType == FPDF_FORMFIELD_COMBOBOX
            && (focusedFieldFlags & FPDF_FORMFLAG_CHOICE_EDIT);
        const bool focusedCommitOnSelectionChange
            = focusedFieldFlags & kChoiceCommitOnSelectionChange;
        const bool focusedReadOnly
            = (focusedFieldFlags & FPDF_FORMFLAG_READONLY)
            || (focusedAnnotationFlags & FPDF_ANNOT_FLAG_READONLY);
        const bool focusedMultiSelect
            = focusedFieldType == FPDF_FORMFIELD_LISTBOX
            && (focusedFieldFlags
                & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
        const bool originalMultiSelect
            = fieldType == FPDF_FORMFIELD_LISTBOX
            && (fieldFlags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
        if (FPDFAnnot_GetSubtype(focusedAnnotation) != FPDF_ANNOT_WIDGET
            || focusedFieldType != fieldType
            || focusedOptionCount != optionCount
            || optionIndex >= focusedOptionCount
            || focusedHidden
            || focusedEditableCombo
            || focusedCommitOnSelectionChange
            || focusedReadOnly
            || FPDFAnnot_HasKey(focusedAnnotation, "A")
            || FPDFAnnot_HasKey(focusedAnnotation, "AA")
            || focusedMultiSelect != originalMultiSelect) {
            FPDFPage_CloseAnnot(focusedAnnotation);
            FPDFPage_CloseAnnot(annotation);
            finishEvent(result);
            return result;
        }
        const QString focusedIdentityToken = choiceFieldIdentityToken(
            m_formHandle,
            focusedAnnotation,
            sourcePage);
        FPDFPage_CloseAnnot(focusedAnnotation);
        FPDFPage_CloseAnnot(annotation);
        if (focusedIdentityToken.isEmpty()
            || focusedIdentityToken != identityToken
            || (!expectedIdentityToken.isEmpty()
                && focusedIdentityToken
                    != expectedIdentityToken)) {
            finishEvent(result);
            return result;
        }

        QVector<bool> selectedBefore;
        selectedBefore.reserve(optionCount);
        for (int index = 0; index < optionCount; ++index) {
            selectedBefore.append(FORM_IsIndexSelected(
                m_formHandle,
                m_activePage,
                index));
        }

        const bool multiSelect
            = focusedMultiSelect;
        const bool selectedValue = multiSelect && toggle
            ? !selectedBefore.at(optionIndex)
            : selected;
        const bool handled = FORM_SetIndexSelected(
            m_formHandle,
            m_activePage,
            optionIndex,
            fieldType == FPDF_FORMFIELD_COMBOBOX || !multiSelect
                ? true
                : selectedValue);

        bool selectionChanged = false;
        for (int index = 0; index < optionCount; ++index) {
            if (selectedBefore.at(index)
                != static_cast<bool>(FORM_IsIndexSelected(
                    m_formHandle,
                    m_activePage,
                    index))) {
                selectionChanged = true;
                break;
            }
        }
        finishEvent(result);
        result.handled = handled;
        result.changed = result.changed || selectionChanged;
        result.invalidated = result.invalidated || selectionChanged;
        return result;
    }

    PDFFormWorker::InteractionResult pointerEventLocked(
        int page,
        int sourcePage,
        PDFFormWorker::PointerEventType type,
        int modifiers,
        const QPointF &position,
        const QSize &deviceSize,
        const QSet<int> &removedWebLinks)
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
            m_cursorType = FXCT_ARROW;
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
        // PDFium's form callbacks handle Widgets, but do not activate ordinary
        // Link annotations. Keep their hit testing on this worker-owned page.
        FPDF_LINK webLink = nullptr;
        const int linkZ = FPDFLink_GetLinkZOrderAtPoint(eventPage, pageX, pageY);
        const int fieldZ = FPDFPage_FormFieldZOrderAtPoint(
            m_formHandle, eventPage, pageX, pageY);
        if (linkZ >= 0 && linkZ > fieldZ && !removedWebLinks.contains(linkZ)) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(eventPage, linkZ);
            if (annotation) {
                const int flags = FPDFAnnot_GetFlags(annotation);
                if (FPDFAnnot_GetSubtype(annotation) == FPDF_ANNOT_LINK
                    && !(flags & (FPDF_ANNOT_FLAG_HIDDEN | FPDF_ANNOT_FLAG_INVISIBLE
                                  | FPDF_ANNOT_FLAG_NOVIEW))) {
                    FPDF_LINK link = FPDFAnnot_GetLink(annotation);
                    if (link) {
                        const FPDF_ACTION action = FPDFLink_GetAction(link);
                        if (FPDFAction_GetType(action) == PDFACTION_URI
                            || FPDFLink_GetDest(m_document, link)
                            || (FPDFAction_GetType(action) == PDFACTION_GOTO
                                && FPDFAction_GetDest(m_document, action)))
                            webLink = link;
                    }
                }
                FPDFPage_CloseAnnot(annotation);
            }
        }
        if (type == PDFFormWorker::PointerEventType::Move && webLink) {
            m_cursorType = FXCT_HAND;
            result.webLinkSourcePage = sourcePage;
            result.webLinkAnnotationIndex = linkZ;
        }
        if (type == PDFFormWorker::PointerEventType::Press)
            m_pressedWebLink = webLink;
        if (type == PDFFormWorker::PointerEventType::Release) {
            const FPDF_LINK pressed = std::exchange(m_pressedWebLink, nullptr);
            if (webLink && webLink == pressed && m_eventExternalUri.isEmpty()) {
                const FPDF_ACTION action = FPDFLink_GetAction(webLink);
                FPDF_DEST destination = FPDFLink_GetDest(m_document, webLink);
                if (!destination && FPDFAction_GetType(action) == PDFACTION_GOTO)
                    destination = FPDFAction_GetDest(m_document, action);
                if (destination) {
                    const int target = FPDFDest_GetDestPageIndex(m_document, destination);
                    if (target >= 0 && target < FPDF_GetPageCount(m_document)) {
                        PDFFormWorker::GoToRequest request;
                        request.sourcePage = target;
                        m_eventGoTo = request;
                        result.webLinkSourcePage = sourcePage;
                        result.webLinkAnnotationIndex = linkZ;
                        result.handled = true;
                    }
                }
                const unsigned long length = FPDFAction_GetURIPath(m_document, action, nullptr, 0);
                if (length > 1 && length <= 8193) {
                    QByteArray uri(static_cast<qsizetype>(length), '\0');
                    if (FPDFAction_GetURIPath(m_document, action, uri.data(), length) == length
                        && uri.back() == '\0') {
                        uri.chop(1);
                        // The GUI controller validates the scheme/host and asks
                        // for confirmation; the worker never opens a URL itself.
                        m_eventExternalUri = std::move(uri);
                        result.webLinkSourcePage = sourcePage;
                        result.webLinkAnnotationIndex = linkZ;
                        result.handled = true;
                    }
                }
            }
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
        } else if (virtualKey == FWL_VKEY_Back) {
            // PDFium treats Backspace as a control character. KeyDown alone
            // reports it handled but does not edit the focused text widget.
            result.handled = FORM_OnChar(
                                 m_formHandle,
                                 m_activePage,
                                 FWL_VKEY_Back,
                                 modifiers)
                || result.handled;
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
        m_pressedWebLink = nullptr;
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
                    if (focusedPage == m_activeSourcePage && m_activePage) {
                        result.focusedPage = m_activePageIndex;
                        result.focusedAnnotationIndex
                            = FPDFPage_GetAnnotIndex(
                                m_activePage,
                                annotation);
                    }
                    const int fieldType = FPDFAnnot_GetFormFieldType(
                        m_formHandle,
                        annotation);
                    const int fieldFlags = FPDFAnnot_GetFormFieldFlags(
                        m_formHandle,
                        annotation);
                    result.acceptsTextInput
                        = fieldType == FPDF_FORMFIELD_TEXTFIELD
                        || (fieldType == FPDF_FORMFIELD_COMBOBOX
                            && (fieldFlags
                                & FPDF_FORMFLAG_CHOICE_EDIT));
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
    FPDF_LINK m_pressedWebLink = nullptr;
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

void submitTrePDFTextFieldsScanPage(
    const TrePDFTextFieldsScan &scan);

void runTrePDFTextFieldsScanPage(
    const TrePDFTextFieldsScan &scan)
{
    if (!scan
        || scan->finished.load(std::memory_order_acquire)) {
        return;
    }

    QVector<PDFFormWorker::TrePDFTextField> pageFields;
    bool pageSucceeded = true;
    bool scanComplete = false;
    {
        PDFiumProcessLock pdfiumLock;
        if (scan->finished.load(std::memory_order_acquire)) {
            return;
        }

        PDFiumFormDocument &document = formDocumentForSession(
            scan->key.sessionId);
        if (scan->pageCount < 0) {
            const std::optional<int> pageCount
                = document.beginTrePDFTextFieldsScanLocked(scan->key);
            if (!pageCount) {
                pageSucceeded = false;
            } else {
                scan->pageCount = *pageCount;
            }
        }

        if (pageSucceeded) {
            scanComplete
                = scan->nextSourcePage >= scan->pageCount;
            if (!scanComplete) {
                pageSucceeded = document.trePDFTextFieldsPageLocked(
                    scan->key,
                    scan->nextSourcePage,
                    pageFields);
            }
        }
    }

    if (scan->finished.load(std::memory_order_acquire)) {
        return;
    }
    if (!pageSucceeded) {
        finishTrePDFTextFieldsScan(scan, false);
        return;
    }
    if (scanComplete) {
        finishTrePDFTextFieldsScan(scan, true);
        return;
    }

    bool duplicateFieldId = false;
    bool allPagesScanned = false;
    {
        QMutexLocker resultLocker(&scan->resultMutex);
        if (scan->finished.load(std::memory_order_acquire)) {
            return;
        }

        QSet<QString> pageFieldIds;
        for (const PDFFormWorker::TrePDFTextField &field
             : std::as_const(pageFields)) {
            if (pageFieldIds.contains(field.id)
                || scan->fieldIds.contains(field.id)) {
                duplicateFieldId = true;
                break;
            }
            pageFieldIds.insert(field.id);
        }
        if (!duplicateFieldId) {
            scan->fieldIds.unite(pageFieldIds);
            scan->result.fields += std::move(pageFields);
            ++scan->nextSourcePage;
            allPagesScanned
                = scan->nextSourcePage >= scan->pageCount;
        }
    }

    if (duplicateFieldId) {
        finishTrePDFTextFieldsScan(scan, false);
    } else if (allPagesScanned) {
        finishTrePDFTextFieldsScan(scan, true);
    } else {
        // Requeue on the background lane so pointer/key/reset commands can run
        // immediately and page renders alternate with discovery pages. A
        // request replacement or session reset marks this scan finished before
        // purging; even the narrow enqueue-after-purge race therefore produces
        // only a no-op continuation.
        submitTrePDFTextFieldsScanPage(scan);
    }
}

void submitTrePDFTextFieldsScanPage(
    const TrePDFTextFieldsScan &scan)
{
    if (!scan
        || scan->finished.load(std::memory_order_acquire)) {
        return;
    }
    const bool submitted = workQueue().submitBackgroundCommand(
        scan->key.sessionId,
        [scan]() {
            runTrePDFTextFieldsScanPage(scan);
        });
    if (!submitted) {
        finishTrePDFTextFieldsScan(scan, false);
    }
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
                        const InteractionCallback &callback,
                        const QSet<int> &removedWebLinks)
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
         callback,
         removedWebLinks]() {
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
                        deviceSize,
                        removedWebLinks);
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

void inspectChoiceFields(const DocumentKey &key,
                         int page,
                         int sourcePage,
                         const ChoiceFieldsCallback &callback)
{
    workQueue().submitCommand(
        key.sessionId,
        [key, page, sourcePage, callback]() {
            ChoiceFieldsResult result;
            {
                PDFiumProcessLock pdfiumLock;
                result = formDocumentForSession(key.sessionId)
                             .choiceFieldsLocked(
                                 key,
                                 page,
                                 sourcePage);
            }
            postResult(std::move(result), callback);
        });
}

void inspectTrePDFTextFields(
    const DocumentKey &key,
    const TrePDFTextFieldsCallback &callback)
{
    auto scan = std::make_shared<TrePDFTextFieldsScanState>();
    scan->key = key;
    scan->callback = callback;

    TrePDFTextFieldsScan replacedScan;
    {
        QMutexLocker locker(&trePDFTextFieldsScanRegistryMutex());
        auto &registry = trePDFTextFieldsScanRegistry();
        replacedScan = registry.value(key.sessionId);
        registry.insert(key.sessionId, scan);
    }
    finishTrePDFTextFieldsScan(replacedScan, false);
    submitTrePDFTextFieldsScanPage(scan);
}

void submitChoiceSelection(const DocumentKey &key,
                           int page,
                           int sourcePage,
                           int annotationIndex,
                           int optionIndex,
                           bool selected,
                           bool toggle,
                           const QString &expectedIdentityToken,
                           const InteractionCallback &callback)
{
    workQueue().submitCommand(
        key.sessionId,
        [key,
         page,
         sourcePage,
         annotationIndex,
         optionIndex,
         selected,
         toggle,
         expectedIdentityToken,
         callback]() {
            InteractionResult result;
            {
                PDFiumProcessLock pdfiumLock;
                result = formDocumentForSession(key.sessionId)
                             .selectChoiceOptionLocked(
                                 key,
                                 page,
                                 sourcePage,
                                 annotationIndex,
                                 optionIndex,
                                 selected,
                                 toggle,
                                 expectedIdentityToken);
            }
            postResult(std::move(result), callback);
        });
}

void submitChoiceFocus(const DocumentKey &key,
                       int page,
                       int sourcePage,
                       int annotationIndex,
                       const QString &expectedIdentityToken,
                       const InteractionCallback &callback)
{
    workQueue().submitCommand(
        key.sessionId,
        [key,
         page,
         sourcePage,
         annotationIndex,
         expectedIdentityToken,
         callback]() {
            InteractionResult result;
            {
                PDFiumProcessLock pdfiumLock;
                result = formDocumentForSession(key.sessionId)
                             .focusChoiceFieldLocked(
                                 key,
                                 page,
                                 sourcePage,
                                 annotationIndex,
                                 expectedIdentityToken);
            }
            postResult(std::move(result), callback);
        });
}

void resetSession(quint64 sessionId, const InteractionCallback &callback)
{
    TrePDFTextFieldsScan cancelledScan;
    {
        // Keep scan replacement and queue purging in one critical section.
        // Otherwise a request from another caller could be registered after
        // cancellation but immediately before resetSession() purges its first
        // command, leaving that request without a completion callback.
        QMutexLocker locker(&trePDFTextFieldsScanRegistryMutex());
        auto &registry = trePDFTextFieldsScanRegistry();
        cancelledScan = registry.take(sessionId);
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
    finishTrePDFTextFieldsScan(cancelledScan, false);
}

void destroySession(quint64 sessionId)
{
    TrePDFTextFieldsScan cancelledScan;
    {
        QMutexLocker locker(&trePDFTextFieldsScanRegistryMutex());
        auto &registry = trePDFTextFieldsScanRegistry();
        cancelledScan = registry.take(sessionId);
        workQueue().destroySession(sessionId, [sessionId]() {
            PDFiumProcessLock pdfiumLock;
            formRegistry().erase(sessionId);
        });
    }
    finishTrePDFTextFieldsScan(cancelledScan, false);
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
