#include "PDFEditControllerTestSupport.h"
#include "PDFFileOpenHandler.h"
#include "PDFMergeController.h"
#include "PDFOCRController.h"
#include "SignatureLibrary.h"
#include "UserFontManager.h"
#include "platform/PlatformService.h"

namespace PDFEditControllerTestSupport {


bool writePdfObjects(const QString &filePath,
                     const QVector<QByteArray> &objects)
{
    QByteArray data("%PDF-1.7\n%\xe2\xe3\xcf\xd3\n");
    QVector<qint64> offsets(objects.size() + 1, 0);
    for (qsizetype index = 0; index < objects.size(); ++index) {
        offsets[index + 1] = data.size();
        data += QByteArray::number(index + 1);
        data += " 0 obj\n";
        data += objects.at(index);
        data += "\nendobj\n";
    }

    const qint64 xrefOffset = data.size();
    data += "xref\n0 ";
    data += QByteArray::number(objects.size() + 1);
    data += "\n0000000000 65535 f \n";
    for (qsizetype index = 1; index < offsets.size(); ++index) {
        data += QByteArray::number(offsets.at(index)).rightJustified(10, '0');
        data += " 00000 n \n";
    }
    data += "trailer\n<< /Size ";
    data += QByteArray::number(objects.size() + 1);
    data += " /Root 1 0 R >>\nstartxref\n";
    data += QByteArray::number(xrefOffset);
    data += "\n%%EOF\n";

    QFile file(filePath);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(data) == data.size();
}

QImage renderFirstPage(const QString &filePath,
                       const QSize &pixelSize,
                       int renderFlags,
                       const QRect &region)
{
    PDFiumProcessLock pdfiumLock;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    const QRect renderRect = region.isEmpty() ? QRect(QPoint(), pixelSize) : region;
    QImage image(renderRect.size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    FPDF_BITMAP bitmap = page
        ? FPDFBitmap_CreateEx(
              image.width(),
              image.height(),
              FPDFBitmap_BGRA,
              image.bits(),
              image.bytesPerLine())
        : nullptr;
    if (bitmap) {
        FPDF_RenderPageBitmap(
            bitmap,
            page,
            -renderRect.x(),
            -renderRect.y(),
            pixelSize.width(),
            pixelSize.height(),
            0,
            renderFlags);
        FPDFBitmap_Destroy(bitmap);
    } else {
        image = {};
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return image;
}

QImage renderFirstPageWithForms(
    const QString &filePath,
    const QSize &pixelSize,
    const QStringList &suppressedWidgetIds,
    const QPointF &focusedNormalizedPoint)
{
    PDFiumProcessLock pdfiumLock;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_FORMFILLINFO formFillInfo = {};
    formFillInfo.version = 2;
    formFillInfo.xfa_disabled = true;
    FPDF_FORMHANDLE formHandle = document
        ? FPDFDOC_InitFormFillEnvironment(document, &formFillInfo)
        : nullptr;
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    if (page && formHandle) {
        FORM_OnAfterLoadPage(page, formHandle);
    }
    bool focusPrepared = true;
    if (focusedNormalizedPoint.x() >= 0.0
        && focusedNormalizedPoint.x() <= 1.0
        && focusedNormalizedPoint.y() >= 0.0
        && focusedNormalizedPoint.y() <= 1.0) {
        double pageX = 0.0;
        double pageY = 0.0;
        focusPrepared = formHandle
            && page
            && FPDF_DeviceToPage(
                page,
                0,
                0,
                pixelSize.width(),
                pixelSize.height(),
                0,
                qRound(focusedNormalizedPoint.x() * pixelSize.width()),
                qRound(focusedNormalizedPoint.y() * pixelSize.height()),
                &pageX,
                &pageY)
            && FORM_OnFocus(
                formHandle,
                page,
                0,
                pageX,
                pageY);
    }
    PDFWidgetAnnotationSuppression widgetSuppression;
    bool widgetsSuppressed
        = widgetSuppression.suppress(page, suppressedWidgetIds);
    QVector<PDFWidgetRenderPatch> widgetPatches;

    QImage image(pixelSize, QImage::Format_ARGB32);
    image.fill(Qt::white);
    FPDF_BITMAP bitmap = page && formHandle && focusPrepared
            && widgetsSuppressed
        ? FPDFBitmap_CreateEx(
              image.width(),
              image.height(),
              FPDFBitmap_BGRA,
              image.bits(),
              image.bytesPerLine())
        : nullptr;
    if (bitmap) {
        FPDF_RenderPageBitmap(
            bitmap,
            page,
            0,
            0,
            image.width(),
            image.height(),
            0,
            FPDF_ANNOT);
        if (!suppressedWidgetIds.isEmpty()) {
            widgetsSuppressed = capturePDFWidgetRenderPatches(
                page,
                pixelSize,
                widgetSuppression.pageRects(),
                image,
                widgetPatches);
        }
        const bool widgetFlagsRestored
            = widgetSuppression.restore();
        FPDF_FFLDraw(
            formHandle,
            bitmap,
            page,
            0,
            0,
            image.width(),
            image.height(),
            0,
            FPDF_ANNOT);
        if (!suppressedWidgetIds.isEmpty()) {
            widgetsSuppressed = widgetsSuppressed
                && restorePDFWidgetRenderPatches(
                    image,
                    widgetPatches);
        }
        FPDFBitmap_Destroy(bitmap);
        if (!widgetFlagsRestored || !widgetsSuppressed) {
            image = {};
        }
    } else {
        image = {};
    }
    if (!widgetSuppression.restore()) {
        image = {};
    }
    if (page && formHandle) {
        FORM_OnBeforeClosePage(page, formHandle);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (formHandle) {
        FPDFDOC_ExitFormFillEnvironment(formHandle);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return image;
}

QImage renderFirstPageInViewer(const QString &filePath,
                               const QSize &logicalSize)
{
    if (logicalSize.isEmpty()) {
        return {};
    }

    PDFDocument document;
    PDFEditController editController;
    PDFFormController formController;
    if (!document.open(QUrl::fromLocalFile(filePath))) {
        return {};
    }
    editController.setDocument(&document);
    formController.setDocument(&document);

    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(logicalSize);

    TestablePDFPageItem pageItem(window.contentItem());
    pageItem.setWidth(logicalSize.width());
    pageItem.setHeight(logicalSize.height());
    pageItem.setEditController(&editController);
    pageItem.setFormController(&formController);
    pageItem.setDocument(&document);
    pageItem.setPage(0);
    pageItem.completeForTest();

    window.show();
    QElapsedTimer timer;
    timer.start();
    while ((!window.isExposed() || !pageItem.draftTextureCurrent())
           && timer.elapsed() < 5000) {
        QTest::qWait(10);
    }
    if (!window.isExposed() || !pageItem.draftTextureCurrent()) {
        return {};
    }

    const QSharedPointer<QQuickItemGrabResult> grabResult
        = pageItem.grabToImage(logicalSize);
    if (!grabResult) {
        return {};
    }
    QSignalSpy readySpy(grabResult.data(), &QQuickItemGrabResult::ready);
    timer.restart();
    while (grabResult->image().isNull()
           && readySpy.isEmpty()
           && timer.elapsed() < 5000) {
        QTest::qWait(10);
    }
    return grabResult->image();
}

qsizetype changedPixelCount(const QImage &left,
                            const QImage &right,
                            const QRect &area)
{
    if (left.isNull()
        || right.isNull()
        || left.size() != right.size()) {
        return 0;
    }
    const QRect boundedArea = area.intersected(left.rect());
    qsizetype changedPixels = 0;
    for (int y = boundedArea.top(); y <= boundedArea.bottom(); ++y) {
        for (int x = boundedArea.left(); x <= boundedArea.right(); ++x) {
            const QColor leftColor = left.pixelColor(x, y);
            const QColor rightColor = right.pixelColor(x, y);
            if (qAbs(leftColor.red() - rightColor.red()) > 8
                || qAbs(leftColor.green() - rightColor.green()) > 8
                || qAbs(leftColor.blue() - rightColor.blue()) > 8) {
                ++changedPixels;
            }
        }
    }
    return changedPixels;
}

QRect changedPixelBounds(const QImage &left,
                         const QImage &right,
                         const QRect &area)
{
    if (left.isNull()
        || right.isNull()
        || left.size() != right.size()) {
        return {};
    }

    const QRect boundedArea = area.intersected(left.rect());
    QRect bounds;
    for (int y = boundedArea.top(); y <= boundedArea.bottom(); ++y) {
        for (int x = boundedArea.left(); x <= boundedArea.right(); ++x) {
            const QColor leftColor = left.pixelColor(x, y);
            const QColor rightColor = right.pixelColor(x, y);
            const bool changed
                = qAbs(leftColor.red() - rightColor.red()) > 8
                || qAbs(leftColor.green() - rightColor.green()) > 8
                || qAbs(leftColor.blue() - rightColor.blue()) > 8;
            if (changed) {
                bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    return bounds;
}

QRect redPixelBounds(const QImage &image, const QRect &area)
{
    if (image.isNull()) {
        return {};
    }
    const QRect boundedArea = (area.isValid() ? area : image.rect())
        .intersected(image.rect());
    QRect bounds;
    for (int y = boundedArea.top(); y <= boundedArea.bottom(); ++y) {
        for (int x = boundedArea.left(); x <= boundedArea.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.red() > 170
                && pixel.red() > pixel.green() * 2
                && pixel.red() > pixel.blue() * 2) {
                bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    return bounds;
}

QRect darkChangedPixelBounds(const QImage &left,
                             const QImage &right,
                             const QRect &area)
{
    if (left.isNull()
        || right.isNull()
        || left.size() != right.size()) {
        return {};
    }

    const QRect boundedArea = area.intersected(left.rect());
    QRect bounds;
    for (int y = boundedArea.top(); y <= boundedArea.bottom(); ++y) {
        for (int x = boundedArea.left(); x <= boundedArea.right(); ++x) {
            const QColor leftColor = left.pixelColor(x, y);
            const QColor rightColor = right.pixelColor(x, y);
            const bool changed
                = qAbs(leftColor.red() - rightColor.red()) > 8
                || qAbs(leftColor.green() - rightColor.green()) > 8
                || qAbs(leftColor.blue() - rightColor.blue()) > 8;
            const bool dark = rightColor.red() < 96
                && rightColor.green() < 96
                && rightColor.blue() < 96;
            if (changed && dark) {
                bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    return bounds;
}

bool containsPatternColors(const QImage &image)
{
    bool red = false;
    bool green = false;
    bool blue = false;
    bool yellow = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            red = red || (color.red() > 220
                          && color.green() < 40
                          && color.blue() < 40);
            green = green || (color.green() > 220
                              && color.red() < 40
                              && color.blue() < 40);
            blue = blue || (color.blue() > 220
                            && color.red() < 40
                            && color.green() < 40);
            yellow = yellow || (color.red() > 220
                                && color.green() > 220
                                && color.blue() < 40);
        }
    }
    return red && green && blue && yellow;
}

bool createDifferentPageSizesPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                "/Resources << >> >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 320 480] "
                "/Resources << >> >>"),
        });
}

bool createThreePageTextPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    const QVector<QSizeF> pageSizes{
        QSizeF(240.0, 360.0),
        QSizeF(320.0, 480.0),
        QSizeF(500.0, 700.0),
    };
    const QStringList pageTexts{
        QStringLiteral("First fixture page"),
        QStringLiteral("Second fixture page"),
        QStringLiteral("Third fixture page"),
    };
    bool succeeded = true;
    for (qsizetype index = 0; succeeded && index < pageSizes.size(); ++index) {
        const QSizeF pageSize = pageSizes.at(index);
        FPDF_PAGE page = FPDFPage_New(
            document,
            static_cast<int>(index),
            pageSize.width(),
            pageSize.height());
        succeeded = page
            && addTextObject(
                document,
                page,
                "Helvetica",
                pageTexts.at(index),
                30.0,
                pageSize.height() - 48.0)
            && FPDFPage_GenerateContent(page);
        if (page) {
            FPDF_ClosePage(page);
        }
    }

    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }

    FPDF_CloseDocument(document);
    return succeeded;
}

bool createOffsetCropPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    FPDF_PAGE page = FPDFPage_New(document, 0, 600.0, 800.0);
    if (page) {
        FPDFPage_SetCropBox(page, 50.0F, 100.0F, 550.0F, 700.0F);
    }
    bool succeeded = page
        && addTextObject(
            document,
            page,
            "Helvetica",
            QStringLiteral("Offset crop text"),
            100.0,
            600.0)
        && FPDFPage_GenerateContent(page);
    if (page) {
        FPDF_ClosePage(page);
    }
    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    FPDF_CloseDocument(document);
    return succeeded;
}

bool createRenderModePdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    FPDF_PAGE page = FPDFPage_New(document, 0, 595.0, 842.0);
    bool succeeded = page
        && addStyledTextObject(
            document,
            page,
            QStringLiteral("Stroke text"),
            FPDF_TEXTRENDERMODE_STROKE,
            QColor(20, 80, 160),
            1.75F,
            72.0,
            700.0)
        && addStyledTextObject(
            document,
            page,
            QStringLiteral("Clip text"),
            FPDF_TEXTRENDERMODE_FILL_CLIP,
            Qt::black,
            1.0F,
            72.0,
            640.0)
        && FPDFPage_GenerateContent(page);
    if (page) {
        FPDF_ClosePage(page);
    }
    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    FPDF_CloseDocument(document);
    return succeeded;
}

bool createInvisibleOcrTextPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    FPDF_PAGE page = FPDFPage_New(document, 0, 400.0, 400.0);
    bool succeeded = page
        && addStyledTextObject(
            document,
            page,
            QStringLiteral("Hidden OCR text"),
            FPDF_TEXTRENDERMODE_INVISIBLE,
            Qt::black,
            1.0F,
            40.0,
            300.0)
        && addTextObject(
            document,
            page,
            "Helvetica",
            QStringLiteral("Visible text"),
            40.0,
            100.0)
        && FPDFPage_GenerateContent(page);
    if (page) {
        FPDF_ClosePage(page);
    }
    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    FPDF_CloseDocument(document);
    return succeeded;
}

bool createVerticalTextPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    FPDF_PAGE page = FPDFPage_New(document, 0, 400.0, 400.0);
    const FS_MATRIX verticalMatrix{
        0.0F,
        1.0F,
        -1.0F,
        0.0F,
        300.0F,
        80.0F};
    bool succeeded = page
        && addTextObject(
            document,
            page,
            "Helvetica",
            QStringLiteral("Upright text"),
            40.0,
            320.0)
        && addTransformedTextObject(
            document,
            page,
            QStringLiteral("Vertical text"),
            verticalMatrix)
        && FPDFPage_GenerateContent(page);
    if (page) {
        FPDF_ClosePage(page);
    }
    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    FPDF_CloseDocument(document);
    return succeeded;
}

bool createTopLevelImagePdf(
    const QString &filePath,
    const FS_MATRIX &imageMatrix,
    int pageRotation,
    bool offsetCrop)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    FPDF_PAGE page = FPDFPage_New(document, 0, 595.0, 842.0);
    if (page) {
        FPDFPage_SetRotation(page, pageRotation);
        if (offsetCrop) {
            FPDFPage_SetCropBox(page, 50.0F, 100.0F, 550.0F, 750.0F);
        }
    }
    FPDF_BITMAP bitmap = page ? FPDFBitmap_Create(64, 32, 1) : nullptr;
    FPDF_PAGEOBJECT imageObject = bitmap
        ? FPDFPageObj_NewImageObj(document)
        : nullptr;
    FPDF_PAGE loadedPages[] = {page};
    bool imageInsertionAttempted = false;
    bool succeeded = page
        && bitmap
        && imageObject
        && FPDFBitmap_FillRect(bitmap, 0, 0, 64, 32, 0xffd86f4b)
        && FPDFBitmap_FillRect(bitmap, 0, 16, 64, 16, 0xff326ad1)
        && FPDFBitmap_FillRect(bitmap, 32, 0, 32, 16, 0xff45ac68)
        && FPDFImageObj_SetBitmap(loadedPages, 1, imageObject, bitmap)
        && FPDFPageObj_SetMatrix(imageObject, &imageMatrix)
        && (imageInsertionAttempted = true)
        && FPDFPage_InsertObject(page, imageObject)
        && addTextObject(
            document,
            page,
            "Helvetica",
            QString::fromLatin1(kImageFixtureText),
            72.0,
            740.0)
        && FPDFPage_GenerateContent(page);

