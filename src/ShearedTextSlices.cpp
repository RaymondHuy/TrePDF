#include "ShearedTextSlices.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QScopedValueRollback>
#include <QSGImageNode>
#include <QSGTextureProvider>
#include <QTextBlock>
#include <QTextLayout>
#include <algorithm>
#include <limits>

namespace {

struct GraphemeInterval {
    qreal left;
    qreal slope;
    int start;
    int end;
};

struct SlopeRun {
    qreal left;
    qreal slope;
    QRectF ink;
};

// One glyph-run request per mixed line. The reported positions already
// include shaping, kerning, fallback fonts and bidi reordering.
QVector<QRectF> lineInk(const QTextLine &line, const QPointF &origin)
{
    QVector<QRectF> ink(line.textLength());
    const auto runs = line.glyphRuns(-1, -1, QTextLayout::RetrieveGlyphIndexes
        | QTextLayout::RetrieveGlyphPositions | QTextLayout::RetrieveStringIndexes);
    for (const auto &run : runs) {
        const auto font = run.rawFont();
        const auto indexes = run.glyphIndexes();
        const auto positions = run.positions();
        const auto offsets = run.stringIndexes();
        for (qsizetype i = 0; i < offsets.size(); ++i) {
            const qsizetype offset = offsets.at(i) - line.textStart();
            if (offset >= 0 && offset < ink.size()) {
                ink[offset] |= font.boundingRect(indexes.at(i)).translated(origin + positions.at(i));
            }
        }
    }
    return ink;
}

// Keep a glyph's negative bearing or overhanging tip on its own side of a
// slope change whenever the neighboring runs have a clear vertical seam.
// Preserve the caret advance if the two runs actually share ink columns.
qreal paintBoundary(const SlopeRun &leftRun, const SlopeRun &rightRun,
                    qreal previous, qreal width)
{
    const qreal left = leftRun.ink.isEmpty() ? previous : qMax(previous, leftRun.ink.right());
    const qreal right = rightRun.ink.isEmpty() ? width : qMin(width, rightRun.ink.left());
    return left <= right ? qBound(left, rightRun.left, right) : qBound(previous, rightRun.left, width);
}

// The layer's texture belongs to Qt. These nodes and their signal connections
// live on the render thread; drop every borrowed texture before the provider dies.
class CaptureNode : public QObject, public QSGNode
{
public:
    CaptureNode(QQuickItem *owner, QSGTextureProvider *provider)
        : m_owner(owner), m_window(owner->window()), m_provider(provider)
    {
        setFlag(UsePreprocess);
        connect(provider, &QSGTextureProvider::textureChanged, this, [this] {
            m_changed = true;
            markDirty(DirtyMaterial);
            if (!firstChild())
                requestUpdate();
        }, Qt::DirectConnection);
        connect(provider, &QObject::destroyed, this, [this] {
            m_provider = nullptr;
            clear();
        }, Qt::DirectConnection);
    }

    QSGTextureProvider *provider() const { return m_provider; }

    void clear()
    {
        while (QSGNode *child = firstChild()) {
            removeChildNode(child);
            delete child;
        }
        m_segments.clear();
        m_updatePending = false;
    }

    void append(const QRectF &rect, qreal slope, qreal baseline, const QRectF &captureRect)
    {
        if (!rect.isEmpty() && !captureRect.isEmpty())
            m_segments.append({rect, slope, baseline, captureRect});
    }

    void populate()
    {
        QSGTexture *texture = m_provider ? m_provider->texture() : nullptr;
        if (!texture)
            return;
        // Create children during scene graph synchronization. Adding them in
        // preprocess is too late for the software renderer to propagate the
        // item's ancestor transforms, and places the text at the window origin.
        // A layer texture may initially have an empty size, but must be nonnull
        // before a software image node enters the scene graph.
        for (const auto &segment : std::as_const(m_segments)) {
            auto *transform = new QSGTransformNode;
            QMatrix4x4 matrix;
            matrix(0, 1) = -segment.slope;
            matrix(0, 3) = segment.slope * segment.baseline;
            transform->setMatrix(matrix);
            auto *image = m_window->createImageNode();
            image->setFiltering(QSGTexture::Linear);
            image->setOwnsTexture(false);
            image->setTexture(texture);
            image->setRect(segment.rect);
            transform->appendChildNode(image);
            appendChildNode(transform);
        }
    }

