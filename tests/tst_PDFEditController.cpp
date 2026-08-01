#include "ApplicationFonts.h"
#include "DraftImageItem.h"
#include "FormFieldLayer.h"
#include "PDFDocument.h"
#include "PDFEditController.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFFormAppearance.h"
#include "PDFFormWorker.h"
#include "PDFSaveWriter.h"
#include "PDFTabModel.h"
#include "PDFWidgetAnnotationSuppression.h"
#include "PDFiumSynchronization.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickTextDocument>
#include <QRawFont>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_formfill.h>
#include <fpdf_fwlevent.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace {

constexpr char kRegularText[] = "Regular PDF text";
constexpr char kBoldText[] = "Bold PDF text";
constexpr char kGroupedText[] = "Hello world";
constexpr char kMixedRegularText[] = "Plain";
constexpr char kMixedBoldText[] = "Strong";
constexpr char kLeftColumnText[] = "Left column";
constexpr char kRightColumnText[] = "Right column";
constexpr char kUprightText[] = "Upright";
constexpr char kObliqueText[] = "Oblique";
constexpr char kStretchedText[] = "Scale";
constexpr char kImageFixtureText[] = "Image fixture text";
constexpr char kFontCollisionTargetText[] = "Wxyz";

class PdfFileWriter : public FPDF_FILEWRITE
{
public:
    explicit PdfFileWriter(const QString &filePath)
        : m_file(filePath)
    {
        version = 1;
        WriteBlock = &PdfFileWriter::writeBlock;
    }

    bool open()
    {
        return m_file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    }

private:
    static int writeBlock(FPDF_FILEWRITE *fileWriter,
                          const void *data,
                          unsigned long size)
    {
        auto *writer = static_cast<PdfFileWriter *>(fileWriter);
        const qint64 byteCount = static_cast<qint64>(size);
        return writer->m_file.write(static_cast<const char *>(data), byteCount) == byteCount;
    }

    QFile m_file;
};

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

bool createPushButtonActionPdf(
    const QString &filePath,
    const QByteArray &uri = QByteArray("https://example.com/"))
{
    const QByteArray appearanceContent(
        "q\n"
        "0.96 0.94 0.91 rg\n"
        "0 0 80 40 re f\n"
        "0.75 0.35 0.10 RG\n"
        "1 w\n"
        "0.5 0.5 79 39 re S\n"
        "Q\n");
    const QByteArray appearanceStream
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [0 0 80 40] /Resources << >> /Length ")
        + QByteArray::number(appearanceContent.size())
        + QByteArray(" >>\nstream\n")
        + appearanceContent
        + QByteArray("endstream");

    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R "
                "/AcroForm 9 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] "
                "/Count 3 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> "
                "/Annots [6 0 R 7 0 R 8 0 R] >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Btn "
                "/Ff 65536 /T (PrevButton) /P 4 0 R /F 4 "
                "/Rect [20 400 100 440] /H /P "
                "/AP << /N 10 0 R >> "
                "/A << /S /GoTo /D [3 0 R /Fit] >> >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Btn "
                "/Ff 65536 /T (UriButton) /P 4 0 R /F 4 "
                "/Rect [140 400 220 440] /H /P "
                "/AP << /N 10 0 R >> /A << /S /URI /URI (")
                + uri
                + QByteArray(") >> >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Btn "
                "/Ff 65536 /T (NextButton) /P 4 0 R /F 4 "
                "/Rect [260 400 340 440] /H /P "
                "/AP << /N 10 0 R >> "
                "/A << /S /GoTo /D [5 0 R /Fit] >> >>"),
            QByteArray(
                "<< /Fields [6 0 R 7 0 R 8 0 R] "
                "/NeedAppearances false >>"),
            appearanceStream,
        });
}

bool createChoiceFieldsPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R "
                "/AcroForm 7 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> "
                "/Annots [4 0 R 5 0 R 6 0 R 9 0 R 10 0 R 11 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 131072 /T (Transport) /TU (Transport choice) "
                "/P 3 0 R /F 4 /Rect [20 400 180 430] "
                "/Opt [(Airplane) (Bicycle) (Car)] "
                "/V (Airplane) /I [0] /DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 0 /T (Month) /TU (Month list) "
                "/P 3 0 R /F 4 /Rect [20 260 180 380] "
                "/Opt [(March) (April) (May) (June)] "
                "/V (April) /I [1] /DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 2097152 /T (Colors) /TU (Color list) "
                "/P 3 0 R /F 4 /Rect [200 260 340 380] "
                "/Opt [(Red) (Yellow) (Blue)] "
                "/V [(Red) (Blue)] /I [0 2] "
                "/DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Fields [4 0 R 5 0 R 6 0 R 9 0 R 10 0 R 11 0 R] "
                "/NeedAppearances true "
                "/DA (/Helv 12 Tf 0 g) "
                "/DR << /Font << /Helv 8 0 R >> >> >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 "
                "/BaseFont /Helvetica >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 393216 /T (EditableTransport) "
                "/P 3 0 R /F 4 /Rect [20 210 180 240] "
                "/Opt [(Airplane) (Bicycle)] "
                "/V (Airplane) /I [0] /DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 67239936 /T (ImmediateTransport) "
                "/P 3 0 R /F 4 /Rect [200 210 340 240] "
                "/Opt [(Airplane) (Bicycle)] "
                "/V (Airplane) /I [0] /DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 131072 /T (ScriptedTransport) "
                "/P 3 0 R /F 4 /Rect [20 160 180 190] "
                "/Opt [(Airplane) (Bicycle)] "
                "/V (Airplane) /I [0] /DA (/Helv 12 Tf 0 g) "
                "/AA << /D << /S /JavaScript /JS () >> >> >>"),
        });
}

QVector<int> selectedChoiceOptions(
    const QString &filePath,
    int annotationIndex)
{
    PDFiumProcessLock pdfiumLock;
    QVector<int> selectedIndices;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    if (!document) {
        return selectedIndices;
    }

    FPDF_FORMFILLINFO formFillInfo = {};
    formFillInfo.version = 2;
    formFillInfo.xfa_disabled = true;
    FPDF_FORMHANDLE formHandle = FPDFDOC_InitFormFillEnvironment(
        document,
        &formFillInfo);
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (formHandle && page) {
        FORM_OnAfterLoadPage(page, formHandle);
        FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(
            page,
            annotationIndex);
        if (annotation) {
            const int optionCount = FPDFAnnot_GetOptionCount(
                formHandle,
                annotation);
            for (int optionIndex = 0;
                 optionIndex < optionCount;
                 ++optionIndex) {
                if (FPDFAnnot_IsOptionSelected(
                        formHandle,
                        annotation,
                        optionIndex)) {
                    selectedIndices.append(optionIndex);
                }
            }
            FPDFPage_CloseAnnot(annotation);
        }
        FORM_OnBeforeClosePage(page, formHandle);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (formHandle) {
        FPDFDOC_ExitFormFillEnvironment(formHandle);
    }
    FPDF_CloseDocument(document);
    return selectedIndices;
}

bool createArialSubsetCollisionSourcePdf(const QString &filePath)
{
    const QByteArray pageContent(
        "BT\n"
        "/F1 10 Tf\n"
        "40 760 Td\n"
        "(\\301) Tj\n"
        "ET\n"
        "BT\n"
        "/F2 10 Tf\n"
        "40 700 Td\n"
        "(Wxyz) Tj\n"
        "ET\n");
    const QByteArray contentStream = QByteArray("<< /Length ")
        + QByteArray::number(pageContent.size())
        + QByteArray(" >>\nstream\n")
        + pageContent
        + QByteArray("endstream");

    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                "/Resources << /Font << /F1 4 0 R /F2 5 0 R >> >> "
                "/Contents 6 0 R >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Courier "
                "/Encoding /WinAnsiEncoding >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Arial "
                "/Encoding /WinAnsiEncoding >>"),
            contentStream,
        });
}

enum class PatternPaintMode {
    Fill,
    Stroke
};

bool createPatternPaintPdf(const QString &filePath, PatternPaintMode mode)
{
    const QByteArray paintContent = mode == PatternPaintMode::Fill
        ? QByteArray(
              "/Pattern cs\n"
              "/P1 scn\n"
              "20 20 160 80 re f\n")
        : QByteArray(
              "/Pattern CS\n"
              "/P1 SCN\n"
              "12 w\n"
              "20 20 160 80 re S\n");
    const QByteArray pageContent = QByteArray("q\n")
        + paintContent
        + QByteArray(
              "Q\n"
              "BT\n"
              "/F1 20 Tf\n"
              "20 160 Td\n"
              "(Pattern edit text) Tj\n"
              "ET\n");
    const QByteArray patternContent(
        "1 0 0 rg 0 0 10 10 re f\n"
        "0 1 0 rg 10 0 10 10 re f\n"
        "0 0 1 rg 0 10 10 10 re f\n"
        "1 1 0 rg 10 10 10 10 re f\n");
    const auto streamObject = [](const QByteArray &dictionary,
                                 const QByteArray &stream) {
        return dictionary + " /Length " + QByteArray::number(stream.size())
            + " >>\nstream\n" + stream + "\nendstream";
    };

    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /F1 4 0 R >> "
                "/Pattern << /P1 6 0 R >> >> /Contents 5 0 R >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
            streamObject("<<", pageContent),
            streamObject(
                "<< /Type /Pattern /PatternType 1 /PaintType 1 "
                "/TilingType 1 /BBox [0 0 20 20] /XStep 20 /YStep 20 "
                "/Resources << >>",
                patternContent),
        });
}

bool createPatternFillPdf(const QString &filePath)
{
    return createPatternPaintPdf(filePath, PatternPaintMode::Fill);
}

bool createPatternStrokePdf(const QString &filePath)
{
    return createPatternPaintPdf(filePath, PatternPaintMode::Stroke);
}

bool createGrayscaleWhiteFillPdf(const QString &filePath)
{
    const QByteArray pageContent(
        "/P <</MCID 0>> BDC\n"
        "1 g\n"
        "20 20 160 80 re f*\n"
        "EMC\n"
        "0 g\n"
        "BT\n"
        "/F1 20 Tf\n"
        "20 160 Td\n"
        "(Grayscale white fill) Tj\n"
        "ET\n");
    const auto contentStream = [](const QByteArray &content) {
        return QByteArray("<< /Length ")
            + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n")
            + content
            + QByteArray("endstream");
    };

    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /F1 4 0 R >> >> "
                "/Contents 5 0 R >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
            contentStream(pageContent),
        });
}

QImage renderFirstPage(const QString &filePath, const QSize &pixelSize)
{
    PDFiumProcessLock pdfiumLock;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    QImage image(pixelSize, QImage::Format_ARGB32);
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
            0,
            0,
            image.width(),
            image.height(),
            0,
            0);
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
    const QStringList &suppressedWidgetIds = {},
    const QPointF &focusedNormalizedPoint = QPointF(-1.0, -1.0))
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

bool addTextObject(FPDF_DOCUMENT document,
                   FPDF_PAGE page,
                   const char *fontName,
                   const QString &text,
                   double x,
                   double y,
                   double horizontalScale = 1.0)
{
    FPDF_PAGEOBJECT textObject = FPDFPageObj_NewTextObj(document, fontName, 24.0F);
    if (!textObject) {
        return false;
    }

    const auto *wideText = reinterpret_cast<const FPDF_WCHAR *>(text.utf16());
    if (!FPDFText_SetText(textObject, wideText)) {
        FPDFPageObj_Destroy(textObject);
        return false;
    }

    FPDFPageObj_Transform(
        textObject,
        horizontalScale,
        0.0,
        0.0,
        1.0,
        x,
        y);
    return FPDFPage_InsertObject(page, textObject);
}

bool addTextObjectWithFont(FPDF_DOCUMENT document,
                           FPDF_PAGE page,
                           FPDF_FONT font,
                           const QString &text,
                           double x,
                           double y)
{
    FPDF_PAGEOBJECT textObject = FPDFPageObj_CreateTextObj(
        document,
        font,
        24.0F);
    if (!textObject) {
        return false;
    }
    const auto *wideText = reinterpret_cast<const FPDF_WCHAR *>(text.utf16());
    if (!FPDFText_SetText(textObject, wideText)) {
        FPDFPageObj_Destroy(textObject);
        return false;
    }
    FPDFPageObj_Transform(textObject, 1.0, 0.0, 0.0, 1.0, x, y);
    return FPDFPage_InsertObject(page, textObject);
}

bool addStyledTextObject(FPDF_DOCUMENT document,
                         FPDF_PAGE page,
                         const QString &text,
                         FPDF_TEXT_RENDERMODE renderMode,
                         const QColor &color,
                         float strokeWidth,
                         double x,
                         double y)
{
    FPDF_PAGEOBJECT textObject = FPDFPageObj_NewTextObj(
        document,
        "Helvetica",
        24.0F);
    if (!textObject) {
        return false;
    }
    const auto *wideText = reinterpret_cast<const FPDF_WCHAR *>(text.utf16());
    const bool usesStroke = renderMode == FPDF_TEXTRENDERMODE_STROKE
        || renderMode == FPDF_TEXTRENDERMODE_FILL_STROKE
        || renderMode == FPDF_TEXTRENDERMODE_STROKE_CLIP
        || renderMode == FPDF_TEXTRENDERMODE_FILL_STROKE_CLIP;
    bool configured = FPDFText_SetText(textObject, wideText)
        && FPDFTextObj_SetTextRenderMode(textObject, renderMode);
    if (configured && usesStroke) {
        configured = FPDFPageObj_SetStrokeColor(
            textObject,
            static_cast<unsigned int>(color.red()),
            static_cast<unsigned int>(color.green()),
            static_cast<unsigned int>(color.blue()),
            static_cast<unsigned int>(color.alpha()))
            && FPDFPageObj_SetStrokeWidth(textObject, strokeWidth);
    } else if (configured) {
        configured = FPDFPageObj_SetFillColor(
            textObject,
            static_cast<unsigned int>(color.red()),
            static_cast<unsigned int>(color.green()),
            static_cast<unsigned int>(color.blue()),
            static_cast<unsigned int>(color.alpha()));
    }
    if (!configured) {
        FPDFPageObj_Destroy(textObject);
        return false;
    }
    FPDFPageObj_Transform(textObject, 1.0, 0.0, 0.0, 1.0, x, y);
    return FPDFPage_InsertObject(page, textObject);
}

bool createFontStylePdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    FPDF_PAGE page = FPDFPage_New(document, 0, 595.0, 842.0);
    bool succeeded = page
        && addTextObject(document,
                         page,
                         "Helvetica",
                         QString::fromLatin1(kRegularText),
                         72.0,
                         740.0)
        && addTextObject(document,
                         page,
                         "Helvetica-Bold",
                         QString::fromLatin1(kBoldText),
                         72.0,
                         680.0)
        && addTextObject(document,
                         page,
                         "Helvetica",
                         QStringLiteral("Hello"),
                         72.0,
                         620.0)
        && addTextObject(document,
                         page,
                         "Helvetica",
                         QStringLiteral("world"),
                         140.0,
                         620.0)
        && addTextObject(document,
                         page,
                         "Helvetica",
                         QString::fromLatin1(kMixedRegularText),
                         72.0,
                         560.0)
        && addTextObject(document,
                         page,
                         "Helvetica-Bold",
                         QString::fromLatin1(kMixedBoldText),
                         140.0,
                         560.0)
        && addTextObject(document,
                         page,
                         "Helvetica",
                         QString::fromLatin1(kLeftColumnText),
                         72.0,
                         500.0)
        && addTextObject(document,
                         page,
                         "Helvetica",
                         QString::fromLatin1(kRightColumnText),
                         360.0,
                         500.0)
        && addTextObject(document,
                         page,
                         "Helvetica",
                         QString::fromLatin1(kUprightText),
                         72.0,
                         440.0)
        && addTextObject(document,
                         page,
                         "Helvetica-Oblique",
                         QString::fromLatin1(kObliqueText),
                         170.0,
                         440.0)
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

bool createStretchedTextPdf(const QString &filePath,
                            qreal horizontalScale = 2.0)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    FPDF_PAGE page = FPDFPage_New(document, 0, 595.0, 842.0);
    bool succeeded = page
        && addTextObject(
            document,
            page,
            "Courier",
            QString::fromLatin1(kStretchedText),
            72.0,
            740.0,
            horizontalScale)
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

bool createEmbeddedArialPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    const QByteArray fontData = ApplicationFonts::bundledFontData(
        QStringLiteral("Arial"),
        false,
        false);
    FPDF_FONT font = fontData.isEmpty()
        ? nullptr
        : FPDFText_LoadFont(
              document,
              reinterpret_cast<const uint8_t *>(fontData.constData()),
              static_cast<uint32_t>(fontData.size()),
              FPDF_FONT_TRUETYPE,
              true);
    FPDF_PAGE page = font ? FPDFPage_New(document, 0, 595.0, 842.0) : nullptr;
    bool succeeded = page
        && addTextObjectWithFont(
            document,
            page,
            font,
            QStringLiteral("Tiếng Việt gốc"),
            72.0,
            740.0)
        && addTextObjectWithFont(
            document,
            page,
            font,
            QStringLiteral("Không đổi"),
            72.0,
            680.0)
        && FPDFPage_GenerateContent(page);
    if (page) {
        FPDF_ClosePage(page);
    }
    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    if (font) {
        FPDFFont_Close(font);
    }
    FPDF_CloseDocument(document);
    return succeeded;
}

bool createEmbeddedFontPdf(const QString &filePath,
                           const QByteArray &fontData,
                           const QString &text)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document || fontData.isEmpty()) {
        if (document) {
            FPDF_CloseDocument(document);
        }
        return false;
    }

    FPDF_FONT font = FPDFText_LoadFont(
        document,
        reinterpret_cast<const uint8_t *>(fontData.constData()),
        static_cast<uint32_t>(fontData.size()),
        FPDF_FONT_TRUETYPE,
        true);
    FPDF_PAGE page = font ? FPDFPage_New(document, 0, 595.0, 842.0) : nullptr;
    bool succeeded = page
        && addTextObjectWithFont(document, page, font, text, 72.0, 740.0)
        && FPDFPage_GenerateContent(page);
    if (page) {
        FPDF_ClosePage(page);
    }
    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
    }
    if (font) {
        FPDFFont_Close(font);
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

bool createTopLevelImagePdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    FPDF_PAGE page = FPDFPage_New(document, 0, 595.0, 842.0);
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
        && FPDFImageObj_SetBitmap(loadedPages, 1, imageObject, bitmap)
        && FPDFImageObj_SetMatrix(
            imageObject,
            120.0,
            0.0,
            0.0,
            60.0,
            360.0,
            620.0)
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

QString pageText(const QString &filePath, int pageIndex = 0)
{
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    if (!document) {
        return {};
    }
    FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
    QString text;
    if (textPage) {
        const int count = FPDFText_CountChars(textPage);
        QVector<unsigned short> buffer(count + 1, 0);
        const int written = FPDFText_GetText(textPage, 0, count, buffer.data());
        if (written > 1) {
            text = QString::fromUtf16(
                reinterpret_cast<const char16_t *>(buffer.constData()),
                written - 1);
        }
    }
    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(document);
    return text;
}

QString normalizedPageText(const QString &filePath, int pageIndex = 0)
{
    QString text = pageText(filePath, pageIndex);
    text.replace(QChar(0xfffe), QLatin1Char(' '));
    text.replace(QChar(0xffff), QLatin1Char(' '));
    return text.simplified();
}

QString pageObjectText(FPDF_PAGEOBJECT object, FPDF_TEXTPAGE textPage)
{
    const unsigned long byteLength = FPDFTextObj_GetText(object, textPage, nullptr, 0);
    if (byteLength < sizeof(FPDF_WCHAR)) {
        return {};
    }

    QVector<FPDF_WCHAR> buffer(
        static_cast<qsizetype>((byteLength + sizeof(FPDF_WCHAR) - 1)
                               / sizeof(FPDF_WCHAR)));
    const unsigned long actualLength = FPDFTextObj_GetText(
        object,
        textPage,
        buffer.data(),
        byteLength);
    qsizetype characterCount = static_cast<qsizetype>(
        actualLength / sizeof(FPDF_WCHAR));
    while (characterCount > 0 && buffer.at(characterCount - 1) == 0) {
        --characterCount;
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(buffer.constData()),
        characterCount);
}

QString baseFontName(FPDF_PAGEOBJECT textObject)
{
    const FPDF_FONT font = FPDFTextObj_GetFont(textObject);
    const size_t byteLength = font
        ? FPDFFont_GetBaseFontName(font, nullptr, 0)
        : 0;
    if (byteLength <= 1) {
        return {};
    }

    QByteArray buffer(static_cast<qsizetype>(byteLength), Qt::Uninitialized);
    if (FPDFFont_GetBaseFontName(
            font,
            buffer.data(),
            static_cast<size_t>(buffer.size()))
        != byteLength) {
        return {};
    }
    buffer.chop(1);
    return QString::fromUtf8(buffer);
}

QString fontFamilyName(FPDF_FONT font)
{
    const size_t byteLength = font
        ? FPDFFont_GetFamilyName(font, nullptr, 0)
        : 0;
    if (byteLength <= 1) {
        return {};
    }

    QByteArray buffer(static_cast<qsizetype>(byteLength), Qt::Uninitialized);
    if (FPDFFont_GetFamilyName(
            font,
            buffer.data(),
            static_cast<size_t>(buffer.size()))
        != byteLength) {
        return {};
    }
    buffer.chop(1);
    return QString::fromUtf8(buffer);
}

bool hasSubsetFontPrefix(const QString &baseName)
{
    constexpr qsizetype prefixLength = 6;
    if (baseName.size() <= prefixLength
        || baseName.at(prefixLength) != QLatin1Char('+')) {
        return false;
    }
    for (qsizetype index = 0; index < prefixLength; ++index) {
        const QChar character = baseName.at(index);
        if (character < QLatin1Char('A') || character > QLatin1Char('Z')) {
            return false;
        }
    }
    return true;
}

QString compactFontName(const QString &name)
{
    QString result;
    result.reserve(name.size());
    for (const QChar character : name) {
        if (character.isLetterOrNumber()) {
            result.append(character.toLower());
        }
    }
    return result;
}

struct TextObjectFontInfo {
    bool found = false;
    QString baseName;
    QString familyName;
    size_t dataSize = 0;
    bool italic = false;
    int embedded = -1;
};

TextObjectFontInfo textObjectFontInfo(const QString &filePath,
                                      const QString &text)
{
    TextObjectFontInfo info;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
    if (page && textPage) {
        const int objectCount = FPDFPage_CountObjects(page);
        for (int index = 0; index < objectCount; ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT
                || !pageObjectText(object, textPage).contains(text)) {
                continue;
            }

            const FPDF_FONT font = FPDFTextObj_GetFont(object);
            info.found = font;
            info.baseName = baseFontName(object);
            info.familyName = fontFamilyName(font);
            if (font) {
                FPDFFont_GetFontData(font, nullptr, 0, &info.dataSize);
            }
            int italicAngle = 0;
            info.italic = font
                && FPDFFont_GetItalicAngle(font, &italicAngle)
                && italicAngle != 0;
            info.embedded = font ? FPDFFont_GetIsEmbedded(font) : -1;
            break;
        }
    }
    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return info;
}

bool textObjectsShareFont(const QString &filePath,
                          const QString &firstText,
                          const QString &secondText)
{
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
    FPDF_FONT firstFont = nullptr;
    FPDF_FONT secondFont = nullptr;
    if (page && textPage) {
        const int objectCount = FPDFPage_CountObjects(page);
        for (int index = 0; index < objectCount; ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT) {
                continue;
            }
            const QString text = pageObjectText(object, textPage);
            if (!firstFont && text.contains(firstText)) {
                firstFont = FPDFTextObj_GetFont(object);
            }
            if (!secondFont && text.contains(secondText)) {
                secondFont = FPDFTextObj_GetFont(object);
            }
        }
    }
    const bool shareFont = firstFont && firstFont == secondFont;
    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return shareFont;
}

QStringList topLevelTextObjects(const QString &filePath)
{
    QStringList texts;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
    if (page && textPage) {
        const int objectCount = FPDFPage_CountObjects(page);
        for (int index = 0; index < objectCount; ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_TEXT) {
                texts.append(pageObjectText(object, textPage));
            }
        }
    }
    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return texts;
}

bool textObjectOrigin(const QString &filePath,
                      const QString &text,
                      QPointF &origin)
{
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
    bool found = false;
    if (page && textPage) {
        const int objectCount = FPDFPage_CountObjects(page);
        for (int index = 0; index < objectCount; ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT
                || !pageObjectText(object, textPage).contains(text)) {
                continue;
            }
            FS_MATRIX matrix{};
            if (FPDFPageObj_GetMatrix(object, &matrix)) {
                origin = QPointF(matrix.e, matrix.f);
                found = true;
                break;
            }
        }
    }
    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return found;
}

bool textObjectMatrix(const QString &filePath,
                      const QString &text,
                      FS_MATRIX &matrix)
{
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
    bool found = false;
    if (page && textPage) {
        const int objectCount = FPDFPage_CountObjects(page);
        for (int index = 0; index < objectCount; ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_TEXT
                && pageObjectText(object, textPage).contains(text)
                && FPDFPageObj_GetMatrix(object, &matrix)) {
                found = true;
                break;
            }
        }
    }
    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return found;
}

qreal horizontalScale(const FS_MATRIX &matrix)
{
    const qreal verticalLength = std::hypot(matrix.c, matrix.d);
    return verticalLength > 0.0
        ? std::hypot(matrix.a, matrix.b) / verticalLength
        : 0.0;
}

bool strokeTextAppearance(const QString &filePath,
                          const QString &text,
                          QColor &strokeColor,
                          qreal &strokeWidth)
{
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
    bool found = false;
    if (page && textPage) {
        const int objectCount = FPDFPage_CountObjects(page);
        for (int index = 0; index < objectCount; ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT
                || !pageObjectText(object, textPage).contains(text)
                || FPDFTextObj_GetTextRenderMode(object) != FPDF_TEXTRENDERMODE_STROKE) {
                continue;
            }
            unsigned int red = 0;
            unsigned int green = 0;
            unsigned int blue = 0;
            unsigned int alpha = 0;
            float width = 0.0F;
            if (FPDFPageObj_GetStrokeColor(
                    object,
                    &red,
                    &green,
                    &blue,
                    &alpha)
                && FPDFPageObj_GetStrokeWidth(object, &width)) {
                strokeColor = QColor::fromRgb(red, green, blue, alpha);
                strokeWidth = width;
                found = true;
                break;
            }
        }
    }
    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return found;
}

struct RichSaveInspection {
    bool readable = false;
    bool redRun = false;
    bool boldItalicRun = false;
    bool underlinePath = false;
    bool redAndBlueShareBaseline = false;
    bool greenIsNextLine = false;
};

RichSaveInspection inspectRichSave(const QString &filePath)
{
    RichSaveInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
    inspection.readable = document && page && textPage;

    qreal redBaseline = 0.0;
    qreal blueBaseline = 0.0;
    qreal greenBaseline = 0.0;
    bool foundRedBaseline = false;
    bool foundBlueBaseline = false;
    bool foundGreenBaseline = false;
    if (inspection.readable) {
        const int objectCount = FPDFPage_CountObjects(page);
        for (int index = 0; index < objectCount; ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            const int type = FPDFPageObj_GetType(object);
            if (type == FPDF_PAGEOBJ_PATH) {
                inspection.underlinePath = true;
                continue;
            }
            if (type != FPDF_PAGEOBJ_TEXT) {
                continue;
            }

            const QString text = pageObjectText(object, textPage);
            FS_MATRIX matrix{};
            if (!FPDFPageObj_GetMatrix(object, &matrix)) {
                continue;
            }
            if (text.contains(QStringLiteral("Red"))) {
                unsigned int red = 0;
                unsigned int green = 0;
                unsigned int blue = 0;
                unsigned int alpha = 0;
                inspection.redRun = FPDFPageObj_GetFillColor(
                    object,
                    &red,
                    &green,
                    &blue,
                    &alpha)
                    && red == 0xc6
                    && green == 0x28
                    && blue == 0x28
                    && alpha == 0xff;
                const QString fontName = baseFontName(object).toLower();
                inspection.boldItalicRun = fontName.contains(QStringLiteral("bold"))
                    && (fontName.contains(QStringLiteral("italic"))
                        || fontName.contains(QStringLiteral("oblique")));
                redBaseline = matrix.f;
                foundRedBaseline = true;
            } else if (text.contains(QStringLiteral("blue"))) {
                blueBaseline = matrix.f;
                foundBlueBaseline = true;
            } else if (text.contains(QStringLiteral("Green"))) {
                greenBaseline = matrix.f;
                foundGreenBaseline = true;
            }
        }
    }

    inspection.redAndBlueShareBaseline = foundRedBaseline
        && foundBlueBaseline
        && qAbs(redBaseline - blueBaseline) < 0.5;
    inspection.greenIsNextLine = foundGreenBaseline
        && foundBlueBaseline
        && greenBaseline < blueBaseline - 1.0;

    if (textPage) {
        FPDFText_ClosePage(textPage);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return inspection;
}

struct HighlightSaveInspection {
    bool readable = false;
    int count = 0;
    QColor color;
    int flags = 0;
    FS_RECTF rect{};
    QVector<FS_QUADPOINTSF> quadPoints;
};

struct AcroFormFieldInspection {
    bool readable = false;
    int formType = FORMTYPE_NONE;
    int widgetCount = 0;
    int fieldType = FPDF_FORMFIELD_UNKNOWN;
    int fieldFlags = 0;
    QString name;
    QString value;
    FS_RECTF rect{};
    bool hasAppearance = false;
    bool hasMaxLength = false;
    float maxLength = 0.0f;
    QString exportValue;
    bool checked = false;
    int annotationFlags = 0;
    int controlCount = -1;
    int controlIndex = -1;
    bool hasAppearanceVersion = false;
    float appearanceVersion = 0.0f;
    bool pointHit = false;
    bool focusHandled = false;
};

AcroFormFieldInspection inspectAcroFormField(
    const QString &filePath,
    const QString &targetFieldName = {},
    int pageIndex = 0)
{
    PDFiumProcessLock pdfiumLock;
    AcroFormFieldInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    if (!document) {
        return inspection;
    }

    inspection.formType = FPDF_GetFormType(document);
    FPDF_FORMFILLINFO formFillInfo = {};
    formFillInfo.version = 2;
    formFillInfo.xfa_disabled = true;
    FPDF_FORMHANDLE formHandle = FPDFDOC_InitFormFillEnvironment(
        document,
        &formFillInfo);
    FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    inspection.readable = formHandle && page;
    if (formHandle && page) {
        FORM_OnAfterLoadPage(page, formHandle);
        const auto readWideString = [](auto getter) {
            const unsigned long byteCount = getter(nullptr, 0);
            if (byteCount < sizeof(FPDF_WCHAR)) {
                return QString();
            }
            QVector<FPDF_WCHAR> buffer(
                static_cast<qsizetype>(
                    (byteCount + sizeof(FPDF_WCHAR) - 1)
                    / sizeof(FPDF_WCHAR)));
            const unsigned long actualByteCount = getter(
                buffer.data(),
                byteCount);
            qsizetype characterCount = static_cast<qsizetype>(
                actualByteCount / sizeof(FPDF_WCHAR));
            while (characterCount > 0
                   && buffer.at(characterCount - 1) == 0) {
                --characterCount;
            }
            return QString::fromUtf16(
                reinterpret_cast<const char16_t *>(
                    buffer.constData()),
                characterCount);
        };
        const int annotationCount = FPDFPage_GetAnnotCount(page);
        for (int index = 0; index < annotationCount; ++index) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
            if (!annotation) {
                continue;
            }
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_WIDGET) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            ++inspection.widgetCount;
            const QString fieldName = readWideString(
                [formHandle, annotation](FPDF_WCHAR *buffer,
                                         unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldName(
                        formHandle,
                        annotation,
                        buffer,
                        byteCount);
                });
            if (inspection.fieldType == FPDF_FORMFIELD_UNKNOWN
                && (targetFieldName.isEmpty()
                    || fieldName == targetFieldName)) {
                inspection.fieldType = FPDFAnnot_GetFormFieldType(
                    formHandle,
                    annotation);
                inspection.fieldFlags = FPDFAnnot_GetFormFieldFlags(
                    formHandle,
                    annotation);
                inspection.name = fieldName;
                inspection.value = readWideString(
                    [formHandle, annotation](FPDF_WCHAR *buffer,
                                             unsigned long byteCount) {
                        return FPDFAnnot_GetFormFieldValue(
                            formHandle,
                            annotation,
                            buffer,
                            byteCount);
                    });
                FPDFAnnot_GetRect(annotation, &inspection.rect);
                inspection.hasAppearance = FPDFAnnot_HasKey(
                    annotation,
                    "AP");
                inspection.exportValue = readWideString(
                    [formHandle, annotation](FPDF_WCHAR *buffer,
                                             unsigned long byteCount) {
                        return FPDFAnnot_GetFormFieldExportValue(
                            formHandle,
                            annotation,
                            buffer,
                            byteCount);
                    });
                inspection.checked = FPDFAnnot_IsChecked(
                    formHandle,
                    annotation);
                inspection.annotationFlags = FPDFAnnot_GetFlags(annotation);
                inspection.controlCount = FPDFAnnot_GetFormControlCount(
                    formHandle,
                    annotation);
                inspection.controlIndex = FPDFAnnot_GetFormControlIndex(
                    formHandle,
                    annotation);
                inspection.hasAppearanceVersion
                    = FPDFAnnot_GetNumberValue(
                        annotation,
                        PDFFormAppearance::kVersionKey,
                        &inspection.appearanceVersion);
                inspection.hasMaxLength = FPDFAnnot_GetNumberValue(
                    annotation,
                    "MaxLen",
                    &inspection.maxLength);
            }
            FPDFPage_CloseAnnot(annotation);
        }
        if (inspection.fieldType != FPDF_FORMFIELD_UNKNOWN) {
            const FS_POINTF center{
                (inspection.rect.left + inspection.rect.right) / 2.0f,
                (inspection.rect.bottom + inspection.rect.top) / 2.0f};
            FPDF_ANNOTATION hit = FPDFAnnot_GetFormFieldAtPoint(
                formHandle,
                page,
                &center);
            inspection.pointHit = hit != nullptr;
            if (hit) {
                FPDFPage_CloseAnnot(hit);
            }
            inspection.focusHandled = FORM_OnFocus(
                formHandle,
                page,
                0,
                center.x,
                center.y);
        }
        FORM_OnBeforeClosePage(page, formHandle);
    }
    if (page) {
        FPDF_ClosePage(page);
    }
    if (formHandle) {
        FPDFDOC_ExitFormFillEnvironment(formHandle);
    }
    FPDF_CloseDocument(document);
    return inspection;
}

