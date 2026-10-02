#pragma once

#include <cmath>

#include <fpdf_edit.h>
#include <fpdf_transformpage.h>

namespace PDFSourceTextGeometry {

// Call under the process-wide PDFium lock. The returned leaf only supplies
// text/font data: mutations always target the original page-level occurrence.
// Replacing that occurrence must never discard siblings or inherited clipping,
// paint effects, or marked-content semantics from a shared Form stream.
inline FPDF_PAGEOBJECT textObject(FPDF_PAGEOBJECT object)
{
    if (!object || FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_TEXT)
        return object;

    constexpr int maximumFormDepth = 16;
    for (int depth = 0; depth <= maximumFormDepth; ++depth) {
        FS_MATRIX matrix{};
        FPDF_BOOL active = false;
        if (!object || FPDFPageObj_CountMarks(object) != 0
            || FPDFPageObj_HasTransparency(object)
            || !FPDFPageObj_GetIsActive(object, &active) || !active
            || !FPDFPageObj_GetMatrix(object, &matrix)) {
            return nullptr;
        }
        const double determinant = double(matrix.a) * matrix.d
            - double(matrix.b) * matrix.c;
        if (!std::isfinite(matrix.a) || !std::isfinite(matrix.b)
            || !std::isfinite(matrix.c) || !std::isfinite(matrix.d)
            || !std::isfinite(matrix.e) || !std::isfinite(matrix.f)
            || !std::isfinite(determinant) || determinant == 0.0) {
            return nullptr;
        }
        // PDFium exposes effective Form BBox clipping on its children, so check
        // every node, including the leaf. Zero paths can still mean text clip.
        const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(object);
        if (!clip || FPDFClipPath_CountPaths(clip) != -1)
            return nullptr;

        unsigned int red = 0, green = 0, blue = 0, alpha = 0;
        if (!FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha)
            || alpha != 255) {
            return nullptr;
        }
        const int type = FPDFPageObj_GetType(object);
        if (type == FPDF_PAGEOBJ_TEXT) {
            // Stroke width belongs to the Form's local coordinate system.
            // Fill-only text can use the existing page-space reflow unchanged.
            return FPDFTextObj_GetTextRenderMode(object) == FPDF_TEXTRENDERMODE_FILL
                ? object : nullptr;
        }
        if (type != FPDF_PAGEOBJ_FORM || depth == maximumFormDepth
            || FPDFFormObj_CountObjects(object) != 1
            || red != 0 || green != 0 || blue != 0) {
            return nullptr;
        }
        object = FPDFFormObj_GetObject(object, 0);
    }
    return nullptr;
}

inline bool isTextOnlyForm(FPDF_PAGEOBJECT object)
{
    return object && FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_FORM
        && textObject(object);
}

} // namespace PDFSourceTextGeometry
