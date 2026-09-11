#include "PDFEditControllerTestSupport.h"

namespace PDFEditControllerTestSupport {


qsizetype skipPdfLiteralString(const QByteArray &data, qsizetype offset)
{
    int depth = 1;
    for (qsizetype index = offset + 1; index < data.size(); ++index) {
        const char value = data.at(index);
        if (value == '\\') {
            if (index + 1 < data.size()) {
                ++index;
                if (data.at(index) == '\r'
                    && index + 1 < data.size()
                    && data.at(index + 1) == '\n') {
                    ++index;
                }
            }
            continue;
        }
        if (value == '(') {
            ++depth;
        } else if (value == ')' && --depth == 0) {
            return index + 1;
        }
    }
    return data.size();
}

qsizetype skipPdfHexString(const QByteArray &data, qsizetype offset)
{
    const qsizetype end = data.indexOf('>', offset + 1);
    return end >= 0 ? end + 1 : data.size();
}

qsizetype skipPdfComment(const QByteArray &data, qsizetype offset)
{
    qsizetype index = offset + 1;
    while (index < data.size()
           && data.at(index) != '\r'
           && data.at(index) != '\n') {
        ++index;
    }
    return index;
}

PdfDictionaryRange pdfDictionaryRangeAt(const QByteArray &data,
                                        qsizetype dictionaryStart)
{
    if (dictionaryStart < 0
        || dictionaryStart + 1 >= data.size()
        || data.at(dictionaryStart) != '<'
        || data.at(dictionaryStart + 1) != '<') {
        return {};
    }

    int depth = 0;
    for (qsizetype index = dictionaryStart; index < data.size();) {
        const char value = data.at(index);
        if (value == '%') {
            index = skipPdfComment(data, index);
            continue;
        }
        if (value == '(') {
            index = skipPdfLiteralString(data, index);
            continue;
        }
        if (value == '<') {
            if (index + 1 < data.size() && data.at(index + 1) == '<') {
                ++depth;
                index += 2;
            } else {
                index = skipPdfHexString(data, index);
            }
            continue;
        }
        if (value == '>'
            && index + 1 < data.size()
            && data.at(index + 1) == '>') {
            --depth;
            index += 2;
            if (depth == 0) {
                return PdfDictionaryRange{dictionaryStart, index};
            }
            if (depth < 0) {
                return {};
            }
            continue;
        }
        ++index;
    }
    return {};
}

bool isPdfWhitespace(char value)
{
    return value == '\0'
        || value == '\t'
        || value == '\n'
        || value == '\f'
        || value == '\r'
        || value == ' ';
}

bool isPdfDelimiter(char value)
{
    return value == '('
        || value == ')'
        || value == '<'
        || value == '>'
        || value == '['
        || value == ']'
        || value == '{'
        || value == '}'
        || value == '/'
        || value == '%';
}

qsizetype skipPdfWhitespaceAndComments(const QByteArray &data,
                                       qsizetype offset,
                                       qsizetype end)
{
    qsizetype index = offset;
    while (index < end) {
        if (isPdfWhitespace(data.at(index))) {
            ++index;
            continue;
        }
        if (data.at(index) == '%') {
            index = skipPdfComment(data, index);
            continue;
        }
        break;
    }
    return index;
}

qsizetype pdfDirectValueOffset(
    const QByteArray &data,
    const PdfDictionaryRange &parent,
    const QByteArray &key)
{
    if (!parent.isValid()) {
        return -1;
    }

    int dictionaryDepth = 0;
    int arrayDepth = 0;
    for (qsizetype index = parent.start; index < parent.end;) {
        const char value = data.at(index);
        if (value == '%') {
            index = skipPdfComment(data, index);
            continue;
        }
        if (value == '(') {
            index = skipPdfLiteralString(data, index);
            continue;
        }
        if (value == '<') {
            if (index + 1 < parent.end && data.at(index + 1) == '<') {
                ++dictionaryDepth;
                index += 2;
            } else {
                index = skipPdfHexString(data, index);
            }
            continue;
        }
        if (value == '>'
            && index + 1 < parent.end
            && data.at(index + 1) == '>') {
            --dictionaryDepth;
            index += 2;
            continue;
        }
        if (value == '[') {
            ++arrayDepth;
            ++index;
            continue;
        }
        if (value == ']') {
            --arrayDepth;
            ++index;
            continue;
        }
        if (value != '/'
            || dictionaryDepth != 1
            || arrayDepth != 0) {
            ++index;
            continue;
        }

        const qsizetype nameStart = index + 1;
        qsizetype nameEnd = nameStart;
        while (nameEnd < parent.end
               && !isPdfWhitespace(data.at(nameEnd))
               && !isPdfDelimiter(data.at(nameEnd))) {
            ++nameEnd;
        }
        if (data.mid(nameStart, nameEnd - nameStart) != key) {
            index = nameEnd;
            continue;
        }
        return skipPdfWhitespaceAndComments(
            data,
            nameEnd,
            parent.end);
    }
    return -1;
}

PdfDictionaryRange pdfDirectDictionaryValue(
    const QByteArray &data,
    const PdfDictionaryRange &parent,
    const QByteArray &key)
{
    const qsizetype valueStart = pdfDirectValueOffset(data, parent, key);
    const PdfDictionaryRange valueRange = pdfDictionaryRangeAt(
        data,
        valueStart);
    return valueRange.isValid() && valueRange.end <= parent.end
        ? valueRange
        : PdfDictionaryRange();
}

PdfIndirectReference pdfDirectIndirectReference(
    const QByteArray &data,
    const PdfDictionaryRange &parent,
    const QByteArray &key)
{
    const qsizetype valueStart = pdfDirectValueOffset(data, parent, key);
    if (valueStart < 0 || valueStart >= parent.end) {
        return {};
    }
    const QString valueText = QString::fromLatin1(
        data.mid(valueStart, parent.end - valueStart));
    const QRegularExpression referenceExpression(
        QStringLiteral(R"(^(\d+)\s+(\d+)\s+R\b)"));
    const QRegularExpressionMatch match = referenceExpression.match(valueText);
    if (!match.hasMatch()) {
        return {};
    }
    bool objectNumberValid = false;
    bool generationValid = false;
    const int objectNumber = match.captured(1).toInt(&objectNumberValid);
    const int generation = match.captured(2).toInt(&generationValid);
    return objectNumberValid && generationValid
        ? PdfIndirectReference{objectNumber, generation}
        : PdfIndirectReference();
}

PdfDictionaryRange pdfDictionaryContainingOffset(const QByteArray &data,
                                                 qsizetype offset)
{
    if (offset < 0 || offset >= data.size()) {
        return {};
    }

    const qsizetype objectKeyword = data.lastIndexOf(" obj", offset);
    const qsizetype scanStart = objectKeyword >= 0
        ? objectKeyword + 4
        : 0;
    QVector<qsizetype> dictionaryStarts;
    for (qsizetype index = scanStart; index < offset;) {
        const char value = data.at(index);
        if (value == '%') {
            index = skipPdfComment(data, index);
            continue;
        }
        if (value == '(') {
            index = skipPdfLiteralString(data, index);
            continue;
        }
        if (value == '<') {
            if (index + 1 < data.size() && data.at(index + 1) == '<') {
                dictionaryStarts.append(index);
                index += 2;
            } else {
                index = skipPdfHexString(data, index);
            }
            continue;
        }
        if (value == '>'
            && index + 1 < data.size()
            && data.at(index + 1) == '>') {
            if (dictionaryStarts.isEmpty()) {
                return {};
            }
            dictionaryStarts.removeLast();
            index += 2;
            continue;
        }
        ++index;
    }
    return dictionaryStarts.isEmpty()
        ? PdfDictionaryRange()
        : pdfDictionaryRangeAt(data, dictionaryStarts.constLast());
}

QRegularExpressionMatch lastRegularExpressionMatch(
    const QRegularExpression &expression,
    const QString &text)
{
    QRegularExpressionMatch lastMatch;
    auto iterator = expression.globalMatch(text);
    while (iterator.hasNext()) {
        lastMatch = iterator.next();
    }
    return lastMatch;
}

QByteArray annotationDictionaryBytes(const QString &filePath,
                                     const QString &targetName)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = file.readAll();
    const QRegularExpression nameExpression(
        QStringLiteral(R"(/NM\s*\(%1\))")
            .arg(QRegularExpression::escape(targetName)));
    const QRegularExpressionMatch nameMatch = lastRegularExpressionMatch(
        nameExpression,
        QString::fromLatin1(data));
    if (!nameMatch.hasMatch()) {
        return {};
    }
    const PdfDictionaryRange range = pdfDictionaryContainingOffset(
        data,
        nameMatch.capturedStart());
    return range.isValid()
        ? data.mid(range.start, range.end - range.start)
        : QByteArray();
}

