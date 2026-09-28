#include "PDFSearchController.h"

#include "PDFDocument.h"
#include "PDFiumSynchronization.h"
#include "PDFiumDocument.h"

#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QPointF>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QVariantMap>
#include <QtGlobal>

#include <fpdf_text.h>

#include <array>
#include <cmath>
#include <utility>

namespace {

constexpr qsizetype kMaximumQueryLength = 256;
constexpr int kMaximumPageCharacters = 2'000'000;
constexpr qsizetype kMaximumMatches = 20'000;
constexpr qsizetype kMaximumRectangles = 100'000;
constexpr int kMaximumRectanglesPerMatch = 256;
constexpr qsizetype kMaximumVisibleRectanglesPerPage = 512;
constexpr qreal kCoordinateMapExtent = 10000.0;
constexpr int kSearchDelayMilliseconds = 300;
constexpr int kSearchDispatchRetryMilliseconds = 20;

QString normalizedSearchQuery(const QString &query)
{
    QString normalized = query.trimmed();
    if (normalized.size() <= kMaximumQueryLength) {
        return normalized;
    }

    normalized.truncate(kMaximumQueryLength);
    if (!normalized.isEmpty() && normalized.back().isHighSurrogate()) {
        normalized.chop(1);
    }
    return normalized;
}

bool cancelled(const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return cancellation
        && cancellation->load(std::memory_order_relaxed);
}

QThreadPool &searchThreadPool()
{
    static QThreadPool pool;
    static const bool configured = []() {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

class SearchDocument final
{
public:
    SearchDocument(
        const QString &filePath,
        const QByteArray &password,
        const std::shared_ptr<std::atomic_bool> &cancellation)
    {
        PDFiumProcessLock pdfiumLock;
        if (cancelled(cancellation)) {
            return;
        }
        m_document = PDFiumDocument::open(filePath, password);
    }

    ~SearchDocument()
    {
        if (!m_document) {
            return;
        }
        PDFiumProcessLock pdfiumLock;
        m_document.reset();
    }

    FPDF_DOCUMENT get() const
    {
        return m_document;
    }

    Q_DISABLE_COPY_MOVE(SearchDocument)

private:
    PDFiumDocument m_document;
};

QSize mappingDeviceSize(const QSizeF &pageSize)
{
    const qreal maximumDimension = qMax(pageSize.width(), pageSize.height());
    if (!std::isfinite(maximumDimension) || maximumDimension <= 0.0) {
        return {};
    }

    const qreal scale = kCoordinateMapExtent / maximumDimension;
    return QSize(qMax(1, qRound(pageSize.width() * scale)),
                 qMax(1, qRound(pageSize.height() * scale)));
}

bool pagePointToDevice(FPDF_PAGE page,
                       const QSize &deviceSize,
                       const QPointF &pagePoint,
                       QPointF &devicePoint)
{
    int deviceX = 0;
    int deviceY = 0;
    if (!FPDF_PageToDevice(page,
                           0,
                           0,
                           deviceSize.width(),
                           deviceSize.height(),
                           0,
                           pagePoint.x(),
                           pagePoint.y(),
                           &deviceX,
                           &deviceY)) {
        return false;
    }

    devicePoint = QPointF(deviceX, deviceY);
    return true;
}

QRectF normalizedSearchRect(FPDF_PAGE page,
                            const QSizeF &pageSize,
                            double left,
                            double top,
                            double right,
                            double bottom)
{
    const QSize deviceSize = mappingDeviceSize(pageSize);
    if (!page || deviceSize.isEmpty()) {
        return {};
    }

    const std::array<QPointF, 4> pagePoints{
        QPointF(left, bottom),
        QPointF(left, top),
        QPointF(right, bottom),
        QPointF(right, top)};
    std::array<QPointF, 4> devicePoints;
    for (size_t index = 0; index < pagePoints.size(); ++index) {
        if (!pagePointToDevice(
                page,
                deviceSize,
                pagePoints.at(index),
                devicePoints.at(index))) {
            return {};
        }
    }

    qreal minimumX = devicePoints.front().x();
    qreal maximumX = minimumX;
    qreal minimumY = devicePoints.front().y();
    qreal maximumY = minimumY;
    for (const QPointF &point : std::as_const(devicePoints)) {
        minimumX = qMin(minimumX, point.x());
        maximumX = qMax(maximumX, point.x());
        minimumY = qMin(minimumY, point.y());
        maximumY = qMax(maximumY, point.y());
    }

    const QRectF normalized(
        minimumX / deviceSize.width(),
        minimumY / deviceSize.height(),
        (maximumX - minimumX) / deviceSize.width(),
        (maximumY - minimumY) / deviceSize.height());
    return normalized.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

} // namespace

PDFSearchController::PDFSearchController(QObject *parent)
    : QObject(parent)
{
    m_searchDelay.setSingleShot(true);
    m_searchDelay.setInterval(kSearchDelayMilliseconds);
    connect(
        &m_searchDelay,
        &QTimer::timeout,
        this,
        &PDFSearchController::startPreparedSearch);
    m_dispatchRetry.setSingleShot(true);
    m_dispatchRetry.setInterval(kSearchDispatchRetryMilliseconds);
    connect(
        &m_dispatchRetry,
        &QTimer::timeout,
        this,
        &PDFSearchController::dispatchPendingWork);
}

PDFSearchController::~PDFSearchController()
{
    cancelPendingSearch();
}

PDFDocument *PDFSearchController::document() const
{
    return m_document.data();
}

QString PDFSearchController::query() const
{
    return m_query;
}

PDFSearchController::SearchState PDFSearchController::state() const
{
    return m_state;
}

int PDFSearchController::resultCount() const
{
    return static_cast<int>(m_matches.size());
}

int PDFSearchController::currentResultIndex() const
{
    return m_currentResultIndex;
}

quint64 PDFSearchController::resultsRevision() const
{
    return m_resultsRevision;
}

void PDFSearchController::setDocument(PDFDocument *document)
{
    if (m_document == document) {
        return;
    }

    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
    }
    clearSearch();
    m_document = document;
    if (m_document) {
        connect(
            m_document,
            &PDFDocument::sourceChanged,
            this,
            &PDFSearchController::clearSearch);
        connect(
            m_document,
            &PDFDocument::pageStructureChanged,
            this,
            [this](bool) {
                clearSearch();
            });
        connect(
            m_document,
            &QObject::destroyed,
            this,
            [this]() {
                m_document = nullptr;
                clearSearch();
                emit documentChanged();
            });
    }
    emit documentChanged();
}

void PDFSearchController::prepareSearchText(const QString &query)
{
    const QString normalizedQuery = normalizedSearchQuery(query);
    if (normalizedQuery.isEmpty()) {
        clearSearch();
        return;
    }

    m_searchDelay.stop();
    prepareSearch(normalizedQuery);
}

void PDFSearchController::scheduleSearch(const QString &query)
{
    prepareSearchText(query);
    if (m_query.isEmpty()) {
        return;
    }
    m_searchDelay.start();
}

void PDFSearchController::search(const QString &query)
{
    const QString normalizedQuery = normalizedSearchQuery(query);
    if (normalizedQuery.isEmpty()) {
        clearSearch();
        return;
    }

    const bool delayedSearchPending = m_searchDelay.isActive();
    m_searchDelay.stop();
    if (normalizedQuery == m_query && !delayedSearchPending
        && ((m_state == Searching && m_cancellation)
            || m_state == Ready
            || m_state == Limited)) {
        if (m_currentResultIndex >= 0
            && m_currentResultIndex < m_matches.size()) {
            activateResult(m_currentResultIndex);
        }
        return;
    }

    if (!delayedSearchPending || normalizedQuery != m_query) {
        prepareSearch(normalizedQuery);
    }
    startPreparedSearch();
}

void PDFSearchController::prepareSearch(const QString &query)
{
    cancelPendingSearch();
    if (m_query != query) {
        m_query = query;
        emit queryChanged();
    }
    resetResults();
    setState(Searching);
}

void PDFSearchController::startPreparedSearch()
{
    m_searchDelay.stop();
    if (m_query.isEmpty()) {
        setState(Idle);
        return;
    }
    if (!m_document || !m_document->isLoaded()) {
        setState(Failed);
        return;
    }

    const QString filePath = m_document->renderFilePath();
    const int revision = m_document->renderRevision();
    if (filePath.isEmpty()) {
        setState(Failed);
        return;
    }

    QVector<PageRequest> requests;
    requests.reserve(m_document->pageCount());
    for (int page = 0; page < m_document->pageCount(); ++page) {
        const int sourcePage = m_document->sourcePageIndex(page);
        const QSizeF pageSize = m_document->pageSize(page);
        if (sourcePage < 0) {
            continue;
        }
        requests.append(PageRequest{page, sourcePage, pageSize});
    }

    if (requests.isEmpty()) {
        setState(Ready);
        return;
    }

    m_cancellation = std::make_shared<std::atomic_bool>(false);
    m_pendingWork = std::make_shared<PendingWork>(PendingWork{
        filePath,
        m_document->documentPassword(),
        revision,
        m_query,
        std::move(requests),
        m_cancellation});
    dispatchPendingWork();
}

void PDFSearchController::dispatchPendingWork()
{
    m_dispatchRetry.stop();
    const std::shared_ptr<PendingWork> work = m_pendingWork;
    if (!work || cancelled(work->cancellation)) {
        m_pendingWork.reset();
        return;
    }

    const QPointer<PDFSearchController> controller(this);
    QRunnable *runnable = QRunnable::create(
        [controller, work]() mutable {
            WorkResult result = performSearch(
                work->filePath,
                work->password,
                work->query,
                work->requests,
                work->cancellation);
            QObject *app = QCoreApplication::instance();
            if (!app) {
                return;
            }

            QMetaObject::invokeMethod(
                app,
                [controller,
                 work,
                 result = std::move(result)]() mutable {
                    if (!controller
                        || controller->m_cancellation != work->cancellation
                        || cancelled(work->cancellation)
                        || !controller->m_document
                        || !controller->m_document->isLoaded()
                        || controller->m_document->renderFilePath()
                            != work->filePath
                        || controller->m_document->renderRevision()
                            != work->revision
                        || controller->m_query != work->query) {
                        return;
                    }

                    controller->m_cancellation.reset();
                    controller->m_matches = std::move(result.matches);
                    controller->m_matchIndicesByPage.clear();
                    for (int index = 0;
                         index < controller->m_matches.size();
                         ++index) {
                        controller->m_matchIndicesByPage[
                            controller->m_matches.at(index).page]
                            .append(index);
                    }
                    controller->m_currentResultIndex
                        = controller->m_matches.isEmpty() ? -1 : 0;
                    ++controller->m_resultsRevision;
                    emit controller->resultsChanged();
                    controller->setState(
                        result.failed
                            ? Failed
                            : result.limited
                                ? Limited
                                : Ready);
                    if (controller->m_currentResultIndex >= 0) {
                        controller->activateResult(
                            controller->m_currentResultIndex);
                    }
                },
                Qt::QueuedConnection);
        });
    if (!searchThreadPool().tryStart(runnable)) {
        delete runnable;
        if (m_pendingWork == work && !cancelled(work->cancellation)) {
            m_dispatchRetry.start();
        }
        return;
    }
    if (m_pendingWork == work) {
        m_pendingWork.reset();
    }
}

void PDFSearchController::clearSearch()
{
    m_searchDelay.stop();
    cancelPendingSearch();
    if (!m_query.isEmpty()) {
        m_query.clear();
        emit queryChanged();
    }
    resetResults();
    setState(Idle);
}

int PDFSearchController::nextResult()
{
    if (m_matches.isEmpty()) {
        return -1;
    }
    return activateResult(
        m_currentResultIndex < 0
            ? 0
            : (m_currentResultIndex + 1) % m_matches.size());
}

int PDFSearchController::previousResult()
{
    if (m_matches.isEmpty()) {
        return -1;
    }
    return activateResult(
        m_currentResultIndex <= 0
            ? static_cast<int>(m_matches.size()) - 1
            : m_currentResultIndex - 1);
}

QVariantList PDFSearchController::highlightsForPage(int page) const
{
    QVariantList highlights;
    const QVector<int> matchIndices = m_matchIndicesByPage.value(page);
    const auto appendMatch = [this, &highlights](int matchIndex,
                                                  qsizetype limit) {
        if (matchIndex < 0 || matchIndex >= m_matches.size()) {
            return;
        }
        const SearchMatch &match = m_matches.at(matchIndex);
        for (const QRectF &rect : match.rects) {
            if (highlights.size() >= limit) {
                return;
            }
            QVariantMap map;
            map.insert(QStringLiteral("x"), rect.x());
            map.insert(QStringLiteral("y"), rect.y());
            map.insert(QStringLiteral("width"), rect.width());
            map.insert(QStringLiteral("height"), rect.height());
            map.insert(
                QStringLiteral("current"),
                matchIndex == m_currentResultIndex);
            highlights.append(map);
        }
    };

    qsizetype ordinaryLimit = kMaximumVisibleRectanglesPerPage;
    const bool currentOnPage = matchIndices.contains(m_currentResultIndex);
    if (currentOnPage) {
        ordinaryLimit -= qMin(
            kMaximumVisibleRectanglesPerPage,
            m_matches.at(m_currentResultIndex).rects.size());
    }
    for (int matchIndex : matchIndices) {
        if (matchIndex == m_currentResultIndex) {
            continue;
        }
        if (highlights.size() >= ordinaryLimit) {
            break;
        }
        appendMatch(matchIndex, ordinaryLimit);
    }
    if (currentOnPage) {
        appendMatch(
            m_currentResultIndex,
            kMaximumVisibleRectanglesPerPage);
    }
    return highlights;
}

PDFSearchController::WorkResult PDFSearchController::performSearch(
    const QString &filePath,
    const QByteArray &password,
    const QString &query,
    const QVector<PageRequest> &requests,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    WorkResult result;
    if (cancelled(cancellation)) {
        return result;
    }
    SearchDocument document(filePath, password, cancellation);
    if (cancelled(cancellation)) {
        return result;
    }
    if (!document.get()) {
        result.failed = true;
        return result;
    }

    qsizetype rectangleCount = 0;
    for (const PageRequest &request : requests) {
        if (cancelled(cancellation)) {
            return result;
        }

        PDFiumProcessLock pdfiumLock;
        if (cancelled(cancellation)) {
            return result;
        }
        if (request.pageSize.isEmpty()) {
            result.limited = true;
        }
        FPDF_PAGE page = FPDF_LoadPage(document.get(), request.sourcePage);
        FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
        if (!textPage) {
            if (page) {
                FPDF_ClosePage(page);
            }
            result.limited = true;
            continue;
        }

        const int characterCount = FPDFText_CountChars(textPage);
        if (characterCount < 0 || characterCount > kMaximumPageCharacters) {
            result.limited = true;
        } else {
            FPDF_SCHHANDLE searchHandle = FPDFText_FindStart(
                textPage,
                reinterpret_cast<const FPDF_WCHAR *>(query.utf16()),
                0,
                0);
            while (searchHandle
                   && !cancelled(cancellation)
                   && FPDFText_FindNext(searchHandle)) {
                if (result.matches.size() >= kMaximumMatches
                    || rectangleCount >= kMaximumRectangles) {
                    result.limited = true;
                    break;
                }

                const int startIndex = FPDFText_GetSchResultIndex(searchHandle);
                const int matchedCharacters = FPDFText_GetSchCount(searchHandle);
                if (startIndex < 0 || matchedCharacters <= 0) {
                    result.limited = true;
                    break;
                }

                SearchMatch match;
                match.page = request.displayPage;
                const int availableRectangles = FPDFText_CountRects(
                    textPage,
                    startIndex,
                    matchedCharacters);
                const int retainedRectangles = qBound(
                    0,
                    availableRectangles,
                    kMaximumRectanglesPerMatch);
                match.rects.reserve(retainedRectangles);
                for (int rectIndex = 0;
                     rectIndex < retainedRectangles
                     && rectangleCount < kMaximumRectangles;
                     ++rectIndex) {
                    double left = 0.0;
                    double top = 0.0;
                    double right = 0.0;
                    double bottom = 0.0;
                    if (!FPDFText_GetRect(
                            textPage,
                            rectIndex,
                            &left,
                            &top,
                            &right,
                            &bottom)) {
                        continue;
                    }
                    const QRectF rect = normalizedSearchRect(
                        page,
                        request.pageSize,
                        left,
                        top,
                        right,
                        bottom);
                    if (rect.isEmpty()
                        || !std::isfinite(rect.x())
                        || !std::isfinite(rect.y())
                        || !std::isfinite(rect.width())
                        || !std::isfinite(rect.height())) {
                        continue;
                    }
                    match.rects.append(rect);
                    ++rectangleCount;
                }
                if (rectangleCount >= kMaximumRectangles) {
                    result.limited = true;
                }
                result.matches.append(std::move(match));
            }
            if (searchHandle) {
                FPDFText_FindClose(searchHandle);
            }
        }

        FPDFText_ClosePage(textPage);
        FPDF_ClosePage(page);
        if (result.limited
            && (result.matches.size() >= kMaximumMatches
                || rectangleCount >= kMaximumRectangles)) {
            break;
        }
    }
    return result;
}

void PDFSearchController::cancelPendingSearch()
{
    m_dispatchRetry.stop();
    m_pendingWork.reset();
    if (m_cancellation) {
        m_cancellation->store(true, std::memory_order_relaxed);
        m_cancellation.reset();
    }
}

void PDFSearchController::resetResults()
{
    if (m_matches.isEmpty()
        && m_matchIndicesByPage.isEmpty()
        && m_currentResultIndex < 0) {
        return;
    }

    m_matches.clear();
    m_matchIndicesByPage.clear();
    m_currentResultIndex = -1;
    ++m_resultsRevision;
    emit resultsChanged();
}

void PDFSearchController::setState(SearchState state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

int PDFSearchController::activateResult(int index)
{
    if (index < 0 || index >= m_matches.size()) {
        return -1;
    }

    if (m_currentResultIndex != index) {
        m_currentResultIndex = index;
        ++m_resultsRevision;
        emit resultsChanged();
    }
    const int page = m_matches.at(index).page;
    QPointF activationPoint(0.5, 0.0);
    if (!m_matches.at(index).rects.isEmpty()) {
        activationPoint = m_matches.at(index).rects.constFirst().center();
    }
    emit resultActivated(
        page,
        qBound<qreal>(0.0, activationPoint.x(), 1.0),
        qBound<qreal>(0.0, activationPoint.y(), 1.0),
        m_resultsRevision);
    return page;
}
