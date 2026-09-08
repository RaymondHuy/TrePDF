#include "PDFWidgetAnnotationSuppression.h"

#include "PDFiumSynchronization.h"

#include <cstring>
#include <limits>
#include <utility>

#include <QSet>

#include <fpdf_annot.h>

namespace {

// FPDF_FFLDraw may paint the focused-widget dotted outline outside the
// annotation rectangle. Capture that device-pixel fringe as well so restoring
// the background does not leave a stale focus box behind.
constexpr int kWidgetInteractionAdornmentMarginPixels = 4;

QString annotationStringValue(FPDF_ANNOTATION annotation,
                              const char *key)
{
    const unsigned long byteCount = FPDFAnnot_GetStringValue(
        annotation,
        key,
        nullptr,
        0);
    if (byteCount < sizeof(FPDF_WCHAR)
        || byteCount % sizeof(FPDF_WCHAR) != 0) {
        return {};
    }

    QVector<FPDF_WCHAR> buffer(
        static_cast<qsizetype>(byteCount / sizeof(FPDF_WCHAR)));
    if (FPDFAnnot_GetStringValue(
            annotation,
            key,
            buffer.data(),
            byteCount) != byteCount) {
        return {};
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        buffer.size() - 1);
}

} // namespace

bool capturePDFWidgetRenderPatches(
    FPDF_PAGE page,
    const QSize &pixelSize,
    const QVector<QRectF> &pageRects,
    const QImage &image,
    QVector<PDFWidgetRenderPatch> &patches,
    const QPoint &bitmapOrigin)
{
    assertPDFiumProcessLockHeld();
    patches.clear();
    if (!page || pixelSize.isEmpty()
        || image.isNull() || image.depth() != 32) {
        return false;
    }

    patches.reserve(pageRects.size());
    for (const QRectF &pageRect : pageRects) {
        if (pageRect.isEmpty()) {
            continue;
        }

        const QPointF corners[] = {
            pageRect.topLeft(),
            pageRect.topRight(),
            pageRect.bottomRight(),
            pageRect.bottomLeft(),
        };
        qint64 minimumX = std::numeric_limits<int>::max();
        qint64 minimumY = std::numeric_limits<int>::max();
        qint64 maximumX = std::numeric_limits<int>::min();
        qint64 maximumY = std::numeric_limits<int>::min();
        for (const QPointF &corner : corners) {
            int deviceX = 0;
            int deviceY = 0;
            if (!FPDF_PageToDevice(
                    page,
                    0,
                    0,
                    pixelSize.width(),
                    pixelSize.height(),
                    0,
                    corner.x(),
                    corner.y(),
                    &deviceX,
                    &deviceY)) {
                patches.clear();
                return false;
            }
            minimumX = qMin(minimumX, qint64(deviceX));
            minimumY = qMin(minimumY, qint64(deviceY));
            maximumX = qMax(maximumX, qint64(deviceX));
            maximumY = qMax(maximumY, qint64(deviceY));
        }
        if (maximumX <= minimumX || maximumY <= minimumY) {
            continue;
        }

        // Convert the full-page device bounds into this bitmap's coordinates
        // before clipping. Use wide intermediates so an off-page annotation or
        // tile origin cannot overflow QRect's signed integer coordinates.
        const qint64 left = qMax<qint64>(
            0, minimumX - bitmapOrigin.x()
                - kWidgetInteractionAdornmentMarginPixels);
        const qint64 top = qMax<qint64>(
            0, minimumY - bitmapOrigin.y()
                - kWidgetInteractionAdornmentMarginPixels);
        const qint64 right = qMin<qint64>(
            image.width(), maximumX - bitmapOrigin.x()
                + kWidgetInteractionAdornmentMarginPixels);
        const qint64 bottom = qMin<qint64>(
            image.height(), maximumY - bitmapOrigin.y()
                + kWidgetInteractionAdornmentMarginPixels);
        if (right <= left || bottom <= top) {
            continue;
        }
        const QRect deviceRect(
            static_cast<int>(left),
            static_cast<int>(top),
            static_cast<int>(right - left),
            static_cast<int>(bottom - top));
        QImage pixels = image.copy(deviceRect);
        if (pixels.isNull()) {
            patches.clear();
            return false;
        }
        patches.append({deviceRect, std::move(pixels)});
    }
    return true;
}