PdfDictionaryRange pdfIndirectObjectDictionary(
    const QByteArray &data,
    int objectNumber,
    int generation,
    bool *isStream)
{
    if (isStream) {
        *isStream = false;
    }
    const QString pdfText = QString::fromLatin1(data);
    const QRegularExpression objectExpression(
        QStringLiteral(
            R"((?:^|[\r\n])\s*%1\s+%2\s+obj\b)")
            .arg(objectNumber)
            .arg(generation));
    const QRegularExpressionMatch objectMatch
        = lastRegularExpressionMatch(objectExpression, pdfText);
    if (!objectMatch.hasMatch()) {
        return {};
    }

    const qsizetype endObject = data.indexOf(
        "endobj",
        objectMatch.capturedEnd());
    const qsizetype dictionaryStart = data.indexOf(
        "<<",
        objectMatch.capturedEnd());
    if (endObject < 0
        || dictionaryStart < 0
        || dictionaryStart >= endObject) {
        return {};
    }
    const PdfDictionaryRange range = pdfDictionaryRangeAt(
        data,
        dictionaryStart);
    if (!range.isValid() || range.end > endObject) {
        return {};
    }
    if (isStream) {
        const qsizetype streamKeyword = skipPdfWhitespaceAndComments(
            data,
            range.end,
            endObject);
        const qsizetype endStream = data.indexOf(
            "endstream",
            streamKeyword + 6);
        *isStream = streamKeyword + 6 <= endObject
            && data.mid(streamKeyword, 6) == QByteArrayLiteral("stream")
            && (streamKeyword + 6 == endObject
                || isPdfWhitespace(data.at(streamKeyword + 6)))
            && endStream >= streamKeyword + 6
            && endStream < endObject;
    }
    return range;
}

