#pragma once

#include "PDFCommentAnnotation.h"
#include "PDFWebLinkScanner.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>

class PDFAcroFormCanonicalizer
{
public:
    enum class FieldType {
        Text,
        Checkbox,
        RadioButton,
        Dropdown,
        ListBox
    };

    enum class Operation {
        Add,
        Update,
        NormalizeAppearance,
        Remove
    };

    struct Field {
        QString widgetId;
        QString fieldName;
        QString tooltip;
        QString value;
        QStringList selectedValues;
        QString defaultValue;
        QStringList options;
        QRectF pageRect;
        FieldType type = FieldType::Text;
        int fieldFlags = 0;
        int maxLength = 0;
        Operation operation = Operation::Add;
    };

    enum class AnnotationRelationshipKind {
        Reply,
        State
    };

    struct AnnotationRelationship {
        int page = -1;
        QString parentThreadId;
        QString parentAnnotationId;
        QString childAnnotationId;
        AnnotationRelationshipKind kind
            = AnnotationRelationshipKind::Reply;
        QString expectedState;
        QString expectedStateModel;
    };

    struct FreeTextAnnotation {
        int page = -1;
        QString annotationId;
        QString text;
        bool preserveDefaultAppearance = false;
    };

    enum class TextMarkupType {
        Highlight,
        Underline,
        Squiggly,
        StrikeOut
    };

    struct TextMarkupAnnotation {
        int page = -1;
        QString annotationId;
        TextMarkupType type = TextMarkupType::Highlight;
    };

    struct StampAnnotation {
        int page = -1;
        QString annotationId;
        QString stampName;
        QString displayText;
        QString detailText;
        QColor color;
        QRectF pageRect;
    };

    enum class SourceCommentGeometryType {
        StickyNote,
        Stamp,
        Highlight,
        Underline,
        Squiggly,
        StrikeOut
    };

    struct SourceCommentGeometryUpdate {
        QString markerId;
        int page = -1;
        SourceCommentGeometryType type
            = SourceCommentGeometryType::StickyNote;
        QRectF pageRect;
        QVector<std::array<QPointF, 4>> pageQuadPoints;
    };

    inline static constexpr char kSourceCommentGeometryMarkerKey[]
        = "TrePDFSourceCommentGeometryId";

    struct LinkAnnotation {
        QString markerId;
        int page = -1;
        QRectF pageRect;
        int pageQuarterTurns = 0;
        int destinationPage = -1;
        PDFWebLinks::ManualLink link;
    };

    static bool finalizeLinkAnnotations(
        const QString &filePath,
        const QVector<LinkAnnotation> &annotations,
        QString &error);

    static bool finalizeFields(const QString &filePath,
                               const QVector<Field> &fields,
                               QString &error);
    static bool finalizeAnnotationRelationships(
        const QString &filePath,
        const QVector<AnnotationRelationship> &relationships,
        QString &error);
    static bool finalizeFreeTextAnnotations(
        const QString &filePath,
        const QVector<FreeTextAnnotation> &annotations,
        QString &error);
    static bool finalizeTextMarkupAnnotations(
        const QString &filePath,
        const QVector<TextMarkupAnnotation> &annotations,
        QString &error);
    static bool finalizeDrawingAnnotations(
        const QString &filePath,
        const QVector<PDFCommentAnnotation::DrawingAnnotation> &annotations,
        QString &error);
    static bool finalizeStampAnnotations(
        const QString &filePath,
        const QVector<StampAnnotation> &annotations,
        QString &error);
    static bool finalizeSourceDrawingGeometryUpdates(
        const QString &filePath,
        const QVector<PDFCommentAnnotation::SourceDrawingGeometryUpdate> &updates,
        QString &error);
    static bool finalizeSourceCommentGeometryUpdates(
        const QString &filePath,
        const QVector<SourceCommentGeometryUpdate> &updates,
        QString &error);
};