HighlightSaveInspection inspectHighlights(const QString &filePath,
                                          int pageIndex = 0)
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
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_HIGHLIGHT) {
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
                inspection.flags = FPDFAnnot_GetFlags(annotation);
                FPDFAnnot_GetRect(annotation, &inspection.rect);
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

struct ShapeSaveInspection {
    bool readable = false;
    int count = 0;
    int filledCount = 0;
    int openCount = 0;
    int bezierCount = 0;
    int blackStrokeCount = 0;
    int transparentFillCount = 0;
    QVector<QVector<QPointF>> normalizedPathPoints;
};

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

struct ImageSaveInspection {
    bool readable = false;
    bool pixelsReadable = false;
    bool matrixReadable = false;
    int count = 0;
    int firstTopLevelIndex = -1;
    unsigned int pixelWidth = 0;
    unsigned int pixelHeight = 0;
    QColor topLeftPixel;
    QColor bottomLeftPixel;
    FS_MATRIX matrix{};
};

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

class TestableFormFieldLayer final : public FormFieldLayer
{
public:
    using FormFieldLayer::mouseMoveEvent;
    using FormFieldLayer::mousePressEvent;
    using FormFieldLayer::mouseReleaseEvent;
};

} // namespace

class PDFEditControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void storesTransientTextFieldsByPage();
    void limitsFormFieldPointerHitArea();
    void movesFormFieldsWithoutResizing();
    void savesTextFieldAsAcroForm();
    void savesAndRehydratesCheckbox();
    void rehydratesTextFieldAfterSamePathSave();
    void normalizesLegacyTextFieldAppearances();
    void addsTextFieldToExistingAcroForm();
    void rejectsDuplicateAcroFormFieldNames();
    void enablesFormScriptingAutomatically();
    void customizesChoiceFieldsAndPersistsSelection();
    void routesPushButtonActions_data();
    void routesPushButtonActions();
    void blocksUnsupportedFormUriSchemes();
    void forwardsExternalLinkRequestsThroughTabModel();
    void serializesPdfiumAcrossThreads();
    void discoversUncachedHoverOffGuiThread();
    void reschedulesCancelledHoverForSamePage();
    void defersUncachedHoverWhilePdfiumIsBusy();
    void registersBundledFonts();
    void keepsArialLayoutStableAfterEmbeddedSubsetCollision();
    void filtersUnsupportedSystemFonts();
    void filtersFontFamiliesByLanguage();
    void intersectsFontFiltersAcrossMixedStyles();
    void detectsPdfFontStyles();
    void propagatesSourceStylesThroughSelection();
    void groupsCompatibleTextObjectsOnSameLine();
    void seedsGroupedFrameFromQtTextWidth();
    void expandsTextEditFrameTransiently();
    void stopsExpandedFrameBeforeRightColumn();
    void keepsExpandedColumnFrameStableAcrossEdits();
    void followsCurrentRightColumnDraftBoundary();
    void hidesDeletedTextFromInteraction();
    void persistsExpandedFrameForTextChanges();
    void persistsExpandedFrameForParagraphAlignment();
    void keepsStandaloneResizeFrameUnexpanded();
    void keepsMixedStylesAsSeparateCandidates();
    void keepsDistantColumnsAsSeparateCandidates();
    void keepsDifferentFontFacesAsSeparateCandidates();
    void editsGroupedLineAsOneDraft();
    void resizesDraftFrameWithUndoRedo();
    void storesReflowedContentBoundsWithUndoRedo();
    void changesFontFamilyForCurrentEdit();
    void resetsStretchWhenChangingFontFamily();
    void normalizesSourceStretchWhenReselectingFontFamily();
    void preservesSourceHorizontalScaleWhenResizing_data();
    void preservesSourceHorizontalScaleWhenResizing();
    void preservesSourceScaleForUnchangedRichTextRuns();
    void keepsAddedDidotScaleStableAcrossRepeatedSaves();
    void appliesFontStylesToTypingAtCaret();
    void appliesTypingFormatBetweenRepeatedCharacters();
    void preservesEditorUndoWhenFormattingSelection();
    void formatsSelectedRangeInMultilineDraft();
    void alignsSelectedParagraphsWithUndoRedo();
    void calibratesDisplayStretchToPdfScale_data();
    void calibratesDisplayStretchToPdfScale();
    void appliesFontStylesToDisplayFont_data();
    void appliesFontStylesToDisplayFont();
    void savesEditedTextRoundTrip();
    void preservesPatternFilledGraphicsWhenSavingText();
    void preservesPatternStrokedGraphicsWhenSavingText();
    void preservesGrayscaleWhiteFillWhenSavingText();
    void savesVietnameseEditWithEmbeddedArial_data();
    void savesVietnameseEditWithEmbeddedArial();
    void savesVietnameseEditWithEmbeddedSystemFont_data();
    void savesVietnameseEditWithEmbeddedSystemFont();
    void preservesInheritedSystemFontForVietnamese();
    void rejectsUnsupportedSystemFontForVietnamese_data();
    void rejectsUnsupportedSystemFontForVietnamese();
    void preservesEmbeddedFontForFormatOnlyVietnameseEdit();
    void savesNormalizedVietnameseRichText();
    void addsHighlightWithUndoRedoAndDiscard();
    void savesHighlightAnnotationRoundTrip();
    void savesHighlightAlongsideTextEdit();
    void preservesPatternGraphicsWhenSavingHighlight();
    void addsBlankPageWithUndoRedoAndDiscard();
    void savesBlankPageRoundTrip();
    void savesEditAfterInsertingEarlierPage();
    void savesMultipleInsertedPagesAfterUndoRedo();
    void deletesSourcePageWithUndoRedoAndDiscard();
    void refusesToDeleteOnlyPage();
    void deletesTransientBlankPage();
    void restoresLastPageSelectionWhenUndoingDeletion();
    void mapsCandidatesAfterDeletingEarlierPage();
    void savesDeletedPageRoundTrip();
    void savesEditAfterDeletingEarlierPage();
    void savesBlankAfterDeletingAllSourcePages();
    void savesCurrentFileRoundTrip();
    void savesUnchangedCopyRoundTrip();
    void savesMultipleDraftsOnSamePage();
    void savesExpandedFrameBeforeRightColumn();
    void savesParagraphAlignmentOnlyEdit();
    void savesDeletedTextRoundTrip();
    void savesAddedTextRoundTrip();
    void rendersAddedImageSnapshot();
    void addsImageWithUndoRedo();
    void movesAddedImageWithUndoRedo();
    void addsShapeWithUndoRedo();
    void savesShapesRoundTrip();
    void movesTopLevelTextWithUndoRedo();
    void movesTopLevelImageWithUndoRedo();
    void deletesTopLevelImageWithUndoRedo();
    void savesAddedImageRoundTrip();
    void savesMovedTopLevelTextRoundTrip();
    void savesMovedTopLevelImageRoundTrip();
    void savesDeletedTopLevelImageRoundTrip();
    void preservesAddedTextLayerOrder();
    void savesRichMultilineTextRoundTrip();
    void savesOffsetCropCoordinates();
    void savesAddedTextOnRotatedPage();
    void preservesStrokeAppearanceAndRejectsClipping();
    void savesAsynchronously();
    void keepsDraftsWhenSaveFails();
    void opensDocumentsInIndependentTabs();
    void keepsViewRotationIndependentAcrossTabs();
    void clearsTabIdentityWhenDocumentCloses();
    void keepsTabDraftsIndependent();
    void savesOnlyRequestedTab();
    void rejectsSaveAsOverAnotherOpenTab();

private:
    QTemporaryDir m_temporaryDirectory;
    PDFDocument m_document;
    PDFEditController m_controller;
    QVariantMap m_regularCandidate;
    QVariantMap m_boldCandidate;
    QVariantMap m_groupedCandidate;
    QVariantMap m_mixedRegularCandidate;
    QVariantMap m_mixedBoldCandidate;
    QVariantMap m_leftColumnCandidate;
    QVariantMap m_rightColumnCandidate;
    QVariantMap m_uprightCandidate;
    QVariantMap m_obliqueCandidate;
};

void PDFEditControllerTest::initTestCase()
{
    QVERIFY2(m_temporaryDirectory.isValid(), "Could not create the test directory");
    ApplicationFonts::registerBundledFonts();

    const QString pdfPath = m_temporaryDirectory.filePath(QStringLiteral("font-styles.pdf"));
    QVERIFY2(createFontStylePdf(pdfPath), "Could not create the PDF test fixture");
    QVERIFY2(m_document.open(QUrl::fromLocalFile(pdfPath)), "Could not open the PDF test fixture");

    m_controller.setDocument(&m_document);
    const QVariantList candidates = waitForCandidates(m_controller, 0);
    m_regularCandidate = candidateForText(candidates, QString::fromLatin1(kRegularText));
    m_boldCandidate = candidateForText(candidates, QString::fromLatin1(kBoldText));
    m_groupedCandidate = candidateForText(candidates, QString::fromLatin1(kGroupedText));
    m_mixedRegularCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kMixedRegularText));
    m_mixedBoldCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kMixedBoldText));
    m_leftColumnCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kLeftColumnText));
    m_rightColumnCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kRightColumnText));
    m_uprightCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kUprightText));
    m_obliqueCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kObliqueText));

    QVERIFY2(!m_regularCandidate.isEmpty(), "Regular text candidate was not discovered");
    QVERIFY2(!m_boldCandidate.isEmpty(), "Bold text candidate was not discovered");
    QVERIFY2(!m_groupedCandidate.isEmpty(), "Grouped text candidate was not discovered");
    QVERIFY2(!m_mixedRegularCandidate.isEmpty(), "Mixed regular candidate was not discovered");
    QVERIFY2(!m_mixedBoldCandidate.isEmpty(), "Mixed bold candidate was not discovered");
    QVERIFY2(!m_leftColumnCandidate.isEmpty(), "Left column candidate was not discovered");
    QVERIFY2(!m_rightColumnCandidate.isEmpty(), "Right column candidate was not discovered");
    QVERIFY2(!m_uprightCandidate.isEmpty(), "Upright candidate was not discovered");
    QVERIFY2(!m_obliqueCandidate.isEmpty(), "Oblique candidate was not discovered");
}

void PDFEditControllerTest::storesTransientTextFieldsByPage()
{
    PDFFormDesignerController controller;
    QSignalSpy fieldsChangedSpy(&controller,
                                &PDFFormDesignerController::textFieldsChanged);
    QSignalSpy focusChangedSpy(
        &controller,
        &PDFFormDesignerController::textFieldFocusedChanged);
    QSignalSpy selectionChangedSpy(
        &controller,
        &PDFFormDesignerController::textFieldSelectionChanged);
    QSignalSpy rowsInsertedSpy(&controller,
                               &QAbstractItemModel::rowsInserted);
    QSignalSpy rowsRemovedSpy(&controller,
                              &QAbstractItemModel::rowsRemoved);
    QSignalSpy dataChangedSpy(&controller,
                              &QAbstractItemModel::dataChanged);

    const QString fieldId = controller.addTextField(2, 0.1, 0.2, 0.4, 0.08);
    QVERIFY(!fieldId.isEmpty());
    QCOMPARE(controller.rowCount(), 1);
    QCOMPARE(rowsInsertedSpy.count(), 1);
    QCOMPARE(controller.roleNames().value(PDFFormDesignerController::IdRole),
             QByteArray("id"));
    QCOMPARE(controller.roleNames().value(PDFFormDesignerController::TypeRole),
             QByteArray("type"));
    const QModelIndex fieldIndex = controller.index(0);
    QCOMPARE(controller.data(fieldIndex,
                             PDFFormDesignerController::IdRole).toString(),
             fieldId);
    QCOMPARE(controller.data(fieldIndex,
                             PDFFormDesignerController::TypeRole).toInt(),
             PDFFormDesignerController::Text);
    QCOMPARE(controller.data(fieldIndex,
                             PDFFormDesignerController::PageRole).toInt(),
             2);
    QCOMPARE(fieldsChangedSpy.count(), 1);
    QCOMPARE(fieldsChangedSpy.takeFirst().at(0).toInt(), 2);
    QCOMPARE(controller.textFieldsForPage(1).size(), 0);

    QVariantList fields = controller.textFieldsForPage(2);
    QCOMPARE(fields.size(), 1);
    const QVariantMap field = fields.constFirst().toMap();
    QCOMPARE(field.value(QStringLiteral("id")).toString(), fieldId);
    QCOMPARE(field.value(QStringLiteral("x")).toReal(), 0.1);
    QCOMPARE(field.value(QStringLiteral("y")).toReal(), 0.2);
    QCOMPARE(field.value(QStringLiteral("width")).toReal(), 0.4);
    QCOMPARE(field.value(QStringLiteral("height")).toReal(), 0.08);

    controller.setTextFieldFocused(fieldId, true);
    controller.selectTextField(fieldId);
    QVERIFY(controller.textFieldFocused());
    QCOMPARE(controller.focusedTextFieldId(), fieldId);
    QCOMPARE(focusChangedSpy.count(), 1);
    QCOMPARE(controller.selectedTextFieldId(), fieldId);
    QCOMPARE(selectionChangedSpy.count(), 1);

    const QString secondFieldId
        = controller.addTextField(2, 0.2, 0.4, 0.25, 0.06);
    QVERIFY(!secondFieldId.isEmpty());
    QCOMPARE(rowsInsertedSpy.count(), 2);
    controller.setTextFieldFocused(secondFieldId, true);
    QCOMPARE(controller.focusedTextFieldId(), secondFieldId);
    QCOMPARE(focusChangedSpy.count(), 2);
    controller.setTextFieldFocused(fieldId, true);
    QCOMPARE(controller.focusedTextFieldId(), fieldId);
    QCOMPARE(focusChangedSpy.count(), 3);
    QVERIFY(controller.removeTextField(secondFieldId));

    controller.setTextFieldText(fieldId, QStringLiteral("Sample value"));
    QCOMPARE(dataChangedSpy.count(), 1);
    fields = controller.textFieldsForPage(2);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("Sample value"));

    QVERIFY(controller.setTextFieldRect(fieldId, 0.15, 0.25, 0.5, 0.1));
    QCOMPARE(dataChangedSpy.count(), 2);
    fields = controller.textFieldsForPage(2);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("x")).toReal(),
             0.15);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("y")).toReal(),
             0.25);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("width")).toReal(),
             0.5);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("height")).toReal(),
             0.1);

    QVERIFY(controller.removeTextField(fieldId));
    QCOMPARE(rowsRemovedSpy.count(), 2);
    QVERIFY(!controller.textFieldFocused());
    QCOMPARE(focusChangedSpy.count(), 4);
    QVERIFY(controller.selectedTextFieldId().isEmpty());
    QCOMPARE(selectionChangedSpy.count(), 2);
    QCOMPARE(controller.textFieldsForPage(2).size(), 0);

    controller.clear();
    QVERIFY(!controller.textFieldFocused());
    QCOMPARE(focusChangedSpy.count(), 4);
    QCOMPARE(controller.textFieldsForPage(2).size(), 0);

    const QString checkboxId = controller.addField(
        PDFFormDesignerController::Checkbox,
        0,
        0.1,
        0.1,
        0.04,
        0.04);
    QVERIFY(!checkboxId.isEmpty());
    QVERIFY(controller.setFieldName(checkboxId,
                                    QStringLiteral("AcceptTerms")));
    QVERIFY(controller.setFieldOptions(
        checkboxId,
        {QStringLiteral("Yes"), QStringLiteral("Off")}));
    QVERIFY(controller.setFieldFlags(
        checkboxId,
        PDFFormDesignerController::Required
            | PDFFormDesignerController::ReadOnly));
    QVERIFY(controller.setFieldValue(checkboxId, QStringLiteral("Yes")));
    const QVariantMap checkbox = controller.fieldData(checkboxId);
    QCOMPARE(checkbox.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Checkbox);
    QCOMPARE(checkbox.value(QStringLiteral("name")).toString(),
             QStringLiteral("AcceptTerms"));
    QCOMPARE(checkbox.value(QStringLiteral("options")).toStringList(),
             QStringList({QStringLiteral("Yes"), QStringLiteral("Off")}));
    QCOMPARE(checkbox.value(QStringLiteral("flags")).toInt(),
             PDFFormDesignerController::Required
                 | PDFFormDesignerController::ReadOnly);
    QVERIFY(checkbox.value(QStringLiteral("dirty")).toBool());
    QCOMPARE(controller.fieldsForPage(0).size(), 1);
    QCOMPARE(controller.textFieldsForPage(0).size(), 0);
    QVERIFY(controller.removeTextField(checkboxId));

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("form-designer-pages.pdf"));
    QVERIFY(createPushButtonActionPdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    PDFEditController editController;
    editController.setDocument(&document);
    controller.setDocument(&document);

    const QString sourceFieldId
        = controller.addTextField(1, 0.1, 0.2, 0.4, 0.08);
    QVERIFY(!sourceFieldId.isEmpty());
    document.setCurrentPage(0);
    QCOMPARE(editController.addBlankPage(), 1);
    QCOMPARE(controller.textFieldsForPage(1).size(), 0);
    fields = controller.textFieldsForPage(2);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("id")).toString(),
             sourceFieldId);

    const QString transientFieldId
        = controller.addTextField(1, 0.15, 0.25, 0.35, 0.07);
    QVERIFY(!transientFieldId.isEmpty());
    editController.undo();
    fields = controller.textFieldsForPage(1);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields.constFirst().toMap().value(QStringLiteral("id")).toString(),
             sourceFieldId);
}

void PDFEditControllerTest::limitsFormFieldPointerHitArea()
{
    PDFFormDesignerController controller;
    const QString fieldId = controller.addTextField(
        0,
        0.1,
        0.2,
        0.4,
        0.08);
    QVERIFY(!fieldId.isEmpty());

    TestableFormFieldLayer fieldLayer;
    fieldLayer.setWidth(1000);
    fieldLayer.setHeight(1000);
    fieldLayer.setController(&controller);
    fieldLayer.setPage(0);

    const QPointF fieldCenter(300, 240);
    const QPointF emptyPagePoint(800, 500);
    QVERIFY(!fieldLayer.contains(fieldCenter));

    fieldLayer.setFieldsVisible(true);
    QVERIFY(!fieldLayer.contains(fieldCenter));

    fieldLayer.setFieldsInteractive(true);
    fieldLayer.setMoveEnabled(true);
    QVERIFY(fieldLayer.acceptHoverEvents());
    QVERIFY(fieldLayer.contains(fieldCenter));
    QVERIFY(!fieldLayer.contains(emptyPagePoint));
    QVERIFY(!fieldLayer.contains(QPointF(1001, 500)));

    fieldLayer.setMoveEnabled(false);
    QVERIFY(!fieldLayer.acceptHoverEvents());
    QVERIFY(fieldLayer.contains(fieldCenter));

    fieldLayer.setPage(1);
    QVERIFY(!fieldLayer.contains(fieldCenter));
    fieldLayer.setPage(0);
    QVERIFY(fieldLayer.contains(fieldCenter));

    fieldLayer.setPooled(true);
    QVERIFY(!fieldLayer.contains(fieldCenter));
    fieldLayer.setPooled(false);
    QVERIFY(fieldLayer.contains(fieldCenter));

    fieldLayer.setFieldsInteractive(false);
    QVERIFY(!fieldLayer.contains(fieldCenter));
    fieldLayer.setFieldsInteractive(true);
    QVERIFY(fieldLayer.contains(fieldCenter));

    QVERIFY(controller.moveField(fieldId, 0.5, 0.6));
    QVERIFY(!fieldLayer.contains(fieldCenter));
    QVERIFY(fieldLayer.contains(QPointF(700, 640)));
}

void PDFEditControllerTest::movesFormFieldsWithoutResizing()
{
    PDFFormDesignerController controller;
    const QString textFieldId = controller.addTextField(
        0,
        0.1,
        0.2,
        0.4,
        0.08);
    const QString checkboxId = controller.addField(
        PDFFormDesignerController::Checkbox,
        0,
        0.8,
        0.8,
        0.06,
        0.06);
    QVERIFY(!textFieldId.isEmpty());
    QVERIFY(!checkboxId.isEmpty());

    TestableFormFieldLayer fieldLayer;
    fieldLayer.setWidth(1000);
    fieldLayer.setHeight(1000);
    fieldLayer.setController(&controller);
    fieldLayer.setPage(0);
    fieldLayer.setFieldsVisible(true);
    fieldLayer.setFieldsInteractive(true);
    fieldLayer.setMoveEnabled(true);
    controller.selectTextField(textFieldId);
    QSignalSpy dataChangedSpy(
        &controller,
        &QAbstractItemModel::dataChanged);

    QVERIFY(fieldLayer.beginFieldMove(textFieldId));
    QVERIFY(fieldLayer.moving());
    QVERIFY(fieldLayer.updateFieldMove(1.0, 1.0));
    QRectF previewRect = fieldLayer.activeFieldRect();
    QVERIFY(qAbs(previewRect.x() - 0.6) < 0.000001);
    QVERIFY(qAbs(previewRect.y() - 0.92) < 0.000001);
    QVERIFY(qAbs(previewRect.width() - 0.4) < 0.000001);
    QVERIFY(qAbs(previewRect.height() - 0.08) < 0.000001);
    QVariantMap field = controller.fieldData(textFieldId);
    QCOMPARE(field.value(QStringLiteral("x")).toReal(), 0.1);
    QCOMPARE(field.value(QStringLiteral("y")).toReal(), 0.2);
    QCOMPARE(dataChangedSpy.count(), 0);

    fieldLayer.cancelFieldMove();
    QVERIFY(!fieldLayer.moving());
    previewRect = fieldLayer.activeFieldRect();
    QCOMPARE(previewRect, QRectF(0.1, 0.2, 0.4, 0.08));
    QCOMPARE(dataChangedSpy.count(), 0);

    QVERIFY(fieldLayer.beginFieldMove(textFieldId));
    QVERIFY(fieldLayer.updateFieldMove(0.2, 0.3));
    QVERIFY(fieldLayer.finishFieldMove());
    QVERIFY(!fieldLayer.moving());
    field = controller.fieldData(textFieldId);
    QCOMPARE(field.value(QStringLiteral("x")).toReal(), 0.3);
    QCOMPARE(field.value(QStringLiteral("y")).toReal(), 0.5);
    QCOMPARE(field.value(QStringLiteral("width")).toReal(), 0.4);
    QCOMPARE(field.value(QStringLiteral("height")).toReal(), 0.08);
    QCOMPARE(dataChangedSpy.count(), 1);

    QVERIFY(controller.moveField(checkboxId, 0.98, -0.4));
    field = controller.fieldData(checkboxId);
    QVERIFY(qAbs(field.value(QStringLiteral("x")).toReal() - 0.94)
            < 0.000001);
    QCOMPARE(field.value(QStringLiteral("y")).toReal(), 0.0);
    QCOMPARE(field.value(QStringLiteral("width")).toReal(), 0.06);
    QCOMPARE(field.value(QStringLiteral("height")).toReal(), 0.06);
    QCOMPARE(dataChangedSpy.count(), 2);

    QVERIFY(controller.moveField(checkboxId, 0.94, 0.0));
    QCOMPARE(dataChangedSpy.count(), 2);
    QVERIFY(!controller.moveField(
        QStringLiteral("missing-field"),
        0.2,
        0.3));
    QVERIFY(!controller.moveField(
        textFieldId,
        std::numeric_limits<qreal>::quiet_NaN(),
        0.3));

    QSignalSpy fieldActivatedSpy(
        &fieldLayer,
        &FormFieldLayer::fieldActivated);
    const QPointF checkboxCenter(970, 30);
    QMouseEvent clickPress(
        QEvent::MouseButtonPress,
        checkboxCenter,
        checkboxCenter,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mousePressEvent(&clickPress);
    QCOMPARE(fieldActivatedSpy.count(), 0);
    QMouseEvent clickRelease(
        QEvent::MouseButtonRelease,
        checkboxCenter,
        checkboxCenter,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    fieldLayer.mouseReleaseEvent(&clickRelease);
    QCOMPARE(fieldActivatedSpy.count(), 1);

    QMouseEvent dragPress(
        QEvent::MouseButtonPress,
        checkboxCenter,
        checkboxCenter,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mousePressEvent(&dragPress);
    const QPointF movedPointer = checkboxCenter + QPointF(-50, 50);
    QMouseEvent dragMove(
        QEvent::MouseMove,
        movedPointer,
        movedPointer,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mouseMoveEvent(&dragMove);
    QMouseEvent dragRelease(
        QEvent::MouseButtonRelease,
        movedPointer,
        movedPointer,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    fieldLayer.mouseReleaseEvent(&dragRelease);
    QCOMPARE(fieldActivatedSpy.count(), 1);
    field = controller.fieldData(checkboxId);
    QVERIFY(qAbs(field.value(QStringLiteral("x")).toReal() - 0.89)
            < 0.000001);
    QVERIFY(qAbs(field.value(QStringLiteral("y")).toReal() - 0.05)
            < 0.000001);
    QCOMPARE(field.value(QStringLiteral("width")).toReal(), 0.06);
    QCOMPARE(field.value(QStringLiteral("height")).toReal(), 0.06);

    const QPointF movedCheckboxCenter(920, 80);
    QMouseEvent canceledPress(
        QEvent::MouseButtonPress,
        movedCheckboxCenter,
        movedCheckboxCenter,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mousePressEvent(&canceledPress);
    const QPointF canceledPointer
        = movedCheckboxCenter + QPointF(50, 50);
    QMouseEvent canceledMove(
        QEvent::MouseMove,
        canceledPointer,
        canceledPointer,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::NoModifier);
    fieldLayer.mouseMoveEvent(&canceledMove);
    QVERIFY(fieldLayer.moving());
    fieldLayer.cancelFieldMove();
    QVERIFY(!fieldLayer.moving());
    QMouseEvent canceledRelease(
        QEvent::MouseButtonRelease,
        canceledPointer,
        canceledPointer,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    fieldLayer.mouseReleaseEvent(&canceledRelease);
    QCOMPARE(fieldActivatedSpy.count(), 1);
    field = controller.fieldData(checkboxId);
    QVERIFY(qAbs(field.value(QStringLiteral("x")).toReal() - 0.89)
            < 0.000001);
    QVERIFY(qAbs(field.value(QStringLiteral("y")).toReal() - 0.05)
            < 0.000001);
}

void PDFEditControllerTest::savesTextFieldAsAcroForm()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-saved.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    PDFFormDesignerController formDesigner;
    formDesigner.setDocument(&document);
    PDFEditController editController;
    editController.setDocument(&document);
    editController.setFormDesignerController(&formDesigner);

    const QString fieldId = formDesigner.addTextField(
        0,
        0.1,
        0.2,
        0.4,
        0.08);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("TênKháchHàng")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Nguyễn Văn A")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));
    const QString secondFieldId = formDesigner.addTextField(
        0,
        0.55,
        0.2,
        0.3,
        0.08);
    QVERIFY(!secondFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        secondFieldId,
        QStringLiteral("SecondText")));
    QVERIFY(formDesigner.setFieldValue(
        secondFieldId,
        QStringLiteral("Second value")));
    QVERIFY(formDesigner.hasDrafts());
    QVERIFY(editController.hasDrafts());

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        editController,
        savedPath);
    QVERIFY2(result.succeeded(), qPrintable(result.detail));
    QVERIFY(QFileInfo::exists(savedPath));

    const AcroFormFieldInspection inspection
        = inspectAcroFormField(
            savedPath,
            QStringLiteral("TênKháchHàng"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 2);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(inspection.name, QStringLiteral("TênKháchHàng"));
    QCOMPARE(inspection.value, QStringLiteral("Nguyễn Văn A"));
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.pointHit);
    QVERIFY(inspection.focusHandled);
    QVERIFY(qAbs(inspection.rect.left - 59.5) < 1.0);
    QVERIFY(qAbs(inspection.rect.right - 297.5) < 1.0);
    QVERIFY(qAbs(inspection.rect.top - 673.6) < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom - 606.24) < 1.0);
    const AcroFormFieldInspection secondInspection
        = inspectAcroFormField(
            savedPath,
            QStringLiteral("SecondText"));
    QCOMPARE(secondInspection.widgetCount, 2);
    QCOMPARE(secondInspection.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(secondInspection.value, QStringLiteral("Second value"));
    QVERIFY(secondInspection.hasAppearance);

    PDFFormController formController;
    PDFDocument reopened;
    formController.setDocument(&reopened);
    QVERIFY(reopened.open(QUrl::fromLocalFile(savedPath)));
    QCOMPARE(reopened.pageCount(), document.pageCount());
    QVERIFY(formController.interactive());
    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    const QSize deviceSize(595, 842);
    const QPointF fieldCenter(178.5, 202.08);
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Press,
        0,
        fieldCenter,
        deviceSize);
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Release,
        0,
        fieldCenter,
        deviceSize);
    QTRY_VERIFY_WITH_TIMEOUT(formController.acceptsTextInput(), 5000);
    formController.selectAll();
    formController.replaceSelection(QStringLiteral("Đã chỉnh sửa"));
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);

    QString snapshotPath;
    QString snapshotError;
    QVERIFY2(
        formController.exportSnapshotBlocking(
            snapshotPath,
            snapshotError),
        qPrintable(snapshotError));
    const AcroFormFieldInspection editedInspection
        = inspectAcroFormField(
            snapshotPath,
            QStringLiteral("TênKháchHàng"));
    QCOMPARE(editedInspection.value, QStringLiteral("Đã chỉnh sửa"));
    QVERIFY(QFile::remove(snapshotPath));
}

void PDFEditControllerTest::savesAndRehydratesCheckbox()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-checkbox.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFFormDesignerController formDesigner;
    PDFFormController formController;
    PDFEditController editController;
    formDesigner.setDocument(&document);
    formController.setDocument(&document);
    formDesigner.setFormController(&formController);
    editController.setDocument(&document);
    editController.setFormController(&formController);
    editController.setFormDesignerController(&formDesigner);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

    const QRectF initialRect(0.12, 0.24, 0.06, 0.06);
    const QString fieldId = formDesigner.addField(
        PDFFormDesignerController::Checkbox,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());
    QVERIFY(fieldId.startsWith(QStringLiteral("checkbox-field-")));
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("AcceptTerms")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Yes")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));
    const QRectF otherInitialRect(0.65, 0.30, 0.08, 0.08);
    const QString otherCheckboxId = formDesigner.addField(
        PDFFormDesignerController::Checkbox,
        0,
        otherInitialRect.x(),
        otherInitialRect.y(),
        otherInitialRect.width(),
        otherInitialRect.height());
    QVERIFY(otherCheckboxId.startsWith(
        QStringLiteral("checkbox-field-")));
    QVERIFY(formDesigner.setFieldName(
        otherCheckboxId,
        QStringLiteral("OtherChoice")));

    const QVector<PDFFormDesignerController::SaveField> addedFields
        = formDesigner.saveFields();
    QCOMPARE(addedFields.size(), 2);
    QCOMPARE(addedFields.constFirst().type,
             PDFFormDesignerController::Checkbox);
    QCOMPARE(addedFields.constFirst().value, QStringLiteral("Yes"));
    QCOMPARE(addedFields.constFirst().defaultValue, QStringLiteral("Off"));
    QCOMPARE(addedFields.constLast().type,
             PDFFormDesignerController::Checkbox);
    QCOMPARE(addedFields.constLast().value, QStringLiteral("Off"));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Checkbox);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    const QStringList sourceFieldIds
        = formDesigner.sourceFieldIdsForPage(0);
    QCOMPARE(sourceFieldIds.size(), 2);
    QVERIFY(sourceFieldIds.contains(fieldId));
    QVERIFY(sourceFieldIds.contains(otherCheckboxId));
    QVERIFY(formDesigner.sourceTextFieldIdsForPage(0).isEmpty());
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        initialRect.center()));

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("AcceptTerms"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 2);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_CHECKBOX);
    QCOMPARE(inspection.value, QStringLiteral("Yes"));
    QCOMPARE(inspection.exportValue, QStringLiteral("Yes"));
    QVERIFY(inspection.checked);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(inspection.controlCount, 1);
    QCOMPARE(inspection.controlIndex, 0);
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kCheckboxVersion));
    QVERIFY(inspection.pointHit);
    QVERIFY(qAbs(inspection.rect.left - 71.4) < 1.0);
    QVERIFY(qAbs(inspection.rect.right - 107.1) < 1.0);
    QVERIFY(qAbs(inspection.rect.top - 639.92) < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom - 589.4) < 1.0);

    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    const QSize deviceSize(595, 842);
    const QPointF fieldCenter(
        initialRect.center().x() * deviceSize.width(),
        initialRect.center().y() * deviceSize.height());
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Press,
        0,
        fieldCenter,
        deviceSize);
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Release,
        0,
        fieldCenter,
        deviceSize);
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);
    QVERIFY(!formController.acceptsTextInput());

    formDesigner.prepareForDesignMode();
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"),
        5000);

    const QRectF resizedRect(0.20, 0.30, 0.08, 0.08);
    formDesigner.selectTextField(fieldId);
    QVERIFY(formDesigner.setFieldRect(
        fieldId,
        resizedRect.x(),
        resizedRect.y(),
        resizedRect.width(),
        resizedRect.height()));
    formDesigner.selectTextField(otherCheckboxId);
    QVERIFY(formDesigner.activateSourceCheckbox(
        otherCheckboxId,
        deviceSize.width(),
        deviceSize.height()));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(otherCheckboxId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(!formController.hasFocus(), 5000);
    formDesigner.selectTextField(fieldId);
    QCOMPARE(formDesigner.selectedTextFieldId(), fieldId);

    QVERIFY(formDesigner.activateSourceCheckbox(
        fieldId,
        deviceSize.width(),
        deviceSize.height()));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(!formController.hasFocus(), 5000);
    QVERIFY(formDesigner.activateSourceCheckbox(
        fieldId,
        deviceSize.width(),
        deviceSize.height()));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(!formController.hasFocus(), 5000);
    QCOMPARE(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("rect"))
            .toRectF(),
        resizedRect);
    QCOMPARE(formDesigner.fieldsForPage(0).size(), 2);
    QCOMPARE(formDesigner.selectedTextFieldId(), fieldId);

    const QVector<PDFFormDesignerController::SaveField> resizedFields
        = formDesigner.saveFields(true);
    QCOMPARE(resizedFields.size(), 1);
    QCOMPARE(resizedFields.constFirst().type,
             PDFFormDesignerController::Checkbox);
    QCOMPARE(resizedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(resizedFields.constFirst().changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::RectPropertyChanged));
    QCOMPARE(resizedFields.constFirst().value, QStringLiteral("Off"));
    QCOMPARE(resizedFields.constFirst().defaultValue, QStringLiteral("Off"));

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Checkbox);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Off"));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());

    const QSize focusedRenderSize(1190, 1684);
    const QImage pageWithoutFocusedCheckbox = renderFirstPage(
        sourcePath,
        focusedRenderSize);
    const QImage pageWithFocusedCheckboxSuppressed
        = renderFirstPageWithForms(
            sourcePath,
            focusedRenderSize,
            {fieldId},
            resizedRect.center());
    QVERIFY(!pageWithoutFocusedCheckbox.isNull());
    QVERIFY(!pageWithFocusedCheckboxSuppressed.isNull());
    const QRect focusedCheckboxArea(
        qRound(resizedRect.x() * focusedRenderSize.width()),
        qRound(resizedRect.y() * focusedRenderSize.height()),
        qRound(resizedRect.width() * focusedRenderSize.width()),
        qRound(resizedRect.height() * focusedRenderSize.height()));
    QCOMPARE(
        changedPixelCount(
            pageWithoutFocusedCheckbox,
            pageWithFocusedCheckboxSuppressed,
            focusedCheckboxArea.adjusted(-8, -8, 8, 8)),
        0);
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        resizedRect.center()));
    QVERIFY(!formDesigner.sourceFieldContainsPoint(
        0,
        initialRect.topLeft()));

    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("AcceptTerms"));
    QCOMPARE(inspection.widgetCount, 2);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_CHECKBOX);
    QCOMPARE(inspection.value, QStringLiteral("Off"));
    QCOMPARE(inspection.exportValue, QStringLiteral("Yes"));
    QVERIFY(!inspection.checked);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.pointHit);
    QVERIFY(qAbs(inspection.rect.left - 119.0) < 1.0);
    QVERIFY(qAbs(inspection.rect.right - 166.6) < 1.0);
    QVERIFY(qAbs(inspection.rect.top - 589.4) < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom - 522.04) < 1.0);
}