// FPDFAnnot_GetAP() exposes only decoded drawing commands. Inspect the final
// serialized object graph as well so a named graphics state cannot go unresolved.
AnnotationAppearanceStructureInspection inspectAnnotationAppearanceStructure(
    const QString &filePath,
    const QString &targetName)
{
    AnnotationAppearanceStructureInspection inspection;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return inspection;
    }
    const QByteArray data = file.readAll();
    inspection.readable = true;
    const QString pdfText = QString::fromLatin1(data);
    const QRegularExpression nameExpression(
        QStringLiteral(R"(/NM\s*\(%1\))")
            .arg(QRegularExpression::escape(targetName)));
    const QRegularExpressionMatch nameMatch
        = lastRegularExpressionMatch(nameExpression, pdfText);
    if (!nameMatch.hasMatch()) {
        return inspection;
    }

    const PdfDictionaryRange annotationRange
        = pdfDictionaryContainingOffset(data, nameMatch.capturedStart());
    if (!annotationRange.isValid()) {
        return inspection;
    }
    inspection.annotationFound = true;
    const PdfDictionaryRange appearanceDictionaryRange
        = pdfDirectDictionaryValue(data, annotationRange, "AP");
    const PdfIndirectReference normalAppearanceReference
        = pdfDirectIndirectReference(data, appearanceDictionaryRange, "N");
    inspection.normalAppearanceIsIndirect
        = normalAppearanceReference.isValid();
    if (!inspection.normalAppearanceIsIndirect) {
        return inspection;
    }
    inspection.normalAppearanceObjectNumber
        = normalAppearanceReference.objectNumber;
    inspection.normalAppearanceGeneration
        = normalAppearanceReference.generation;

    const PdfDictionaryRange appearanceRange = pdfIndirectObjectDictionary(
        data,
        normalAppearanceReference.objectNumber,
        normalAppearanceReference.generation,
        &inspection.appearanceIsStream);
    if (!appearanceRange.isValid()) {
        return inspection;
    }
    inspection.appearanceObjectFound = true;
    const QString appearanceDictionary = QString::fromLatin1(
        data.mid(
            appearanceRange.start,
            appearanceRange.end - appearanceRange.start));
    inspection.typeIsXObject = QRegularExpression(
        QStringLiteral(R"(/Type\s*/XObject\b)"))
        .match(appearanceDictionary)
        .hasMatch();
    inspection.subtypeIsForm = QRegularExpression(
        QStringLiteral(R"(/Subtype\s*/Form\b)"))
        .match(appearanceDictionary)
        .hasMatch();
    inspection.matrixIsIdentity = QRegularExpression(
        QStringLiteral(
            R"(/Matrix\s*\[\s*1\s+0\s+0\s+1\s+0\s+0\s*\])"))
        .match(appearanceDictionary)
        .hasMatch();

    const PdfDictionaryRange resourcesRange
        = pdfDirectDictionaryValue(data, appearanceRange, "Resources");
    inspection.hasResources = resourcesRange.isValid();
    const PdfDictionaryRange extGStateRange
        = pdfDirectDictionaryValue(data, resourcesRange, "ExtGState");
    inspection.hasExtGState = extGStateRange.isValid();
    const PdfDictionaryRange graphicsStateRange
        = pdfDirectDictionaryValue(data, extGStateRange, "GS");
    inspection.hasGraphicsState = graphicsStateRange.isValid();
    const QString graphicsStateDictionary = graphicsStateRange.isValid()
        ? QString::fromLatin1(
              data.mid(
                  graphicsStateRange.start,
                  graphicsStateRange.end - graphicsStateRange.start))
        : QString();
    inspection.graphicsStateTypeIsExtGState = QRegularExpression(
        QStringLiteral(R"(/Type\s*/ExtGState\b)"))
        .match(graphicsStateDictionary)
        .hasMatch();
    inspection.blendModeIsNormal = QRegularExpression(
        QStringLiteral(R"(/BM\s*/Normal\b)"))
        .match(graphicsStateDictionary)
        .hasMatch();
    inspection.alphaSourceIsFalse = QRegularExpression(
        QStringLiteral(R"(/AIS\s+false\b)"))
        .match(graphicsStateDictionary)
        .hasMatch();

    static const QString numberPattern
        = QStringLiteral(R"(([+-]?(?:\d+(?:\.\d*)?|\.\d+)))");
    const QRegularExpression boundingBoxExpression(
        QStringLiteral(
            R"(/BBox\s*\[\s*%1\s+%1\s+%1\s+%1\s*\])")
            .arg(numberPattern));
    const QRegularExpressionMatch boundingBoxMatch
        = boundingBoxExpression.match(appearanceDictionary);
    if (boundingBoxMatch.hasMatch()) {
        for (int capture = 1; capture <= 4; ++capture) {
            bool valid = false;
            const qreal value = boundingBoxMatch
                .captured(capture)
                .toDouble(&valid);
            if (!valid) {
                inspection.boundingBox.clear();
                break;
            }
            inspection.boundingBox.append(value);
        }
    }

    const QRegularExpression strokingOpacityExpression(
        QStringLiteral(R"(/CA\s+%1(?=[\s/\]>]))")
            .arg(numberPattern));
    const QRegularExpression nonStrokingOpacityExpression(
        QStringLiteral(R"(/ca\s+%1(?=[\s/\]>]))")
            .arg(numberPattern));
    bool strokingOpacityValid = false;
    inspection.strokingOpacity = strokingOpacityExpression
        .match(graphicsStateDictionary)
        .captured(1)
        .toDouble(&strokingOpacityValid);
    if (!strokingOpacityValid) {
        inspection.strokingOpacity
            = std::numeric_limits<qreal>::quiet_NaN();
    }
    bool nonStrokingOpacityValid = false;
    inspection.nonStrokingOpacity = nonStrokingOpacityExpression
        .match(graphicsStateDictionary)
        .captured(1)
        .toDouble(&nonStrokingOpacityValid);
    if (!nonStrokingOpacityValid) {
        inspection.nonStrokingOpacity
            = std::numeric_limits<qreal>::quiet_NaN();
    }
    return inspection;
}