    // FPDFPage_InsertObject() takes ownership on success and frees the object
    // itself on failure. Only destroy objects that never reached that call.
    if (imageObject && !imageInsertionAttempted) {
        FPDFPageObj_Destroy(imageObject);
    }
    if (bitmap) {
        FPDFBitmap_Destroy(bitmap);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    FPDF_CloseDocument(document);
    return succeeded;
}

bool createRotatedPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }
    FPDF_PAGE page = FPDFPage_New(document, 0, 595.0, 842.0);
    if (page) {
        FPDFPage_SetRotation(page, 1);
    }
    bool succeeded = page
        && addTextObject(
            document,
            page,
            "Helvetica",
            QStringLiteral("Rotation source"),
            72.0,
            700.0)
        && FPDFPage_GenerateContent(page);
    if (page) {
        FPDF_ClosePage(page);
    }
    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    FPDF_CloseDocument(document);
    return succeeded;
}

QVariantMap candidateForText(const QVariantList &candidates, const QString &text)
{
    for (const QVariant &candidateValue : candidates) {
        const QVariantMap candidate = candidateValue.toMap();
        if (candidate.value(QStringLiteral("kind")).toString() == QStringLiteral("text")
            && candidate.value(QStringLiteral("text")).toString().trimmed() == text) {
            return candidate;
        }
    }
    return {};
}