void PDFEditControllerTest::rehydratesTextFieldAfterSamePathSave()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-same-path.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFFormDesignerController formDesigner;
    PDFFormController formController;
    PDFEditController editController;
    formDesigner.setDocument(&document);
    formController.setDocument(&document);
    formDesigner.setFormController(&formController);
    editController.setDocument(&document);
    editController.setFormController(&formController);
    editController.setFormDesignerController(&formDesigner);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

    const QRectF initialFieldRect(0.18, 0.28, 0.42, 0.07);
    const QRectF blankFieldRect(0.18, 0.40, 0.42, 0.07);
    const QString fieldId = formDesigner.addTextField(
        0,
        initialFieldRect.x(),
        initialFieldRect.y(),
        initialFieldRect.width(),
        initialFieldRect.height());
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("PersistentText")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("First saved value")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));
    QVERIFY(!formDesigner.sourceTextFieldContainsPoint(
        0,
        initialFieldRect.center()));
    const QString blankFieldId = formDesigner.addTextField(
        0,
        blankFieldRect.x(),
        blankFieldRect.y(),
        blankFieldRect.width(),
        blankFieldRect.height());
    QVERIFY(!blankFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        blankFieldId,
        QStringLiteral("PersistentBlank")));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QCOMPARE(
        saveFinishedSpy.constFirst().constFirst().toBool(),
        true);
    QVERIFY(document.isLoaded());
    QCOMPARE(
        QFileInfo(document.source().toLocalFile()).absoluteFilePath(),
        QFileInfo(sourcePath).absoluteFilePath());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QStringList sourceFieldIds
        = formDesigner.sourceTextFieldIdsForPage(0);
    sourceFieldIds.sort();
    QStringList expectedSourceFieldIds{fieldId, blankFieldId};
    expectedSourceFieldIds.sort();
    QCOMPARE(sourceFieldIds, expectedSourceFieldIds);
    QVERIFY(formDesigner.sourceTextFieldContainsPoint(
        0,
        initialFieldRect.center()));
    QVERIFY(formDesigner.sourceTextFieldContainsPoint(
        0,
        blankFieldRect.center()));
    QVERIFY(!formDesigner.sourceTextFieldContainsPoint(
        0,
        QPointF(0.9, 0.9)));

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("id")).toString(), fieldId);
    QCOMPARE(
        hydrated.value(QStringLiteral("name")).toString(),
        QStringLiteral("PersistentText"));
    QCOMPARE(
        hydrated.value(QStringLiteral("value")).toString(),
        QStringLiteral("First saved value"));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(
        hydrated.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Required);

    const AcroFormFieldInspection firstSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentText"));
    QVERIFY(firstSave.readable);
    QCOMPARE(firstSave.widgetCount, 2);
    QCOMPARE(firstSave.value, QStringLiteral("First saved value"));
    QVERIFY(firstSave.hasAppearance);
    QVERIFY(firstSave.pointHit);
    QVERIFY(firstSave.focusHandled);
    const AcroFormFieldInspection blankSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentBlank"));
    QCOMPARE(blankSave.widgetCount, 2);
    QCOMPARE(blankSave.value, QString());
    QVERIFY(blankSave.hasAppearance);
    QVERIFY(blankSave.pointHit);
    QVERIFY(blankSave.focusHandled);

    const QRectF resizedFieldRect(0.24, 0.20, 0.50, 0.10);
    const auto normalizedRectsMatch = [](const QRectF &actual,
                                         const QRectF &expected) {
        constexpr qreal tolerance = 0.00005;
        return qAbs(actual.x() - expected.x()) <= tolerance
            && qAbs(actual.y() - expected.y()) <= tolerance
            && qAbs(actual.width() - expected.width()) <= tolerance
            && qAbs(actual.height() - expected.height()) <= tolerance;
    };
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Updated after reopen")));
    QVERIFY(formDesigner.setTextFieldRect(
        fieldId,
        resizedFieldRect.x(),
        resizedFieldRect.y(),
        resizedFieldRect.width(),
        resizedFieldRect.height()));
    const QPointF exposedSourcePoint(0.20, 0.33);
    const QPointF resizedOnlyPoint(0.70, 0.25);
    QVERIFY(initialFieldRect.contains(exposedSourcePoint));
    QVERIFY(!resizedFieldRect.contains(exposedSourcePoint));
    QVERIFY(!initialFieldRect.contains(resizedOnlyPoint));
    QVERIFY(resizedFieldRect.contains(resizedOnlyPoint));
    QVERIFY(formDesigner.sourceTextFieldContainsPoint(
        0,
        exposedSourcePoint));
    QVERIFY(!formDesigner.sourceTextFieldContainsPoint(
        0,
        resizedOnlyPoint));
    const QVector<PDFFormDesignerController::SaveField> geometryChange
        = formDesigner.saveFields();
    QCOMPARE(geometryChange.size(), 1);
    QCOMPARE(
        geometryChange.constFirst().operation,
        PDFFormDesignerController::SaveField::Update);
    QCOMPARE(
        geometryChange.constFirst().changedProperties,
        static_cast<quint32>(
            PDFFormDesignerController::ValuePropertyChanged
            | PDFFormDesignerController::RectPropertyChanged));
    QCOMPARE(
        geometryChange.constFirst().normalizedRect,
        resizedFieldRect);
    const QVector<PDFFormDesignerController::SaveField>
        fieldsWithAppearanceNormalization
        = formDesigner.saveFields(true);
    QCOMPARE(fieldsWithAppearanceNormalization.size(), 1);
    QCOMPARE(
        fieldsWithAppearanceNormalization.constFirst().id,
        fieldId);
    QVERIFY(formDesigner.hasDrafts());
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QCOMPARE(
        saveFinishedSpy.constLast().constFirst().toBool(),
        true);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QVERIFY(!formDesigner.sourceTextFieldContainsPoint(
        0,
        exposedSourcePoint));
    QVERIFY(formDesigner.sourceTextFieldContainsPoint(
        0,
        resizedOnlyPoint));

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(
        hydrated.value(QStringLiteral("value")).toString(),
        QStringLiteral("Updated after reopen"));
    QVERIFY(normalizedRectsMatch(
        hydrated.value(QStringLiteral("rect")).toRectF(),
        resizedFieldRect));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    const AcroFormFieldInspection secondSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentText"));
    QCOMPARE(secondSave.widgetCount, 2);
    QCOMPARE(secondSave.value, QStringLiteral("Updated after reopen"));
    QVERIFY(secondSave.hasAppearance);
    QVERIFY(secondSave.pointHit);
    QVERIFY(secondSave.focusHandled);
    constexpr qreal rectTolerance = 0.05;
    QVERIFY(qAbs(
        secondSave.rect.left
        - resizedFieldRect.left() * 595.0) <= rectTolerance);
    QVERIFY(qAbs(
        secondSave.rect.right
        - resizedFieldRect.right() * 595.0) <= rectTolerance);
    QVERIFY(qAbs(
        secondSave.rect.bottom
        - (1.0 - resizedFieldRect.bottom()) * 842.0) <= rectTolerance);
    QVERIFY(qAbs(
        secondSave.rect.top
        - (1.0 - resizedFieldRect.top()) * 842.0) <= rectTolerance);

    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    const QSize nativeEditSize(595, 842);
    const QPointF nativeEditCenter(
        resizedFieldRect.center().x() * nativeEditSize.width(),
        resizedFieldRect.center().y() * nativeEditSize.height());
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Press,
        0,
        nativeEditCenter,
        nativeEditSize);
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Release,
        0,
        nativeEditCenter,
        nativeEditSize);
    QTRY_VERIFY_WITH_TIMEOUT(formController.acceptsTextInput(), 5000);
    formController.selectAll();
    formController.replaceSelection(
        QStringLiteral("Native value must survive"));
    formController.keyEvent(FWL_VKEY_Back, 0, {});
    const QString nativeValueAfterBackspace
        = QStringLiteral("Native value must surviv");
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);
    QCOMPARE(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Updated after reopen"));
    formDesigner.prepareForDesignMode();
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        nativeValueAfterBackspace,
        5000);
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("PersistentTextRenamed")));
    const QVector<PDFFormDesignerController::SaveField> nameChange
        = formDesigner.saveFields();
    QCOMPARE(nameChange.size(), 1);
    QCOMPARE(nameChange.constFirst().operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(
        nameChange.constFirst().changedProperties,
        static_cast<quint32>(
            PDFFormDesignerController::NamePropertyChanged));

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QCOMPARE(
        saveFinishedSpy.constLast().constFirst().toBool(),
        true);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(
        hydrated.value(QStringLiteral("name")).toString(),
        QStringLiteral("PersistentTextRenamed"));
    QCOMPARE(
        hydrated.value(QStringLiteral("value")).toString(),
        nativeValueAfterBackspace);
    const AcroFormFieldInspection nativeValueSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentTextRenamed"));
    QCOMPARE(nativeValueSave.widgetCount, 2);
    QCOMPARE(
        nativeValueSave.value,
        nativeValueAfterBackspace);
    QVERIFY(nativeValueSave.hasAppearance);
    QVERIFY(nativeValueSave.pointHit);
    QVERIFY(nativeValueSave.focusHandled);

    const QSize renderSize(595, 842);
    const QImage pageWithoutForms = renderFirstPage(
        sourcePath,
        renderSize);
    const QImage pageWithForms = renderFirstPageWithForms(
        sourcePath,
        renderSize);
    const QImage pageWithDesignerWidgetsSuppressed
        = renderFirstPageWithForms(
            sourcePath,
            renderSize,
            {fieldId, blankFieldId});
    const QImage pageWithFocusedDesignerWidgetsSuppressed
        = renderFirstPageWithForms(
            sourcePath,
            renderSize,
            {fieldId, blankFieldId},
            resizedFieldRect.center());
    const QImage pageWithOneDesignerWidgetSuppressed
        = renderFirstPageWithForms(
            sourcePath,
            renderSize,
            {fieldId});
    QVERIFY(!pageWithoutForms.isNull());
    QVERIFY(!pageWithForms.isNull());
    QVERIFY(!pageWithDesignerWidgetsSuppressed.isNull());
    QVERIFY(!pageWithFocusedDesignerWidgetsSuppressed.isNull());
    QVERIFY(!pageWithOneDesignerWidgetSuppressed.isNull());
    const auto fieldArea = [&renderSize](const QRectF &normalizedRect) {
        return QRect(
            qRound(normalizedRect.x() * renderSize.width()),
            qRound(normalizedRect.y() * renderSize.height()),
            qRound(normalizedRect.width() * renderSize.width()),
            qRound(normalizedRect.height() * renderSize.height()));
    };
    QVERIFY(
        changedPixelCount(
            pageWithoutForms,
            pageWithForms,
            fieldArea(resizedFieldRect))
        > 100);
    QVERIFY(
        changedPixelCount(
            pageWithoutForms,
            pageWithForms,
            fieldArea(blankFieldRect))
        > 100);
    QCOMPARE(
        changedPixelCount(
            pageWithoutForms,
            pageWithDesignerWidgetsSuppressed,
            fieldArea(resizedFieldRect)),
        0);
    QCOMPARE(
        changedPixelCount(
            pageWithoutForms,
            pageWithDesignerWidgetsSuppressed,
            fieldArea(blankFieldRect)),
        0);
    QCOMPARE(
        changedPixelCount(
            pageWithoutForms,
            pageWithFocusedDesignerWidgetsSuppressed,
            fieldArea(resizedFieldRect).adjusted(-6, -6, 6, 6)),
        0);
    QCOMPARE(
        changedPixelCount(
            pageWithoutForms,
            pageWithOneDesignerWidgetSuppressed,
            fieldArea(resizedFieldRect)),
        0);
    QVERIFY(
        changedPixelCount(
            pageWithoutForms,
            pageWithOneDesignerWidgetSuppressed,
            fieldArea(blankFieldRect))
        > 100);
    const QRect appearanceBounds = changedPixelBounds(
        pageWithOneDesignerWidgetSuppressed,
        pageWithForms,
        pageWithForms.rect());
    QVERIFY(!appearanceBounds.isEmpty());
    const QRect expectedAppearanceBounds = fieldArea(resizedFieldRect);
    constexpr int appearanceEdgeTolerance = 2;
    QVERIFY(
        qAbs(
            appearanceBounds.left()
            - expectedAppearanceBounds.left())
        <= appearanceEdgeTolerance);
    QVERIFY(
        qAbs(
            appearanceBounds.top()
            - expectedAppearanceBounds.top())
        <= appearanceEdgeTolerance);
    QVERIFY(
        qAbs(
            appearanceBounds.right()
            - expectedAppearanceBounds.right())
        <= appearanceEdgeTolerance);
    QVERIFY(
        qAbs(
            appearanceBounds.bottom()
            - expectedAppearanceBounds.bottom())
        <= appearanceEdgeTolerance);
    constexpr int cornerExtent = 4;
    const QVector<QRect> cornerArms{
        QRect(appearanceBounds.left(),
              appearanceBounds.top(),
              cornerExtent,
              1),
        QRect(appearanceBounds.left(),
              appearanceBounds.top(),
              1,
              cornerExtent),
        QRect(appearanceBounds.right() - cornerExtent + 1,
              appearanceBounds.top(),
              cornerExtent,
              1),
        QRect(appearanceBounds.right(),
              appearanceBounds.top(),
              1,
              cornerExtent),
        QRect(appearanceBounds.left(),
              appearanceBounds.bottom(),
              cornerExtent,
              1),
        QRect(appearanceBounds.left(),
              appearanceBounds.bottom() - cornerExtent + 1,
              1,
              cornerExtent),
        QRect(appearanceBounds.right() - cornerExtent + 1,
              appearanceBounds.bottom(),
              cornerExtent,
              1),
        QRect(appearanceBounds.right(),
              appearanceBounds.bottom() - cornerExtent + 1,
              1,
              cornerExtent)};
    for (qsizetype arm = 0; arm < cornerArms.size(); ++arm) {
        const qsizetype changedArmPixels = changedPixelCount(
            pageWithoutForms,
            pageWithForms,
            cornerArms.at(arm));
        QVERIFY2(
            changedArmPixels >= cornerExtent - 1,
            qPrintable(QStringLiteral(
                "AcroForm text-field corner arm %1 contains only %2 "
                "changed pixels; its border must remain square.")
                           .arg(arm)
                           .arg(changedArmPixels)));
    }
    const QRect textBounds = darkChangedPixelBounds(
        pageWithoutForms,
        pageWithForms,
        fieldArea(resizedFieldRect).adjusted(4, 4, -4, -4));
    QVERIFY(!textBounds.isEmpty());
    QVERIFY2(
        textBounds.height() >= 7 && textBounds.height() <= 18,
        qPrintable(QStringLiteral(
            "The unfocused AcroForm appearance rendered text %1 px high; "
            "it must stay consistent with the 12 pt native editor.")
                       .arg(textBounds.height())));

    PDFDocumentSession freshSession;
    QVERIFY(freshSession.open(QUrl::fromLocalFile(sourcePath)));
    QTRY_COMPARE_WITH_TIMEOUT(
        freshSession.formDesignerController()->rowCount(),
        2,
        10000);
    const QVariantMap freshField
        = freshSession.formDesignerController()->fieldData(fieldId);
    QCOMPARE(
        freshField.value(QStringLiteral("value")).toString(),
        nativeValueAfterBackspace);
    QCOMPARE(
        freshField.value(QStringLiteral("name")).toString(),
        QStringLiteral("PersistentTextRenamed"));
    QVERIFY(normalizedRectsMatch(
        freshField.value(QStringLiteral("rect")).toRectF(),
        resizedFieldRect));
    QVERIFY(freshField.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!freshSession.formDesignerController()->hasDrafts());
    QVERIFY(freshSession.formDesignerController()->setFieldValue(
        fieldId,
        QStringLiteral("Discard this value")));
    QVERIFY(freshSession.formDesignerController()->hasDrafts());
    freshSession.editController()->discardDrafts();
    QTRY_COMPARE_WITH_TIMEOUT(
        freshSession.formDesignerController()->rowCount(),
        2,
        10000);
    QTRY_COMPARE_WITH_TIMEOUT(
        freshSession.formDesignerController()
            ->fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        nativeValueAfterBackspace,
        10000);
    QVERIFY(!freshSession.formDesignerController()->hasDrafts());
}

