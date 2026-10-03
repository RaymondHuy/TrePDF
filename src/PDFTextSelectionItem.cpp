#include "PDFTextSelectionItem.h"

#include "PDFDocument.h"
#include "PDFTextSelectionController.h"
#include "PDFiumDocument.h"
#include "PDFiumSynchronization.h"

#include <QCoreApplication>
#include <QCursor>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QRunnable>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QTextBoundaryFinder>
#include <QThreadPool>

#include <fpdf_annot.h>
#include <fpdf_text.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr int kMaximumCharacters = 50'000;
constexpr int kMaximumAnnotations = 4'096;
constexpr int kMappingExtent = 100'000;
constexpr unsigned long kCopyPermission = 1UL << 4;

bool cancelled(const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return cancellation && cancellation->load(std::memory_order_relaxed);
}

QThreadPool &selectionThreadPool()
{
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(30000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

QRectF normalizedRect(FPDF_PAGE page, const QSize &deviceSize,
                      double left, double bottom, double right, double top)
{
    if (!std::isfinite(left) || !std::isfinite(bottom)
        || !std::isfinite(right) || !std::isfinite(top)
        || left > right || bottom > top) {
        return {};
    }
    const std::array<QPointF, 4> corners{
        QPointF(left, bottom), QPointF(left, top),
        QPointF(right, bottom), QPointF(right, top)};
    qreal minimumX = std::numeric_limits<qreal>::max();
    qreal minimumY = minimumX;
    qreal maximumX = std::numeric_limits<qreal>::lowest();
    qreal maximumY = maximumX;
    for (const QPointF &corner : corners) {
        int x = 0;
        int y = 0;
        if (!FPDF_PageToDevice(page, 0, 0, deviceSize.width(),
                               deviceSize.height(), 0,
                               corner.x(), corner.y(), &x, &y)) {
            return {};
        }
        minimumX = qMin(minimumX, qreal(x) / deviceSize.width());
        maximumX = qMax(maximumX, qreal(x) / deviceSize.width());
        minimumY = qMin(minimumY, qreal(y) / deviceSize.height());
        maximumY = qMax(maximumY, qreal(y) / deviceSize.height());
    }
    return QRectF(QPointF(minimumX, minimumY), QPointF(maximumX, maximumY))
        .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
}

// These handles are destroyed before the caller's process lock is released.
struct PageHandles {
    FPDF_PAGE page = nullptr;
    FPDF_TEXTPAGE text = nullptr;
    ~PageHandles()
    {
        if (text) {
            FPDFText_ClosePage(text);
        }
        if (page) {
            FPDF_ClosePage(page);
        }
    }
};

} // namespace

PDFTextSelectionItem::PDFTextSelectionItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);
    m_dispatchRetry.setSingleShot(true);
    m_dispatchRetry.setInterval(20);
    connect(&m_dispatchRetry, &QTimer::timeout,
            this, &PDFTextSelectionItem::dispatchPendingWork);
    connect(this, &QQuickItem::widthChanged, this, &QQuickItem::update);
    connect(this, &QQuickItem::heightChanged, this, &QQuickItem::update);
}

PDFTextSelectionItem::~PDFTextSelectionItem()
{
    cancelWork();
    if (m_controller) {
        m_controller->release(this);
    }
}

PDFDocument *PDFTextSelectionItem::document() const { return m_document.data(); }
PDFTextSelectionController *PDFTextSelectionItem::controller() const
{
    return m_controller.data();
}
int PDFTextSelectionItem::page() const { return m_page; }
bool PDFTextSelectionItem::active() const { return m_active; }
bool PDFTextSelectionItem::ready() const { return m_data.ready; }
bool PDFTextSelectionItem::limited() const { return m_data.limited; }

void PDFTextSelectionItem::setDocument(PDFDocument *document)
{
    if (m_document == document) {
        return;
    }
    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
    }
    m_document = document;
    if (m_document) {
        connect(m_document, &PDFDocument::sourceChanged,
                this, &PDFTextSelectionItem::refresh);
        connect(m_document, &PDFDocument::pageStructureChanged,
                this, &PDFTextSelectionItem::refresh);
        connect(m_document, &QObject::destroyed,
                this, &PDFTextSelectionItem::refresh);
    }
    refresh();
    emit documentChanged();
}

