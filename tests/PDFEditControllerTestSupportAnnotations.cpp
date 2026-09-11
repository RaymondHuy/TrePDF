#include "PDFEditControllerTestSupport.h"

namespace PDFEditControllerTestSupport {


QString annotationDictionaryString(FPDF_ANNOTATION annotation,
                                   FPDF_BYTESTRING key)
{
    const unsigned long byteLength = FPDFAnnot_GetStringValue(
        annotation,
        key,
        nullptr,
        0);
    if (byteLength < sizeof(FPDF_WCHAR)) {
        return {};
    }
    QVector<FPDF_WCHAR> buffer(
        static_cast<qsizetype>(
            (byteLength + sizeof(FPDF_WCHAR) - 1)
            / sizeof(FPDF_WCHAR)));
    const unsigned long actualLength = FPDFAnnot_GetStringValue(
        annotation,
        key,
        buffer.data(),
        byteLength);
    qsizetype characterCount = static_cast<qsizetype>(
        actualLength / sizeof(FPDF_WCHAR));
    while (characterCount > 0
           && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
}

QString annotationAppearanceString(FPDF_ANNOTATION annotation)
{
    const unsigned long byteLength = FPDFAnnot_GetAP(
        annotation,
        FPDF_ANNOT_APPEARANCEMODE_NORMAL,
        nullptr,
        0);
    if (byteLength < sizeof(FPDF_WCHAR)) {
        return {};
    }
    QVector<FPDF_WCHAR> buffer(
        static_cast<qsizetype>(
            (byteLength + sizeof(FPDF_WCHAR) - 1)
            / sizeof(FPDF_WCHAR)));
    const unsigned long actualLength = FPDFAnnot_GetAP(
        annotation,
        FPDF_ANNOT_APPEARANCEMODE_NORMAL,
        buffer.data(),
        byteLength);
    qsizetype characterCount = static_cast<qsizetype>(
        actualLength / sizeof(FPDF_WCHAR));
    while (characterCount > 0
           && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
}

CommentAnnotationInspection inspectCommentAnnotation(
    const QString &filePath,
    const QString &targetName,
    int pageIndex)
{
    PDFiumProcessLock pdfiumLock;
    CommentAnnotationInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    inspection.readable = document && page;
    if (page) {
        inspection.annotationCount = FPDFPage_GetAnnotCount(page);
        for (int index = 0;
             index < inspection.annotationCount;
             ++index) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
            if (!annotation) {
                continue;
            }
            const QString name = annotationDictionaryString(
                annotation,
                "NM");
            if (name != targetName) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            inspection.annotationIndex = index;
            inspection.subtype = FPDFAnnot_GetSubtype(annotation);
            inspection.name = name;
            inspection.appearanceName = annotationDictionaryString(
                annotation,
                "Name");
            FPDFAnnot_GetRect(annotation, &inspection.rect);
            inspection.contents = annotationDictionaryString(
                annotation,
                "Contents");
            inspection.richContents = annotationDictionaryString(
                annotation,
                "RC");
            inspection.author = annotationDictionaryString(
                annotation,
                "T");
            inspection.subject = annotationDictionaryString(
                annotation,
                "Subj");
            inspection.creationDate = annotationDictionaryString(
                annotation,
                "CreationDate");
            inspection.modificationDate = annotationDictionaryString(
                annotation,
                "M");
            inspection.defaultAppearance = annotationDictionaryString(
                annotation,
                "DA");
            inspection.flags = FPDFAnnot_GetFlags(annotation);
            inspection.hasAppearance = FPDFAnnot_HasKey(
                annotation,
                "AP");
            inspection.appearance = annotationAppearanceString(annotation);
            inspection.relation = annotationDictionaryString(
                annotation,
                "RT");

            const size_t quadCount
                = FPDFAnnot_CountAttachmentPoints(annotation);
            inspection.quadPoints.reserve(
                static_cast<qsizetype>(quadCount));
            for (size_t quadIndex = 0;
                 quadIndex < quadCount;
                 ++quadIndex) {
                FS_QUADPOINTSF quad{};
                if (FPDFAnnot_GetAttachmentPoints(
                        annotation,
                        quadIndex,
                        &quad)) {
                    inspection.quadPoints.append(quad);
                }
            }

            FPDF_ANNOTATION popup = FPDFAnnot_GetLinkedAnnot(
                annotation,
                "Popup");
            if (popup) {
                inspection.popupIndex = FPDFPage_GetAnnotIndex(page, popup);
                FPDF_ANNOTATION popupParent = FPDFAnnot_GetLinkedAnnot(
                    popup,
                    "Parent");
                if (popupParent) {
                    inspection.popupParentMatches
                        = FPDFPage_GetAnnotIndex(page, popupParent) == index;
                    FPDFPage_CloseAnnot(popupParent);
                }
                FPDFPage_CloseAnnot(popup);
            }

            FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(
                annotation,
                "IRT");
            if (parent) {
                inspection.parentIndex = FPDFPage_GetAnnotIndex(page, parent);
                inspection.parentName = annotationDictionaryString(
                    parent,
                    "NM");
                FPDFPage_CloseAnnot(parent);
            }

            FPDFPage_CloseAnnot(annotation);
            break;
        }
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return inspection;
}

HighlightSaveInspection inspectTextMarkupAnnotations(
    const QString &filePath,
    FPDF_ANNOTATION_SUBTYPE subtype,
    int pageIndex)
{
    PDFiumProcessLock pdfiumLock;
    HighlightSaveInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    inspection.readable = document && page;
    if (page) {
        const int annotationCount = FPDFPage_GetAnnotCount(page);
        for (int index = 0; index < annotationCount; ++index) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
            if (!annotation) {
                continue;
            }
            if (FPDFAnnot_GetSubtype(annotation) != subtype) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            ++inspection.count;
            if (inspection.count == 1) {
                unsigned int red = 0;
                unsigned int green = 0;
                unsigned int blue = 0;
                unsigned int alpha = 0;
                if (FPDFAnnot_GetColor(
                        annotation,
                        FPDFANNOT_COLORTYPE_Color,
                        &red,
                        &green,
                        &blue,
                        &alpha)) {
                    inspection.color = QColor::fromRgb(
                        red,
                        green,
                        blue,
                        alpha);
                }
                inspection.hasAppearance = FPDFAnnot_HasKey(
                    annotation,
                    "AP");
                inspection.appearance = annotationAppearanceString(
                    annotation);
                inspection.appearanceObjectCount
                    = FPDFAnnot_GetObjectCount(annotation);
                for (int objectIndex = 0;
                     objectIndex < inspection.appearanceObjectCount;
                     ++objectIndex) {
                    FPDF_PAGEOBJECT object = FPDFAnnot_GetObject(
                        annotation,
                        objectIndex);
                    if (!object
                        || FPDFPageObj_GetType(object)
                            != FPDF_PAGEOBJ_PATH) {
                        continue;
                    }
                    ++inspection.appearancePathCount;
                    if (inspection.color.isValid()) {
                        continue;
                    }

                    int fillMode = FPDF_FILLMODE_NONE;
                    FPDF_BOOL stroke = false;
                    unsigned int pathRed = 0;
                    unsigned int pathGreen = 0;
                    unsigned int pathBlue = 0;
                    unsigned int pathAlpha = 255;
                    const bool hasDrawMode = FPDFPath_GetDrawMode(
                        object,
                        &fillMode,
                        &stroke);
                    const bool hasPathColor = hasDrawMode
                        && ((fillMode != FPDF_FILLMODE_NONE
                             && FPDFPageObj_GetFillColor(
                                 object,
                                 &pathRed,
                                 &pathGreen,
                                 &pathBlue,
                                 &pathAlpha))
                            || (stroke
                                && FPDFPageObj_GetStrokeColor(
                                    object,
                                    &pathRed,
                                    &pathGreen,
                                    &pathBlue,
                                    &pathAlpha)));
                    if (!hasPathColor) {
                        continue;
                    }

                    inspection.color = QColor::fromRgb(
                        pathRed,
                        pathGreen,
                        pathBlue,
                        pathAlpha);
                }
                inspection.flags = FPDFAnnot_GetFlags(annotation);
                FPDFAnnot_GetRect(annotation, &inspection.rect);
                inspection.author = annotationDictionaryString(
                    annotation,
                    "T");
                inspection.creationDate = annotationDictionaryString(
                    annotation,
                    "CreationDate");
                inspection.modificationDate = annotationDictionaryString(
                    annotation,
                    "M");
                inspection.contents = annotationDictionaryString(
                    annotation,
                    "Contents");
                const size_t quadCount
                    = FPDFAnnot_CountAttachmentPoints(annotation);
                inspection.quadPoints.reserve(
                    static_cast<qsizetype>(quadCount));
                for (size_t quadIndex = 0;
                     quadIndex < quadCount;
                     ++quadIndex) {
                    FS_QUADPOINTSF quad{};
                    if (FPDFAnnot_GetAttachmentPoints(
                            annotation,
                            quadIndex,
                            &quad)) {
                        inspection.quadPoints.append(quad);
                    }
                }
            }
            FPDFPage_CloseAnnot(annotation);
        }
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return inspection;
}

HighlightSaveInspection inspectHighlights(const QString &filePath,
                                          int pageIndex)
{
    return inspectTextMarkupAnnotations(
        filePath,
        FPDF_ANNOT_HIGHLIGHT,
        pageIndex);
}

StickyNoteSaveInspection inspectStickyNotes(const QString &filePath,
                                            int pageIndex)
{
    PDFiumProcessLock pdfiumLock;
    StickyNoteSaveInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    inspection.readable = document && page;
    if (page) {
        const int annotationCount = FPDFPage_GetAnnotCount(page);
        for (int index = 0; index < annotationCount; ++index) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
            if (!annotation) {
                continue;
            }
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_TEXT) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            ++inspection.count;
            inspection.names.append(annotationDictionaryString(
                annotation,
                "NM"));
            if (inspection.count == 1) {
                unsigned int red = 0;
                unsigned int green = 0;
                unsigned int blue = 0;
                unsigned int alpha = 0;
                if (FPDFAnnot_GetColor(
                        annotation,
                        FPDFANNOT_COLORTYPE_Color,
                        &red,
                        &green,
                        &blue,
                        &alpha)) {
                    inspection.color = QColor::fromRgb(
                        red,
                        green,
                        blue,
                        alpha);
                }
                inspection.flags = FPDFAnnot_GetFlags(annotation);
                FPDFAnnot_GetRect(annotation, &inspection.rect);
                inspection.contents = annotationDictionaryString(
                    annotation,
                    "Contents");
                inspection.author = annotationDictionaryString(
                    annotation,
                    "T");
                inspection.creationDate = annotationDictionaryString(
                    annotation,
                    "CreationDate");
                inspection.modificationDate = annotationDictionaryString(
                    annotation,
                    "M");
            }
            FPDFPage_CloseAnnot(annotation);
        }
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return inspection;
}

CommentStateInspection inspectCommentStateTransitions(
    const QString &filePath,
    int pageIndex)
{
    PDFiumProcessLock pdfiumLock;
    CommentStateInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    inspection.readable = document && page;
    if (page) {
        inspection.annotationCount = FPDFPage_GetAnnotCount(page);
        for (int annotationIndex = 0;
             annotationIndex < inspection.annotationCount;
             ++annotationIndex) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
                page,
                annotationIndex);
            if (!annotation) {
                continue;
            }

            const QString state = annotationDictionaryString(
                annotation,
                "State");
            const QString stateModel = annotationDictionaryString(
                annotation,
                "StateModel");
            if (state.isEmpty() || stateModel.isEmpty()) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            CommentStateTransitionInspection transition;
            transition.annotationIndex = annotationIndex;
            transition.name = annotationDictionaryString(
                annotation,
                "NM");
            transition.state = state;
            transition.stateModel = stateModel;
            transition.relation = annotationDictionaryString(
                annotation,
                "RT");
            transition.author = annotationDictionaryString(
                annotation,
                "T");
            transition.modificationDate = annotationDictionaryString(
                annotation,
                "M");
            FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(
                annotation,
                "IRT");
            if (parent) {
                transition.parentIndex = FPDFPage_GetAnnotIndex(
                    page,
                    parent);
                transition.parentName = annotationDictionaryString(
                    parent,
                    "NM");
                FPDFPage_CloseAnnot(parent);
            }
            inspection.transitions.append(std::move(transition));
            FPDFPage_CloseAnnot(annotation);
        }
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return inspection;
}

CommentReplyInspection inspectCommentReply(const QString &filePath,
                                           int pageIndex)
{
    PDFiumProcessLock pdfiumLock;
    CommentReplyInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    inspection.readable = document && page;
    if (page) {
        const int annotationCount = FPDFPage_GetAnnotCount(page);
        for (int annotationIndex = 0;
             annotationIndex < annotationCount;
             ++annotationIndex) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
                page,
                annotationIndex);
            if (!annotation) {
                continue;
            }
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_TEXT) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }
            ++inspection.textAnnotationCount;
            FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(
                annotation,
                "IRT");
            if (!parent) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }
            inspection.replyIndex = annotationIndex;
            inspection.rootIndex = FPDFPage_GetAnnotIndex(page, parent);
            inspection.replyContents = annotationDictionaryString(
                annotation,
                "Contents");
            inspection.relation = annotationDictionaryString(
                annotation,
                "RT");
            inspection.replyToThreadId = annotationDictionaryString(
                annotation,
                "TrePDFReplyTo");
            inspection.rootContents = annotationDictionaryString(
                parent,
                "Contents");
            inspection.parentThreadId = annotationDictionaryString(
                parent,
                "TrePDFThreadId");
            FPDFPage_CloseAnnot(parent);
            FPDFPage_CloseAnnot(annotation);
        }
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return inspection;
}