void PDFEditControllerTest::normalizesLegacyTextFieldAppearances()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("legacy-appearance-source.pdf"));
    const QString legacyPath = m_temporaryDirectory.filePath(
        QStringLiteral("legacy-appearance.pdf"));
    const QString normalizedPath = m_temporaryDirectory.filePath(
        QStringLiteral("normalized-appearance.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    PDFFormDesignerController sourceDesigner;
    sourceDesigner.setDocument(&sourceDocument);
    PDFEditController sourceEditor;
    sourceEditor.setDocument(&sourceDocument);
    sourceEditor.setFormDesignerController(&sourceDesigner);

    const QString editedFieldId = sourceDesigner.addTextField(
        0,
        0.1,
        0.2,
        0.35,
        0.07);
    const QString legacyFieldId = sourceDesigner.addTextField(
        0,
        0.55,
        0.2,
        0.35,
        0.07);
    QVERIFY(!editedFieldId.isEmpty());
    QVERIFY(!legacyFieldId.isEmpty());
    QVERIFY(sourceDesigner.setFieldName(
        editedFieldId,
        QStringLiteral("EditedPeer")));
    QVERIFY(sourceDesigner.setFieldName(
        legacyFieldId,
        QStringLiteral("LegacyPeer")));
    QVERIFY(sourceDesigner.setFieldValue(
        editedFieldId,
        QStringLiteral("Edited value")));
    QVERIFY(sourceDesigner.setFieldValue(
        legacyFieldId,
        QStringLiteral("Legacy value")));
    const PDFSaveWriter::Result initialSave = PDFSaveWriter::save(
        sourceEditor,
        legacyPath);
    QVERIFY2(initialSave.succeeded(), qPrintable(initialSave.detail));

    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::ReadWrite));
    QByteArray legacyBytes = legacyFile.readAll();
    const QByteArray appearanceMarker("/TrePDFApVersion 1");
    const qsizetype firstMarker = legacyBytes.indexOf(appearanceMarker);
    const qsizetype secondMarker = legacyBytes.indexOf(
        appearanceMarker,
        firstMarker + appearanceMarker.size());
    QVERIFY(firstMarker >= 0);
    QVERIFY(secondMarker > firstMarker);
    const QByteArray legacyMaxLength
        = QByteArrayLiteral("/MaxLen 12")
              .leftJustified(appearanceMarker.size(), ' ');
    QCOMPARE(legacyMaxLength.size(), appearanceMarker.size());
    legacyBytes.replace(
        secondMarker,
        appearanceMarker.size(),
        legacyMaxLength);
    QVERIFY(legacyFile.seek(0));
    QCOMPARE(legacyFile.write(legacyBytes), legacyBytes.size());
    QVERIFY(legacyFile.resize(legacyBytes.size()));
    legacyFile.close();

    PDFDocument legacyDocument;
    PDFFormController legacyFormController;
    PDFFormDesignerController legacyDesigner;
    PDFEditController legacyEditor;
    legacyFormController.setDocument(&legacyDocument);
    legacyDesigner.setFormController(&legacyFormController);
    legacyDesigner.setDocument(&legacyDocument);
    legacyEditor.setDocument(&legacyDocument);
    legacyEditor.setFormController(&legacyFormController);
    legacyEditor.setFormDesignerController(&legacyDesigner);
    QVERIFY(legacyDocument.open(QUrl::fromLocalFile(legacyPath)));
    QTRY_COMPARE_WITH_TIMEOUT(legacyDesigner.rowCount(), 2, 10000);

    QVERIFY(legacyDesigner.setFieldValue(
        editedFieldId,
        QStringLiteral("Trigger normalization")));
    const QVector<PDFFormDesignerController::SaveField> saveFields
        = legacyDesigner.saveFields(true);
    QCOMPARE(saveFields.size(), 2);
    const auto legacyField = std::find_if(
        saveFields.cbegin(),
        saveFields.cend(),
        [&legacyFieldId](
            const PDFFormDesignerController::SaveField &field) {
            return field.id == legacyFieldId;
        });
    QVERIFY(legacyField != saveFields.cend());
    QCOMPARE(
        legacyField->changedProperties,
        static_cast<quint32>(
            PDFFormDesignerController::NoPropertyChanges));

    const PDFSaveWriter::Result normalizedSave = PDFSaveWriter::save(
        legacyEditor,
        normalizedPath);
    QVERIFY2(
        normalizedSave.succeeded(),
        qPrintable(normalizedSave.detail));
    const AcroFormFieldInspection normalizedLegacyField
        = inspectAcroFormField(
            normalizedPath,
            QStringLiteral("LegacyPeer"));
    QVERIFY(normalizedLegacyField.readable);
    QVERIFY(normalizedLegacyField.hasAppearance);
    QVERIFY(normalizedLegacyField.hasMaxLength);
    QCOMPARE(normalizedLegacyField.maxLength, 12.0f);

    PDFDocument reopenedDocument;
    PDFFormController reopenedFormController;
    PDFFormDesignerController reopenedDesigner;
    reopenedFormController.setDocument(&reopenedDocument);
    reopenedDesigner.setFormController(&reopenedFormController);
    reopenedDesigner.setDocument(&reopenedDocument);
    QVERIFY(reopenedDocument.open(
        QUrl::fromLocalFile(normalizedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(reopenedDesigner.rowCount(), 2, 10000);
    QVERIFY(reopenedDesigner.setFieldValue(
        editedFieldId,
        QStringLiteral("Current appearance")));
    const QVector<PDFFormDesignerController::SaveField>
        currentAppearanceFields = reopenedDesigner.saveFields(true);
    QCOMPARE(currentAppearanceFields.size(), 1);
    QCOMPARE(currentAppearanceFields.constFirst().id, editedFieldId);
}

void PDFEditControllerTest::addsTextFieldToExistingAcroForm()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("existing-acroform-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("existing-acroform-saved.pdf"));
    const QString resavedPath = m_temporaryDirectory.filePath(
        QStringLiteral("existing-acroform-resaved.pdf"));
    QVERIFY(createChoiceFieldsPdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    PDFFormDesignerController formDesigner;
    formDesigner.setDocument(&document);
    PDFEditController editController;
    editController.setDocument(&document);
    editController.setFormDesignerController(&formDesigner);

    const QString fieldId = formDesigner.addTextField(
        0,
        0.55,
        0.75,
        0.35,
        0.08);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("AdditionalText")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Preserved choices")));

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        editController,
        savedPath);
    QVERIFY2(result.succeeded(), qPrintable(result.detail));
    QVERIFY(QFileInfo::exists(savedPath));

    const AcroFormFieldInspection addedField
        = inspectAcroFormField(
            savedPath,
            QStringLiteral("AdditionalText"));
    QVERIFY(addedField.readable);
    QCOMPARE(addedField.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(addedField.widgetCount, 7);
    QCOMPARE(addedField.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(addedField.name, QStringLiteral("AdditionalText"));
    QCOMPARE(addedField.value, QStringLiteral("Preserved choices"));
    QVERIFY(addedField.hasAppearance);
    QVERIFY(addedField.pointHit);
    QVERIFY(addedField.focusHandled);

    PDFDocument reopened;
    PDFFormController formController;
    formController.setDocument(&reopened);
    QVERIFY(reopened.open(QUrl::fromLocalFile(savedPath)));
    QVERIFY(formController.interactive());
    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    QVector<PDFFormWorker::ChoiceField> fields;
    bool inspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&fields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> inspectedFields) {
            fields = std::move(inspectedFields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(fields.size(), 3);
    QCOMPARE(
        fields.at(0).options,
        QStringList({
            QStringLiteral("Airplane"),
            QStringLiteral("Bicycle"),
            QStringLiteral("Car"),
        }));
    QCOMPARE(fields.at(0).selectedIndices, QVector<int>({0}));
    QCOMPARE(
        fields.at(1).options,
        QStringList({
            QStringLiteral("March"),
            QStringLiteral("April"),
            QStringLiteral("May"),
            QStringLiteral("June"),
        }));
    QCOMPARE(fields.at(1).selectedIndices, QVector<int>({1}));
    QCOMPARE(
        fields.at(2).options,
        QStringList({
            QStringLiteral("Red"),
            QStringLiteral("Yellow"),
            QStringLiteral("Blue"),
        }));
    QCOMPARE(fields.at(2).selectedIndices, QVector<int>({0, 2}));

    const QSize deviceSize(360, 480);
    const QPointF fieldCenter(261.0, 379.2);
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Press,
        0,
        fieldCenter,
        deviceSize);
    formController.pointerEvent(
        0,
        PDFFormWorker::PointerEventType::Release,
        0,
        fieldCenter,
        deviceSize);
    QTRY_VERIFY_WITH_TIMEOUT(formController.acceptsTextInput(), 5000);
    formController.selectAll();
    formController.replaceSelection(
        QStringLiteral("Edited after reopen"));
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);

    PDFFormDesignerController reopenedFormDesigner;
    reopenedFormDesigner.setDocument(&reopened);
    const QString secondFieldId = reopenedFormDesigner.addTextField(
        0,
        0.55,
        0.67,
        0.35,
        0.06);
    QVERIFY(!secondFieldId.isEmpty());
    QVERIFY(reopenedFormDesigner.setFieldName(
        secondFieldId,
        QStringLiteral("SecondAdditionalText")));
    QVERIFY(reopenedFormDesigner.setFieldValue(
        secondFieldId,
        QStringLiteral("Second pass")));

    PDFEditController reopenedEditController;
    reopenedEditController.setDocument(&reopened);
    reopenedEditController.setFormController(&formController);
    reopenedEditController.setFormDesignerController(
        &reopenedFormDesigner);
    QVERIFY(reopenedEditController.hasDrafts());
    QSignalSpy saveFinishedSpy(
        &reopenedEditController,
        &PDFEditController::saveFinished);
    QVERIFY(reopenedEditController.requestSaveAs(
        QUrl::fromLocalFile(resavedPath)));
    QVERIFY(reopenedEditController.saving());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QCOMPARE(
        saveFinishedSpy.constFirst().constFirst().toBool(),
        true);
    QTRY_VERIFY_WITH_TIMEOUT(!reopenedEditController.saving(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        QFileInfo::exists(resavedPath)
            && QFileInfo(resavedPath).size() > 0,
        5000);

    const AcroFormFieldInspection editedField
        = inspectAcroFormField(
            resavedPath,
            QStringLiteral("AdditionalText"));
    QVERIFY(editedField.readable);
    QCOMPARE(editedField.widgetCount, 8);
    QCOMPARE(editedField.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(editedField.value, QStringLiteral("Edited after reopen"));
    const AcroFormFieldInspection secondAddedField
        = inspectAcroFormField(
            resavedPath,
            QStringLiteral("SecondAdditionalText"));
    QCOMPARE(secondAddedField.widgetCount, 8);
    QCOMPARE(secondAddedField.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QCOMPARE(secondAddedField.value, QStringLiteral("Second pass"));
    QVERIFY(secondAddedField.hasAppearance);
    QVERIFY(secondAddedField.pointHit);
    QVERIFY(secondAddedField.focusHandled);

    PDFDocument twiceReopened;
    PDFFormController twiceReopenedFormController;
    twiceReopenedFormController.setDocument(&twiceReopened);
    QVERIFY(twiceReopened.open(QUrl::fromLocalFile(resavedPath)));
    fields.clear();
    inspectionCompleted = false;
    twiceReopenedFormController.requestChoiceFields(
        0,
        [&fields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> inspectedFields) {
            fields = std::move(inspectedFields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(fields.size(), 3);
    QCOMPARE(
        fields.at(0).options,
        QStringList({
            QStringLiteral("Airplane"),
            QStringLiteral("Bicycle"),
            QStringLiteral("Car"),
        }));
    QCOMPARE(fields.at(0).selectedIndices, QVector<int>({0}));
    QCOMPARE(
        fields.at(1).options,
        QStringList({
            QStringLiteral("March"),
            QStringLiteral("April"),
            QStringLiteral("May"),
            QStringLiteral("June"),
        }));
    QCOMPARE(fields.at(1).selectedIndices, QVector<int>({1}));
    QCOMPARE(
        fields.at(2).options,
        QStringList({
            QStringLiteral("Red"),
            QStringLiteral("Yellow"),
            QStringLiteral("Blue"),
        }));
    QCOMPARE(fields.at(2).selectedIndices, QVector<int>({0, 2}));
}

void PDFEditControllerTest::rejectsDuplicateAcroFormFieldNames()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("duplicate-acroform-name-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("duplicate-acroform-name-saved.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    PDFFormDesignerController formDesigner;
    formDesigner.setDocument(&document);
    PDFEditController editController;
    editController.setDocument(&document);
    editController.setFormDesignerController(&formDesigner);

    const QString firstFieldId = formDesigner.addTextField(
        0,
        0.1,
        0.2,
        0.3,
        0.08);
    const QString secondFieldId = formDesigner.addTextField(
        0,
        0.55,
        0.2,
        0.3,
        0.08);
    QVERIFY(!firstFieldId.isEmpty());
    QVERIFY(!secondFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        firstFieldId,
        QStringLiteral("DuplicateName")));
    QVERIFY(formDesigner.setFieldName(
        secondFieldId,
        QStringLiteral("DuplicateName")));

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        editController,
        savedPath);
    QCOMPARE(result.error, PDFSaveWriter::Error::AcroFormCreationFailed);
    QVERIFY(!result.detail.isEmpty());
    QVERIFY(!QFileInfo::exists(savedPath));
}

void PDFEditControllerTest::enablesFormScriptingAutomatically()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("automatic-form-scripting.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFFormController controller;
    controller.setDocument(&document);
    QSignalSpy enableFailedSpy(
        &controller,
        &PDFFormController::javascriptEnableFailed);

    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QTRY_VERIFY_WITH_TIMEOUT(controller.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !controller.javascriptTransitionPending(),
        5000);
    QCOMPARE(enableFailedSpy.count(), 0);
}

void PDFEditControllerTest::customizesChoiceFieldsAndPersistsSelection()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("choice-fields.pdf"));
    QVERIFY(createChoiceFieldsPdf(sourcePath));

    PDFDocument document;
    PDFFormController controller;
    controller.setDocument(&document);
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QVERIFY(controller.interactive());

    QVector<PDFFormWorker::ChoiceField> fields;
    bool inspectionCompleted = false;
    controller.requestChoiceFields(
        0,
        [&fields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> inspectedFields) {
            fields = std::move(inspectedFields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(fields.size(), 3);

    const PDFFormWorker::ChoiceField combo = fields.at(0);
    QCOMPARE(combo.annotationIndex, 0);
    QCOMPARE(combo.sourcePage, 0);
    QCOMPARE(combo.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(
        combo.accessibleName,
        QStringLiteral("Transport choice"));
    QVERIFY(!combo.identityToken.isEmpty());
    QCOMPARE(
        combo.options,
        QStringList({
            QStringLiteral("Airplane"),
            QStringLiteral("Bicycle"),
            QStringLiteral("Car"),
        }));
    QCOMPARE(combo.selectedIndices, QVector<int>({0}));
    QVERIFY(!combo.readOnly);
    QVERIFY(!combo.multiSelect);
    QVERIFY(qAbs(combo.normalizedRect.x() - 20.0 / 360.0) < 0.00001);
    QVERIFY(qAbs(combo.normalizedRect.y() - 50.0 / 480.0) < 0.00001);
    QVERIFY(qAbs(combo.normalizedRect.width() - 160.0 / 360.0) < 0.00001);
    QVERIFY(qAbs(combo.normalizedRect.height() - 30.0 / 480.0) < 0.00001);

    const PDFFormWorker::ChoiceField singleList = fields.at(1);
    QCOMPARE(singleList.fieldType, FPDF_FORMFIELD_LISTBOX);
    QCOMPARE(singleList.selectedIndices, QVector<int>({1}));
    QVERIFY(!singleList.multiSelect);

    const PDFFormWorker::ChoiceField multiList = fields.at(2);
    QCOMPARE(multiList.fieldType, FPDF_FORMFIELD_LISTBOX);
    QCOMPARE(multiList.selectedIndices, QVector<int>({0, 2}));
    QVERIFY(multiList.multiSelect);

    bool focusCompleted = false;
    bool focusSucceeded = false;
    controller.focusChoiceField(
        0,
        combo.annotationIndex,
        combo.sourcePage,
        combo.identityToken,
        [&focusCompleted, &focusSucceeded](bool focused) {
            focusSucceeded = focused;
            focusCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(focusCompleted, 5000);
    QVERIFY(focusSucceeded);
    QCOMPARE(controller.focusedPage(), 0);
    QCOMPARE(
        controller.focusedAnnotationIndex(),
        combo.annotationIndex);

    controller.selectChoiceOption(
        0,
        combo.annotationIndex,
        2,
        true,
        combo.sourcePage,
        false,
        combo.identityToken);
    controller.selectChoiceOption(
        0,
        singleList.annotationIndex,
        3,
        true,
        singleList.sourcePage,
        false,
        singleList.identityToken);
    controller.selectChoiceOption(
        0,
        multiList.annotationIndex,
        0,
        false,
        multiList.sourcePage,
        true,
        multiList.identityToken);
    controller.selectChoiceOption(
        0,
        multiList.annotationIndex,
        1,
        true,
        multiList.sourcePage,
        true,
        multiList.identityToken);
    // Toggle twice before either async refresh completes. The worker must
    // resolve each toggle against live PDFium state, not a stale QML snapshot.
    controller.selectChoiceOption(
        0,
        multiList.annotationIndex,
        2,
        false,
        multiList.sourcePage,
        true,
        multiList.identityToken);
    controller.selectChoiceOption(
        0,
        multiList.annotationIndex,
        2,
        false,
        multiList.sourcePage,
        true,
        multiList.identityToken);
    QTRY_VERIFY_WITH_TIMEOUT(controller.hasDrafts(), 5000);

    fields.clear();
    inspectionCompleted = false;
    controller.requestChoiceFields(
        0,
        [&fields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> inspectedFields) {
            fields = std::move(inspectedFields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(fields.size(), 3);
    QCOMPARE(fields.at(0).selectedIndices, QVector<int>({2}));
    QCOMPARE(fields.at(1).selectedIndices, QVector<int>({3}));
    QCOMPARE(fields.at(2).selectedIndices, QVector<int>({1, 2}));
    // PDFium does not expose undo history for non-editable choice fields,
    // including selections made through its native key and pointer paths.
    // Keep the custom controls aligned with that focus-local form behavior.
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QString snapshotPath;
    QString error;
    QVERIFY2(
        controller.exportSnapshotBlocking(snapshotPath, error),
        qPrintable(error));
    QCOMPARE(selectedChoiceOptions(snapshotPath, 0), QVector<int>({2}));
    QCOMPARE(selectedChoiceOptions(snapshotPath, 1), QVector<int>({3}));
    QCOMPARE(
        selectedChoiceOptions(snapshotPath, 2),
        QVector<int>({1, 2}));
    QVERIFY(QFile::remove(snapshotPath));
}

void PDFEditControllerTest::routesPushButtonActions_data()
{
    QTest::addColumn<QPointF>("clickPosition");
    QTest::addColumn<int>("initialCurrentPage");
    QTest::addColumn<int>("expectedPage");
    QTest::addColumn<bool>("expectExternalLink");

    QTest::newRow("goto-prev")
        << QPointF(60.0, 60.0)
        << 1
        << 0
        << false;
    QTest::newRow("goto-prev-while-model-already-targets-page")
        << QPointF(60.0, 60.0)
        << 0
        << 0
        << false;
    QTest::newRow("uri-https")
        << QPointF(180.0, 60.0)
        << 1
        << -1
        << true;
    QTest::newRow("goto-next")
        << QPointF(300.0, 60.0)
        << 1
        << 2
        << false;
}

void PDFEditControllerTest::routesPushButtonActions()
{
    QFETCH(QPointF, clickPosition);
    QFETCH(int, initialCurrentPage);
    QFETCH(int, expectedPage);
    QFETCH(bool, expectExternalLink);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("form-actions-%1.pdf").arg(
            QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createPushButtonActionPdf(sourcePath));

    PDFDocument document;
    PDFFormController controller;
    controller.setDocument(&document);
    QSignalSpy navigationSpy(
        &controller,
        &PDFFormController::pageNavigationRequested);
    QSignalSpy externalLinkSpy(
        &controller,
        &PDFFormController::externalLinkRequested);
    QSignalSpy cancelledLinkSpy(
        &controller,
        &PDFFormController::externalLinkRequestCancelled);
    QSignalSpy enableFailedSpy(
        &controller,
        &PDFFormController::javascriptEnableFailed);

    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QVERIFY(controller.interactive());
    QTRY_VERIFY_WITH_TIMEOUT(controller.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !controller.javascriptTransitionPending(),
        5000);
    QCOMPARE(enableFailedSpy.count(), 0);

    document.setCurrentPage(initialCurrentPage);
    QCOMPARE(document.currentPage(), initialCurrentPage);
    const QSize deviceSize(360, 480);
    controller.pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Press,
        0,
        clickPosition,
        deviceSize);
    controller.pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Release,
        0,
        clickPosition,
        deviceSize);

    if (expectExternalLink) {
        QTRY_COMPARE_WITH_TIMEOUT(externalLinkSpy.count(), 1, 5000);
        QCOMPARE(navigationSpy.count(), 0);
        QCOMPARE(document.currentPage(), initialCurrentPage);
        const QList<QVariant> arguments
            = externalLinkSpy.takeFirst();
        const quint64 requestId = arguments.at(0).toULongLong();
        QVERIFY(requestId != 0);
        QCOMPARE(
            arguments.at(1).toString(),
            QStringLiteral("example.com"));
        QCOMPARE(
            arguments.at(2).toString(),
            QStringLiteral("https://example.com/"));
        QCOMPARE(arguments.at(3).toBool(), false);
        controller.cancelExternalLink(requestId);
        QCOMPARE(cancelledLinkSpy.count(), 1);
    } else {
        QTRY_COMPARE_WITH_TIMEOUT(navigationSpy.count(), 1, 5000);
        QCOMPARE(
            navigationSpy.constFirst().constFirst().toInt(),
            expectedPage);
        QTRY_COMPARE_WITH_TIMEOUT(
            document.currentPage(),
            expectedPage,
            5000);
        QCOMPARE(externalLinkSpy.count(), 0);
    }
}

void PDFEditControllerTest::blocksUnsupportedFormUriSchemes()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("form-actions-blocked-uri.pdf"));
    QVERIFY(createPushButtonActionPdf(
        sourcePath,
        QByteArray("file:///tmp/blocked")));

    PDFDocument document;
    PDFFormController controller;
    controller.setDocument(&document);
    QSignalSpy navigationSpy(
        &controller,
        &PDFFormController::pageNavigationRequested);
    QSignalSpy externalLinkSpy(
        &controller,
        &PDFFormController::externalLinkRequested);

    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QTRY_VERIFY_WITH_TIMEOUT(controller.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !controller.javascriptTransitionPending(),
        5000);
    document.setCurrentPage(1);

    controller.pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Press,
        0,
        QPointF(180.0, 60.0),
        QSize(360, 480));
    controller.pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Release,
        0,
        QPointF(180.0, 60.0),
        QSize(360, 480));

    QVERIFY(controller.hasDrafts());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.hasDrafts(), 5000);
    QCOMPARE(externalLinkSpy.count(), 0);
    QCOMPARE(navigationSpy.count(), 0);
    QCOMPARE(document.currentPage(), 1);
}

void PDFEditControllerTest::forwardsExternalLinkRequestsThroughTabModel()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("form-actions-tab-model.pdf"));
    QVERIFY(createPushButtonActionPdf(sourcePath));

    PDFTabModel tabs;
    QSignalSpy externalLinkSpy(
        &tabs,
        &PDFTabModel::externalLinkRequested);
    PDFDocumentSession *session = tabs.open(
        QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->formController()->javascriptEnabled(),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !session->formController()->javascriptTransitionPending(),
        5000);
    session->document()->setCurrentPage(1);

    session->formController()->pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Press,
        0,
        QPointF(180.0, 60.0),
        QSize(360, 480));
    session->formController()->pointerEvent(
        1,
        PDFFormWorker::PointerEventType::Release,
        0,
        QPointF(180.0, 60.0),
        QSize(360, 480));

    QTRY_COMPARE_WITH_TIMEOUT(externalLinkSpy.count(), 1, 5000);
    const QList<QVariant> arguments = externalLinkSpy.takeFirst();
    QCOMPARE(
        arguments.at(0).value<PDFDocumentSession *>(),
        session);
    const quint64 requestId = arguments.at(1).toULongLong();
    QVERIFY(requestId != 0);
    QCOMPARE(
        arguments.at(2).toString(),
        QStringLiteral("example.com"));
    QCOMPARE(
        arguments.at(3).toString(),
        QStringLiteral("https://example.com/"));
    QCOMPARE(arguments.at(4).toBool(), false);
    session->formController()->cancelExternalLink(requestId);
}

void PDFEditControllerTest::serializesPdfiumAcrossThreads()
{
    const auto workerStarted = std::make_shared<QSemaphore>();
    const auto workerAcquired = std::make_shared<QSemaphore>();
    QThread *worker = nullptr;
    bool startedWhileLocked = false;
    bool remainedBlocked = false;
    {
        PDFiumProcessLock mainThreadLock;
        worker = QThread::create([workerStarted, workerAcquired]() {
            workerStarted->release();
            QElapsedTimer deadline;
            deadline.start();
            while (!QThread::currentThread()->isInterruptionRequested()
                   && deadline.elapsed() < 2000) {
                PDFiumProcessLock workerLock(
                    PDFiumProcessLock::Mode::TryLock);
                if (workerLock.isLocked()) {
                    workerAcquired->release();
                    return;
                }
                QThread::msleep(1);
            }
        });
        worker->start();

        startedWhileLocked = workerStarted->tryAcquire(1, 1000);
        remainedBlocked = startedWhileLocked
            && !workerAcquired->tryAcquire(1, 100);
    }

    const bool acquiredAfterUnlock = workerAcquired->tryAcquire(1, 1000);
    const bool finished = worker->wait(2500);
    if (!finished) {
        worker->requestInterruption();
        worker->wait();
    }
    delete worker;

    QVERIFY(startedWhileLocked);
    QVERIFY(remainedBlocked);
    QVERIFY(acquiredAfterUnlock);
    QVERIFY(finished);
}

void PDFEditControllerTest::discoversUncachedHoverOffGuiThread()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("async-hover-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QSignalSpy candidatesReadySpy(
        &controller,
        &PDFEditController::pageCandidatesReady);

    QElapsedTimer hoverTimer;
    hoverTimer.start();
    const QVariantMap initialHover = controller.candidateAt(0, 0.5, 0.5);
    const qint64 hoverMilliseconds = hoverTimer.elapsed();

    QVERIFY(initialHover.isEmpty());
    QVERIFY2(
        hoverMilliseconds < 250,
        qPrintable(QStringLiteral("Uncached hover took %1 ms")
                       .arg(hoverMilliseconds)));
    QTRY_VERIFY_WITH_TIMEOUT(candidatesReadySpy.size() > 0, 2000);
    QVERIFY(!candidateForText(
                 waitForCandidates(controller, 0),
                 QString::fromLatin1(kRegularText))
                 .isEmpty());
}

void PDFEditControllerTest::reschedulesCancelledHoverForSamePage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("rescheduled-hover-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QSignalSpy candidatesReadySpy(
        &controller,
        &PDFEditController::pageCandidatesReady);

    const auto holderLocked = std::make_shared<QSemaphore>();
    const auto releaseHolder = std::make_shared<QSemaphore>();
    QThread *holder = QThread::create([holderLocked, releaseHolder]() {
        QElapsedTimer deadline;
        deadline.start();
        while (!QThread::currentThread()->isInterruptionRequested()
               && deadline.elapsed() < 2000) {
            PDFiumProcessLock holderLock(PDFiumProcessLock::Mode::TryLock);
            if (holderLock.isLocked()) {
                holderLocked->release();
                releaseHolder->tryAcquire(1, 1000);
                return;
            }
            QThread::msleep(1);
        }
    });
    holder->start();
    const bool holderStarted = holderLocked->tryAcquire(1, 1000);

    QVariantMap initialHover;
    QVariantMap rescheduledHover;
    if (holderStarted) {
        initialHover = controller.candidateAt(0, 0.5, 0.5);
        controller.setDocument(nullptr);
        controller.setDocument(&document);
        rescheduledHover = controller.candidateAt(0, 0.5, 0.5);
    }
    releaseHolder->release();
    const bool holderFinished = holder->wait(2500);
    if (!holderFinished) {
        holder->requestInterruption();
        holder->wait();
    }
    delete holder;

    QVERIFY(holderStarted);
    QVERIFY(holderFinished);
    QVERIFY(initialHover.isEmpty());
    QVERIFY(rescheduledHover.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(candidatesReadySpy.size() > 0, 2000);
    QVERIFY(!candidateForText(
                 waitForCandidates(controller, 0),
                 QString::fromLatin1(kRegularText))
                 .isEmpty());
}

void PDFEditControllerTest::defersUncachedHoverWhilePdfiumIsBusy()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("deferred-hover-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QSignalSpy candidatesReadySpy(
        &controller,
        &PDFEditController::pageCandidatesReady);

    const auto holderLocked = std::make_shared<QSemaphore>();
    const auto releaseHolder = std::make_shared<QSemaphore>();
    QThread *holder = QThread::create([holderLocked, releaseHolder]() {
        QElapsedTimer deadline;
        deadline.start();
        while (!QThread::currentThread()->isInterruptionRequested()
               && deadline.elapsed() < 2000) {
            PDFiumProcessLock holderLock(PDFiumProcessLock::Mode::TryLock);
            if (holderLock.isLocked()) {
                holderLocked->release();
                releaseHolder->tryAcquire(1, 1000);
                return;
            }
            QThread::msleep(1);
        }
    });
    holder->start();
    const bool holderStarted = holderLocked->tryAcquire(1, 1000);

    QElapsedTimer hoverTimer;
    hoverTimer.start();
    const QVariantMap uncachedHover = holderStarted
        ? controller.candidateAt(0, 0.5, 0.5)
        : QVariantMap();
    const qint64 uncachedHoverMilliseconds = hoverTimer.elapsed();
    releaseHolder->release();
    const bool holderFinished = holder->wait(2500);
    if (!holderFinished) {
        holder->requestInterruption();
        holder->wait();
    }
    delete holder;

    QVERIFY(holderStarted);
    QVERIFY(holderFinished);
    QVERIFY(uncachedHover.isEmpty());
    QVERIFY2(
        uncachedHoverMilliseconds < 250,
        qPrintable(QStringLiteral("Uncached hover waited %1 ms")
                       .arg(uncachedHoverMilliseconds)));
    QTRY_VERIFY_WITH_TIMEOUT(candidatesReadySpy.size() > 0, 2000);

    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    const QRectF sourceRect = candidate.value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(!sourceRect.isEmpty());

    const auto cachedHolderLocked = std::make_shared<QSemaphore>();
    const auto releaseCachedHolder = std::make_shared<QSemaphore>();
    QThread *cachedHolder = QThread::create(
        [cachedHolderLocked, releaseCachedHolder]() {
            QElapsedTimer deadline;
            deadline.start();
            while (!QThread::currentThread()->isInterruptionRequested()
                   && deadline.elapsed() < 2000) {
                PDFiumProcessLock holderLock(PDFiumProcessLock::Mode::TryLock);
                if (holderLock.isLocked()) {
                    cachedHolderLocked->release();
                    releaseCachedHolder->tryAcquire(1, 1000);
                    return;
                }
                QThread::msleep(1);
            }
        });
    cachedHolder->start();
    const bool cachedHolderStarted = cachedHolderLocked->tryAcquire(1, 1000);

    hoverTimer.restart();
    const QVariantMap cachedHover = cachedHolderStarted
        ? controller.candidateAt(
              0,
              sourceRect.center().x(),
              sourceRect.center().y())
        : QVariantMap();
    const qint64 cachedHoverMilliseconds = hoverTimer.elapsed();
    releaseCachedHolder->release();
    const bool cachedHolderFinished = cachedHolder->wait(2500);
    if (!cachedHolderFinished) {
        cachedHolder->requestInterruption();
        cachedHolder->wait();
    }
    delete cachedHolder;

    QVERIFY(cachedHolderStarted);
    QVERIFY(cachedHolderFinished);
    QVERIFY(!cachedHover.isEmpty());
    QVERIFY2(
        cachedHoverMilliseconds < 250,
        qPrintable(QStringLiteral("Cached hover waited %1 ms")
                       .arg(cachedHoverMilliseconds)));
}

void PDFEditControllerTest::registersBundledFonts()
{
    QVERIFY(QFile::exists(QStringLiteral(
        ":/trepdf/fonts/Arial/Arial-Regular.TTF")));
    QVERIFY(ApplicationFonts::bundledFontFamilies().contains(
        QStringLiteral("Arial"),
        Qt::CaseInsensitive));
    QCOMPARE(
        m_controller.bundledFontFamilies(),
        ApplicationFonts::bundledFontFamilies());
    QVERIFY(!ApplicationFonts::systemFontFamilies().isEmpty());

    const QStringList availableFamilies = m_controller.availableFontFamilies();
    QCOMPARE(
        availableFamilies.first(),
        ApplicationFonts::bundledFontFamilies().first());
    QVERIFY(availableFamilies.contains(
        QStringLiteral("Arial"),
        Qt::CaseInsensitive));

    QStringList uniqueFamilies;
    for (const QString &family : availableFamilies) {
        QVERIFY(ApplicationFonts::isFontFaceSelectable(family, false, false));
        QVERIFY(!uniqueFamilies.contains(family, Qt::CaseInsensitive));
        uniqueFamilies.append(family);
    }
}

void PDFEditControllerTest::keepsArialLayoutStableAfterEmbeddedSubsetCollision()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("arial-collision-source.pdf"));
    const QString collisionPath = m_temporaryDirectory.filePath(
        QStringLiteral("arial-collision-subset.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("arial-collision-output.pdf"));
    QVERIFY(createArialSubsetCollisionSourcePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    const QVariantMap seedCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QStringLiteral("Á"));
    QVERIFY(!seedCandidate.isEmpty());
    QVERIFY(seedCandidate.value(QStringLiteral("fontFamily")).toString()
            != QStringLiteral("Arial"));
    QVERIFY(controller.selectCandidate(
        0,
        seedCandidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.setSelectedTextRange(0, QStringLiteral("Á").size());
    controller.setSelectedFontFamily(QStringLiteral("Arial"));
    controller.finishSelectedTextEdit();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(collisionPath)));

    const TextObjectFontInfo embeddedFont = textObjectFontInfo(
        collisionPath,
        QStringLiteral("Á"));
    QVERIFY(embeddedFont.found);
    QCOMPARE(embeddedFont.embedded, 1);
    QVERIFY(hasSubsetFontPrefix(embeddedFont.baseName));
    QCOMPARE(embeddedFont.familyName, QStringLiteral("Arial"));

    const TextObjectFontInfo sourceFont = textObjectFontInfo(
        collisionPath,
        QString::fromLatin1(kFontCollisionTargetText));
    QVERIFY(sourceFont.found);
    QCOMPARE(sourceFont.embedded, 0);
    QCOMPARE(sourceFont.familyName, QStringLiteral("Arial"));

    const QVariantMap targetCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kFontCollisionTargetText));
    QVERIFY(!targetCandidate.isEmpty());
    QCOMPARE(
        targetCandidate.value(QStringLiteral("fontFamily")).toString(),
        QStringLiteral("Arial"));
    QVERIFY(controller.selectCandidate(
        0,
        targetCandidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();

    const QSizeF pageSize = document.pageSize(0);
    const qreal layoutWidth = (
        controller.selectedFrameRect().right() - controller.selectedOriginX())
        * pageSize.width();
    const QFont previewFont = controller.fontForDisplay(
        controller.selectedFontFamily(),
        controller.selectedFontStretch(),
        controller.selectedFontBold(),
        controller.selectedFontItalic(),
        controller.selectedFontUnderline(),
        controller.selectedDisplayFontSizeRatio() * pageSize.height());
    const QFontMetricsF previewMetrics(previewFont);
    QString editedText = QString::fromLatin1(kFontCollisionTargetText);
    while (editedText.size() < 1024) {
        const QString extendedText = editedText + QLatin1Char('x');
        if (previewMetrics.horizontalAdvance(extendedText)
            >= layoutWidth - 8.0) {
            break;
        }
        editedText = extendedText;
    }
    QVERIFY(layoutWidth > 0.0);
    QVERIFY(editedText.size()
            > QString::fromLatin1(kFontCollisionTargetText).size());
    QVERIFY(previewMetrics.horizontalAdvance(editedText) < layoutWidth - 8.0);
    QVERIFY(previewMetrics.horizontalAdvance(editedText) > layoutWidth * 0.75);

    controller.updateSelectedTextPreview(editedText);
    controller.finishSelectedTextEdit();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY2(
        topLevelTextObjects(outputPath).contains(editedText),
        qPrintable(topLevelTextObjects(outputPath).join(QLatin1Char('|'))));
}

void PDFEditControllerTest::filtersUnsupportedSystemFonts()
{
    for (int styleIndex = 0; styleIndex < 4; ++styleIndex) {
        const bool bold = (styleIndex & 2) != 0;
        const bool italic = (styleIndex & 1) != 0;
        const QStringList families = ApplicationFonts::availableFontFamilies(
            bold,
            italic);
        QVERIFY(families.contains(QStringLiteral("Arial"), Qt::CaseInsensitive));

        QStringList uniqueFamilies;
        for (const QString &family : families) {
            QVERIFY(ApplicationFonts::isFontFaceSelectable(
                family,
                bold,
                italic));
            QVERIFY(!uniqueFamilies.contains(family, Qt::CaseInsensitive));
            uniqueFamilies.append(family);
        }
    }

    const QString appleColorEmoji = QStringLiteral("Apple Color Emoji");
    if (ApplicationFonts::systemFontFamilies().contains(
            appleColorEmoji,
            Qt::CaseInsensitive)) {
        QVERIFY(!ApplicationFonts::isFontFaceSelectable(
            appleColorEmoji,
            false,
            false));
        QVERIFY(!ApplicationFonts::availableFontFamilies().contains(
            appleColorEmoji,
            Qt::CaseInsensitive));
        QVERIFY(!ApplicationFonts::systemFontFace(
            appleColorEmoji,
            false,
            false).isValid());

        QVERIFY(m_controller.selectCandidate(
            0,
            m_regularCandidate.value(QStringLiteral("objectId")).toString()));
        const QString originalFamily = m_controller.selectedFontFamily();
        m_controller.setSelectedFontFamily(appleColorEmoji);
        QCOMPARE(m_controller.selectedFontFamily(), originalFamily);
        QVERIFY(!m_controller.hasDrafts());
        m_controller.clearSelection();
    }

    QVERIFY(m_controller.selectCandidate(
        0,
        m_boldCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(
        m_controller.availableFontFamilies(),
        ApplicationFonts::availableFontFamilies(true, false));
    m_controller.clearSelection();
}

void PDFEditControllerTest::filtersFontFamiliesByLanguage()
{
    m_controller.discardDrafts();
    m_controller.clearSelection();

    const QString arial = QStringLiteral("Arial");
    const QString vietnameseText = QStringLiteral("Tiếng Việt có dấu");
    const QString chineseText = QStringLiteral("中文");
    QVERIFY(ApplicationFonts::fontFaceSupportsText(
        arial,
        false,
        false,
        vietnameseText));
    QVERIFY(!ApplicationFonts::fontFaceSupportsText(
        arial,
        false,
        false,
        chineseText));

    for (int styleIndex = 0; styleIndex < 4; ++styleIndex) {
        const bool bold = (styleIndex & 2) != 0;
        const bool italic = (styleIndex & 1) != 0;
        const QStringList allFamilies = ApplicationFonts::availableFontFamilies(
            bold,
            italic);
        const QStringList vietnameseFamilies =
            ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::Vietnamese,
                bold,
                italic);
        QVERIFY(vietnameseFamilies.contains(arial, Qt::CaseInsensitive));
        const QStringList englishFamilies =
            ApplicationFonts::fontFamiliesForLanguage(
                ApplicationFonts::FontLanguage::English,
                bold,
                italic);
        QVERIFY(englishFamilies.contains(arial, Qt::CaseInsensitive));

        for (ApplicationFonts::FontLanguage language : {
                 ApplicationFonts::FontLanguage::Vietnamese,
                 ApplicationFonts::FontLanguage::English,
                 ApplicationFonts::FontLanguage::Chinese,
                 ApplicationFonts::FontLanguage::Japanese,
                 ApplicationFonts::FontLanguage::Korean}) {
            const QStringList languageFamilies =
                ApplicationFonts::fontFamiliesForLanguage(
                    language,
                    bold,
                    italic);
            for (const QString &family : languageFamilies) {
                QVERIFY(allFamilies.contains(family, Qt::CaseInsensitive));
            }
        }
    }

    QVERIFY(m_controller.fontFamiliesForFilter(
        PDFEditController::VietnameseFonts).contains(
            arial,
            Qt::CaseInsensitive));
    QVERIFY(m_controller.fontFamiliesForFilter(
        PDFEditController::EnglishFonts).contains(
            arial,
            Qt::CaseInsensitive));
    QVERIFY(!m_controller.fontFamiliesForFilter(
        PDFEditController::ChineseFonts).contains(
            arial,
            Qt::CaseInsensitive));
    QVERIFY(!m_controller.fontFamiliesForFilter(
        PDFEditController::JapaneseFonts).contains(
            arial,
            Qt::CaseInsensitive));
    QVERIFY(!m_controller.fontFamiliesForFilter(
        PDFEditController::KoreanFonts).contains(
            arial,
            Qt::CaseInsensitive));
}

void PDFEditControllerTest::intersectsFontFiltersAcrossMixedStyles()
{
    m_controller.discardDrafts();
    m_controller.clearSelection();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(QStringLiteral("ABCD"));
    m_controller.setSelectedTextRange(1, 2);
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedTextRange(0, 4);

    QStringList expectedEnglish = ApplicationFonts::fontFamiliesForLanguage(
        ApplicationFonts::FontLanguage::English,
        false,
        false);
    const QStringList boldEnglish =
        ApplicationFonts::fontFamiliesForLanguage(
            ApplicationFonts::FontLanguage::English,
            true,
            false);
    expectedEnglish.erase(
        std::remove_if(
            expectedEnglish.begin(),
            expectedEnglish.end(),
            [&boldEnglish](const QString &family) {
                return !boldEnglish.contains(family, Qt::CaseInsensitive);
            }),
        expectedEnglish.end());
    QCOMPARE(
        m_controller.fontFamiliesForFilter(PDFEditController::EnglishFonts),
        expectedEnglish);

    m_controller.cancelSelectedTextEdit();
    m_controller.clearSelection();
    QVERIFY(!m_controller.hasDrafts());

    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const QString originalFamily = m_controller.selectedFontFamily();
    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(1, 0);
    m_controller.setSelectedFontBold(true);
    QCOMPARE(
        m_controller.fontFamiliesForFilter(PDFEditController::EnglishFonts),
        expectedEnglish);

    const QStringList regularEnglish =
        ApplicationFonts::fontFamiliesForLanguage(
            ApplicationFonts::FontLanguage::English,
            false,
            false);
    QString regularOnlyFamily;
    for (const QString &family : regularEnglish) {
        if (!boldEnglish.contains(family, Qt::CaseInsensitive)) {
            regularOnlyFamily = family;
            break;
        }
    }
    if (!regularOnlyFamily.isEmpty()) {
        m_controller.setSelectedFontFamily(regularOnlyFamily);
        QCOMPARE(m_controller.selectedFontFamily(), originalFamily);
        QVERIFY(!m_controller.hasDrafts());
    }

    m_controller.cancelSelectedTextEdit();
    m_controller.clearSelection();
    QVERIFY(!m_controller.hasDrafts());
}

void PDFEditControllerTest::detectsPdfFontStyles()
{
    QCOMPARE(m_regularCandidate.value(QStringLiteral("fontBold")).toBool(), false);
    QCOMPARE(m_boldCandidate.value(QStringLiteral("fontBold")).toBool(), true);
    QCOMPARE(m_uprightCandidate.value(QStringLiteral("fontItalic")).toBool(), false);
    QCOMPARE(m_obliqueCandidate.value(QStringLiteral("fontItalic")).toBool(), true);
    QCOMPARE(m_regularCandidate.value(QStringLiteral("fontUnderline")).toBool(), false);
}

void PDFEditControllerTest::propagatesSourceStylesThroughSelection()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(m_controller.selectedFontBold(), false);

    QVERIFY(m_controller.selectCandidate(
        0,
        m_boldCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(m_controller.selectedFontBold(), true);

    QVERIFY(m_controller.selectCandidate(
        0,
        m_uprightCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(m_controller.selectedFontItalic(), false);

    QVERIFY(m_controller.selectCandidate(
        0,
        m_obliqueCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(m_controller.selectedFontItalic(), true);
    QCOMPARE(m_controller.selectedFontUnderline(), false);
}

void PDFEditControllerTest::groupsCompatibleTextObjectsOnSameLine()
{
    QCOMPARE(m_groupedCandidate.value(QStringLiteral("grouped")).toBool(), true);
    QVERIFY(m_groupedCandidate.value(QStringLiteral("objectId"))
                .toString()
                .startsWith(QStringLiteral("line:")));
    QCOMPARE(
        m_groupedCandidate.value(QStringLiteral("sourceRects")).toList().size(),
        2);

    QVERIFY(m_controller.selectCandidate(
        0,
        m_groupedCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(m_controller.selectedText(), QString::fromLatin1(kGroupedText));
    QCOMPARE(m_controller.selectedSourceRects().size(), 2);
}

void PDFEditControllerTest::seedsGroupedFrameFromQtTextWidth()
{
    const QRectF sourceRect = m_groupedCandidate.value(
        QStringLiteral("sourceRect")).toRectF();
    const QRectF frameRect = m_groupedCandidate.value(
        QStringLiteral("frameRect")).toRectF();
    const QSizeF pageSize = m_document.pageSize(0);
    const qreal displayFontSizeRatio = m_groupedCandidate.value(
        QStringLiteral("displayFontSizeRatio")).toReal();
    const QFont displayFont = m_controller.fontForDisplay(
        m_groupedCandidate.value(QStringLiteral("fontFamily")).toString(),
        m_groupedCandidate.value(QStringLiteral("fontStretch")).toInt(),
        m_groupedCandidate.value(QStringLiteral("fontBold")).toBool(),
        m_groupedCandidate.value(QStringLiteral("fontItalic")).toBool(),
        m_groupedCandidate.value(QStringLiteral("fontUnderline")).toBool(),
        displayFontSizeRatio * pageSize.height());
    const QFontMetricsF metrics(displayFont);
    const QString text = m_groupedCandidate.value(QStringLiteral("text")).toString();
    const qreal requiredWidth = qMax(
        metrics.horizontalAdvance(text),
        metrics.boundingRect(text).width());
    const qreal originX = m_groupedCandidate.value(QStringLiteral("originX")).toReal();
    const qreal availableWidth = (frameRect.right() - originX) * pageSize.width();

    QVERIFY(frameRect.left() <= sourceRect.left());
    QVERIFY(frameRect.right() >= sourceRect.right());
    QVERIFY2(
        availableWidth >= requiredWidth,
        qPrintable(QStringLiteral("Grouped frame width %1 is smaller than Qt text width %2")
                       .arg(availableWidth)
                       .arg(requiredWidth)));
}

void PDFEditControllerTest::expandsTextEditFrameTransiently()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF sourceRect = m_controller.selectedSourceRect();
    const QRectF originalFrameRect = m_controller.selectedFrameRect();
    const QString originalText = m_controller.selectedText();
    m_controller.beginSelectedTextEdit();

    const QRectF editingFrameRect = m_controller.selectedFrameRect();
    QCOMPARE(editingFrameRect.left(), originalFrameRect.left());
    QCOMPARE(editingFrameRect.top(), originalFrameRect.top());
    QCOMPARE(editingFrameRect.height(), originalFrameRect.height());
    QVERIFY(editingFrameRect.right() > originalFrameRect.right());
    QVERIFY(editingFrameRect.right() <= 1.0);
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    QVERIFY(!m_controller.hasDrafts());

    // EditOverlay submits the current plain text when the editor closes.
    m_controller.updateSelectedTextPreview(originalText);
    m_controller.finishSelectedTextEdit();

    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    QVERIFY(!m_controller.hasDrafts());
    QVERIFY(!m_controller.canUndo());
}

void PDFEditControllerTest::stopsExpandedFrameBeforeRightColumn()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_leftColumnCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF originalLeftFrame = m_controller.selectedFrameRect();
    const QRectF rightSourceRect = m_rightColumnCandidate.value(
        QStringLiteral("sourceRect")).toRectF();
    m_controller.beginSelectedTextEdit();
    const QRectF expandedLeftFrame = m_controller.selectedFrameRect();
    QVERIFY(expandedLeftFrame.right() > originalLeftFrame.right());
    QVERIFY(expandedLeftFrame.right() < rightSourceRect.left());
    m_controller.finishSelectedTextEdit();
    QVERIFY(!m_controller.hasDrafts());

    QVERIFY(m_controller.selectCandidate(
        0,
        m_rightColumnCandidate.value(QStringLiteral("objectId")).toString()));
    const QRectF originalRightFrame = m_controller.selectedFrameRect();
    m_controller.beginSelectedTextEdit();
    const QRectF expandedRightFrame = m_controller.selectedFrameRect();
    QVERIFY(expandedRightFrame.right() > originalRightFrame.right());
    QVERIFY(expandedRightFrame.right() <= 1.0);
    m_controller.finishSelectedTextEdit();
    QVERIFY(!m_controller.hasDrafts());
}

void PDFEditControllerTest::keepsExpandedColumnFrameStableAcrossEdits()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_leftColumnCandidate.value(QStringLiteral("objectId")).toString()));

    m_controller.beginSelectedTextEdit();
    const QRectF expandedFrameRect = m_controller.selectedFrameRect();
    const QRectF rightSourceRect = m_rightColumnCandidate.value(
        QStringLiteral("sourceRect")).toRectF();
    QVERIFY(expandedFrameRect.right() < rightSourceRect.left());
    m_controller.updateSelectedTextPreview(QStringLiteral("First left edit"));
    m_controller.finishSelectedTextEdit();
    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);

    m_controller.beginSelectedTextEdit();
    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);
    m_controller.updateSelectedTextPreview(QStringLiteral("Second left edit"));
    m_controller.finishSelectedTextEdit();
    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);

    m_controller.beginSelectedTextEdit();
    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);
    QVERIFY(m_controller.selectedFrameRect().right() < rightSourceRect.left());
    m_controller.finishSelectedTextEdit();
    m_controller.discardDrafts();
}

void PDFEditControllerTest::followsCurrentRightColumnDraftBoundary()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_rightColumnCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF originalRightFrame = m_controller.selectedFrameRect();
    const qreal horizontalShift = qMin<qreal>(
        0.04,
        originalRightFrame.left() / 2.0);
    const QRectF movedRightFrame(
        originalRightFrame.x() - horizontalShift,
        originalRightFrame.y(),
        originalRightFrame.width(),
        originalRightFrame.height());
    m_controller.beginSelectedFrameResize();
    m_controller.updateSelectedFrameRect(movedRightFrame);
    m_controller.finishSelectedFrameResize();
    QCOMPARE(m_controller.selectedFrameRect(), movedRightFrame);

    QVERIFY(m_controller.selectCandidate(
        0,
        m_leftColumnCandidate.value(QStringLiteral("objectId")).toString()));
    m_controller.beginSelectedTextEdit();
    const QRectF blockedFrame = m_controller.selectedFrameRect();
    QVERIFY(blockedFrame.right() < movedRightFrame.left());
    m_controller.cancelSelectedTextEdit();

    QVERIFY(m_controller.selectCandidate(
        0,
        m_rightColumnCandidate.value(QStringLiteral("objectId")).toString()));
    m_controller.commitSelectedText(QString());
    QVERIFY(m_controller.selectCandidate(
        0,
        m_leftColumnCandidate.value(QStringLiteral("objectId")).toString()));
    m_controller.beginSelectedTextEdit();
    QVERIFY(m_controller.selectedFrameRect().right() > blockedFrame.right());
    m_controller.cancelSelectedTextEdit();
    m_controller.discardDrafts();
}