QVariantMap candidateForKind(const QVariantList &candidates, const QString &kind)
{
    for (const QVariant &candidateValue : candidates) {
        const QVariantMap candidate = candidateValue.toMap();
        if (candidate.value(QStringLiteral("kind")).toString() == kind) {
            return candidate;
        }
    }
    return {};
}

QVariantList transformedInkStrokes(const QVariantList &strokes,
                                   const QRectF &sourceFrame,
                                   const QRectF &targetFrame)
{
    QVariantList transformed;
    if (sourceFrame.width() <= 0.0 || sourceFrame.height() <= 0.0) {
        return transformed;
    }

    const qreal horizontalScale
        = targetFrame.width() / sourceFrame.width();
    const qreal verticalScale
        = targetFrame.height() / sourceFrame.height();
    transformed.reserve(strokes.size());
    for (const QVariant &strokeValue : strokes) {
        const QVariantList stroke = strokeValue.toList();
        QVariantList transformedStroke;
        transformedStroke.reserve(stroke.size());
        for (const QVariant &pointValue : stroke) {
            const QPointF point = pointValue.toPointF();
            transformedStroke.append(QVariant::fromValue(QPointF(
                targetFrame.left()
                    + (point.x() - sourceFrame.left()) * horizontalScale,
                targetFrame.top()
                    + (point.y() - sourceFrame.top()) * verticalScale)));
        }
        transformed.append(QVariant::fromValue(transformedStroke));
    }
    return transformed;
}