    void preprocess() override
    {
        QSGTexture *texture = m_provider ? m_provider->texture() : nullptr;
        if (!texture)
            return;
        if (auto *dynamic = qobject_cast<QSGDynamicTexture *>(texture))
            dynamic->updateTexture();
        // Some backends provide their first texture after updatePaintNode.
        // Retain the segments and request another synchronization instead of
        // either losing the first frame permanently or inserting unsafe nodes.
        if (!firstChild()) {
            requestUpdate();
            return;
        }
        const QSize pixels = texture->textureSize();
        qsizetype index = 0;
        for (QSGNode *child = firstChild(); child; child = child->nextSibling(), ++index) {
            auto *image = static_cast<QSGImageNode *>(child->firstChild());
            const auto &segment = m_segments.at(index);
            const auto &r = segment.rect;
            const auto &capture = segment.capture;
            image->setTexture(texture);
            image->setSourceRect((r.x() - capture.x()) / capture.width() * pixels.width(),
                                 (r.y() - capture.y()) / capture.height() * pixels.height(),
                                 r.width() / capture.width() * pixels.width(),
                                 r.height() / capture.height() * pixels.height());
            if (m_changed)
                image->markDirty(DirtyMaterial);
        }
        m_changed = false;
    }

private:
    void requestUpdate()
    {
        if (!m_updatePending && m_owner && !m_segments.isEmpty()) {
            m_updatePending = true;
            QMetaObject::invokeMethod(m_owner, "update", Qt::QueuedConnection);
        }
    }

    QPointer<QQuickItem> m_owner;
    QQuickWindow *m_window;
    QSGTextureProvider *m_provider;
    struct Segment {
        QRectF rect;
        qreal slope;
        qreal baseline;
        QRectF capture;
    };
    QVector<Segment> m_segments;
    bool m_changed = true;
    bool m_updatePending = false;
};
} // namespace

ShearedTextSlices::ShearedTextSlices(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *view) {
        if (m_windowConnection)
            disconnect(m_windowConnection);
        if (view) {
            // Scrolling can move any ancestor without changing this item's
            // local geometry. afterAnimating runs on the GUI thread before
            // scene graph synchronization, so capture changes remain safe.
            m_windowConnection = connect(view, &QQuickWindow::afterAnimating,
                                          this, &ShearedTextSlices::refreshCapture);
        }
        refreshCapture();
    });
}

void ShearedTextSlices::setEditor(QQuickItem *editor)
{
    if (m_editor == editor)
        return;
    if (m_editor)
        m_editor->removeEventFilter(this);
    if (m_quickDocument)
        disconnect(m_quickDocument, nullptr, this, nullptr);
    m_editor = editor;
    m_quickDocument = editor
        ? editor->property("textDocument").value<QQuickTextDocument *>() : nullptr;
    if (m_editor)
        m_editor->installEventFilter(this);
    if (m_quickDocument)
        connect(m_quickDocument, &QQuickTextDocument::textDocumentChanged,
                this, &ShearedTextSlices::attachDocument);
    attachDocument();
    emit editorChanged();
}

void ShearedTextSlices::attachDocument()
{
    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
        disconnect(m_document->documentLayout(), nullptr, this, nullptr);
    }
    m_document = m_quickDocument ? m_quickDocument->textDocument() : nullptr;
    if (m_document) {
        // Geometry is sampled after layout on the GUI thread, never by render nodes.
        connect(m_document, &QTextDocument::contentsChanged, this, &QQuickItem::polish);
        connect(m_document->documentLayout(), &QAbstractTextDocumentLayout::update,
                this, &QQuickItem::polish);
    }
    polish();
}

void ShearedTextSlices::setCapture(QQuickItem *capture)
{
    if (m_capture == capture)
        return;
    m_capture = capture;
    update();
    emit captureChanged();
}

void ShearedTextSlices::setCharacterSpans(const QVariantList &spans)
{
    if (m_spans == spans)
        return;
    m_spans = spans;
    polish();
    emit characterSpansChanged();
}

void ShearedTextSlices::geometryChange(const QRectF &geometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(geometry, oldGeometry);
    if (geometry.size() != oldGeometry.size())
        polish();
}

