#pragma once

#include <QByteArray>
#include <QString>
#include <QStringView>

#include <fpdf_annot.h>

namespace PDFAnnotationUtils {

// Functions accepting an annotation borrow the handle. The caller must keep
// the annotation alive and hold the process-wide PDFium lock.
QString annotationString(FPDF_ANNOTATION annotation, FPDF_BYTESTRING key);
QString annotationNormalAppearance(FPDF_ANNOTATION annotation);
QByteArray normalAppearanceFingerprint(QStringView appearance);
QString normalizedCommentText(QString text);
bool isSupportedCommentAnnotationSubtype(int subtype);
bool isMarkupAnnotationSubtype(int subtype);
bool isProjectionAnnotation(FPDF_ANNOTATION annotation);
bool isMarkupAnnotation(FPDF_ANNOTATION annotation);
bool annotationUsesReplyRelation(FPDF_ANNOTATION annotation);

} // namespace PDFAnnotationUtils