QVariantList transformedDrawingVertices(const QVariantList &vertices,
                                        const QRectF &sourceFrame,
                                        const QRectF &targetFrame)
{
    QVariantList transformed;
    if (sourceFrame.width() <= 0.0 || sourceFrame.height() <= 0.0) {
        return transformed;
    }

    const qreal horizontalScale
        = targetFrame.width() / sourceFrame.width();
    const qreal verticalScale
        = targetFrame.height() / sourceFrame.height();
    transformed.reserve(vertices.size());
    for (const QVariant &pointValue : vertices) {
        const QPointF point = pointValue.toPointF();
        transformed.append(QVariant::fromValue(QPointF(
            targetFrame.left()
                + (point.x() - sourceFrame.left()) * horizontalScale,
            targetFrame.top()
                + (point.y() - sourceFrame.top()) * verticalScale)));
    }
    return transformed;
}

QVariantList waitForCandidates(PDFEditController &controller, int page)
{
    QSignalSpy candidatesReadySpy(
        &controller,
        &PDFEditController::pageCandidatesReady);
    QVariantList candidates = controller.candidatesForPage(page);
    if (candidates.isEmpty() && candidatesReadySpy.isEmpty()) {
        candidatesReadySpy.wait(2000);
    }
    return candidates.isEmpty()
        ? controller.candidatesForPage(page)
        : candidates;
}

