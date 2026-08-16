#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

namespace PDFCommentAnnotation {

enum class DrawingType {
    Square,
    Circle,
    Line,
    Polygon,
    PolyLine,
    Ink
};

enum class BorderStyle {
    Solid,
    Dashed
};

enum class BorderEffect {
    None,
    Cloudy
};

enum class LineEnding {
    None,
    Square,
    Circle,
    Diamond,
    OpenArrow,
    ClosedArrow,
    Butt,
    ReverseOpenArrow,
    ReverseClosedArrow,
    Slash
};

// The preview and the PDF appearance writer consume the same property
// snapshot, keeping the annotation dictionary and its /AP stream in sync.
struct CommonProperties {
    int flags = 4; // PDF Print flag; intentionally independent of PDFium.
    QString subject;
    QColor strokeColor = Qt::black;
    QColor interiorColor;
    qreal opacity = 1.0;
    qreal borderWidth = 1.5;
    BorderStyle borderStyle = BorderStyle::Solid;
    QVector<qreal> dashPattern;
    BorderEffect borderEffect = BorderEffect::None;
    qreal borderEffectIntensity = 0.0;
    QString intent;
    QString name;
    LineEnding startLineEnding = LineEnding::None;
    LineEnding endLineEnding = LineEnding::None;
};

// Geometry is expressed in PDF page coordinates. UI/controller geometry stays
// normalized until the save writer maps it through FPDF_DeviceToPage(). Ink
// keeps each /InkList path as a separate stroke instead of flattening paths.
struct DrawingAnnotation {
    int page = -1;
    QString annotationId;
    DrawingType type = DrawingType::Square;
    QRectF pageRect;
    QVector<QPointF> pageVertices;
    QVector<QVector<QPointF>> pageStrokes;
    CommonProperties properties;
};

// PDFium's FPDFAnnot_SetRect() may enlarge an existing normal appearance's
// /BBox. Source Ink geometry therefore crosses the save boundary through a
// temporary marker and is patched directly after PDFium serializes the file.
// A resize of a TrePDF-managed Ink may replace only the normal appearance when
// the canonical vector writer supports every captured property. External Ink
// and pure movement always preserve /AP.
inline constexpr char kSourceInkGeometryMarkerKey[]
    = "TrePDFSourceInkGeometryId";

struct SourceInkGeometryUpdate {
    QString markerId;
    DrawingAnnotation annotation;
    bool regenerateAppearance = false;
};

} // namespace PDFCommentAnnotation
