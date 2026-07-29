#pragma once

#include <QRectF>
#include <QString>
#include <QVector>

class PDFAcroFormWriter
{
public:
    enum class FieldType {
        Text,
        Checkbox,
        RadioButton,
        Dropdown,
        Date,
        Signature
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
        QString defaultValue;
        FieldType type = FieldType::Text;
        Operation operation = Operation::Add;
        int page = -1;
        QRectF pageRect;
        bool required = false;
        bool readOnly = false;
        bool valueChanged = true;
    };

    struct Result {
        bool succeeded = false;
        QString error;
    };

    static Result write(const QString &sourceFilePath,
                        const QString &targetFilePath,
                        const QVector<Field> &fields);
};