void PDFEditControllerTest::hidesDeletedTextFromInteraction()
{
    m_controller.discardDrafts();
    const QString objectId = m_regularCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QPointF hitPoint = m_regularCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .center();
    const auto containsObject = [&objectId](const QVariantList &candidates) {
        return std::any_of(
            candidates.cbegin(),
            candidates.cend(),
            [&objectId](const QVariant &candidate) {
                return candidate.toMap()
                    .value(QStringLiteral("objectId"))
                    .toString() == objectId;
            });
    };

    QCOMPARE(
        m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(containsObject(waitForCandidates(m_controller, 0)));
    QVERIFY(m_controller.selectCandidate(0, objectId));
    m_controller.commitSelectedText(QString());

    const QVariantList deletedDrafts = m_controller.draftsForPage(0);
    QCOMPARE(deletedDrafts.size(), 1);
    QVERIFY(deletedDrafts.constFirst().toMap()
        .value(QStringLiteral("text"))
        .toString()
        .isEmpty());
    QVERIFY(m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
    QVERIFY(!containsObject(waitForCandidates(m_controller, 0)));
    QVERIFY(!m_controller.selectCandidate(0, objectId));
    QVERIFY(m_controller.selectedObjectId().isEmpty());

    m_controller.undo();
    QCOMPARE(
        m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(containsObject(waitForCandidates(m_controller, 0)));
    QVERIFY(m_controller.selectCandidate(0, objectId));

    m_controller.redo();
    QVERIFY(m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
    QVERIFY(!containsObject(waitForCandidates(m_controller, 0)));
    QVERIFY(m_controller.selectedObjectId().isEmpty());
    m_controller.discardDrafts();
}

void PDFEditControllerTest::persistsExpandedFrameForTextChanges()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF sourceRect = m_controller.selectedSourceRect();
    const QRectF originalFrameRect = m_controller.selectedFrameRect();
    m_controller.beginSelectedTextEdit();
    const QRectF expandedFrameRect = m_controller.selectedFrameRect();
    const QString editedText = QStringLiteral(
        "Regular PDF text edited across the remaining line width");
    m_controller.updateSelectedTextPreview(editedText);
    m_controller.finishSelectedTextEdit();

    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    QCOMPARE(m_controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        m_controller.draftsForPage(0).constFirst().toMap()
            .value(QStringLiteral("frameRect")).toRectF(),
        expandedFrameRect);

    m_controller.undo();
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    m_controller.discardDrafts();
}

void PDFEditControllerTest::persistsExpandedFrameForParagraphAlignment()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF sourceRect = m_controller.selectedSourceRect();
    const QRectF originalFrameRect = m_controller.selectedFrameRect();
    m_controller.beginSelectedTextEdit();
    const QRectF expandedFrameRect = m_controller.selectedFrameRect();
    QVERIFY(expandedFrameRect.right() > originalFrameRect.right());

    m_controller.setSelectedParagraphAlignment(
        PDFEditController::RightAlignment);
    m_controller.finishSelectedTextEdit();

    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    QCOMPARE(
        m_controller.selectedParagraphAlignment(),
        static_cast<int>(PDFEditController::RightAlignment));
    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap draft = drafts.constFirst().toMap();
    QCOMPARE(
        draft.value(QStringLiteral("frameRect")).toRectF(),
        expandedFrameRect);
    QCOMPARE(
        draft.value(QStringLiteral("paragraphFormats"))
            .toList()
            .constFirst()
            .toMap()
            .value(QStringLiteral("alignment"))
            .toString(),
        QStringLiteral("right"));

    m_controller.undo();
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);
    QCOMPARE(
        m_controller.selectedParagraphAlignment(),
        static_cast<int>(PDFEditController::RightAlignment));
    m_controller.discardDrafts();
}

void PDFEditControllerTest::keepsStandaloneResizeFrameUnexpanded()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF originalFrameRect = m_controller.selectedFrameRect();
    m_controller.beginSelectedFrameResize();
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    m_controller.cancelSelectedFrameResize();

    QVERIFY(!m_controller.textEditActive());
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QVERIFY(!m_controller.hasDrafts());

    m_controller.beginSelectedTextEdit();
    const QRectF expandedFrameRect = m_controller.selectedFrameRect();
    const QRectF resizedFrameRect(
        expandedFrameRect.x(),
        expandedFrameRect.y(),
        expandedFrameRect.width() * 0.75,
        expandedFrameRect.height());
    m_controller.beginSelectedFrameResize();
    m_controller.updateSelectedFrameRect(resizedFrameRect);
    QCOMPARE(m_controller.selectedFrameRect(), resizedFrameRect);
    m_controller.cancelSelectedFrameResize();
    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);
    m_controller.finishSelectedTextEdit();

    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QVERIFY(!m_controller.hasDrafts());

    m_controller.beginSelectedTextEdit();
    const QRectF stableExpandedFrameRect = m_controller.selectedFrameRect();
    const QRectF movedFrameRect(
        stableExpandedFrameRect.x(),
        stableExpandedFrameRect.y(),
        stableExpandedFrameRect.width() * 0.75,
        stableExpandedFrameRect.height());
    m_controller.beginSelectedFrameResize();
    m_controller.updateSelectedFrameRect(movedFrameRect);
    m_controller.updateSelectedFrameRect(stableExpandedFrameRect);
    m_controller.finishSelectedFrameResize();
    m_controller.finishSelectedTextEdit();

    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QVERIFY(!m_controller.hasDrafts());
    QVERIFY(!m_controller.canUndo());
}

void PDFEditControllerTest::keepsMixedStylesAsSeparateCandidates()
{
    QCOMPARE(m_mixedRegularCandidate.value(QStringLiteral("grouped")).toBool(), false);
    QCOMPARE(m_mixedBoldCandidate.value(QStringLiteral("grouped")).toBool(), false);
    QCOMPARE(m_mixedRegularCandidate.value(QStringLiteral("fontBold")).toBool(), false);
    QCOMPARE(m_mixedBoldCandidate.value(QStringLiteral("fontBold")).toBool(), true);
}

void PDFEditControllerTest::keepsDistantColumnsAsSeparateCandidates()
{
    QCOMPARE(m_leftColumnCandidate.value(QStringLiteral("grouped")).toBool(), false);
    QCOMPARE(m_rightColumnCandidate.value(QStringLiteral("grouped")).toBool(), false);
}

void PDFEditControllerTest::keepsDifferentFontFacesAsSeparateCandidates()
{
    QCOMPARE(m_uprightCandidate.value(QStringLiteral("grouped")).toBool(), false);
    QCOMPARE(m_obliqueCandidate.value(QStringLiteral("grouped")).toBool(), false);
}

void PDFEditControllerTest::editsGroupedLineAsOneDraft()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_groupedCandidate.value(QStringLiteral("objectId")).toString()));

    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(QStringLiteral("Hello TrePDF"));
    QCOMPARE(m_controller.draftedTextObjectPathsForPage(0).size(), 2);
    QVERIFY(m_controller.allDraftedTextObjectsSuppressibleForPage(0));
    m_controller.finishSelectedTextEdit();

    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    QCOMPARE(
        drafts.constFirst().toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Hello TrePDF"));
    QVERIFY(m_controller.canUndo());

    m_controller.undo();
    QCOMPARE(m_controller.draftsForPage(0).size(), 0);
    QCOMPARE(m_controller.draftedTextObjectPathsForPage(0).size(), 0);

    m_controller.redo();
    QCOMPARE(m_controller.draftsForPage(0).size(), 1);
    QCOMPARE(m_controller.draftedTextObjectPathsForPage(0).size(), 2);

    m_controller.discardDrafts();
    QVERIFY(!m_controller.hasDrafts());
}

void PDFEditControllerTest::resizesDraftFrameWithUndoRedo()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF originalFrameRect = m_controller.selectedFrameRect();
    const qreal originalOriginX = m_controller.selectedOriginX();
    const qreal originalBaselineY = m_controller.selectedBaselineY();
    const qreal horizontalShift = qMin(0.01, originalFrameRect.width() / 4.0);
    const qreal verticalShift = qMin(0.01, originalFrameRect.height() / 4.0);
    const QRectF resizedFrameRect(
        originalFrameRect.x() + horizontalShift,
        originalFrameRect.y() + verticalShift,
        qMin(
            1.0 - originalFrameRect.x() - horizontalShift,
            originalFrameRect.width() + 0.08),
        qMin(
            1.0 - originalFrameRect.y() - verticalShift,
            originalFrameRect.height() + 0.04));
    QVERIFY(resizedFrameRect != originalFrameRect);

    m_controller.beginSelectedFrameResize();
    QVERIFY(m_controller.textEditActive());
    m_controller.updateSelectedFrameRect(resizedFrameRect);
    QCOMPARE(m_controller.selectedFrameRect(), resizedFrameRect);
    QCOMPARE(m_controller.selectedContentBounds(), resizedFrameRect);
    QCOMPARE(m_controller.selectedOriginX(), originalOriginX + horizontalShift);
    QCOMPARE(m_controller.selectedBaselineY(), originalBaselineY + verticalShift);
    m_controller.finishSelectedFrameResize();

    QVERIFY(!m_controller.textEditActive());
    QVERIFY(m_controller.canUndo());
    QCOMPARE(m_controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        m_controller.draftsForPage(0).constFirst().toMap()
            .value(QStringLiteral("frameRect")).toRectF(),
        resizedFrameRect);

    m_controller.undo();
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QCOMPARE(m_controller.selectedFrameRect(), resizedFrameRect);
    QCOMPARE(m_controller.selectedContentBounds(), resizedFrameRect);

    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    m_controller.beginSelectedFrameResize();
    m_controller.updateSelectedFrameRect(resizedFrameRect);
    m_controller.cancelSelectedFrameResize();
    QVERIFY(!m_controller.textEditActive());
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    const QString editedText = QStringLiteral("Text and geometry in one edit");
    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(editedText);
    m_controller.beginSelectedFrameResize();
    m_controller.updateSelectedFrameRect(resizedFrameRect);
    m_controller.finishSelectedFrameResize();
    QVERIFY(m_controller.textEditActive());
    QCOMPARE(m_controller.selectedText(), editedText);
    QCOMPARE(m_controller.selectedFrameRect(), resizedFrameRect);

    m_controller.cancelSelectedTextEdit();
    QCOMPARE(
        m_controller.selectedText(),
        m_regularCandidate.value(QStringLiteral("text")).toString());
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
}

void PDFEditControllerTest::storesReflowedContentBoundsWithUndoRedo()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF originalFrameRect = m_controller.selectedFrameRect();
    m_controller.beginSelectedTextEdit();
    const QRectF frameRect = m_controller.selectedFrameRect();
    const QRectF contentBounds(
        frameRect.x(),
        frameRect.y(),
        frameRect.width(),
        qMin(1.0 - frameRect.y(), frameRect.height() + 0.05));
    const QString multilineText = QStringLiteral("First line\nSecond line");

    m_controller.updateSelectedTextPreview(multilineText);
    m_controller.updateSelectedContentBounds(contentBounds);
    QCOMPARE(m_controller.selectedFrameRect(), frameRect);
    QCOMPARE(m_controller.selectedContentBounds(), contentBounds);
    m_controller.finishSelectedTextEdit();

    QCOMPARE(m_controller.draftsForPage(0).size(), 1);
    m_controller.undo();
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QCOMPARE(m_controller.selectedContentBounds(), originalFrameRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QCOMPARE(m_controller.selectedFrameRect(), frameRect);
    QCOMPARE(m_controller.selectedContentBounds(), contentBounds);
    QCOMPARE(m_controller.selectedText(), multilineText);

    m_controller.discardDrafts();
}

void PDFEditControllerTest::changesFontFamilyForCurrentEdit()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QString originalFamily = m_controller.selectedFontFamily();
    QString replacementFamily;
    for (const QString &family : m_controller.availableFontFamilies()) {
        if (family.compare(originalFamily, Qt::CaseInsensitive) != 0) {
            replacementFamily = family;
            break;
        }
    }
    QVERIFY(!replacementFamily.isEmpty());

    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedFontFamily(replacementFamily);
    m_controller.updateSelectedTextPreview(QStringLiteral("Restyled PDF text"));
    QVERIFY(m_controller.textEditActive());
    QCOMPARE(m_controller.selectedFontFamily(), replacementFamily);

    QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    QCOMPARE(
        drafts.constFirst().toMap().value(QStringLiteral("fontFamily")).toString(),
        replacementFamily);
    QCOMPARE(
        drafts.constFirst().toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Restyled PDF text"));

    m_controller.finishSelectedTextEdit();
    QVERIFY(!m_controller.textEditActive());
    QVERIFY(m_controller.canUndo());

    m_controller.undo();
    QCOMPARE(m_controller.selectedFontFamily(), originalFamily);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QCOMPARE(m_controller.selectedFontFamily(), replacementFamily);
    drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    QCOMPARE(
        drafts.constFirst().toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Restyled PDF text"));
    QCOMPARE(
        drafts.constFirst().toMap().value(QStringLiteral("fontFamily")).toString(),
        replacementFamily);

    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedFontFamily(replacementFamily);
    m_controller.cancelSelectedTextEdit();
    QCOMPARE(m_controller.selectedFontFamily(), originalFamily);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.setSelectedFontFamily(replacementFamily);
    QVERIFY(!m_controller.textEditActive());
    QCOMPARE(m_controller.selectedFontFamily(), replacementFamily);
    QVERIFY(m_controller.canUndo());

    m_controller.undo();
    QCOMPARE(m_controller.selectedFontFamily(), originalFamily);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QCOMPARE(m_controller.selectedFontFamily(), replacementFamily);
    drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    QCOMPARE(
        drafts.constFirst().toMap().value(QStringLiteral("fontFamily")).toString(),
        replacementFamily);

    m_controller.discardDrafts();
}

void PDFEditControllerTest::resetsStretchWhenChangingFontFamily()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("stretched-font-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("stretched-font-output.pdf"));
    QVERIFY(createStretchedTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString text = QString::fromLatin1(kStretchedText);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        text);
    QVERIFY(!candidate.isEmpty());
    QCOMPARE(
        candidate.value(QStringLiteral("fontStretch")).toInt(),
        static_cast<int>(QFont::UltraExpanded));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));

    const QString originalFamily = controller.selectedFontFamily();
    const QString replacementFamily = QStringLiteral("Arial");
    QVERIFY(ApplicationFonts::bundledFontFamilies().contains(
        replacementFamily,
        Qt::CaseInsensitive));
    QVERIFY(originalFamily.compare(
        replacementFamily,
        Qt::CaseInsensitive) != 0);

    controller.beginSelectedTextEdit();
    controller.setSelectedTextRange(0, 4);
    controller.setSelectedFontFamily(replacementFamily);
    QVariantList drafts = controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    QVariantList spans = drafts.constFirst().toMap()
        .value(QStringLiteral("characterSpans"))
        .toList();
    QCOMPARE(spans.size(), 2);
    QCOMPARE(
        spans.at(0).toMap().value(QStringLiteral("fontFamily")).toString(),
        replacementFamily);
    QCOMPARE(
        spans.at(0).toMap().value(QStringLiteral("fontStretch")).toInt(),
        static_cast<int>(QFont::Unstretched));
    QCOMPARE(
        spans.at(1).toMap().value(QStringLiteral("fontFamily")).toString(),
        originalFamily);
    QCOMPARE(
        spans.at(1).toMap().value(QStringLiteral("fontStretch")).toInt(),
        static_cast<int>(QFont::UltraExpanded));
    controller.cancelSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());

    controller.beginSelectedTextEdit();
    controller.setSelectedTextRange(0, text.size());
    controller.setSelectedFontFamily(replacementFamily);
    controller.finishSelectedTextEdit();
    QCOMPARE(
        controller.selectedFontStretch(),
        static_cast<int>(QFont::Unstretched));
    drafts = controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    QCOMPARE(
        drafts.constFirst().toMap().value(QStringLiteral("fontStretch")).toInt(),
        static_cast<int>(QFont::Unstretched));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QVariantMap savedCandidate = candidateForText(
        waitForCandidates(controller, 0),
        text);
    QVERIFY(!savedCandidate.isEmpty());
    const int savedStretch = savedCandidate
        .value(QStringLiteral("fontStretch"))
        .toInt();
    QVERIFY2(
        qAbs(savedStretch - static_cast<int>(QFont::Unstretched)) <= 10,
        qPrintable(QStringLiteral("Expected changed font stretch near 100, got %1")
                       .arg(savedStretch)));
    FS_MATRIX matrix{};
    QVERIFY(textObjectMatrix(outputPath, text, matrix));
    const qreal savedScale = horizontalScale(matrix);
    QVERIFY2(
        savedScale >= 0.9 && savedScale <= 1.1,
        qPrintable(QStringLiteral("Expected changed font scale near 1, got %1")
                       .arg(savedScale, 0, 'f', 6)));
}

void PDFEditControllerTest::normalizesSourceStretchWhenReselectingFontFamily()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("reselect-stretched-font-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("reselect-stretched-font-output.pdf"));
    QVERIFY(createStretchedTextPdf(sourcePath, 4.45));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString text = QString::fromLatin1(kStretchedText);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        text);
    QVERIFY(!candidate.isEmpty());
    QCOMPARE(
        candidate.value(QStringLiteral("fontStretch")).toInt(),
        static_cast<int>(QFont::UltraExpanded));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));

    const QString fontFamily = controller.selectedFontFamily();
    if (!ApplicationFonts::isFontFaceSelectable(fontFamily, false, false)) {
        QSKIP(qPrintable(QStringLiteral("Source font is not selectable: %1")
                             .arg(fontFamily)));
    }
    controller.setSelectedFontFamily(fontFamily);
    QCOMPARE(
        controller.selectedFontStretch(),
        static_cast<int>(QFont::Unstretched));
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    FS_MATRIX matrix{};
    QVERIFY(textObjectMatrix(outputPath, text, matrix));
    const qreal savedScale = horizontalScale(matrix);
    QVERIFY2(
        savedScale >= 0.9 && savedScale <= 1.1,
        qPrintable(QStringLiteral("Expected normalized scale near 1, got %1")
                       .arg(savedScale, 0, 'f', 6)));
}

void PDFEditControllerTest::preservesSourceHorizontalScaleWhenResizing_data()
{
    QTest::addColumn<qreal>("expectedScale");

    QTest::newRow("condensed") << 0.5;
    QTest::newRow("expanded") << 2.0;
}

void PDFEditControllerTest::preservesSourceHorizontalScaleWhenResizing()
{
    QFETCH(qreal, expectedScale);

    const QString dataTag = QString::fromLatin1(QTest::currentDataTag());
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("preserve-source-scale-%1-source.pdf").arg(dataTag));
    QVERIFY(createStretchedTextPdf(sourcePath, expectedScale));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString text = QString::fromLatin1(kStretchedText);

    for (int round = 1; round <= 2; ++round) {
        const QVariantMap candidate = candidateForText(
            waitForCandidates(controller, 0),
            text);
        QVERIFY(!candidate.isEmpty());
        QVERIFY(controller.selectCandidate(
            0,
            candidate.value(QStringLiteral("objectId")).toString()));

        QRectF frame = controller.selectedFrameRect();
        qreal targetRight = 0.85;
        if (qAbs(frame.right() - targetRight) < 0.001) {
            targetRight = 0.80;
        }
        frame.setRight(targetRight);
        QVERIFY(frame.width() > 0.0);
        controller.beginSelectedFrameResize();
        controller.updateSelectedFrameRect(frame);
        controller.finishSelectedFrameResize();
        QVERIFY(controller.hasDrafts());

        const QString outputPath = m_temporaryDirectory.filePath(
            QStringLiteral("preserve-source-scale-%1-round-%2.pdf")
                .arg(dataTag)
                .arg(round));
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

        FS_MATRIX matrix{};
        QVERIFY(textObjectMatrix(outputPath, text, matrix));
        const qreal savedScale = horizontalScale(matrix);
        QVERIFY2(
            qAbs(savedScale - expectedScale) <= 0.01,
            qPrintable(QStringLiteral(
                "Round %1 expected source scale %2, got %3")
                           .arg(round)
                           .arg(expectedScale, 0, 'f', 6)
                           .arg(savedScale, 0, 'f', 6)));
    }
}

void PDFEditControllerTest::preservesSourceScaleForUnchangedRichTextRuns()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-stretch-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-stretch-output.pdf"));
    QVERIFY(createStretchedTextPdf(sourcePath, 2.0));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString text = QString::fromLatin1(kStretchedText);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        text);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));

    controller.beginSelectedTextEdit();
    controller.setSelectedTextRange(1, 3);
    controller.setSelectedFontFamily(QStringLiteral("Arial"));
    controller.finishSelectedTextEdit();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    FS_MATRIX prefixMatrix{};
    FS_MATRIX changedMatrix{};
    FS_MATRIX suffixMatrix{};
    QVERIFY(textObjectMatrix(outputPath, QStringLiteral("S"), prefixMatrix));
    QVERIFY(textObjectMatrix(outputPath, QStringLiteral("cal"), changedMatrix));
    QVERIFY(textObjectMatrix(outputPath, QStringLiteral("e"), suffixMatrix));
    const qreal prefixScale = horizontalScale(prefixMatrix);
    const qreal changedScale = horizontalScale(changedMatrix);
    const qreal suffixScale = horizontalScale(suffixMatrix);
    QVERIFY2(
        qAbs(prefixScale - 2.0) <= 0.01,
        qPrintable(QStringLiteral("Prefix source scale changed to %1")
                       .arg(prefixScale, 0, 'f', 6)));
    QVERIFY2(
        changedScale >= 0.85 && changedScale <= 1.20,
        qPrintable(QStringLiteral("Changed Arial scale is %1")
                       .arg(changedScale, 0, 'f', 6)));
    QVERIFY2(
        qAbs(suffixScale - 2.0) <= 0.01,
        qPrintable(QStringLiteral("Suffix source scale changed to %1")
                       .arg(suffixScale, 0, 'f', 6)));
}

void PDFEditControllerTest::keepsAddedDidotScaleStableAcrossRepeatedSaves()
{
    const QString fontFamily = QStringLiteral("Didot");
    if (!ApplicationFonts::systemFontFamilies().contains(
            fontFamily,
            Qt::CaseInsensitive)
        || !ApplicationFonts::isFontFaceSelectable(fontFamily, false, false)) {
        QSKIP("Didot is not available as a save-compatible system font");
    }
    const ApplicationFonts::FontFaceData face =
        ApplicationFonts::systemFontFace(fontFamily, false, false);
    if (!face.isValid()) {
        QSKIP("Didot font data is unavailable");
    }

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("didot-scale-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(!controller.addText(0, 0.2, 0.65).isEmpty());
    const QString text = QStringLiteral("Didot scale stable");
    controller.updateSelectedTextPreview(text);
    controller.setSelectedTextRange(0, text.size());
    controller.setSelectedFontFamily(fontFamily);
    QCOMPARE(controller.selectedFontFamily(), fontFamily);
    controller.finishSelectedTextEdit();

    qreal baselineScale = 0.0;
    QString finalOutputPath;
    for (int round = 1; round <= 5; ++round) {
        if (round > 1) {
            const QVariantMap candidate = candidateForText(
                waitForCandidates(controller, 0),
                text);
            QVERIFY(!candidate.isEmpty());
            QVERIFY(controller.selectCandidate(
                0,
                candidate.value(QStringLiteral("objectId")).toString()));
        }

        QRectF frame = controller.selectedFrameRect();
        qreal targetRight = 0.85;
        if (qAbs(frame.right() - targetRight) < 0.001) {
            targetRight = 0.80;
        }
        frame.setRight(targetRight);
        QVERIFY(frame.width() > 0.0);
        controller.beginSelectedFrameResize();
        controller.updateSelectedFrameRect(frame);
        controller.finishSelectedFrameResize();
        QVERIFY(controller.hasDrafts());

        finalOutputPath = m_temporaryDirectory.filePath(
            QStringLiteral("didot-scale-round-%1.pdf").arg(round));
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(finalOutputPath)));

        FS_MATRIX matrix{};
        QVERIFY(textObjectMatrix(finalOutputPath, text, matrix));
        const qreal savedScale = horizontalScale(matrix);
        const QVariantMap savedCandidate = candidateForText(
            waitForCandidates(controller, 0),
            text);
        QVERIFY(!savedCandidate.isEmpty());
        const QString savedFamily = savedCandidate
            .value(QStringLiteral("fontFamily"))
            .toString();
        const int savedStretch = savedCandidate
            .value(QStringLiteral("fontStretch"))
            .toInt();
        const bool savedBold = savedCandidate
            .value(QStringLiteral("fontBold"))
            .toBool();
        const bool savedItalic = savedCandidate
            .value(QStringLiteral("fontItalic"))
            .toBool();
        const qreal previewPixelSize = savedCandidate
            .value(QStringLiteral("displayFontSizeRatio"))
            .toReal() * document.pageSize(0).height();
        const QFont normalPreviewFont = controller.fontForDisplay(
            savedFamily,
            QFont::Unstretched,
            savedBold,
            savedItalic,
            false,
            previewPixelSize);
        const QFont scaledPreviewFont = controller.fontForDisplay(
            savedFamily,
            savedStretch,
            savedBold,
            savedItalic,
            false,
            previewPixelSize);
        const qreal normalPreviewWidth = QFontMetricsF(normalPreviewFont)
            .horizontalAdvance(text);
        const qreal scaledPreviewWidth = QFontMetricsF(scaledPreviewFont)
            .horizontalAdvance(text);
        QVERIFY(normalPreviewWidth > 0.0);
        const qreal previewScale = scaledPreviewWidth / normalPreviewWidth;
        QVERIFY2(
            qAbs(previewScale - savedScale) <= 0.02,
            qPrintable(QStringLiteral(
                "Round %1 PDF scale %2 does not match preview scale %3 "
                "for stretch %4")
                           .arg(round)
                           .arg(savedScale, 0, 'f', 6)
                           .arg(previewScale, 0, 'f', 6)
                           .arg(savedStretch)));
        if (round == 1) {
            baselineScale = savedScale;
            QVERIFY2(
                baselineScale >= 0.85 && baselineScale <= 1.20,
                qPrintable(QStringLiteral(
                    "Unexpected first-save Didot scale: %1")
                               .arg(baselineScale, 0, 'f', 6)));
        } else {
            QVERIFY2(
                qAbs(savedScale - baselineScale) <= 0.01,
                qPrintable(QStringLiteral(
                    "Didot scale drifted at round %1: baseline %2, got %3")
                               .arg(round)
                               .arg(baselineScale, 0, 'f', 6)
                               .arg(savedScale, 0, 'f', 6)));
        }
    }

    const QString verificationPath = qEnvironmentVariable(
        "TREPDF_DIDOT_SCALE_TEST_OUTPUT");
    if (!verificationPath.isEmpty()) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(finalOutputPath, verificationPath));
    }
}

void PDFEditControllerTest::calibratesDisplayStretchToPdfScale_data()
{
    QTest::addColumn<QString>("fontFamily");
    QTest::addColumn<int>("targetStretch");
    QTest::addColumn<int>("pixelSize");

    QTest::newRow("bundled-arial-condensed")
        << QStringLiteral("Arial") << 89 << 16;
    QTest::newRow("bundled-arial-expanded")
        << QStringLiteral("Arial") << 125 << 64;
    if (ApplicationFonts::systemFontFamilies().contains(
            QStringLiteral("Didot"),
            Qt::CaseInsensitive)
        && ApplicationFonts::isFontFaceSelectable(
            QStringLiteral("Didot"),
            false,
            false)) {
        QTest::newRow("system-didot-condensed")
            << QStringLiteral("Didot") << 89 << 16;
    }
}

void PDFEditControllerTest::calibratesDisplayStretchToPdfScale()
{
    QFETCH(QString, fontFamily);
    QFETCH(int, targetStretch);
    QFETCH(int, pixelSize);

    const QString probe = QStringLiteral(
        "Display width AVATAR To Wa 0123456789");
    const QFont normalFont = m_controller.fontForDisplay(
        fontFamily,
        QFont::Unstretched,
        false,
        false,
        false,
        pixelSize);
    const QFont calibratedFont = m_controller.fontForDisplay(
        fontFamily,
        targetStretch,
        false,
        false,
        false,
        pixelSize);
    const qreal normalWidth = QFontMetricsF(normalFont).horizontalAdvance(probe);
    const qreal calibratedWidth = QFontMetricsF(calibratedFont)
        .horizontalAdvance(probe);
    QVERIFY(normalWidth > 0.0);
    const qreal actualRatio = calibratedWidth / normalWidth;
    const qreal targetRatio = targetStretch
        / static_cast<qreal>(QFont::Unstretched);
    QVERIFY2(
        qAbs(actualRatio - targetRatio) <= 0.02,
        qPrintable(QStringLiteral(
            "%1 target stretch %2 produced QFont stretch %3 and ratio %4")
                       .arg(fontFamily)
                       .arg(targetStretch)
                       .arg(calibratedFont.stretch())
                       .arg(actualRatio, 0, 'f', 6)));
}

