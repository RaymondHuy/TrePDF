#include "PDFAnnotationUtils.h"

#include "PDFiumSynchronization.h"

#include <QCryptographicHash>
#include <QVector>

namespace {

constexpr unsigned long kMaximumAnnotationStringBytes
    = 4UL * 1024UL * 1024UL;
constexpr unsigned long kMaximumAnnotationAppearanceBytes
    = 1UL * 1024UL * 1024UL;

} // namespace

namespace PDFAnnotationUtils {

QString annotationString(FPDF_ANNOTATION annotation, FPDF_BYTESTRING key)
{
    assertPDFiumProcessLockHeld();
    if (!annotation || !key) {
        return {};
    }

    const unsigned long byteLength = FPDFAnnot_GetStringValue(
        annotation,
        key,
        nullptr,
        0);
    if (byteLength < sizeof(FPDF_WCHAR)
        || byteLength > kMaximumAnnotationStringBytes
        || byteLength % sizeof(FPDF_WCHAR) != 0) {
        return {};
    }

    QVector<FPDF_WCHAR> buffer(static_cast<qsizetype>(
        byteLength / sizeof(FPDF_WCHAR)));
    const unsigned long actualLength = FPDFAnnot_GetStringValue(
        annotation,
        key,
        buffer.data(),
        byteLength);
    if (actualLength < sizeof(FPDF_WCHAR)
        || actualLength > byteLength
        || actualLength % sizeof(FPDF_WCHAR) != 0) {
        return {};
    }

    qsizetype characterCount = static_cast<qsizetype>(
        actualLength / sizeof(FPDF_WCHAR));
    while (characterCount > 0 && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
}

QString annotationNormalAppearance(FPDF_ANNOTATION annotation)
{
    assertPDFiumProcessLockHeld();
    if (!annotation) {
        return {};
    }

    const unsigned long byteLength = FPDFAnnot_GetAP(
        annotation,
        FPDF_ANNOT_APPEARANCEMODE_NORMAL,
        nullptr,
        0);
    if (byteLength < sizeof(FPDF_WCHAR)
        || byteLength > kMaximumAnnotationAppearanceBytes
        || byteLength % sizeof(FPDF_WCHAR) != 0) {
        return {};
    }

    QVector<FPDF_WCHAR> buffer(static_cast<qsizetype>(
        byteLength / sizeof(FPDF_WCHAR)));
    const unsigned long actualLength = FPDFAnnot_GetAP(
        annotation,
        FPDF_ANNOT_APPEARANCEMODE_NORMAL,
        buffer.data(),
        byteLength);
    if (actualLength < sizeof(FPDF_WCHAR)
        || actualLength > byteLength
        || actualLength % sizeof(FPDF_WCHAR) != 0) {
        return {};
    }

    qsizetype characterCount = static_cast<qsizetype>(
        actualLength / sizeof(FPDF_WCHAR));
    while (characterCount > 0 && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
}

QByteArray normalAppearanceFingerprint(QStringView appearance)
{
    if (appearance.isEmpty()) {
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(
        reinterpret_cast<const char *>(appearance.utf16()),
        appearance.size() * static_cast<qsizetype>(sizeof(char16_t))));
    return hash.result();
}

QString normalizedCommentText(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

bool isSupportedCommentAnnotationSubtype(int subtype)
{
    return subtype == FPDF_ANNOT_HIGHLIGHT
        || subtype == FPDF_ANNOT_UNDERLINE
        || subtype == FPDF_ANNOT_SQUIGGLY
        || subtype == FPDF_ANNOT_STRIKEOUT
        || subtype == FPDF_ANNOT_TEXT
        || subtype == FPDF_ANNOT_FREETEXT
        || subtype == FPDF_ANNOT_SQUARE
        || subtype == FPDF_ANNOT_CIRCLE
        || subtype == FPDF_ANNOT_LINE
        || subtype == FPDF_ANNOT_POLYGON
        || subtype == FPDF_ANNOT_POLYLINE
        || subtype == FPDF_ANNOT_INK
        || subtype == FPDF_ANNOT_STAMP;
}

bool isMarkupAnnotationSubtype(int subtype)
{
    switch (subtype) {
    case FPDF_ANNOT_TEXT:
    case FPDF_ANNOT_FREETEXT:
    case FPDF_ANNOT_LINE:
    case FPDF_ANNOT_SQUARE:
    case FPDF_ANNOT_CIRCLE:
    case FPDF_ANNOT_POLYGON:
    case FPDF_ANNOT_POLYLINE:
    case FPDF_ANNOT_HIGHLIGHT:
    case FPDF_ANNOT_UNDERLINE:
    case FPDF_ANNOT_SQUIGGLY:
    case FPDF_ANNOT_STRIKEOUT:
    case FPDF_ANNOT_STAMP:
    case FPDF_ANNOT_CARET:
    case FPDF_ANNOT_INK:
    case FPDF_ANNOT_FILEATTACHMENT:
    case FPDF_ANNOT_SOUND:
    case FPDF_ANNOT_REDACT:
        return true;
    default:
        return false;
    }
}

bool isProjectionAnnotation(FPDF_ANNOTATION annotation)
{
    assertPDFiumProcessLockHeld();
    return annotation
        && FPDFAnnot_GetSubtype(annotation) == FPDF_ANNOT_UNKNOWN
        && annotationString(annotation, "Subtype")
            == QLatin1String("Projection");
}

bool isMarkupAnnotation(FPDF_ANNOTATION annotation)
{
    assertPDFiumProcessLockHeld();
    return annotation
        && (isMarkupAnnotationSubtype(FPDFAnnot_GetSubtype(annotation))
            || isProjectionAnnotation(annotation));
}

bool isCommentStateAnnotation(FPDF_ANNOTATION annotation)
{
    assertPDFiumProcessLockHeld();
    // PDF 1.5+ stores annotation state transitions in separate Text
    // annotations linked to the reviewed annotation through /IRT.
    if (!annotation
        || FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_TEXT
        || !annotationUsesReplyRelation(annotation)) {
        return false;
    }
    const QString state = annotationString(annotation, "State").trimmed();
    const QString stateModel
        = annotationString(annotation, "StateModel").trimmed();
    if (state.isEmpty()
        || (stateModel.compare(
                QLatin1String("Review"),
                Qt::CaseInsensitive) != 0
            && stateModel.compare(
                   QLatin1String("Marked"),
                   Qt::CaseInsensitive) != 0)) {
        return false;
    }

    FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(annotation, "IRT");
    if (!parent) {
        return false;
    }
    FPDFPage_CloseAnnot(parent);
    return true;
}

bool annotationUsesReplyRelation(FPDF_ANNOTATION annotation)
{
    assertPDFiumProcessLockHeld();
    const QString relation = annotationString(annotation, "RT").trimmed();
    return relation.isEmpty()
        || relation.compare(QLatin1String("R"), Qt::CaseInsensitive) == 0;
}

} // namespace PDFAnnotationUtils