int annotationSubtypeCount(
    const QString &filePath,
    FPDF_ANNOTATION_SUBTYPE subtype,
    int pageIndex)
{
    PDFiumProcessLock pdfiumLock;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    if (!page) {
        if (document) {
            FPDF_CloseDocument(document);
        }
        return -1;
    }

    int count = 0;
    const int annotationCount = FPDFPage_GetAnnotCount(page);
    for (int annotationIndex = 0;
         annotationIndex < annotationCount;
         ++annotationIndex) {
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
            page,
            annotationIndex);
        if (annotation) {
            if (FPDFAnnot_GetSubtype(annotation) == subtype) {
                ++count;
            }
            FPDFPage_CloseAnnot(annotation);
        }
    }
    FPDF_ClosePage(page);
    FPDF_CloseDocument(document);
    return count;
}

InkAnnotationInspection inspectInkAnnotation(
    const QString &filePath,
    const QString &targetName,
    int pageIndex)
{
    PDFiumProcessLock pdfiumLock;
    InkAnnotationInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    inspection.readable = document && page;
    if (page) {
        const int annotationCount = FPDFPage_GetAnnotCount(page);
        for (int annotationIndex = 0;
             annotationIndex < annotationCount;
             ++annotationIndex) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
                page,
                annotationIndex);
            if (!annotation) {
                continue;
            }
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_INK) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }
            ++inspection.count;
            if (annotationDictionaryString(annotation, "NM")
                != targetName) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            inspection.annotationFound = true;
            const unsigned long pathCount
                = FPDFAnnot_GetInkListCount(annotation);
            inspection.paths.reserve(static_cast<qsizetype>(pathCount));
            for (unsigned long pathIndex = 0;
                 pathIndex < pathCount;
                 ++pathIndex) {
                const unsigned long pointCount = FPDFAnnot_GetInkListPath(
                    annotation,
                    pathIndex,
                    nullptr,
                    0);
                QVector<FS_POINTF> points(
                    static_cast<qsizetype>(pointCount));
                if (pointCount == 0
                    || FPDFAnnot_GetInkListPath(
                           annotation,
                           pathIndex,
                           points.data(),
                           pointCount) != pointCount) {
                    inspection.paths.clear();
                    break;
                }
                QVector<QPointF> path;
                path.reserve(points.size());
                for (const FS_POINTF &point : std::as_const(points)) {
                    path.append(QPointF(point.x, point.y));
                }
                inspection.paths.append(std::move(path));
            }
            FPDFPage_CloseAnnot(annotation);
        }
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return inspection;
}

