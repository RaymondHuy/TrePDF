#pragma once

#include "PDFCommentAnnotation.h"

#include <QColor>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

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

    struct AnnotationReply {
        int page = -1;
        QString parentThreadId;
        QString replyId;
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
        QColor color;
        QRectF pageRect;
    };

    static bool finalizeFields(const QString &filePath,
                               const QVector<Field> &fields,
                               QString &error);
    static bool finalizeAnnotationReplies(
        const QString &filePath,
        const QVector<AnnotationReply> &replies,
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
};