void PDFTextSelectionItem::setController(PDFTextSelectionController *controller)
{
    if (m_controller == controller) {
        return;
    }
    if (m_controller) {
        m_controller->release(this);
        disconnect(m_controller, nullptr, this, nullptr);
    }
    clearLocalSelection();
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &QObject::destroyed,
                this, &PDFTextSelectionItem::clearLocalSelection);
    }
    emit controllerChanged();
}

void PDFTextSelectionItem::setPage(int page)
{
    if (m_page == page) {
        return;
    }
    m_page = page;
    refresh();
    emit pageChanged();
}

void PDFTextSelectionItem::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    setAcceptedMouseButtons(active ? Qt::LeftButton | Qt::RightButton : Qt::NoButton);
    setAcceptHoverEvents(active);
    refresh();
    emit activeChanged();
}

void PDFTextSelectionItem::cancelWork()
{
    m_dispatchRetry.stop();
    if (m_cancellation) {
        m_cancellation->store(true, std::memory_order_relaxed);
    }
    m_pendingWork.reset();
    m_cancellation.reset();
}

void PDFTextSelectionItem::refresh()
{
    cancelWork();
    if (m_controller) {
        m_controller->release(this);
    }
    clearLocalSelection();
    m_data = {};
    unsetCursor();
    emit dataChanged();
    if (!m_active || !m_document || !m_document->isLoaded()
        || !m_document->allowsTextCopy() || m_page < 0
        || m_page >= m_document->pageCount()) {
        return;
    }
    const int sourcePage = m_document->sourcePageIndex(m_page);
    if (sourcePage < 0) {
        m_data.ready = true;
        emit dataChanged();
        return;
    }
    m_cancellation = std::make_shared<std::atomic_bool>(false);
    m_pendingWork = std::make_shared<PendingWork>(PendingWork{
        m_document->renderFilePath(), m_document->documentPassword(),
        m_document->renderRevision(), m_page, sourcePage,
        m_document->pageSize(m_page), m_cancellation});
    dispatchPendingWork();
}

void PDFTextSelectionItem::dispatchPendingWork()
{
    m_dispatchRetry.stop();
    const std::shared_ptr<PendingWork> work = m_pendingWork;
    if (!work || cancelled(work->cancellation)) {
        m_pendingWork.reset();
        return;
    }
    const QPointer<PDFTextSelectionItem> item(this);
    QRunnable *runnable = QRunnable::create([item, work] {
        PageData result = extractPage(*work);
        QObject *app = QCoreApplication::instance();
        if (!app || cancelled(work->cancellation)) {
            return;
        }
        QMetaObject::invokeMethod(app,
            [item, work, result = std::move(result)]() mutable {
                if (!item || !item->m_active || !item->m_document
                    || !item->m_document->isLoaded()
                    || !item->m_document->allowsTextCopy()
                    || item->m_cancellation != work->cancellation
                    || cancelled(work->cancellation)
                    || item->m_page != work->displayPage
                    || item->m_document->sourcePageIndex(item->m_page) != work->sourcePage
                    || item->m_document->renderRevision() != work->revision
                    || item->m_document->renderFilePath() != work->filePath) {
                    return;
                }
                item->m_cancellation.reset();
                item->m_data = std::move(result);
                emit item->dataChanged();
            }, Qt::QueuedConnection);
    });
    if (!selectionThreadPool().tryStart(runnable)) {
        delete runnable;
        m_dispatchRetry.start();
        return;
    }
    if (m_pendingWork == work) {
        m_pendingWork.reset();
    }
}

