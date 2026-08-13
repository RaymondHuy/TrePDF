#pragma once

#include <QString>

#include <fpdf_annot.h>

namespace PDFAnnotationUtils {

// Functions accepting an annotation borrow the handle. The caller must keep
// the annotation alive and hold the process-wide PDFium lock.
QString annotationString(FPDF_ANNOTATION annotation, FPDF_BYTESTRING key);
QString normalizedCommentText(QString text);
bool isSupportedCommentAnnotationSubtype(int subtype);
bool isMarkupAnnotationSubtype(int subtype);
bool isProjectionAnnotation(FPDF_ANNOTATION annotation);
bool isMarkupAnnotation(FPDF_ANNOTATION annotation);
bool annotationUsesReplyRelation(FPDF_ANNOTATION annotation);

} // namespace PDFAnnotationUtils
