#pragma once

#include <QRectF>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <fpdf_edit.h>
#include <fpdf_transformpage.h>

namespace PDFSourceImageGeometry {

inline bool finiteRect(const QRectF &rect)
{
    return std::isfinite(rect.x()) && std::isfinite(rect.y())
        && std::isfinite(rect.width()) && std::isfinite(rect.height())
        && rect.width() > 0.0 && rect.height() > 0.0;
}

inline bool finiteMatrix(const FS_MATRIX &matrix)
{
    return std::isfinite(matrix.a) && std::isfinite(matrix.b)
        && std::isfinite(matrix.c) && std::isfinite(matrix.d)
        && std::isfinite(matrix.e) && std::isfinite(matrix.f);
}

inline bool finiteInvertibleMatrix(const FS_MATRIX &matrix)
{
    const double determinant = static_cast<double>(matrix.a) * matrix.d
        - static_cast<double>(matrix.b) * matrix.c;
    return finiteMatrix(matrix) && std::isfinite(determinant) && determinant != 0.0;
}

// Edit the page-level occurrence, never its shared Form stream. Only a chain
// containing one image and no other content can act as an image as a whole.
inline bool isImageOnlyForm(FPDF_PAGEOBJECT object)
{
    if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_FORM
        || FPDFPageObj_HasTransparency(object)) {
        return false;
    }
    // PDFium cannot preserve arbitrary outer soft-mask graphics state when
    // regenerating an occurrence, nor move its page-space clip with the image.
    // Internal image masks and Form clips stay untouched in their own stream.
    const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(object);
    if (!clip || FPDFClipPath_CountPaths(clip) != -1)
        return false;
    // A stencil image can inherit this paint. PDFium does not serialize every
    // non-RGB outer color space; default black is safe without decoding pixels.
    unsigned int red = 0, green = 0, blue = 0, alpha = 0;
    if (!FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha)
        || red != 0 || green != 0 || blue != 0 || alpha != 255) {
        return false;
    }
    constexpr int maximumFormDepth = 16;
    for (int depth = 0; depth <= maximumFormDepth; ++depth) {
        FS_MATRIX matrix{};
        if (!object || FPDFPageObj_CountMarks(object) != 0
            || !FPDFPageObj_GetMatrix(object, &matrix)
            || !finiteInvertibleMatrix(matrix)) {
            return false;
        }
        const int type = FPDFPageObj_GetType(object);
        if (type == FPDF_PAGEOBJ_IMAGE)
            return true;
        if (type != FPDF_PAGEOBJ_FORM || depth == maximumFormDepth
            || FPDFFormObj_CountObjects(object) != 1) {
            return false;
        }
        object = FPDFFormObj_GetObject(object, 0);
    }
    return false;
}

inline bool isImageObject(FPDF_PAGEOBJECT object)
{
    return object && (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_IMAGE
                      || isImageOnlyForm(object));
}

inline bool hasSizeChange(const QRectF &sourceRect, const QRectF &frameRect)
{
    return !qFuzzyCompare(sourceRect.width(), frameRect.width())
        || !qFuzzyCompare(sourceRect.height(), frameRect.height());
}

// Coordinates have their origin at the visible top-left, with y growing down.
// Keep these quarter turns exact so four rotations cannot accumulate drift.
inline QTransform unitQuarterTurnTransform(int imageQuarterTurns)
{
    switch ((imageQuarterTurns % 4 + 4) % 4) {
    case 1:
        return QTransform(0.0, 1.0, -1.0, 0.0, 1.0, 0.0);
    case 2:
        return QTransform(-1.0, 0.0, 0.0, -1.0, 1.0, 1.0);
    case 3:
        return QTransform(0.0, -1.0, 1.0, 0.0, 0.0, 1.0);
    default:
        return {};
    }
}

// Call only while holding the process-wide PDFium lock. A valid object always
// exposes a clip wrapper; CountPaths returns -1 for an absent clip reference.
// A zero path count can still contain text clipping, so it is not resize-safe.
// The public API cannot snapshot/restore custom clipping exactly for previews.
inline bool canResize(FPDF_PAGE page, FPDF_PAGEOBJECT object)
{
    FS_MATRIX matrix{};
    if (!page || !isImageObject(object)
        || !FPDFPageObj_GetMatrix(object, &matrix)
        || !finiteInvertibleMatrix(matrix)) {
        return false;
    }
    const FPDF_CLIPPATH clipPath = FPDFPageObj_GetClipPath(object);
    if (!clipPath || FPDFClipPath_CountPaths(clipPath) != -1) {
        return false;
    }

    // Candidate bounds are clipped to the visible page. Scaling that partial
    // frame would reveal image pixels outside it, so require the full image.
    FS_RECTF pageBounds{};
    if (!FPDF_GetPageBoundingBox(page, &pageBounds)
        || !std::isfinite(pageBounds.left) || !std::isfinite(pageBounds.right)
        || !std::isfinite(pageBounds.top) || !std::isfinite(pageBounds.bottom)
        || pageBounds.right <= pageBounds.left || pageBounds.top <= pageBounds.bottom) {
        return false;
    }
    const double epsilon = std::max(
        double(pageBounds.right) - pageBounds.left,
        double(pageBounds.top) - pageBounds.bottom) * 1e-6;
    if (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_FORM) {
        // Internal Form BBoxes and clipping travel with this occurrence. Only
        // an external clip (checked above) would stay behind during a transform.
        float left = 0, bottom = 0, right = 0, top = 0;
        return FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top)
            && std::isfinite(left) && std::isfinite(bottom)
            && std::isfinite(right) && std::isfinite(top)
            && right > left && top > bottom
            && left >= pageBounds.left - epsilon
            && right <= pageBounds.right + epsilon
            && bottom >= pageBounds.bottom - epsilon
            && top <= pageBounds.top + epsilon;
    }
    const std::array<QPointF, 4> corners{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
    for (const QPointF &corner : corners) {
        const double pageX = matrix.a * corner.x() + matrix.c * corner.y() + matrix.e;
        const double pageY = matrix.b * corner.x() + matrix.d * corner.y() + matrix.f;
        if (pageX < pageBounds.left - epsilon || pageX > pageBounds.right + epsilon
            || pageY < pageBounds.bottom - epsilon || pageY > pageBounds.top + epsilon) {
            return false;
        }
    }
    return true;
}

