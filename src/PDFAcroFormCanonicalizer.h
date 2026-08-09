#pragma once

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

    static bool finalizeFields(const QString &filePath,
                               const QVector<Field> &fields,
                               QString &error);
    static bool finalizeAnnotationReplies(
        const QString &filePath,
        const QVector<AnnotationReply> &replies,
        QString &error);
};
