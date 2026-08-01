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
        Dropdown
    };

    enum class Operation {
        Add,
        Update,
        NormalizeAppearance
    };

    struct Field {
        QString widgetId;
        QString fieldName;
        QString tooltip;
        QString value;
        QString defaultValue;
        QStringList options;
        QRectF pageRect;
        FieldType type = FieldType::Text;
        int fieldFlags = 0;
        Operation operation = Operation::Add;
    };

    static bool finalizeFields(const QString &filePath,
                               const QVector<Field> &fields,
                               QString &error);
};