// Compose the existing image matrix with the normalized source-to-target
// frame mapping and optional clockwise rotation. This retains the image's
// original rotation, reflection, shear and
// pixel stream. Deriving the page basis at exact device corners avoids raster
// rounding, and accounts for an offset CropBox and intrinsic page rotation.
inline bool transformedMatrix(FPDF_PAGE page,
                              FPDF_PAGEOBJECT object,
                              const QRectF &sourceRect,
                              const QRectF &frameRect,
                              const FS_MATRIX &originalMatrix,
                              FS_MATRIX &result,
                              int imageQuarterTurns = 0)
{
    const int quarterTurns = (imageQuarterTurns % 4 + 4) % 4;
    if (!page || !isImageObject(object)
        || !finiteRect(sourceRect) || !finiteRect(frameRect)
        || !finiteMatrix(originalMatrix)
        || ((quarterTurns != 0 || hasSizeChange(sourceRect, frameRect))
            && !canResize(page, object))) {
        return false;
    }

    double originX = 0.0;
    double originY = 0.0;
    double rightX = 0.0;
    double rightY = 0.0;
    double downX = 0.0;
    double downY = 0.0;
    if (!FPDF_DeviceToPage(page, 0, 0, 1, 1, 0, 0, 0, &originX, &originY)
        || !FPDF_DeviceToPage(page, 0, 0, 1, 1, 0, 1, 0, &rightX, &rightY)
        || !FPDF_DeviceToPage(page, 0, 0, 1, 1, 0, 0, 1, &downX, &downY)) {
        return false;
    }

    const QTransform normalizedToPage(rightX - originX, rightY - originY,
                                      downX - originX, downY - originY,
                                      originX, originY);
    const auto representable = [](qreal value) {
        return std::isfinite(value)
            && std::abs(value) <= std::numeric_limits<float>::max();
    };
    if (quarterTurns == 0 && !hasSizeChange(sourceRect, frameRect)) {
        const QPointF delta = frameRect.topLeft() - sourceRect.topLeft();
        const qreal translatedX = originalMatrix.e
            + delta.x() * normalizedToPage.m11()
            + delta.y() * normalizedToPage.m21();
        const qreal translatedY = originalMatrix.f
            + delta.x() * normalizedToPage.m12()
            + delta.y() * normalizedToPage.m22();
        if (!representable(translatedX) || !representable(translatedY)) {
            return false;
        }
        result = originalMatrix;
        result.e = static_cast<float>(translatedX);
        result.f = static_cast<float>(translatedY);
        return true;
    }
    bool invertible = false;
    const QTransform pageToNormalized = normalizedToPage.inverted(&invertible);
    if (!invertible) {
        return false;
    }
    const qreal scaleX = frameRect.width() / sourceRect.width();
    const qreal scaleY = frameRect.height() / sourceRect.height();
    QTransform frameMapping(scaleX, 0.0, 0.0, scaleY,
        frameRect.x() - scaleX * sourceRect.x(),
        frameRect.y() - scaleY * sourceRect.y());
    if (quarterTurns != 0) {
        const QTransform sourceToUnit(1.0 / sourceRect.width(), 0.0,
            0.0, 1.0 / sourceRect.height(),
            -sourceRect.x() / sourceRect.width(),
            -sourceRect.y() / sourceRect.height());
        const QTransform unitToFrame(frameRect.width(), 0.0,
            0.0, frameRect.height(), frameRect.x(), frameRect.y());
        frameMapping = sourceToUnit * unitQuarterTurnTransform(quarterTurns)
            * unitToFrame;
    }
    const QTransform original(originalMatrix.a, originalMatrix.b,
                              originalMatrix.c, originalMatrix.d,
                              originalMatrix.e, originalMatrix.f);
    const QTransform transformed = original * pageToNormalized
        * frameMapping * normalizedToPage;
    if (!representable(transformed.m11()) || !representable(transformed.m12())
        || !representable(transformed.m21()) || !representable(transformed.m22())
        || !representable(transformed.dx()) || !representable(transformed.dy())) {
        return false;
    }
    result = FS_MATRIX{static_cast<float>(transformed.m11()),
                       static_cast<float>(transformed.m12()),
                       static_cast<float>(transformed.m21()),
                       static_cast<float>(transformed.m22()),
                       static_cast<float>(transformed.dx()),
                       static_cast<float>(transformed.dy())};
    return finiteInvertibleMatrix(result);
}

} // namespace PDFSourceImageGeometry