void PDFEditControllerTest::appliesFontStylesToTypingAtCaret()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const QString originalText = QString::fromLatin1(kRegularText);
    const qsizetype caretPosition = 8;
    const QString styledInsertion = QStringLiteral("new");
    const QString regularInsertion = QStringLiteral(" plain");
    QSignalSpy typingFormatAppliedSpy(
        &m_controller,
        &PDFEditController::typingFormatApplied);
    QVERIFY(!m_controller.selectedFontBold());
    QVERIFY(!m_controller.selectedFontItalic());
    QVERIFY(!m_controller.selectedFontUnderline());

    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(caretPosition, 0);
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedFontItalic(true);
    m_controller.setSelectedFontUnderline(true);
    QVERIFY(m_controller.textEditActive());
    QVERIFY(m_controller.selectedFontBold());
    QVERIFY(m_controller.selectedFontItalic());
    QVERIFY(m_controller.selectedFontUnderline());
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
    QVERIFY(!m_controller.canUndo());

    const QColor typingColor(QStringLiteral("#1565c0"));
    m_controller.setSelectedColor(typingColor);
    QCOMPARE(m_controller.selectedColor(), typingColor);

    const QString styledText = originalText.left(caretPosition)
        + styledInsertion
        + originalText.mid(caretPosition);
    const qsizetype styledCaret = caretPosition + styledInsertion.size();
    m_controller.updateSelectedTextPreview(styledText, styledCaret);
    QCOMPARE(typingFormatAppliedSpy.size(), 1);
    m_controller.setSelectedTextRange(styledCaret, 0);

    QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap styledDraft = drafts.constFirst().toMap();
    QCOMPARE(styledDraft.value(QStringLiteral("text")).toString(), styledText);
    const QVariantList styledSpans = styledDraft.value(
        QStringLiteral("characterSpans")).toList();
    QCOMPARE(styledSpans.size(), 3);
    const QVariantMap styledSpan = styledSpans.at(1).toMap();
    QCOMPARE(styledSpan.value(QStringLiteral("start")).toLongLong(), caretPosition);
    QCOMPARE(
        styledSpan.value(QStringLiteral("length")).toLongLong(),
        styledInsertion.size());
    QVERIFY(styledSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(styledSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(styledSpan.value(QStringLiteral("fontUnderline")).toBool());
    QCOMPARE(
        styledSpan.value(QStringLiteral("color")).value<QColor>(),
        typingColor);
    QVERIFY(m_controller.selectedFontBold());
    QVERIFY(m_controller.selectedFontItalic());
    QVERIFY(m_controller.selectedFontUnderline());

    m_controller.setSelectedFontBold(false);
    m_controller.setSelectedFontItalic(false);
    m_controller.setSelectedFontUnderline(false);
    QVERIFY(!m_controller.selectedFontBold());
    QVERIFY(!m_controller.selectedFontItalic());
    QVERIFY(!m_controller.selectedFontUnderline());

    const QString finalText = styledText.left(styledCaret)
        + regularInsertion
        + styledText.mid(styledCaret);
    m_controller.updateSelectedTextPreview(
        finalText,
        styledCaret + regularInsertion.size());
    QCOMPARE(typingFormatAppliedSpy.size(), 2);
    m_controller.setSelectedTextRange(
        styledCaret + regularInsertion.size(),
        0);

    drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap finalDraft = drafts.constFirst().toMap();
    QCOMPARE(finalDraft.value(QStringLiteral("text")).toString(), finalText);
    const QVariantList finalSpans = finalDraft.value(
        QStringLiteral("characterSpans")).toList();
    QCOMPARE(finalSpans.size(), 3);
    const QVariantMap prefixSpan = finalSpans.at(0).toMap();
    const QVariantMap finalStyledSpan = finalSpans.at(1).toMap();
    const QVariantMap suffixSpan = finalSpans.at(2).toMap();
    QCOMPARE(prefixSpan.value(QStringLiteral("length")).toLongLong(), caretPosition);
    QVERIFY(!prefixSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!prefixSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(!prefixSpan.value(QStringLiteral("fontUnderline")).toBool());
    QVERIFY(finalStyledSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(finalStyledSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(finalStyledSpan.value(QStringLiteral("fontUnderline")).toBool());
    QCOMPARE(
        suffixSpan.value(QStringLiteral("start")).toLongLong(),
        styledCaret);
    QVERIFY(!suffixSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!suffixSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(!suffixSpan.value(QStringLiteral("fontUnderline")).toBool());

    m_controller.finishSelectedTextEdit();
    QVERIFY(m_controller.canUndo());
    m_controller.undo();
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
    m_controller.redo();
    QCOMPARE(
        m_controller.draftsForPage(0).constFirst().toMap()
            .value(QStringLiteral("characterSpans")).toList().size(),
        3);

    m_controller.discardDrafts();
}

void PDFEditControllerTest::appliesTypingFormatBetweenRepeatedCharacters()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(QStringLiteral("aaa"));
    m_controller.setSelectedTextRange(1, 0);
    m_controller.setSelectedFontBold(true);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral(
            "import QtQuick\n"
            "TextEdit { textFormat: TextEdit.RichText; text: \"aaa\" }"),
        QUrl());
    std::unique_ptr<QObject> textEdit(component.create());
    QVERIFY2(textEdit, qPrintable(component.errorString()));
    auto *quickDocument = textEdit->property("textDocument")
                              .value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *textDocument = quickDocument->textDocument();
    QVERIFY(textDocument);
    textDocument->clearUndoRedoStacks();
    QVERIFY(QMetaObject::invokeMethod(
        textEdit.get(),
        "insert",
        Q_ARG(int, 1),
        Q_ARG(QString, QStringLiteral("a"))));

    m_controller.updateSelectedTextPreview(QStringLiteral("aaaa"), 2);
    m_controller.applyTypingFormatToEditorDocument(quickDocument, 1, 1);
    m_controller.setSelectedTextRange(2, 0);

    QTextCursor formattedCursor(textDocument);
    formattedCursor.setPosition(1);
    formattedCursor.setPosition(2, QTextCursor::KeepAnchor);
    QCOMPARE(formattedCursor.charFormat().fontWeight(), QFont::Bold);
    textDocument->undo();
    QCOMPARE(textDocument->toPlainText(), QStringLiteral("aaa"));

    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap draft = drafts.constFirst().toMap();
    QCOMPARE(draft.value(QStringLiteral("text")).toString(), QStringLiteral("aaaa"));
    const QVariantList spans = draft.value(
        QStringLiteral("characterSpans")).toList();
    QCOMPARE(spans.size(), 3);
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("start")).toLongLong(), 0);
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("length")).toLongLong(), 1);
    QVERIFY(!spans.at(0).toMap().value(QStringLiteral("fontBold")).toBool());
    QCOMPARE(spans.at(1).toMap().value(QStringLiteral("start")).toLongLong(), 1);
    QCOMPARE(spans.at(1).toMap().value(QStringLiteral("length")).toLongLong(), 1);
    QVERIFY(spans.at(1).toMap().value(QStringLiteral("fontBold")).toBool());
    QCOMPARE(spans.at(2).toMap().value(QStringLiteral("start")).toLongLong(), 2);
    QCOMPARE(spans.at(2).toMap().value(QStringLiteral("length")).toLongLong(), 2);
    QVERIFY(!spans.at(2).toMap().value(QStringLiteral("fontBold")).toBool());

    m_controller.cancelSelectedTextEdit();
    m_controller.discardDrafts();
}

void PDFEditControllerTest::preservesEditorUndoWhenFormattingSelection()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QString editedText = QStringLiteral("ABCDX");
    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(editedText);
    m_controller.setSelectedTextRange(1, 2);
    QSignalSpy selectionStyleSpy(
        &m_controller,
        &PDFEditController::selectionFontStyleApplied);
    m_controller.setSelectedFontBold(true);
    QCOMPARE(selectionStyleSpy.size(), 1);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral(
            "import QtQuick\n"
            "TextEdit { textFormat: TextEdit.RichText; text: \"ABCD\" }"),
        QUrl());
    std::unique_ptr<QObject> textEdit(component.create());
    QVERIFY2(textEdit, qPrintable(component.errorString()));
    auto *quickDocument = textEdit->property("textDocument")
                              .value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *textDocument = quickDocument->textDocument();
    QVERIFY(textDocument);
    textDocument->clearUndoRedoStacks();
    QSignalSpy textChangedSpy(textEdit.get(), SIGNAL(textChanged()));
    QVERIFY(QMetaObject::invokeMethod(
        textEdit.get(),
        "insert",
        Q_ARG(int, 4),
        Q_ARG(QString, QStringLiteral("X"))));
    QCOMPARE(textDocument->toPlainText(), editedText);
    const int signalsAfterInsertion = textChangedSpy.size();
    QVERIFY(signalsAfterInsertion > 0);

    const QList<QVariant> styleArguments = selectionStyleSpy.takeFirst();
    QVERIFY(m_controller.applyFontStyleToEditorDocument(
        quickDocument,
        styleArguments.at(0).toInt(),
        styleArguments.at(1).toInt(),
        styleArguments.at(2).toInt(),
        styleArguments.at(3).toBool()));
    QVERIFY(textChangedSpy.size() > signalsAfterInsertion);

    QTextCursor formattedCursor(textDocument);
    formattedCursor.setPosition(1);
    formattedCursor.setPosition(3, QTextCursor::KeepAnchor);
    QCOMPARE(formattedCursor.charFormat().fontWeight(), QFont::Bold);

    const int signalsAfterFormatting = textChangedSpy.size();
    textDocument->undo();
    QCOMPARE(textDocument->toPlainText(), editedText);
    QVERIFY(textChangedSpy.size() > signalsAfterFormatting);
    formattedCursor.setPosition(1);
    formattedCursor.setPosition(3, QTextCursor::KeepAnchor);
    QVERIFY(formattedCursor.charFormat().fontWeight() < QFont::Bold);
    QVERIFY(m_controller.syncEditorFontStyles(quickDocument));

    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantList spans = drafts.constFirst().toMap()
                                   .value(QStringLiteral("characterSpans"))
                                   .toList();
    QCOMPARE(spans.size(), 1);
    QVERIFY(!spans.constFirst().toMap()
                 .value(QStringLiteral("fontBold"))
                 .toBool());

    textDocument->undo();
    QCOMPARE(textDocument->toPlainText(), QStringLiteral("ABCD"));

    m_controller.cancelSelectedTextEdit();
    m_controller.discardDrafts();
}

void PDFEditControllerTest::formatsSelectedRangeInMultilineDraft()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QString text = QStringLiteral("Red blue\nGreen");
    const QColor rangeColor(QStringLiteral("#c62828"));
    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(text);
    m_controller.setSelectedTextRange(4, 4);
    m_controller.setSelectedColor(rangeColor);
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedFontItalic(true);
    m_controller.setSelectedFontUnderline(true);

    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap draft = drafts.constFirst().toMap();
    QCOMPARE(draft.value(QStringLiteral("text")).toString(), text);

    const QVariantList spans = draft.value(QStringLiteral("characterSpans")).toList();
    QCOMPARE(spans.size(), 3);
    const QVariantMap firstSpan = spans.at(0).toMap();
    QCOMPARE(firstSpan.value(QStringLiteral("start")).toLongLong(), 0);
    QCOMPARE(firstSpan.value(QStringLiteral("length")).toLongLong(), 4);
    QVERIFY(!firstSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!firstSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(!firstSpan.value(QStringLiteral("fontUnderline")).toBool());
    QVERIFY(firstSpan.value(QStringLiteral("color")).value<QColor>() != rangeColor);
    const QVariantMap selectedSpan = spans.at(1).toMap();
    QCOMPARE(selectedSpan.value(QStringLiteral("start")).toLongLong(), 4);
    QCOMPARE(selectedSpan.value(QStringLiteral("length")).toLongLong(), 4);
    QVERIFY(selectedSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(selectedSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(selectedSpan.value(QStringLiteral("fontUnderline")).toBool());
    QCOMPARE(selectedSpan.value(QStringLiteral("color")).value<QColor>(), rangeColor);
    const QVariantMap remainingSpan = spans.at(2).toMap();
    QCOMPARE(remainingSpan.value(QStringLiteral("start")).toLongLong(), 8);
    QCOMPARE(
        remainingSpan.value(QStringLiteral("length")).toLongLong(),
        text.size() - 8);
    QVERIFY(!remainingSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!remainingSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(!remainingSpan.value(QStringLiteral("fontUnderline")).toBool());
    QVERIFY(remainingSpan.value(QStringLiteral("color")).value<QColor>() != rangeColor);

    const QVariantList paragraphs = draft.value(
        QStringLiteral("paragraphFormats")).toList();
    QCOMPARE(paragraphs.size(), 2);
    QCOMPARE(paragraphs.at(0).toMap().value(QStringLiteral("start")).toLongLong(), 0);
    QCOMPARE(paragraphs.at(0).toMap().value(QStringLiteral("length")).toLongLong(), 8);
    QCOMPARE(paragraphs.at(1).toMap().value(QStringLiteral("start")).toLongLong(), 9);
    QCOMPARE(paragraphs.at(1).toMap().value(QStringLiteral("length")).toLongLong(), 5);

    m_controller.finishSelectedTextEdit();
    m_controller.undo();
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
    m_controller.redo();
    QCOMPARE(
        m_controller.draftsForPage(0).constFirst().toMap()
            .value(QStringLiteral("characterSpans")).toList().size(),
        3);

    m_controller.discardDrafts();
}

void PDFEditControllerTest::alignsSelectedParagraphsWithUndoRedo()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QString text = QStringLiteral("First\nSecond\nThird");
    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(text);
    m_controller.setSelectedTextRange(7, 0);
    QCOMPARE(
        m_controller.selectedParagraphAlignment(),
        static_cast<int>(PDFEditController::LeftAlignment));

    m_controller.setSelectedParagraphAlignment(
        PDFEditController::CenterAlignment);
    QCOMPARE(
        m_controller.selectedParagraphAlignment(),
        static_cast<int>(PDFEditController::CenterAlignment));

    QVariantList paragraphs = m_controller.draftsForPage(0)
                                  .constFirst()
                                  .toMap()
                                  .value(QStringLiteral("paragraphFormats"))
                                  .toList();
    QCOMPARE(paragraphs.size(), 3);
    QCOMPARE(
        paragraphs.at(0).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("left"));
    QCOMPARE(
        paragraphs.at(1).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("center"));
    QCOMPARE(
        paragraphs.at(2).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("left"));

    m_controller.updateSelectedTextPreview(
        QStringLiteral("First\nSecond!\nThird"));
    paragraphs = m_controller.draftsForPage(0)
                     .constFirst()
                     .toMap()
                     .value(QStringLiteral("paragraphFormats"))
                     .toList();
    QCOMPARE(
        paragraphs.at(1).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("center"));

    m_controller.setSelectedTextRange(0, text.size());
    QCOMPARE(m_controller.selectedParagraphAlignment(), -1);
    m_controller.finishSelectedTextEdit();

    QVERIFY(m_controller.canUndo());
    m_controller.undo();
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
    m_controller.redo();
    paragraphs = m_controller.draftsForPage(0)
                     .constFirst()
                     .toMap()
                     .value(QStringLiteral("paragraphFormats"))
                     .toList();
    QCOMPARE(
        paragraphs.at(1).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("center"));

    m_controller.discardDrafts();
}

void PDFEditControllerTest::appliesFontStylesToDisplayFont_data()
{
    QTest::addColumn<bool>("fontBold");
    QTest::addColumn<bool>("fontItalic");
    QTest::addColumn<bool>("fontUnderline");

    QTest::newRow("regular") << false << false << false;
    QTest::newRow("bold") << true << false << false;
    QTest::newRow("italic") << false << true << false;
    QTest::newRow("underline") << false << false << true;
    QTest::newRow("combined") << true << true << true;
}

void PDFEditControllerTest::appliesFontStylesToDisplayFont()
{
    QFETCH(bool, fontBold);
    QFETCH(bool, fontItalic);
    QFETCH(bool, fontUnderline);

    const QFont displayFont = m_controller.fontForDisplay(
        QString(),
        QFont::Unstretched,
        fontBold,
        fontItalic,
        fontUnderline,
        18.0);

    QCOMPARE(displayFont.bold(), fontBold);
    QCOMPARE(displayFont.italic(), fontItalic);
    QCOMPARE(displayFont.underline(), fontUnderline);
    QCOMPARE(displayFont.pixelSize(), 18);
    QCOMPARE(displayFont.stretch(), static_cast<int>(QFont::Unstretched));
}

void PDFEditControllerTest::savesEditedTextRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-edit-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-edit-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kGroupedText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Saved grouped text"));
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(document.source().toLocalFile(), outputPath);
    const QString savedText = normalizedPageText(outputPath);
    QVERIFY2(
        savedText.contains(QStringLiteral("Saved grouped text")),
        qPrintable(QStringLiteral("Saved page text: %1").arg(savedText)));
    QVERIFY(!savedText.contains(QString::fromLatin1(kGroupedText)));
    QVERIFY(!savedText.contains(QStringLiteral("Hello")));
    QVERIFY(!savedText.contains(QStringLiteral("world")));
    const QStringList objectTexts = topLevelTextObjects(outputPath);
    QVERIFY(objectTexts.size() >= 3);
    QCOMPARE(objectTexts.at(0), QString::fromLatin1(kRegularText));
    QCOMPARE(objectTexts.at(1), QString::fromLatin1(kBoldText));
    QVERIFY(objectTexts.at(2).contains(QStringLiteral("Saved")));
    const QString sourceText = normalizedPageText(sourcePath);
    QVERIFY(sourceText.contains(QString::fromLatin1(kGroupedText)));
    QVERIFY(!sourceText.contains(QStringLiteral("Saved grouped text")));
}

void PDFEditControllerTest::preservesPatternFilledGraphicsWhenSavingText()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("pattern-fill-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("pattern-fill-output.pdf"));
    QVERIFY(createPatternFillPdf(sourcePath));
    QVERIFY(containsPatternColors(renderFirstPage(sourcePath, QSize(200, 200))));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QStringLiteral("Pattern edit text"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Pattern text edit"));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(normalizedPageText(outputPath).contains(
        QStringLiteral("Pattern text edit")));
    const QString verificationPath = qEnvironmentVariable(
        "TREPDF_PATTERN_TEST_OUTPUT");
    if (!verificationPath.isEmpty()) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(outputPath, verificationPath));
    }
    QVERIFY(containsPatternColors(renderFirstPage(outputPath, QSize(200, 200))));
    const QVariantList savedCandidates = waitForCandidates(controller, 0);
    QVERIFY(!candidateForText(
                 savedCandidates,
                 QStringLiteral("Pattern text edit"))
                 .isEmpty());
    QVERIFY(candidateForKind(savedCandidates, QStringLiteral("image")).isEmpty());
}

void PDFEditControllerTest::preservesPatternStrokedGraphicsWhenSavingText()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("pattern-stroke-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("pattern-stroke-output.pdf"));
    QVERIFY(createPatternStrokePdf(sourcePath));
    QVERIFY(containsPatternColors(renderFirstPage(sourcePath, QSize(200, 200))));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QStringLiteral("Pattern edit text"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Pattern text edit"));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(normalizedPageText(outputPath).contains(
        QStringLiteral("Pattern text edit")));
    QVERIFY(containsPatternColors(renderFirstPage(outputPath, QSize(200, 200))));
    const QVariantList savedCandidates = waitForCandidates(controller, 0);
    QVERIFY(!candidateForText(
                 savedCandidates,
                 QStringLiteral("Pattern text edit"))
                 .isEmpty());
    QVERIFY(candidateForKind(savedCandidates, QStringLiteral("image")).isEmpty());
}

void PDFEditControllerTest::preservesGrayscaleWhiteFillWhenSavingText()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("grayscale-white-fill-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("grayscale-white-fill-output.pdf"));
    QVERIFY(createGrayscaleWhiteFillPdf(sourcePath));
    const QImage sourceImage = renderFirstPage(sourcePath, QSize(200, 200));
    QVERIFY(!sourceImage.isNull());

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QStringLiteral("Grayscale white fill"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Edited grayscale text"));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QString verificationPath = qEnvironmentVariable(
        "TREPDF_GRAYSCALE_TEST_OUTPUT");
    if (!verificationPath.isEmpty()) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(outputPath, verificationPath));
    }
    const QImage outputImage = renderFirstPage(outputPath, QSize(200, 200));
    QVERIFY(!outputImage.isNull());
    QCOMPARE(
        changedPixelCount(
            sourceImage,
            outputImage,
            QRect(30, 110, 140, 60)),
        0);
}

void PDFEditControllerTest::savesVietnameseEditWithEmbeddedArial_data()
{
    QTest::addColumn<QString>("editedText");
    QTest::addColumn<QString>("sourceText");
    QTest::addColumn<QString>("untouchedText");
    QTest::addColumn<bool>("expectedBold");

    const QString text = QStringLiteral("Tiếng Việt");
    const QString decomposedText = text.normalized(QString::NormalizationForm_D);
    QTest::newRow("nfc-regular")
        << text
        << QString::fromLatin1(kRegularText)
        << QString::fromLatin1(kBoldText)
        << false;
    QTest::newRow("nfd-regular")
        << decomposedText
        << QString::fromLatin1(kRegularText)
        << QString::fromLatin1(kBoldText)
        << false;
    QTest::newRow("nfc-bold")
        << text
        << QString::fromLatin1(kBoldText)
        << QString::fromLatin1(kRegularText)
        << true;
    QTest::newRow("nfd-bold")
        << decomposedText
        << QString::fromLatin1(kBoldText)
        << QString::fromLatin1(kRegularText)
        << true;
}

void PDFEditControllerTest::savesVietnameseEditWithEmbeddedArial()
{
    QFETCH(QString, editedText);
    QFETCH(QString, sourceText);
    QFETCH(QString, untouchedText);
    QFETCH(bool, expectedBold);

    const QString expectedText = editedText.normalized(
        QString::NormalizationForm_C);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-vietnamese-source-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-vietnamese-output-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        sourceText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(editedText);
    QCOMPARE(controller.selectedFontFamily(), QStringLiteral("Arial"));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QString savedText = pageText(outputPath);
    QVERIFY2(
        savedText.contains(expectedText),
        qPrintable(QStringLiteral("Saved page text: %1").arg(savedText)));
    QCOMPARE(
        savedText,
        savedText.normalized(QString::NormalizationForm_C));

    const TextObjectFontInfo editedFont = textObjectFontInfo(
        outputPath,
        expectedText);
    QVERIFY(editedFont.found);
    QVERIFY2(
        editedFont.baseName.contains(QStringLiteral("Arial"), Qt::CaseInsensitive),
        qPrintable(QStringLiteral("Edited font: %1").arg(editedFont.baseName)));
    QCOMPARE(
        editedFont.baseName.contains(QStringLiteral("Bold"), Qt::CaseInsensitive),
        expectedBold);
    QCOMPARE(editedFont.embedded, 1);
    QVERIFY2(
        hasSubsetFontPrefix(editedFont.baseName),
        qPrintable(QStringLiteral("Expected subset font name, got: %1")
                       .arg(editedFont.baseName)));
    const QByteArray fullFontData = ApplicationFonts::bundledFontData(
        QStringLiteral("Arial"),
        expectedBold,
        false);
    QVERIFY(!fullFontData.isEmpty());
    QVERIFY(editedFont.dataSize > 0);
    QVERIFY2(
        editedFont.dataSize < static_cast<size_t>(fullFontData.size()),
        qPrintable(QStringLiteral("Expected subset font smaller than full face: %1 >= %2")
                       .arg(editedFont.dataSize)
                       .arg(fullFontData.size())));

    const TextObjectFontInfo untouchedFont = textObjectFontInfo(
        outputPath,
        untouchedText);
    QVERIFY(untouchedFont.found);
    QVERIFY(untouchedFont.baseName.contains(
        QStringLiteral("Helvetica"),
        Qt::CaseInsensitive));
    QCOMPARE(untouchedFont.embedded, 0);

    const QString verificationPath = qEnvironmentVariable(
        "TREPDF_VIETNAMESE_SAVE_TEST_OUTPUT");
    if (!verificationPath.isEmpty()
        && QString::fromLatin1(QTest::currentDataTag())
            == QLatin1String("nfd-bold")) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(outputPath, verificationPath));
    }
}

void PDFEditControllerTest::savesVietnameseEditWithEmbeddedSystemFont_data()
{
    QTest::addColumn<QString>("fontFamily");
    QTest::addColumn<bool>("fontBold");
    QTest::addColumn<bool>("fontItalic");
    QTest::addColumn<QString>("expectedText");

    QTest::newRow("georgia-regular")
        << QStringLiteral("Georgia") << false << false
        << QStringLiteral("Tiếng Việt");
    QTest::newRow("georgia-bold")
        << QStringLiteral("Georgia") << true << false
        << QStringLiteral("Tiếng Việt");
    QTest::newRow("georgia-italic")
        << QStringLiteral("Georgia") << false << true
        << QStringLiteral("Tiếng Việt");
    QTest::newRow("georgia-bold-italic")
        << QStringLiteral("Georgia") << true << true
        << QStringLiteral("Tiếng Việt");
    // Helvetica Neue is stored in a TTC on macOS. The styled row verifies that the
    // selected face is materialized instead of PDFium loading collection face 0.
    QTest::newRow("helvetica-neue-ttc-bold-italic")
        << QStringLiteral("Helvetica Neue") << true << true
        << QStringLiteral("Tiếng Việt");
    // Heiti TC's exact TrueType face is about 53 MiB. This verifies that a system
    // face above the old 32 MiB limit reaches PDFium and is subset on save.
    QTest::newRow("heiti-tc-large-ttc")
        << QStringLiteral("Heiti TC") << false << false
        << QStringLiteral("繁體漢字測試");
}

void PDFEditControllerTest::savesVietnameseEditWithEmbeddedSystemFont()
{
    QFETCH(QString, fontFamily);
    QFETCH(bool, fontBold);
    QFETCH(bool, fontItalic);
    QFETCH(QString, expectedText);

    if (!ApplicationFonts::systemFontFamilies().contains(
            fontFamily,
            Qt::CaseInsensitive)) {
        QSKIP(qPrintable(QStringLiteral("Missing system font: %1").arg(fontFamily)));
    }
    ApplicationFonts::FontFaceData systemFace =
        ApplicationFonts::systemFontFace(fontFamily, fontBold, fontItalic);
    if (!systemFace.isValid()) {
        QSKIP(qPrintable(QStringLiteral("System face unavailable: %1 (%2)")
                             .arg(fontFamily)
                             .arg(static_cast<int>(systemFace.error))));
    }
    const size_t fullFontDataSize = static_cast<size_t>(systemFace.data.size());
    systemFace.data.clear();
    systemFace.data.squeeze();

    const QString editedText = expectedText.normalized(
        QString::NormalizationForm_D);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-system-font-source-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-system-font-output-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(editedText);
    controller.setSelectedTextRange(0, editedText.size());
    controller.setSelectedFontFamily(fontFamily);
    controller.setSelectedFontBold(fontBold);
    controller.setSelectedFontItalic(fontItalic);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.selectedFontFamily(), fontFamily);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QString savedText = pageText(outputPath);
    QVERIFY2(
        normalizedPageText(outputPath).contains(expectedText),
        qPrintable(QStringLiteral("Saved page text: %1").arg(savedText)));
    QCOMPARE(savedText, savedText.normalized(QString::NormalizationForm_C));

    const TextObjectFontInfo editedFont = textObjectFontInfo(
        outputPath,
        expectedText.left(3));
    QVERIFY(editedFont.found);
    const QString compactExpectedFamily = compactFontName(fontFamily);
    QVERIFY2(
        compactFontName(editedFont.familyName).contains(compactExpectedFamily)
            || compactFontName(editedFont.baseName).contains(compactExpectedFamily),
        qPrintable(QStringLiteral("Saved font: %1 / %2")
                       .arg(editedFont.familyName, editedFont.baseName)));
    QCOMPARE(
        editedFont.baseName.contains(QStringLiteral("Bold"), Qt::CaseInsensitive),
        fontBold);
    QCOMPARE(editedFont.italic, fontItalic);
    QCOMPARE(editedFont.embedded, 1);
    QVERIFY2(
        hasSubsetFontPrefix(editedFont.baseName),
        qPrintable(QStringLiteral("Expected subset font name, got: %1")
                       .arg(editedFont.baseName)));
    QVERIFY(editedFont.dataSize > 0);
    QVERIFY2(
        editedFont.dataSize < fullFontDataSize,
        qPrintable(QStringLiteral("Expected subset font smaller than full face: %1 >= %2")
                       .arg(editedFont.dataSize)
                       .arg(fullFontDataSize)));

    const TextObjectFontInfo untouchedFont = textObjectFontInfo(
        outputPath,
        QString::fromLatin1(kBoldText));
    QVERIFY(untouchedFont.found);
    QVERIFY(untouchedFont.baseName.contains(
        QStringLiteral("Helvetica"),
        Qt::CaseInsensitive));
    QCOMPARE(untouchedFont.embedded, 0);

    const QString verificationPath = qEnvironmentVariable(
        "TREPDF_SYSTEM_FONT_SAVE_TEST_OUTPUT");
    if (!verificationPath.isEmpty()
        && QString::fromLatin1(QTest::currentDataTag())
            == QLatin1String("helvetica-neue-ttc-bold-italic")) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(outputPath, verificationPath));
    }
}

void PDFEditControllerTest::preservesInheritedSystemFontForVietnamese()
{
    const QString fontFamily = QStringLiteral("Helvetica Neue");
    if (!ApplicationFonts::systemFontFamilies().contains(
            fontFamily,
            Qt::CaseInsensitive)) {
        QSKIP("Helvetica Neue is not installed");
    }
    const ApplicationFonts::FontFaceData systemFace =
        ApplicationFonts::systemFontFace(fontFamily, false, false);
    if (!systemFace.isValid()) {
        QSKIP("Helvetica Neue regular cannot be embedded on this host");
    }

    const QString expectedText = QStringLiteral("Tiếng Việt");
    const QRawFont rawFont(
        systemFace.data,
        16.0,
        QFont::PreferNoHinting);
    if (!rawFont.isValid()) {
        QSKIP("Materialized Helvetica Neue face is invalid");
    }
    for (uint character : expectedText.toUcs4()) {
        if (!QChar::isSpace(static_cast<char32_t>(character))
            && !rawFont.supportsCharacter(character)) {
            QSKIP("Helvetica Neue does not cover the Vietnamese fixture");
        }
    }

    const QString sourceText = QStringLiteral("Inherited system font");
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-inherited-system-font-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-inherited-system-font-output.pdf"));
    QVERIFY(createEmbeddedFontPdf(sourcePath, systemFace.data, sourceText));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        sourceText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(
        expectedText.normalized(QString::NormalizationForm_D));
    QCOMPARE(controller.selectedFontFamily(), systemFace.family);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(normalizedPageText(outputPath).contains(expectedText));
    const TextObjectFontInfo editedFont = textObjectFontInfo(
        outputPath,
        expectedText);
    QVERIFY(editedFont.found);
    const QString compactExpectedFamily = compactFontName(fontFamily);
    QVERIFY2(
        compactFontName(editedFont.familyName).contains(compactExpectedFamily)
            || compactFontName(editedFont.baseName).contains(compactExpectedFamily),
        qPrintable(QStringLiteral("Saved font: %1 / %2")
                       .arg(editedFont.familyName, editedFont.baseName)));
    QCOMPARE(editedFont.embedded, 1);
    QVERIFY2(
        hasSubsetFontPrefix(editedFont.baseName),
        qPrintable(QStringLiteral("Expected subset font name, got: %1")
                       .arg(editedFont.baseName)));
    QVERIFY(editedFont.dataSize > 0);
    QVERIFY(editedFont.dataSize < static_cast<size_t>(systemFace.data.size()));
}

void PDFEditControllerTest::rejectsUnsupportedSystemFontForVietnamese_data()
{
    QTest::addColumn<QString>("fontFamily");
    QTest::addColumn<bool>("fontItalic");
    QTest::addColumn<bool>("faceDataValid");
    QTest::addColumn<bool>("selectable");

    QTest::newRow("missing-vietnamese-glyphs")
        << QStringLiteral("Comic Sans MS") << false << true << true;
    QTest::newRow("restricted-license")
        << QStringLiteral("Brush Script MT") << true << false << false;
    QTest::newRow("preview-print-only")
        << QStringLiteral("Avenir Next") << false << false << false;
}

void PDFEditControllerTest::rejectsUnsupportedSystemFontForVietnamese()
{
    QFETCH(QString, fontFamily);
    QFETCH(bool, fontItalic);
    QFETCH(bool, faceDataValid);
    QFETCH(bool, selectable);

    if (!ApplicationFonts::systemFontFamilies().contains(
            fontFamily,
            Qt::CaseInsensitive)) {
        QSKIP(qPrintable(QStringLiteral("Missing system font: %1").arg(fontFamily)));
    }
    const ApplicationFonts::FontFaceData systemFace =
        ApplicationFonts::systemFontFace(fontFamily, false, fontItalic);
    if (systemFace.isValid() != faceDataValid) {
        QSKIP(qPrintable(QStringLiteral("System font prerequisites changed: %1")
                             .arg(fontFamily)));
    }
    if (ApplicationFonts::isFontFaceSelectable(
            fontFamily,
            false,
            fontItalic) != selectable) {
        QSKIP(qPrintable(QStringLiteral("System font filter changed: %1")
                             .arg(fontFamily)));
    }
    QVERIFY(faceDataValid
            || systemFace.error != ApplicationFonts::FontFaceData::Error::None);
    if (QString::fromLatin1(QTest::currentDataTag())
        == QLatin1String("preview-print-only")) {
        if (systemFace.error
            != ApplicationFonts::FontFaceData::Error::EmbeddingRestricted) {
            QSKIP("Avenir Next embedding permission changed");
        }
    }
    if (QString::fromLatin1(QTest::currentDataTag())
            == QLatin1String("missing-vietnamese-glyphs")
        && systemFace.isValid()) {
        const QRawFont rawFont(
            systemFace.data,
            16.0,
            QFont::PreferNoHinting);
        QVERIFY(rawFont.isValid());
        bool supportsVietnamese = true;
        for (uint character : QStringLiteral("Tiếng Việt").toUcs4()) {
            if (!QChar::isSpace(static_cast<char32_t>(character))
                && !rawFont.supportsCharacter(character)) {
                supportsVietnamese = false;
                break;
            }
        }
        if (supportsVietnamese) {
            QSKIP("Comic Sans MS now covers the Vietnamese fixture");
        }
    }

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("reject-system-font-source-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("reject-system-font-output-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    if (!selectable) {
        const QString originalFamily = controller.selectedFontFamily();
        controller.setSelectedFontFamily(fontFamily);
        QCOMPARE(controller.selectedFontFamily(), originalFamily);
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!ApplicationFonts::availableFontFamilies(
            false,
            fontItalic).contains(fontFamily, Qt::CaseInsensitive));
        return;
    }

    const QString editedText = QStringLiteral("Tiếng Việt").normalized(
        QString::NormalizationForm_D);
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(editedText);
    controller.setSelectedTextRange(0, editedText.size());
    controller.setSelectedFontFamily(fontFamily);
    controller.setSelectedFontItalic(fontItalic);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.selectedFontFamily(), fontFamily);

    const int revisionBeforeSave = document.renderRevision();
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    const PDFSaveWriter::Result writerResult = PDFSaveWriter::save(
        controller,
        outputPath);
    QCOMPARE(writerResult.error, PDFSaveWriter::Error::FontUnavailable);
    QCOMPARE(writerResult.detail, fontFamily);
    if (QString::fromLatin1(QTest::currentDataTag())
        == QLatin1String("missing-vietnamese-glyphs")) {
        QVERIFY(writerResult.unsupportedCodePoint != 0);
        const QRawFont rawFont(
            systemFace.data,
            16.0,
            QFont::PreferNoHinting);
        QVERIFY(rawFont.isValid());
        QVERIFY(!rawFont.supportsCharacter(writerResult.unsupportedCodePoint));
    } else {
        QCOMPARE(writerResult.unsupportedCodePoint, 0U);
    }
    QSignalSpy statusSpy(&controller, &PDFEditController::statusMessage);
    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!QFile::exists(outputPath));
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(document.source().toLocalFile(), sourcePath);
    QCOMPARE(document.renderRevision(), revisionBeforeSave);
    QVERIFY(pageText(sourcePath).contains(QString::fromLatin1(kRegularText)));
    QVERIFY(!statusSpy.isEmpty());
    const QString statusMessage = statusSpy.constLast().constFirst().toString();
    if (writerResult.unsupportedCodePoint != 0) {
        QCOMPARE(statusMessage, QStringLiteral("PdfSaveFontMissingCharacter"));
    } else {
        QCOMPARE(statusMessage, QStringLiteral("PdfSaveFontUnavailable"));
    }
}