PDFTextSelectionItem::PageData PDFTextSelectionItem::extractPage(const PendingWork &work)
{
    PageData result;
    if (cancelled(work.cancellation)) {
        return result;
    }
    PDFiumProcessLock lock;
    if (cancelled(work.cancellation)) {
        return result;
    }
    PDFiumDocument document = PDFiumDocument::open(work.filePath, work.password);
    if (!document || !(FPDF_GetDocPermissions(document) & kCopyPermission)) {
        return result;
    }
    const qreal maximumDimension = qMax(work.pageSize.width(), work.pageSize.height());
    if (!std::isfinite(maximumDimension) || maximumDimension <= 0.0
        || work.pageSize.width() <= 0.0 || work.pageSize.height() <= 0.0) {
        return result;
    }
    const qreal scale = kMappingExtent / maximumDimension;
    const QSize deviceSize(qMax(1, qRound(work.pageSize.width() * scale)),
                           qMax(1, qRound(work.pageSize.height() * scale)));
    PageHandles handles;
    handles.page = FPDF_LoadPage(document, work.sourcePage);
    if (!handles.page || cancelled(work.cancellation)) {
        return result;
    }
    const int annotationCount = FPDFPage_GetAnnotCount(handles.page);
    if (annotationCount < 0 || annotationCount > kMaximumAnnotations) {
        result.limited = true;
        return result;
    }
    for (int index = 0; index < annotationCount; ++index) {
        if (cancelled(work.cancellation)) {
            return {};
        }
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(handles.page, index);
        if (!annotation) {
            continue;
        }
        const auto subtype = FPDFAnnot_GetSubtype(annotation);
        const int flags = FPDFAnnot_GetFlags(annotation);
        FS_RECTF bounds{};
        if ((subtype == FPDF_ANNOT_WIDGET || subtype == FPDF_ANNOT_LINK)
            && !(flags & (FPDF_ANNOT_FLAG_INVISIBLE | FPDF_ANNOT_FLAG_HIDDEN
                          | FPDF_ANNOT_FLAG_NOVIEW))
            && FPDFAnnot_GetRect(annotation, &bounds)) {
            const QRectF rect = normalizedRect(handles.page, deviceSize,
                bounds.left, bounds.bottom, bounds.right, bounds.top);
            if (!rect.isEmpty()) {
                result.blockedRects.append(rect);
            }
        }
        FPDFPage_CloseAnnot(annotation);
    }
    handles.text = FPDFText_LoadPage(handles.page);
    if (!handles.text || cancelled(work.cancellation)) {
        return {};
    }
    const int count = FPDFText_CountChars(handles.text);
    if (count < 0 || count > kMaximumCharacters) {
        result.limited = true;
        return result;
    }
    result.characters.reserve(count);
    result.text.reserve(count);
    for (int index = 0; index < count; ++index) {
        if ((index & 255) == 0 && cancelled(work.cancellation)) {
            return {};
        }
        const char32_t unicode = FPDFText_GetUnicode(handles.text, index);
        if (unicode == 0 || unicode > 0x10ffff) {
            continue;
        }
        // GetUnicode retains scalar values outside the BMP; the older GetText
        // API only promises UCS-2. Keep PDFium's generated spaces and newlines.
        const QString character = unicode >= 0xd800 && unicode <= 0xdfff
            ? QString(QChar(ushort(unicode))) : QString::fromUcs4(&unicode, 1);
        FS_RECTF bounds{};
        QRectF rect;
        const bool hasBox = FPDFText_GetLooseCharBox(handles.text, index, &bounds);
        if (hasBox) {
            rect = normalizedRect(handles.page, deviceSize,
                bounds.left, bounds.bottom, bounds.right, bounds.top);
        }
        const bool whitespace = character.trimmed().isEmpty();
        // Keep combining marks and zero-width characters in the copied text
        // even when they have no hit-testable box. Only discard a real glyph
        // box that lies entirely outside the visible crop.
        if (!whitespace && hasBox && bounds.right > bounds.left
            && bounds.top > bounds.bottom && rect.isEmpty()) {
            continue;
        }
        const int offset = int(result.text.size());
        result.text.append(character);
        result.characters.append(Character{
            offset, int(character.size()), rect, !whitespace && !rect.isEmpty()});
    }
    result.ready = !cancelled(work.cancellation);
    return result;
}

bool PDFTextSelectionItem::canCopy() const
{
    return m_active && m_data.ready && m_document && m_document->isLoaded()
        && m_document->allowsTextCopy();
}

void PDFTextSelectionItem::clearLocalSelection()
{
    m_anchor = m_first = m_last = -1;
    m_dragging = false;
    setKeepMouseGrab(false);
    update();
}

