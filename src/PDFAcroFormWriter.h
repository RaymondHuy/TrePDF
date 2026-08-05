#pragma once

#include <QRectF>
#include <QString>
#include <QStringList>
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
        QStringList options;
        FieldType type = FieldType::Text;
        Operation operation = Operation::Add;
        int page = -1;
        QRectF pageRect;
        bool required = false;
        bool readOnly = false;
        bool multiline = false;
        bool comb = false;
        int maxLength = 0;
        bool valueChanged = true;
        bool optionsChanged = true;
    };

    struct Result {
        bool succeeded = false;
        QString error;
    };

    static Result write(const QString &sourceFilePath,
                        const QString &targetFilePath,
                        const QVector<Field> &fields);
};