bool restorePDFWidgetRenderPatches(
    QImage &image,
    const QVector<PDFWidgetRenderPatch> &patches)
{
    if (image.isNull() || image.depth() != 32) {
        return false;
    }
    for (const PDFWidgetRenderPatch &patch : patches) {
        if (patch.pixels.isNull()
            || patch.pixels.depth() != image.depth()
            || patch.pixels.size() != patch.deviceRect.size()
            || !image.rect().contains(patch.deviceRect)) {
            return false;
        }
    }

    constexpr qsizetype bytesPerPixel = 4;
    for (const PDFWidgetRenderPatch &patch : patches) {
        const qsizetype rowBytes
            = patch.deviceRect.width() * bytesPerPixel;
        for (int row = 0; row < patch.deviceRect.height(); ++row) {
            std::memcpy(
                image.scanLine(patch.deviceRect.y() + row)
                    + patch.deviceRect.x() * bytesPerPixel,
                patch.pixels.constScanLine(row),
                rowBytes);
        }
    }
    return true;
}

PDFWidgetAnnotationSuppression::~PDFWidgetAnnotationSuppression()
{
    restore();
}

bool PDFWidgetAnnotationSuppression::suppress(
    FPDF_PAGE page,
    const QStringList &widgetIds)
{
    assertPDFiumProcessLockHeld();
    if (!page) {
        return widgetIds.isEmpty();
    }

    m_page = page;
    QSet<QString> targetIds;
    targetIds.reserve(widgetIds.size());
    for (const QString &widgetId : widgetIds) {
        targetIds.insert(widgetId);
    }
    if (targetIds.isEmpty()) {
        return true;
    }

    QSet<QString> matchedIds;
    const int annotationCount = FPDFPage_GetAnnotCount(page);
    for (int annotationIndex = 0;
         annotationIndex < annotationCount;
         ++annotationIndex) {
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
            page,
            annotationIndex);
        if (!annotation) {
            continue;
        }

        const QString widgetId
            = FPDFAnnot_GetSubtype(annotation) == FPDF_ANNOT_WIDGET
            ? annotationStringValue(annotation, "NM")
            : QString();
        if (!targetIds.contains(widgetId)) {
            FPDFPage_CloseAnnot(annotation);
            continue;
        }

        const int flags = FPDFAnnot_GetFlags(annotation);
        FS_RECTF rect{};
        if (!FPDFAnnot_GetRect(annotation, &rect)) {
            FPDFPage_CloseAnnot(annotation);
            restore();
            return false;
        }
        m_states.append({
            annotationIndex,
            flags,
            QRectF(
                rect.left,
                rect.bottom,
                rect.right - rect.left,
                rect.top - rect.bottom)});
        matchedIds.insert(widgetId);
        const bool hidden = flags & FPDF_ANNOT_FLAG_HIDDEN;
        const bool updated = hidden
            || FPDFAnnot_SetFlags(
                annotation,
                flags | FPDF_ANNOT_FLAG_HIDDEN);
        FPDFPage_CloseAnnot(annotation);
        if (!updated) {
            restore();
            return false;
        }
    }
    if (matchedIds != targetIds) {
        restore();
        return false;
    }
    return true;
}

bool PDFWidgetAnnotationSuppression::restore()
{
    assertPDFiumProcessLockHeld();
    if (m_restored) {
        return m_restoreSucceeded;
    }

    m_restored = true;
    for (auto state = m_states.crbegin();
         state != m_states.crend();
         ++state) {
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
            m_page,
            state->annotationIndex);
        if (!annotation) {
            m_restoreSucceeded = false;
            continue;
        }
        if (!FPDFAnnot_SetFlags(annotation, state->flags)) {
            m_restoreSucceeded = false;
        }
        FPDFPage_CloseAnnot(annotation);
    }
    m_states.clear();
    return m_restoreSucceeded;
}

bool PDFWidgetAnnotationSuppression::restorationSucceeded() const
{
    return m_restoreSucceeded;
}

QVector<QRectF> PDFWidgetAnnotationSuppression::pageRects() const
{
    QVector<QRectF> rects;
    rects.reserve(m_states.size());
    for (const State &state : m_states) {
        rects.append(state.pageRect);
    }
    return rects;
}