void ShearedTextSlices::updatePolish()
{
    m_slices.clear();
    update();
    if (!m_editor || !m_document || width() <= 0 || height() <= 0)
        return;

    // The editor admission limit is 32768 UTF-16 units. A saved logical frame
    // can still be larger, so size the style table only up to the visible model.
    const int count = m_document->characterCount();
    if (count < 1 || count > 32769)
        return;
    QVector<qreal> slopes(count, 0.0);
    for (const QVariant &value : std::as_const(m_spans)) {
        const QVariantMap span = value.toMap();
        const int start = qBound(0, span.value(QStringLiteral("start")).toInt(), count);
        const int end = qBound(start, span.value(QStringLiteral("end")).toInt(), count);
        const qreal slope = span.value(QStringLiteral("textShear")).toReal();
        if (qIsFinite(slope) && slope != 0.0 && qAbs(slope) <= 0.5)
            std::fill(slopes.begin() + start, slopes.begin() + end, slope);
    }
    const QRectF bounds = boundingRect();
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        const auto *layout = block.layout();
        if (!layout || !layout->lineCount())
            continue;
        // positionToRectangle accounts for native padding and internal scroll.
        // Each block can have independent margins, indent and alignment.
        QRectF caret;
        if (!QMetaObject::invokeMethod(m_editor, "positionToRectangle", Qt::DirectConnection,
                                      Q_RETURN_ARG(QRectF, caret), Q_ARG(int, block.position())))
            return;
        const auto first = layout->lineAt(0);
        const QPointF origin(caret.left() - first.cursorToX(0), caret.top() - first.y());
        const int preedit = layout->preeditAreaPosition();
        const int preeditLength = layout->preeditAreaText().size();
        for (int i = 0; i < layout->lineCount(); ++i) {
            const auto line = layout->lineAt(i);
            const qreal top = origin.y() + line.y();
            const qreal bottom = top + line.height();
            if (bottom <= 0 || top >= bounds.bottom())
                continue;

            // cursorToX follows visual order even when part of a line is RTL.
            // Keep graphemes intact at the boundaries of their style runs.
            QVector<GraphemeInterval> intervals;
            const int end = line.textStart() + line.textLength();
            int pos = line.textStart();
            while (pos < end) {
                int docOffset = pos;
                if (preedit >= 0 && pos >= preedit)
                    docOffset = pos < preedit + preeditLength ? qMax(0, preedit - 1)
                                                              : pos - preeditLength;
                const qreal slope = slopes.at(qBound(0, block.position() + docOffset, count - 1));
                const qreal x1 = origin.x() + line.cursorToX(pos, QTextLine::Leading);
                const qreal x2 = origin.x() + line.cursorToX(pos, QTextLine::Trailing);
                const int next = std::max(pos + 1, layout->nextCursorPosition(pos));
                intervals.append({std::min(x1, x2), slope, pos, next});
                pos = next;
            }
            std::sort(intervals.begin(), intervals.end(),
                      [](const auto &left, const auto &right) { return left.left < right.left; });
            const qreal baseline = top + line.ascent();
            if (intervals.isEmpty()) {
                const int offset = qBound(0, block.position() - 1, count - 1);
                const QRectF rect = QRectF(0, top, width(), bottom - top).intersected(bounds);
                m_slices.append({rect, rect, slopes.at(offset), baseline});
                continue;
            }
            const bool mixed = std::ranges::any_of(intervals, [&](const auto &part) {
                return part.slope != intervals.first().slope;
            });
            const auto ink = mixed ? lineInk(line, origin) : QVector<QRectF>{};
            QVector<SlopeRun> runs;
            for (const auto &part : std::as_const(intervals)) {
                if (runs.isEmpty() || runs.last().slope != part.slope)
                    runs.append({part.left, part.slope, {}});
                for (int at = part.start - line.textStart();
                     at < part.end - line.textStart() && at < ink.size(); ++at)
                    runs.last().ink |= ink.at(at);
            }
            qreal left = bounds.left();
            qreal paintLeft = left;
            for (qsizetype j = 0; j < runs.size(); ++j) {
                const bool last = j + 1 == runs.size();
                const qreal right = last ? width() : qBound(left, runs.at(j + 1).left, width());
                const qreal paintRight = last ? width()
                    : paintBoundary(runs.at(j), runs.at(j + 1), paintLeft, width());
                const QRectF rect = QRectF(left, top, right - left, bottom - top).intersected(bounds);
                const QRectF paint = QRectF(paintLeft, top, paintRight - paintLeft, bottom - top)
                    .intersected(bounds);
                if (!rect.isEmpty())
                    m_slices.append({rect, paint, runs.at(j).slope, baseline});
                left = right;
                paintLeft = paintRight;
            }
        }
    }
    refreshCapture();
}

