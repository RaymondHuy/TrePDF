#pragma once

#include <QtGlobal>

namespace PDFFormAppearance {

// These versions describe TrePDF's generated Widget presentation and remain
// separate from the field-data schema. Radio regeneration also migrates the
// legacy merged field/widget into the interoperable parent/child hierarchy.
inline constexpr char kVersionKey[] = "TrePDFApVersion";
inline constexpr char kFontVersionKey[] = "TrePDFFormFontVersion";
// PDFium's public annotation API does not expose an inherited parent /DV.
// Keep the equivalent per-widget bit so TrePDF can rehydrate that value.
inline constexpr char kRadioButtonDefaultCheckedKey[]
    = "TrePDFRadioDefaultChecked";
inline constexpr int kFontVersion = 2;
inline constexpr int kTextFieldVersion = 1;
inline constexpr int kCheckboxVersion = 1;
inline constexpr int kRadioButtonVersion = 3;
inline constexpr int kDropdownVersion = 2;
inline constexpr qreal kTextFieldFontPointSize = 12.0;
inline constexpr qreal kTextFieldBorderPointWidth = 1.0;
inline constexpr quint32 kTextFieldBorderRgb = 0xff8c8c8c;
inline constexpr qreal kTextFieldMinimumHorizontalPaddingPoints = 2.0;
inline constexpr qreal kTextFieldHorizontalPaddingRatio = 0.1;
inline constexpr qreal kRadioButtonVisualScale = 0.72;
inline constexpr qreal kRadioButtonBorderPointWidth = 1.0;
inline constexpr qreal kRadioButtonMinimumBorderPointWidth = 0.5;
inline constexpr qreal kRadioButtonDotRadiusRatio = 0.42;

} // namespace PDFFormAppearance