void PDFEditControllerTest::preservesEmbeddedFontForFormatOnlyVietnameseEdit()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-embedded-format-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-embedded-format-output.pdf"));
    const QString editedText = QStringLiteral("Tiếng Việt gốc");
    const QString untouchedText = QStringLiteral("Không đổi");
    QVERIFY(createEmbeddedArialPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        editedText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.setSelectedColor(QColor(QStringLiteral("#c62828")));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QString savedText = pageText(outputPath);
    QVERIFY(savedText.contains(editedText));
    QVERIFY(savedText.contains(untouchedText));
    QVERIFY(textObjectsShareFont(outputPath, editedText, untouchedText));

    const TextObjectFontInfo fontInfo = textObjectFontInfo(
        outputPath,
        editedText);
    QVERIFY(fontInfo.found);
    QCOMPARE(fontInfo.embedded, 1);
}

void PDFEditControllerTest::savesNormalizedVietnameseRichText()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-vietnamese-rich-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-vietnamese-rich-output.pdf"));
    const QString expectedText = QStringLiteral("Tiếng thường\nViệt");
    const QString editedText = expectedText.normalized(
        QString::NormalizationForm_D);
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(editedText);
    controller.setSelectedTextRange(0, editedText.indexOf(QLatin1Char(' ')));
    controller.setSelectedFontBold(true);
    controller.finishSelectedTextEdit();

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QString savedText = pageText(outputPath);
    QVERIFY2(
        normalizedPageText(outputPath).contains(
            QStringLiteral("Tiếng thường Việt")),
        qPrintable(QStringLiteral("Saved page text: %1").arg(savedText)));
    QCOMPARE(
        savedText,
        savedText.normalized(QString::NormalizationForm_C));

    const TextObjectFontInfo boldFont = textObjectFontInfo(
        outputPath,
        QStringLiteral("Tiếng"));
    const TextObjectFontInfo regularFont = textObjectFontInfo(
        outputPath,
        QStringLiteral("thường"));
    QVERIFY(boldFont.found);
    QVERIFY(regularFont.found);
    QVERIFY(boldFont.baseName.contains(
        QStringLiteral("Bold"),
        Qt::CaseInsensitive));
    QVERIFY(!regularFont.baseName.contains(
        QStringLiteral("Bold"),
        Qt::CaseInsensitive));
    QCOMPARE(boldFont.embedded, 1);
    QCOMPARE(regularFont.embedded, 1);

    FS_MATRIX firstLine = {};
    FS_MATRIX secondLine = {};
    QVERIFY(textObjectMatrix(outputPath, QStringLiteral("Tiếng"), firstLine));
    QVERIFY(textObjectMatrix(outputPath, QStringLiteral("Việt"), secondLine));
    QVERIFY(secondLine.f < firstLine.f - 1.0F);
}

void PDFEditControllerTest::addsHighlightWithUndoRedoAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-highlight-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QCOMPARE(controller.highlightColor(), QColor(255, 235, 59, 96));
    QSignalSpy highlightColorSpy(
        &controller,
        &PDFEditController::highlightColorChanged);
    const QColor chosenColor(255, 128, 171, 96);
    controller.setHighlightColor(chosenColor);
    QCOMPARE(controller.highlightColor(), chosenColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    controller.setHighlightColor(chosenColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    controller.setTool(PDFEditController::HighlightTool);

    const QRectF selectionRect = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantMap highlight = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(
        highlight.value(QStringLiteral("kind")).toString(),
        QStringLiteral("highlight"));
    QCOMPARE(
        highlight.value(QStringLiteral("sourceRect")).toRectF(),
        selectionRect);
    QCOMPARE(
        highlight.value(QStringLiteral("highlightColor")).value<QColor>(),
        chosenColor);
    QCOMPARE(
        highlight.value(QStringLiteral("highlightRects")).toList().size(),
        1);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        candidateForKind(
            controller.candidatesForPage(0),
            QStringLiteral("highlight"))
            .value(QStringLiteral("objectId"))
            .toString(),
        highlight.value(QStringLiteral("objectId")).toString());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());

    controller.setTool(PDFEditController::SelectTool);
    QCOMPARE(
        controller.candidateAt(
            0,
            selectionRect.center().x(),
            selectionRect.center().y())
            .value(QStringLiteral("kind"))
            .toString(),
        QStringLiteral("highlight"));
    controller.setTool(PDFEditController::EditTextTool);
    QCOMPARE(
        controller.candidateAt(
            0,
            selectionRect.center().x(),
            selectionRect.center().y())
            .value(QStringLiteral("objectId"))
            .toString(),
        textCandidate.value(QStringLiteral("objectId")).toString());

    controller.setHighlightColor(QColor(79, 195, 247, 96));
    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(candidateForKind(
                controller.candidatesForPage(0),
                QStringLiteral("highlight"))
                .isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());

    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("highlightColor"))
            .value<QColor>(),
        chosenColor);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());

    controller.discardDrafts();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::savesHighlightAnnotationRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);
    const QRectF selectionRect(0.10, 0.20, 0.30, 0.05);
    const QColor highlightColor(139, 195, 74, 96);
    controller.setHighlightColor(highlightColor);
    QVERIFY(!controller.addHighlight(0, selectionRect).isEmpty());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const HighlightSaveInspection sourceInspection
        = inspectHighlights(sourcePath);
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.count, 0);

    const HighlightSaveInspection inspection
        = inspectHighlights(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.color, highlightColor);
    QVERIFY(inspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(inspection.quadPoints.size(), 1);

    const qreal expectedLeft = selectionRect.left() * pageSize.width();
    const qreal expectedRight = selectionRect.right() * pageSize.width();
    const qreal expectedTop = (1.0 - selectionRect.top())
        * pageSize.height();
    const qreal expectedBottom = (1.0 - selectionRect.bottom())
        * pageSize.height();
    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    QVERIFY(closeEnough(inspection.rect.left, expectedLeft));
    QVERIFY(closeEnough(inspection.rect.right, expectedRight));
    QVERIFY(closeEnough(inspection.rect.top, expectedTop));
    QVERIFY(closeEnough(inspection.rect.bottom, expectedBottom));
    const FS_QUADPOINTSF quad = inspection.quadPoints.constFirst();
    QVERIFY(closeEnough(quad.x1, expectedLeft));
    QVERIFY(closeEnough(quad.y1, expectedTop));
    QVERIFY(closeEnough(quad.x2, expectedRight));
    QVERIFY(closeEnough(quad.y2, expectedTop));
    QVERIFY(closeEnough(quad.x3, expectedLeft));
    QVERIFY(closeEnough(quad.y3, expectedBottom));
    QVERIFY(closeEnough(quad.x4, expectedRight));
    QVERIFY(closeEnough(quad.y4, expectedBottom));
}

void PDFEditControllerTest::savesHighlightAlongsideTextEdit()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-text-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-text-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    QVERIFY(!controller.addHighlight(
        0,
        QRectF(0.12, 0.16, 0.25, 0.04)).isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        textCandidate.value(QStringLiteral("objectId")).toString()));
    const QString editedText = QStringLiteral("Highlighted edited text");
    controller.commitSelectedText(editedText);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(normalizedPageText(outputPath).contains(editedText));
    const HighlightSaveInspection inspection
        = inspectHighlights(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.quadPoints.size(), 1);
}

void PDFEditControllerTest::preservesPatternGraphicsWhenSavingHighlight()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-pattern-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-highlight-pattern-output.pdf"));
    QVERIFY(createPatternFillPdf(sourcePath));
    QVERIFY(containsPatternColors(renderFirstPage(sourcePath, QSize(200, 200))));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(!controller.addHighlight(
        0,
        QRectF(0.70, 0.10, 0.20, 0.05)).isEmpty());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const HighlightSaveInspection inspection
        = inspectHighlights(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QVERIFY(containsPatternColors(renderFirstPage(outputPath, QSize(200, 200))));
}

void PDFEditControllerTest::addsBlankPageWithUndoRedoAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-blank-page-source.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.sourcePageCount(), 2);
    const QSizeF currentPageSize = document.pageSize(0);
    const QSizeF followingPageSize = document.pageSize(1);
    QCOMPARE(currentPageSize, QSizeF(595.0, 842.0));
    QCOMPARE(followingPageSize, QSizeF(320.0, 480.0));
    controller.setDocument(&document);

    QSignalSpy pageCountSpy(&document, &PDFDocument::pageCountChanged);
    QSignalSpy pageStructureSpy(
        &document,
        &PDFDocument::pageStructureChanged);
    const int addedPage = controller.addBlankPage();

    QCOMPARE(addedPage, 1);
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), addedPage);
    QCOMPARE(document.pageSize(addedPage), currentPageSize);
    QVERIFY(document.isTransientBlankPage(addedPage));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), -1);
    QCOMPARE(document.sourcePageIndex(2), 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(pageCountSpy.count(), 1);
    QCOMPARE(pageStructureSpy.count(), 1);
    QCOMPARE(pageStructureSpy.constLast().constFirst().toBool(), false);

    QCOMPARE(controller.undo(), 0);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(addedPage), followingPageSize);
    QVERIFY(!document.isTransientBlankPage(addedPage));
    QCOMPARE(document.sourcePageIndex(addedPage), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.canRedo());
    QCOMPARE(pageCountSpy.count(), 2);
    QCOMPARE(pageStructureSpy.count(), 2);
    QCOMPARE(pageStructureSpy.constLast().constFirst().toBool(), false);

    QCOMPARE(controller.redo(), addedPage);
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), addedPage);
    QCOMPARE(document.pageSize(addedPage), currentPageSize);
    QVERIFY(document.isTransientBlankPage(addedPage));
    QCOMPARE(document.sourcePageIndex(2), 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(pageCountSpy.count(), 3);
    QCOMPARE(pageStructureSpy.count(), 3);
    QCOMPARE(pageStructureSpy.constLast().constFirst().toBool(), false);

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.pageSize(addedPage), followingPageSize);
    QVERIFY(!document.isTransientBlankPage(addedPage));
    QCOMPARE(document.sourcePageIndex(addedPage), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(pageCountSpy.count(), 4);
    QCOMPARE(pageStructureSpy.count(), 4);
    QCOMPARE(pageStructureSpy.constLast().constFirst().toBool(), true);
}

void PDFEditControllerTest::savesBlankPageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-blank-page-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-blank-page-output.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    const QSizeF currentPageSize = document.pageSize(0);
    const QSizeF followingPageSize = document.pageSize(1);
    controller.setDocument(&document);

    const int addedPage = controller.addBlankPage();
    QCOMPARE(addedPage, 1);
    QCOMPARE(document.pageSize(addedPage), currentPageSize);
    QCOMPARE(document.sourcePageIndex(addedPage), -1);
    QCOMPARE(document.sourcePageIndex(2), 1);
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.sourcePageCount(), 3);
    QCOMPARE(document.pageSize(addedPage), currentPageSize);
    QVERIFY(!document.isTransientBlankPage(addedPage));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    PDFDocument savedDocument;
    QVERIFY(savedDocument.open(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(savedDocument.pageCount(), 3);
    QCOMPARE(savedDocument.pageSize(0), currentPageSize);
    QCOMPARE(savedDocument.pageSize(addedPage), currentPageSize);
    QCOMPARE(savedDocument.pageSize(2), followingPageSize);
    QVERIFY(normalizedPageText(outputPath, addedPage).isEmpty());

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 2);
    QCOMPARE(sourceDocument.pageSize(0), currentPageSize);
    QCOMPARE(sourceDocument.pageSize(1), followingPageSize);
}

void PDFEditControllerTest::savesEditAfterInsertingEarlierPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-inserted-page-edit-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-inserted-page-edit-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    const QVariantMap sourceCandidate = candidateForText(
        waitForCandidates(controller, 1),
        QStringLiteral("Second fixture page"));
    QVERIFY(!sourceCandidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        1,
        sourceCandidate.value(QStringLiteral("objectId")).toString()));
    const QString editedText = QStringLiteral("Edited page after insertion");
    controller.commitSelectedText(editedText);
    QCOMPARE(controller.draftsForPage(1).size(), 1);

    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);
    QCOMPARE(document.pageCount(), 4);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), -1);
    QCOMPARE(document.sourcePageIndex(2), 1);
    QCOMPARE(document.sourcePageIndex(3), 2);
    QVERIFY(document.isTransientBlankPage(1));
    QVERIFY(controller.draftsForPage(1).isEmpty());
    const QVariantList shiftedCandidates = waitForCandidates(controller, 2);
    QVERIFY(!candidateForText(shiftedCandidates, editedText).isEmpty());
    QCOMPARE(controller.draftsForPage(2).size(), 1);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.pageCount(), 4);
    QCOMPARE(document.sourcePageCount(), 4);
    QCOMPARE(document.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(1), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(2), QSizeF(320.0, 480.0));
    QCOMPARE(document.pageSize(3), QSizeF(500.0, 700.0));
    QVERIFY(normalizedPageText(outputPath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(outputPath, 1).isEmpty());
    QVERIFY(normalizedPageText(outputPath, 2).contains(editedText));
    QVERIFY(normalizedPageText(outputPath, 3).contains(
        QStringLiteral("Third fixture page")));
    QVERIFY(!controller.hasDrafts());

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 3);
    QVERIFY(normalizedPageText(sourcePath, 1).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(!normalizedPageText(sourcePath, 1).contains(editedText));
}

void PDFEditControllerTest::savesMultipleInsertedPagesAfterUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-multiple-inserted-pages-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-multiple-inserted-pages-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    QCOMPARE(controller.addBlankPage(), 1);
    document.setCurrentPage(2);
    QCOMPARE(controller.addBlankPage(), 3);
    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);

    QCOMPARE(document.pageCount(), 6);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), -1);
    QCOMPARE(document.sourcePageIndex(2), -1);
    QCOMPARE(document.sourcePageIndex(3), 1);
    QCOMPARE(document.sourcePageIndex(4), -1);
    QCOMPARE(document.sourcePageIndex(5), 2);

    document.setCurrentPage(3);
    QCOMPARE(controller.deleteCurrentPage(), 3);
    QCOMPARE(document.pageCount(), 5);
    QCOMPARE(document.sourcePageIndex(3), -1);
    QCOMPARE(document.sourcePageIndex(4), 2);

    document.setCurrentPage(2);
    QCOMPARE(controller.deleteCurrentPage(), 2);
    QCOMPARE(document.pageCount(), 4);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), -1);
    QCOMPARE(document.sourcePageIndex(2), -1);
    QCOMPARE(document.sourcePageIndex(3), 2);

    QCOMPARE(controller.undo(), 2);
    QCOMPARE(document.pageCount(), 5);
    QCOMPARE(controller.undo(), 3);
    QCOMPARE(document.pageCount(), 6);
    QCOMPARE(document.sourcePageIndex(3), 1);
    QCOMPARE(controller.redo(), 3);
    QCOMPARE(document.pageCount(), 5);
    QCOMPARE(controller.redo(), 2);
    QCOMPARE(document.pageCount(), 4);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.pageCount(), 4);
    QCOMPARE(document.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(1), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(2), QSizeF(320.0, 480.0));
    QCOMPARE(document.pageSize(3), QSizeF(500.0, 700.0));
    QVERIFY(normalizedPageText(outputPath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(outputPath, 1).isEmpty());
    QVERIFY(normalizedPageText(outputPath, 2).isEmpty());
    QVERIFY(normalizedPageText(outputPath, 3).contains(
        QStringLiteral("Third fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 0).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 3).contains(
        QStringLiteral("Second fixture page")));

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 3);
    QVERIFY(normalizedPageText(sourcePath, 1).contains(
        QStringLiteral("Second fixture page")));
}

void PDFEditControllerTest::deletesSourcePageWithUndoRedoAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-source-page.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QSignalSpy pageStructureSpy(
        &document,
        &PDFDocument::pageStructureChanged);
    document.setCurrentPage(1);

    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 1);
    QCOMPARE(document.sourcePageIndex(2), 2);
    QCOMPARE(document.displayPageIndexForSourcePage(0), 0);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(2), 2);

    QCOMPARE(controller.deleteCurrentPage(), 1);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(500.0, 700.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 2);
    QCOMPARE(document.displayPageIndexForSourcePage(0), 0);
    QCOMPARE(document.displayPageIndexForSourcePage(1), -1);
    QCOMPARE(document.displayPageIndexForSourcePage(2), 1);
    QCOMPARE(pageStructureSpy.count(), 1);
    QCOMPARE(
        pageStructureSpy.constFirst().constFirst().toBool(),
        true);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());

    controller.undo();
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 1);
    QCOMPARE(document.sourcePageIndex(2), 2);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.canRedo());

    controller.redo();
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(500.0, 700.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 2);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 1);
    QCOMPARE(document.sourcePageIndex(2), 2);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::refusesToDeleteOnlyPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-only-page.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);

    QCOMPARE(controller.deleteCurrentPage(), -1);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), pageSize);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::deletesTransientBlankPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-transient-blank-page.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF sourcePageSize = document.pageSize(0);

    const int blankPage = controller.addBlankPage();
    QCOMPARE(blankPage, 1);
    document.setCurrentPage(blankPage);
    QCOMPARE(document.sourcePageIndex(blankPage), -1);

    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), sourcePageSize);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QVERIFY(controller.canUndo());

    QCOMPARE(controller.undo(), blankPage);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), blankPage);
    QCOMPARE(document.pageSize(blankPage), sourcePageSize);
    QCOMPARE(document.sourcePageIndex(blankPage), -1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(controller.canRedo());

    controller.redo();
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.sourcePageIndex(0), 0);

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), sourcePageSize);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::restoresLastPageSelectionWhenUndoingDeletion()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-last-page-selection.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    document.setCurrentPage(2);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 2),
        QStringLiteral("Third fixture page"));
    QVERIFY(!candidate.isEmpty());
    const QString objectId = candidate.value(
        QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectCandidate(2, objectId));

    QCOMPARE(controller.deleteCurrentPage(), 1);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(controller.selectedPage(), -1);

    QCOMPARE(controller.undo(), 2);
    QCOMPARE(document.currentPage(), 2);
    QCOMPARE(controller.selectedPage(), 2);
    QCOMPARE(controller.selectedObjectId(), objectId);

    QCOMPARE(controller.redo(), 1);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(controller.selectedPage(), -1);
}

void PDFEditControllerTest::mapsCandidatesAfterDeletingEarlierPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-page-candidate-mapping.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    document.setCurrentPage(0);

    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), QSizeF(320.0, 480.0));
    QCOMPARE(document.sourcePageIndex(0), 1);

    const QVariantList shiftedCandidates = waitForCandidates(controller, 0);
    QVERIFY(!candidateForText(
                 shiftedCandidates,
                 QStringLiteral("Second fixture page")).isEmpty());
    QVERIFY(candidateForText(
                shiftedCandidates,
                QStringLiteral("First fixture page")).isEmpty());

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.sourcePageIndex(0), 0);
}

void PDFEditControllerTest::savesDeletedPageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-deleted-page-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-deleted-page-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    document.setCurrentPage(1);

    QCOMPARE(controller.deleteCurrentPage(), 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    QCOMPARE(document.source().toLocalFile(), outputPath);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.sourcePageCount(), 2);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(1), QSizeF(500.0, 700.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(normalizedPageText(outputPath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(outputPath, 1).contains(
        QStringLiteral("Third fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 0).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 1).contains(
        QStringLiteral("Second fixture page")));

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 3);
    QCOMPARE(sourceDocument.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(sourceDocument.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(sourceDocument.pageSize(2), QSizeF(500.0, 700.0));
    QVERIFY(normalizedPageText(sourcePath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(sourcePath, 1).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(normalizedPageText(sourcePath, 2).contains(
        QStringLiteral("Third fixture page")));
}

void PDFEditControllerTest::savesEditAfterDeletingEarlierPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-shifted-edit-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-shifted-edit-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap sourceCandidate = candidateForText(
        waitForCandidates(controller, 1),
        QStringLiteral("Second fixture page"));
    QVERIFY(!sourceCandidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        1,
        sourceCandidate.value(QStringLiteral("objectId")).toString()));
    const QString editedText = QStringLiteral("Edited shifted second page");
    controller.commitSelectedText(editedText);
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.draftsForPage(1).size(), 1);

    document.setCurrentPage(0);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.sourcePageIndex(0), 1);
    QCOMPARE(document.sourcePageIndex(1), 2);
    const QVariantList shiftedCandidates = waitForCandidates(controller, 0);
    QVERIFY(!candidateForText(shiftedCandidates, editedText).isEmpty());
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.draftsForPage(1).isEmpty());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.pageSize(0), QSizeF(320.0, 480.0));
    QCOMPARE(document.pageSize(1), QSizeF(500.0, 700.0));
    const QString firstOutputPageText = normalizedPageText(outputPath, 0);
    const QString secondOutputPageText = normalizedPageText(outputPath, 1);
    QVERIFY(firstOutputPageText.contains(editedText));
    QVERIFY(!firstOutputPageText.contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(secondOutputPageText.contains(
        QStringLiteral("Third fixture page")));
    QVERIFY(!secondOutputPageText.contains(editedText));
    QVERIFY(!secondOutputPageText.contains(
        QStringLiteral("Second fixture page")));

    QCOMPARE(normalizedPageText(sourcePath, 0), QStringLiteral("First fixture page"));
    QCOMPARE(normalizedPageText(sourcePath, 1), QStringLiteral("Second fixture page"));
    QCOMPARE(normalizedPageText(sourcePath, 2), QStringLiteral("Third fixture page"));
}

void PDFEditControllerTest::savesBlankAfterDeletingAllSourcePages()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-blank-without-source-pages-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-blank-without-source-pages-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    document.setCurrentPage(1);
    QCOMPARE(controller.deleteCurrentPage(), 1);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 2);

    document.setCurrentPage(1);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.sourcePageIndex(0), 0);
    const QSizeF blankPageSize = document.pageSize(0);
    QCOMPARE(blankPageSize, QSizeF(240.0, 360.0));

    const int blankPage = controller.addBlankPage();
    QCOMPARE(blankPage, 1);
    QCOMPARE(document.sourcePageIndex(blankPage), -1);
    QCOMPARE(document.pageSize(blankPage), blankPageSize);

    document.setCurrentPage(0);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.sourcePageIndex(0), -1);
    QCOMPARE(document.pageSize(0), blankPageSize);
    QVERIFY(document.isTransientBlankPage(0));
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.sourcePageCount(), 1);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.pageSize(0), blankPageSize);
    QVERIFY(!document.isTransientBlankPage(0));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(normalizedPageText(outputPath).isEmpty());

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 3);
    QCOMPARE(sourceDocument.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(sourceDocument.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(sourceDocument.pageSize(2), QSizeF(500.0, 700.0));
    QVERIFY(normalizedPageText(sourcePath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(sourcePath, 1).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(normalizedPageText(sourcePath, 2).contains(
        QStringLiteral("Third fixture page")));
}

void PDFEditControllerTest::savesCurrentFileRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-current.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kBoldText));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Saved to current file"));
    const int revisionBeforeSave = document.renderRevision();
    const QSizeF pageSizeBeforeSave = document.pageSize(0);
    QVERIFY(controller.canUndo());

    QVERIFY(controller.save());
    QCOMPARE(document.source().toLocalFile(), sourcePath);
    QVERIFY(document.renderRevision() > revisionBeforeSave);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.pageSize(0), pageSizeBeforeSave);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(normalizedPageText(sourcePath).contains(
        QStringLiteral("Saved to current file")));
}

void PDFEditControllerTest::savesUnchangedCopyRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-copy-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-copy-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));
    const QString sourceText = normalizedPageText(sourcePath);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(!controller.hasDrafts());

    QSignalSpy saveFinishedSpy(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(outputPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 5000);
    QCOMPARE(saveFinishedSpy.constFirst().constFirst().toBool(), true);
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(normalizedPageText(outputPath), sourceText);
    QCOMPARE(normalizedPageText(sourcePath), sourceText);
}

void PDFEditControllerTest::savesMultipleDraftsOnSamePage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-multiple-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-multiple-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const QVariantMap regularCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kRegularText));
    const QVariantMap boldCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kBoldText));
    QVERIFY(controller.selectCandidate(
        0,
        regularCandidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("First draft saved"));
    QVERIFY(controller.selectCandidate(
        0,
        boldCandidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Second draft saved"));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QString savedText = normalizedPageText(outputPath);
    QVERIFY(savedText.contains(QStringLiteral("First draft saved")));
    QVERIFY(savedText.contains(QStringLiteral("Second draft saved")));
    QVERIFY(!savedText.contains(QString::fromLatin1(kRegularText)));
    QVERIFY(!savedText.contains(QString::fromLatin1(kBoldText)));
}

void PDFEditControllerTest::savesExpandedFrameBeforeRightColumn()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-column-frame-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-column-frame-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const QVariantMap leftCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kLeftColumnText));
    const QVariantMap rightCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kRightColumnText));
    QVERIFY(!leftCandidate.isEmpty());
    QVERIFY(!rightCandidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        leftCandidate.value(QStringLiteral("objectId")).toString()));

    controller.beginSelectedTextEdit();
    const QRectF expandedFrameRect = controller.selectedFrameRect();
    const QRectF rightSourceRect = rightCandidate.value(
        QStringLiteral("sourceRect")).toRectF();
    QVERIFY(expandedFrameRect.right() < rightSourceRect.left());
    const QString editedText = QStringLiteral("Edited left column stays");
    controller.updateSelectedTextPreview(editedText);
    controller.finishSelectedTextEdit();

    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        controller.draftsForPage(0).constFirst().toMap()
            .value(QStringLiteral("frameRect")).toRectF(),
        expandedFrameRect);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QString savedText = normalizedPageText(outputPath);
    QVERIFY(savedText.contains(editedText));
    QVERIFY(savedText.contains(QString::fromLatin1(kRightColumnText)));
}

void PDFEditControllerTest::savesParagraphAlignmentOnlyEdit()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-alignment-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-alignment-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString text = QString::fromLatin1(kRegularText);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        text);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));

    const QRectF originalFrameRect = controller.selectedFrameRect();
    controller.beginSelectedTextEdit();
    const QRectF expandedFrameRect = controller.selectedFrameRect();
    QVERIFY(expandedFrameRect.right() > originalFrameRect.right());
    controller.setSelectedParagraphAlignment(
        PDFEditController::RightAlignment);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.selectedFrameRect(), expandedFrameRect);

    QPointF sourceOrigin;
    QVERIFY(textObjectOrigin(sourcePath, text, sourceOrigin));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QPointF savedOrigin;
    QVERIFY(textObjectOrigin(outputPath, text, savedOrigin));
    QVERIFY2(
        savedOrigin.x() > sourceOrigin.x() + 1.0,
        qPrintable(QStringLiteral(
            "Expected right alignment to move text right from %1, got %2")
                       .arg(sourceOrigin.x(), 0, 'f', 3)
                       .arg(savedOrigin.x(), 0, 'f', 3)));
}

void PDFEditControllerTest::savesDeletedTextRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-delete-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-delete-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QString());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QString savedText = normalizedPageText(outputPath);
    QVERIFY(!savedText.isEmpty());
    QVERIFY(!savedText.contains(QString::fromLatin1(kRegularText)));
    QVERIFY(savedText.contains(QString::fromLatin1(kBoldText)));
}

void PDFEditControllerTest::savesAddedTextRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-add-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-add-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(!controller.addText(0, 0.2, 0.65).isEmpty());
    const QString addedText = QStringLiteral("Văn bản mới đã lưu");
    controller.updateSelectedTextPreview(addedText);
    controller.finishSelectedTextEdit();
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    const QString savedText = normalizedPageText(outputPath);
    QVERIFY2(
        savedText.contains(addedText),
        qPrintable(QStringLiteral("Saved page text: %1").arg(savedText)));

    const QString verificationPath = qEnvironmentVariable("TREPDF_SAVE_TEST_OUTPUT");
    if (!verificationPath.isEmpty()) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(outputPath, verificationPath));
    }
}

void PDFEditControllerTest::rendersAddedImageSnapshot()
{
    QImage source(8, 8, QImage::Format_ARGB32);
    source.fill(QColor(QStringLiteral("#2b5fd8")));
    for (int y = 0; y < source.height() / 2; ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor(QStringLiteral("#d86f4b")));
        }
    }

    DraftImageItem item;
    item.setWidth(32);
    item.setHeight(32);
    item.setSourceImage(QVariant::fromValue(source));
    source.fill(Qt::green);

    QImage rendered(32, 32, QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    item.paint(&painter);
    painter.end();

    const QColor topPixel = rendered.pixelColor(16, 4);
    const QColor bottomPixel = rendered.pixelColor(16, 28);
    QVERIFY(topPixel.red() > topPixel.blue());
    QVERIFY(bottomPixel.blue() > bottomPixel.red());
}

void PDFEditControllerTest::addsImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-image-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-image-preview.png"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#4b8fd8")));
    QVERIFY(image.save(imagePath, "PNG"));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::AddImageTool);

    const QPointF insertionPoint(0.2, 0.65);
    const QVariantMap added = controller.addImage(
        0,
        insertionPoint.x(),
        insertionPoint.y(),
        QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    QCOMPARE(added.value(QStringLiteral("kind")).toString(), QStringLiteral("addedImage"));
    QCOMPARE(added.value(QStringLiteral("added")).toBool(), true);
    QCOMPARE(added.value(QStringLiteral("imagePixelWidth")).toInt(), 80);
    QCOMPARE(added.value(QStringLiteral("imagePixelHeight")).toInt(), 40);
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    const QString objectId = added.value(QStringLiteral("objectId")).toString();
    const QRectF frame = added.value(QStringLiteral("frameRect")).toRectF();
    QVERIFY(frame.contains(insertionPoint));
    QVERIFY(frame.left() >= 0.0);
    QVERIFY(frame.top() >= 0.0);
    QVERIFY(frame.right() <= 1.0);
    QVERIFY(frame.bottom() <= 1.0);
    const QSizeF pageSize = document.pageSize(0);
    const qreal displayedAspect = frame.width() * pageSize.width()
        / (frame.height() * pageSize.height());
    QVERIFY(qAbs(displayedAspect - 2.0) < 0.001);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.textEditActive());
    QCOMPARE(
        controller.candidateAt(0, frame.center().x(), frame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);

    controller.undo();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());
    const QVariantList afterUndo = waitForCandidates(controller, 0);
    QVERIFY(std::none_of(
        afterUndo.cbegin(),
        afterUndo.cend(),
        [&objectId](const QVariant &value) {
            return value.toMap().value(QStringLiteral("objectId")).toString()
                == objectId;
        }));

    controller.redo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    const QVariantList afterRedo = waitForCandidates(controller, 0);
    const auto restored = std::find_if(
        afterRedo.cbegin(),
        afterRedo.cend(),
        [&objectId](const QVariant &value) {
            return value.toMap().value(QStringLiteral("objectId")).toString()
                == objectId;
        });
    QVERIFY(restored != afterRedo.cend());
    QCOMPARE(
        restored->toMap().value(QStringLiteral("frameRect")).toRectF(),
        frame);

    QVERIFY(controller.selectCandidate(0, objectId));
    const QRectF resizedFrame(
        frame.x() + 0.01,
        frame.y() + 0.01,
        frame.width() * 0.8,
        frame.height() * 0.8);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
    controller.finishSelectedFrameResize();
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
}

void PDFEditControllerTest::movesAddedImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-added-image-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-added-image.png"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#4b8fd8")));
    QVERIFY(image.save(imagePath, "PNG"));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap added = controller.addImage(
        0,
        0.2,
        0.55,
        QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    const QString objectId = added.value(QStringLiteral("objectId")).toString();
    const QRectF originalFrame = added.value(QStringLiteral("frameRect")).toRectF();
    const qreal movedX = originalFrame.right() + 0.03;
    QVERIFY(movedX + originalFrame.width() < 1.0);
    const QRectF movedFrame(
        movedX,
        originalFrame.y() + 0.08,
        originalFrame.width(),
        originalFrame.height());

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();

    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.selectedContentBounds(), movedFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);
    QCOMPARE(
        controller.candidateAt(0, movedFrame.center().x(), movedFrame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);
    QVERIFY(controller.candidateAt(
        0,
        originalFrame.center().x(),
        originalFrame.center().y()).value(QStringLiteral("objectId")).toString()
        != objectId);

    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
}