void ShearedTextSlices::refreshCapture()
{
    if (!window())
        return;
    QRectF visible = mapRectFromScene(QRectF(QPointF(), window()->size()));
    for (auto *ancestor = parentItem(); ancestor; ancestor = ancestor->parentItem()) {
        if (ancestor->clip())
            visible = visible.intersected(mapRectFromItem(ancestor, ancestor->boundingRect()));
    }
    // Ink is moved horizontally about each line's baseline. Include source
    // pixels just outside the clip that can move into the visible region.
    qreal bleed = 0.0;
    for (const auto &slice : std::as_const(m_slices))
        bleed = qMax(bleed, qAbs(slice.slope) * slice.rect.height());
    visible = visible.adjusted(-bleed, 0, bleed, 0).intersected(boundingRect());
    const QPointF origin = mapToScene(QPointF());
    const qreal density = window()->effectiveDevicePixelRatio()
        * qMax(QLineF(origin, mapToScene(QPointF(1, 0))).length(),
               QLineF(origin, mapToScene(QPointF(0, 1))).length());
    const QSize pixels(qMax(1, qCeil(visible.width() * density)),
                        qMax(1, qCeil(visible.height() * density)));
    if (visible == m_captureRect && pixels == m_captureSize)
        return;
    m_captureRect = visible;
    m_captureSize = pixels;
    emit captureGeometryChanged();
    update();
}

QPointF ShearedTextSlices::unmapPoint(const QPointF &point) const
{
    QPointF best = point;
    qreal nearest = std::numeric_limits<qreal>::infinity();
    // At a change of slope the original runs may overlap or separate slightly
    // above the baseline. In a gap, choose the nearest visible run edge.
    for (const Slice &slice : m_slices) {
        if (point.y() < slice.rect.top() || point.y() >= slice.rect.bottom())
            continue;
        const qreal x = point.x() + slice.slope * (point.y() - slice.baseline);
        const qreal distance = std::max({slice.rect.left() - x, x - slice.rect.right(), 0.0});
        if (distance <= nearest) {
            nearest = distance;
            best.setX(qBound(slice.rect.left(), x, slice.rect.right()));
        }
    }
    return best;
}

bool ShearedTextSlices::eventFilter(QObject *object, QEvent *event)
{
    if (m_forwarding || object != m_editor || !isVisible())
        return false;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
        break;
    default:
        return false;
    }
    auto *mouse = static_cast<QMouseEvent *>(event);
    const QPointF local = unmapPoint(mouse->position());
    if (local == mouse->position())
        return false;
    QMouseEvent mapped(mouse->type(), local, m_editor->mapToScene(local), mouse->globalPosition(),
                       mouse->button(), mouse->buttons(), mouse->modifiers(), mouse->pointingDevice());
    mapped.setTimestamp(mouse->timestamp());
    QScopedValueRollback guard(m_forwarding, true);
    QCoreApplication::sendEvent(object, &mapped);
    mouse->setAccepted(mapped.isAccepted());
    return true;
}

QSGNode *ShearedTextSlices::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<CaptureNode *>(old);
    auto *provider = m_capture && m_capture->isTextureProvider() ? m_capture->textureProvider() : nullptr;
    if (!provider || m_slices.isEmpty()) {
        delete node;
        return nullptr;
    }
    if (!node || node->provider() != provider) {
        delete node;
        node = new CaptureNode(this, provider);
    } else {
        node->clear();
    }
    for (const auto &slice : std::as_const(m_slices))
        node->append(slice.paintRect.intersected(m_captureRect), slice.slope, slice.baseline, m_captureRect);
    node->populate();
    return node;
}