ShapeSaveInspection inspectShapePaths(const QString &filePath)
{
    ShapeSaveInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    inspection.readable = document && page;
    if (page) {
        const qreal pageWidth = FPDF_GetPageWidthF(page);
        const qreal pageHeight = FPDF_GetPageHeightF(page);
        const int deviceWidth = qMax(1, qRound(pageWidth * 1000.0));
        const int deviceHeight = qMax(1, qRound(pageHeight * 1000.0));
        const int objectCount = FPDFPage_CountObjects(page);
        for (int objectIndex = 0;
             objectIndex < objectCount;
             ++objectIndex) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(
                page,
                objectIndex);
            if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH) {
                continue;
            }

            ++inspection.count;
            for (int markIndex = 0;
                 markIndex < FPDFPageObj_CountMarks(object);
                 ++markIndex) {
                FPDF_WCHAR name[32] = {};
                unsigned long byteCount = 0;
                if (FPDFPageObjMark_GetName(
                        FPDFPageObj_GetMark(object, markIndex),
                        name, sizeof(name), &byteCount)
                    && byteCount <= sizeof(name)
                    && QString::fromUtf16(reinterpret_cast<const char16_t *>(name))
                        == QStringLiteral("TrePDFShape")) {
                    ++inspection.managedCount;
                }
            }
            const auto pathColor = [object](bool interior) {
                unsigned int red = 0;
                unsigned int green = 0;
                unsigned int blue = 0;
                unsigned int alpha = 0;
                const bool available = interior
                    ? FPDFPageObj_GetFillColor(object, &red, &green, &blue, &alpha)
                    : FPDFPageObj_GetStrokeColor(object, &red, &green, &blue, &alpha);
                return available ? QColor(red, green, blue, alpha) : QColor();
            };
            inspection.strokeColors.append(pathColor(false));
            inspection.fillColors.append(pathColor(true));
            int fillMode = FPDF_FILLMODE_NONE;
            FPDF_BOOL stroke = false;
            if (FPDFPath_GetDrawMode(object, &fillMode, &stroke)
                && fillMode != FPDF_FILLMODE_NONE) {
                ++inspection.filledCount;
                unsigned int red = 0;
                unsigned int green = 0;
                unsigned int blue = 0;
                unsigned int alpha = 0;
                if (FPDFPageObj_GetFillColor(
                        object,
                        &red,
                        &green,
                        &blue,
                        &alpha)
                    && alpha == 0) {
                    ++inspection.transparentFillCount;
                }
            } else {
                ++inspection.openCount;
            }

            unsigned int red = 0;
            unsigned int green = 0;
            unsigned int blue = 0;
            unsigned int alpha = 0;
            if (FPDFPageObj_GetStrokeColor(
                    object,
                    &red,
                    &green,
                    &blue,
                    &alpha)
                && red == 0
                && green == 0
                && blue == 0
                && alpha == 255) {
                ++inspection.blackStrokeCount;
            }

            const int segmentCount = FPDFPath_CountSegments(object);
            QVector<QPointF> normalizedPoints;
            normalizedPoints.reserve(segmentCount);
            for (int segmentIndex = 0;
                 segmentIndex < segmentCount;
                 ++segmentIndex) {
                FPDF_PATHSEGMENT segment = FPDFPath_GetPathSegment(
                    object,
                    segmentIndex);
                float pageX = 0.0F;
                float pageY = 0.0F;
                int deviceX = 0;
                int deviceY = 0;
                if (segment
                    && FPDFPathSegment_GetPoint(
                        segment,
                        &pageX,
                        &pageY)
                    && FPDF_PageToDevice(
                        page,
                        0,
                        0,
                        deviceWidth,
                        deviceHeight,
                        0,
                        pageX,
                        pageY,
                        &deviceX,
                        &deviceY)) {
                    normalizedPoints.append(QPointF(
                        static_cast<qreal>(deviceX) / deviceWidth,
                        static_cast<qreal>(deviceY) / deviceHeight));
                }
                if (segment
                    && FPDFPathSegment_GetType(segment)
                        == FPDF_SEGMENT_BEZIERTO) {
                    ++inspection.bezierCount;
                    break;
                }
            }
            inspection.normalizedPathPoints.append(
                std::move(normalizedPoints));
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

bool createUnmarkedShapeContentPdf(const QString &filePath,
                                  const QByteArray &content,
                                  const QByteArray &pageGeometry)
{
    return writePdfObjects(filePath, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R ") + pageGeometry
            + QByteArray(" /Resources << /ExtGState << /GS0 << /CA 1 /ca 0 >> "
                   "/GS1 << /CA 1 /ca 0.5 >> >> >> "
                   "/Contents 4 0 R >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n") + content + QByteArray("\nendstream")
    });
}