QRectF rectFromCoordinateMap(const QVariant &value)
{
    const QVariantMap map = value.toMap();
    return QRectF(
        map.value(QStringLiteral("x")).toDouble(),
        map.value(QStringLiteral("y")).toDouble(),
        map.value(QStringLiteral("width")).toDouble(),
        map.value(QStringLiteral("height")).toDouble());
}

QRectF boundingRectForCoordinateMaps(const QVariantList &values)
{
    QRectF bounds;
    for (const QVariant &value : values) {
        const QRectF rect = rectFromCoordinateMap(value);
        bounds = bounds.isEmpty() ? rect : bounds.united(rect);
    }
    return bounds;
}

QString editOverlayQmlPath()
{
#ifdef Q_OS_WIN
    return QFINDTESTDATA("../src/Components/Windows/EditOverlay.qml");
#else
    return QFINDTESTDATA("../src/Components/EditOverlay.qml");
#endif
}

bool registerQmlTestTypes(const QString &themePath)
{
    static bool registered = false;
    if (registered) {
        return true;
    }
    const QString canonicalThemePath = QFileInfo(themePath).canonicalFilePath();
    if (canonicalThemePath.isEmpty()) {
        return false;
    }

    const bool cppTypesRegistered = qmlRegisterSingletonType<PDFFileOpenHandler>(
                                        "TrePDF", 1, 0, "FileOpenHandler",
                                        &PDFFileOpenHandler::create) >= 0
        && qmlRegisterSingletonType<PlatformService>(
               "TrePDF", 1, 0, "PlatformService",
               [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject * {
                   auto *application = QCoreApplication::instance();
                   if (!application->findChild<PlatformService *>(
                           QString(), Qt::FindDirectChildrenOnly)) {
                       new PlatformService(
                           PDFFileOpenHandler::create(engine, scriptEngine), application);
                   }
                   return PlatformService::create(engine, scriptEngine);
               }) >= 0
        && qmlRegisterType<PDFDocument>(
                                        "TrePDF",
                                        1,
                                        0,
                                        "PDFDocument") >= 0
        && qmlRegisterType<PDFEditController>(
               "TrePDF",
               1,
               0,
               "PDFEditController") >= 0
        && qmlRegisterType<SignatureLibrary>(
               "TrePDF", 1, 0, "SignatureLibrary") >= 0
        && qmlRegisterType<UserFontManager>(
               "TrePDF", 1, 0, "UserFontManager") >= 0
        && qmlRegisterType<PDFSearchController>(
               "TrePDF",
               1,
               0,
               "PDFSearchController") >= 0
        && qmlRegisterType<PDFTextSelectionController>(
               "TrePDF", 1, 0, "PDFTextSelectionController") >= 0
        && qmlRegisterType<PDFTextSelectionItem>(
               "TrePDF", 1, 0, "PDFTextSelectionItem") >= 0
        && qmlRegisterType<PDFMergeController>("TrePDF", 1, 0, "PDFMergeController") >= 0
        && qmlRegisterType<PDFExcelExportController>(
               "TrePDF", 1, 0, "PDFExcelExportController") >= 0
        && qmlRegisterType<PDFOCRController>(
               "TrePDF", 1, 0, "PDFOCRController") >= 0
        && qmlRegisterType<PDFFormController>(
               "TrePDF",
               1,
               0,
               "PDFFormController") >= 0
        && qmlRegisterType<PDFFormDesignerController>(
               "TrePDF",
               1,
               0,
               "PDFFormDesignerController") >= 0
        && qmlRegisterType<PDFFormView>(
               "TrePDF",
               1,
               0,
               "PDFFormView") >= 0
        && qmlRegisterType<FormFieldLayer>(
               "TrePDF",
               1,
               0,
               "FormFieldLayer") >= 0
        && qmlRegisterType<PDFPageItem>(
               "TrePDF",
               1,
               0,
               "PDFPageView") >= 0
        && qmlRegisterType<DraftImageItem>(
               "TrePDF",
               1,
               0,
               "DraftImageView") >= 0
        && qmlRegisterUncreatableType<PDFDocumentSession>(
               "TrePDF",
               1,
               0,
               "PDFDocumentSession",
               QStringLiteral(
                   "PDF document sessions are created by PDFTabModel")) >= 0
        && qmlRegisterType<PDFTabModel>(
               "TrePDF",
               1,
               0,
               "PDFTabModel") >= 0
        && qmlRegisterType(
               QUrl::fromLocalFile(canonicalThemePath),
               "TrePDF",
               1,
               0,
               "AppTheme") >= 0;
    if (!cppTypesRegistered) {
        return false;
    }

    const QDir componentsDirectory(QFileInfo(canonicalThemePath).absolutePath());
    const QStringList qmlFiles = componentsDirectory.entryList(
        {QStringLiteral("*.qml")},
        QDir::Files,
        QDir::Name);
    for (const QString &fileName : qmlFiles) {
        const QString typeName = QFileInfo(fileName).completeBaseName();
        if (typeName == QLatin1String("AppTheme")) {
            continue;
        }
        const QString qmlPath = typeName == QLatin1String("EditOverlay")
            ? editOverlayQmlPath()
            : componentsDirectory.absoluteFilePath(fileName);
        if (qmlPath.isEmpty()) {
            return false;
        }
        const QByteArray encodedTypeName = typeName.toUtf8();
        if (qmlRegisterType(
                QUrl::fromLocalFile(qmlPath),
                "TrePDF",
                1,
                0,
                encodedTypeName.constData()) < 0) {
            return false;
        }
    }
    const QDir platformDirectory(componentsDirectory.filePath(QStringLiteral("ios")));
    const QStringList platformFiles = platformDirectory.entryList(
        {QStringLiteral("*.qml")}, QDir::Files, QDir::Name);
    if (platformFiles.isEmpty())
        return false;
    for (const QString &fileName : platformFiles) {
        const QByteArray typeName = QFileInfo(fileName).completeBaseName().toUtf8();
        if (qmlRegisterType(QUrl::fromLocalFile(platformDirectory.filePath(fileName)),
                            "TrePDF", 1, 0, typeName.constData()) < 0)
            return false;
    }
    registered = true;
    return registered;
}

} // namespace PDFEditControllerTestSupport