bool hasSingleBidirectionalCommentPopup(const QString &filePath,
                                        int pageIndex)
{
    PDFiumProcessLock pdfiumLock;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    bool valid = false;
    if (page) {
        int commentIndex = -1;
        int popupIndex = -1;
        const int annotationCount = FPDFPage_GetAnnotCount(page);
        for (int annotationIndex = 0;
             annotationIndex < annotationCount;
             ++annotationIndex) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
                page,
                annotationIndex);
            if (!annotation) {
                continue;
            }
            const FPDF_ANNOTATION_SUBTYPE subtype
                = FPDFAnnot_GetSubtype(annotation);
            if (subtype == FPDF_ANNOT_TEXT) {
                commentIndex = commentIndex < 0 ? annotationIndex : -2;
            } else if (subtype == FPDF_ANNOT_POPUP) {
                popupIndex = popupIndex < 0 ? annotationIndex : -2;
            }
            FPDFPage_CloseAnnot(annotation);
        }

        FPDF_ANNOTATION comment = commentIndex >= 0
            ? FPDFPage_GetAnnot(page, commentIndex)
            : nullptr;
        FPDF_ANNOTATION popup = popupIndex >= 0
            ? FPDFPage_GetAnnot(page, popupIndex)
            : nullptr;
        FPDF_ANNOTATION forwardPopup = comment
            ? FPDFAnnot_GetLinkedAnnot(comment, "Popup")
            : nullptr;
        FPDF_ANNOTATION reverseParent = popup
            ? FPDFAnnot_GetLinkedAnnot(popup, "Parent")
            : nullptr;
        valid = forwardPopup
            && reverseParent
            && FPDFPage_GetAnnotIndex(page, forwardPopup) == popupIndex
            && FPDFPage_GetAnnotIndex(page, reverseParent) == commentIndex;
        if (reverseParent) {
            FPDFPage_CloseAnnot(reverseParent);
        }
        if (forwardPopup) {
            FPDFPage_CloseAnnot(forwardPopup);
        }
        if (popup) {
            FPDFPage_CloseAnnot(popup);
        }
        if (comment) {
            FPDFPage_CloseAnnot(comment);
        }
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return valid;
}

} // namespace PDFEditControllerTestSupport
