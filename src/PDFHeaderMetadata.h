#pragma once

#include "PDFiumSynchronization.h"

#include <QCryptographicHash>
#include <QImage>
#include <QString>
#include <QUuid>

#include <fpdf_edit.h>

namespace PDFHeaderMetadata {

inline constexpr char kMarkName[] = "TrePDFHeader";
inline constexpr qint64 kMaximumDigestPixels = 4000000;

struct Metadata {
    QString group;
    QString role;
    bool invalid = false;
};

inline bool valid(const QString &group, const QString &role)
{
    const QUuid uuid(group);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == group
        && (role == QLatin1String("text") || role == QLatin1String("image")
            || role == QLatin1String("pageNumber"));
}

inline Metadata read(FPDF_PAGEOBJECT object)
{
    assertPDFiumProcessLockHeld();
    Metadata result;
    const int count = object ? FPDFPageObj_CountMarks(object) : -1;
    if (count < 0 || count > 64) {
        result.invalid = true;
        return result;
    }
    const auto readString = [](FPDF_PAGEOBJECTMARK mark, const char *key,
                               QString &value) {
        FPDF_WCHAR buffer[64]{};
        unsigned long bytes = 0;
        const bool read = key
            ? FPDFPageObjMark_GetParamStringValue(mark, key, buffer, sizeof(buffer), &bytes)
            : FPDFPageObjMark_GetName(mark, buffer, sizeof(buffer), &bytes);
        if (!read || bytes < 2 || bytes > sizeof(buffer) || bytes % 2 != 0
            || buffer[bytes / 2 - 1] != 0) {
            return false;
        }
        value = QString::fromUtf16(reinterpret_cast<const char16_t *>(buffer), bytes / 2 - 1);
        return !value.contains(QChar::Null);
    };
    bool found = false;
    for (int index = 0; index < count; ++index) {
        const auto mark = FPDFPageObj_GetMark(object, index);
        QString name;
        if (!mark || !readString(mark, nullptr, name) || name != QLatin1String(kMarkName))
            continue;
        int version = 0;
        QString group, role;
        if (found || !FPDFPageObjMark_GetParamIntValue(mark, "Version", &version)
            || version != 1 || !readString(mark, "Group", group)
            || !readString(mark, "Role", role) || !valid(group, role)) {
            result = Metadata{{}, {}, true};
            return result;
        }
        found = true;
        result.group = group;
        result.role = role;
    }
    return result;
}

inline bool write(FPDF_DOCUMENT document, FPDF_PAGEOBJECT object,
                  const QString &group, const QString &role)
{
    assertPDFiumProcessLockHeld();
    if (group.isEmpty() && role.isEmpty())
        return true;
    if (!document || !object || !valid(group, role))
        return false;
    const QByteArray groupBytes = group.toLatin1();
    const QByteArray roleBytes = role.toLatin1();
    const auto mark = FPDFPageObj_AddMark(object, kMarkName);
    return mark
        && FPDFPageObjMark_SetIntParam(document, object, mark, "Version", 1)
        && FPDFPageObjMark_SetStringParam(document, object, mark, "Group", groupBytes.constData())
        && FPDFPageObjMark_SetStringParam(document, object, mark, "Role", roleBytes.constData());
}

inline QByteArray imageDigest(const QImage &source)
{
    if (source.isNull() || qint64(source.width()) * source.height() > kMaximumDigestPixels)
        return {};
    // A fixed channel order excludes row padding and platform endianness.
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(image.width()) + 'x' + QByteArray::number(image.height()) + ':');
    for (int row = 0; row < image.height(); ++row)
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(row)),
                                    qsizetype(image.width()) * 4));
    return hash.result();
}

} // namespace PDFHeaderMetadata
