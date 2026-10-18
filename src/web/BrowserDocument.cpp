#include "BrowserDocument.h"
#include "PageRaster.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <limits>

#ifdef Q_OS_WASM
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {
QPointer<BrowserDocument> currentDocument;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void trepdfWebMessage(const char *json)
{
    if (currentDocument && json)
        currentDocument->receiveMessage(QByteArray(json));
}
EMSCRIPTEN_KEEPALIVE void trepdfWebPixels(int request, int generation, int width,
                                       int height, const unsigned char *bytes)
{
    if (currentDocument)
        currentDocument->receivePixels(request, generation, width, height, bytes);
}
}

BrowserDocument::BrowserDocument(QObject *parent) : QObject(parent)
{
    currentDocument = this;
    QTimer::singleShot(0, this, [this] {
#ifdef Q_OS_WASM
        EM_ASM({ Module.trepdfWeb.initialize(); });
#else
        m_error = tr("Open this viewer in a browser to choose a PDF.");
        emit stateChanged();
#endif
    });
}

BrowserDocument::~BrowserDocument()
{
    if (currentDocument == this) {
#ifdef Q_OS_WASM
        EM_ASM({ Module.trepdfWeb.dispose(); });
#endif
        currentDocument.clear();
    }
}

void BrowserDocument::openFile()
{
#ifdef Q_OS_WASM
    if (m_ready && !m_busy)
        EM_ASM({ Module.trepdfWeb.chooseFile(); });
#endif
}

void BrowserDocument::closeFile()
{
#ifdef Q_OS_WASM
    EM_ASM({ Module.trepdfWeb.closeFile(); });
#endif
}

void BrowserDocument::downloadFile()
{
#ifdef Q_OS_WASM
    if (!m_pages.isEmpty() && !m_busy) {
        if (m_dirty) {
            m_busy = true;
            m_error.clear();
            emit stateChanged();
        }
        EM_ASM({ Module.trepdfWeb.downloadFile(); });
    }
#endif
}

bool BrowserDocument::replaceImage(int id)
{
    if (!m_canEdit || m_busy || m_pages.isEmpty())
        return false;
    bool eligible = false;
    for (const QVariant &value : m_objects) {
        const QVariantMap object = value.toMap();
        if (object.value(QStringLiteral("id")).toInt() == id
            && object.value(QStringLiteral("type")).toString() == QStringLiteral("image")
            && object.value(QStringLiteral("editable")).toBool()) {
            eligible = true;
            break;
        }
    }
    if (!eligible)
        return false;
#ifdef Q_OS_WASM
    return EM_ASM_INT({ return Module.trepdfWeb.replaceImage($0) ? 1 : 0; }, id) != 0;
#else
    return false;
#endif
}

bool BrowserDocument::applyEdit(const QVariantMap &command)
{
    if (!m_canEdit || m_busy || m_pages.isEmpty())
        return false;
#ifdef Q_OS_WASM
    const QByteArray json = QJsonDocument(QJsonObject::fromVariantMap(command))
                                .toJson(QJsonDocument::Compact);
    m_busy = true;
    m_error.clear();
    emit stateChanged();
    const bool accepted = EM_ASM_INT({
        return Module.trepdfWeb.edit(UTF8ToString($0)) ? 1 : 0;
    }, json.constData());
    if (!accepted) {
        m_busy = false;
        m_error = tr("Wait for the current operation, then try again.");
        emit stateChanged();
    }
    return accepted;
#else
    Q_UNUSED(command);
    return false;
#endif
}

void BrowserDocument::undo()
{
    if (m_canUndo)
        applyEdit({{QStringLiteral("action"), QStringLiteral("undo")}});
}

void BrowserDocument::redo()
{
    if (m_canRedo)
        applyEdit({{QStringLiteral("action"), QStringLiteral("redo")}});
}

void BrowserDocument::discard()
{
    if (m_dirty)
        applyEdit({{QStringLiteral("action"), QStringLiteral("discard")}});
}

void BrowserDocument::setPendingInput(bool pending)
{
#ifdef Q_OS_WASM
    EM_ASM({ Module.trepdfWeb.setPendingInput(!!$0); }, pending);
#else
    Q_UNUSED(pending);
#endif
}

