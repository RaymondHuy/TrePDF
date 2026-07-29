#pragma once

#include <QRectF>
#include <QString>
#include <QVector>

class PDFAcroFormCanonicalizer
{
public:
    enum class Operation {
        Add,
        Update,
        NormalizeAppearance
    };

    struct TextField {
        QString widgetId;
        QString fieldName;
        QString tooltip;
        QString value;
        QString defaultValue;
        QRectF pageRect;
        int fieldFlags = 0;
        Operation operation = Operation::Add;
    };

    static bool finalizeTextFields(const QString &filePath,
                                   const QVector<TextField> &fields,
                                   QString &error);
};
