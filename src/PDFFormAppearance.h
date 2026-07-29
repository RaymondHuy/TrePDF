#pragma once

#include <QtGlobal>

namespace PDFFormAppearance {

// This version describes only TrePDF's generated Widget appearance. It is
// intentionally separate from the field-data schema so future validation
// properties can survive an appearance-only migration unchanged.
inline constexpr char kVersionKey[] = "TrePDFApVersion";
inline constexpr int kTextFieldVersion = 1;
inline constexpr qreal kTextFieldFontPointSize = 12.0;
inline constexpr qreal kTextFieldBorderPointWidth = 1.0;
inline constexpr quint32 kTextFieldBorderRgb = 0xff8c8c8c;
inline constexpr qreal kTextFieldMinimumHorizontalPaddingPoints = 2.0;
inline constexpr qreal kTextFieldHorizontalPaddingRatio = 0.1;

} // namespace PDFFormAppearance