void BrowserDocument::clearPages()
{
    m_requests.clear();
    m_pages.clear();
    m_objects.clear();
    m_canEdit = m_dirty = m_canUndo = m_canRedo = false;
    m_editReason.clear();
    m_sourceNotice.clear();
    emit reset();
    emit pagesChanged();
}

int BrowserDocument::requestPage(PageRaster *item, int page, int width)
{
    if (!item || m_busy || page < 0 || page >= m_pages.size() || width <= 0)
        return 0;
    do {
        m_nextRequest = m_nextRequest == std::numeric_limits<int>::max() ? 1 : m_nextRequest + 1;
    } while (m_requests.contains(m_nextRequest));
    const int request = m_nextRequest;
    m_requests.insert(request, item);
#ifdef Q_OS_WASM
    // Dispatch after PageRaster has stored its request ID, including errors that
    // the bridge can report immediately (for example a full render queue).
    QTimer::singleShot(0, this, [this, request, page, width] {
        if (m_requests.contains(request))
            EM_ASM({ Module.trepdfWeb.render($0, $1, $2); }, request, page, qBound(1, width, 4096));
    });
#endif
    return request;
}

void BrowserDocument::cancelRequest(int request)
{
    if (!m_requests.remove(request))
        return;
#ifdef Q_OS_WASM
    EM_ASM({ Module.trepdfWeb.cancel($0); }, request);
#endif
}

void BrowserDocument::receiveMessage(const QByteArray &json)
{
    const QJsonObject message = QJsonDocument::fromJson(json).object();
    const QString type = message.value("type").toString();
    const int generation = message.value("generation").toInt();
    if (type == "loading" || type == "closed") {
        m_generation = generation;
        m_busy = type == "loading";
        m_fileName = message.value("fileName").toString();
        m_error.clear();
        clearPages();
    } else if (type == "ready") {
        m_ready = true;
    } else if (generation != m_generation) {
        return;
    } else if (type == "editing") {
        m_busy = true;
        m_error.clear();
    } else if (type == "indexed") {
        m_objects = message.value("objects").toArray().toVariantList();
        m_sourceNotice = message.value("sourceNotice").toString();
    } else if (type == "opened") {
        m_objects = message.value("objects").toArray().toVariantList();
        m_sourceNotice = message.value("sourceNotice").toString();
        m_pages = message.value("pages").toArray().toVariantList();
        m_canEdit = message.value("canEdit").toBool();
        m_editReason = message.value("editReason").toString();
        m_busy = false;
        emit pagesChanged();
    } else if (type == "edited" || type == "exported") {
        m_busy = false;
        m_error.clear();
        m_objects = message.value("objects").toArray().toVariantList();
        m_dirty = message.value("dirty").toBool();
        m_sourceNotice = message.value("sourceNotice").toString();
        m_canUndo = message.value("canUndo").toBool();
        m_canRedo = message.value("canRedo").toBool();
        if (type == "edited") {
            m_requests.clear();
            emit reset();
        }
        emit stateChanged();
        if (type == "edited")
            emit edited(message.value("selectedId").toInt(-1));
        return;
    } else if (type == "error") {
        const QString error = message.value("message").toString();
        const int request = message.value("requestId").toInt();
        if (request) {
            if (auto item = m_requests.take(request))
                item->acceptError(request, error);
            return;
        }
        m_error = error;
        m_busy = false;
        if (message.value("fatal").toBool()) {
            clearPages();
            m_fileName.clear();
        }
    }
    emit stateChanged();
}

void BrowserDocument::receivePixels(int request, int generation, int width, int height,
                                    const unsigned char *bytes)
{
    if (generation != m_generation)
        return;
    const auto item = m_requests.take(request);
    if (!item)
        return;
    if (!bytes || width < 1 || height < 1 || width > 4096 || height > 4096
        || qint64(width) * height > 4'000'000) {
        item->acceptError(request, tr("This page could not be rendered."));
        return;
    }
    item->acceptImage(request, QImage(bytes, width, height, width * 4,
                                    QImage::Format_RGBA8888).copy());
}