void PDFTextSelectionItem::selectRange(int first, int last)
{
    if (!canCopy() || !m_controller || m_data.characters.isEmpty()) {
        return;
    }
    first = qBound(0, first, int(m_data.characters.size()) - 1);
    last = qBound(0, last, int(m_data.characters.size()) - 1);
    if (first > last) {
        std::swap(first, last);
    }
    // Character boxes can describe only the base of a combining sequence.
    // Expand both ends to whole graphemes, also protecting surrogate pairs.
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, m_data.text);
    finder.setPosition(m_data.characters.at(first).offset);
    const int startOffset = finder.isAtBoundary()
        ? finder.position() : finder.toPreviousBoundary();
    const Character &lastCharacter = m_data.characters.at(last);
    finder.setPosition(lastCharacter.offset + lastCharacter.length);
    const int endOffset = finder.isAtBoundary()
        ? finder.position() : finder.toNextBoundary();
    while (first > 0 && m_data.characters.at(first).offset > startOffset) {
        --first;
    }
    while (last + 1 < m_data.characters.size()
           && m_data.characters.at(last).offset + m_data.characters.at(last).length < endOffset) {
        ++last;
    }
    m_first = first;
    m_last = last;
    const Character &start = m_data.characters.at(first);
    const Character &end = m_data.characters.at(last);
    QString selected = m_data.text.mid(start.offset, end.offset + end.length - start.offset);
    selected.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    selected.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    m_controller->setSelection(this, selected);
    update();
}

QRectF PDFTextSelectionItem::itemRect(const QRectF &rect) const
{
    return QRectF(rect.x() * width(), rect.y() * height(),
                  rect.width() * width(), rect.height() * height());
}

bool PDFTextSelectionItem::blockedAt(const QPointF &position) const
{
    return std::any_of(m_data.blockedRects.cbegin(), m_data.blockedRects.cend(),
        [this, &position](const QRectF &rect) { return itemRect(rect).contains(position); });
}

int PDFTextSelectionItem::characterAt(const QPointF &position, bool nearest) const
{
    if (!canCopy() || width() <= 0.0 || height() <= 0.0
        || (!nearest && blockedAt(position))) {
        return -1;
    }
    qreal bestDistance = std::numeric_limits<qreal>::max();
    int best = -1;
    for (int index = 0; index < m_data.characters.size(); ++index) {
        const Character &character = m_data.characters.at(index);
        if (!character.hitTestable) {
            continue;
        }
        const QRectF rect = itemRect(character.rect);
        if (!nearest && !rect.adjusted(-1.5, -1.5, 1.5, 1.5).contains(position)) {
            continue;
        }
        const qreal dx = std::max({rect.left() - position.x(), qreal(0), position.x() - rect.right()});
        const qreal dy = std::max({rect.top() - position.y(), qreal(0), position.y() - rect.bottom()});
        // Give text on the same visual line precedence in the margins.
        const qreal centerDx = position.x() - rect.center().x();
        const qreal centerDy = position.y() - rect.center().y();
        const qreal distance = dx * dx + dy * dy * 4.0
            + (centerDx * centerDx + centerDy * centerDy) * 0.001;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = index;
        }
    }
    return best;
}

bool PDFTextSelectionItem::selectedAt(const QPointF &position) const
{
    if (m_first < 0 || m_last >= m_data.characters.size()) {
        return false;
    }
    for (int index = m_first; index <= m_last; ++index) {
        if (itemRect(m_data.characters.at(index).rect).contains(position)) {
            return true;
        }
    }
    return false;
}

bool PDFTextSelectionItem::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride && hasActiveFocus() && canCopy()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->matches(QKeySequence::Copy) && m_first >= 0)
            || key->matches(QKeySequence::SelectAll)
            || key->key() == Qt::Key_Escape) {
            event->accept();
            return true;
        }
    }
    return QQuickItem::event(event);
}

void PDFTextSelectionItem::hoverMoveEvent(QHoverEvent *event)
{
    if (characterAt(event->position()) >= 0) {
        setCursor(Qt::IBeamCursor);
    } else {
        unsetCursor();
    }
    event->ignore();
}

void PDFTextSelectionItem::hoverLeaveEvent(QHoverEvent *event)
{
    unsetCursor();
    QQuickItem::hoverLeaveEvent(event);
}

void PDFTextSelectionItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton && canCopy() && selectedAt(event->position())) {
        emit selectionStarted();
        forceActiveFocus(Qt::MouseFocusReason);
        emit contextMenuRequested(event->position());
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !m_controller) {
        event->ignore();
        return;
    }
    const int character = characterAt(event->position());
    if (character < 0) {
        m_controller->clear();
        event->ignore();
        return;
    }
    m_controller->clear();
    m_anchor = character;
    m_pressPosition = event->position();
    m_dragging = true;
    setKeepMouseGrab(true);
    emit selectionStarted();
    forceActiveFocus(Qt::MouseFocusReason);
    event->accept();
}