void PDFEditControllerTest::addsShapeWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-shape-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setShapeType(PDFEditController::ArrowShape);
    controller.setTool(PDFEditController::AddShapeTool);

    const QPointF startPoint(0.18, 0.42);
    const QPointF endPoint(0.62, 0.425);
    const QPointF constrainedEnd = controller.constrainedShapeEnd(
        0,
        startPoint,
        endPoint);
    QCOMPARE(constrainedEnd.x(), endPoint.x());
    QCOMPARE(constrainedEnd.y(), startPoint.y());
    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(controller.shapeDragIsValid(0, startPoint, endPoint));
    QVERIFY(!controller.shapeDragIsValid(
        0,
        startPoint,
        QPointF(
            startPoint.x() + 2.0 / pageSize.width(),
            startPoint.y())));
    const QVariantMap added = controller.addShape(
        0,
        startPoint,
        endPoint);
    QVERIFY(!added.isEmpty());
    QCOMPARE(
        added.value(QStringLiteral("kind")).toString(),
        QStringLiteral("addedShape"));
    QCOMPARE(
        added.value(QStringLiteral("shapeType")).toInt(),
        static_cast<int>(PDFEditController::ArrowShape));
    QCOMPARE(
        added.value(QStringLiteral("shapeStrokeColor")).value<QColor>(),
        QColor(Qt::black));
    QCOMPARE(
        added.value(QStringLiteral("shapeFillColor")).value<QColor>(),
        QColor(Qt::transparent));
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(controller.draftsForPage(0).size(), 1);

    const QString objectId = added
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF frame = added
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QPointF shapeStart = added
        .value(QStringLiteral("shapeStart"))
        .toPointF();
    const QPointF shapeEnd = added
        .value(QStringLiteral("shapeEnd"))
        .toPointF();
    QVERIFY(frame.contains(startPoint));
    QVERIFY(frame.contains(constrainedEnd));
    QVERIFY(qAbs(shapeStart.y() - 0.5) < 0.001);
    QVERIFY(qAbs(shapeEnd.y() - 0.5) < 0.001);
    const QPointF savedStart(
        frame.x() + shapeStart.x() * frame.width(),
        frame.y() + shapeStart.y() * frame.height());
    const QPointF savedEnd(
        frame.x() + shapeEnd.x() * frame.width(),
        frame.y() + shapeEnd.y() * frame.height());
    QVERIFY(QLineF(savedStart, startPoint).length() < 0.00001);
    QVERIFY(QLineF(savedEnd, constrainedEnd).length() < 0.00001);
    const qreal strokeWidthRatio = added
        .value(QStringLiteral("shapeStrokeWidthRatio"))
        .toReal();
    const qreal hitTolerancePoints = controller.shapeHitToleranceRatio(
        0,
        strokeWidthRatio) * pageSize.height();
    QVERIFY(qAbs(hitTolerancePoints - 3.0) < 0.001);
    const qreal strokeWidthPoints = qMax<qreal>(
        0.75,
        strokeWidthRatio * pageSize.height());
    const qreal lineLengthPoints = QLineF(
        QPointF(
            startPoint.x() * pageSize.width(),
            startPoint.y() * pageSize.height()),
        QPointF(
            constrainedEnd.x() * pageSize.width(),
            constrainedEnd.y() * pageSize.height())).length();
    const qreal arrowHalfWidth = qMin(
        qMax<qreal>(6.0, strokeWidthPoints * 4.0),
        lineLengthPoints * 0.35) * 0.55;
    QVERIFY(frame.height() * pageSize.height()
        >= 2.0 * (arrowHalfWidth + strokeWidthPoints / 2.0) - 0.001);
    QCOMPARE(controller.selectedObjectId(), objectId);
    QCOMPARE(
        controller.candidateAt(0, frame.center().x(), frame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);

    const QRectF resizedFrame(
        frame.x() + 0.04,
        frame.y() + 0.05,
        frame.width() * 0.75,
        frame.height() * 2.0);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);

    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);

    controller.deleteSelectedObject();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.candidateAt(
        0,
        resizedFrame.center().x(),
        resizedFrame.center().y()).isEmpty());

    controller.undo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);

    controller.setShapeType(PDFEditController::LineShape);
    controller.setTool(PDFEditController::AddShapeTool);
    const QVariantMap diagonalLine = controller.addShape(
        0,
        QPointF(0.10, 0.10),
        QPointF(0.50, 0.50));
    QVERIFY(!diagonalLine.isEmpty());
    const QString diagonalLineId = diagonalLine
        .value(QStringLiteral("objectId"))
        .toString();
    QCOMPARE(
        controller.candidateAt(0, 0.30, 0.30)
            .value(QStringLiteral("objectId")).toString(),
        diagonalLineId);
    QVERIFY(
        controller.candidateAt(0, 0.10, 0.50)
            .value(QStringLiteral("objectId")).toString()
        != diagonalLineId);

    controller.setShapeType(PDFEditController::DiamondShape);
    controller.setTool(PDFEditController::AddShapeTool);
    const QVariantMap transparentDiamond = controller.addShape(
        0,
        QPointF(0.65, 0.10),
        QPointF(0.85, 0.30));
    QVERIFY(!transparentDiamond.isEmpty());
    const QString transparentDiamondId = transparentDiamond
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(
        controller.candidateAt(0, 0.75, 0.20)
            .value(QStringLiteral("objectId")).toString()
        != transparentDiamondId);
    QCOMPARE(
        controller.candidateAt(0, 0.75, 0.10)
            .value(QStringLiteral("objectId")).toString(),
        transparentDiamondId);

    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.draftsForPage(0).isEmpty());
}

void PDFEditControllerTest::savesShapesRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-shapes-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-shapes-output.pdf"));
    QVERIFY(createOffsetCropPdf(sourcePath));
    const ShapeSaveInspection sourceInspection
        = inspectShapePaths(sourcePath);
    QVERIFY(sourceInspection.readable);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    struct ShapeInput {
        PDFEditController::ShapeType type;
        QPointF start;
        QPointF end;
    };
    const QVector<ShapeInput> shapes{
        {PDFEditController::RectangleShape, {0.08, 0.12}, {0.28, 0.24}},
        {PDFEditController::EllipseShape, {0.38, 0.12}, {0.58, 0.24}},
        {PDFEditController::LineShape, {0.68, 0.22}, {0.88, 0.12}},
        {PDFEditController::ArrowShape, {0.08, 0.38}, {0.28, 0.385}},
        {PDFEditController::TriangleShape, {0.38, 0.30}, {0.58, 0.42}},
        {PDFEditController::DiamondShape, {0.68, 0.30}, {0.88, 0.42}},
    };
    QVector<QPointF> expectedEnds;
    expectedEnds.reserve(shapes.size());
    for (const ShapeInput &shape : shapes) {
        controller.setShapeType(shape.type);
        controller.setTool(PDFEditController::AddShapeTool);
        expectedEnds.append(controller.constrainedShapeEnd(
            0,
            shape.start,
            shape.end));
        QVERIFY(!controller.addShape(
            0,
            shape.start,
            shape.end).isEmpty());
    }
    QCOMPARE(controller.draftsForPage(0).size(), shapes.size());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const ShapeSaveInspection savedInspection
        = inspectShapePaths(outputPath);
    QVERIFY(savedInspection.readable);
    QCOMPARE(
        savedInspection.count,
        sourceInspection.count + shapes.size());
    QVERIFY(savedInspection.filledCount
        >= sourceInspection.filledCount + 4);
    QVERIFY(savedInspection.openCount
        >= sourceInspection.openCount + 2);
    QVERIFY(savedInspection.bezierCount
        >= sourceInspection.bezierCount + 1);
    QVERIFY(savedInspection.blackStrokeCount
        >= sourceInspection.blackStrokeCount + shapes.size());
    QVERIFY(savedInspection.transparentFillCount
        >= sourceInspection.transparentFillCount + 4);
    const auto verifyOpenPathEndpoints = [&savedInspection,
                                          &sourceInspection,
                                          &shapes,
                                          &expectedEnds](
                                             qsizetype shapeIndex) {
        const qsizetype pathIndex = sourceInspection.count + shapeIndex;
        QVERIFY(pathIndex
            < savedInspection.normalizedPathPoints.size());
        const QVector<QPointF> &points
            = savedInspection.normalizedPathPoints.at(pathIndex);
        QVERIFY(points.size() >= 2);
        QVERIFY(QLineF(points.at(0), shapes.at(shapeIndex).start).length()
            < 0.002);
        QVERIFY(QLineF(points.at(1), expectedEnds.at(shapeIndex)).length()
            < 0.002);
    };
    verifyOpenPathEndpoints(2);
    verifyOpenPathEndpoints(3);

    const QString verificationPath = qEnvironmentVariable(
        "TREPDF_SHAPE_TEST_OUTPUT");
    if (!verificationPath.isEmpty()) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(outputPath, verificationPath));
    }
}

void PDFEditControllerTest::movesTopLevelTextWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-text-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kGroupedText));
    QVERIFY2(!textCandidate.isEmpty(), "Top-level text candidate was not discovered");
    const QString objectId = textCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF originalFrame = textCandidate
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const auto translationForInterval = [](qreal start, qreal end) {
        const qreal trailingSpace = 1.0 - end;
        if (trailingSpace >= 0.05) {
            return qMin<qreal>(0.08, trailingSpace);
        }
        return start >= 0.05
            ? -qMin<qreal>(0.08, start)
            : 0.0;
    };
    const QPointF translation(
        translationForInterval(originalFrame.left(), originalFrame.right()),
        translationForInterval(originalFrame.top(), originalFrame.bottom()));
    QVERIFY(!qFuzzyIsNull(translation.x()));
    QVERIFY(!qFuzzyIsNull(translation.y()));
    const QRectF movedFrame = originalFrame.translated(translation);

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    QVERIFY(controller.textEditActive());
    controller.updateSelectedFrameRect(movedFrame);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    controller.finishSelectedFrameResize();

    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    const QVariantList drafts = controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    QCOMPARE(
        drafts.constFirst().toMap()
            .value(QStringLiteral("sourceTextMoveOnly")).toBool(),
        true);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 0);
    const QVector<PDFEditController::DraftObjectMove> moves
        = controller.draftedObjectMovesForPage(0);
    QCOMPARE(moves.size(), 2);
    for (const PDFEditController::DraftObjectMove &move : moves) {
        QCOMPARE(move.kind, PDFEditController::DraftObjectMove::Kind::Text);
        QCOMPARE(move.sourceRect, originalFrame);
        QCOMPARE(move.frameRect, movedFrame);
        QCOMPARE(move.objectPath.size(), 1);
    }

    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);

    controller.redo();
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 2);

    controller.commitSelectedText(QStringLiteral("Moved and edited PDF text"));
    const QVariantList editedDrafts = controller.draftsForPage(0);
    QCOMPARE(editedDrafts.size(), 1);
    QCOMPARE(
        editedDrafts.constFirst().toMap()
            .value(QStringLiteral("sourceTextMoveOnly")).toBool(),
        false);
    QVERIFY(!controller.draftedTextObjectPathsForPage(0).isEmpty());
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);
}

void PDFEditControllerTest::movesTopLevelImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-image-source.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    const QString objectId = imageCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF originalFrame = imageCandidate
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const qreal movedX = qMax<qreal>(0.0, originalFrame.x() - 0.3);
    const qreal movedY = qMin<qreal>(
        1.0 - originalFrame.height(),
        originalFrame.y() + 0.12);
    const QRectF requestedFrame(
        movedX,
        movedY,
        originalFrame.width() * 0.5,
        originalFrame.height() * 0.5);
    const QRectF movedFrame(
        movedX,
        movedY,
        originalFrame.width(),
        originalFrame.height());
    QVERIFY(movedFrame != originalFrame);

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    QVERIFY(controller.textEditActive());
    controller.updateSelectedFrameRect(requestedFrame);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.selectedContentBounds(), movedFrame);
    controller.finishSelectedFrameResize();

    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 0);
    const QVector<PDFEditController::DraftObjectMove> moves
        = controller.draftedObjectMovesForPage(0);
    QCOMPARE(moves.size(), 1);
    QCOMPARE(moves.constFirst().kind, PDFEditController::DraftObjectMove::Kind::Image);
    QCOMPARE(moves.constFirst().sourceRect, originalFrame);
    QCOMPARE(moves.constFirst().frameRect, movedFrame);
    QCOMPARE(moves.constFirst().objectPath.size(), 1);
    QCOMPARE(
        controller.candidateAt(0, movedFrame.center().x(), movedFrame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);
    QVERIFY(controller.candidateAt(
        0,
        originalFrame.center().x(),
        originalFrame.center().y()).value(QStringLiteral("objectId")).toString()
        != objectId);

    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);

    controller.redo();
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 1);
}

void PDFEditControllerTest::deletesTopLevelImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-image-source.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    QCOMPARE(imageCandidate.value(QStringLiteral("editable")).toBool(), true);
    const QString objectId = imageCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QPointF hitPoint = imageCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .center();
    const auto containsImage = [&objectId](const QVariantList &candidates) {
        return std::any_of(
            candidates.cbegin(),
            candidates.cend(),
            [&objectId](const QVariant &candidateValue) {
                const QVariantMap candidate = candidateValue.toMap();
                return candidate.value(QStringLiteral("kind")).toString()
                        == QLatin1String("image")
                    && candidate.value(QStringLiteral("objectId")).toString()
                        == objectId;
            });
    };

    QCOMPARE(
        controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedKind(), QStringLiteral("image"));
    QVERIFY(controller.selectedEditable());

    controller.deleteSelectedObject();

    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(controller.selectedObjectId(), QString());
    const QVariantList deletionDrafts = controller.draftsForPage(0);
    QCOMPARE(deletionDrafts.size(), 1);
    QVERIFY(deletionDrafts.constFirst().toMap()
        .value(QStringLiteral("deleted"))
        .toBool());
    QCOMPARE(
        deletionDrafts.constFirst().toMap()
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
    QVERIFY(controller.allDraftedTextObjectsSuppressibleForPage(0));
    QVERIFY(!containsImage(waitForCandidates(controller, 0)));
    QVERIFY(controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
    QVERIFY(!controller.selectCandidate(0, objectId));

    controller.undo();

    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 0);
    QVERIFY(containsImage(waitForCandidates(controller, 0)));
    QCOMPARE(
        controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(controller.selectCandidate(0, objectId));

    controller.redo();

    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
    QCOMPARE(controller.selectedObjectId(), QString());
    QVERIFY(!containsImage(waitForCandidates(controller, 0)));
    QVERIFY(controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
}

void PDFEditControllerTest::savesAddedImageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-image-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-image-input.png"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-image-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#2b5fd8")));
    for (int y = 0; y < image.height() / 2; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor(QStringLiteral("#d86f4b")));
        }
    }
    QVERIFY(image.save(imagePath, "PNG"));
    QCOMPARE(inspectImages(sourcePath).count, 0);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap added = controller.addImage(
        0,
        0.45,
        0.35,
        QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    const QRectF frame = added.value(QStringLiteral("frameRect")).toRectF();
    QVERIFY(QFile::remove(imagePath));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const ImageSaveInspection inspection = inspectImages(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.pixelWidth, 80U);
    QCOMPARE(inspection.pixelHeight, 40U);
    QVERIFY(inspection.pixelsReadable);
    QVERIFY(inspection.topLeftPixel.red() > inspection.topLeftPixel.blue());
    QVERIFY(inspection.bottomLeftPixel.blue() > inspection.bottomLeftPixel.red());
    QCOMPARE(inspectImages(sourcePath).count, 0);

    const QVariantList savedCandidates = waitForCandidates(controller, 0);
    const auto savedImage = std::find_if(
        savedCandidates.cbegin(),
        savedCandidates.cend(),
        [](const QVariant &value) {
            return value.toMap().value(QStringLiteral("kind")).toString()
                == QLatin1String("image");
        });
    QVERIFY(savedImage != savedCandidates.cend());
    const QRectF savedRect = savedImage->toMap()
        .value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(qAbs(savedRect.x() - frame.x()) < 0.001);
    QVERIFY(qAbs(savedRect.y() - frame.y()) < 0.001);
    QVERIFY(qAbs(savedRect.width() - frame.width()) < 0.001);
    QVERIFY(qAbs(savedRect.height() - frame.height()) < 0.001);
}

void PDFEditControllerTest::savesMovedTopLevelTextRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-move-text-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-move-text-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    const QString text = QString::fromLatin1(kRegularText);
    FS_MATRIX sourceMatrix{};
    QVERIFY(textObjectMatrix(sourcePath, text, sourceMatrix));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        text);
    QVERIFY2(!textCandidate.isEmpty(), "Top-level text candidate was not discovered");
    const QRectF originalFrame = textCandidate
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF originalSourceRect = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const auto translationForInterval = [](qreal start, qreal end) {
        const qreal trailingSpace = 1.0 - end;
        if (trailingSpace >= 0.05) {
            return qMin<qreal>(0.08, trailingSpace);
        }
        return start >= 0.05
            ? -qMin<qreal>(0.08, start)
            : 0.0;
    };
    const QPointF translation(
        translationForInterval(originalFrame.left(), originalFrame.right()),
        translationForInterval(originalFrame.top(), originalFrame.bottom()));
    QVERIFY(!qFuzzyIsNull(translation.x()));
    QVERIFY(!qFuzzyIsNull(translation.y()));
    const QRectF movedFrame = originalFrame.translated(translation);
    QVERIFY(controller.selectCandidate(
        0,
        textCandidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();

    const QVariantList drafts = controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    QCOMPARE(
        drafts.constFirst().toMap()
            .value(QStringLiteral("sourceTextMoveOnly")).toBool(),
        true);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());

    FS_MATRIX savedMatrix{};
    QVERIFY(textObjectMatrix(outputPath, text, savedMatrix));
    QVERIFY(qAbs(savedMatrix.a - sourceMatrix.a) < 0.001F);
    QVERIFY(qAbs(savedMatrix.b - sourceMatrix.b) < 0.001F);
    QVERIFY(qAbs(savedMatrix.c - sourceMatrix.c) < 0.001F);
    QVERIFY(qAbs(savedMatrix.d - sourceMatrix.d) < 0.001F);
    QVERIFY(qAbs(savedMatrix.e - sourceMatrix.e) > 1.0F);
    QVERIFY(qAbs(savedMatrix.f - sourceMatrix.f) > 1.0F);
    QVERIFY(normalizedPageText(outputPath).contains(text));

    const QVariantMap savedCandidate = candidateForText(
        waitForCandidates(controller, 0),
        text);
    QVERIFY(!savedCandidate.isEmpty());
    const QRectF savedRect = savedCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QRectF expectedSavedRect = originalSourceRect.translated(translation);
    QVERIFY(qAbs(savedRect.x() - expectedSavedRect.x()) < 0.001);
    QVERIFY(qAbs(savedRect.y() - expectedSavedRect.y()) < 0.001);
    QVERIFY(qAbs(savedRect.width() - expectedSavedRect.width()) < 0.001);
    QVERIFY(qAbs(savedRect.height() - expectedSavedRect.height()) < 0.001);

    FS_MATRIX unchangedSourceMatrix{};
    QVERIFY(textObjectMatrix(sourcePath, text, unchangedSourceMatrix));
    QCOMPARE(unchangedSourceMatrix.a, sourceMatrix.a);
    QCOMPARE(unchangedSourceMatrix.b, sourceMatrix.b);
    QCOMPARE(unchangedSourceMatrix.c, sourceMatrix.c);
    QCOMPARE(unchangedSourceMatrix.d, sourceMatrix.d);
    QCOMPARE(unchangedSourceMatrix.e, sourceMatrix.e);
    QCOMPARE(unchangedSourceMatrix.f, sourceMatrix.f);
}

void PDFEditControllerTest::savesMovedTopLevelImageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-move-image-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-move-image-output.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    const ImageSaveInspection sourceInspection = inspectImages(sourcePath);
    QVERIFY(sourceInspection.readable);
    QVERIFY(sourceInspection.matrixReadable);
    QCOMPARE(sourceInspection.count, 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    const QRectF originalFrame = imageCandidate
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF movedFrame(
        qMax<qreal>(0.0, originalFrame.x() - 0.2),
        qMin<qreal>(1.0 - originalFrame.height(), originalFrame.y() + 0.1),
        originalFrame.width(),
        originalFrame.height());
    QVERIFY(movedFrame != originalFrame);
    QVERIFY(controller.selectCandidate(
        0,
        imageCandidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());

    const ImageSaveInspection savedInspection = inspectImages(outputPath);
    QVERIFY(savedInspection.readable);
    QVERIFY(savedInspection.matrixReadable);
    QCOMPARE(savedInspection.count, 1);
    QCOMPARE(savedInspection.firstTopLevelIndex, sourceInspection.firstTopLevelIndex);
    QCOMPARE(savedInspection.pixelWidth, sourceInspection.pixelWidth);
    QCOMPARE(savedInspection.pixelHeight, sourceInspection.pixelHeight);
    QCOMPARE(savedInspection.pixelsReadable, sourceInspection.pixelsReadable);
    QCOMPARE(savedInspection.topLeftPixel, sourceInspection.topLeftPixel);
    QCOMPARE(savedInspection.bottomLeftPixel, sourceInspection.bottomLeftPixel);
    QVERIFY(qAbs(savedInspection.matrix.a - sourceInspection.matrix.a) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.b - sourceInspection.matrix.b) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.c - sourceInspection.matrix.c) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.d - sourceInspection.matrix.d) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.e - sourceInspection.matrix.e) > 1.0F);
    QVERIFY(qAbs(savedInspection.matrix.f - sourceInspection.matrix.f) > 1.0F);
    QVERIFY(normalizedPageText(outputPath).contains(
        QString::fromLatin1(kImageFixtureText)));

    const QVariantMap savedCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY(!savedCandidate.isEmpty());
    const QRectF savedRect = savedCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    QVERIFY(qAbs(savedRect.x() - movedFrame.x()) < 0.001);
    QVERIFY(qAbs(savedRect.y() - movedFrame.y()) < 0.001);
    QVERIFY(qAbs(savedRect.width() - movedFrame.width()) < 0.001);
    QVERIFY(qAbs(savedRect.height() - movedFrame.height()) < 0.001);

    const ImageSaveInspection unchangedSourceInspection = inspectImages(sourcePath);
    QVERIFY(unchangedSourceInspection.matrixReadable);
    QCOMPARE(unchangedSourceInspection.matrix.e, sourceInspection.matrix.e);
    QCOMPARE(unchangedSourceInspection.matrix.f, sourceInspection.matrix.f);
}

void PDFEditControllerTest::savesDeletedTopLevelImageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-delete-image-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-delete-image-output.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    const ImageSaveInspection sourceBeforeSave = inspectImages(sourcePath);
    QVERIFY(sourceBeforeSave.readable);
    QCOMPARE(sourceBeforeSave.count, 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    QVERIFY(imageCandidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(controller.selectCandidate(
        0,
        imageCandidate.value(QStringLiteral("objectId")).toString()));
    controller.deleteSelectedObject();
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const ImageSaveInspection savedInspection = inspectImages(outputPath);
    QVERIFY(savedInspection.readable);
    QCOMPARE(savedInspection.count, 0);
    QVERIFY(normalizedPageText(outputPath).contains(
        QString::fromLatin1(kImageFixtureText)));

    const ImageSaveInspection unchangedSourceInspection = inspectImages(sourcePath);
    QVERIFY(unchangedSourceInspection.readable);
    QCOMPARE(unchangedSourceInspection.count, 1);
    QVERIFY(normalizedPageText(sourcePath).contains(
        QString::fromLatin1(kImageFixtureText)));
}

void PDFEditControllerTest::preservesAddedTextLayerOrder()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-added-order-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-added-order-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    for (int index = 1; index <= 12; ++index) {
        QVERIFY(!controller.addText(0, 0.2, 0.65).isEmpty());
        controller.updateSelectedTextPreview(
            QStringLiteral("Layer %1").arg(index));
        controller.finishSelectedTextEdit();
    }

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QStringList objectTexts = topLevelTextObjects(outputPath);
    QVERIFY(objectTexts.size() >= 22);
    const QStringList savedLayers = objectTexts.sliced(objectTexts.size() - 12);
    for (int index = 1; index <= 12; ++index) {
        QCOMPARE(
            savedLayers.at(index - 1).trimmed(),
            QStringLiteral("Layer %1").arg(index));
    }
}

void PDFEditControllerTest::savesRichMultilineTextRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-rich-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-rich-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(QStringLiteral("Red blue\nGreen"));
    controller.setSelectedTextRange(0, 3);
    controller.setSelectedFontBold(true);
    controller.setSelectedFontItalic(true);
    controller.setSelectedFontUnderline(true);
    controller.setSelectedColor(QColor(QStringLiteral("#c62828")));
    controller.finishSelectedTextEdit();

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(normalizedPageText(outputPath).contains(QStringLiteral("Red blue Green")));
    const RichSaveInspection inspection = inspectRichSave(outputPath);
    QVERIFY(inspection.readable);
    QVERIFY(inspection.redRun);
    QVERIFY(inspection.boldItalicRun);
    QVERIFY(inspection.underlinePath);
    QVERIFY(inspection.redAndBlueShareBaseline);
    QVERIFY(inspection.greenIsNextLine);
}

void PDFEditControllerTest::savesOffsetCropCoordinates()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-crop-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-crop-output.pdf"));
    QVERIFY(createOffsetCropPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QStringLiteral("Offset crop text"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(candidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Offset crop saved"));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QPointF origin;
    QVERIFY2(
        textObjectOrigin(outputPath, QStringLiteral("Offset crop"), origin),
        qPrintable(topLevelTextObjects(outputPath).join(QLatin1Char('|'))));
    QVERIFY(qAbs(origin.x() - 100.0) < 1.0);
    QVERIFY(qAbs(origin.y() - 600.0) < 1.0);
}

void PDFEditControllerTest::savesAddedTextOnRotatedPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-rotated-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-rotated-output.pdf"));
    QVERIFY(createRotatedPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(!controller.addText(0, 0.2, 0.65).isEmpty());
    controller.updateSelectedTextPreview(QStringLiteral("Rotated add"));
    controller.finishSelectedTextEdit();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    FS_MATRIX matrix{};
    QVERIFY(textObjectMatrix(outputPath, QStringLiteral("Rotated"), matrix));
    QVERIFY(qAbs(matrix.a) < 0.05);
    QVERIFY(qAbs(matrix.d) < 0.05);
    QVERIFY(qAbs(matrix.b) > 0.5);
    QVERIFY(qAbs(matrix.c) > 0.5);
}

void PDFEditControllerTest::preservesStrokeAppearanceAndRejectsClipping()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-render-mode-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-render-mode-output.pdf"));
    QVERIFY(createRenderModePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const QVariantMap strokeCandidate = candidateForText(
        candidates,
        QStringLiteral("Stroke text"));
    const QVariantMap clipCandidate = candidateForText(
        candidates,
        QStringLiteral("Clip text"));
    QVERIFY(strokeCandidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(!clipCandidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(controller.selectCandidate(
        0,
        strokeCandidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Stroke saved"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    QColor strokeColor;
    qreal strokeWidth = 0.0;
    QVERIFY(strokeTextAppearance(
        outputPath,
        QStringLiteral("Stroke"),
        strokeColor,
        strokeWidth));
    QCOMPARE(strokeColor, QColor(20, 80, 160));
    QVERIFY(qAbs(strokeWidth - 1.75) < 0.01);
}

void PDFEditControllerTest::savesAsynchronously()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-async-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-async-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    const QString expectedText = QStringLiteral("Lưu tiếng Việt");
    const QString editedText = expectedText.normalized(
        QString::NormalizationForm_D);
    controller.commitSelectedText(editedText);

    QSignalSpy saveFinishedSpy(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(controller.saving());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    controller.undo();
    controller.redo();
    controller.discardDrafts();
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 5000);
    QCOMPARE(saveFinishedSpy.constFirst().constFirst().toBool(), true);
    QVERIFY(!controller.saving());
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(normalizedPageText(outputPath).contains(expectedText));
    const TextObjectFontInfo editedFont = textObjectFontInfo(
        outputPath,
        QStringLiteral("Lưu"));
    QVERIFY(editedFont.found);
    QCOMPARE(editedFont.embedded, 1);
}

void PDFEditControllerTest::keepsDraftsWhenSaveFails()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-failure-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("This draft must survive"));
    const int revisionBeforeSave = document.renderRevision();
    QVERIFY(controller.canUndo());

    const QString invalidPath = m_temporaryDirectory.filePath(
        QStringLiteral("missing-directory/output.pdf"));
    QSignalSpy saveFinishedSpy(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(invalidPath)));
    QVERIFY(controller.saving());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 5000);
    QCOMPARE(saveFinishedSpy.constFirst().constFirst().toBool(), false);
    QVERIFY(!QFile::exists(invalidPath));
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.saving());
    QCOMPARE(document.source().toLocalFile(), sourcePath);
    QCOMPARE(document.renderRevision(), revisionBeforeSave);
    QVERIFY(pageText(sourcePath).contains(QString::fromLatin1(kRegularText)));
}

void PDFEditControllerTest::opensDocumentsInIndependentTabs()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("tabs-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("tabs-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    QVERIFY(firstSession);
    QCOMPARE(tabs.count(), 1);
    QCOMPARE(tabs.currentSession(), firstSession);

    firstSession->setZoom(1.75);
    firstSession->setScrollY(420.0);
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(secondSession);
    QVERIFY(secondSession != firstSession);
    QCOMPARE(tabs.count(), 2);
    QCOMPARE(tabs.currentIndex(), 1);
    QCOMPARE(tabs.currentSession(), secondSession);
    QCOMPARE(secondSession->zoom(), 1.0);
    QCOMPARE(secondSession->scrollY(), 0.0);

    const QString duplicatePath = QFileInfo(firstPath).dir().filePath(
        QStringLiteral("./tabs-first.pdf"));
    QCOMPARE(tabs.open(QUrl::fromLocalFile(duplicatePath)), firstSession);
    QCOMPARE(tabs.count(), 2);
    QCOMPARE(tabs.currentIndex(), 0);
    QCOMPARE(firstSession->zoom(), 1.75);
    QCOMPARE(firstSession->scrollY(), 420.0);
}

void PDFEditControllerTest::keepsViewRotationIndependentAcrossTabs()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("rotation-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("rotation-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(firstSession);
    QVERIFY(secondSession);

    QSignalSpy rotationSpy(firstSession, &PDFDocumentSession::viewRotationChanged);
    firstSession->rotateView(1);
    QCOMPARE(firstSession->viewRotation(), 90);
    QCOMPARE(secondSession->viewRotation(), 0);
    QCOMPARE(rotationSpy.count(), 1);

    firstSession->rotateView(-2);
    QCOMPARE(firstSession->viewRotation(), 270);
    QCOMPARE(rotationSpy.count(), 2);

    firstSession->setViewRotation(450);
    QCOMPARE(firstSession->viewRotation(), 90);
    QCOMPARE(rotationSpy.count(), 3);

    firstSession->setViewRotation(90);
    QCOMPARE(rotationSpy.count(), 3);
}

void PDFEditControllerTest::clearsTabIdentityWhenDocumentCloses()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-identity.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFTabModel tabs;
    PDFDocumentSession *session = tabs.open(QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);
    QCOMPARE(session->title(), QStringLiteral("tab-identity.pdf"));
    QCOMPARE(session->filePath(), sourcePath);

    session->document()->close();

    QVERIFY(session->title().isEmpty());
    QVERIFY(session->filePath().isEmpty());
}

void PDFEditControllerTest::keepsTabDraftsIndependent()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-draft-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-draft-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(firstSession);
    QVERIFY(secondSession);

    PDFEditController *firstController = firstSession->editController();
    const QVariantMap candidate = candidateForText(
        waitForCandidates(*firstController, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(firstController->selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    firstController->commitSelectedText(QStringLiteral("Draft in first tab"));

    QVERIFY(firstSession->hasDrafts());
    QVERIFY(!secondSession->hasDrafts());
    QVERIFY(tabs.hasDrafts());
    QVERIFY(!tabs.closeSession(firstSession));
    QCOMPARE(tabs.count(), 2);

    firstController->discardDrafts();
    QVERIFY(!firstSession->hasDrafts());
    QVERIFY(tabs.closeSession(firstSession));
    QCOMPARE(tabs.count(), 1);
    QCOMPARE(tabs.currentIndex(), 0);
    QCOMPARE(tabs.currentSession(), secondSession);
    QVERIFY(!tabs.hasDrafts());
}

void PDFEditControllerTest::savesOnlyRequestedTab()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-second.pdf"));
    const QString firstOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-first-output.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(firstSession);
    QVERIFY(secondSession);

    const auto addDraft = [](PDFDocumentSession *session, const QString &text) {
        PDFEditController *controller = session->editController();
        const QVariantMap candidate = candidateForText(
            waitForCandidates(*controller, 0),
            QString::fromLatin1(kRegularText));
        if (candidate.isEmpty()
            || !controller->selectCandidate(
                0,
                candidate.value(QStringLiteral("objectId")).toString())) {
            return false;
        }
        controller->commitSelectedText(text);
        return controller->hasDrafts();
    };
    QVERIFY(addDraft(firstSession, QStringLiteral("First tab draft")));
    QVERIFY(addDraft(secondSession, QStringLiteral("Second tab draft")));

    QSignalSpy saveFinishedSpy(&tabs, &PDFTabModel::sessionSaveFinished);
    QVERIFY(tabs.requestSaveAs(
        firstSession,
        QUrl::fromLocalFile(firstOutputPath)));
    QVERIFY(tabs.anySaving());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 5000);
    QCOMPARE(saveFinishedSpy.constFirst().at(0).value<PDFDocumentSession *>(),
             firstSession);
    QCOMPARE(saveFinishedSpy.constFirst().at(1).toBool(), true);
    QVERIFY(!tabs.anySaving());
    QVERIFY(!firstSession->hasDrafts());
    QVERIFY(secondSession->hasDrafts());
    QCOMPARE(firstSession->document()->source().toLocalFile(), firstOutputPath);
    QCOMPARE(secondSession->document()->source().toLocalFile(), secondPath);
    QVERIFY(normalizedPageText(firstOutputPath).contains(
        QStringLiteral("First tab draft")));
    QVERIFY(normalizedPageText(secondPath).contains(
        QString::fromLatin1(kRegularText)));
}

void PDFEditControllerTest::rejectsSaveAsOverAnotherOpenTab()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-as-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-as-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(firstSession);
    QVERIFY(secondSession);

    QSignalSpy statusSpy(&tabs, &PDFTabModel::statusMessage);
    QVERIFY(!tabs.requestSaveAs(firstSession, secondSession->document()->source()));
    QCOMPARE(statusSpy.count(), 1);
    QVERIFY(!firstSession->saving());
    QCOMPARE(firstSession->document()->source().toLocalFile(), firstPath);
    QCOMPARE(secondSession->document()->source().toLocalFile(), secondPath);
}

QTEST_MAIN(PDFEditControllerTest)

#include "tst_PDFEditController.moc"