bool tamperMarkedShapePath(const QString &sourcePath,
                          const QString &targetPath,
                          bool changeGeometry)
{
    const QByteArray encodedPath = QFile::encodeName(sourcePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    bool changed = false;
    if (page) {
        for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_PATH
                || FPDFPageObj_CountMarks(object) != 1) {
                continue;
            }
            if (changeGeometry) {
                FPDFPageObj_Transform(object, 1, 0, 0, 1, 25, 0);
                changed = true;
            } else {
                const QByteArray invalidJson("{broken shape metadata");
                changed = FPDFPageObjMark_SetBlobParam(
                    document, object, FPDFPageObj_GetMark(object, 0), "Data",
                    reinterpret_cast<const unsigned char *>(invalidJson.constData()),
                    static_cast<unsigned long>(invalidJson.size()));
            }
            break;
        }
    }
    bool saved = changed && FPDFPage_GenerateContent(page);
    if (page) {
        FPDF_ClosePage(page);
    }
    if (saved) {
        PdfFileWriter writer(targetPath);
        saved = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return saved;
}

ImageSaveInspection inspectImages(const QString &filePath)
{
    ImageSaveInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    inspection.readable = document && page;
    if (page) {
        const int objectCount = FPDFPage_CountObjects(page);
        for (int index = 0; index < objectCount; ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_IMAGE) {
                continue;
            }
            ++inspection.count;
            if (inspection.count == 1) {
                inspection.firstTopLevelIndex = index;
                inspection.matrixReadable = FPDFPageObj_GetMatrix(
                    object,
                    &inspection.matrix);
                if (inspection.matrixReadable) {
                    // Use PDFium's public page projection independently of the
                    // writer's affine composition, including CropBox/Rotate.
                    constexpr int deviceExtent = 1000000;
                    for (const QPointF unitPoint : {
                             QPointF(0, 0), QPointF(1, 0),
                             QPointF(1, 1), QPointF(0, 1)}) {
                        const FS_MATRIX &matrix = inspection.matrix;
                        int deviceX = 0;
                        int deviceY = 0;
                        if (FPDF_PageToDevice(
                                page, 0, 0, deviceExtent, deviceExtent, 0,
                                matrix.a * unitPoint.x() + matrix.c * unitPoint.y() + matrix.e,
                                matrix.b * unitPoint.x() + matrix.d * unitPoint.y() + matrix.f,
                                &deviceX, &deviceY)) {
                            inspection.normalizedCorners.append(QPointF(
                                qreal(deviceX) / deviceExtent,
                                qreal(deviceY) / deviceExtent));
                        }
                    }
                }
                FPDFImageObj_GetImagePixelSize(
                    object,
                    &inspection.pixelWidth,
                    &inspection.pixelHeight);
                FPDF_BITMAP bitmap = FPDFImageObj_GetBitmap(object);
                const int format = bitmap
                    ? FPDFBitmap_GetFormat(bitmap)
                    : FPDFBitmap_Unknown;
                const int width = bitmap ? FPDFBitmap_GetWidth(bitmap) : 0;
                const int height = bitmap ? FPDFBitmap_GetHeight(bitmap) : 0;
                const int stride = bitmap ? FPDFBitmap_GetStride(bitmap) : 0;
                const auto *buffer = bitmap
                    ? static_cast<const uchar *>(FPDFBitmap_GetBuffer(bitmap))
                    : nullptr;
                const int bytesPerPixel = format == FPDFBitmap_BGR ? 3 : 4;
                if (buffer
                    && width > 0
                    && height > 1
                    && stride >= width * bytesPerPixel
                    && (format == FPDFBitmap_BGR
                        || format == FPDFBitmap_BGRx
                        || format == FPDFBitmap_BGRA)) {
                    const auto pixelAt = [&](int row) {
                        const uchar *pixel = buffer
                            + static_cast<qsizetype>(row) * stride;
                        return QColor(
                            pixel[2],
                            pixel[1],
                            pixel[0],
                            format == FPDFBitmap_BGRA ? pixel[3] : 255);
                    };
                    inspection.pixelsReadable = true;
                    inspection.topLeftPixel = pixelAt(0);
                    inspection.bottomLeftPixel = pixelAt(height - 1);
                    inspection.pixels = QImage(width, height, QImage::Format_ARGB32);
                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            const uchar *pixel = buffer
                                + static_cast<qsizetype>(y) * stride
                                + x * bytesPerPixel;
                            inspection.pixels.setPixelColor(x, y, QColor(
                                pixel[2], pixel[1], pixel[0],
                                format == FPDFBitmap_BGRA ? pixel[3] : 255));
                        }
                    }
                }
                if (bitmap) {
                    FPDFBitmap_Destroy(bitmap);
                }
            }
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

} // namespace PDFEditControllerTestSupport