void PDFTextSelectionItem::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || m_anchor < 0) {
        event->ignore();
        return;
    }
    if (m_first < 0 && (event->position() - m_pressPosition).manhattanLength() < 3.0) {
        event->accept();
        return;
    }
    const int character = characterAt(event->position(), true);
    if (character >= 0) {
        selectRange(m_anchor, character);
    }
    event->accept();
}

void PDFTextSelectionItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        mouseMoveEvent(event);
        m_dragging = false;
        setKeepMouseGrab(false);
        event->accept();
        return;
    }
    event->ignore();
}

void PDFTextSelectionItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int characterIndex = characterAt(event->position());
    if (event->button() != Qt::LeftButton || characterIndex < 0 || !m_controller) {
        event->ignore();
        return;
    }
    const Character &character = m_data.characters.at(characterIndex);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, m_data.text);
    finder.setPosition(character.offset);
    const int start = finder.isAtBoundary() ? character.offset : finder.toPreviousBoundary();
    finder.setPosition(character.offset + character.length);
    const int end = finder.isAtBoundary()
        ? character.offset + character.length : finder.toNextBoundary();
    int first = characterIndex;
    int last = characterIndex;
    while (first > 0 && m_data.characters.at(first - 1).offset >= start) {
        --first;
    }
    while (last + 1 < m_data.characters.size()
           && m_data.characters.at(last + 1).offset < end) {
        ++last;
    }
    m_anchor = first;
    m_dragging = false;
    setKeepMouseGrab(false);
    emit selectionStarted();
    forceActiveFocus(Qt::MouseFocusReason);
    selectRange(first, last);
    event->accept();
}

void PDFTextSelectionItem::mouseUngrabEvent()
{
    m_dragging = false;
    setKeepMouseGrab(false);
    QQuickItem::mouseUngrabEvent();
}

void PDFTextSelectionItem::keyPressEvent(QKeyEvent *event)
{
    if (!canCopy() || !m_controller) {
        event->ignore();
        return;
    }
    if (event->matches(QKeySequence::Copy) && m_first >= 0) {
        if (m_controller->copy()) {
            event->accept();
            return;
        }
    } else if (event->matches(QKeySequence::SelectAll)) {
        selectRange(0, int(m_data.characters.size()) - 1);
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Escape) {
        m_controller->clear();
        event->accept();
        return;
    }
    event->ignore();
}

QSGNode *PDFTextSelectionItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (!m_active || m_first < 0 || m_last >= m_data.characters.size()
        || width() <= 0.0 || height() <= 0.0) {
        delete oldNode;
        return nullptr;
    }
    QVector<QRectF> rects;
    rects.reserve(m_last - m_first + 1);
    for (int index = m_first; index <= m_last; ++index) {
        const QRectF rect = itemRect(m_data.characters.at(index).rect);
        if (rect.isEmpty()) {
            continue;
        }
        if (!rects.isEmpty()) {
            QRectF &previous = rects.last();
            const qreal tolerance = qMin(previous.height(), rect.height()) * 0.25;
            if (std::abs(previous.top() - rect.top()) <= tolerance
                && std::abs(previous.bottom() - rect.bottom()) <= tolerance
                && rect.left() >= previous.left()
                && rect.left() - previous.right() <= rect.height() * 0.6) {
                previous = previous.united(rect);
                continue;
            }
        }
        rects.append(rect);
    }
    auto *node = static_cast<QSGGeometryNode *>(oldNode);
    if (!node) {
        node = new QSGGeometryNode;
        auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto *material = new QSGFlatColorMaterial;
        material->setColor(QColor(45, 130, 230, 85));
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    QSGGeometry *geometry = node->geometry();
    geometry->allocate(int(rects.size()) * 6);
    auto *vertices = geometry->vertexDataAsPoint2D();
    for (const QRectF &rect : std::as_const(rects)) {
        vertices[0].set(float(rect.left()), float(rect.top()));
        vertices[1].set(float(rect.right()), float(rect.top()));
        vertices[2].set(float(rect.left()), float(rect.bottom()));
        vertices[3].set(float(rect.left()), float(rect.bottom()));
        vertices[4].set(float(rect.right()), float(rect.top()));
        vertices[5].set(float(rect.right()), float(rect.bottom()));
        vertices += 6;
    }
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
