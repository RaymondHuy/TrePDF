#include "ApplicationFonts.h"
#include "DraftImageItem.h"
#include "FormFieldLayer.h"
#include "PDFAnnotationUtils.h"
#include "PDFDocument.h"
#include "PDFEditController.h"
#include "PDFFormController.h"
#include "PDFFormDesignerController.h"
#include "PDFFormAppearance.h"
#include "PDFFormView.h"
#include "PDFFormWorker.h"
#include "PDFPageItem.h"
#include "PDFSaveWriter.h"
#include "PDFTabModel.h"
#include "PDFWidgetAnnotationSuppression.h"
#include "PDFiumSynchronization.h"

#include <QElapsedTimer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickItemGrabResult>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRawFont>
#include <QRegularExpression>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSemaphore>
#include <QSignalSpy>
#include <QScopeGuard>
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
constexpr char kBottomEdgeText[] = "BOTTOM ABC";
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
constexpr char kLegacyListBoxFieldName[] = "LegacyOfficeList";
constexpr char kLegacyListBoxWidgetId[]
    = "listbox-field-22222222-2222-4222-8222-222222222222";
constexpr char kSourceTextMarkupAppearance[]
    = "q\n"
      "0.13 0.27 0.61 RG\n"
      "2 w\n"
      "7 11 m 133 11 l S\n"
      "% TrePDF source text markup appearance sentinel\n"
      "Q\n";
constexpr int kPdfButtonNoToggleToOff = 1 << 14;
constexpr int kPdfButtonRadio = 1 << 15;
constexpr int kPdfTextDoNotScroll = 1 << 23;
constexpr int kPdfTextComb = 1 << 24;

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

bool createLegacyTrePDFRadioButtonPdf(const QString &filePath)
{
    const QByteArray offContent(
        "q 1 1 1 rg 0 0 18 18 re f 0 0 0 RG 1 w "
        "2 2 14 14 re S Q");
    const QByteArray onContent(
        "q 1 1 1 rg 0 0 18 18 re f 0 0 0 RG 1 w "
        "2 2 14 14 re S 0 0 0 rg 6 6 6 6 re f Q");
    const auto appearanceObject = [](const QByteArray &content) {
        return QByteArray(
                   "<< /Type /XObject /Subtype /Form /FormType 1 "
                   "/BBox [0 0 18 18] /Resources << >> /Length ")
            + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n")
            + content
            + QByteArray("\nendstream");
    };
    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 595 842] /Resources << >> "
                "/Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 49152 "
                "/T (PreferredContact) /TU (Preferred contact) "
                "/NM (radio-field-11111111-1111-1111-1111-111111111111) "
                "/P 3 0 R /F 4 /Rect [100 600 118 618] "
                "/V /Yes /DV /Yes /AS /Yes "
                "/AP << /N << /Off 6 0 R /Yes 7 0 R >> >> "
                "/TrePDFApVersion 2 >>"),
            QByteArray(
                "<< /Fields [4 0 R] /NeedAppearances false >>"),
            appearanceObject(offContent),
            appearanceObject(onContent),
        });
}

bool createLegacyTrePDFListBoxPdf(const QString &filePath)
{
    const QByteArray appearanceContent(
        "q\n"
        "1 1 1 rg\n"
        "0 0 220 72 re f\n"
        "0.55 0.55 0.55 RG\n"
        "1 w\n"
        "0.5 0.5 219 71 re S\n"
        "Q\n");
    const QByteArray appearanceStream
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [0 0 220 72] /Resources << >> /Length ")
        + QByteArray::number(appearanceContent.size())
        + QByteArray(" >>\nstream\n")
        + appearanceContent
        + QByteArray("endstream");

    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 595 842] "
                "/Resources << /Font << /Helv 7 0 R >> >> "
                "/Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 0 "
                "/T (")
                + QByteArray(kLegacyListBoxFieldName)
                + QByteArray(") /TU (Legacy office list) /NM (")
                + QByteArray(kLegacyListBoxWidgetId)
                + QByteArray(") /P 3 0 R /F 4 "
                    "/Rect [80 620 300 692] "
                    "/Opt [(Alpha) (Beta) (Gamma)] "
                    "/V (Beta) /DV (Alpha) /I [1] /TI 0 "
                    "/DA (/Helv 12 Tf 0 g) "
                    "/AP << /N 6 0 R >> /TrePDFApVersion 1 >>"),
            QByteArray(
                "<< /Fields [4 0 R] /NeedAppearances false "
                "/DR << /Font << /Helv 7 0 R >> >> "
                "/DA (/Helv 12 Tf 0 g) >>"),
            appearanceStream,
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
        });
}

bool createDropdownDeleteRegressionPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R "
                "/TrePDFRegression 4 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 595 842] /Resources << >> >>"),
            QByteArray("(<<)"),
        });
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

bool createTrePDFDropdownWithExportValuesPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray(
                "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 360 480] /Resources << >> "
                "/Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Widget /FT /Ch "
                "/Ff 131072 /T (Transport) /TU (Transport choice) "
                "/NM (dropdown-field-11111111-1111-1111-1111-111111111111) "
                "/P 3 0 R /F 4 /Rect [20 400 180 430] "
                "/Opt [[(Airplane) (Airplane)] [(bike) (Bicycle)] "
                "[(car) (Car)]] /V (Airplane) /DV (Airplane) /I [0] "
                "/DA (/Helv 12 Tf 0 g) >>"),
            QByteArray(
                "<< /Fields [4 0 R] /NeedAppearances true "
                "/DA (/Helv 12 Tf 0 g) "
                "/DR << /Font << /Helv 6 0 R >> >> >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
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

QImage renderFirstPage(const QString &filePath,
                       const QSize &pixelSize,
                       int renderFlags = 0)
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

class TestablePDFPageItem final : public PDFPageItem
{
public:
    explicit TestablePDFPageItem(QQuickItem *parent = nullptr)
        : PDFPageItem(parent)
    {
    }

    void completeForTest()
    {
        if (!isComponentComplete()) {
            componentComplete();
        }
    }
};

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

QRect redPixelBounds(const QImage &image, const QRect &area = {})
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

bool addTransformedTextObject(FPDF_DOCUMENT document,
                              FPDF_PAGE page,
                              const QString &text,
                              const FS_MATRIX &matrix)
{
    FPDF_PAGEOBJECT textObject = FPDFPageObj_NewTextObj(
        document,
        "Helvetica",
        24.0F);
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
        matrix.a,
        matrix.b,
        matrix.c,
        matrix.d,
        matrix.e,
        matrix.f);
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

bool createBottomEdgeTextPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    FPDF_PAGE page = FPDFPage_New(document, 0, 200.0, 200.0);
    bool succeeded = page
        && addTextObject(
            document,
            page,
            "Helvetica",
            QString::fromLatin1(kBottomEdgeText),
            20.0,
            1.0)
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

bool createSquigglyAspectRatioPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    const QSizeF pageSizes[] = {
        QSizeF(300.0, 800.0),
        QSizeF(800.0, 300.0),
    };
    // Both pages use the same point-space glyph positions. The inter-glyph
    // gap is below the configured 1.25-glyph-height limit, while the old
    // mixed-axis normalized comparison split it only on the tall page.
    bool succeeded = true;
    for (qsizetype pageIndex = 0;
         pageIndex < std::size(pageSizes);
         ++pageIndex) {
        const QSizeF pageSize = pageSizes[pageIndex];
        FPDF_PAGE page = FPDFPage_New(
            document,
            static_cast<int>(pageIndex),
            pageSize.width(),
            pageSize.height());
        const bool pageSucceeded = page
            && addTextObject(document,
                             page,
                             "Helvetica",
                             QStringLiteral("A"),
                             40.0,
                             150.0)
            && addTextObject(document,
                             page,
                             "Helvetica",
                             QStringLiteral("B"),
                             72.0,
                             150.0)
            && FPDFPage_GenerateContent(page);
        if (page) {
            FPDF_ClosePage(page);
        }
        if (!pageSucceeded) {
            succeeded = false;
            break;
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

bool createStickyNotePopupPdf(const QString &filePath)
{
    const QByteArray appearanceContent(
        "q 1 0.92 0 rg 0 0 24 24 re f "
        "0.2 0.2 0.2 RG 1 w 3 7 m 21 7 l S Q");
    const QByteArray appearanceObject
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [0 0 24 24] /Resources << >> /Length ")
        + QByteArray::number(appearanceContent.size())
        + QByteArray(" >>\nstream\n")
        + appearanceContent
        + QByteArray("\nendstream");
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R 6 0 R 7 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [32 144 56 168] "
                "/Contents (Reverse-only popup comment) "
                "/T (Alice Example) "
                "/CreationDate (D:20240102030405+07'00') "
                "/NM (reverse-only-popup-comment) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Popup /P 3 0 R "
                "/Rect [64 72 184 152] /Parent 4 0 R /Open true /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [132 144 156 168] "
                "/Contents (Preserved popup comment) "
                "/RC (<body xmlns=\"http://www.w3.org/1999/xhtml\">"
                "<p>Preserved popup comment</p></body>) "
                "/T (Bob Example) "
                "/CreationDate (D:20240203040506+07'00') "
                "/M (D:20240304050607+07'00') "
                "/NM (preserved-popup-comment) /F 4 /Popup 7 0 R "
                "/AP << /N 8 0 R >> >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Popup /P 3 0 R "
                "/Rect [72 32 192 112] /Parent 6 0 R /Open false /F 4 >>"),
            appearanceObject,
        });
}

bool createTextMarkupCommentPdf(const QString &filePath,
                                const QByteArray &subtypeName,
                                const QByteArray &annotationName,
                                const QByteArray &contents)
{
    const QByteArray appearanceContent(kSourceTextMarkupAppearance);
    const QByteArray appearanceObject
        = QByteArray(
              "<< /Type /XObject /Subtype /Form /FormType 1 "
              "/BBox [0 0 140 22] /Resources << >> /Length ")
        + QByteArray::number(appearanceContent.size())
        + QByteArray(" >>\nstream\n")
        + appearanceContent
        + QByteArray("endstream");
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R] >>"),
            QByteArray("<< /Type /Annot /Subtype /")
                + subtypeName
                + QByteArray(
                    " /P 3 0 R /Rect [20 120 160 142] "
                    "/QuadPoints [20 142 160 142 20 120 160 120] "
                    "/Contents (")
                + contents
                + QByteArray(
                    ") /T (Markup Author) "
                    "/CreationDate (D:20240405060708+07'00') "
                    "/M (D:20240506070809+07'00') /NM (")
                + annotationName
                + QByteArray(
                    ") /C [1 0.9 0] /CA 0.55 /F 4 "
                    "/AP << /N 5 0 R >> >>"),
            appearanceObject,
        });
}

bool createProtectedCommentTextPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Read-only comment) "
                "/NM (read-only-comment) /F 64 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [60 144 84 168] /Contents (Locked comment) "
                "/NM (locked-contents-comment) /F 512 >>"),
        });
}

bool createProtectedFreeTextPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /Helv 6 0 R >> >> "
                "/Annots [4 0 R 5 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 3 0 R "
                "/Rect [20 120 90 160] /Contents (Read-only free text) "
                "/DA (/Helv 12 Tf 0 g) /NM (read-only-free-text) /F 64 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 3 0 R "
                "/Rect [105 120 180 160] /Contents (Locked free text) "
                "/DA (/Helv 12 Tf 0 g) /NM (locked-free-text) /F 512 >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                "/Encoding /WinAnsiEncoding >>"),
        });
}

bool createPageLocalDuplicateFreeTextNamesPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray(
                "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /Helv 7 0 R >> >> "
                "/Annots [5 0 R] >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /Helv 7 0 R >> >> "
                "/Annots [6 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 3 0 R "
                "/Rect [20 120 180 170] /Contents (Page zero text) "
                "/DA (/Helv 12 Tf 0 g) /NM (page-local-name) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 4 0 R "
                "/Rect [20 120 180 170] /Contents (Page one text) "
                "/DA (/Helv 12 Tf 0 g) /NM (page-local-name) /F 4 >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                "/Encoding /WinAnsiEncoding >>"),
        });
}

bool createNestedCommentRepliesPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R 6 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Root comment) "
                "/CreationDate (D:20240101090000Z) /NM (root-comment) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [52 144 76 168] /Contents (First reply) "
                "/CreationDate (D:20240101100000Z) /NM (first-reply) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [80 144 104 168] /Contents (Nested reply) "
                "/CreationDate (D:20240101110000Z) /NM (nested-reply) "
                "/IRT 5 0 R /RT /R /F 4 >>"),
        });
}

bool createZeroRectUnderlineReplyPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R 6 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Underline /P 3 0 R "
                "/Rect [20 120 180 132] "
                "/QuadPoints [20 132 180 132 20 120 180 120] "
                "/Contents (Root underline) "
                "/CreationDate (D:20240101090000Z) "
                "/NM (foxit-underline-root) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Foxit underline reply) "
                "/CreationDate (D:20240101100000Z) "
                "/NM (foxit-underline-reply) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [0 0 0 0] /Contents (Zero rect root) "
                "/CreationDate (D:20240101110000Z) "
                "/NM (zero-rect-root) /F 4 >>"),
        });
}

bool createRootCommentOnlyPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Root comment) "
                "/CreationDate (D:20240101090000Z) /NM (root-comment) /F 4 >>"),
        });
}

bool createUnsupportedRootCommentReplyPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Link /P 3 0 R "
                "/Rect [24 144 48 168] /Border [0 0 0] /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [52 144 76 168] /Contents (Unresolved reply) "
                "/NM (unresolved-reply) /IRT 4 0 R /RT /R /F 4 >>"),
        });
}

bool createMixedSubtypeCommentRepliesPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Version /2.0 /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /Helv 10 0 R >> >> "
                "/Annots [4 0 R 5 0 R 6 0 R 7 0 R 8 0 R 9 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Mixed root) "
                "/NM (mixed-root) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /FreeText /P 3 0 R "
                "/Rect [52 144 100 168] /Contents (FreeText reply) "
                "/DA (/Helv 10 Tf 0 g) /NM (mixed-free-text) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [104 144 128 168] "
                "/Contents (Text after FreeText) /NM (mixed-text) "
                "/IRT 5 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Stamp /P 3 0 R "
                "/Rect [24 100 64 128] /Contents (Stamp reply) "
                "/Name /Draft /NM (mixed-stamp) /IRT 4 0 R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Ink /P 3 0 R "
                "/Rect [72 96 112 128] /Contents (Ink reply) "
                "/InkList [[76 104 84 112 92 104]] /NM (mixed-ink) "
                "/IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Projection /P 3 0 R "
                "/Rect [120 96 160 128] /Contents (Projection reply) "
                "/NM (mixed-projection) /IRT 4 0 R /RT /R /F 4 >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                "/Encoding /WinAnsiEncoding >>"),
        });
}

bool createUnsupportedMarkupCommentsPdf(const QString &filePath)
{
    struct AnnotationDefinition {
        QByteArray subtype;
        QByteArray contents;
        QByteArray subtypeData;
    };
    const QVector<AnnotationDefinition> definitions{
        {"Line", "unsupported-line", "/L [20 520 120 540]"},
        {"Square", "unsupported-square", {}},
        {"Circle", "unsupported-circle", {}},
        {"Polygon", "unsupported-polygon",
         "/Vertices [20 400 120 400 70 430]"},
        {"PolyLine", "unsupported-polyline",
         "/Vertices [20 360 70 390 120 360]"},
        {"Stamp", "unsupported-stamp", "/Name /Draft"},
        {"Caret", "unsupported-caret", "/Sy /P"},
        {"Ink", "unsupported-ink",
         // Foxit commonly repeats consecutive samples in otherwise valid
         // Ink paths. Rehydration must remove those samples without making
         // the comment read-only.
         "/InkList [[24 274 24 274 60 288 60 288 112 276] "
         "[30 282 30 282 72 274 72 274]]"},
        {"FileAttachment", "unsupported-file-attachment",
         "/FS << /Type /Filespec /F (sample.txt) >>"},
        {"Sound", "unsupported-sound", "/Sound 16 0 R"},
        {"Redact", "unsupported-redact",
         "/QuadPoints [20 140 120 140 20 120 120 120]"},
        {"Projection", "unsupported-projection", {}},
    };

    QByteArray annotationReferences;
    for (qsizetype index = 0; index < definitions.size(); ++index) {
        annotationReferences += QByteArray::number(index + 4) + " 0 R ";
    }
    QVector<QByteArray> objects{
        QByteArray("<< /Type /Catalog /Version /2.0 /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 600] "
            "/Resources << >> /Annots [")
            + annotationReferences + QByteArray("] >>"),
    };
    for (qsizetype index = 0; index < definitions.size(); ++index) {
        const AnnotationDefinition &definition = definitions.at(index);
        const int top = 560 - static_cast<int>(index) * 38;
        const int bottom = top - 24;
        QByteArray annotation(
            "<< /Type /Annot /P 3 0 R /Subtype /");
        annotation += definition.subtype;
        annotation += " /Rect [20 ";
        annotation += QByteArray::number(bottom);
        annotation += " 120 ";
        annotation += QByteArray::number(top);
        annotation += "] /Contents (";
        annotation += definition.contents;
        annotation += ") /T (Markup author) ";
        annotation += "/CreationDate (D:20240101090000Z) ";
        annotation += "/M (D:20240101100000Z) /NM (unsupported-";
        annotation += QByteArray::number(index);
        annotation += ") /F 4 ";
        if (index < 5 || definition.subtype == QByteArrayLiteral("Ink")) {
            annotation += "/C [.1 .2 .3] /CA .75 "
                "/BS << /Type /Border /W 2 /S /S >> "
                "/AP << /N 17 0 R >> ";
        }
        annotation += definition.subtypeData;
        annotation += " >>";
        objects.append(std::move(annotation));
    }
    objects.append(QByteArray(
        "<< /Type /Sound /R 8000 /C 1 /B 8 /E /Unsigned /Length 0 >>\n"
        "stream\n\nendstream"));
    const QByteArray drawingAppearance(
        "q\n/GS gs\n.1 .2 .3 RG\n2 w\n0 0 m\n100 24 l\nS\nQ\n");
    objects.append(
        QByteArray(
            "<< /Type /XObject /Subtype /Form /FormType 1 "
            "/BBox [0 0 100 24] /Matrix [1 0 0 1 0 0] "
            "/Resources << /ExtGState << /GS << /Type /ExtGState "
            "/CA .75 /ca .75 /AIS false /BM /Normal >> >> >> /Length ")
        + QByteArray::number(drawingAppearance.size())
        + QByteArray(" >>\nstream\n")
        + drawingAppearance
        + QByteArray("endstream"));
    return writePdfObjects(filePath, objects);
}

bool createNonMarkupCommentReplyPdf(const QString &filePath)
{
    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << >> /Annots [4 0 R 5 0 R] >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Text /P 3 0 R "
                "/Rect [24 144 48 168] /Contents (Protected root) "
                "/NM (protected-root) /F 4 >>"),
            QByteArray(
                "<< /Type /Annot /Subtype /Link /P 3 0 R "
                "/Rect [52 144 100 168] /Border [0 0 0] "
                "/IRT 4 0 R /RT /R /F 4 >>"),
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

QRectF normalizedGlyphBoundsForText(const QString &filePath,
                                    const QString &text,
                                    int pageIndex = 0)
{
    PDFiumProcessLock pdfiumLock;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    FPDF_TEXTPAGE textPage = page
        ? FPDFText_LoadPage(page)
        : nullptr;

    QRectF bounds;
    if (textPage && !text.isEmpty()) {
        const int characterCount = FPDFText_CountChars(textPage);
        int textStart = -1;
        for (int index = 0;
             index + text.size() <= characterCount;
             ++index) {
            bool matches = true;
            for (qsizetype offset = 0; offset < text.size(); ++offset) {
                if (FPDFText_GetUnicode(
                        textPage,
                        index + static_cast<int>(offset))
                    != text.at(offset).unicode()) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                textStart = index;
                break;
            }
        }

        const qreal pageWidth = FPDF_GetPageWidthF(page);
        const qreal pageHeight = FPDF_GetPageHeightF(page);
        if (textStart >= 0 && pageWidth > 0.0 && pageHeight > 0.0) {
            for (qsizetype offset = 0; offset < text.size(); ++offset) {
                if (text.at(offset).isSpace()) {
                    continue;
                }
                double left = 0.0;
                double right = 0.0;
                double bottom = 0.0;
                double top = 0.0;
                if (!FPDFText_GetCharBox(
                        textPage,
                        textStart + static_cast<int>(offset),
                        &left,
                        &right,
                        &bottom,
                        &top)) {
                    continue;
                }
                const QRectF glyphRect(
                    left / pageWidth,
                    (pageHeight - top) / pageHeight,
                    (right - left) / pageWidth,
                    (top - bottom) / pageHeight);
                bounds = bounds.isEmpty()
                    ? glyphRect
                    : bounds.united(glyphRect);
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
    return bounds;
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

int topLevelPageObjectCount(const QString &filePath, int pageIndex = 0)
{
    PDFiumProcessLock pdfiumLock;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    const int objectCount = page ? FPDFPage_CountObjects(page) : -1;
    if (page) {
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return objectCount;
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
    QString author;
    QString creationDate;
    QString modificationDate;
    QString contents;
    bool hasAppearance = false;
    QString appearance;
    int appearanceObjectCount = 0;
    int appearancePathCount = 0;
};

struct StickyNoteSaveInspection {
    bool readable = false;
    int count = 0;
    QColor color;
    int flags = 0;
    FS_RECTF rect{};
    QString contents;
    QStringList names;
    QString author;
    QString creationDate;
    QString modificationDate;
};

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

struct CommentAnnotationInspection {
    bool readable = false;
    int annotationCount = 0;
    int annotationIndex = -1;
    int subtype = FPDF_ANNOT_UNKNOWN;
    FS_RECTF rect{};
    QString contents;
    QString richContents;
    QString name;
    QString author;
    QString subject;
    QString creationDate;
    QString modificationDate;
    QString defaultAppearance;
    int flags = 0;
    bool hasAppearance = false;
    QString appearance;
    int popupIndex = -1;
    bool popupParentMatches = false;
    int parentIndex = -1;
    QString parentName;
    QString relation;
    QVector<FS_QUADPOINTSF> quadPoints;
};

struct PdfDictionaryRange {
    qsizetype start = -1;
    qsizetype end = -1;

    bool isValid() const
    {
        return start >= 0 && end > start;
    }
};

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

struct PdfIndirectReference {
    int objectNumber = 0;
    int generation = 0;

    bool isValid() const
    {
        return objectNumber > 0 && generation >= 0;
    }
};

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

struct AnnotationAppearanceStructureInspection {
    bool readable = false;
    bool annotationFound = false;
    bool normalAppearanceIsIndirect = false;
    bool appearanceObjectFound = false;
    bool appearanceIsStream = false;
    bool typeIsXObject = false;
    bool subtypeIsForm = false;
    bool matrixIsIdentity = false;
    QVector<qreal> boundingBox;
    bool hasResources = false;
    bool hasExtGState = false;
    bool hasGraphicsState = false;
    bool graphicsStateTypeIsExtGState = false;
    bool blendModeIsNormal = false;
    bool alphaSourceIsFalse = false;
    qreal strokingOpacity = std::numeric_limits<qreal>::quiet_NaN();
    qreal nonStrokingOpacity = std::numeric_limits<qreal>::quiet_NaN();
};

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

CommentAnnotationInspection inspectCommentAnnotation(
    const QString &filePath,
    const QString &targetName,
    int pageIndex = 0)
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

struct AcroFormFieldInspection {
    bool readable = false;
    int formType = FORMTYPE_NONE;
    int widgetCount = 0;
    int fieldType = FPDF_FORMFIELD_UNKNOWN;
    int fieldFlags = 0;
    QString widgetId;
    QString name;
    QString value;
    QString defaultValue;
    QStringList options;
    QVector<int> selectedIndices;
    FS_RECTF rect{};
    bool hasAppearance = false;
    bool hasMaxLength = false;
    float maxLength = 0.0f;
    QString exportValue;
    bool checked = false;
    int annotationFlags = 0;
    int controlCount = -1;
    int controlIndex = -1;
    bool hasParent = false;
    bool hasLocalFieldType = false;
    bool hasLocalFieldFlags = false;
    bool hasLocalValue = false;
    bool hasAppearanceVersion = false;
    float appearanceVersion = 0.0f;
    QString appearanceStream;
    bool pointHit = false;
    bool focusHandled = false;
};

AcroFormFieldInspection inspectAcroFormField(
    const QString &filePath,
    const QString &targetFieldName = {},
    int pageIndex = 0,
    const QString &targetWidgetId = {})
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
            const QString widgetId = readWideString(
                [annotation](FPDF_WCHAR *buffer,
                             unsigned long byteCount) {
                    return FPDFAnnot_GetStringValue(
                        annotation,
                        "NM",
                        buffer,
                        byteCount);
                });
            if (inspection.fieldType == FPDF_FORMFIELD_UNKNOWN
                && (targetFieldName.isEmpty()
                    || fieldName == targetFieldName)
                && (targetWidgetId.isEmpty()
                    || widgetId == targetWidgetId)) {
                inspection.fieldType = FPDFAnnot_GetFormFieldType(
                    formHandle,
                    annotation);
                inspection.fieldFlags = FPDFAnnot_GetFormFieldFlags(
                    formHandle,
                    annotation);
                inspection.widgetId = widgetId;
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
                inspection.defaultValue = readWideString(
                    [annotation](FPDF_WCHAR *buffer,
                                 unsigned long byteCount) {
                        return FPDFAnnot_GetStringValue(
                            annotation,
                            "DV",
                            buffer,
                            byteCount);
                    });
                if (inspection.defaultValue.isEmpty()) {
                    FPDF_ANNOTATION parent = FPDFAnnot_GetLinkedAnnot(
                        annotation,
                        "Parent");
                    if (parent) {
                        inspection.defaultValue = readWideString(
                            [parent](FPDF_WCHAR *buffer,
                                     unsigned long byteCount) {
                                return FPDFAnnot_GetStringValue(
                                    parent,
                                    "DV",
                                    buffer,
                                    byteCount);
                            });
                        FPDFPage_CloseAnnot(parent);
                    }
                }
                if (inspection.fieldType == FPDF_FORMFIELD_RADIOBUTTON) {
                    float defaultChecked = 0.0f;
                    if (FPDFAnnot_GetNumberValue(
                            annotation,
                            PDFFormAppearance::kRadioButtonDefaultCheckedKey,
                            &defaultChecked)) {
                        inspection.defaultValue = defaultChecked != 0.0f
                            ? QStringLiteral("Yes")
                            : QStringLiteral("Off");
                    } else if (inspection.defaultValue.isEmpty()) {
                        inspection.defaultValue = QStringLiteral("Off");
                    }
                }
                const int optionCount = FPDFAnnot_GetOptionCount(
                    formHandle,
                    annotation);
                if (optionCount > 0) {
                    inspection.options.reserve(optionCount);
                    for (int optionIndex = 0;
                         optionIndex < optionCount;
                         ++optionIndex) {
                        inspection.options.append(readWideString(
                            [formHandle,
                             annotation,
                             optionIndex](FPDF_WCHAR *buffer,
                                          unsigned long byteCount) {
                                return FPDFAnnot_GetOptionLabel(
                                    formHandle,
                                    annotation,
                                    optionIndex,
                                    buffer,
                                    byteCount);
                            }));
                        if (FPDFAnnot_IsOptionSelected(
                                formHandle,
                                annotation,
                                optionIndex)) {
                            inspection.selectedIndices.append(optionIndex);
                        }
                    }
                }
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
                inspection.hasParent = FPDFAnnot_HasKey(
                    annotation,
                    "Parent");
                inspection.hasLocalFieldType = FPDFAnnot_HasKey(
                    annotation,
                    "FT");
                inspection.hasLocalFieldFlags = FPDFAnnot_HasKey(
                    annotation,
                    "Ff");
                inspection.hasLocalValue = FPDFAnnot_HasKey(
                    annotation,
                    "V");
                inspection.hasAppearanceVersion
                    = FPDFAnnot_GetNumberValue(
                        annotation,
                        PDFFormAppearance::kVersionKey,
                        &inspection.appearanceVersion);
                inspection.appearanceStream = readWideString(
                    [annotation](FPDF_WCHAR *buffer,
                                 unsigned long byteCount) {
                        return FPDFAnnot_GetAP(
                            annotation,
                            FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                            buffer,
                            byteCount);
                    });
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

QVector<bool> clickAcroFormFieldTwice(
    const QString &filePath,
    const QString &targetFieldName,
    int pageIndex = 0)
{
    PDFiumProcessLock pdfiumLock;
    QVector<bool> states;
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
    FPDF_PAGE page = document
        ? FPDF_LoadPage(document, pageIndex)
        : nullptr;
    if (formHandle && page) {
        FORM_OnAfterLoadPage(page, formHandle);
        const auto readWideString = [](auto getter) {
            const unsigned long byteCount = getter(nullptr, 0);
            if (byteCount < sizeof(FPDF_WCHAR)) {
                return QString();
            }
            QVector<FPDF_WCHAR> buffer(
                static_cast<qsizetype>(
                    byteCount / sizeof(FPDF_WCHAR)));
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
                reinterpret_cast<const char16_t *>(buffer.constData()),
                characterCount);
        };
        const int annotationCount = FPDFPage_GetAnnotCount(page);
        for (int index = 0; index < annotationCount; ++index) {
            FPDF_ANNOTATION annotation = FPDFPage_GetAnnot(page, index);
            if (!annotation) {
                continue;
            }
            const QString fieldName = readWideString(
                [formHandle, annotation](FPDF_WCHAR *buffer,
                                         unsigned long byteCount) {
                    return FPDFAnnot_GetFormFieldName(
                        formHandle,
                        annotation,
                        buffer,
                        byteCount);
                });
            FS_RECTF rect = {};
            if (FPDFAnnot_GetSubtype(annotation) != FPDF_ANNOT_WIDGET
                || fieldName != targetFieldName
                || !FPDFAnnot_GetRect(annotation, &rect)) {
                FPDFPage_CloseAnnot(annotation);
                continue;
            }

            const double centerX = (rect.left + rect.right) / 2.0;
            const double centerY = (rect.bottom + rect.top) / 2.0;
            states.append(FPDFAnnot_IsChecked(formHandle, annotation));
            for (int click = 0; click < 2; ++click) {
                FORM_OnFocus(
                    formHandle,
                    page,
                    0,
                    centerX,
                    centerY);
                FORM_OnLButtonDown(
                    formHandle,
                    page,
                    0,
                    centerX,
                    centerY);
                FORM_OnLButtonUp(
                    formHandle,
                    page,
                    0,
                    centerX,
                    centerY);
                states.append(FPDFAnnot_IsChecked(
                    formHandle,
                    annotation));
            }
            FPDFPage_CloseAnnot(annotation);
            break;
        }
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
    return states;
}

HighlightSaveInspection inspectTextMarkupAnnotations(
    const QString &filePath,
    FPDF_ANNOTATION_SUBTYPE subtype,
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
                                          int pageIndex = 0)
{
    return inspectTextMarkupAnnotations(
        filePath,
        FPDF_ANNOT_HIGHLIGHT,
        pageIndex);
}

StickyNoteSaveInspection inspectStickyNotes(const QString &filePath,
                                            int pageIndex = 0)
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

struct CommentReplyInspection {
    bool readable = false;
    int textAnnotationCount = 0;
    int rootIndex = -1;
    int replyIndex = -1;
    QString rootContents;
    QString replyContents;
    QString relation;
    QString parentThreadId;
    QString replyToThreadId;
};

CommentReplyInspection inspectCommentReply(const QString &filePath,
                                           int pageIndex = 0)
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
    int pageIndex = 0)
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

struct InkAnnotationInspection {
    bool readable = false;
    int count = 0;
    bool annotationFound = false;
    QVector<QVector<QPointF>> paths;
};

InkAnnotationInspection inspectInkAnnotation(
    const QString &filePath,
    const QString &targetName,
    int pageIndex = 0)
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
                                        int pageIndex = 0)
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
    void limitsTransientTextFieldInputLength();
    void storesTransientRadioButtons();
    void renamesDropdownOptionsWithoutLosingSelection();
    void limitsFormFieldPointerHitArea();
    void rendersMultilineFormFieldPreview();
    void movesFormFieldsWithoutResizing();
    void savesTextFieldAsAcroForm();
    void savesAndRehydratesCombField();
    void savesAndRehydratesCheckbox();
    void savesAndRehydratesRadioButton();
    void savesRehydratesAndMutatesRadioButtonGroup();
    void scopesRadioSnapshotsToAffectedGroups();
    void migratesLegacyMergedRadioButton();
    void savesRehydratesAndInteractsWithDropdown();
    void savesRehydratesAndInteractsWithListBox();
    void migratesLegacyListBoxWhenAddingMultiSelectListBox();
    void deletesRehydratedDropdownWithoutChangingPeer();
    void deletesDropdownWhenItsSourcePageIsDeleted();
    void rejectsDropdownExportDisplayPairs();
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
    void dropsAsyncResultsAfterControllerDestruction();
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
    void keepsTextMarkupColorsIndependent();
    void requiresSourceTextOnlyForSquiggly();
    void prioritizesPendingSquigglyCandidateScan();
    void resumesNormalCandidateScanAfterPendingSquigglyPriority();
    void discardsPendingSquigglyBeforeCandidateScanCompletes();
    void preservesTextEditorWhenPendingSquigglyCompletes();
    void remapsPendingSquigglyAcrossPageInsertionUndoRedo();
    void cancelsPendingSquigglyWhenSourcePageIsDeleted();
    void keepsSquigglyGapGroupingStableAcrossPageAspectRatios();
    void snapsSquigglyToSourceTextAndSavesQuadPoints();
    void clipsSquigglyGapAtPageBottom();
    void usesInvisibleOcrTextForSquiggly();
    void rejectsVerticalTextForSquiggly();
    void matchesSquigglyPreviewGeometryToPdfium();
    void matchesSquigglyPreviewRenderingToSavedPdfium();
    void addsUnderlineStrikeoutAndSquigglyWithUndoRedoAndDelete_data();
    void addsUnderlineStrikeoutAndSquigglyWithUndoRedoAndDelete();
    void addsStickyNoteWithPanelModelAndUndo();
    void addsFreeTextWithPanelModelAndUndo();
    void addsCommentReplyWithOneLevelModelAndUndo();
    void editsSourceStickyNoteComment();
    void editsSourceTextMarkupComment_data();
    void editsSourceTextMarkupComment();
    void editsSourceReplyComment();
    void importsZeroRectUnderlineReply();
    void rejectsEditingProtectedSourceComments();
    void protectsSourceFreeTextComments();
    void enforcesCommentTextLengthLimit();
    void boundsSharedAnnotationStringReads();
    void flattensNestedSourceCommentReplies();
    void hidesCommentRepliesFromViewer();
    void hidesUnresolvedSourceReplyFromViewerModel();
    void indexesDrawingAndUnsupportedMarkupComments();
    void ownsPendingDrawingInteractionAcrossPages();
    void rejectsInvalidDrawingCommentGeometry_data();
    void rejectsInvalidDrawingCommentGeometry();
    void savesDrawingCommentsRoundTrip_data();
    void savesDrawingCommentsRoundTrip();
    void savesInkCommentRoundTrip();
    void editsRepliesAndDeletesSourceDrawingCommentsRoundTrip_data();
    void editsRepliesAndDeletesSourceDrawingCommentsRoundTrip();
    void deletesMixedSubtypeSourceReplyThread();
    void rejectsNonMarkupReplyDescendantDeletion();
    void deletesSourceCommentsWithUndoRedoDiscardAndSave();
    void deletesSourceCommentPopupRoundTrip();
    void savesHighlightAnnotationRoundTrip();
    void savesUnderlineStrikeoutAndSquigglyAnnotationRoundTrip_data();
    void savesUnderlineStrikeoutAndSquigglyAnnotationRoundTrip();
    void savesStickyNoteRoundTrip();
    void savesAndEditsFreeTextRoundTrip();
    void savesFreeTextWithPageLocalDuplicateNames();
    void savesCommentReplyRoundTrip();
    void normalizesCommentContentsLineEndingsOnSave();
    void keepsStickyNoteNamesUniqueAcrossReopen();
    void keepsMarkupReplyIdsUniqueAcrossReopen();
    void indexesCommentsAcrossDocument();
    void reindexesCommentsAfterDiscardingPageDeletion();
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

    QVERIFY(controller.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required
            | PDFFormDesignerController::Multiline));
    QVERIFY(controller.setFieldMaxLength(fieldId, 6));
    controller.setTextFieldText(fieldId, QStringLiteral("ab\ncdef"));
    QVariantMap configuredTextField = controller.fieldData(fieldId);
    QCOMPARE(
        configuredTextField.value(QStringLiteral("flags")).toInt(),
        PDFFormDesignerController::Required
            | PDFFormDesignerController::Multiline);
    QCOMPARE(
        configuredTextField.value(QStringLiteral("maxLength")).toInt(),
        6);
    QCOMPARE(
        configuredTextField.value(QStringLiteral("value")).toString(),
        QStringLiteral("ab\ncde"));
    QVERIFY(controller.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));
    QCOMPARE(
        controller.fieldData(fieldId)
            .value(QStringLiteral("value")).toString(),
        QStringLiteral("ab cde"));
    QVERIFY(controller.setFieldMaxLength(fieldId, 0));
    QCOMPARE(
        controller.fieldData(fieldId)
            .value(QStringLiteral("maxLength")).toInt(),
        0);

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

void PDFEditControllerTest::limitsTransientTextFieldInputLength()
{
    constexpr int maximumInputLength = 300000;
    PDFFormDesignerController controller;
    QCOMPARE(
        controller.maximumTextFieldInputLength(),
        maximumInputLength);

    const QString fieldId = controller.addTextField(
        0,
        0.1,
        0.1,
        0.4,
        0.2);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(controller.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Multiline));

    QString oversizedValue(
        maximumInputLength + 1,
        QLatin1Char('a'));
    oversizedValue[100] = QLatin1Char('\n');
    controller.setTextFieldText(fieldId, oversizedValue);

    const QString acceptedValue = controller.fieldData(fieldId)
                                      .value(QStringLiteral("value"))
                                      .toString();
    QCOMPARE(acceptedValue.size(), maximumInputLength);
    QCOMPARE(acceptedValue.at(100), QLatin1Char('\n'));
    QCOMPARE(
        controller.fieldData(fieldId)
            .value(QStringLiteral("maxLength")).toInt(),
        0);

    QString surrogateBoundaryValue(
        maximumInputLength - 1,
        QLatin1Char('b'));
    surrogateBoundaryValue.append(
        QString::fromUtf8("\xF0\x9F\x98\x80"));
    QCOMPARE(
        surrogateBoundaryValue.size(),
        maximumInputLength + 1);
    controller.setTextFieldText(fieldId, surrogateBoundaryValue);
    const QString surrogateSafeValue = controller.fieldData(fieldId)
                                           .value(QStringLiteral("value"))
                                           .toString();
    QCOMPARE(
        surrogateSafeValue.size(),
        maximumInputLength - 1);
    QVERIFY(!surrogateSafeValue.at(
                 surrogateSafeValue.size() - 1).isHighSurrogate());

    QVERIFY(controller.setFieldMaxLength(
        fieldId,
        maximumInputLength));
    QVERIFY(!controller.setFieldMaxLength(
        fieldId,
        maximumInputLength + 1));
}

void PDFEditControllerTest::storesTransientRadioButtons()
{
    PDFFormDesignerController controller;
    const QRectF initialRect(0.16, 0.22, 0.05, 0.05);
    const QString fieldId = controller.addField(
        PDFFormDesignerController::RadioButton,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());

    QVERIFY(fieldId.startsWith(QStringLiteral("radio-field-")));
    QCOMPARE(controller.rowCount(), 1);
    QVERIFY(controller.hasDrafts());
    QVariantMap field = controller.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(field.value(QStringLiteral("value")).toString(),
             QStringLiteral("Off"));
    QCOMPARE(field.value(QStringLiteral("options")).toStringList(),
             QStringList({QStringLiteral("Yes"), QStringLiteral("Off")}));
    QCOMPARE(field.value(QStringLiteral("rect")).toRectF(), initialRect);

    QVector<PDFFormDesignerController::SaveField> savedFields
        = controller.saveFields();
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().type,
             PDFFormDesignerController::RadioButton);
    QCOMPARE(savedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(savedFields.constFirst().value, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().defaultValue, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().normalizedRect, initialRect);

    QVERIFY(!controller.setFieldValue(fieldId, QStringLiteral("On")));
    QVERIFY(controller.setFieldValue(fieldId, QStringLiteral("Yes")));
    field = controller.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    savedFields = controller.saveFields();
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().type,
             PDFFormDesignerController::RadioButton);
    QCOMPARE(savedFields.constFirst().value, QStringLiteral("Yes"));
    QCOMPARE(savedFields.constFirst().defaultValue, QStringLiteral("Off"));

    QVERIFY(controller.removeField(fieldId));
    QCOMPARE(controller.rowCount(), 0);
    QVERIFY(controller.fieldData(fieldId).isEmpty());
    QVERIFY(controller.fieldsForPage(0).isEmpty());
    QVERIFY(controller.saveFields().isEmpty());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::renamesDropdownOptionsWithoutLosingSelection()
{
    PDFFormDesignerController controller;
    const QString fieldId = controller.addField(
        PDFFormDesignerController::Dropdown,
        0,
        0.1,
        0.2,
        0.4,
        0.08);
    QVERIFY(!fieldId.isEmpty());

    const QStringList options({
        QStringLiteral("Alpha"),
        QStringLiteral("Beta"),
        QStringLiteral("Gamma"),
    });
    QVERIFY(controller.setFieldOptions(fieldId, options));
    QVERIFY(controller.setFieldValue(fieldId, QStringLiteral("Beta")));

    QVERIFY(controller.renameFieldOption(
        fieldId,
        1,
        QStringLiteral("Beta renamed")));
    QVariantMap field = controller.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("options")).toStringList(),
             QStringList({QStringLiteral("Alpha"),
                          QStringLiteral("Beta renamed"),
                          QStringLiteral("Gamma")}));
    QCOMPARE(field.value(QStringLiteral("value")).toString(),
             QStringLiteral("Beta renamed"));

    QVERIFY(controller.renameFieldOption(
        fieldId,
        0,
        QStringLiteral("Alpha renamed")));
    const QVector<PDFFormDesignerController::SaveField> savedFields
        = controller.saveFields();
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().defaultValue,
             QStringLiteral("Alpha renamed"));
    QCOMPARE(savedFields.constFirst().value,
             QStringLiteral("Beta renamed"));

    QVERIFY(!controller.renameFieldOption(
        fieldId,
        1,
        QStringLiteral("Gamma")));
    QVERIFY(!controller.renameFieldOption(fieldId, 1, QString{}));
    QVERIFY(!controller.renameFieldOption(
        fieldId,
        3,
        QStringLiteral("Out of range")));
    field = controller.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("options")).toStringList(),
             QStringList({QStringLiteral("Alpha renamed"),
                          QStringLiteral("Beta renamed"),
                          QStringLiteral("Gamma")}));
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

    controller.selectTextField(fieldId);
    QCOMPARE(fieldLayer.activeFieldId(), fieldId);
    QCOMPARE(fieldLayer.activeFieldMaxLength(), 0);
    QVERIFY(controller.setFieldMaxLength(fieldId, 12));
    QCOMPARE(fieldLayer.activeFieldMaxLength(), 12);
    controller.clearTextFieldSelection();
    QCOMPARE(fieldLayer.activeFieldId(), QString());
    QCOMPARE(fieldLayer.activeFieldMaxLength(), 0);
    controller.selectTextField(fieldId);
    QCOMPARE(fieldLayer.activeFieldMaxLength(), 12);

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

void PDFEditControllerTest::rendersMultilineFormFieldPreview()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The form preview needs a Qt Quick render screen.");
    }

    PDFFormDesignerController controller;
    const QString fieldId = controller.addTextField(
        0,
        0.05,
        0.08,
        0.9,
        0.84);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(controller.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Multiline));
    QVERIFY(controller.setFieldValue(
        fieldId,
        QStringLiteral("Alpha\nBeta")));

    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(320, 120);

    TestableFormFieldLayer fieldLayer;
    fieldLayer.setParentItem(window.contentItem());
    fieldLayer.setWidth(window.width());
    fieldLayer.setHeight(window.height());
    fieldLayer.setController(&controller);
    fieldLayer.setPage(0);
    fieldLayer.setFieldsVisible(true);

    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    const QSharedPointer<QQuickItemGrabResult> grabResult
        = fieldLayer.grabToImage(window.size());
    QVERIFY(grabResult);
    QSignalSpy readySpy(
        grabResult.data(),
        &QQuickItemGrabResult::ready);
    if (grabResult->image().isNull()) {
        QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() > 0, 5000);
    }
    const QImage preview = grabResult->image();
    QVERIFY(!preview.isNull());

    int firstTextRow = preview.height();
    int lastTextRow = -1;
    for (int y = 8; y < preview.height() - 8; ++y) {
        bool rowContainsText = false;
        for (int x = 24; x < preview.width() - 24; ++x) {
            const QColor pixel = preview.pixelColor(x, y);
            if (pixel.red() < 100
                && pixel.green() < 100
                && pixel.blue() < 100) {
                rowContainsText = true;
                break;
            }
        }
        if (rowContainsText) {
            firstTextRow = qMin(firstTextRow, y);
            lastTextRow = qMax(lastTextRow, y);
        }
    }
    QVERIFY(firstTextRow < preview.height() / 3);
    QVERIFY(lastTextRow - firstTextRow >= 16);
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
        PDFFormDesignerController::Required
            | PDFFormDesignerController::Multiline));
    QVERIFY(formDesigner.setFieldMaxLength(fieldId, 32));
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
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_TEXT_MULTILINE);
    QVERIFY(inspection.hasMaxLength);
    QCOMPARE(inspection.maxLength, 32.0f);
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
    QVERIFY(!secondInspection.hasMaxLength);

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

void PDFEditControllerTest::savesAndRehydratesCombField()
{
    constexpr int defaultCombLength = 5;
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-comb-field.pdf"));
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

    const QString fieldId = formDesigner.addCombField(
        0,
        0.16,
        0.24,
        0.50,
        0.07);
    QVERIFY(!fieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("VerificationCode")));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("ABCDE")));

    QVariantMap field = formDesigner.fieldData(fieldId);
    QCOMPARE(
        field.value(QStringLiteral("type")).toInt(),
        PDFFormDesignerController::Text);
    QVERIFY(
        field.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Comb);
    QVERIFY(
        !(field.value(QStringLiteral("flags")).toInt()
          & PDFFormDesignerController::Multiline));
    QCOMPARE(
        field.value(QStringLiteral("maxLength")).toInt(),
        defaultCombLength);

    QVERIFY(!formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Comb
            | PDFFormDesignerController::Multiline));
    field = formDesigner.fieldData(fieldId);
    QVERIFY(
        field.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Comb);
    QVERIFY(
        !(field.value(QStringLiteral("flags")).toInt()
          & PDFFormDesignerController::Multiline));

    QVERIFY(formDesigner.setFieldMaxLength(fieldId, 0));
    field = formDesigner.fieldData(fieldId);
    QCOMPARE(field.value(QStringLiteral("maxLength")).toInt(), 0);
    QVERIFY(
        !(field.value(QStringLiteral("flags")).toInt()
          & PDFFormDesignerController::Comb));
    QVERIFY(formDesigner.setFieldMaxLength(
        fieldId,
        defaultCombLength));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Comb));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    field = formDesigner.fieldData(fieldId);
    QCOMPARE(
        field.value(QStringLiteral("type")).toInt(),
        PDFFormDesignerController::Text);
    QVERIFY(field.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!field.value(QStringLiteral("dirty")).toBool());
    QVERIFY(
        field.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Comb);
    QVERIFY(
        !(field.value(QStringLiteral("flags")).toInt()
          & PDFFormDesignerController::Multiline));
    QCOMPARE(
        field.value(QStringLiteral("maxLength")).toInt(),
        defaultCombLength);

    const AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("VerificationCode"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_TEXTFIELD);
    QVERIFY(!inspection.hasParent);
    QVERIFY(inspection.hasLocalFieldType);
    QVERIFY(inspection.hasLocalFieldFlags);
    QVERIFY(inspection.fieldFlags & kPdfTextComb);
    QVERIFY(inspection.fieldFlags & kPdfTextDoNotScroll);
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_TEXT_MULTILINE));
    QVERIFY(inspection.hasMaxLength);
    QCOMPARE(inspection.maxLength, float(defaultCombLength));
    QCOMPARE(inspection.name, QStringLiteral("VerificationCode"));
    QCOMPARE(inspection.value, QStringLiteral("ABCDE"));
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.pointHit);
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

void PDFEditControllerTest::savesAndRehydratesRadioButton()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-saved.pdf"));
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

    const QRectF initialRect(0.18, 0.26, 0.06, 0.06);
    const auto normalizedRectsMatch = [](const QRectF &actual,
                                         const QRectF &expected) {
        constexpr qreal tolerance = 0.0001;
        return qAbs(actual.x() - expected.x()) < tolerance
            && qAbs(actual.y() - expected.y()) < tolerance
            && qAbs(actual.width() - expected.width()) < tolerance
            && qAbs(actual.height() - expected.height()) < tolerance;
    };
    const QString fieldId = formDesigner.addField(
        PDFFormDesignerController::RadioButton,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());
    QVERIFY(fieldId.startsWith(QStringLiteral("radio-field-")));
    QCOMPARE(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"));
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("PreferredContact")));
    QVERIFY(formDesigner.setFieldValue(fieldId, QStringLiteral("Yes")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));

    QVector<PDFFormDesignerController::SaveField> savedFields
        = formDesigner.saveFields();
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().type,
             PDFFormDesignerController::RadioButton);
    QCOMPARE(savedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(savedFields.constFirst().value, QStringLiteral("Yes"));
    QCOMPARE(savedFields.constFirst().defaultValue, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().normalizedRect, initialRect);

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSaveAs(QUrl::fromLocalFile(savedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QCOMPARE(document.source().toLocalFile(), savedPath);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const AcroFormFieldInspection untouchedSource
        = inspectAcroFormField(sourcePath);
    QVERIFY(untouchedSource.readable);
    QCOMPARE(untouchedSource.widgetCount, 0);

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    QVERIFY(normalizedRectsMatch(
        hydrated.value(QStringLiteral("rect")).toRectF(),
        initialRect));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(formDesigner.sourceFieldIdsForPage(0).contains(fieldId));
    QVERIFY(formDesigner.sourceTextFieldIdsForPage(0).isEmpty());
    QVERIFY(formDesigner.sourceFieldContainsPoint(0, initialRect.center()));

    AcroFormFieldInspection inspection = inspectAcroFormField(
        savedPath,
        QStringLiteral("PreferredContact"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(inspection.name, QStringLiteral("PreferredContact"));
    QCOMPARE(inspection.value, fieldId);
    QCOMPARE(inspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(inspection.exportValue, fieldId);
    QVERIFY(inspection.checked);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QCOMPARE(
        inspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearanceStream.isEmpty());
    QVERIFY(inspection.appearanceStream.contains("0 0 0 RG"));
    QVERIFY(inspection.appearanceStream.contains("0 0 0 rg"));
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));
    QCOMPARE(inspection.controlCount, 1);
    QCOMPARE(inspection.controlIndex, 0);
    QVERIFY(inspection.hasParent);
    QVERIFY(!inspection.hasLocalFieldType);
    QVERIFY(!inspection.hasLocalFieldFlags);
    QVERIFY(!inspection.hasLocalValue);
    QVERIFY(inspection.pointHit);
    QCOMPARE(
        clickAcroFormFieldTwice(
            savedPath,
            QStringLiteral("PreferredContact")),
        QVector<bool>({true, true, true}));
    QVERIFY(qAbs(inspection.rect.left
                 - initialRect.left() * 595.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.right
                 - initialRect.right() * 595.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.top
                 - (1.0 - initialRect.top()) * 842.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom
                 - (1.0 - initialRect.bottom()) * 842.0)
            < 1.0);

    formDesigner.prepareForDesignMode();
    const QString renamedGroupId
        = QStringLiteral("PreferredContactRenamed");
    QVERIFY(formDesigner.setFieldName(fieldId, renamedGroupId));
    QVERIFY(formDesigner.setFieldValue(fieldId, QStringLiteral("Off")));
    const QRectF movedRect(0.38,
                           0.34,
                           initialRect.width(),
                           initialRect.height());
    QVERIFY(formDesigner.moveField(
        fieldId,
        movedRect.x(),
        movedRect.y()));
    QVERIFY(normalizedRectsMatch(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("rect"))
            .toRectF(),
        movedRect));
    const QRectF finalRect(0.42, 0.38, 0.08, 0.07);
    QVERIFY(formDesigner.setFieldRect(
        fieldId,
        finalRect.x(),
        finalRect.y(),
        finalRect.width(),
        finalRect.height()));

    savedFields = formDesigner.saveFields(true);
    QCOMPARE(savedFields.size(), 1);
    QCOMPARE(savedFields.constFirst().type,
             PDFFormDesignerController::RadioButton);
    QCOMPARE(savedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(savedFields.constFirst().changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::NamePropertyChanged
                 | PDFFormDesignerController::ValuePropertyChanged
                 | PDFFormDesignerController::RectPropertyChanged));
    QCOMPARE(savedFields.constFirst().name, renamedGroupId);
    QCOMPARE(savedFields.constFirst().value, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().defaultValue, QStringLiteral("Off"));
    QCOMPARE(savedFields.constFirst().normalizedRect, finalRect);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Off"));
    QCOMPARE(hydrated.value(QStringLiteral("name")).toString(),
             renamedGroupId);
    QVERIFY(normalizedRectsMatch(
        hydrated.value(QStringLiteral("rect")).toRectF(),
        finalRect));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(formDesigner.sourceFieldContainsPoint(0, finalRect.center()));
    QVERIFY(!formDesigner.sourceFieldContainsPoint(0, initialRect.center()));

    inspection = inspectAcroFormField(
        savedPath,
        renamedGroupId);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(inspection.name, renamedGroupId);
    QCOMPARE(inspection.value, QStringLiteral("Off"));
    QCOMPARE(inspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(inspection.exportValue, fieldId);
    QVERIFY(!inspection.checked);
    QCOMPARE(
        inspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearanceStream.isEmpty());
    QVERIFY(inspection.hasParent);
    QVERIFY(!inspection.hasLocalFieldType);
    QVERIFY(!inspection.hasLocalFieldFlags);
    QVERIFY(!inspection.hasLocalValue);
    QCOMPARE(
        clickAcroFormFieldTwice(
            savedPath,
            renamedGroupId),
        QVector<bool>({false, true, true}));
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));
    QVERIFY(qAbs(inspection.rect.left
                 - finalRect.left() * 595.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.right
                 - finalRect.right() * 595.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.top
                 - (1.0 - finalRect.top()) * 842.0)
            < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom
                 - (1.0 - finalRect.bottom()) * 842.0)
            < 1.0);
}

void PDFEditControllerTest::savesRehydratesAndMutatesRadioButtonGroup()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-group-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-group-saved.pdf"));
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

    const QString groupId = QStringLiteral("PreferredChannel");
    const QRectF firstRect(0.14, 0.24, 0.06, 0.06);
    const QRectF secondRect(0.32, 0.24, 0.06, 0.06);
    const QString firstId = formDesigner.addField(
        PDFFormDesignerController::RadioButton,
        0,
        firstRect.x(),
        firstRect.y(),
        firstRect.width(),
        firstRect.height());
    const QString secondId = formDesigner.addField(
        PDFFormDesignerController::RadioButton,
        0,
        secondRect.x(),
        secondRect.y(),
        secondRect.width(),
        secondRect.height());
    QVERIFY(firstId.startsWith(QStringLiteral("radio-field-")));
    QVERIFY(secondId.startsWith(QStringLiteral("radio-field-")));
    QVERIFY(firstId != secondId);
    QVERIFY(formDesigner.setFieldName(firstId, groupId));
    QVERIFY(formDesigner.setFieldName(secondId, groupId));
    QVERIFY(formDesigner.setFieldFlags(
        firstId,
        PDFFormDesignerController::Required));

    QVERIFY(formDesigner.setFieldValue(firstId, QStringLiteral("Yes")));
    QCOMPARE(
        formDesigner.fieldData(firstId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"));
    QCOMPARE(
        formDesigner.fieldData(secondId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"));
    QVERIFY(formDesigner.setFieldValue(secondId, QStringLiteral("Yes")));
    QCOMPARE(
        formDesigner.fieldData(firstId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"));
    QCOMPARE(
        formDesigner.fieldData(secondId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"));

    const auto savedFieldForId = [](
        const QVector<PDFFormDesignerController::SaveField> &fields,
        const QString &fieldId)
        -> const PDFFormDesignerController::SaveField * {
        const auto field = std::find_if(
            fields.cbegin(),
            fields.cend(),
            [&fieldId](const PDFFormDesignerController::SaveField &candidate) {
                return candidate.id == fieldId;
            });
        return field != fields.cend() ? &*field : nullptr;
    };

    QVector<PDFFormDesignerController::SaveField> savedFields
        = formDesigner.saveFields();
    QCOMPARE(savedFields.size(), 2);
    const PDFFormDesignerController::SaveField *firstSaved
        = savedFieldForId(savedFields, firstId);
    const PDFFormDesignerController::SaveField *secondSaved
        = savedFieldForId(savedFields, secondId);
    QVERIFY(firstSaved);
    QVERIFY(secondSaved);
    QCOMPARE(firstSaved->type, PDFFormDesignerController::RadioButton);
    QCOMPARE(secondSaved->type, PDFFormDesignerController::RadioButton);
    QCOMPARE(firstSaved->operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(secondSaved->operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(firstSaved->name, groupId);
    QCOMPARE(secondSaved->name, groupId);
    QCOMPARE(firstSaved->value, QStringLiteral("Off"));
    QCOMPARE(secondSaved->value, QStringLiteral("Yes"));
    QCOMPARE(firstSaved->defaultValue, QStringLiteral("Off"));
    QCOMPARE(secondSaved->defaultValue, QStringLiteral("Off"));
    QCOMPARE(firstSaved->flags,
             static_cast<int>(PDFFormDesignerController::Required));
    QCOMPARE(secondSaved->flags,
             static_cast<int>(PDFFormDesignerController::Required));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSaveAs(QUrl::fromLocalFile(savedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QCOMPARE(document.source().toLocalFile(), savedPath);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const AcroFormFieldInspection untouchedSource
        = inspectAcroFormField(sourcePath);
    QVERIFY(untouchedSource.readable);
    QCOMPARE(untouchedSource.widgetCount, 0);

    QVariantMap firstHydrated = formDesigner.fieldData(firstId);
    QVariantMap secondHydrated = formDesigner.fieldData(secondId);
    QCOMPARE(firstHydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(secondHydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(firstHydrated.value(QStringLiteral("name")).toString(), groupId);
    QCOMPARE(secondHydrated.value(QStringLiteral("name")).toString(), groupId);
    QCOMPARE(firstHydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Off"));
    QCOMPARE(secondHydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    QVERIFY(firstHydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(secondHydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!firstHydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(!secondHydrated.value(QStringLiteral("dirty")).toBool());

    AcroFormFieldInspection firstInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        firstId);
    AcroFormFieldInspection secondInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        secondId);
    QVERIFY(firstInspection.readable);
    QVERIFY(secondInspection.readable);
    QCOMPARE(firstInspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(firstInspection.widgetCount, 2);
    QCOMPARE(secondInspection.widgetCount, 2);
    QCOMPARE(firstInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(secondInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(firstInspection.widgetId, firstId);
    QCOMPARE(secondInspection.widgetId, secondId);
    QCOMPARE(firstInspection.name, groupId);
    QCOMPARE(secondInspection.name, groupId);
    QCOMPARE(firstInspection.value, secondId);
    QCOMPARE(secondInspection.value, secondId);
    QCOMPARE(firstInspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(secondInspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(firstInspection.exportValue, firstId);
    QCOMPARE(secondInspection.exportValue, secondId);
    QVERIFY(!firstInspection.checked);
    QVERIFY(secondInspection.checked);
    QCOMPARE(
        firstInspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QCOMPARE(
        secondInspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QVERIFY(firstInspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(secondInspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(firstInspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(secondInspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(firstInspection.hasAppearance);
    QVERIFY(secondInspection.hasAppearance);
    QVERIFY(!firstInspection.appearanceStream.isEmpty());
    QVERIFY(!secondInspection.appearanceStream.isEmpty());
    QVERIFY(firstInspection.hasAppearanceVersion);
    QVERIFY(secondInspection.hasAppearanceVersion);
    QCOMPARE(firstInspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));
    QCOMPARE(secondInspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));
    QVERIFY(firstInspection.hasParent);
    QVERIFY(secondInspection.hasParent);
    QVERIFY(!firstInspection.hasLocalFieldType);
    QVERIFY(!secondInspection.hasLocalFieldType);
    QVERIFY(!firstInspection.hasLocalFieldFlags);
    QVERIFY(!secondInspection.hasLocalFieldFlags);
    QVERIFY(!firstInspection.hasLocalValue);
    QVERIFY(!secondInspection.hasLocalValue);
    QCOMPARE(firstInspection.controlCount, 2);
    QCOMPARE(secondInspection.controlCount, 2);
    QVERIFY(firstInspection.controlIndex >= 0);
    QVERIFY(secondInspection.controlIndex >= 0);
    QVERIFY(firstInspection.controlIndex != secondInspection.controlIndex);

    formDesigner.prepareForDesignMode();
    QVERIFY(formDesigner.setFieldValue(firstId, QStringLiteral("Yes")));
    QCOMPARE(
        formDesigner.fieldData(firstId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Yes"));
    QCOMPARE(
        formDesigner.fieldData(secondId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Off"));
    const QRectF movedFirstRect(0.20, 0.34, 0.08, 0.07);
    QVERIFY(formDesigner.moveField(
        firstId,
        movedFirstRect.x(),
        movedFirstRect.y()));
    QVERIFY(formDesigner.setFieldRect(
        firstId,
        movedFirstRect.x(),
        movedFirstRect.y(),
        movedFirstRect.width(),
        movedFirstRect.height()));

    savedFields = formDesigner.saveFields(true);
    QCOMPARE(savedFields.size(), 2);
    firstSaved = savedFieldForId(savedFields, firstId);
    secondSaved = savedFieldForId(savedFields, secondId);
    QVERIFY(firstSaved);
    QVERIFY(secondSaved);
    QCOMPARE(firstSaved->operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(secondSaved->operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(firstSaved->value, QStringLiteral("Yes"));
    QCOMPARE(secondSaved->value, QStringLiteral("Off"));
    QVERIFY(firstSaved->changedProperties
            & PDFFormDesignerController::ValuePropertyChanged);
    QVERIFY(firstSaved->changedProperties
            & PDFFormDesignerController::RectPropertyChanged);
    QCOMPARE(secondSaved->changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::ValuePropertyChanged));

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    firstInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        firstId);
    secondInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        secondId);
    QCOMPARE(firstInspection.value, firstId);
    QCOMPARE(secondInspection.value, firstId);
    QCOMPARE(firstInspection.exportValue, firstId);
    QCOMPARE(secondInspection.exportValue, secondId);
    QVERIFY(firstInspection.checked);
    QVERIFY(!secondInspection.checked);
    QCOMPARE(firstInspection.controlCount, 2);
    QCOMPARE(secondInspection.controlCount, 2);
    QVERIFY(firstInspection.controlIndex != secondInspection.controlIndex);
    QVERIFY(qAbs(firstInspection.rect.left
                 - movedFirstRect.left() * 595.0)
            < 1.0);
    QVERIFY(qAbs(firstInspection.rect.right
                 - movedFirstRect.right() * 595.0)
            < 1.0);
    QVERIFY(qAbs(firstInspection.rect.top
                 - (1.0 - movedFirstRect.top()) * 842.0)
            < 1.0);
    QVERIFY(qAbs(firstInspection.rect.bottom
                 - (1.0 - movedFirstRect.bottom()) * 842.0)
            < 1.0);

    QVERIFY(formDesigner.removeField(secondId));
    QCOMPARE(formDesigner.rowCount(), 1);
    QVERIFY(formDesigner.hasDrafts());
    savedFields = formDesigner.saveFields();
    QCOMPARE(savedFields.size(), 2);
    firstSaved = savedFieldForId(savedFields, firstId);
    secondSaved = savedFieldForId(savedFields, secondId);
    QVERIFY(firstSaved);
    QVERIFY(secondSaved);
    QCOMPARE(firstSaved->operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(firstSaved->changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::NoPropertyChanges));
    QCOMPARE(secondSaved->operation,
             PDFFormDesignerController::SaveField::Remove);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QVERIFY(formDesigner.fieldData(secondId).isEmpty());
    firstHydrated = formDesigner.fieldData(firstId);
    QCOMPARE(firstHydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::RadioButton);
    QCOMPARE(firstHydrated.value(QStringLiteral("name")).toString(), groupId);
    QCOMPARE(firstHydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Yes"));
    QVERIFY(firstHydrated.value(QStringLiteral("sourceBacked")).toBool());
    const QStringList sourceFieldIds
        = formDesigner.sourceFieldIdsForPage(0);
    QCOMPARE(sourceFieldIds.size(), 1);
    QCOMPARE(sourceFieldIds.constFirst(), firstId);

    firstInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        firstId);
    QCOMPARE(firstInspection.widgetCount, 1);
    QCOMPARE(firstInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(firstInspection.value, firstId);
    QCOMPARE(firstInspection.defaultValue, QStringLiteral("Off"));
    QCOMPARE(firstInspection.exportValue, firstId);
    QVERIFY(firstInspection.checked);
    QCOMPARE(firstInspection.controlCount, 1);
    QCOMPARE(firstInspection.controlIndex, 0);
    QVERIFY(firstInspection.hasAppearance);
    QVERIFY(firstInspection.hasParent);
    const AcroFormFieldInspection removedInspection = inspectAcroFormField(
        savedPath,
        groupId,
        0,
        secondId);
    QCOMPARE(removedInspection.widgetCount, 1);
    QCOMPARE(removedInspection.fieldType, FPDF_FORMFIELD_UNKNOWN);
}

void PDFEditControllerTest::scopesRadioSnapshotsToAffectedGroups()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-scope-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-radio-scope-saved.pdf"));
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

    const QString firstGroup = QStringLiteral("FirstRadioGroup");
    const QString secondGroup = QStringLiteral("SecondRadioGroup");
    const auto addRadio = [&formDesigner](const QRectF &rect,
                                          const QString &groupName) {
        const QString id = formDesigner.addField(
            PDFFormDesignerController::RadioButton,
            0,
            rect.x(),
            rect.y(),
            rect.width(),
            rect.height());
        return !id.isEmpty() && formDesigner.setFieldName(id, groupName)
            ? id
            : QString();
    };
    const QString firstGroupFirst = addRadio(
        QRectF(0.12, 0.20, 0.05, 0.05),
        firstGroup);
    const QString firstGroupSecond = addRadio(
        QRectF(0.22, 0.20, 0.05, 0.05),
        firstGroup);
    const QString secondGroupFirst = addRadio(
        QRectF(0.12, 0.34, 0.05, 0.05),
        secondGroup);
    const QString secondGroupSecond = addRadio(
        QRectF(0.22, 0.34, 0.05, 0.05),
        secondGroup);
    QVERIFY(!firstGroupFirst.isEmpty());
    QVERIFY(!firstGroupSecond.isEmpty());
    QVERIFY(!secondGroupFirst.isEmpty());
    QVERIFY(!secondGroupSecond.isEmpty());

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSaveAs(QUrl::fromLocalFile(savedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 4, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const auto sortedFieldIds = [](
        const QVector<PDFFormDesignerController::SaveField> &fields) {
        QStringList ids;
        ids.reserve(fields.size());
        for (const PDFFormDesignerController::SaveField &field : fields) {
            ids.append(field.id);
        }
        ids.sort();
        return ids;
    };
    const auto sortedIds = [](QStringList ids) {
        ids.sort();
        return ids;
    };

    formDesigner.prepareForDesignMode();
    QVERIFY(formDesigner.moveField(firstGroupFirst, 0.16, 0.24));
    QVector<PDFFormDesignerController::SaveField> savedFields
        = formDesigner.saveFields();
    QCOMPARE(
        sortedFieldIds(savedFields),
        sortedIds({firstGroupFirst, firstGroupSecond}));

    formDesigner.resetFromDocument();
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 4, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    formDesigner.prepareForDesignMode();
    QVERIFY(formDesigner.setFieldName(firstGroupFirst, secondGroup));
    savedFields = formDesigner.saveFields();
    QCOMPARE(
        sortedFieldIds(savedFields),
        sortedIds({
            firstGroupFirst,
            firstGroupSecond,
            secondGroupFirst,
            secondGroupSecond}));

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 4, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const AcroFormFieldInspection firstGroupInspection
        = inspectAcroFormField(
            savedPath,
            firstGroup,
            0,
            firstGroupSecond);
    QVERIFY(firstGroupInspection.readable);
    QCOMPARE(firstGroupInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(firstGroupInspection.controlCount, 1);

    const AcroFormFieldInspection secondGroupInspection
        = inspectAcroFormField(
            savedPath,
            secondGroup,
            0,
            firstGroupFirst);
    QVERIFY(secondGroupInspection.readable);
    QCOMPARE(secondGroupInspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(secondGroupInspection.controlCount, 3);
}

void PDFEditControllerTest::migratesLegacyMergedRadioButton()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("legacy-merged-radio.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("migrated-radio.pdf"));
    QVERIFY(createLegacyTrePDFRadioButtonPdf(sourcePath));

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("PreferredContact"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QVERIFY(inspection.checked);
    QCOMPARE(inspection.defaultValue, QStringLiteral("Yes"));
    QVERIFY(!inspection.hasParent);
    QCOMPARE(inspection.appearanceVersion, 2.0f);

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
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);

    const QString fieldId
        = QStringLiteral(
            "radio-field-11111111-1111-1111-1111-111111111111");
    QCOMPARE(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("type"))
            .toInt(),
        PDFFormDesignerController::RadioButton);
    formDesigner.prepareForDesignMode();
    QVERIFY(formDesigner.setFieldValue(fieldId, QStringLiteral("Off")));
    QVector<PDFFormDesignerController::SaveField> radioFields
        = formDesigner.saveFields(true);
    QCOMPARE(radioFields.size(), 1);
    QCOMPARE(radioFields.constFirst().defaultValue,
             QStringLiteral("Yes"));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSaveAs(QUrl::fromLocalFile(savedPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    const AcroFormFieldInspection untouchedSource = inspectAcroFormField(
        sourcePath,
        QStringLiteral("PreferredContact"));
    QVERIFY(!untouchedSource.hasParent);
    QCOMPARE(untouchedSource.appearanceVersion, 2.0f);

    inspection = inspectAcroFormField(
        savedPath,
        QStringLiteral("PreferredContact"));
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_RADIOBUTTON);
    QCOMPARE(inspection.value, QStringLiteral("Off"));
    QCOMPARE(inspection.defaultValue, QStringLiteral("Yes"));
    QVERIFY(!inspection.checked);
    QCOMPARE(
        inspection.fieldFlags
            & (kPdfButtonNoToggleToOff | kPdfButtonRadio),
        kPdfButtonRadio);
    QVERIFY(inspection.hasParent);
    QVERIFY(!inspection.hasLocalFieldType);
    QVERIFY(!inspection.hasLocalFieldFlags);
    QVERIFY(!inspection.hasLocalValue);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(
                 PDFFormAppearance::kRadioButtonVersion));

    formDesigner.prepareForDesignMode();
    const QRectF hydratedRect = formDesigner.fieldData(fieldId)
                                    .value(QStringLiteral("rect"))
                                    .toRectF();
    QVERIFY(formDesigner.setFieldRect(
        fieldId,
        hydratedRect.x() + 0.01,
        hydratedRect.y(),
        hydratedRect.width(),
        hydratedRect.height()));
    radioFields = formDesigner.saveFields();
    QCOMPARE(radioFields.size(), 1);
    QCOMPARE(radioFields.constFirst().defaultValue,
             QStringLiteral("Yes"));
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    inspection = inspectAcroFormField(
        savedPath,
        QStringLiteral("PreferredContact"));
    QCOMPARE(inspection.value, QStringLiteral("Off"));
    QCOMPARE(inspection.defaultValue, QStringLiteral("Yes"));
    QVERIFY(!inspection.checked);
    QCOMPARE(
        clickAcroFormFieldTwice(
            savedPath,
            QStringLiteral("PreferredContact")),
        QVector<bool>({false, true, true}));
}

void PDFEditControllerTest::savesRehydratesAndInteractsWithDropdown()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-dropdown.pdf"));
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

    const QStringList options({
        QStringLiteral("Tùy chọn 1"),
        QStringLiteral("Tùy chọn 2"),
        QStringLiteral("Tùy chọn 3"),
    });
    const QRectF initialRect(0.16, 0.22, 0.40, 0.06);
    const QString fieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());
    QVERIFY(fieldId.startsWith(QStringLiteral("dropdown-field-")));
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("Transport")));
    QVERIFY(formDesigner.setFieldOptions(fieldId, options));
    QVERIFY(formDesigner.setFieldValue(
        fieldId,
        QStringLiteral("Tùy chọn 2")));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));

    const QVector<PDFFormDesignerController::SaveField> addedFields
        = formDesigner.saveFields();
    QCOMPARE(addedFields.size(), 1);
    QCOMPARE(addedFields.constFirst().type,
             PDFFormDesignerController::Dropdown);
    QCOMPARE(addedFields.constFirst().options, options);
    QCOMPARE(addedFields.constFirst().value,
             QStringLiteral("Tùy chọn 2"));
    QCOMPARE(addedFields.constFirst().defaultValue,
             QStringLiteral("Tùy chọn 1"));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Dropdown);
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             options);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Tùy chọn 2"));
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());
    QVERIFY(formDesigner.sourceFieldIdsForPage(0).contains(fieldId));
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        initialRect.center()));

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("Transport"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_COMBO);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_EDIT));
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT));
    QCOMPARE(inspection.name, QStringLiteral("Transport"));
    QCOMPARE(inspection.value, QStringLiteral("Tùy chọn 2"));
    QCOMPARE(inspection.defaultValue, QStringLiteral("Tùy chọn 1"));
    QCOMPARE(inspection.options, options);
    QCOMPARE(inspection.selectedIndices, QVector<int>({1}));
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.appearanceStream.contains(
        QStringLiteral("/Im0 Do")));
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kDropdownVersion));
    QVERIFY(inspection.pointHit);

    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    QVector<PDFFormWorker::ChoiceField> choiceFields;
    bool inspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&choiceFields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> fields) {
            choiceFields = std::move(fields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(choiceFields.size(), 1);
    const PDFFormWorker::ChoiceField choice = choiceFields.constFirst();
    QCOMPARE(choice.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(choice.options, options);
    QCOMPARE(choice.selectedIndices, QVector<int>({1}));
    QVERIFY(!choice.readOnly);
    QVERIFY(!choice.multiSelect);
    QVERIFY(!choice.identityToken.isEmpty());
    QVERIFY(formDesigner.managesSourceChoiceField(
        choice.sourcePage,
        choice.annotationIndex,
        choice.identityToken));
    QVERIFY(!formDesigner.managesSourceChoiceField(
        choice.sourcePage,
        choice.annotationIndex,
        choice.identityToken + QStringLiteral("-stale")));
    QVERIFY(formDesigner.managesSourceWidget(
        0,
        choice.annotationIndex,
        fieldId));
    QVERIFY(!formDesigner.managesSourceWidget(
        0,
        choice.annotationIndex,
        fieldId + QStringLiteral("-stale")));
    QVERIFY(!formDesigner.managesSourceWidget(
        0,
        choice.annotationIndex + 1,
        fieldId));

    editController.setFormDesignerController(nullptr);
    const QVariantMap sourceWidgetCandidate = candidateForKind(
        waitForCandidates(editController, 0),
        QStringLiteral("widget"));
    QVERIFY(!sourceWidgetCandidate.isEmpty());
    QVERIFY(editController.selectCandidate(
        0,
        sourceWidgetCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(editController.selectedKind(), QStringLiteral("widget"));
    editController.setFormDesignerController(&formDesigner);
    QCOMPARE(editController.selectedPage(), -1);
    QVERIFY(editController.selectedObjectId().isEmpty());
    QVERIFY(candidateForKind(
        editController.candidatesForPage(0),
        QStringLiteral("widget")).isEmpty());
    const QVariantMap sourcePositionCandidate = editController.candidateAt(
        0,
        initialRect.center().x(),
        initialRect.center().y());
    QVERIFY(sourcePositionCandidate.value(QStringLiteral("kind")).toString()
            != QStringLiteral("widget"));

    PDFFormView formView;
    formView.setController(&formController);
    formView.setFormDesignerController(&formDesigner);
    formView.setPage(0);
    formView.setInteractive(true);
    QTRY_COMPARE_WITH_TIMEOUT(formView.choiceFields().size(), 1, 5000);
    formView.setFormDesignMode(true);
    QCOMPARE(formView.choiceFields().size(), 0);
    formView.setFormDesignMode(false);
    QCOMPARE(formView.choiceFields().size(), 1);

    bool focusCompleted = false;
    bool focusSucceeded = false;
    formController.focusChoiceField(
        0,
        choice.annotationIndex,
        choice.sourcePage,
        choice.identityToken,
        [&focusCompleted, &focusSucceeded](bool focused) {
            focusSucceeded = focused;
            focusCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(focusCompleted, 5000);
    QVERIFY(focusSucceeded);
    QVERIFY(formDesigner.activateSourceDropdown(fieldId, 2));
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);
    QVERIFY(!formController.canUndo());
    QVERIFY(!formController.canRedo());

    choiceFields.clear();
    inspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&choiceFields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> fields) {
            choiceFields = std::move(fields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(choiceFields.size(), 1);
    QCOMPARE(choiceFields.constFirst().selectedIndices,
             QVector<int>({2}));

    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Tùy chọn 3"),
        5000);

    // Saving only the native form draft must replace PDFium's regenerated
    // text appearance with TrePDF's Unicode-safe raster appearance.
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("Transport"));
    QCOMPARE(inspection.value, QStringLiteral("Tùy chọn 3"));
    QVERIFY(inspection.appearanceStream.contains(
        QStringLiteral("/Im0 Do")));
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kDropdownVersion));

    formDesigner.prepareForDesignMode();
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        QStringLiteral("Tùy chọn 3"),
        5000);

    const QRectF resizedRect(0.22, 0.32, 0.46, 0.07);
    QVERIFY(formDesigner.setFieldRect(
        fieldId,
        resizedRect.x(),
        resizedRect.y(),
        resizedRect.width(),
        resizedRect.height()));
    const QVariantMap oldPositionCandidate = editController.candidateAt(
        0,
        initialRect.center().x(),
        initialRect.center().y());
    QVERIFY(oldPositionCandidate.value(QStringLiteral("kind")).toString()
            != QStringLiteral("widget"));
    const QVector<PDFFormDesignerController::SaveField> resizedFields
        = formDesigner.saveFields(true);
    QCOMPARE(resizedFields.size(), 1);
    QCOMPARE(resizedFields.constFirst().type,
             PDFFormDesignerController::Dropdown);
    QCOMPARE(resizedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Update);
    QCOMPARE(resizedFields.constFirst().changedProperties,
             static_cast<quint32>(
                 PDFFormDesignerController::RectPropertyChanged));
    QCOMPARE(resizedFields.constFirst().value,
             QStringLiteral("Tùy chọn 3"));
    QCOMPARE(resizedFields.constFirst().defaultValue,
             QStringLiteral("Tùy chọn 1"));
    QCOMPARE(resizedFields.constFirst().options, options);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Dropdown);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             QStringLiteral("Tùy chọn 3"));
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             options);
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());

    QStringList editedOptions = options;
    editedOptions.append(QStringLiteral("Tùy chọn 4"));
    QVERIFY(formDesigner.setFieldOptions(fieldId, editedOptions));
    QVERIFY(formDesigner.setFieldValue(fieldId, editedOptions.constLast()));
    editedOptions.removeAt(1);
    QVERIFY(formDesigner.setFieldOptions(fieldId, editedOptions));
    editedOptions[0] = QStringLiteral("Tùy chọn một");
    QVERIFY(formDesigner.renameFieldOption(
        fieldId,
        0,
        editedOptions.at(0)));
    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             editedOptions);
    QVERIFY(hydrated.value(QStringLiteral("optionsDirty")).toBool());

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 4, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             editedOptions.constLast());
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             editedOptions);
    QVERIFY(!hydrated.value(QStringLiteral("optionsDirty")).toBool());
    QVERIFY(!formDesigner.removeField(
        fieldId + QStringLiteral("-missing")));

    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("Transport"));
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(inspection.value, editedOptions.constLast());
    QCOMPARE(inspection.defaultValue, editedOptions.constFirst());
    QCOMPARE(inspection.options, editedOptions);
    QCOMPARE(inspection.selectedIndices, QVector<int>({2}));
    QVERIFY(inspection.hasAppearance);
    QFile savedDropdown(sourcePath);
    QVERIFY(savedDropdown.open(QIODevice::ReadOnly));
    const QByteArray savedBytes = savedDropdown.readAll();
    const QByteArray unicodeFontMarker
        = QByteArrayLiteral("/")
        + QByteArray(PDFFormAppearance::kFontVersionKey)
        + ' '
        + QByteArray::number(PDFFormAppearance::kFontVersion);
    QCOMPARE(savedBytes.count(unicodeFontMarker), 1);
    QVERIFY(savedBytes.contains("/Type0"));
    QVERIFY(savedBytes.contains("/CIDToGIDMap"));
    QVERIFY(qAbs(inspection.rect.left - 130.9) < 1.0);
    QVERIFY(qAbs(inspection.rect.right - 404.6) < 1.0);
    QVERIFY(qAbs(inspection.rect.top - 572.56) < 1.0);
    QVERIFY(qAbs(inspection.rect.bottom - 513.62) < 1.0);
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        resizedRect.center()));
    QVERIFY(!formDesigner.sourceFieldContainsPoint(
        0,
        initialRect.topLeft()));
}

void PDFEditControllerTest::savesRehydratesAndInteractsWithListBox()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-list-box.pdf"));
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

    const QStringList options({
        QStringLiteral("Hà Nội"),
        QStringLiteral("Đà Nẵng"),
        QStringLiteral("TP. Hồ Chí Minh"),
        QStringLiteral("Cần Thơ"),
        QStringLiteral("Hải Phòng"),
    });
    const QStringList initialSelection({
        options.at(1),
        options.at(3),
    });
    const QRectF fieldRect(0.14, 0.18, 0.46, 0.18);
    const QString fieldId = formDesigner.addField(
        PDFFormDesignerController::ListBox,
        0,
        fieldRect.x(),
        fieldRect.y(),
        fieldRect.width(),
        fieldRect.height());
    QVERIFY(fieldId.startsWith(QStringLiteral("listbox-field-")));
    const QVariantMap initialField = formDesigner.fieldData(fieldId);
    QCOMPARE(initialField.value(QStringLiteral("options")).toStringList().size(),
             5);
    QCOMPARE(initialField.value(QStringLiteral("selectedValues"))
                 .toStringList().size(),
             1);
    QVERIFY(formDesigner.setFieldName(
        fieldId,
        QStringLiteral("OfficeLocation")));
    QVERIFY(formDesigner.setFieldOptions(fieldId, options));
    QVERIFY(formDesigner.setFieldSelectedValues(
        fieldId,
        initialSelection));
    QVERIFY(formDesigner.setFieldFlags(
        fieldId,
        PDFFormDesignerController::Required));

    const QVector<PDFFormDesignerController::SaveField> addedFields
        = formDesigner.saveFields();
    QCOMPARE(addedFields.size(), 1);
    QCOMPARE(addedFields.constFirst().type,
             PDFFormDesignerController::ListBox);
    QCOMPARE(addedFields.constFirst().options, options);
    QCOMPARE(addedFields.constFirst().value, options.at(1));
    QCOMPARE(addedFields.constFirst().selectedValues, initialSelection);
    QCOMPARE(addedFields.constFirst().defaultValue, options.constFirst());

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);

    QVariantMap hydrated = formDesigner.fieldData(fieldId);
    QCOMPARE(hydrated.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::ListBox);
    QCOMPARE(hydrated.value(QStringLiteral("options")).toStringList(),
             options);
    QCOMPARE(hydrated.value(QStringLiteral("value")).toString(),
             options.at(1));
    QCOMPARE(hydrated.value(QStringLiteral("selectedValues")).toStringList(),
             initialSelection);
    QVERIFY(hydrated.value(QStringLiteral("sourceBacked")).toBool());
    QVERIFY(!hydrated.value(QStringLiteral("dirty")).toBool());

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("OfficeLocation"));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_REQUIRED);
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_COMBO));
    QVERIFY(!(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_EDIT));
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
    QCOMPARE(inspection.defaultValue, options.constFirst());
    QCOMPARE(inspection.options, options);
    QCOMPARE(inspection.selectedIndices, QVector<int>({1, 3}));
    QVERIFY(inspection.annotationFlags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(inspection.appearanceStream.contains(
        QStringLiteral("/Im0 Do")));
    QVERIFY(inspection.hasAppearanceVersion);
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kListBoxVersion));

    QTRY_VERIFY_WITH_TIMEOUT(formController.javascriptEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !formController.javascriptTransitionPending(),
        5000);
    QVector<PDFFormWorker::ChoiceField> choiceFields;
    bool inspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&choiceFields, &inspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> fields) {
            choiceFields = std::move(fields);
            inspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(inspectionCompleted, 5000);
    QCOMPARE(choiceFields.size(), 1);
    const PDFFormWorker::ChoiceField choice = choiceFields.constFirst();
    QCOMPARE(choice.fieldType, FPDF_FORMFIELD_LISTBOX);
    QCOMPARE(choice.options, options);
    QCOMPARE(choice.selectedIndices, QVector<int>({1, 3}));
    QVERIFY(!choice.readOnly);
    QVERIFY(choice.multiSelect);
    QVERIFY(!choice.identityToken.isEmpty());
    QVERIFY(formDesigner.managesSourceChoiceField(
        choice.sourcePage,
        choice.annotationIndex,
        choice.identityToken));

    QVERIFY(formDesigner.toggleSourceChoiceOption(fieldId, 0));
    QVERIFY(formDesigner.toggleSourceChoiceOption(fieldId, 1));
    QTRY_VERIFY_WITH_TIMEOUT(formController.hasDrafts(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("selectedValues"))
            .toStringList(),
        QStringList({options.at(0), options.at(3)}),
        5000);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("OfficeLocation"));
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(inspection.fieldFlags & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
    QCOMPARE(inspection.selectedIndices, QVector<int>({0, 3}));
    QCOMPARE(inspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kListBoxVersion));

    QVERIFY(formDesigner.toggleSourceChoiceOption(fieldId, 0));
    QVERIFY(formDesigner.toggleSourceChoiceOption(fieldId, 3));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("selectedValues"))
            .toStringList(),
        QStringList(),
        5000);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("OfficeLocation"));
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QCOMPARE(inspection.selectedIndices, QVector<int>());

    QVERIFY(formDesigner.removeField(fieldId));
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 4, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 0, 10000);
    inspection = inspectAcroFormField(
        sourcePath,
        QStringLiteral("OfficeLocation"));
    QCOMPARE(inspection.widgetCount, 0);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_UNKNOWN);
}

void PDFEditControllerTest::migratesLegacyListBoxWhenAddingMultiSelectListBox()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-legacy-list-box-migration.pdf"));
    QVERIFY(createLegacyTrePDFListBoxPdf(sourcePath));

    const QString legacyFieldId = QString::fromLatin1(
        kLegacyListBoxWidgetId);
    const QString legacyFieldName = QString::fromLatin1(
        kLegacyListBoxFieldName);
    const QStringList legacyOptions({
        QStringLiteral("Alpha"),
        QStringLiteral("Beta"),
        QStringLiteral("Gamma"),
    });
    const QStringList legacySelection({legacyOptions.at(1)});
    const QStringList addedOptions({
        QStringLiteral("North"),
        QStringLiteral("South"),
        QStringLiteral("East"),
        QStringLiteral("West"),
        QStringLiteral("Central"),
        QStringLiteral("Remote"),
    });
    const QStringList addedSelection({
        addedOptions.at(1),
        addedOptions.at(4),
    });
    const QString addedFieldName = QStringLiteral("NewMultiOfficeList");
    QString addedFieldId;

    AcroFormFieldInspection legacyInspection = inspectAcroFormField(
        sourcePath,
        legacyFieldName);
    QVERIFY(legacyInspection.readable);
    QCOMPARE(legacyInspection.widgetCount, 1);
    QCOMPARE(legacyInspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_COMBO));
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_EDIT));
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_MULTI_SELECT));
    QCOMPARE(legacyInspection.options, legacyOptions);
    QCOMPARE(legacyInspection.selectedIndices, QVector<int>({1}));
    QVERIFY(legacyInspection.hasAppearanceVersion);
    QCOMPARE(legacyInspection.appearanceVersion, 1.0f);

    {
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

        QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
        const QVariantMap legacyField = formDesigner.fieldData(
            legacyFieldId);
        QCOMPARE(legacyField.value(QStringLiteral("type")).toInt(),
                 PDFFormDesignerController::ListBox);
        QCOMPARE(legacyField.value(QStringLiteral("options")).toStringList(),
                 legacyOptions);
        QCOMPARE(legacyField.value(QStringLiteral("selectedValues"))
                     .toStringList(),
                 legacySelection);
        QCOMPARE(legacyField.value(QStringLiteral("value")).toString(),
                 legacySelection.constFirst());
        QVERIFY(legacyField.value(QStringLiteral("sourceBacked")).toBool());
        QVERIFY(!legacyField.value(QStringLiteral("dirty")).toBool());

        const QRectF addedRect(0.14, 0.42, 0.46, 0.18);
        addedFieldId = formDesigner.addField(
            PDFFormDesignerController::ListBox,
            0,
            addedRect.x(),
            addedRect.y(),
            addedRect.width(),
            addedRect.height());
        QVERIFY(!addedFieldId.isEmpty());
        QVERIFY(formDesigner.setFieldName(
            addedFieldId,
            addedFieldName));
        QVERIFY(formDesigner.setFieldOptions(
            addedFieldId,
            addedOptions));
        QVERIFY(formDesigner.setFieldSelectedValues(
            addedFieldId,
            addedSelection));

        QSignalSpy saveFinishedSpy(
            &editController,
            &PDFEditController::saveFinished);
        QVERIFY(editController.requestSave());
        QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
        QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
        QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    }

    {
        PDFDocument document;
        PDFFormDesignerController formDesigner;
        PDFFormController formController;
        formDesigner.setDocument(&document);
        formController.setDocument(&document);
        formDesigner.setFormController(&formController);
        QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));

        QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
        const QVariantMap legacyField = formDesigner.fieldData(
            legacyFieldId);
        QCOMPARE(legacyField.value(QStringLiteral("type")).toInt(),
                 PDFFormDesignerController::ListBox);
        QCOMPARE(legacyField.value(QStringLiteral("selectedValues"))
                     .toStringList(),
                 legacySelection);
        QCOMPARE(legacyField.value(QStringLiteral("value")).toString(),
                 legacySelection.constFirst());
        QVERIFY(legacyField.value(QStringLiteral("sourceBacked")).toBool());

        const QVariantMap addedField = formDesigner.fieldData(
            addedFieldId);
        QCOMPARE(addedField.value(QStringLiteral("type")).toInt(),
                 PDFFormDesignerController::ListBox);
        QCOMPARE(addedField.value(QStringLiteral("options")).toStringList(),
                 addedOptions);
        QCOMPARE(addedField.value(QStringLiteral("selectedValues"))
                     .toStringList(),
                 addedSelection);
        QCOMPARE(addedField.value(QStringLiteral("value")).toString(),
                 addedSelection.constFirst());
        QVERIFY(addedField.value(QStringLiteral("sourceBacked")).toBool());
    }

    legacyInspection = inspectAcroFormField(
        sourcePath,
        legacyFieldName);
    QVERIFY(legacyInspection.readable);
    QCOMPARE(legacyInspection.widgetCount, 2);
    QCOMPARE(legacyInspection.widgetId, legacyFieldId);
    QCOMPARE(legacyInspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(legacyInspection.fieldFlags
            & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_COMBO));
    QVERIFY(!(legacyInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_EDIT));
    QCOMPARE(legacyInspection.options, legacyOptions);
    QCOMPARE(legacyInspection.value, legacySelection.constFirst());
    QCOMPARE(legacyInspection.selectedIndices, QVector<int>({1}));
    QVERIFY(legacyInspection.hasAppearance);
    QVERIFY(legacyInspection.hasAppearanceVersion);
    QCOMPARE(legacyInspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kListBoxVersion));

    const AcroFormFieldInspection addedInspection = inspectAcroFormField(
        sourcePath,
        addedFieldName);
    QVERIFY(addedInspection.readable);
    QCOMPARE(addedInspection.widgetCount, 2);
    QCOMPARE(addedInspection.widgetId, addedFieldId);
    QCOMPARE(addedInspection.fieldType, FPDF_FORMFIELD_LISTBOX);
    QVERIFY(addedInspection.fieldFlags
            & FPDF_FORMFLAG_CHOICE_MULTI_SELECT);
    QVERIFY(!(addedInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_COMBO));
    QVERIFY(!(addedInspection.fieldFlags
              & FPDF_FORMFLAG_CHOICE_EDIT));
    QCOMPARE(addedInspection.options, addedOptions);
    QCOMPARE(addedInspection.value, addedSelection.constFirst());
    QCOMPARE(addedInspection.selectedIndices, QVector<int>({1, 4}));
    QVERIFY(addedInspection.hasAppearance);
    QVERIFY(addedInspection.hasAppearanceVersion);
    QCOMPARE(addedInspection.appearanceVersion,
             static_cast<float>(PDFFormAppearance::kListBoxVersion));
}

void PDFEditControllerTest::deletesRehydratedDropdownWithoutChangingPeer()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("acroform-dropdown-delete.pdf"));
    QVERIFY(createDropdownDeleteRegressionPdf(sourcePath));

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

    const QStringList removedOptions({
        QStringLiteral("Bus"),
        QStringLiteral("Train"),
        QStringLiteral("Plane"),
    });
    const QStringList peerOptions({
        QStringLiteral("Hanoi"),
        QStringLiteral("Da Nang"),
        QStringLiteral("Ho Chi Minh City"),
    });
    const QRectF removedRect(0.12, 0.18, 0.34, 0.06);
    const QRectF peerRect(0.54, 0.38, 0.36, 0.07);
    const QString removedFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        removedRect.x(),
        removedRect.y(),
        removedRect.width(),
        removedRect.height());
    const QString peerFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        peerRect.x(),
        peerRect.y(),
        peerRect.width(),
        peerRect.height());
    QVERIFY(removedFieldId.startsWith(
        QStringLiteral("dropdown-field-")));
    QVERIFY(peerFieldId.startsWith(QStringLiteral("dropdown-field-")));
    QVERIFY(formDesigner.setFieldName(
        removedFieldId,
        QStringLiteral("TransportToDelete")));
    QVERIFY(formDesigner.setFieldOptions(
        removedFieldId,
        removedOptions));
    QVERIFY(formDesigner.setFieldValue(
        removedFieldId,
        QStringLiteral("Train")));
    QVERIFY(formDesigner.setFieldName(
        peerFieldId,
        QStringLiteral("DestinationToKeep")));
    QVERIFY(formDesigner.setFieldOptions(peerFieldId, peerOptions));
    QVERIFY(formDesigner.setFieldValue(
        peerFieldId,
        QStringLiteral("Da Nang")));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QFile savedFixture(sourcePath);
    QVERIFY(savedFixture.open(QIODevice::ReadOnly));
    QByteArray savedFixtureBytes = savedFixture.readAll();
    QVERIFY(savedFixtureBytes.contains(
        QByteArrayLiteral("/TrePDFRegression 4 0 R")));
    QVERIFY(savedFixtureBytes.contains(QByteArrayLiteral("(<<)")));

    const QVariantMap rehydratedRemoved
        = formDesigner.fieldData(removedFieldId);
    const QVariantMap rehydratedPeer
        = formDesigner.fieldData(peerFieldId);
    QVERIFY(rehydratedRemoved.value(
        QStringLiteral("sourceBacked")).toBool());
    QVERIFY(rehydratedPeer.value(
        QStringLiteral("sourceBacked")).toBool());

    QVector<PDFFormWorker::ChoiceField> choiceFields;
    bool choiceInspectionCompleted = false;
    formController.requestChoiceFields(
        0,
        [&choiceFields, &choiceInspectionCompleted](
            QVector<PDFFormWorker::ChoiceField> fields) {
            choiceFields = std::move(fields);
            choiceInspectionCompleted = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(choiceInspectionCompleted, 5000);
    QCOMPARE(choiceFields.size(), 2);
    const auto removedChoice = std::find_if(
        choiceFields.cbegin(),
        choiceFields.cend(),
        [&removedRect](const PDFFormWorker::ChoiceField &choice) {
            return qAbs(choice.normalizedRect.x() - removedRect.x())
                    < 0.01
                && qAbs(choice.normalizedRect.y() - removedRect.y())
                    < 0.01;
        });
    QVERIFY(removedChoice != choiceFields.cend());
    const int removedAnnotationIndex = removedChoice->annotationIndex;
    const int removedSourcePage = removedChoice->sourcePage;
    const QString removedIdentityToken = removedChoice->identityToken;
    QVERIFY(!removedIdentityToken.isEmpty());

    QVERIFY(formDesigner.removeField(removedFieldId));
    QCOMPARE(formDesigner.rowCount(), 1);
    QVERIFY(formDesigner.hasDrafts());
    QVERIFY(formDesigner.fieldData(removedFieldId).isEmpty());
    const QVariantList visibleFields = formDesigner.fieldsForPage(0);
    QCOMPARE(visibleFields.size(), 1);
    QCOMPARE(visibleFields.constFirst().toMap()
                 .value(QStringLiteral("id"))
                 .toString(),
             peerFieldId);

    const QStringList suppressedFieldIds
        = formDesigner.sourceFieldIdsForPage(0);
    QCOMPARE(suppressedFieldIds.size(), 2);
    QVERIFY(suppressedFieldIds.contains(removedFieldId));
    QVERIFY(suppressedFieldIds.contains(peerFieldId));
    QVERIFY(formDesigner.managesSourceChoiceField(
        removedSourcePage,
        removedAnnotationIndex,
        removedIdentityToken));
    QVERIFY(formDesigner.managesSourceWidget(
        0,
        removedAnnotationIndex,
        removedFieldId));
    QVERIFY(formDesigner.sourceFieldContainsPoint(
        0,
        removedRect.center()));
    QVERIFY(!formDesigner.sourceFieldContainsPoint(
        0,
        QPointF(0.98, 0.98)));

    const QVector<PDFFormDesignerController::SaveField> removedFields
        = formDesigner.saveFields();
    QCOMPARE(removedFields.size(), 1);
    QCOMPARE(removedFields.constFirst().id, removedFieldId);
    QCOMPARE(removedFields.constFirst().type,
             PDFFormDesignerController::Dropdown);
    QCOMPARE(removedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Remove);
    QCOMPARE(removedFields.constFirst().page, 0);

    editController.discardDrafts();
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        formDesigner.fieldData(removedFieldId)
            .value(QStringLiteral("sourceBacked"))
            .toBool(),
        5000);
    QCOMPARE(inspectAcroFormField(
                 sourcePath,
                 QStringLiteral("TransportToDelete"))
                 .fieldType,
             FPDF_FORMFIELD_COMBOBOX);

    const QStringList addedOptions({
        QStringLiteral("Standard"),
        QStringLiteral("Express"),
        QStringLiteral("Overnight"),
    });
    const QRectF addedRect(0.20, 0.64, 0.42, 0.06);
    const QString addedFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        addedRect.x(),
        addedRect.y(),
        addedRect.width(),
        addedRect.height());
    QVERIFY(addedFieldId.startsWith(QStringLiteral("dropdown-field-")));
    QVERIFY(formDesigner.setFieldName(
        addedFieldId,
        QStringLiteral("DeliveryToAdd")));
    QVERIFY(formDesigner.setFieldOptions(addedFieldId, addedOptions));
    QVERIFY(formDesigner.setFieldValue(
        addedFieldId,
        QStringLiteral("Overnight")));
    QVERIFY(formDesigner.removeField(removedFieldId));
    const QVector<PDFFormDesignerController::SaveField> mixedFields
        = formDesigner.saveFields();
    QCOMPARE(mixedFields.size(), 2);
    const auto addedSnapshot = std::find_if(
        mixedFields.cbegin(),
        mixedFields.cend(),
        [](const PDFFormDesignerController::SaveField &field) {
            return field.operation
                == PDFFormDesignerController::SaveField::Add;
        });
    const auto removedSnapshot = std::find_if(
        mixedFields.cbegin(),
        mixedFields.cend(),
        [](const PDFFormDesignerController::SaveField &field) {
            return field.operation
                == PDFFormDesignerController::SaveField::Remove;
        });
    QVERIFY(addedSnapshot != mixedFields.cend());
    QVERIFY(removedSnapshot != mixedFields.cend());
    QCOMPARE(addedSnapshot->id, addedFieldId);
    QCOMPARE(addedSnapshot->name, QStringLiteral("DeliveryToAdd"));
    QCOMPARE(addedSnapshot->options, addedOptions);
    QCOMPARE(addedSnapshot->value, QStringLiteral("Overnight"));
    QCOMPARE(removedSnapshot->id, removedFieldId);
    QCOMPARE(removedSnapshot->operation,
             PDFFormDesignerController::SaveField::Remove);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QVERIFY(formDesigner.fieldData(removedFieldId).isEmpty());
    savedFixture.close();
    QVERIFY(savedFixture.open(QIODevice::ReadOnly));
    savedFixtureBytes = savedFixture.readAll();
    QVERIFY(savedFixtureBytes.contains(
        QByteArrayLiteral("/TrePDFRegression 4 0 R")));
    QVERIFY(savedFixtureBytes.contains(QByteArrayLiteral("(<<)")));

    const QStringList remainingSourceFieldIds
        = formDesigner.sourceFieldIdsForPage(0);
    QCOMPARE(remainingSourceFieldIds.size(), 2);
    QVERIFY(!remainingSourceFieldIds.contains(removedFieldId));
    QVERIFY(remainingSourceFieldIds.contains(peerFieldId));
    QVERIFY(remainingSourceFieldIds.contains(addedFieldId));
    const QVariantMap remainingPeer
        = formDesigner.fieldData(peerFieldId);
    QVERIFY(remainingPeer.value(
        QStringLiteral("sourceBacked")).toBool());
    QCOMPARE(remainingPeer.value(QStringLiteral("value")).toString(),
             QStringLiteral("Da Nang"));
    QCOMPARE(remainingPeer.value(QStringLiteral("options")).toStringList(),
             peerOptions);
    const QVariantMap remainingAdded
        = formDesigner.fieldData(addedFieldId);
    QVERIFY(remainingAdded.value(
        QStringLiteral("sourceBacked")).toBool());
    QCOMPARE(remainingAdded.value(QStringLiteral("value")).toString(),
             QStringLiteral("Overnight"));
    QCOMPARE(remainingAdded.value(QStringLiteral("options")).toStringList(),
             addedOptions);

    const AcroFormFieldInspection deletedInspection
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("TransportToDelete"));
    QVERIFY(deletedInspection.readable);
    QCOMPARE(deletedInspection.widgetCount, 2);
    QCOMPARE(deletedInspection.fieldType, FPDF_FORMFIELD_UNKNOWN);
    QVERIFY(deletedInspection.name.isEmpty());

    const AcroFormFieldInspection peerInspection
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("DestinationToKeep"));
    QVERIFY(peerInspection.readable);
    QCOMPARE(peerInspection.formType, FORMTYPE_ACRO_FORM);
    QCOMPARE(peerInspection.widgetCount, 2);
    QCOMPARE(peerInspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(peerInspection.name,
             QStringLiteral("DestinationToKeep"));
    QCOMPARE(peerInspection.value, QStringLiteral("Da Nang"));
    QCOMPARE(peerInspection.defaultValue, peerOptions.constFirst());
    QCOMPARE(peerInspection.options, peerOptions);
    QCOMPARE(peerInspection.selectedIndices, QVector<int>({1}));
    QVERIFY(peerInspection.hasAppearance);
    QVERIFY(qAbs(peerInspection.rect.left - 321.3) < 1.0);
    QVERIFY(qAbs(peerInspection.rect.right - 535.5) < 1.0);
    QVERIFY(qAbs(peerInspection.rect.top - 521.66) < 1.0);
    QVERIFY(qAbs(peerInspection.rect.bottom - 462.72) < 1.0);

    const AcroFormFieldInspection addedInspection
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("DeliveryToAdd"));
    QVERIFY(addedInspection.readable);
    QCOMPARE(addedInspection.widgetCount, 2);
    QCOMPARE(addedInspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(addedInspection.name, QStringLiteral("DeliveryToAdd"));
    QCOMPARE(addedInspection.value, QStringLiteral("Overnight"));
    QCOMPARE(addedInspection.defaultValue, addedOptions.constFirst());
    QCOMPARE(addedInspection.options, addedOptions);
    QCOMPARE(addedInspection.selectedIndices, QVector<int>({2}));
    QVERIFY(addedInspection.hasAppearance);
    QVERIFY(qAbs(addedInspection.rect.left - 119.0) < 1.0);
    QVERIFY(qAbs(addedInspection.rect.right - 368.9) < 1.0);
    QVERIFY(qAbs(addedInspection.rect.top - 303.12) < 1.0);
    QVERIFY(qAbs(addedInspection.rect.bottom - 252.6) < 1.0);
}

void PDFEditControllerTest::deletesDropdownWhenItsSourcePageIsDeleted()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("dropdown-delete-with-source-page.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

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
    QCOMPARE(document.pageCount(), 2);

    const QString fieldName = QStringLiteral("ReusablePageZeroDropdown");
    const QStringList initialOptions({
        QStringLiteral("Alpha"),
        QStringLiteral("Beta"),
        QStringLiteral("Gamma"),
    });
    const QRectF initialRect(0.16, 0.20, 0.42, 0.07);
    const QString removedFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        initialRect.x(),
        initialRect.y(),
        initialRect.width(),
        initialRect.height());
    QVERIFY(!removedFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(removedFieldId, fieldName));
    QVERIFY(formDesigner.setFieldOptions(
        removedFieldId,
        initialOptions));
    QVERIFY(formDesigner.setFieldValue(
        removedFieldId,
        QStringLiteral("Beta")));

    QSignalSpy saveFinishedSpy(
        &editController,
        &PDFEditController::saveFinished);
    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 10000);
    QVERIFY(saveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!formDesigner.hasDrafts(), 10000);
    QVERIFY(formDesigner.fieldData(removedFieldId)
                .value(QStringLiteral("sourceBacked"))
                .toBool());

    QVERIFY(formDesigner.removeField(removedFieldId));
    QCOMPARE(formDesigner.rowCount(), 0);
    document.setCurrentPage(0);
    QCOMPARE(editController.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.sourcePageIndex(0), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(0), -1);

    const QVector<PDFFormDesignerController::SaveField> removedFields
        = formDesigner.saveFields();
    QCOMPARE(removedFields.size(), 1);
    QCOMPARE(removedFields.constFirst().id, removedFieldId);
    QCOMPARE(removedFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Remove);
    QCOMPARE(removedFields.constFirst().page, -1);
    QVERIFY(editController.hasDrafts());

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 2, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(document.pageCount(), 1, 10000);
    QCOMPARE(document.sourcePageCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 0, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!editController.hasDrafts(), 10000);
    QVERIFY(formDesigner.fieldData(removedFieldId).isEmpty());
    QVERIFY(formDesigner.sourceFieldIdsForPage(0).isEmpty());

    AcroFormFieldInspection inspection = inspectAcroFormField(
        sourcePath,
        fieldName);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.widgetCount, 0);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_UNKNOWN);

    const QStringList replacementOptions({
        QStringLiteral("One"),
        QStringLiteral("Two"),
        QStringLiteral("Three"),
    });
    const QRectF replacementRect(0.22, 0.34, 0.52, 0.09);
    const QString replacementFieldId = formDesigner.addField(
        PDFFormDesignerController::Dropdown,
        0,
        replacementRect.x(),
        replacementRect.y(),
        replacementRect.width(),
        replacementRect.height());
    QVERIFY(!replacementFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(replacementFieldId, fieldName));
    QVERIFY(formDesigner.setFieldOptions(
        replacementFieldId,
        replacementOptions));
    QVERIFY(formDesigner.setFieldValue(
        replacementFieldId,
        QStringLiteral("Three")));

    const QVector<PDFFormDesignerController::SaveField> replacementFields
        = formDesigner.saveFields();
    QCOMPARE(replacementFields.size(), 1);
    QCOMPARE(replacementFields.constFirst().id, replacementFieldId);
    QCOMPARE(replacementFields.constFirst().operation,
             PDFFormDesignerController::SaveField::Add);
    QCOMPARE(replacementFields.constFirst().name, fieldName);

    QVERIFY(editController.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 3, 10000);
    QVERIFY(saveFinishedSpy.constLast().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!editController.hasDrafts(), 10000);

    const QVariantMap replacement
        = formDesigner.fieldData(replacementFieldId);
    QVERIFY(replacement.value(
        QStringLiteral("sourceBacked")).toBool());
    QCOMPARE(replacement.value(QStringLiteral("name")).toString(),
             fieldName);
    QCOMPARE(replacement.value(QStringLiteral("options")).toStringList(),
             replacementOptions);
    QCOMPARE(replacement.value(QStringLiteral("value")).toString(),
             QStringLiteral("Three"));

    inspection = inspectAcroFormField(sourcePath, fieldName);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.widgetCount, 1);
    QCOMPARE(inspection.fieldType, FPDF_FORMFIELD_COMBOBOX);
    QCOMPARE(inspection.name, fieldName);
    QCOMPARE(inspection.options, replacementOptions);
    QCOMPARE(inspection.value, QStringLiteral("Three"));
    QCOMPARE(inspection.selectedIndices, QVector<int>({2}));
}

void PDFEditControllerTest::rejectsDropdownExportDisplayPairs()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("dropdown-export-display-pairs.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("dropdown-export-display-pairs-saved.pdf"));
    QVERIFY(createTrePDFDropdownWithExportValuesPdf(sourcePath));

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
    QTRY_COMPARE_WITH_TIMEOUT(formDesigner.rowCount(), 1, 5000);

    const QVariantMap field = formDesigner.fieldData(
        QStringLiteral(
            "dropdown-field-11111111-1111-1111-1111-111111111111"));
    QCOMPARE(field.value(QStringLiteral("type")).toInt(),
             PDFFormDesignerController::Dropdown);
    QCOMPARE(field.value(QStringLiteral("options")).toStringList(),
             QStringList({
                 QStringLiteral("Airplane"),
                 QStringLiteral("Bicycle"),
                 QStringLiteral("Car"),
             }));
    QCOMPARE(field.value(QStringLiteral("value")).toString(),
             QStringLiteral("Airplane"));
    QVERIFY(formDesigner.setFieldRect(
        field.value(QStringLiteral("id")).toString(),
        0.10,
        0.20,
        0.50,
        0.08));

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        editController,
        savedPath);
    QCOMPARE(result.error, PDFSaveWriter::Error::AcroFormCreationFailed);
    QVERIFY(!result.detail.isEmpty());
    QVERIFY(!QFileInfo::exists(savedPath));
    QVERIFY(QFileInfo::exists(sourcePath));
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
        PDFFormDesignerController::Required
            | PDFFormDesignerController::Multiline));
    QVERIFY(formDesigner.setFieldMaxLength(fieldId, 64));
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
    QVERIFY(
        hydrated.value(QStringLiteral("flags")).toInt()
        & PDFFormDesignerController::Multiline);
    QCOMPARE(
        hydrated.value(QStringLiteral("maxLength")).toInt(),
        64);

    const AcroFormFieldInspection firstSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentText"));
    QVERIFY(firstSave.readable);
    QCOMPARE(firstSave.widgetCount, 2);
    QCOMPARE(firstSave.value, QStringLiteral("First saved value"));
    QVERIFY(firstSave.fieldFlags & FPDF_FORMFLAG_TEXT_MULTILINE);
    QVERIFY(firstSave.hasMaxLength);
    QCOMPARE(firstSave.maxLength, 64.0f);
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
    QVERIFY(formDesigner.setFieldMaxLength(fieldId, 0));
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
            | PDFFormDesignerController::MaxLengthPropertyChanged
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
    QCOMPARE(
        hydrated.value(QStringLiteral("maxLength")).toInt(),
        0);
    const AcroFormFieldInspection secondSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentText"));
    QCOMPARE(secondSave.widgetCount, 2);
    QCOMPARE(secondSave.value, QStringLiteral("Updated after reopen"));
    QVERIFY(secondSave.fieldFlags & FPDF_FORMFLAG_TEXT_MULTILINE);
    QVERIFY(!secondSave.hasMaxLength);
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
    constexpr int tightenedMaxLength = 22;
    const QString limitedNativeValue
        = nativeValueAfterBackspace.first(tightenedMaxLength);
    QVERIFY(formDesigner.setFieldMaxLength(
        fieldId,
        tightenedMaxLength));
    QTRY_COMPARE_WITH_TIMEOUT(
        formDesigner.fieldData(fieldId)
            .value(QStringLiteral("value"))
            .toString(),
        limitedNativeValue,
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
            PDFFormDesignerController::ValuePropertyChanged
            | PDFFormDesignerController::NamePropertyChanged
            | PDFFormDesignerController::MaxLengthPropertyChanged));
    QCOMPARE(nameChange.constFirst().maxLength, tightenedMaxLength);
    QCOMPARE(nameChange.constFirst().value, limitedNativeValue);

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
        limitedNativeValue);
    QCOMPARE(
        hydrated.value(QStringLiteral("maxLength")).toInt(),
        tightenedMaxLength);
    const AcroFormFieldInspection nativeValueSave
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentTextRenamed"));
    QCOMPARE(nativeValueSave.widgetCount, 2);
    QCOMPARE(
        nativeValueSave.value,
        limitedNativeValue);
    QVERIFY(nativeValueSave.hasMaxLength);
    QCOMPARE(
        nativeValueSave.maxLength,
        static_cast<float>(tightenedMaxLength));
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
        limitedNativeValue);
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
        limitedNativeValue,
        10000);
    QVERIFY(!freshSession.formDesignerController()->hasDrafts());

    PDFFormDesignerController *freshDesigner
        = freshSession.formDesignerController();
    PDFEditController *freshEditor = freshSession.editController();
    freshDesigner->selectTextField(fieldId);
    freshDesigner->setTextFieldFocused(fieldId, true);
    QVERIFY(freshDesigner->removeField(fieldId));
    QCOMPARE(freshDesigner->rowCount(), 1);
    QVERIFY(freshDesigner->fieldData(fieldId).isEmpty());
    QVERIFY(freshDesigner->selectedTextFieldId().isEmpty());
    QVERIFY(!freshDesigner->textFieldFocused());
    QVERIFY(freshDesigner->hasDrafts());
    const QStringList suppressedAfterRemoval
        = freshDesigner->sourceFieldIdsForPage(0);
    QCOMPARE(suppressedAfterRemoval.size(), 2);
    QVERIFY(suppressedAfterRemoval.contains(fieldId));
    QVERIFY(suppressedAfterRemoval.contains(blankFieldId));
    const QVector<PDFFormDesignerController::SaveField> removalSnapshot
        = freshDesigner->saveFields();
    QCOMPARE(removalSnapshot.size(), 1);
    QCOMPARE(removalSnapshot.constFirst().id, fieldId);
    QCOMPARE(removalSnapshot.constFirst().type,
             PDFFormDesignerController::Text);
    QCOMPARE(removalSnapshot.constFirst().operation,
             PDFFormDesignerController::SaveField::Remove);
    QCOMPARE(removalSnapshot.constFirst().page, 0);

    freshEditor->discardDrafts();
    QTRY_COMPARE_WITH_TIMEOUT(freshDesigner->rowCount(), 2, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        freshDesigner->fieldData(fieldId)
            .value(QStringLiteral("sourceBacked"))
            .toBool(),
        10000);
    QVERIFY(!freshDesigner->hasDrafts());

    QVERIFY(freshDesigner->removeField(fieldId));
    QSignalSpy freshSaveFinishedSpy(
        freshEditor,
        &PDFEditController::saveFinished);
    QVERIFY(freshEditor->requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(freshSaveFinishedSpy.count(), 1, 10000);
    QVERIFY(freshSaveFinishedSpy.constFirst().constFirst().toBool());
    QTRY_COMPARE_WITH_TIMEOUT(freshDesigner->rowCount(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!freshDesigner->hasDrafts(), 10000);
    QVERIFY(freshDesigner->fieldData(fieldId).isEmpty());
    const QVariantMap remainingBlankField
        = freshDesigner->fieldData(blankFieldId);
    QCOMPARE(
        remainingBlankField.value(QStringLiteral("name")).toString(),
        QStringLiteral("PersistentBlank"));
    QVERIFY(
        remainingBlankField.value(QStringLiteral("sourceBacked")).toBool());

    const AcroFormFieldInspection removedTextField
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentTextRenamed"));
    QVERIFY(removedTextField.readable);
    QCOMPARE(removedTextField.widgetCount, 1);
    QCOMPARE(
        removedTextField.fieldType,
        FPDF_FORMFIELD_UNKNOWN);
    const AcroFormFieldInspection remainingTextField
        = inspectAcroFormField(
            sourcePath,
            QStringLiteral("PersistentBlank"));
    QVERIFY(remainingTextField.readable);
    QCOMPARE(remainingTextField.widgetCount, 1);
    QCOMPARE(
        remainingTextField.fieldType,
        FPDF_FORMFIELD_TEXTFIELD);
    QVERIFY(remainingTextField.hasAppearance);
    QVERIFY(remainingTextField.pointHit);
    QVERIFY(remainingTextField.focusHandled);
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

void PDFEditControllerTest::dropsAsyncResultsAfterControllerDestruction()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("destroyed-controller-async-source.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto controller = std::make_unique<PDFEditController>();
        controller->setDocument(&document);
        controller->candidateAt(0, 0.5, 0.5);
        controller->requestCommentIndex();
        controller.reset();
    }

    QTest::qWait(1000);
    QVERIFY(document.isLoaded());
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

void PDFEditControllerTest::keepsTextMarkupColorsIndependent()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("independent-markup-colors-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());

    const QColor defaultHighlightColor(255, 235, 59, 96);
    const QColor defaultUnderlineColor(198, 40, 40, 192);
    const QColor defaultSquigglyColor(198, 40, 40, 192);
    const QColor defaultStrikeoutColor(198, 40, 40, 192);
    const QColor highlightColor(255, 128, 171, 96);
    const QColor underlineColor(73, 91, 211, 176);
    const QColor squigglyColor(38, 166, 154, 160);
    const QColor strikeoutColor(123, 31, 162, 184);
    QCOMPARE(controller.highlightColor(), defaultHighlightColor);
    QCOMPARE(controller.underlineColor(), defaultUnderlineColor);
    QCOMPARE(controller.squigglyColor(), defaultSquigglyColor);
    QCOMPARE(controller.strikeoutColor(), defaultStrikeoutColor);

    QSignalSpy highlightColorSpy(
        &controller,
        &PDFEditController::highlightColorChanged);
    QSignalSpy underlineColorSpy(
        &controller,
        &PDFEditController::underlineColorChanged);
    QSignalSpy squigglyColorSpy(
        &controller,
        &PDFEditController::squigglyColorChanged);
    QSignalSpy strikeoutColorSpy(
        &controller,
        &PDFEditController::strikeoutColorChanged);

    controller.setHighlightColor(highlightColor);
    QCOMPARE(controller.highlightColor(), highlightColor);
    QCOMPARE(controller.underlineColor(), defaultUnderlineColor);
    QCOMPARE(controller.squigglyColor(), defaultSquigglyColor);
    QCOMPARE(controller.strikeoutColor(), defaultStrikeoutColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    QCOMPARE(underlineColorSpy.count(), 0);
    QCOMPARE(squigglyColorSpy.count(), 0);
    QCOMPARE(strikeoutColorSpy.count(), 0);

    controller.setUnderlineColor(underlineColor);
    QCOMPARE(controller.highlightColor(), highlightColor);
    QCOMPARE(controller.underlineColor(), underlineColor);
    QCOMPARE(controller.squigglyColor(), defaultSquigglyColor);
    QCOMPARE(controller.strikeoutColor(), defaultStrikeoutColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    QCOMPARE(underlineColorSpy.count(), 1);
    QCOMPARE(squigglyColorSpy.count(), 0);
    QCOMPARE(strikeoutColorSpy.count(), 0);
    controller.setUnderlineColor(underlineColor);
    QCOMPARE(underlineColorSpy.count(), 1);

    controller.setSquigglyColor(squigglyColor);
    QCOMPARE(controller.highlightColor(), highlightColor);
    QCOMPARE(controller.underlineColor(), underlineColor);
    QCOMPARE(controller.squigglyColor(), squigglyColor);
    QCOMPARE(controller.strikeoutColor(), defaultStrikeoutColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    QCOMPARE(underlineColorSpy.count(), 1);
    QCOMPARE(squigglyColorSpy.count(), 1);
    QCOMPARE(strikeoutColorSpy.count(), 0);
    controller.setSquigglyColor(squigglyColor);
    QCOMPARE(squigglyColorSpy.count(), 1);

    controller.setStrikeoutColor(strikeoutColor);
    QCOMPARE(controller.highlightColor(), highlightColor);
    QCOMPARE(controller.underlineColor(), underlineColor);
    QCOMPARE(controller.squigglyColor(), squigglyColor);
    QCOMPARE(controller.strikeoutColor(), strikeoutColor);
    QCOMPARE(highlightColorSpy.count(), 1);
    QCOMPARE(underlineColorSpy.count(), 1);
    QCOMPARE(squigglyColorSpy.count(), 1);
    QCOMPARE(strikeoutColorSpy.count(), 1);
    controller.setStrikeoutColor(strikeoutColor);
    QCOMPARE(strikeoutColorSpy.count(), 1);

    controller.setTool(PDFEditController::HighlightTool);
    const QVariantMap highlight = controller.addHighlight(
        0,
        QRectF(0.10, 0.20, 0.30, 0.05));
    QCOMPARE(
        highlight.value(QStringLiteral("highlightColor")).value<QColor>(),
        highlightColor);

    controller.setTool(PDFEditController::UnderlineTool);
    const QVariantMap underline = controller.addHighlight(
        0,
        QRectF(0.12, 0.30, 0.30, 0.05));
    QCOMPARE(
        underline.value(QStringLiteral("kind")).toString(),
        QStringLiteral("underline"));
    QCOMPARE(
        underline.value(QStringLiteral("highlightColor")).value<QColor>(),
        underlineColor);

    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap squiggly = controller.addHighlight(
        0,
        textCandidate.value(QStringLiteral("sourceRect")).toRectF());
    QCOMPARE(
        squiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QCOMPARE(
        squiggly.value(QStringLiteral("highlightColor")).value<QColor>(),
        squigglyColor);

    controller.setTool(PDFEditController::StrikeoutTool);
    const QVariantMap strikeout = controller.addHighlight(
        0,
        QRectF(0.14, 0.40, 0.30, 0.05));
    QCOMPARE(
        strikeout.value(QStringLiteral("kind")).toString(),
        QStringLiteral("strikeout"));
    QCOMPARE(
        strikeout.value(QStringLiteral("highlightColor")).value<QColor>(),
        strikeoutColor);
    QCOMPARE(controller.draftsForPage(0).size(), 4);
}

void PDFEditControllerTest::requiresSourceTextOnlyForSquiggly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("squiggly-text-hit-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);
    const QColor queuedSquigglyColor(73, 91, 211, 187);
    controller.setSquigglyColor(queuedSquigglyColor);
    QSignalSpy statusSpy(
        &controller,
        &PDFEditController::statusMessage);
    const QVariantMap pendingSquiggly = controller.addHighlight(
        0,
        QRectF(0.05, 0.05, 0.90, 0.90));
    QCOMPARE(
        pendingSquiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.hasDrafts());
    QCOMPARE(statusSpy.count(), 0);

    controller.setTool(PDFEditController::HighlightTool);
    controller.setSquigglyColor(QColor(198, 40, 40, 192));

    const QVariantList candidates = waitForCandidates(controller, 0);
    QTRY_COMPARE_WITH_TIMEOUT(controller.draftsForPage(0).size(), 1, 2000);
    const QVariantMap completedSquiggly = controller
        .draftsForPage(0)
        .constFirst()
        .toMap();
    QCOMPARE(
        completedSquiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QCOMPARE(
        completedSquiggly
            .value(QStringLiteral("highlightColor"))
            .value<QColor>(),
        queuedSquigglyColor);
    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());

    const QVariantMap groupedCandidate = candidateForText(
        candidates,
        QString::fromLatin1(kGroupedText));
    QVERIFY(!groupedCandidate.isEmpty());
    const QVariantList sourceRects = groupedCandidate
        .value(QStringLiteral("sourceRects"))
        .toList();
    QCOMPARE(sourceRects.size(), 2);
    QRectF leftWordRect = rectFromCoordinateMap(sourceRects.at(0));
    QRectF rightWordRect = rectFromCoordinateMap(sourceRects.at(1));
    if (leftWordRect.left() > rightWordRect.left()) {
        std::swap(leftWordRect, rightWordRect);
    }
    const qreal gapWidth = rightWordRect.left() - leftWordRect.right();
    const qreal gapTop = qMax(leftWordRect.top(), rightWordRect.top());
    const qreal gapBottom = qMin(
        leftWordRect.bottom(),
        rightWordRect.bottom());
    QVERIFY(gapWidth > 0.0);
    QVERIFY(gapBottom > gapTop);
    const qreal horizontalInset = gapWidth * 0.20;
    const QRectF whitespaceRect(
        leftWordRect.right() + horizontalInset,
        gapTop,
        gapWidth - 2.0 * horizontalInset,
        gapBottom - gapTop);
    QVERIFY(whitespaceRect.isValid());
    QVERIFY(controller
        .textMarkupRectsForSelection(0, whitespaceRect)
        .isEmpty());

    controller.setTool(PDFEditController::SquigglyTool);
    statusSpy.clear();
    QVERIFY(controller.addHighlight(0, whitespaceRect).isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(
        statusSpy.constFirst().constFirst().toString(),
        qtTrId("NoTextInSelectedArea"));

    constexpr qreal thinSelectionHeight = 0.00025;
    const QRectF thinTextSelection(
        leftWordRect.left(),
        leftWordRect.center().y() - thinSelectionHeight / 2.0,
        leftWordRect.width(),
        thinSelectionHeight);
    const QVariantMap thinSquiggly = controller.addHighlight(
        0,
        thinTextSelection);
    QCOMPARE(
        thinSquiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());

    struct MarkupCase {
        PDFEditController::Tool tool;
        QString kind;
    };
    const MarkupCase freeformCases[] = {
        {PDFEditController::HighlightTool, QStringLiteral("highlight")},
        {PDFEditController::UnderlineTool, QStringLiteral("underline")},
        {PDFEditController::StrikeoutTool, QStringLiteral("strikeout")},
    };
    for (const MarkupCase &markupCase : freeformCases) {
        controller.setTool(markupCase.tool);
        const QVariantMap annotation = controller.addHighlight(
            0,
            whitespaceRect);
        QCOMPARE(
            annotation.value(QStringLiteral("kind")).toString(),
            markupCase.kind);
        QCOMPARE(
            annotation.value(QStringLiteral("sourceRect")).toRectF(),
            whitespaceRect);
        QCOMPARE(controller.draftsForPage(0).size(), 1);
        QCOMPARE(controller.undo(), -1);
        QVERIFY(controller.draftsForPage(0).isEmpty());
    }
}

void PDFEditControllerTest::prioritizesPendingSquigglyCandidateScan()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("pending-squiggly-scan-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("pending-squiggly-scan-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);
    QSignalSpy statusSpy(
        &controller,
        &PDFEditController::statusMessage);

    QVERIFY(controller.candidatesForPage(1).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        0,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.hasDrafts());

    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(
        statusSpy.constFirst().constFirst().toString(),
        qtTrId("TextGeometryStillLoading"));
    statusSpy.clear();

    controller.candidatesForPage(2);
    controller.candidatesForPage(1);
    QTRY_COMPARE_WITH_TIMEOUT(controller.draftsForPage(0).size(), 1, 5000);
    const QVariantMap completedSquiggly = controller
        .draftsForPage(0)
        .constFirst()
        .toMap();
    QCOMPARE(
        completedSquiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QVERIFY(!completedSquiggly
        .value(QStringLiteral("highlightRects"))
        .toList()
        .isEmpty());
    QCOMPARE(statusSpy.count(), 0);
}

void PDFEditControllerTest::resumesNormalCandidateScanAfterPendingSquigglyPriority()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("pending-squiggly-resumes-normal-scan-source.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);
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

    QVariantList runningNormalCandidates;
    QVariantMap pendingSquiggly;
    QVariantList deferredNormalCandidates;
    if (holderStarted) {
        // Page 1 is guaranteed to remain in flight until the priority and
        // deferred requests below have both been registered.
        runningNormalCandidates = controller.candidatesForPage(1);
        pendingSquiggly = controller.addHighlight(
            0,
            QRectF(0.01, 0.01, 0.98, 0.40));

        // Request page 2 exactly once. The scheduler must defer it while page
        // 0 preempts page 1, then resume it without another caller request.
        deferredNormalCandidates = controller.candidatesForPage(2);
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
    QVERIFY(runningNormalCandidates.isEmpty());
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(deferredNormalCandidates.isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());

    const auto readyPages = [&candidatesReadySpy]() {
        QVector<int> pages;
        pages.reserve(candidatesReadySpy.size());
        for (const QList<QVariant> &arguments : candidatesReadySpy) {
            pages.append(arguments.constFirst().toInt());
        }
        return pages;
    };
    QTRY_VERIFY_WITH_TIMEOUT(
        readyPages().contains(0) && readyPages().contains(2),
        5000);

    const QVector<int> completedPages = readyPages();
    QVERIFY(completedPages.indexOf(0) < completedPages.indexOf(2));
    QVERIFY(!completedPages.contains(1));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(!controller.candidatesForPage(0).isEmpty());
    QVERIFY(!controller.candidatesForPage(2).isEmpty());
}

void PDFEditControllerTest::discardsPendingSquigglyBeforeCandidateScanCompletes()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("discard-pending-squiggly-source.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);

    QVERIFY(controller.candidatesForPage(1).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        0,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.hasDrafts());

    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!waitForCandidates(controller, 0).isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::preservesTextEditorWhenPendingSquigglyCompletes()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("pending-squiggly-preserves-editor-source.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    const QVariantMap editCandidate = candidateForText(
        waitForCandidates(controller, 2),
        QStringLiteral("Third fixture page"));
    QVERIFY(!editCandidate.isEmpty());

    controller.setTool(PDFEditController::SquigglyTool);
    QVERIFY(controller.candidatesForPage(1).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        0,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.draftsForPage(0).isEmpty());

    const QString selectedObjectId = editCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    controller.setTool(PDFEditController::EditTextTool);
    QVERIFY(controller.selectCandidate(2, selectedObjectId));
    controller.beginSelectedTextEdit();
    QVERIFY(controller.textEditActive());
    QCOMPARE(controller.selectedPage(), 2);
    QCOMPARE(controller.selectedObjectId(), selectedObjectId);

    QTRY_COMPARE_WITH_TIMEOUT(controller.draftsForPage(0).size(), 1, 5000);
    QVERIFY(controller.textEditActive());
    QCOMPARE(controller.selectedPage(), 2);
    QCOMPARE(controller.selectedObjectId(), selectedObjectId);
    QCOMPARE(controller.selectedText(), QStringLiteral("Third fixture page"));
}

void PDFEditControllerTest::remapsPendingSquigglyAcrossPageInsertionUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("remap-pending-squiggly-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("remap-pending-squiggly-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);

    // Keep the single candidate-scan worker occupied so page 1 stays pending
    // while the page-structure commands below remap its display index.
    QVERIFY(controller.candidatesForPage(0).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        1,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.draftsForPage(1).isEmpty());
    QVERIFY(controller.hasDrafts());

    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 2);
    QVERIFY(controller.draftsForPage(2).isEmpty());

    QCOMPARE(controller.undo(), 0);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 1);
    QCOMPARE(controller.redo(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 2);

    QTRY_COMPARE_WITH_TIMEOUT(controller.draftsForPage(2).size(), 1, 5000);
    QVERIFY(controller.draftsForPage(1).isEmpty());
    const QVariantMap completedSquiggly = controller
        .draftsForPage(2)
        .constFirst()
        .toMap();
    QCOMPARE(
        completedSquiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QVERIFY(!completedSquiggly
        .value(QStringLiteral("highlightRects"))
        .toList()
        .isEmpty());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.pageCount(), 4);
    QVERIFY(normalizedPageText(outputPath, 2).contains(
        QStringLiteral("Second fixture page")));
}

void PDFEditControllerTest::cancelsPendingSquigglyWhenSourcePageIsDeleted()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("cancel-deleted-page-pending-squiggly-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("cancel-deleted-page-pending-squiggly-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SquigglyTool);

    QVERIFY(controller.candidatesForPage(0).isEmpty());
    const QVariantMap pendingSquiggly = controller.addHighlight(
        1,
        QRectF(0.01, 0.01, 0.98, 0.40));
    QVERIFY(pendingSquiggly.value(QStringLiteral("pending")).toBool());
    QVERIFY(controller.hasDrafts());

    document.setCurrentPage(1);
    QCOMPARE(controller.deleteCurrentPage(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), -1);
    QVERIFY(controller.hasDrafts());

    // Undo restores the source page, but the annotation request was explicitly
    // cancelled when that page was deleted and must not be resurrected by a
    // late candidate-scan result.
    QCOMPARE(controller.undo(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!waitForCandidates(controller, 1).isEmpty());
    QVERIFY(controller.draftsForPage(1).isEmpty());
    QVERIFY(!controller.hasDrafts());

    QCOMPARE(controller.redo(), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(1), -1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.pageCount(), 2);
    QVERIFY(!normalizedPageText(outputPath, 0).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 1).contains(
        QStringLiteral("Second fixture page")));
}

void PDFEditControllerTest::keepsSquigglyGapGroupingStableAcrossPageAspectRatios()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("squiggly-aspect-ratio-source.pdf"));
    QVERIFY(createSquigglyAspectRatioPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(document.pageCount(), 2);
    controller.setDocument(&document);

    QVector<QVariantList> markupRectsByPage;
    QVector<QRectF> physicalBoundsByPage;
    for (int page = 0; page < document.pageCount(); ++page) {
        QVERIFY(!waitForCandidates(controller, page).isEmpty());
        const QVariantList markupRects
            = controller.textMarkupRectsForSelection(
                page,
                QRectF(0.0, 0.0, 1.0, 1.0));
        markupRectsByPage.append(markupRects);
        if (markupRects.size() != 1) {
            continue;
        }

        const QRectF normalizedBounds = rectFromCoordinateMap(
            markupRects.constFirst());
        const QSizeF pageSize = document.pageSize(page);
        physicalBoundsByPage.append(QRectF(
            normalizedBounds.x() * pageSize.width(),
            (1.0 - normalizedBounds.bottom()) * pageSize.height(),
            normalizedBounds.width() * pageSize.width(),
            normalizedBounds.height() * pageSize.height()));
    }

    QCOMPARE(markupRectsByPage.size(), 2);
    QCOMPARE(markupRectsByPage.at(0).size(), markupRectsByPage.at(1).size());
    QCOMPARE(markupRectsByPage.at(0).size(), 1);
    QCOMPARE(physicalBoundsByPage.size(), 2);
    constexpr qreal physicalTolerancePoints = 0.01;
    const QRectF portraitBounds = physicalBoundsByPage.at(0);
    const QRectF landscapeBounds = physicalBoundsByPage.at(1);
    QVERIFY(qAbs(portraitBounds.left() - landscapeBounds.left())
        < physicalTolerancePoints);
    QVERIFY(qAbs(portraitBounds.top() - landscapeBounds.top())
        < physicalTolerancePoints);
    QVERIFY(qAbs(portraitBounds.width() - landscapeBounds.width())
        < physicalTolerancePoints);
    QVERIFY(qAbs(portraitBounds.height() - landscapeBounds.height())
        < physicalTolerancePoints);
}

void PDFEditControllerTest::snapsSquigglyToSourceTextAndSavesQuadPoints()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("snap-squiggly-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("snap-squiggly-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const QVariantMap firstLine = candidateForText(
        candidates,
        QString::fromLatin1(kRegularText));
    const QVariantMap secondLine = candidateForText(
        candidates,
        QString::fromLatin1(kBoldText));
    QVERIFY(!firstLine.isEmpty());
    QVERIFY(!secondLine.isEmpty());

    const QRectF firstLineRect = firstLine
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QRectF secondLineRect = secondLine
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QRectF selectionRect = firstLineRect.united(secondLineRect);
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QCOMPARE(snappedRects.size(), 2);
    const QRectF firstSnappedRect = rectFromCoordinateMap(
        snappedRects.at(0));
    const QRectF secondSnappedRect = rectFromCoordinateMap(
        snappedRects.at(1));
    QVERIFY(firstSnappedRect.isValid());
    QVERIFY(secondSnappedRect.isValid());
    QVERIFY(firstSnappedRect.intersects(firstLineRect));
    QVERIFY(secondSnappedRect.intersects(secondLineRect));
    QVERIFY(firstSnappedRect.bottom() < secondSnappedRect.top());

    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(pageSize.width() > 0.0);
    QVERIFY(pageSize.height() > 0.0);
    const QRectF firstGlyphBounds = normalizedGlyphBoundsForText(
        sourcePath,
        QString::fromLatin1(kRegularText));
    const QRectF secondGlyphBounds = normalizedGlyphBoundsForText(
        sourcePath,
        QString::fromLatin1(kBoldText));
    QVERIFY(firstGlyphBounds.isValid());
    QVERIFY(secondGlyphBounds.isValid());
    constexpr qreal expectedBottomPaddingPoints = 3.5;
    const qreal expectedGapRatio
        = expectedBottomPaddingPoints / pageSize.height();
    constexpr qreal normalizedTolerance = 0.00001;
    QVERIFY(qAbs(firstSnappedRect.left() - firstGlyphBounds.left())
        < normalizedTolerance);
    QVERIFY(qAbs(firstSnappedRect.top() - firstGlyphBounds.top())
        < normalizedTolerance);
    QVERIFY(qAbs(firstSnappedRect.right() - firstGlyphBounds.right())
        < normalizedTolerance);
    QVERIFY(qAbs(firstSnappedRect.bottom()
                 - qMin<qreal>(1.0,
                               firstGlyphBounds.bottom()
                                   + expectedGapRatio))
        < normalizedTolerance);
    QVERIFY(qAbs(secondSnappedRect.left() - secondGlyphBounds.left())
        < normalizedTolerance);
    QVERIFY(qAbs(secondSnappedRect.top() - secondGlyphBounds.top())
        < normalizedTolerance);
    QVERIFY(qAbs(secondSnappedRect.right() - secondGlyphBounds.right())
        < normalizedTolerance);
    QVERIFY(qAbs(secondSnappedRect.bottom()
                 - qMin<qreal>(1.0,
                               secondGlyphBounds.bottom()
                                   + expectedGapRatio))
        < normalizedTolerance);

    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap squiggly = controller.addHighlight(
        0,
        selectionRect);
    QVERIFY(!squiggly.isEmpty());
    QCOMPARE(
        squiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QCOMPARE(
        squiggly.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);
    QCOMPARE(
        squiggly.value(QStringLiteral("sourceRect")).toRectF(),
        boundingRectForCoordinateMaps(snappedRects));
    QCOMPARE(controller.draftsForPage(0).size(), 1);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const HighlightSaveInspection inspection
        = inspectTextMarkupAnnotations(outputPath, FPDF_ANNOT_SQUIGGLY);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.quadPoints.size(), snappedRects.size());

    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    const QVector<QRectF> sourceGlyphBounds{
        firstGlyphBounds,
        secondGlyphBounds};
    QVector<bool> matchedQuads(inspection.quadPoints.size(), false);
    for (qsizetype rectIndex = 0;
         rectIndex < snappedRects.size();
         ++rectIndex) {
        const QRectF rect = rectFromCoordinateMap(
            snappedRects.at(rectIndex));
        const qreal expectedLeft = rect.left() * pageSize.width();
        const qreal expectedRight = rect.right() * pageSize.width();
        const qreal expectedTop = (1.0 - rect.top()) * pageSize.height();
        const qreal expectedBottom = (1.0 - rect.bottom())
            * pageSize.height();
        bool matched = false;
        FS_QUADPOINTSF matchedQuad{};
        for (qsizetype index = 0;
             index < inspection.quadPoints.size();
             ++index) {
            if (matchedQuads.at(index)) {
                continue;
            }
            const FS_QUADPOINTSF quad = inspection.quadPoints.at(index);
            if (!closeEnough(quad.x1, expectedLeft)
                || !closeEnough(quad.y1, expectedTop)
                || !closeEnough(quad.x2, expectedRight)
                || !closeEnough(quad.y2, expectedTop)
                || !closeEnough(quad.x3, expectedLeft)
                || !closeEnough(quad.y3, expectedBottom)
                || !closeEnough(quad.x4, expectedRight)
                || !closeEnough(quad.y4, expectedBottom)) {
                continue;
            }
            matchedQuads[index] = true;
            matched = true;
            matchedQuad = quad;
            break;
        }
        QVERIFY(matched);
        const qreal unpaddedBottom = (1.0
            - sourceGlyphBounds.at(rectIndex).bottom())
            * pageSize.height();
        QVERIFY(closeEnough(
            matchedQuad.y3,
            unpaddedBottom - expectedBottomPaddingPoints));
        QVERIFY(closeEnough(
            matchedQuad.y4,
            unpaddedBottom - expectedBottomPaddingPoints));
    }
}

void PDFEditControllerTest::clipsSquigglyGapAtPageBottom()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("bottom-edge-squiggly-source.pdf"));
    QVERIFY(createBottomEdgeTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kBottomEdgeText));
    QVERIFY(!textCandidate.isEmpty());

    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(pageSize.height() > 0.0);
    const QRectF glyphBounds = normalizedGlyphBoundsForText(
        sourcePath,
        QString::fromLatin1(kBottomEdgeText));
    QVERIFY(glyphBounds.isValid());
    constexpr qreal expectedBottomPaddingPoints = 3.5;
    const qreal expectedGapRatio
        = expectedBottomPaddingPoints / pageSize.height();
    QVERIFY(glyphBounds.bottom() < 1.0);
    QVERIFY(glyphBounds.bottom() + expectedGapRatio > 1.0);

    const QRectF selectionRect = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QCOMPARE(snappedRects.size(), 1);
    const QRectF snappedRect = rectFromCoordinateMap(
        snappedRects.constFirst());
    constexpr qreal normalizedTolerance = 0.00001;
    QVERIFY(qAbs(snappedRect.left() - glyphBounds.left())
        < normalizedTolerance);
    QVERIFY(qAbs(snappedRect.top() - glyphBounds.top())
        < normalizedTolerance);
    QVERIFY(qAbs(snappedRect.right() - glyphBounds.right())
        < normalizedTolerance);
    QVERIFY(qAbs(snappedRect.bottom() - 1.0) < normalizedTolerance);

    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap squiggly = controller.addHighlight(
        0,
        selectionRect);
    QVERIFY(!squiggly.isEmpty());
    QCOMPARE(
        squiggly.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);
}

void PDFEditControllerTest::usesInvisibleOcrTextForSquiggly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("invisible-ocr-squiggly-source.pdf"));
    QVERIFY(createInvisibleOcrTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    QSignalSpy candidatesReadySpy(
        &controller,
        &PDFEditController::pageCandidatesReady);
    QVariantList candidates = controller.candidatesForPage(0);
    if (candidatesReadySpy.isEmpty()) {
        QVERIFY(candidatesReadySpy.wait(2000));
    }
    QVERIFY(!candidatesReadySpy.isEmpty());
    QCOMPARE(
        candidatesReadySpy.constLast().constFirst().toInt(),
        0);
    candidates = controller.candidatesForPage(0);

    QVERIFY(candidateForText(
        candidates,
        QStringLiteral("Hidden OCR text")).isEmpty());
    const QVariantMap visibleCandidate = candidateForText(
        candidates,
        QStringLiteral("Visible text"));
    QVERIFY(!visibleCandidate.isEmpty());

    const QRectF hiddenSearchRect(0.08, 0.16, 0.62, 0.16);
    const QVariantList hiddenMarkupRects
        = controller.textMarkupRectsForSelection(0, hiddenSearchRect);
    QVERIFY(!hiddenMarkupRects.isEmpty());
    const QRectF firstHiddenGlyph = rectFromCoordinateMap(
        hiddenMarkupRects.constFirst());
    QVERIFY(firstHiddenGlyph.isValid());
    QVERIFY(hiddenSearchRect.intersects(firstHiddenGlyph));
    QVERIFY(controller.candidateAt(
        0,
        firstHiddenGlyph.center().x(),
        firstHiddenGlyph.center().y()).isEmpty());

    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap hiddenSquiggly = controller.addHighlight(
        0,
        hiddenSearchRect);
    QVERIFY(!hiddenSquiggly.isEmpty());
    QCOMPARE(
        hiddenSquiggly.value(QStringLiteral("kind")).toString(),
        QStringLiteral("squiggly"));
    QCOMPARE(
        hiddenSquiggly.value(QStringLiteral("highlightRects")).toList(),
        hiddenMarkupRects);
    QCOMPARE(controller.draftsForPage(0).size(), 1);

    const QRectF visibleRect = visibleCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList visibleMarkupRects
        = controller.textMarkupRectsForSelection(0, visibleRect);
    QVERIFY(!visibleMarkupRects.isEmpty());
    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap visibleSquiggly = controller.addHighlight(
        0,
        visibleRect);
    QVERIFY(!visibleSquiggly.isEmpty());
    QCOMPARE(
        visibleSquiggly.value(QStringLiteral("highlightRects")).toList(),
        visibleMarkupRects);
    QCOMPARE(controller.draftsForPage(0).size(), 2);
}

void PDFEditControllerTest::rejectsVerticalTextForSquiggly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("vertical-text-squiggly-source.pdf"));
    QVERIFY(createVerticalTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const QVariantMap uprightCandidate = candidateForText(
        candidates,
        QStringLiteral("Upright text"));
    const QVariantMap verticalCandidate = candidateForText(
        candidates,
        QStringLiteral("Vertical text"));
    QVERIFY(!uprightCandidate.isEmpty());
    QVERIFY(!verticalCandidate.isEmpty());

    const QRectF verticalRect = verticalCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    QCOMPARE(
        controller.candidateAt(
            0,
            verticalRect.center().x(),
            verticalRect.center().y())
            .value(QStringLiteral("text"))
            .toString()
            .trimmed(),
        QStringLiteral("Vertical text"));
    QVERIFY(controller
        .textMarkupRectsForSelection(0, verticalRect)
        .isEmpty());

    controller.setTool(PDFEditController::SquigglyTool);
    QSignalSpy statusSpy(
        &controller,
        &PDFEditController::statusMessage);
    QVERIFY(controller.addHighlight(0, verticalRect).isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(
        statusSpy.constFirst().constFirst().toString(),
        qtTrId("NoTextInSelectedArea"));

    const QRectF uprightRect = uprightCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList uprightMarkupRects
        = controller.textMarkupRectsForSelection(0, uprightRect);
    QVERIFY(!uprightMarkupRects.isEmpty());
    controller.setTool(PDFEditController::SquigglyTool);
    const QVariantMap uprightSquiggly = controller.addHighlight(
        0,
        uprightRect);
    QVERIFY(!uprightSquiggly.isEmpty());
    QCOMPARE(
        uprightSquiggly.value(QStringLiteral("highlightRects")).toList(),
        uprightMarkupRects);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
}

void PDFEditControllerTest::matchesSquigglyPreviewGeometryToPdfium()
{
    const QString componentPath = QFINDTESTDATA(
        "../src/Components/SquigglyLine.qml");
    QVERIFY(!componentPath.isEmpty());

    QQmlEngine engine;
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(componentPath));
    const QVariantMap initialProperties{
        {QStringLiteral("waveColor"), QColor(Qt::red)},
        {QStringLiteral("pagePointScale"), 2.0},
        {QStringLiteral("width"), 10.0},
        {QStringLiteral("height"), 12.0}};
    std::unique_ptr<QObject> squiggly(
        component.createWithInitialProperties(initialProperties));
    QVERIFY2(squiggly, qPrintable(component.errorString()));

    QCOMPARE(
        squiggly->property("waveStrokeWidth").toReal(),
        2.0);
    QCOMPARE(squiggly->property("waveDelta").toReal(), 4.0);
    QCOMPARE(
        squiggly->property("wavePath").toString(),
        QStringLiteral("M 0 8 L 4 12 L 8 8 L 10 10"));

    QVERIFY(squiggly->setProperty("width", 3.0));
    QCOMPARE(
        squiggly->property("wavePath").toString(),
        QStringLiteral("M 0 8 L 3 11"));

    QVERIFY(squiggly->setProperty("width", 12.0));
    QCOMPARE(
        squiggly->property("wavePath").toString(),
        QStringLiteral("M 0 8 L 4 12 L 8 8 L 12 12"));

    QVERIFY(squiggly->setProperty("pagePointScale", 1.5));
    QVERIFY(squiggly->setProperty("width", 7.5));
    QVERIFY(squiggly->setProperty("height", 20.0));
    QCOMPARE(
        squiggly->property("waveStrokeWidth").toReal(),
        1.5);
    QCOMPARE(squiggly->property("waveDelta").toReal(), 3.0);
    QCOMPARE(
        squiggly->property("wavePath").toString(),
        QStringLiteral("M 0 17 L 3 20 L 6 17 L 7.5 18.5"));
}

void PDFEditControllerTest::matchesSquigglyPreviewRenderingToSavedPdfium()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The Squiggly preview needs a Qt Quick render screen.");
    }

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("squiggly-preview-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("squiggly-preview-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!textCandidate.isEmpty());
    const QRectF selectionRect = textCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    const QVariantList snappedRects
        = controller.textMarkupRectsForSelection(0, selectionRect);
    QCOMPARE(snappedRects.size(), 1);
    const QRectF squigglyRect = rectFromCoordinateMap(
        snappedRects.constFirst());

    controller.setTool(PDFEditController::SquigglyTool);
    controller.setSquigglyColor(QColor(230, 30, 30));
    const QVariantMap draft = controller.addHighlight(0, selectionRect);
    QCOMPARE(
        draft.value(QStringLiteral("highlightRects")).toList(),
        snappedRects);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const QString componentPath = QFINDTESTDATA(
        "../src/Components/SquigglyLine.qml");
    QVERIFY(!componentPath.isEmpty());
    QQmlEngine engine;
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(componentPath));
    const QSizeF pageSize = document.pageSize(0);
    for (const qreal pagePointScale : {1.0, 2.0}) {
        const QSize renderedPageSize(
            qRound(pageSize.width() * pagePointScale),
            qRound(pageSize.height() * pagePointScale));
        const QImage savedPage = renderFirstPage(
            outputPath,
            renderedPageSize,
            FPDF_ANNOT);
        QVERIFY(!savedPage.isNull());
        const QRect savedSearchArea = QRectF(
            squigglyRect.x() * savedPage.width(),
            squigglyRect.y() * savedPage.height(),
            squigglyRect.width() * savedPage.width(),
            squigglyRect.height() * savedPage.height())
            .toAlignedRect()
            .adjusted(-2, -2, 2, 2);
        const QRect savedBounds = redPixelBounds(
            savedPage,
            savedSearchArea);
        QVERIFY(savedBounds.isValid());

        const qreal previewWidth = squigglyRect.width()
            * pageSize.width()
            * pagePointScale;
        const qreal previewHeight = squigglyRect.height()
            * pageSize.height()
            * pagePointScale;
        const QSize previewImageSize(
            qMax(1, qCeil(previewWidth)),
            qMax(1, qCeil(previewHeight)));
        const QVariantMap initialProperties{
            {QStringLiteral("waveColor"), QColor(230, 30, 30)},
            {QStringLiteral("pagePointScale"), pagePointScale},
            {QStringLiteral("width"), previewWidth},
            {QStringLiteral("height"), previewHeight}};
        std::unique_ptr<QObject> previewObject(
            component.createWithInitialProperties(initialProperties));
        QVERIFY2(previewObject, qPrintable(component.errorString()));
        auto *previewItem = qobject_cast<QQuickItem *>(previewObject.get());
        QVERIFY(previewItem);

        QQuickWindow window;
        window.setColor(Qt::white);
        window.resize(previewImageSize);
        previewItem->setParentItem(window.contentItem());
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
        const QSharedPointer<QQuickItemGrabResult> grabResult
            = previewItem->grabToImage(previewImageSize);
        QVERIFY(grabResult);
        QSignalSpy readySpy(
            grabResult.data(),
            &QQuickItemGrabResult::ready);
        if (grabResult->image().isNull()) {
            QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() > 0, 5000);
        }
        const QImage previewImage = grabResult->image();
        QVERIFY(!previewImage.isNull());
        const QRect previewBounds = redPixelBounds(previewImage);
        QVERIFY(previewBounds.isValid());

        const qreal savedBottom = squigglyRect.bottom()
            * savedPage.height();
        const qreal savedTop = squigglyRect.top()
            * savedPage.height();
        const qreal savedWaveTopOffset = savedBounds.top() - savedTop;
        const qreal savedWaveBottomOffset = savedBottom
            - (savedBounds.bottom() + 1);
        const qreal previewWaveTopOffset = previewBounds.top();
        const qreal previewWaveBottomOffset = previewImage.height()
            - (previewBounds.bottom() + 1);
        // Qt Quick and PDFium rasterize the same point geometry independently;
        // allow one antialiased edge pixel on each side of the wave band.
        QVERIFY2(
            qAbs(savedBounds.height() - previewBounds.height()) <= 2,
            qPrintable(QStringLiteral(
                "scale=%1 savedBounds=%2,%3 %4x%5 "
                "previewBounds=%6,%7 %8x%9")
                .arg(pagePointScale)
                .arg(savedBounds.x())
                .arg(savedBounds.y())
                .arg(savedBounds.width())
                .arg(savedBounds.height())
                .arg(previewBounds.x())
                .arg(previewBounds.y())
                .arg(previewBounds.width())
                .arg(previewBounds.height())));
        QVERIFY(qAbs(savedWaveTopOffset - previewWaveTopOffset) <= 2.0);
        QVERIFY(qAbs(savedWaveBottomOffset - previewWaveBottomOffset) <= 2.0);
    }
}

void PDFEditControllerTest::addsUnderlineStrikeoutAndSquigglyWithUndoRedoAndDelete_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<QString>("kind");

    QTest::newRow("underline")
        << static_cast<int>(PDFEditController::UnderlineTool)
        << QStringLiteral("underline");
    QTest::newRow("strikeout")
        << static_cast<int>(PDFEditController::StrikeoutTool)
        << QStringLiteral("strikeout");
    QTest::newRow("squiggly")
        << static_cast<int>(PDFEditController::SquigglyTool)
        << QStringLiteral("squiggly");
}

void PDFEditControllerTest::addsUnderlineStrikeoutAndSquigglyWithUndoRedoAndDelete()
{
    QFETCH(int, tool);
    QFETCH(QString, kind);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-%1-source.pdf").arg(kind));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    QRectF selectionRect(0.12, 0.18, 0.28, 0.045);
    QRectF expectedSourceRect = selectionRect;
    if (tool == static_cast<int>(PDFEditController::SquigglyTool)) {
        const QVariantMap textCandidate = candidateForText(
            waitForCandidates(controller, 0),
            QString::fromLatin1(kRegularText));
        QVERIFY(!textCandidate.isEmpty());
        selectionRect = textCandidate
            .value(QStringLiteral("sourceRect"))
            .toRectF();
        const QVariantList snappedRects
            = controller.textMarkupRectsForSelection(0, selectionRect);
        QVERIFY(!snappedRects.isEmpty());
        expectedSourceRect = boundingRectForCoordinateMaps(snappedRects);
    }
    controller.setTool(static_cast<PDFEditController::Tool>(tool));
    const QVariantMap annotation = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(annotation.value(QStringLiteral("kind")).toString(), kind);
    QCOMPARE(
        annotation.value(QStringLiteral("sourceRect")).toRectF(),
        expectedSourceRect);
    QVERIFY(
        annotation.value(QStringLiteral("objectId")).toString().startsWith(
            kind + QLatin1Char(':')));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(!candidateForKind(controller.candidatesForPage(0), kind).isEmpty());

    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(candidateForKind(controller.candidatesForPage(0), kind).isEmpty());
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.draftsForPage(0).size(), 1);

    controller.setTool(PDFEditController::SelectTool);
    const QString objectId = annotation
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.selectCandidate(0, objectId));
    controller.deleteSelectedObject();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(candidateForKind(controller.candidatesForPage(0), kind).isEmpty());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        controller.draftsForPage(0)
            .constFirst()
            .toMap()
            .value(QStringLiteral("kind"))
            .toString(),
        kind);
    QCOMPARE(controller.redo(), -1);
    QVERIFY(controller.draftsForPage(0).isEmpty());
}

void PDFEditControllerTest::addsStickyNoteWithPanelModelAndUndo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-sticky-note-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::StickyNoteTool);

    const QVariantMap note = controller.addStickyNote(
        0,
        QPointF(0.2, 0.3));
    QCOMPARE(
        note.value(QStringLiteral("kind")).toString(),
        QStringLiteral("stickyNote"));
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    QCOMPARE(controller.selectedObjectId(),
             note.value(QStringLiteral("objectId")).toString());
    const QDateTime creationDate = note
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    QVERIFY(creationDate.isValid());
    QVERIFY(!note
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        note.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(
        controller.commentItems()
            .constFirst()
            .toMap()
            .value(QStringLiteral("page"))
            .toInt(),
        0);

    const QString commentText = QStringLiteral("Cần kiểm tra lại số liệu.");
    controller.setSelectedCommentText(commentText);
    QCOMPARE(controller.selectedText(), commentText);
    QCOMPARE(
        controller.commentItems()
            .constFirst()
            .toMap()
            .value(QStringLiteral("text"))
            .toString(),
        commentText);
    const QDateTime modificationDate = controller.commentItems()
        .constFirst()
        .toMap()
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime();
    QVERIFY(modificationDate.isValid());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.selectedText(), QString());
    const QVariantMap undoneNote = controller.commentItems()
        .constFirst()
        .toMap();
    QVERIFY(!undoneNote
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        undoneNote.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.selectedText(), commentText);
    const QVariantMap redoneNote = controller.commentItems()
        .constFirst()
        .toMap();
    QCOMPARE(
        redoneNote.value(QStringLiteral("commentModifiedDate")).toDateTime(),
        modificationDate);
    QCOMPARE(
        redoneNote.value(QStringLiteral("commentDate")).toDateTime(),
        modificationDate);
    QCOMPARE(controller.selectRelativeComment(1), 0);

    QVERIFY(controller.removeComment(
        0,
        note.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.commentItems().isEmpty());
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 1);
}

void PDFEditControllerTest::addsFreeTextWithPanelModelAndUndo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-free-text-source.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::FreeTextTool);

    const QRectF placement(0.12, 0.16, 0.32, 0.12);
    const QVariantMap freeText = controller.addFreeText(0, placement);
    QVERIFY(!freeText.isEmpty());
    QCOMPARE(
        freeText.value(QStringLiteral("kind")).toString(),
        QStringLiteral("freeText"));
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    const QString objectId = freeText
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!objectId.isEmpty());
    QCOMPARE(controller.selectedPage(), 0);
    QCOMPARE(controller.selectedObjectId(), objectId);
    QCOMPARE(controller.commentItems().size(), 1);

    const QVariantMap initialItem = controller.commentItems()
        .constFirst()
        .toMap();
    QCOMPARE(
        initialItem.value(QStringLiteral("kind")).toString(),
        QStringLiteral("freeText"));
    QVERIFY(initialItem
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    QVERIFY(initialItem
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime()
        .isValid());

    const QString commentText
        = QStringLiteral("Nội dung Free Text có dấu tiếng Việt.");
    QVERIFY(controller.setCommentText(0, objectId, commentText));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        commentText);
    QVERIFY(controller.hasDrafts());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QString());
    QCOMPARE(controller.undo(), -1);
    QVERIFY(controller.commentItems().isEmpty());
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        commentText);

    QVERIFY(controller.removeComment(0, objectId));
    QVERIFY(controller.commentItems().isEmpty());
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 1);
    controller.discardDrafts();
    QVERIFY(controller.commentItems().isEmpty());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::addsCommentReplyWithOneLevelModelAndUndo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-comment-reply-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::StickyNoteTool);
    const QVariantMap root = controller.addStickyNote(
        0,
        QPointF(0.2, 0.3));
    QVERIFY(!root.isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Root comment"));
    const QString rootObjectId = root
        .value(QStringLiteral("objectId"))
        .toString();

    const QString replyText = QStringLiteral("First-level reply");
    const QVariantMap reply = controller.addCommentReply(
        0,
        rootObjectId,
        replyText);
    QVERIFY(!reply.isEmpty());
    const QString replyObjectId = reply
        .value(QStringLiteral("objectId"))
        .toString();
    QCOMPARE(
        reply.value(QStringLiteral("kind")).toString(),
        QStringLiteral("stickyNote"));
    QCOMPARE(
        reply.value(QStringLiteral("parentObjectId")).toString(),
        rootObjectId);
    QVERIFY(reply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(controller.selectedCommentReply());

    const QVariantList pageDrafts = controller.draftsForPage(0);
    QCOMPARE(pageDrafts.size(), 1);
    QCOMPARE(
        pageDrafts.constFirst().toMap()
            .value(QStringLiteral("objectId")).toString(),
        rootObjectId);
    const QVariantList pageCandidates = controller.candidatesForPage(0);
    for (const QVariant &pageCandidate : pageCandidates) {
        QVERIFY(
            pageCandidate.toMap()
                .value(QStringLiteral("objectId")).toString()
            != replyObjectId);
    }

    QVariantList comments = controller.commentItems();
    QCOMPARE(comments.size(), 2);
    const QVariantMap rootItem = comments.at(0).toMap();
    const QVariantMap replyItem = comments.at(1).toMap();
    QVERIFY(!rootItem.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(rootItem.value(QStringLiteral("replyCount")).toInt(), 1);
    QVERIFY(replyItem.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        replyItem.value(QStringLiteral("parentObjectId")).toString(),
        rootObjectId);
    QCOMPARE(
        replyItem.value(QStringLiteral("text")).toString(),
        replyText);

    QVERIFY(controller.addCommentReply(
        0,
        replyItem.value(QStringLiteral("objectId")).toString(),
        QStringLiteral("Rejected nested reply")).isEmpty());
    QVERIFY(controller.addCommentReply(
        0,
        rootObjectId,
        QStringLiteral("   \n  ")).isEmpty());
    QCOMPARE(controller.commentItems().size(), 2);

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.commentItems().size(), 2);

    QVERIFY(controller.removeComment(0, replyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("replyCount")).toInt(),
        0);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 2);

    QVERIFY(controller.selectComment(0, replyObjectId));
    QCOMPARE(controller.selectedObjectId(), replyObjectId);
    QVERIFY(controller.removeComment(0, rootObjectId));
    QVERIFY(controller.commentItems().isEmpty());
    QVERIFY(controller.selectedObjectId().isEmpty());
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 2);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("replyCount")).toInt(),
        1);
}

void PDFEditControllerTest::editsSourceStickyNoteComment()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-sticky-note.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-sticky-note-output.pdf"));
    QVERIFY(createStickyNotePopupPdf(sourcePath));

    const CommentAnnotationInspection sourceInspection
        = inspectCommentAnnotation(
            sourcePath,
            QStringLiteral("preserved-popup-comment"));
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.annotationCount, 4);
    QCOMPARE(sourceInspection.subtype, FPDF_ANNOT_TEXT);
    QVERIFY(sourceInspection.richContents.contains(
        sourceInspection.contents));
    QVERIFY(sourceInspection.hasAppearance);
    QVERIFY(!sourceInspection.appearance.isEmpty());
    QVERIFY(sourceInspection.popupIndex >= 0);
    QVERIFY(sourceInspection.popupParentMatches);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const QVariantMap sourceComment = controller.commentItems().at(1).toMap();
    QCOMPARE(
        sourceComment.value(QStringLiteral("text")).toString(),
        sourceInspection.contents);
    QVERIFY(!sourceComment.value(QStringLiteral("editable")).toBool());
    QVERIFY(sourceComment
        .value(QStringLiteral("commentTextEditable")).toBool());
    const QString objectId = sourceComment
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.selectComment(0, objectId));

    const QString draftText = QStringLiteral("Edited source sticky note");
    controller.setSelectedCommentText(draftText);
    QCOMPARE(controller.selectedText(), draftText);
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        draftText);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        sourceInspection.contents);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());

    QCOMPARE(controller.redo(), -1);
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        draftText);
    QVERIFY(controller.hasDrafts());

    controller.discardDrafts();
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        sourceInspection.contents);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const QString savedText = QStringLiteral("Saved source sticky note");
    QVERIFY(controller.selectComment(0, objectId));
    controller.setSelectedCommentText(savedText);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection outputInspection
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("preserved-popup-comment"));
    QVERIFY(outputInspection.readable);
    QCOMPARE(
        outputInspection.annotationCount,
        sourceInspection.annotationCount);
    QCOMPARE(outputInspection.subtype, sourceInspection.subtype);
    QCOMPARE(outputInspection.contents, savedText);
    QVERIFY(outputInspection.richContents.contains(savedText));
    QVERIFY(!outputInspection.richContents.contains(
        sourceInspection.contents));
    QCOMPARE(outputInspection.name, sourceInspection.name);
    QCOMPARE(outputInspection.author, sourceInspection.author);
    QCOMPARE(
        outputInspection.creationDate,
        sourceInspection.creationDate);
    QVERIFY(!outputInspection.modificationDate.isEmpty());
    QVERIFY(
        outputInspection.modificationDate
        != sourceInspection.modificationDate);
    QCOMPARE(
        outputInspection.hasAppearance,
        sourceInspection.hasAppearance);
    QCOMPARE(outputInspection.appearance, sourceInspection.appearance);
    QVERIFY(outputInspection.popupIndex >= 0);
    QVERIFY(outputInspection.popupParentMatches);
    QVERIFY(qAbs(outputInspection.rect.left
                 - sourceInspection.rect.left) < 0.001);
    QVERIFY(qAbs(outputInspection.rect.right
                 - sourceInspection.rect.right) < 0.001);
    QVERIFY(qAbs(outputInspection.rect.top
                 - sourceInspection.rect.top) < 0.001);
    QVERIFY(qAbs(outputInspection.rect.bottom
                 - sourceInspection.rect.bottom) < 0.001);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        savedText,
        5000);
    const QVariantMap rehydratedComment
        = controller.commentItems().at(1).toMap();
    QCOMPARE(
        rehydratedComment.value(QStringLiteral("text")).toString(),
        savedText);
    QVERIFY(!rehydratedComment.value(QStringLiteral("editable")).toBool());
    QVERIFY(rehydratedComment
        .value(QStringLiteral("commentTextEditable")).toBool());
}

void PDFEditControllerTest::editsSourceTextMarkupComment_data()
{
    QTest::addColumn<QByteArray>("pdfSubtypeName");
    QTest::addColumn<int>("subtype");
    QTest::addColumn<QString>("annotationName");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QByteArray>("sourceText");
    QTest::addColumn<QString>("savedText");

    QTest::newRow("highlight")
        << QByteArray("Highlight")
        << static_cast<int>(FPDF_ANNOT_HIGHLIGHT)
        << QStringLiteral("editable-source-highlight")
        << QStringLiteral("highlight")
        << QByteArray("Source highlight comment")
        << QStringLiteral("Edited highlight comment");
    QTest::newRow("underline")
        << QByteArray("Underline")
        << static_cast<int>(FPDF_ANNOT_UNDERLINE)
        << QStringLiteral("editable-source-underline")
        << QStringLiteral("underline")
        << QByteArray("Source underline comment")
        << QStringLiteral("Edited underline comment");
    QTest::newRow("squiggly")
        << QByteArray("Squiggly")
        << static_cast<int>(FPDF_ANNOT_SQUIGGLY)
        << QStringLiteral("editable-source-squiggly")
        << QStringLiteral("squiggly")
        << QByteArray("Source squiggly comment")
        << QStringLiteral("Edited squiggly comment");
    QTest::newRow("strikeout")
        << QByteArray("StrikeOut")
        << static_cast<int>(FPDF_ANNOT_STRIKEOUT)
        << QStringLiteral("editable-source-strikeout")
        << QStringLiteral("strikeout")
        << QByteArray("Source strikeout comment")
        << QStringLiteral("Edited strikeout comment");
}

void PDFEditControllerTest::editsSourceTextMarkupComment()
{
    QFETCH(QByteArray, pdfSubtypeName);
    QFETCH(int, subtype);
    QFETCH(QString, annotationName);
    QFETCH(QString, kind);
    QFETCH(QByteArray, sourceText);
    QFETCH(QString, savedText);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-%1.pdf").arg(kind));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-%1-output.pdf").arg(kind));
    QVERIFY(createTextMarkupCommentPdf(
        sourcePath,
        pdfSubtypeName,
        annotationName.toLatin1(),
        sourceText));

    const CommentAnnotationInspection sourceInspection
        = inspectCommentAnnotation(
            sourcePath,
            annotationName);
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.annotationCount, 1);
    QCOMPARE(sourceInspection.subtype, subtype);
    QCOMPARE(sourceInspection.quadPoints.size(), 1);
    QVERIFY(sourceInspection.hasAppearance);
    QVERIFY(sourceInspection.appearance.contains(
        QStringLiteral("TrePDF source text markup appearance sentinel")));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    const QVariantMap sourceComment
        = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        sourceComment.value(QStringLiteral("kind")).toString(),
        kind);
    QCOMPARE(
        sourceComment
            .value(QStringLiteral("highlightColor"))
            .value<QColor>(),
        QColor(33, 69, 156, 255));
    QVERIFY(!sourceComment.value(QStringLiteral("editable")).toBool());
    QVERIFY(sourceComment
        .value(QStringLiteral("commentTextEditable")).toBool());
    const QString objectId = sourceComment
        .value(QStringLiteral("objectId"))
        .toString();

    QVERIFY(controller.setCommentText(0, objectId, savedText));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        savedText);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentAnnotationInspection outputInspection
        = inspectCommentAnnotation(
            outputPath,
            annotationName);
    QVERIFY(outputInspection.readable);
    QCOMPARE(
        outputInspection.annotationCount,
        sourceInspection.annotationCount);
    QCOMPARE(outputInspection.subtype, sourceInspection.subtype);
    QCOMPARE(outputInspection.contents, savedText);
    QCOMPARE(outputInspection.name, sourceInspection.name);
    QCOMPARE(outputInspection.author, sourceInspection.author);
    QCOMPARE(
        outputInspection.creationDate,
        sourceInspection.creationDate);
    QVERIFY(!outputInspection.modificationDate.isEmpty());
    QVERIFY(
        outputInspection.modificationDate
        != sourceInspection.modificationDate);
    QVERIFY(outputInspection.hasAppearance);
    QCOMPARE(outputInspection.appearance, sourceInspection.appearance);
    QVERIFY(qAbs(outputInspection.rect.left
                 - sourceInspection.rect.left) < 0.001);
    QVERIFY(qAbs(outputInspection.rect.right
                 - sourceInspection.rect.right) < 0.001);
    QVERIFY(qAbs(outputInspection.rect.top
                 - sourceInspection.rect.top) < 0.001);
    QVERIFY(qAbs(outputInspection.rect.bottom
                 - sourceInspection.rect.bottom) < 0.001);
    QCOMPARE(
        outputInspection.quadPoints.size(),
        sourceInspection.quadPoints.size());
    const FS_QUADPOINTSF sourceQuad
        = sourceInspection.quadPoints.constFirst();
    const FS_QUADPOINTSF outputQuad
        = outputInspection.quadPoints.constFirst();
    QVERIFY(qAbs(outputQuad.x1 - sourceQuad.x1) < 0.001);
    QVERIFY(qAbs(outputQuad.y1 - sourceQuad.y1) < 0.001);
    QVERIFY(qAbs(outputQuad.x2 - sourceQuad.x2) < 0.001);
    QVERIFY(qAbs(outputQuad.y2 - sourceQuad.y2) < 0.001);
    QVERIFY(qAbs(outputQuad.x3 - sourceQuad.x3) < 0.001);
    QVERIFY(qAbs(outputQuad.y3 - sourceQuad.y3) < 0.001);
    QVERIFY(qAbs(outputQuad.x4 - sourceQuad.x4) < 0.001);
    QVERIFY(qAbs(outputQuad.y4 - sourceQuad.y4) < 0.001);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        savedText,
        5000);
    const QVariantMap rehydratedComment
        = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        rehydratedComment.value(QStringLiteral("text")).toString(),
        savedText);
    QVERIFY(!rehydratedComment.value(QStringLiteral("editable")).toBool());
    QVERIFY(rehydratedComment
        .value(QStringLiteral("commentTextEditable")).toBool());
}

void PDFEditControllerTest::editsSourceReplyComment()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-reply.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("edit-source-reply-output.pdf"));
    QVERIFY(createNestedCommentRepliesPdf(sourcePath));

    const CommentAnnotationInspection sourceReply
        = inspectCommentAnnotation(
            sourcePath,
            QStringLiteral("first-reply"));
    const CommentAnnotationInspection sourceNestedReply
        = inspectCommentAnnotation(
            sourcePath,
            QStringLiteral("nested-reply"));
    QVERIFY(sourceReply.readable);
    QCOMPARE(sourceReply.annotationCount, 3);
    QCOMPARE(sourceReply.subtype, FPDF_ANNOT_TEXT);
    QCOMPARE(sourceReply.parentName, QStringLiteral("root-comment"));
    QCOMPARE(sourceReply.relation, QStringLiteral("R"));
    QVERIFY(!sourceReply.hasAppearance);
    QCOMPARE(sourceNestedReply.parentName, QStringLiteral("first-reply"));
    QCOMPARE(sourceNestedReply.relation, QStringLiteral("R"));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);
    const QVariantMap reply = controller.commentItems().at(1).toMap();
    QVERIFY(reply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(!reply.value(QStringLiteral("editable")).toBool());
    QVERIFY(reply.value(QStringLiteral("commentTextEditable")).toBool());
    const QString replyObjectId = reply
        .value(QStringLiteral("objectId"))
        .toString();

    const QString savedText = QStringLiteral("Edited source reply");
    QVERIFY(controller.setCommentText(0, replyObjectId, savedText));
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        savedText);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentAnnotationInspection outputReply
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("first-reply"));
    const CommentAnnotationInspection outputNestedReply
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("nested-reply"));
    QVERIFY(outputReply.readable);
    QCOMPARE(outputReply.annotationCount, sourceReply.annotationCount);
    QCOMPARE(outputReply.subtype, sourceReply.subtype);
    QCOMPARE(outputReply.contents, savedText);
    QCOMPARE(outputReply.parentName, sourceReply.parentName);
    QCOMPARE(outputReply.relation, QStringLiteral("R"));
    QVERIFY(outputReply.parentIndex >= 0);
    QVERIFY(outputReply.hasAppearance);
    QVERIFY(!outputReply.appearance.isEmpty());
    QVERIFY(!outputReply.modificationDate.isEmpty());
    QVERIFY(qAbs(outputReply.rect.left - sourceReply.rect.left) < 0.001);
    QVERIFY(qAbs(outputReply.rect.right - sourceReply.rect.right) < 0.001);
    QVERIFY(qAbs(outputReply.rect.top - sourceReply.rect.top) < 0.001);
    QVERIFY(qAbs(outputReply.rect.bottom - sourceReply.rect.bottom) < 0.001);
    QCOMPARE(outputNestedReply.contents, sourceNestedReply.contents);
    QCOMPARE(outputNestedReply.parentName, sourceNestedReply.parentName);
    QCOMPARE(outputNestedReply.relation, QStringLiteral("R"));
    QVERIFY(outputNestedReply.parentIndex >= 0);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);
    QVariantMap rehydratedReply;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        for (const QVariant &commentValue : controller.commentItems()) {
            const QVariantMap comment = commentValue.toMap();
            if (comment.value(QStringLiteral("text")).toString()
                == savedText) {
                rehydratedReply = comment;
                return true;
            }
        }
        return false;
    }(), 5000);
    QVERIFY(!rehydratedReply.isEmpty());
    QVERIFY(rehydratedReply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(!rehydratedReply.value(QStringLiteral("editable")).toBool());
    QVERIFY(rehydratedReply
        .value(QStringLiteral("commentTextEditable")).toBool());
}

void PDFEditControllerTest::importsZeroRectUnderlineReply()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("zero-rect-underline-reply.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("zero-rect-underline-reply-output.pdf"));
    const QString deletedOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("zero-rect-underline-reply-deleted.pdf"));
    QVERIFY(createZeroRectUnderlineReplyPdf(sourcePath));

    const CommentAnnotationInspection sourceReply
        = inspectCommentAnnotation(
            sourcePath,
            QStringLiteral("foxit-underline-reply"));
    QVERIFY(sourceReply.readable);
    QCOMPARE(sourceReply.annotationCount, 3);
    QCOMPARE(sourceReply.subtype, FPDF_ANNOT_TEXT);
    QCOMPARE(
        sourceReply.parentName,
        QStringLiteral("foxit-underline-root"));
    QCOMPARE(sourceReply.relation, QStringLiteral("R"));
    QCOMPARE(sourceReply.rect.left, 0.0f);
    QCOMPARE(sourceReply.rect.right, 0.0f);
    QCOMPARE(sourceReply.rect.top, 0.0f);
    QCOMPARE(sourceReply.rect.bottom, 0.0f);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const QVariantMap root = controller.commentItems().at(0).toMap();
    const QVariantMap reply = controller.commentItems().at(1).toMap();
    QCOMPARE(
        root.value(QStringLiteral("kind")).toString(),
        QStringLiteral("underline"));
    QCOMPARE(
        root.value(QStringLiteral("text")).toString(),
        QStringLiteral("Root underline"));
    QCOMPARE(root.value(QStringLiteral("replyCount")).toInt(), 1);
    QVERIFY(reply.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        reply.value(QStringLiteral("text")).toString(),
        QStringLiteral("Foxit underline reply"));
    QCOMPARE(
        reply.value(QStringLiteral("parentObjectId")).toString(),
        root.value(QStringLiteral("objectId")).toString());
    QVERIFY(reply
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .isEmpty());

    const QString replyObjectId = reply
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.selectComment(0, replyObjectId));
    QVERIFY(controller.selectedCommentReply());
    QVERIFY(controller.removeComment(0, replyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 2);

    const QString savedText
        = QStringLiteral("Edited Foxit underline reply");
    QVERIFY(controller.setCommentText(0, replyObjectId, savedText));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentAnnotationInspection outputReply
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("foxit-underline-reply"));
    QVERIFY(outputReply.readable);
    QCOMPARE(outputReply.annotationCount, sourceReply.annotationCount);
    QCOMPARE(outputReply.subtype, sourceReply.subtype);
    QCOMPARE(outputReply.contents, savedText);
    QCOMPARE(outputReply.parentName, sourceReply.parentName);
    QCOMPARE(outputReply.relation, QStringLiteral("R"));
    QCOMPARE(outputReply.rect.left, sourceReply.rect.left);
    QCOMPARE(outputReply.rect.right, sourceReply.rect.right);
    QCOMPARE(outputReply.rect.top, sourceReply.rect.top);
    QCOMPARE(outputReply.rect.bottom, sourceReply.rect.bottom);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    QVariantMap rehydratedReply;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("text")).toString()
            == savedText) {
            rehydratedReply = comment;
            break;
        }
    }
    QVERIFY(!rehydratedReply.isEmpty());
    QVERIFY(rehydratedReply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(rehydratedReply
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .isEmpty());

    const QString rehydratedReplyObjectId = rehydratedReply
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.removeComment(0, rehydratedReplyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(deletedOutputPath)));

    const CommentAnnotationInspection deletedReply
        = inspectCommentAnnotation(
            deletedOutputPath,
            QStringLiteral("foxit-underline-reply"));
    QVERIFY(deletedReply.readable);
    QCOMPARE(deletedReply.annotationCount, 2);
    QCOMPARE(deletedReply.annotationIndex, -1);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("Root underline"));
}

void PDFEditControllerTest::rejectsEditingProtectedSourceComments()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("protected-source-comments.pdf"));
    QVERIFY(createProtectedCommentTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const QVariantList comments = controller.commentItems();
    for (const QVariant &commentValue : comments) {
        const QVariantMap comment = commentValue.toMap();
        QVERIFY(!comment.value(QStringLiteral("editable")).toBool());
        QVERIFY(!comment
            .value(QStringLiteral("commentTextEditable")).toBool());
        QVERIFY(!controller.setCommentText(
            comment.value(QStringLiteral("page")).toInt(),
            comment.value(QStringLiteral("objectId")).toString(),
            QStringLiteral("Rejected edit")));
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
        QVERIFY(!controller.canRedo());
    }

    QCOMPARE(controller.commentItems(), comments);
}

void PDFEditControllerTest::protectsSourceFreeTextComments()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("protected-source-free-text.pdf"));
    QVERIFY(createProtectedFreeTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    const QVariantList comments = controller.commentItems();
    for (const QVariant &commentValue : comments) {
        const QVariantMap comment = commentValue.toMap();
        QCOMPARE(
            comment.value(QStringLiteral("kind")).toString(),
            QStringLiteral("freeText"));
        QVERIFY(!comment.value(QStringLiteral("editable")).toBool());
        QVERIFY(!comment
            .value(QStringLiteral("commentTextEditable"))
            .toBool());
        const int page = comment.value(QStringLiteral("page")).toInt();
        const QString objectId = comment
            .value(QStringLiteral("objectId"))
            .toString();
        QVERIFY(!controller.setCommentText(
            page,
            objectId,
            QStringLiteral("Rejected Free Text edit")));
        QVERIFY(!controller.removeComment(page, objectId));
    }

    QCOMPARE(controller.commentItems(), comments);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::enforcesCommentTextLengthLimit()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-text-length-limit.pdf"));
    QVERIFY(createRootCommentOnlyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap root = controller.commentItems().constFirst().toMap();
    const QString rootObjectId = root
        .value(QStringLiteral("objectId"))
        .toString();
    const QString sourceText = root
        .value(QStringLiteral("text"))
        .toString();
    const QString acceptedText
        = QStringLiteral("\U0001F642").repeated(350);
    const QString rejectedText = acceptedText + QLatin1Char('x');
    QCOMPARE(acceptedText.size(), 700);
    QCOMPARE(rejectedText.size(), 701);

    QVERIFY(controller.setCommentText(0, rootObjectId, acceptedText));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        acceptedText);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    controller.discardDrafts();
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        sourceText);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(!controller.setCommentText(0, rootObjectId, rejectedText));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        sourceText);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(controller.setCommentText(0, rootObjectId, QString()));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QString());
    QVERIFY(controller.hasDrafts());
    controller.discardDrafts();

    const QVariantMap acceptedReply = controller.addCommentReply(
        0,
        rootObjectId,
        acceptedText);
    QVERIFY(!acceptedReply.isEmpty());
    QCOMPARE(
        acceptedReply.value(QStringLiteral("text")).toString(),
        acceptedText);
    QCOMPARE(controller.commentItems().size(), 2);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    controller.discardDrafts();
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(controller.addCommentReply(
        0,
        rootObjectId,
        rejectedText).isEmpty());
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        sourceText);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    QVERIFY(controller.addCommentReply(
        0,
        rootObjectId,
        QString()).isEmpty());
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::boundsSharedAnnotationStringReads()
{
    PDFiumProcessLock pdfiumLock;
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    QVERIFY(document);
    const auto closeDocument = qScopeGuard([document]() {
        FPDF_CloseDocument(document);
    });

    FPDF_PAGE page = FPDFPage_New(document, 0, 200.0, 200.0);
    QVERIFY(page);
    const auto closePage = qScopeGuard([page]() {
        FPDF_ClosePage(page);
    });

    FPDF_ANNOTATION annotation = FPDFPage_CreateAnnot(
        page,
        FPDF_ANNOT_TEXT);
    QVERIFY(annotation);
    const auto closeAnnotation = qScopeGuard([annotation]() {
        FPDFPage_CloseAnnot(annotation);
    });

    constexpr unsigned long maximumBytes = 4UL * 1024UL * 1024UL;
    constexpr qsizetype maximumCharacters
        = maximumBytes / sizeof(FPDF_WCHAR) - 1;
    QString contents(maximumCharacters, QLatin1Char('a'));
    QVERIFY(FPDFAnnot_SetStringValue(
        annotation,
        "Contents",
        reinterpret_cast<const FPDF_WCHAR *>(contents.utf16())));
    QCOMPARE(
        FPDFAnnot_GetStringValue(annotation, "Contents", nullptr, 0),
        maximumBytes);
    QString boundedContents = PDFAnnotationUtils::annotationString(
        annotation,
        "Contents");
    QCOMPARE(boundedContents.size(), maximumCharacters);
    QVERIFY(boundedContents == contents);

    boundedContents.clear();
    contents.append(QLatin1Char('b'));
    QVERIFY(FPDFAnnot_SetStringValue(
        annotation,
        "Contents",
        reinterpret_cast<const FPDF_WCHAR *>(contents.utf16())));
    QCOMPARE(
        FPDFAnnot_GetStringValue(annotation, "Contents", nullptr, 0),
        maximumBytes + sizeof(FPDF_WCHAR));
    QVERIFY(PDFAnnotationUtils::annotationString(
        annotation,
        "Contents").isEmpty());
}

void PDFEditControllerTest::flattensNestedSourceCommentReplies()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("nested-comment-replies-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("nested-comment-replies-deleted.pdf"));
    QVERIFY(createNestedCommentRepliesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);

    const QVariantList comments = controller.commentItems();
    const QVariantMap root = comments.at(0).toMap();
    const QString rootObjectId = root
        .value(QStringLiteral("objectId"))
        .toString();
    QCOMPARE(
        root.value(QStringLiteral("text")).toString(),
        QStringLiteral("Root comment"));
    QCOMPARE(root.value(QStringLiteral("replyCount")).toInt(), 2);
    for (qsizetype index = 1; index < comments.size(); ++index) {
        const QVariantMap reply = comments.at(index).toMap();
        QVERIFY(reply.value(QStringLiteral("isReply")).toBool());
        QCOMPARE(
            reply.value(QStringLiteral("parentObjectId")).toString(),
            rootObjectId);
        QCOMPARE(reply.value(QStringLiteral("replyCount")).toInt(), 0);
    }
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("First reply"));
    QCOMPARE(
        comments.at(2).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Nested reply"));
    QVERIFY(controller.addCommentReply(
        0,
        comments.at(1).toMap()
            .value(QStringLiteral("objectId")).toString(),
        QStringLiteral("Rejected nested reply")).isEmpty());

    const QString firstReplyObjectId = comments.at(1).toMap()
        .value(QStringLiteral("objectId"))
        .toString();
    const QString nestedReplyObjectId = comments.at(2).toMap()
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(controller.selectComment(0, nestedReplyObjectId));
    QVERIFY(controller.removeComment(0, firstReplyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(controller.selectedObjectId().isEmpty());
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("replyCount")).toInt(),
        0);
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 3);

    QVERIFY(controller.removeComment(0, rootObjectId));
    QVERIFY(controller.commentItems().isEmpty());
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 3);
    QVERIFY(controller.removeComment(0, firstReplyObjectId));
    QCOMPARE(controller.commentItems().size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentReplyInspection inspection
        = inspectCommentReply(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.textAnnotationCount, 1);
    const StickyNoteSaveInspection rootInspection
        = inspectStickyNotes(outputPath);
    QVERIFY(rootInspection.readable);
    QCOMPARE(rootInspection.count, 1);
    QCOMPARE(rootInspection.contents, QStringLiteral("Root comment"));
}

void PDFEditControllerTest::hidesCommentRepliesFromViewer()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The PDF viewer regression test needs a Qt Quick render screen.");
    }

    const QString rootOnlyPath = m_temporaryDirectory.filePath(
        QStringLiteral("viewer-root-comment-only.pdf"));
    const QString repliesPath = m_temporaryDirectory.filePath(
        QStringLiteral("viewer-comment-replies.pdf"));
    QVERIFY(createRootCommentOnlyPdf(rootOnlyPath));
    QVERIFY(createNestedCommentRepliesPdf(repliesPath));

    const QSize renderSize(240, 240);
    const QImage rootOnlyImage = renderFirstPageInViewer(
        rootOnlyPath,
        renderSize);
    const QImage repliesImage = renderFirstPageInViewer(
        repliesPath,
        renderSize);
    QVERIFY(!rootOnlyImage.isNull());
    QVERIFY(!repliesImage.isNull());
    QCOMPARE(rootOnlyImage.size(), repliesImage.size());

    QImage blankImage(rootOnlyImage.size(), QImage::Format_ARGB32);
    blankImage.fill(Qt::white);
    QVERIFY(changedPixelCount(
        rootOnlyImage,
        blankImage,
        rootOnlyImage.rect()) > 0);
    QCOMPARE(
        changedPixelCount(
            rootOnlyImage,
            repliesImage,
            rootOnlyImage.rect()),
        0);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(repliesPath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);
    QCOMPARE(controller.commentItems().size(), 3);
    const QVariantMap sourceReply = controller.commentItems().at(1).toMap();
    QVERIFY(sourceReply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(controller.selectComment(
        0,
        sourceReply.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedCommentReply());
    const QVariantList pageCandidates = waitForCandidates(controller, 0);
    bool rootCandidateFound = false;
    for (const QVariant &candidate : pageCandidates) {
        const QVariantMap candidateMap = candidate.toMap();
        QVERIFY(!candidateMap.value(QStringLiteral("isReply")).toBool());
        rootCandidateFound = rootCandidateFound
            || (candidateMap.value(QStringLiteral("kind")).toString()
                    == QStringLiteral("stickyNote")
                && candidateMap.value(QStringLiteral("text")).toString()
                    == QStringLiteral("Root comment"));
    }
    QVERIFY(rootCandidateFound);
}

void PDFEditControllerTest::hidesUnresolvedSourceReplyFromViewerModel()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("viewer-unresolved-comment-reply.pdf"));
    QVERIFY(createUnsupportedRootCommentReplyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap reply = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        reply.value(QStringLiteral("text")).toString(),
        QStringLiteral("Unresolved reply"));
    QVERIFY(!reply.value(QStringLiteral("isReply")).toBool());
    QVERIFY(controller.selectComment(
        0,
        reply.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedCommentReply());

    QVERIFY(waitForCandidates(controller, 0).isEmpty());
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.candidateAt(0, 0.32, 0.22).isEmpty());
}

void PDFEditControllerTest::ownsPendingDrawingInteractionAcrossPages()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("drawing-interaction-owner.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(document.pageCount(), 2);
    controller.setDocument(&document);

    QCOMPARE(controller.pendingDrawingCommentPage(), -1);
    QVERIFY(!controller.claimDrawingCommentInteraction(0));
    controller.setTool(PDFEditController::SquareTool);
    QCOMPARE(
        static_cast<int>(PDFEditController::SquareCommentTool),
        static_cast<int>(PDFEditController::SquareTool));

    QSignalSpy ownerChanged(
        &controller,
        &PDFEditController::pendingDrawingCommentPageChanged);
    QVERIFY(controller.claimDrawingCommentInteraction(0));
    QCOMPARE(controller.pendingDrawingCommentPage(), 0);
    QCOMPARE(ownerChanged.count(), 1);

    QVERIFY(controller.claimDrawingCommentInteraction(0));
    QCOMPARE(ownerChanged.count(), 1);
    QVERIFY(controller.claimDrawingCommentInteraction(1));
    QCOMPARE(controller.pendingDrawingCommentPage(), 1);
    QCOMPARE(ownerChanged.count(), 2);

    controller.releaseDrawingCommentInteraction(0);
    QCOMPARE(controller.pendingDrawingCommentPage(), 1);
    QCOMPARE(ownerChanged.count(), 2);
    controller.releaseDrawingCommentInteraction(1);
    QCOMPARE(controller.pendingDrawingCommentPage(), -1);
    QCOMPARE(ownerChanged.count(), 3);

    QVERIFY(controller.claimDrawingCommentInteraction(0));
    controller.setTool(PDFEditController::CircleTool);
    QCOMPARE(controller.pendingDrawingCommentPage(), -1);
    QVERIFY(!controller.claimDrawingCommentInteraction(-1));
    QVERIFY(!controller.claimDrawingCommentInteraction(2));
}

void PDFEditControllerTest::rejectsInvalidDrawingCommentGeometry_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<QVariantList>("vertices");

    QTest::newRow("square-extra-point")
        << static_cast<int>(PDFEditController::SquareTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.1, 0.1)),
               QVariant::fromValue(QPointF(0.3, 0.3)),
               QVariant::fromValue(QPointF(0.4, 0.4))};
    QTest::newRow("circle-one-point")
        << static_cast<int>(PDFEditController::CircleTool)
        << QVariantList{QVariant::fromValue(QPointF(0.1, 0.1))};
    QTest::newRow("line-extra-point")
        << static_cast<int>(PDFEditController::LineTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.1, 0.1)),
               QVariant::fromValue(QPointF(0.3, 0.3)),
               QVariant::fromValue(QPointF(0.4, 0.4))};
    QTest::newRow("polygon-too-few-points")
        << static_cast<int>(PDFEditController::PolygonTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.1, 0.1)),
               QVariant::fromValue(QPointF(0.3, 0.3))};
    QTest::newRow("polygon-zero-area")
        << static_cast<int>(PDFEditController::PolygonTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.1, 0.1)),
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(0.3, 0.3))};
    QTest::newRow("polyline-duplicate-points")
        << static_cast<int>(PDFEditController::PolyLineTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(0.2, 0.2))};
    QTest::newRow("ink-one-point")
        << static_cast<int>(PDFEditController::InkTool)
        << QVariantList{QVariant::fromValue(QPointF(0.2, 0.2))};
    QTest::newRow("ink-duplicate-points")
        << static_cast<int>(PDFEditController::InkTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(0.2, 0.2))};
    QTest::newRow("ink-too-short")
        << static_cast<int>(PDFEditController::InkTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(0.201, 0.2))};
    QTest::newRow("ink-non-finite")
        << static_cast<int>(PDFEditController::InkTool)
        << QVariantList{
               QVariant::fromValue(QPointF(0.2, 0.2)),
               QVariant::fromValue(QPointF(
                   std::numeric_limits<qreal>::infinity(),
                   0.3))};

    QVariantList oversizedInkStroke;
    oversizedInkStroke.reserve(4097);
    for (int index = 0; index < 4097; ++index) {
        oversizedInkStroke.append(QVariant::fromValue(QPointF(
            static_cast<qreal>(index) / 4096.0,
            index % 2 == 0 ? 0.2 : 0.3)));
    }
    QTest::newRow("ink-too-many-points")
        << static_cast<int>(PDFEditController::InkTool)
        << oversizedInkStroke;
}

void PDFEditControllerTest::rejectsInvalidDrawingCommentGeometry()
{
    QFETCH(int, tool);
    QFETCH(QVariantList, vertices);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("invalid-drawing-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(static_cast<PDFEditController::Tool>(tool));

    QVERIFY(controller.addDrawingComment(0, vertices).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.draftsForPage(0).size(), 0);
    QCOMPARE(controller.tool(), static_cast<PDFEditController::Tool>(tool));
}

void PDFEditControllerTest::savesDrawingCommentsRoundTrip_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<int>("subtype");
    QTest::addColumn<QVariantList>("vertices");

    QTest::newRow("square")
        << static_cast<int>(PDFEditController::SquareTool)
        << QStringLiteral("square")
        << static_cast<int>(FPDF_ANNOT_SQUARE)
        << QVariantList{
               QVariant::fromValue(QPointF(0.14, 0.18)),
               QVariant::fromValue(QPointF(0.42, 0.38))};
    QTest::newRow("circle")
        << static_cast<int>(PDFEditController::CircleTool)
        << QStringLiteral("circle")
        << static_cast<int>(FPDF_ANNOT_CIRCLE)
        << QVariantList{
               QVariant::fromValue(QPointF(0.18, 0.22)),
               QVariant::fromValue(QPointF(0.46, 0.44))};
    QTest::newRow("line")
        << static_cast<int>(PDFEditController::LineTool)
        << QStringLiteral("line")
        << static_cast<int>(FPDF_ANNOT_LINE)
        << QVariantList{
               QVariant::fromValue(QPointF(0.12, 0.28)),
               QVariant::fromValue(QPointF(0.54, 0.34))};
    QTest::newRow("polygon")
        << static_cast<int>(PDFEditController::PolygonTool)
        << QStringLiteral("polygon")
        << static_cast<int>(FPDF_ANNOT_POLYGON)
        << QVariantList{
               QVariant::fromValue(QPointF(0.16, 0.18)),
               QVariant::fromValue(QPointF(0.48, 0.22)),
               QVariant::fromValue(QPointF(0.34, 0.48))};
    QTest::newRow("polyline")
        << static_cast<int>(PDFEditController::PolyLineTool)
        << QStringLiteral("polyLine")
        << static_cast<int>(FPDF_ANNOT_POLYLINE)
        << QVariantList{
               QVariant::fromValue(QPointF(0.12, 0.16)),
               QVariant::fromValue(QPointF(0.34, 0.30)),
               QVariant::fromValue(QPointF(0.56, 0.20))};
}

void PDFEditControllerTest::savesDrawingCommentsRoundTrip()
{
    QFETCH(int, tool);
    QFETCH(QString, kind);
    QFETCH(int, subtype);
    QFETCH(QVariantList, vertices);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("drawing-comment-%1-source.pdf").arg(kind));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("drawing-comment-%1-output.pdf").arg(kind));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QColor drawingColor(32, 105, 196, 153);
    controller.setDrawingCommentColor(drawingColor);
    controller.setTool(static_cast<PDFEditController::Tool>(tool));

    const QVariantMap added = controller.addDrawingComment(0, vertices);
    QVERIFY(!added.isEmpty());
    QCOMPARE(added.value(QStringLiteral("kind")).toString(), kind);
    QCOMPARE(
        added.value(QStringLiteral("drawingVertices")).toList().size(),
        vertices.size());
    QCOMPARE(
        added.value(QStringLiteral("annotationPoints")).toList(),
        added.value(QStringLiteral("drawingVertices")).toList());
    QCOMPARE(
        added.value(QStringLiteral("drawingStrokeColor")).value<QColor>(),
        QColor(32, 105, 196));
    QCOMPARE(
        added.value(QStringLiteral("strokeColor")).value<QColor>(),
        QColor(32, 105, 196));
    QVERIFY(!added.value(QStringLiteral("fillColor")).value<QColor>().isValid());
    QVERIFY(qAbs(
        added.value(QStringLiteral("drawingOpacity")).toReal()
            - drawingColor.alphaF()) < 0.002);
    QVERIFY(qAbs(
        added.value(QStringLiteral("opacity")).toReal()
            - drawingColor.alphaF()) < 0.002);
    QVERIFY(qAbs(
        added.value(QStringLiteral("strokeWidthRatio")).toReal()
            - 1.5 / document.pageSize(0).height()) < 0.000001);
    QCOMPARE(added.value(QStringLiteral("borderStyle")).toInt(), 0);
    QVERIFY(added.value(QStringLiteral("dashPattern")).toList().isEmpty());
    QCOMPARE(added.value(QStringLiteral("borderEffect")).toInt(), 0);
    QCOMPARE(added.value(QStringLiteral("lineStartStyle")).toInt(), 0);
    QCOMPARE(added.value(QStringLiteral("lineEndStyle")).toInt(), 0);
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    QCOMPARE(controller.selectedKind(), kind);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(controller.undo(), -1);
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.redo(), -1);
    QVERIFY(controller.hasDrafts());

    const QString annotationId = added
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!annotationId.isEmpty());
    const int sourcePageObjectCount = topLevelPageObjectCount(sourcePath);
    QVERIFY(sourcePageObjectCount > 0);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(topLevelPageObjectCount(outputPath), sourcePageObjectCount);
    QCOMPARE(
        annotationSubtypeCount(sourcePath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype)),
        0);
    QCOMPARE(
        annotationSubtypeCount(outputPath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype)),
        1);

    const CommentAnnotationInspection inspection
        = inspectCommentAnnotation(outputPath, annotationId);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.subtype, subtype);
    QVERIFY(inspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(
        inspection.subject,
        added.value(QStringLiteral("label")).toString());
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearance.isEmpty());
    QVERIFY(inspection.appearance.contains(QStringLiteral("/GS gs")));
    QVERIFY(inspection.appearance.contains(QStringLiteral("1.5 w")));
    QVERIFY(inspection.appearance.contains(QStringLiteral("1 J 1 j")));
    QVERIFY(inspection.appearance.contains(QStringLiteral("[] 0 d")));
    const QRegularExpression closePathOperator(
        QStringLiteral(R"((?:^|\s)h(?:\s|$))"));
    const QRegularExpression cubicPathOperator(
        QStringLiteral(R"((?:^|\s)c(?:\s|$))"));
    if (kind == QLatin1String("square")
        || kind == QLatin1String("circle")
        || kind == QLatin1String("polygon")) {
        QCOMPARE(inspection.appearance.count(closePathOperator), 1);
    } else {
        QCOMPARE(inspection.appearance.count(closePathOperator), 0);
    }
    QCOMPARE(
        inspection.appearance.count(cubicPathOperator),
        kind == QLatin1String("circle") ? 4 : 0);
    const QByteArray annotationDictionary = annotationDictionaryBytes(
        outputPath,
        annotationId);
    QVERIFY(!annotationDictionary.isEmpty());
    const QString annotationDictionaryText = QString::fromLatin1(
        annotationDictionary);
    const auto hasDictionaryPattern = [&annotationDictionaryText](
                                          const QString &pattern) {
        return QRegularExpression(pattern)
            .match(annotationDictionaryText)
            .hasMatch();
    };
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/C\s*\[)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/CA\s+)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/BS\s*<<)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/W\s+1\.5\b)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/S\s*/S\b)")));
    QVERIFY(hasDictionaryPattern(
        QStringLiteral(R"(/AP\s*<<\s*/N\s+\d+\s+0\s+R\b)")));
    QVERIFY(!annotationDictionary.contains("/IC"));
    QVERIFY(!annotationDictionary.contains("/BE"));
    QVERIFY(!annotationDictionary.contains("/IT"));
    const QRegularExpression lineGeometryKey(
        QStringLiteral(R"(/L\b)"));
    const QRegularExpression verticesGeometryKey(
        QStringLiteral(R"(/Vertices\b)"));
    const QRegularExpression lineEndingKey(
        QStringLiteral(R"(/LE\b)"));
    QCOMPARE(
        lineGeometryKey.match(annotationDictionaryText).hasMatch(),
        kind == QLatin1String("line"));
    QCOMPARE(
        verticesGeometryKey.match(annotationDictionaryText).hasMatch(),
        kind == QLatin1String("polygon")
            || kind == QLatin1String("polyLine"));
    QCOMPARE(
        lineEndingKey.match(annotationDictionaryText).hasMatch(),
        kind == QLatin1String("line")
            || kind == QLatin1String("polyLine"));
    if (kind == QLatin1String("line")
        || kind == QLatin1String("polyLine")) {
        QVERIFY(hasDictionaryPattern(
            QStringLiteral(
                R"(/LE\s*\[\s*/None\s+/None\s*\])")));
    }
    const AnnotationAppearanceStructureInspection appearance
        = inspectAnnotationAppearanceStructure(outputPath, annotationId);
    QVERIFY(appearance.readable);
    QVERIFY(appearance.annotationFound);
    QVERIFY(appearance.normalAppearanceIsIndirect);
    QVERIFY(appearance.appearanceObjectFound);
    QVERIFY(appearance.appearanceIsStream);
    QVERIFY(appearance.typeIsXObject);
    QVERIFY(appearance.subtypeIsForm);
    QVERIFY(appearance.matrixIsIdentity);
    QCOMPARE(appearance.boundingBox.size(), 4);
    QVERIFY(appearance.hasResources);
    QVERIFY(appearance.hasExtGState);
    QVERIFY(appearance.hasGraphicsState);
    QVERIFY(appearance.graphicsStateTypeIsExtGState);
    QVERIFY(appearance.blendModeIsNormal);
    QVERIFY(appearance.alphaSourceIsFalse);
    QVERIFY(qAbs(
        appearance.strokingOpacity - drawingColor.alphaF()) < 0.002);
    QVERIFY(qAbs(
        appearance.nonStrokingOpacity - drawingColor.alphaF()) < 0.002);

    controller.requestCommentIndex();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.commentItems().isEmpty(), 5000);
    bool rehydrated = false;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("kind")).toString() == kind) {
            QCOMPARE(
                comment.value(QStringLiteral("kind")).toString(),
                kind);
            QVERIFY(comment
                .value(QStringLiteral("commentTextEditable"))
                .toBool());
            QCOMPARE(
                comment
                    .value(QStringLiteral("drawingStrokeColor"))
                    .value<QColor>(),
                QColor(32, 105, 196));
            QCOMPARE(
                comment.value(QStringLiteral("strokeColor")).value<QColor>(),
                QColor(32, 105, 196));
            QVERIFY(qAbs(
                comment
                    .value(QStringLiteral("drawingOpacity"))
                    .toReal()
                    - drawingColor.alphaF()) < 0.01);
            QVERIFY(qAbs(
                comment
                    .value(QStringLiteral("drawingBorderWidth"))
                    .toReal()
                    - 1.5) < 0.002);
            QVERIFY(qAbs(
                comment.value(QStringLiteral("strokeWidthRatio")).toReal()
                    - 1.5 / document.pageSize(0).height()) < 0.000001);
            const QVariantList rehydratedVertices = comment
                .value(QStringLiteral("annotationPoints"))
                .toList();
            QCOMPARE(rehydratedVertices.size(), vertices.size());
            for (qsizetype index = 0;
                 index < rehydratedVertices.size();
                 ++index) {
                const QPointF actual = rehydratedVertices.at(index).toPointF();
                const QPointF expected = vertices.at(index).toPointF();
                QVERIFY(qAbs(actual.x() - expected.x()) < 0.002);
                QVERIFY(qAbs(actual.y() - expected.y()) < 0.002);
            }
            rehydrated = true;
            break;
        }
    }
    QVERIFY(rehydrated);
}

void PDFEditControllerTest::savesInkCommentRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("ink-comment-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("ink-comment-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(!pageSize.isEmpty());
    const QColor drawingColor(32, 105, 196, 153);
    controller.setDrawingCommentColor(drawingColor);
    controller.setTool(PDFEditController::InkTool);
    const QVariantList inputPoints{
        QVariant::fromValue(QPointF(0.12, 0.18)),
        QVariant::fromValue(QPointF(0.18, 0.27)),
        QVariant::fromValue(QPointF(0.27, 0.20)),
        QVariant::fromValue(QPointF(0.36, 0.34)),
        QVariant::fromValue(QPointF(0.48, 0.24)),
    };

    const QVariantMap added = controller.addDrawingComment(0, inputPoints);
    QVERIFY(!added.isEmpty());
    QCOMPARE(
        added.value(QStringLiteral("kind")).toString(),
        QStringLiteral("ink"));
    const QVariantList addedStrokes = added
        .value(QStringLiteral("inkStrokes"))
        .toList();
    QCOMPARE(addedStrokes.size(), 1);
    QCOMPARE(
        added.value(QStringLiteral("annotationStrokes")).toList(),
        addedStrokes);
    const QVariantList addedPath = addedStrokes.constFirst().toList();
    QVERIFY(addedPath.size() >= 2);
    QVERIFY(addedPath.size() <= 1024);
    QCOMPARE(
        addedPath.constFirst().toPointF(),
        inputPoints.constFirst().toPointF());
    QCOMPARE(
        addedPath.constLast().toPointF(),
        inputPoints.constLast().toPointF());
    QCOMPARE(
        added.value(QStringLiteral("drawingStrokeColor")).value<QColor>(),
        QColor(32, 105, 196));
    QCOMPARE(
        added.value(QStringLiteral("strokeColor")).value<QColor>(),
        QColor(32, 105, 196));
    QVERIFY(qAbs(
        added.value(QStringLiteral("drawingOpacity")).toReal()
            - drawingColor.alphaF()) < 0.002);
    QVERIFY(qAbs(
        added.value(QStringLiteral("drawingBorderWidth")).toReal()
            - 1.5) < 0.002);
    QVERIFY(qAbs(
        added.value(QStringLiteral("strokeWidthRatio")).toReal()
            - 1.5 / pageSize.height()) < 0.000001);
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    QCOMPARE(controller.selectedKind(), QStringLiteral("ink"));
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(controller.undo(), -1);
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.redo(), -1);
    QVERIFY(controller.hasDrafts());

    const QString annotationId = added
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!annotationId.isEmpty());
    const int sourcePageObjectCount = topLevelPageObjectCount(sourcePath);
    QVERIFY(sourcePageObjectCount > 0);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(topLevelPageObjectCount(outputPath), sourcePageObjectCount);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_INK), 0);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_INK), 1);

    const CommentAnnotationInspection commentInspection
        = inspectCommentAnnotation(outputPath, annotationId);
    QVERIFY(commentInspection.readable);
    QCOMPARE(commentInspection.subtype, FPDF_ANNOT_INK);
    QVERIFY(commentInspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(
        commentInspection.subject,
        added.value(QStringLiteral("label")).toString());
    QVERIFY(commentInspection.hasAppearance);
    QVERIFY(!commentInspection.appearance.isEmpty());
    QVERIFY(commentInspection.appearance.contains(QStringLiteral("/GS gs")));
    QVERIFY(commentInspection.appearance.contains(QStringLiteral("1.5 w")));
    QVERIFY(commentInspection.appearance.contains(QStringLiteral("1 J 1 j")));
    QVERIFY(commentInspection.appearance.contains(QStringLiteral("[] 0 d")));
    QVERIFY(QRegularExpression(QStringLiteral(R"((?:^|\s)m(?:\s|$))"))
        .match(commentInspection.appearance)
        .hasMatch());
    QVERIFY(QRegularExpression(QStringLiteral(R"((?:^|\s)l(?:\s|$))"))
        .match(commentInspection.appearance)
        .hasMatch());

    const InkAnnotationInspection inkInspection = inspectInkAnnotation(
        outputPath,
        annotationId);
    QVERIFY(inkInspection.readable);
    QCOMPARE(inkInspection.count, 1);
    QVERIFY(inkInspection.annotationFound);
    QCOMPARE(inkInspection.paths.size(), 1);
    QCOMPARE(inkInspection.paths.constFirst().size(), addedPath.size());
    for (qsizetype pointIndex = 0;
         pointIndex < addedPath.size();
         ++pointIndex) {
        const QPointF actual = inkInspection.paths.constFirst().at(
            pointIndex);
        const QPointF expected = addedPath.at(pointIndex).toPointF();
        QVERIFY(qAbs(actual.x() / pageSize.width() - expected.x()) < 0.002);
        QVERIFY(qAbs(
            (1.0 - actual.y() / pageSize.height()) - expected.y()) < 0.002);
    }
    const QByteArray annotationDictionary = annotationDictionaryBytes(
        outputPath,
        annotationId);
    QVERIFY(!annotationDictionary.isEmpty());
    const QString annotationDictionaryText = QString::fromLatin1(
        annotationDictionary);
    const auto hasDictionaryPattern = [&annotationDictionaryText](
                                          const QString &pattern) {
        return QRegularExpression(pattern)
            .match(annotationDictionaryText)
            .hasMatch();
    };
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/InkList\s*\[)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/C\s*\[)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/CA\s+)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/BS\s*<<)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/W\s+1\.5\b)")));
    QVERIFY(hasDictionaryPattern(QStringLiteral(R"(/S\s*/S\b)")));
    QVERIFY(hasDictionaryPattern(
        QStringLiteral(R"(/AP\s*<<\s*/N\s+\d+\s+0\s+R\b)")));
    QVERIFY(!annotationDictionary.contains("/Vertices"));
    QVERIFY(!hasDictionaryPattern(QStringLiteral(R"(/L\b)")));

    const AnnotationAppearanceStructureInspection appearance
        = inspectAnnotationAppearanceStructure(outputPath, annotationId);
    QVERIFY(appearance.readable);
    QVERIFY(appearance.annotationFound);
    QVERIFY(appearance.normalAppearanceIsIndirect);
    QVERIFY(appearance.appearanceObjectFound);
    QVERIFY(appearance.appearanceIsStream);
    QVERIFY(appearance.typeIsXObject);
    QVERIFY(appearance.subtypeIsForm);
    QVERIFY(appearance.matrixIsIdentity);
    QCOMPARE(appearance.boundingBox.size(), 4);
    QVERIFY(appearance.hasResources);
    QVERIFY(appearance.hasExtGState);
    QVERIFY(appearance.hasGraphicsState);
    QVERIFY(appearance.graphicsStateTypeIsExtGState);
    QVERIFY(appearance.blendModeIsNormal);
    QVERIFY(appearance.alphaSourceIsFalse);
    QVERIFY(qAbs(
        appearance.strokingOpacity - drawingColor.alphaF()) < 0.002);
    QVERIFY(qAbs(
        appearance.nonStrokingOpacity - drawingColor.alphaF()) < 0.002);

    controller.requestCommentIndex();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.commentItems().isEmpty(), 5000);
    bool rehydrated = false;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("kind")).toString()
            != QLatin1String("ink")) {
            continue;
        }
        QVERIFY(comment
            .value(QStringLiteral("commentTextEditable"))
            .toBool());
        QCOMPARE(
            comment
                .value(QStringLiteral("drawingStrokeColor"))
                .value<QColor>(),
            QColor(32, 105, 196));
        QVERIFY(qAbs(
            comment.value(QStringLiteral("drawingOpacity")).toReal()
                - drawingColor.alphaF()) < 0.01);
        QVERIFY(qAbs(
            comment.value(QStringLiteral("drawingBorderWidth")).toReal()
                - 1.5) < 0.002);
        const QVariantList rehydratedStrokes = comment
            .value(QStringLiteral("inkStrokes"))
            .toList();
        QCOMPARE(rehydratedStrokes.size(), 1);
        QCOMPARE(
            comment.value(QStringLiteral("annotationStrokes")).toList(),
            rehydratedStrokes);
        const QVariantList rehydratedPath = rehydratedStrokes
            .constFirst()
            .toList();
        QCOMPARE(rehydratedPath.size(), addedPath.size());
        for (qsizetype pointIndex = 0;
             pointIndex < addedPath.size();
             ++pointIndex) {
            const QPointF actual = rehydratedPath.at(pointIndex).toPointF();
            const QPointF expected = addedPath.at(pointIndex).toPointF();
            QVERIFY(qAbs(actual.x() - expected.x()) < 0.002);
            QVERIFY(qAbs(actual.y() - expected.y()) < 0.002);
        }
        rehydrated = true;
        break;
    }
    QVERIFY(rehydrated);
}

void PDFEditControllerTest::
    editsRepliesAndDeletesSourceDrawingCommentsRoundTrip_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("sourceContents");
    QTest::addColumn<QString>("sourceName");
    QTest::addColumn<int>("subtype");

    QTest::newRow("square")
        << QStringLiteral("square")
        << QStringLiteral("unsupported-square")
        << QStringLiteral("unsupported-1")
        << static_cast<int>(FPDF_ANNOT_SQUARE);
    QTest::newRow("circle")
        << QStringLiteral("circle")
        << QStringLiteral("unsupported-circle")
        << QStringLiteral("unsupported-2")
        << static_cast<int>(FPDF_ANNOT_CIRCLE);
    QTest::newRow("line")
        << QStringLiteral("line")
        << QStringLiteral("unsupported-line")
        << QStringLiteral("unsupported-0")
        << static_cast<int>(FPDF_ANNOT_LINE);
    QTest::newRow("polygon")
        << QStringLiteral("polygon")
        << QStringLiteral("unsupported-polygon")
        << QStringLiteral("unsupported-3")
        << static_cast<int>(FPDF_ANNOT_POLYGON);
    QTest::newRow("polyline")
        << QStringLiteral("polyLine")
        << QStringLiteral("unsupported-polyline")
        << QStringLiteral("unsupported-4")
        << static_cast<int>(FPDF_ANNOT_POLYLINE);
    QTest::newRow("ink")
        << QStringLiteral("ink")
        << QStringLiteral("unsupported-ink")
        << QStringLiteral("unsupported-7")
        << static_cast<int>(FPDF_ANNOT_INK);
}

void PDFEditControllerTest::
    editsRepliesAndDeletesSourceDrawingCommentsRoundTrip()
{
    QFETCH(QString, kind);
    QFETCH(QString, sourceContents);
    QFETCH(QString, sourceName);
    QFETCH(int, subtype);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-drawing-%1.pdf").arg(kind));
    const QString editedPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-drawing-%1-edited.pdf").arg(kind));
    const QString deletedPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-drawing-%1-deleted.pdf").arg(kind));
    QVERIFY(createUnsupportedMarkupCommentsPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 12, 5000);

    QVariantMap sourceComment;
    for (const QVariant &value : controller.commentItems()) {
        const QVariantMap comment = value.toMap();
        if (comment.value(QStringLiteral("text")).toString()
                == sourceContents) {
            sourceComment = comment;
            break;
        }
    }
    QVERIFY(!sourceComment.isEmpty());
    QCOMPARE(sourceComment.value(QStringLiteral("kind")).toString(), kind);
    QVERIFY(sourceComment
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    QCOMPARE(
        sourceComment
            .value(QStringLiteral("drawingStrokeColor"))
            .value<QColor>(),
        QColor(26, 51, 77));
    QVERIFY(qAbs(
        sourceComment.value(QStringLiteral("drawingOpacity")).toReal()
            - 0.75) < 0.01);
    QVERIFY(qAbs(
        sourceComment.value(QStringLiteral("drawingBorderWidth")).toReal()
            - 2.0) < 0.01);

    const CommentAnnotationInspection originalInspection
        = inspectCommentAnnotation(sourcePath, sourceName);
    QVERIFY(originalInspection.readable);
    QCOMPARE(originalInspection.subtype, subtype);
    QVERIFY(originalInspection.hasAppearance);
    QVERIFY(!originalInspection.appearance.isEmpty());

    const QString objectId = sourceComment
        .value(QStringLiteral("objectId"))
        .toString();
    const QString editedContents = QStringLiteral(
        "Drawing comment đã được chỉnh sửa.");
    const QString replyContents = QStringLiteral(
        "Phản hồi cho drawing comment.");
    QVERIFY(controller.setCommentText(0, objectId, editedContents));
    const QVariantMap reply = controller.addCommentReply(
        0,
        objectId,
        replyContents);
    QVERIFY(!reply.isEmpty());
    const QString replyId = reply
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!replyId.isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(editedPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection sourceStillOriginal
        = inspectCommentAnnotation(sourcePath, sourceName);
    QCOMPARE(sourceStillOriginal.contents, sourceContents);
    QCOMPARE(sourceStillOriginal.appearance, originalInspection.appearance);
    const CommentAnnotationInspection editedInspection
        = inspectCommentAnnotation(editedPath, sourceName);
    QVERIFY(editedInspection.readable);
    QCOMPARE(editedInspection.subtype, subtype);
    QCOMPARE(editedInspection.contents, editedContents);
    QCOMPARE(editedInspection.appearance, originalInspection.appearance);
    const CommentAnnotationInspection replyInspection
        = inspectCommentAnnotation(editedPath, replyId);
    QVERIFY(replyInspection.readable);
    QCOMPARE(replyInspection.subtype, FPDF_ANNOT_TEXT);
    QCOMPARE(replyInspection.contents, replyContents);
    QCOMPARE(replyInspection.parentName, sourceName);
    QCOMPARE(replyInspection.relation, QStringLiteral("R"));

    controller.requestCommentIndex();
    QTRY_VERIFY_WITH_TIMEOUT(controller.commentItems().size() >= 13, 5000);
    QString rehydratedObjectId;
    for (const QVariant &value : controller.commentItems()) {
        const QVariantMap comment = value.toMap();
        if (!comment.value(QStringLiteral("isReply")).toBool()
            && comment.value(QStringLiteral("kind")).toString() == kind
            && comment.value(QStringLiteral("text")).toString()
                == editedContents) {
            rehydratedObjectId = comment
                .value(QStringLiteral("objectId"))
                .toString();
            break;
        }
    }
    QVERIFY(!rehydratedObjectId.isEmpty());
    QVERIFY(controller.removeComment(0, rehydratedObjectId));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(deletedPath)));
    QVERIFY(!controller.hasDrafts());

    QCOMPARE(
        annotationSubtypeCount(
            sourcePath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype)),
        1);
    QCOMPARE(
        annotationSubtypeCount(
            deletedPath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype)),
        0);
    QCOMPARE(
        inspectCommentAnnotation(deletedPath, sourceName).annotationIndex,
        -1);
    QCOMPARE(
        inspectCommentAnnotation(deletedPath, replyId).annotationIndex,
        -1);
}

void PDFEditControllerTest::indexesDrawingAndUnsupportedMarkupComments()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("unsupported-markup-comments.pdf"));
    QVERIFY(createUnsupportedMarkupCommentsPdf(sourcePath));
    const InkAnnotationInspection sourceInk = inspectInkAnnotation(
        sourcePath,
        QStringLiteral("unsupported-7"));
    QVERIFY(sourceInk.readable);
    QVERIFY(sourceInk.annotationFound);
    QCOMPARE(sourceInk.paths.size(), 2);
    QCOMPARE(sourceInk.paths.constFirst().size(), 5);
    QCOMPARE(sourceInk.paths.constLast().size(), 4);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 12, 5000);

    const QSet<QString> expectedContents{
        QStringLiteral("unsupported-line"),
        QStringLiteral("unsupported-square"),
        QStringLiteral("unsupported-circle"),
        QStringLiteral("unsupported-polygon"),
        QStringLiteral("unsupported-polyline"),
        QStringLiteral("unsupported-stamp"),
        QStringLiteral("unsupported-caret"),
        QStringLiteral("unsupported-ink"),
        QStringLiteral("unsupported-file-attachment"),
        QStringLiteral("unsupported-sound"),
        QStringLiteral("unsupported-redact"),
        QStringLiteral("unsupported-projection"),
    };
    QSet<QString> indexedContents;
    const QHash<QString, QString> drawingKinds{
        {QStringLiteral("unsupported-line"), QStringLiteral("line")},
        {QStringLiteral("unsupported-square"), QStringLiteral("square")},
        {QStringLiteral("unsupported-circle"), QStringLiteral("circle")},
        {QStringLiteral("unsupported-polygon"), QStringLiteral("polygon")},
        {QStringLiteral("unsupported-polyline"), QStringLiteral("polyLine")},
        {QStringLiteral("unsupported-ink"), QStringLiteral("ink")},
    };
    const QVariantList comments = controller.commentItems();
    for (const QVariant &commentValue : comments) {
        const QVariantMap comment = commentValue.toMap();
        const QString text = comment.value(QStringLiteral("text")).toString();
        const QString objectId = comment
            .value(QStringLiteral("objectId"))
            .toString();
        indexedContents.insert(text);
        const bool drawingComment = drawingKinds.contains(text);
        QCOMPARE(
            comment.value(QStringLiteral("kind")).toString(),
            drawingComment
                ? drawingKinds.value(text)
                : QStringLiteral("comment"));
        QVERIFY(!comment.value(QStringLiteral("label")).toString().isEmpty());
        QCOMPARE(
            comment.value(QStringLiteral("commentAuthor")).toString(),
            QStringLiteral("Markup author"));
        QVERIFY(!comment.value(QStringLiteral("editable")).toBool());
        QCOMPARE(
            comment.value(QStringLiteral("commentTextEditable")).toBool(),
            drawingComment);
        QCOMPARE(
            comment.value(QStringLiteral("commentTextEditReason"))
                .toString()
                .isEmpty(),
            drawingComment);
        QVERIFY(controller.selectComment(0, objectId));
        QVERIFY(!controller.selectedEditable());
        if (drawingComment) {
            if (text == QLatin1String("unsupported-ink")) {
                const QVariantList strokes = comment
                    .value(QStringLiteral("inkStrokes"))
                    .toList();
                QCOMPARE(strokes.size(), 2);
                QCOMPARE(strokes.constFirst().toList().size(), 3);
                QCOMPARE(strokes.constLast().toList().size(), 2);
                QCOMPARE(
                    comment
                        .value(QStringLiteral("annotationStrokes"))
                        .toList(),
                    strokes);
            }
            QVERIFY(controller.setCommentText(
                0,
                objectId,
                QStringLiteral("Editable drawing comment")));
            QVERIFY(controller.hasDrafts());
            controller.undo();
            QVERIFY(!controller.hasDrafts());

            QVERIFY(!controller.addCommentReply(
                0,
                objectId,
                QStringLiteral("Drawing reply")).isEmpty());
            QVERIFY(controller.hasDrafts());
            controller.undo();
            QVERIFY(!controller.hasDrafts());

            QVERIFY(controller.removeComment(0, objectId));
            QVERIFY(controller.hasDrafts());
            controller.undo();
            QVERIFY(!controller.hasDrafts());
        } else {
            QVERIFY(!controller.setCommentText(
                0,
                objectId,
                QStringLiteral("Mutation must be rejected")));
            QVERIFY(!controller.removeComment(0, objectId));
            QVERIFY(controller.addCommentReply(
                0,
                objectId,
                QStringLiteral("Reply must be rejected")).isEmpty());
        }
    }
    QCOMPARE(indexedContents, expectedContents);
    QCOMPARE(controller.commentItems().size(), 12);
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::deletesMixedSubtypeSourceReplyThread()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-subtype-replies-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-subtype-replies-output.pdf"));
    QVERIFY(createMixedSubtypeCommentRepliesPdf(sourcePath));
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_TEXT), 2);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_FREETEXT), 1);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_STAMP), 1);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_INK), 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 6, 5000);

    QVariantMap root;
    QVariantMap supportedNestedReply;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        const QString text = comment.value(QStringLiteral("text")).toString();
        if (text == QLatin1String("Mixed root")) {
            root = comment;
        } else if (text == QLatin1String("Text after FreeText")) {
            supportedNestedReply = comment;
        }
    }
    QVERIFY(!root.isEmpty());
    QVERIFY(!supportedNestedReply.isEmpty());
    QVERIFY(supportedNestedReply.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        supportedNestedReply.value(QStringLiteral("parentObjectId")).toString(),
        root.value(QStringLiteral("objectId")).toString());

    QVERIFY(controller.removeComment(
        0,
        root.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.commentItems().isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_TEXT), 0);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_FREETEXT), 0);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_STAMP), 0);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_INK), 0);
    const CommentAnnotationInspection projectionInspection
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("mixed-projection"));
    QVERIFY(projectionInspection.readable);
    QCOMPARE(projectionInspection.annotationIndex, -1);
    QCOMPARE(projectionInspection.annotationCount, 0);
}

void PDFEditControllerTest::rejectsNonMarkupReplyDescendantDeletion()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("non-markup-reply-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("non-markup-reply-output.pdf"));
    QVERIFY(createNonMarkupCommentReplyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap root = controller.commentItems().constFirst().toMap();
    QCOMPARE(
        root.value(QStringLiteral("text")).toString(),
        QStringLiteral("Protected root"));
    QVERIFY(controller.removeComment(
        0,
        root.value(QStringLiteral("objectId")).toString()));

    const PDFSaveWriter::Result result = PDFSaveWriter::save(
        controller,
        outputPath);
    QCOMPARE(
        result.error,
        PDFSaveWriter::Error::CommentAnnotationRemovalFailed);
    QVERIFY(!QFile::exists(outputPath));
    QVERIFY(controller.hasDrafts());
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_TEXT), 1);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_LINK), 1);
}

void PDFEditControllerTest::deletesSourceCommentsWithUndoRedoDiscardAndSave()
{
    const QString basePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comments-base.pdf"));
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comments-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comments-output.pdf"));
    QVERIFY(createFontStylePdf(basePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(basePath)));
    controller.setDocument(&document);
    const QVariantMap squigglyTextCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kUprightText));
    QVERIFY(!squigglyTextCandidate.isEmpty());

    controller.setTool(PDFEditController::HighlightTool);
    QVERIFY(!controller.addHighlight(
        0,
        QRectF(0.10, 0.18, 0.22, 0.04)).isEmpty());
    controller.setTool(PDFEditController::UnderlineTool);
    QVERIFY(!controller.addHighlight(
        0,
        QRectF(0.12, 0.28, 0.24, 0.04)).isEmpty());
    controller.setTool(PDFEditController::StrikeoutTool);
    QVERIFY(!controller.addHighlight(
        0,
        QRectF(0.14, 0.38, 0.26, 0.04)).isEmpty());
    controller.setTool(PDFEditController::SquigglyTool);
    QVERIFY(!controller.addHighlight(
        0,
        squigglyTextCandidate
            .value(QStringLiteral("sourceRect"))
            .toRectF()).isEmpty());
    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.2, 0.6)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Source comment"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(sourcePath)));
    QVERIFY(!controller.hasDrafts());

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 5, 5000);
    const QVariantMap stickyNote = candidateForKind(
        controller.commentItems(),
        QStringLiteral("stickyNote"));
    QVERIFY(!stickyNote.isEmpty());
    QVERIFY(controller.removeComment(
        stickyNote.value(QStringLiteral("page")).toInt(),
        stickyNote.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.commentItems().size(), 4);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());

    QCOMPARE(controller.undo(), -1);
    QCOMPARE(controller.commentItems().size(), 5);
    QVERIFY(controller.canRedo());
    QCOMPARE(controller.redo(), -1);
    QCOMPARE(controller.commentItems().size(), 4);

    controller.discardDrafts();
    QCOMPARE(controller.commentItems().size(), 5);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const QVariantList commentsToDelete = controller.commentItems();
    for (const QVariant &commentValue : commentsToDelete) {
        const QVariantMap comment = commentValue.toMap();
        QVERIFY(controller.removeComment(
            comment.value(QStringLiteral("page")).toInt(),
            comment.value(QStringLiteral("objectId")).toString()));
    }
    QVERIFY(controller.commentItems().isEmpty());
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    QCOMPARE(inspectHighlights(sourcePath).count, 1);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            sourcePath,
            FPDF_ANNOT_UNDERLINE).count,
        1);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            sourcePath,
            FPDF_ANNOT_STRIKEOUT).count,
        1);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            sourcePath,
            FPDF_ANNOT_SQUIGGLY).count,
        1);
    QCOMPARE(inspectStickyNotes(sourcePath).count, 1);

    QCOMPARE(inspectHighlights(outputPath).count, 0);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            outputPath,
            FPDF_ANNOT_UNDERLINE).count,
        0);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            outputPath,
            FPDF_ANNOT_STRIKEOUT).count,
        0);
    QCOMPARE(
        inspectTextMarkupAnnotations(
            outputPath,
            FPDF_ANNOT_SQUIGGLY).count,
        0);
    QCOMPARE(inspectStickyNotes(outputPath).count, 0);
    QVERIFY(normalizedPageText(outputPath).contains(
        QString::fromLatin1(kRegularText)));
}

void PDFEditControllerTest::deletesSourceCommentPopupRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comment-popup-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-comment-popup-output.pdf"));
    QVERIFY(createStickyNotePopupPdf(sourcePath));
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_TEXT), 2);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_POPUP), 2);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    const QVariantList indexedComments = controller.commentItems();
    const QVariantMap comment = indexedComments.constFirst().toMap();
    QCOMPARE(
        comment.value(QStringLiteral("text")).toString(),
        QStringLiteral("Reverse-only popup comment"));
    QCOMPARE(
        comment.value(QStringLiteral("commentAuthor")).toString(),
        QStringLiteral("Alice Example"));
    const QDateTime creationDate = comment
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    QCOMPARE(
        creationDate.toString(Qt::ISODate),
        QStringLiteral("2024-01-02T03:04:05+07:00"));
    QVERIFY(!comment
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        comment.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);

    const QVariantMap preservedComment = indexedComments.at(1).toMap();
    QCOMPARE(
        preservedComment.value(QStringLiteral("commentAuthor")).toString(),
        QStringLiteral("Bob Example"));
    const QDateTime modificationDate = preservedComment
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime();
    QCOMPARE(
        modificationDate.toString(Qt::ISODate),
        QStringLiteral("2024-03-04T05:06:07+07:00"));
    QCOMPARE(
        preservedComment.value(QStringLiteral("commentDate")).toDateTime(),
        modificationDate);
    QVERIFY(controller.removeComment(
        comment.value(QStringLiteral("page")).toInt(),
        comment.value(QStringLiteral("objectId")).toString()));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_TEXT), 2);
    QCOMPARE(annotationSubtypeCount(sourcePath, FPDF_ANNOT_POPUP), 2);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_TEXT), 1);
    QCOMPARE(annotationSubtypeCount(outputPath, FPDF_ANNOT_POPUP), 1);
    const StickyNoteSaveInspection remaining = inspectStickyNotes(outputPath);
    QCOMPARE(remaining.count, 1);
    QCOMPARE(remaining.contents, QStringLiteral("Preserved popup comment"));
    QCOMPARE(remaining.author, QStringLiteral("Bob Example"));
    QCOMPARE(
        remaining.creationDate,
        QStringLiteral("D:20240203040506+07'00'"));
    QCOMPARE(
        remaining.modificationDate,
        QStringLiteral("D:20240304050607+07'00'"));
    QVERIFY(hasSingleBidirectionalCommentPopup(outputPath));
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
    const QVariantMap addedHighlight = controller.addHighlight(
        0,
        selectionRect);
    QVERIFY(!addedHighlight.isEmpty());
    QVERIFY(!addedHighlight
        .value(QStringLiteral("commentAuthor"))
        .toString()
        .isEmpty());
    const QDateTime creationDate = addedHighlight
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    QVERIFY(creationDate.isValid());
    QVERIFY(!addedHighlight
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        addedHighlight.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);

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
    QCOMPARE(inspection.contents, QString());
    QCOMPARE(inspection.quadPoints.size(), 1);
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearance.isEmpty());
    QVERIFY(inspection.appearanceObjectCount > 0);
    QCOMPARE(
        inspection.appearancePathCount,
        inspection.appearanceObjectCount);
    QCOMPARE(
        inspection.author,
        addedHighlight.value(QStringLiteral("commentAuthor")).toString());
    QVERIFY(!inspection.creationDate.isEmpty());
    QVERIFY(inspection.modificationDate.isEmpty());

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

void PDFEditControllerTest::savesUnderlineStrikeoutAndSquigglyAnnotationRoundTrip_data()
{
    QTest::addColumn<int>("tool");
    QTest::addColumn<int>("subtype");
    QTest::addColumn<QString>("kind");

    QTest::newRow("underline")
        << static_cast<int>(PDFEditController::UnderlineTool)
        << static_cast<int>(FPDF_ANNOT_UNDERLINE)
        << QStringLiteral("underline");
    QTest::newRow("strikeout")
        << static_cast<int>(PDFEditController::StrikeoutTool)
        << static_cast<int>(FPDF_ANNOT_STRIKEOUT)
        << QStringLiteral("strikeout");
    QTest::newRow("squiggly")
        << static_cast<int>(PDFEditController::SquigglyTool)
        << static_cast<int>(FPDF_ANNOT_SQUIGGLY)
        << QStringLiteral("squiggly");
}

void PDFEditControllerTest::savesUnderlineStrikeoutAndSquigglyAnnotationRoundTrip()
{
    QFETCH(int, tool);
    QFETCH(int, subtype);
    QFETCH(QString, kind);

    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-%1-source.pdf").arg(kind));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-%1-output.pdf").arg(kind));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);
    QRectF selectionRect(0.14, 0.24, 0.32, 0.04);
    QVector<QRectF> expectedRects;
    if (tool == static_cast<int>(PDFEditController::SquigglyTool)) {
        const QVariantMap textCandidate = candidateForText(
            waitForCandidates(controller, 0),
            QString::fromLatin1(kGroupedText));
        QVERIFY(!textCandidate.isEmpty());
        selectionRect = textCandidate
            .value(QStringLiteral("sourceRect"))
            .toRectF();
        const QVariantList snappedRects
            = controller.textMarkupRectsForSelection(0, selectionRect);
        QVERIFY(!snappedRects.isEmpty());
        expectedRects.reserve(snappedRects.size());
        for (const QVariant &rectValue : snappedRects) {
            expectedRects.append(rectFromCoordinateMap(rectValue));
        }
    } else {
        expectedRects.append(selectionRect);
    }
    controller.setTool(static_cast<PDFEditController::Tool>(tool));
    const QColor annotationColor(38, 166, 154, 160);
    const QColor unrelatedHighlightColor(255, 128, 171, 96);
    if (tool == static_cast<int>(PDFEditController::UnderlineTool)) {
        controller.setHighlightColor(unrelatedHighlightColor);
        controller.setUnderlineColor(annotationColor);
        QCOMPARE(controller.highlightColor(), unrelatedHighlightColor);
    } else if (tool == static_cast<int>(PDFEditController::SquigglyTool)) {
        controller.setHighlightColor(unrelatedHighlightColor);
        controller.setSquigglyColor(annotationColor);
        QCOMPARE(controller.highlightColor(), unrelatedHighlightColor);
    } else if (tool == static_cast<int>(PDFEditController::StrikeoutTool)) {
        controller.setHighlightColor(unrelatedHighlightColor);
        controller.setStrikeoutColor(annotationColor);
        QCOMPARE(controller.highlightColor(), unrelatedHighlightColor);
    } else {
        controller.setHighlightColor(annotationColor);
    }
    const QVariantMap addedAnnotation = controller.addHighlight(
        0,
        selectionRect);
    QCOMPARE(
        addedAnnotation.value(QStringLiteral("kind")).toString(),
        kind);
    QCOMPARE(
        addedAnnotation
            .value(QStringLiteral("highlightColor"))
            .value<QColor>(),
        annotationColor);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());

    const HighlightSaveInspection sourceInspection
        = inspectTextMarkupAnnotations(
            sourcePath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype));
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.count, 0);

    const HighlightSaveInspection inspection
        = inspectTextMarkupAnnotations(
            outputPath,
            static_cast<FPDF_ANNOTATION_SUBTYPE>(subtype));
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.color, annotationColor);
    QVERIFY(inspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(inspection.contents, QString());
    QCOMPARE(inspection.quadPoints.size(), expectedRects.size());
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearance.isEmpty());
    QVERIFY(inspection.appearanceObjectCount > 0);
    QCOMPARE(
        inspection.appearancePathCount,
        inspection.appearanceObjectCount);

    QRectF expectedAnnotationRect;
    for (const QRectF &rect : std::as_const(expectedRects)) {
        expectedAnnotationRect = expectedAnnotationRect.isEmpty()
            ? rect
            : expectedAnnotationRect.united(rect);
    }
    const qreal expectedLeft = expectedAnnotationRect.left()
        * pageSize.width();
    const qreal expectedRight = expectedAnnotationRect.right()
        * pageSize.width();
    const qreal expectedTop = (1.0 - expectedAnnotationRect.top())
        * pageSize.height();
    const qreal expectedBottom = (1.0 - expectedAnnotationRect.bottom())
        * pageSize.height();
    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    QVERIFY(closeEnough(inspection.rect.left, expectedLeft));
    QVERIFY(closeEnough(inspection.rect.right, expectedRight));
    QVERIFY(closeEnough(inspection.rect.top, expectedTop));
    QVERIFY(closeEnough(inspection.rect.bottom, expectedBottom));
    for (qsizetype index = 0; index < expectedRects.size(); ++index) {
        const QRectF rect = expectedRects.at(index);
        const qreal quadLeft = rect.left() * pageSize.width();
        const qreal quadRight = rect.right() * pageSize.width();
        const qreal quadTop = (1.0 - rect.top()) * pageSize.height();
        const qreal quadBottom = (1.0 - rect.bottom()) * pageSize.height();
        const FS_QUADPOINTSF quad = inspection.quadPoints.at(index);
        QVERIFY(closeEnough(quad.x1, quadLeft));
        QVERIFY(closeEnough(quad.y1, quadTop));
        QVERIFY(closeEnough(quad.x2, quadRight));
        QVERIFY(closeEnough(quad.y2, quadTop));
        QVERIFY(closeEnough(quad.x3, quadLeft));
        QVERIFY(closeEnough(quad.y3, quadBottom));
        QVERIFY(closeEnough(quad.x4, quadRight));
        QVERIFY(closeEnough(quad.y4, quadBottom));
    }
}

void PDFEditControllerTest::savesStickyNoteRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-sticky-note-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-sticky-note-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::StickyNoteTool);
    const QSizeF pageSize = document.pageSize(0);
    const QPointF position(0.2, 0.3);
    const QString commentText = QStringLiteral(
        "Cần kiểm tra lại số liệu.");
    const QVariantMap addedNote = controller.addStickyNote(0, position);
    QVERIFY(!addedNote.isEmpty());
    const QString commentAuthor = addedNote
        .value(QStringLiteral("commentAuthor"))
        .toString();
    const QDateTime creationDate = addedNote
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    QVERIFY(!commentAuthor.isEmpty());
    QVERIFY(creationDate.isValid());
    QVERIFY(!addedNote
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime()
        .isValid());
    QCOMPARE(
        addedNote.value(QStringLiteral("commentDate")).toDateTime(),
        creationDate);
    controller.setSelectedCommentText(commentText);
    const QVariantMap editedNote = controller.commentItems()
        .constFirst()
        .toMap();
    const QDateTime modificationDate = editedNote
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime();
    QVERIFY(modificationDate.isValid());
    QVERIFY(modificationDate >= creationDate);
    QCOMPARE(
        editedNote.value(QStringLiteral("commentDate")).toDateTime(),
        modificationDate);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const StickyNoteSaveInspection sourceInspection
        = inspectStickyNotes(sourcePath);
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.count, 0);

    const StickyNoteSaveInspection inspection
        = inspectStickyNotes(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QVERIFY(inspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QCOMPARE(inspection.contents, commentText);
    QCOMPARE(inspection.author, commentAuthor);
    QVERIFY(!inspection.creationDate.isEmpty());
    QVERIFY(!inspection.modificationDate.isEmpty());
    const CommentAnnotationInspection appearanceInspection
        = inspectCommentAnnotation(
            outputPath,
            addedNote.value(QStringLiteral("objectId")).toString());
    QVERIFY(appearanceInspection.readable);
    QVERIFY(appearanceInspection.hasAppearance);
    QVERIFY(!appearanceInspection.appearance.isEmpty());

    const qreal expectedLeft = position.x() * pageSize.width();
    const qreal expectedRight = expectedLeft + 24.0;
    const qreal expectedTop = (1.0 - position.y()) * pageSize.height();
    const qreal expectedBottom = expectedTop - 24.0;
    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    QVERIFY(closeEnough(inspection.rect.left, expectedLeft));
    QVERIFY(closeEnough(inspection.rect.right, expectedRight));
    QVERIFY(closeEnough(inspection.rect.top, expectedTop));
    QVERIFY(closeEnough(inspection.rect.bottom, expectedBottom));

    const QSize renderSize(
        qRound(pageSize.width()),
        qRound(pageSize.height()));
    const QImage withoutAnnotations = renderFirstPage(
        outputPath,
        renderSize);
    const QImage withAnnotations = renderFirstPage(
        outputPath,
        renderSize,
        FPDF_ANNOT);
    const QRect notePixels(
        qRound(position.x() * renderSize.width()),
        qRound(position.y() * renderSize.height()),
        26,
        26);
    QVERIFY(changedPixelCount(
        withoutAnnotations,
        withAnnotations,
        notePixels) > 0);

    const QVariantMap rehydratedNote = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("stickyNote"));
    QVERIFY(!rehydratedNote.isEmpty());
    QCOMPARE(
        rehydratedNote.value(QStringLiteral("highlightColor")).value<QColor>(),
        QColor(255, 235, 59));
    QCOMPARE(
        rehydratedNote.value(QStringLiteral("text")).toString(),
        commentText);
    QCOMPARE(
        rehydratedNote.value(QStringLiteral("commentAuthor")).toString(),
        commentAuthor);
    const QDateTime rehydratedCreationDate = rehydratedNote
        .value(QStringLiteral("commentCreationDate"))
        .toDateTime();
    const QDateTime rehydratedModificationDate = rehydratedNote
        .value(QStringLiteral("commentModifiedDate"))
        .toDateTime();
    QCOMPARE(
        rehydratedCreationDate.toSecsSinceEpoch(),
        creationDate.toSecsSinceEpoch());
    QCOMPARE(
        rehydratedModificationDate.toSecsSinceEpoch(),
        modificationDate.toSecsSinceEpoch());
    QVERIFY(!rehydratedNote.value(QStringLiteral("editable")).toBool());
    QVERIFY(controller.selectComment(
        0,
        rehydratedNote.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!controller.selectedEditable());
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    QVERIFY(controller.commentItems().constFirst().toMap()
        .value(QStringLiteral("commentTextEditable")).toBool());
    QCOMPARE(controller.commentItems().size(), 1);
}

void PDFEditControllerTest::savesAndEditsFreeTextRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-free-text-source.pdf"));
    const QString firstOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-free-text-first-output.pdf"));
    const QString secondOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-free-text-second-output.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);
    const QRectF placement(0.12, 0.14, 0.28, 0.15);
    const QString firstText = QStringLiteral(
        "Tiếng Việt có dấu phải hiển thị đầy đủ và tự động xuống dòng "
        "bên trong hộp Free Text hẹp.");

    controller.setTool(PDFEditController::FreeTextTool);
    const QVariantMap added = controller.addFreeText(0, placement);
    QVERIFY(!added.isEmpty());
    const QString objectId = added
        .value(QStringLiteral("objectId"))
        .toString();
    const QString author = added
        .value(QStringLiteral("commentAuthor"))
        .toString();
    QVERIFY(!objectId.isEmpty());
    QVERIFY(!author.isEmpty());
    QVERIFY(controller.setCommentText(0, objectId, firstText));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(firstOutputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection firstInspection
        = inspectCommentAnnotation(firstOutputPath, objectId);
    QVERIFY(firstInspection.readable);
    QCOMPARE(firstInspection.annotationCount, 1);
    QCOMPARE(firstInspection.subtype, FPDF_ANNOT_FREETEXT);
    QCOMPARE(firstInspection.contents, firstText);
    QCOMPARE(firstInspection.author, author);
    QVERIFY(!firstInspection.creationDate.isEmpty());
    QVERIFY(!firstInspection.modificationDate.isEmpty());
    QVERIFY(firstInspection.flags & FPDF_ANNOT_FLAG_PRINT);
    QVERIFY(!firstInspection.defaultAppearance.isEmpty());
    QVERIFY(firstInspection.hasAppearance);

    const auto closeEnough = [](qreal actual, qreal expected) {
        return qAbs(actual - expected) < 0.05;
    };
    QVERIFY(closeEnough(
        firstInspection.rect.left,
        placement.left() * pageSize.width()));
    QVERIFY(closeEnough(
        firstInspection.rect.right,
        placement.right() * pageSize.width()));
    QVERIFY(closeEnough(
        firstInspection.rect.top,
        (1.0 - placement.top()) * pageSize.height()));
    QVERIFY(closeEnough(
        firstInspection.rect.bottom,
        (1.0 - placement.bottom()) * pageSize.height()));

    const QSize renderSize(
        qRound(pageSize.width()),
        qRound(pageSize.height()));
    const QImage sourceWithoutAnnotations = renderFirstPage(
        sourcePath,
        renderSize);
    const QImage firstWithoutAnnotations = renderFirstPage(
        firstOutputPath,
        renderSize);
    const QImage firstWithAnnotations = renderFirstPage(
        firstOutputPath,
        renderSize,
        FPDF_ANNOT);
    QCOMPARE(
        changedPixelCount(
            sourceWithoutAnnotations,
            firstWithoutAnnotations,
            sourceWithoutAnnotations.rect()),
        0);
    const QRect placementPixels(
        qRound(placement.x() * renderSize.width()),
        qRound(placement.y() * renderSize.height()),
        qRound(placement.width() * renderSize.width()),
        qRound(placement.height() * renderSize.height()));
    const QRect textInterior = placementPixels.adjusted(6, 6, -6, -6);
    const QRect firstTextBounds = darkChangedPixelBounds(
        firstWithoutAnnotations,
        firstWithAnnotations,
        textInterior);
    QVERIFY2(
        !firstTextBounds.isEmpty(),
        "The Free Text appearance did not render Vietnamese glyphs.");
    QVERIFY2(
        firstTextBounds.height() > 20,
        "The narrow Free Text appearance did not wrap onto multiple lines.");

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    const QVariantMap rehydrated = controller.commentItems()
        .constFirst()
        .toMap();
    QCOMPARE(
        rehydrated.value(QStringLiteral("kind")).toString(),
        QStringLiteral("freeText"));
    QCOMPARE(
        rehydrated.value(QStringLiteral("text")).toString(),
        firstText);
    QVERIFY(rehydrated
        .value(QStringLiteral("commentTextEditable"))
        .toBool());
    const QString rehydratedObjectId = rehydrated
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!rehydratedObjectId.isEmpty());

    const QString secondText = QStringLiteral(
        "Nội dung Free Text đã được chỉnh sửa sau khi mở lại.");
    QVERIFY(controller.setCommentText(
        0,
        rehydratedObjectId,
        secondText));
    const QString replyText = QStringLiteral(
        "Phản hồi cho bình luận Free Text.");
    const QVariantMap reply = controller.addCommentReply(
        0,
        rehydratedObjectId,
        replyText);
    QVERIFY(!reply.isEmpty());
    const QString replyId = reply
        .value(QStringLiteral("objectId"))
        .toString();
    QVERIFY(!replyId.isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(secondOutputPath)));
    QVERIFY(!controller.hasDrafts());

    const CommentAnnotationInspection preservedFirst
        = inspectCommentAnnotation(firstOutputPath, objectId);
    QCOMPARE(preservedFirst.contents, firstText);
    const CommentAnnotationInspection secondInspection
        = inspectCommentAnnotation(secondOutputPath, objectId);
    QVERIFY(secondInspection.readable);
    QCOMPARE(secondInspection.subtype, FPDF_ANNOT_FREETEXT);
    QCOMPARE(secondInspection.contents, secondText);
    QVERIFY(secondInspection.hasAppearance);
    QVERIFY(!secondInspection.modificationDate.isEmpty());
    const CommentAnnotationInspection replyInspection
        = inspectCommentAnnotation(secondOutputPath, replyId);
    QVERIFY(replyInspection.readable);
    QCOMPARE(replyInspection.subtype, FPDF_ANNOT_TEXT);
    QCOMPARE(replyInspection.contents, replyText);
    QCOMPARE(replyInspection.parentName, objectId);
    QCOMPARE(replyInspection.relation, QStringLiteral("R"));

    const QImage secondWithAnnotations = renderFirstPage(
        secondOutputPath,
        renderSize,
        FPDF_ANNOT);
    QVERIFY(
        changedPixelCount(
            firstWithAnnotations,
            secondWithAnnotations,
            placementPixels) > 0);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    QVariantMap rehydratedRoot;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("kind")).toString()
                == QLatin1String("freeText")
            && !comment.value(QStringLiteral("isReply")).toBool()) {
            rehydratedRoot = comment;
            break;
        }
    }
    QVERIFY(!rehydratedRoot.isEmpty());
    QCOMPARE(
        rehydratedRoot.value(QStringLiteral("text")).toString(),
        secondText);
}

void PDFEditControllerTest::savesFreeTextWithPageLocalDuplicateNames()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("duplicate-free-text-names-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("duplicate-free-text-names-output.pdf"));
    QVERIFY(createPageLocalDuplicateFreeTextNamesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);

    QVariantMap secondPageComment;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        QCOMPARE(
            comment.value(QStringLiteral("kind")).toString(),
            QStringLiteral("freeText"));
        if (comment.value(QStringLiteral("page")).toInt() == 1) {
            secondPageComment = comment;
        }
    }
    QVERIFY(!secondPageComment.isEmpty());
    const QString replacement = QStringLiteral(
        "Nội dung chỉ được thay trên trang thứ hai.");
    QVERIFY(controller.setCommentText(
        1,
        secondPageComment.value(QStringLiteral("objectId")).toString(),
        replacement));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const CommentAnnotationInspection firstPage
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("page-local-name"),
            0);
    const CommentAnnotationInspection secondPage
        = inspectCommentAnnotation(
            outputPath,
            QStringLiteral("page-local-name"),
            1);
    QVERIFY(firstPage.readable);
    QVERIFY(secondPage.readable);
    QCOMPARE(firstPage.contents, QStringLiteral("Page zero text"));
    QCOMPARE(secondPage.contents, replacement);
    QVERIFY(!firstPage.hasAppearance);
    QVERIFY(secondPage.hasAppearance);
}

void PDFEditControllerTest::savesCommentReplyRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-comment-reply-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-comment-reply-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    PDFFormDesignerController formDesigner;
    formDesigner.setDocument(&document);
    controller.setFormDesignerController(&formDesigner);
    const QString formFieldId = formDesigner.addTextField(
        0,
        0.55,
        0.72,
        0.3,
        0.06);
    QVERIFY(!formFieldId.isEmpty());
    QVERIFY(formDesigner.setFieldName(
        formFieldId,
        QStringLiteral("ReplyCompanion")));
    controller.setTool(PDFEditController::StickyNoteTool);

    const QString rootText = QStringLiteral("Root comment");
    const QString replyText = QStringLiteral("Persisted reply");
    const QVariantMap root = controller.addStickyNote(
        0,
        QPointF(0.2, 0.3));
    QVERIFY(!root.isEmpty());
    controller.setSelectedCommentText(rootText);
    const QVariantMap reply = controller.addCommentReply(
        0,
        root.value(QStringLiteral("objectId")).toString(),
        replyText);
    QVERIFY(!reply.isEmpty());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const CommentReplyInspection sourceInspection
        = inspectCommentReply(sourcePath);
    QVERIFY(sourceInspection.readable);
    QCOMPARE(sourceInspection.textAnnotationCount, 0);

    const CommentReplyInspection inspection
        = inspectCommentReply(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.textAnnotationCount, 2);
    QVERIFY(inspection.rootIndex >= 0);
    QVERIFY(inspection.replyIndex >= 0);
    QVERIFY(inspection.rootIndex != inspection.replyIndex);
    QCOMPARE(inspection.rootContents, rootText);
    QCOMPARE(inspection.replyContents, replyText);
    QCOMPARE(inspection.relation, QStringLiteral("R"));
    QVERIFY(!inspection.parentThreadId.isEmpty());
    QCOMPARE(inspection.replyToThreadId, inspection.parentThreadId);
    const CommentAnnotationInspection rootAppearance
        = inspectCommentAnnotation(
            outputPath,
            root.value(QStringLiteral("objectId")).toString());
    const CommentAnnotationInspection replyAppearance
        = inspectCommentAnnotation(
            outputPath,
            reply.value(QStringLiteral("objectId")).toString());
    QVERIFY(rootAppearance.hasAppearance);
    QVERIFY(!rootAppearance.appearance.isEmpty());
    QVERIFY(replyAppearance.hasAppearance);
    QVERIFY(!replyAppearance.appearance.isEmpty());
    const AcroFormFieldInspection fieldInspection
        = inspectAcroFormField(
            outputPath,
            QStringLiteral("ReplyCompanion"));
    QVERIFY(fieldInspection.readable);
    QCOMPARE(fieldInspection.widgetCount, 1);
    QCOMPARE(fieldInspection.fieldType, FPDF_FORMFIELD_TEXTFIELD);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    const QVariantList comments = controller.commentItems();
    const QVariantMap rehydratedRoot = comments.at(0).toMap();
    const QVariantMap rehydratedReply = comments.at(1).toMap();
    const QString rehydratedRootObjectId = rehydratedRoot
        .value(QStringLiteral("objectId"))
        .toString();

    QCOMPARE(
        rehydratedRoot.value(QStringLiteral("text")).toString(),
        rootText);
    QVERIFY(!rehydratedRoot.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        rehydratedRoot.value(QStringLiteral("parentObjectId")).toString(),
        QString());
    QCOMPARE(
        rehydratedRoot.value(QStringLiteral("replyCount")).toInt(),
        1);
    QCOMPARE(
        rehydratedReply.value(QStringLiteral("text")).toString(),
        replyText);
    QVERIFY(rehydratedReply.value(QStringLiteral("isReply")).toBool());
    QCOMPARE(
        rehydratedReply.value(QStringLiteral("parentObjectId")).toString(),
        rehydratedRootObjectId);
    QCOMPARE(
        rehydratedReply.value(QStringLiteral("replyCount")).toInt(),
        0);
    QVERIFY(!rehydratedRoot.value(QStringLiteral("editable")).toBool());
    QVERIFY(!rehydratedReply.value(QStringLiteral("editable")).toBool());
    QVERIFY(rehydratedRoot
        .value(QStringLiteral("commentTextEditable")).toBool());
    QVERIFY(rehydratedReply
        .value(QStringLiteral("commentTextEditable")).toBool());
}

void PDFEditControllerTest::normalizesCommentContentsLineEndingsOnSave()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-line-endings-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-line-endings-output.pdf"));
    QVERIFY(createRootCommentOnlyPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);

    const QVariantMap root = controller.commentItems().constFirst().toMap();
    const QString rootObjectId = root
        .value(QStringLiteral("objectId"))
        .toString();
    const QString mixedLineEndings
        = QStringLiteral("Dòng 1\nDòng 2\r\nDòng 3\rDòng 4");
    const QString editingText
        = QStringLiteral("Dòng 1\nDòng 2\nDòng 3\nDòng 4");
    const QString pdfText
        = QStringLiteral("Dòng 1\rDòng 2\rDòng 3\rDòng 4");
    QVERIFY(controller.setCommentText(
        0,
        rootObjectId,
        mixedLineEndings));
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        editingText);

    const QVariantMap reply = controller.addCommentReply(
        0,
        rootObjectId,
        mixedLineEndings);
    QVERIFY(!reply.isEmpty());
    controller.setTool(PDFEditController::FreeTextTool);
    const QVariantMap freeText = controller.addFreeText(
        0,
        QRectF(0.15, 0.55, 0.35, 0.15));
    QVERIFY(!freeText.isEmpty());
    QVERIFY(controller.setCommentText(
        0,
        freeText.value(QStringLiteral("objectId")).toString(),
        mixedLineEndings));
    const QVariantMap highlight = controller.addHighlight(
        0,
        QRectF(0.15, 0.35, 0.35, 0.05));
    QVERIFY(!highlight.isEmpty());
    QVERIFY(controller.setCommentText(
        0,
        highlight.value(QStringLiteral("objectId")).toString(),
        mixedLineEndings));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QVector<QString> annotationNames{
        QStringLiteral("root-comment"),
        reply.value(QStringLiteral("objectId")).toString(),
        freeText.value(QStringLiteral("objectId")).toString(),
        highlight.value(QStringLiteral("objectId")).toString(),
    };
    for (const QString &annotationName : annotationNames) {
        const CommentAnnotationInspection inspection
            = inspectCommentAnnotation(outputPath, annotationName);
        QVERIFY(inspection.readable);
        QCOMPARE(inspection.contents, pdfText);
        QVERIFY(!inspection.contents.contains(QLatin1Char('\n')));
        QVERIFY(inspection.hasAppearance);
        QVERIFY(!inspection.appearance.isEmpty());
    }

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 4, 5000);
    for (const QVariant &commentValue : controller.commentItems()) {
        QCOMPARE(
            commentValue.toMap().value(QStringLiteral("text")).toString(),
            editingText);
    }
}

void PDFEditControllerTest::keepsStickyNoteNamesUniqueAcrossReopen()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("sticky-note-name-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("sticky-note-name-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.2, 0.3)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("First comment"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const StickyNoteSaveInspection firstInspection
        = inspectStickyNotes(outputPath);
    QVERIFY(firstInspection.readable);
    QCOMPARE(firstInspection.count, 1);
    QCOMPARE(firstInspection.names.size(), 1);
    QVERIFY(!firstInspection.names.constFirst().isEmpty());
    const QString firstName = firstInspection.names.constFirst();

    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.55, 0.45)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Second comment"));
    QVERIFY(controller.save());

    const StickyNoteSaveInspection secondInspection
        = inspectStickyNotes(outputPath);
    QVERIFY(secondInspection.readable);
    QCOMPARE(secondInspection.count, 2);
    QCOMPARE(secondInspection.names.size(), 2);
    QSet<QString> uniqueNames;
    for (const QString &name : secondInspection.names) {
        QVERIFY(!name.isEmpty());
        uniqueNames.insert(name);
    }
    QCOMPARE(uniqueNames.size(), secondInspection.names.size());
    QVERIFY(uniqueNames.contains(firstName));
}

void PDFEditControllerTest::keepsMarkupReplyIdsUniqueAcrossReopen()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("markup-reply-id-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("markup-reply-id-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    controller.setTool(PDFEditController::HighlightTool);
    QVERIFY(!controller.addHighlight(
        0,
        QRectF(0.12, 0.18, 0.22, 0.04)).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    const QString sourceHighlightObjectId = controller.commentItems()
        .constFirst()
        .toMap()
        .value(QStringLiteral("objectId"))
        .toString();

    controller.setTool(PDFEditController::HighlightTool);
    const QVariantMap secondHighlight = controller.addHighlight(
        0,
        QRectF(0.12, 0.28, 0.22, 0.04));
    QVERIFY(!secondHighlight.isEmpty());
    const QVariantMap reply = controller.addCommentReply(
        0,
        sourceHighlightObjectId,
        QStringLiteral("Reply to source highlight"));
    QVERIFY(!reply.isEmpty());
    QVERIFY(controller.save());

    const CommentAnnotationInspection replyInspection
        = inspectCommentAnnotation(
            outputPath,
            reply.value(QStringLiteral("objectId")).toString());
    QVERIFY(replyInspection.readable);
    QVERIFY(replyInspection.hasAppearance);
    QVERIFY(replyInspection.appearance.contains(QStringLiteral("/GS gs")));

    const AnnotationAppearanceStructureInspection appearanceStructure
        = inspectAnnotationAppearanceStructure(
            outputPath,
            reply.value(QStringLiteral("objectId")).toString());
    QVERIFY(appearanceStructure.readable);
    QVERIFY(appearanceStructure.annotationFound);
    QVERIFY(appearanceStructure.normalAppearanceIsIndirect);
    QVERIFY(appearanceStructure.appearanceObjectFound);
    QVERIFY(appearanceStructure.appearanceIsStream);
    QVERIFY(appearanceStructure.typeIsXObject);
    QVERIFY(appearanceStructure.subtypeIsForm);
    QCOMPARE(appearanceStructure.boundingBox.size(), 4);
    QVERIFY(appearanceStructure.hasResources);
    QVERIFY(appearanceStructure.hasExtGState);
    QVERIFY(appearanceStructure.hasGraphicsState);
    QVERIFY(appearanceStructure.graphicsStateTypeIsExtGState);
    QVERIFY(appearanceStructure.blendModeIsNormal);
    QVERIFY(appearanceStructure.alphaSourceIsFalse);

    constexpr qreal expectedReplyOpacity = 96.0 / 255.0;
    QVERIFY(qAbs(appearanceStructure.strokingOpacity
                 - expectedReplyOpacity) < 0.00001);
    QVERIFY(qAbs(appearanceStructure.nonStrokingOpacity
                 - expectedReplyOpacity) < 0.00001);
    QVERIFY(qAbs(appearanceStructure.boundingBox.at(0)
                 - replyInspection.rect.left) < 0.01);
    QVERIFY(qAbs(appearanceStructure.boundingBox.at(1)
                 - replyInspection.rect.bottom) < 0.01);
    QVERIFY(qAbs(appearanceStructure.boundingBox.at(2)
                 - replyInspection.rect.right) < 0.01);
    QVERIFY(qAbs(appearanceStructure.boundingBox.at(3)
                 - replyInspection.rect.top) < 0.01);

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 3, 5000);
    int replyCount = 0;
    int rootsWithReplies = 0;
    for (const QVariant &commentValue : controller.commentItems()) {
        const QVariantMap comment = commentValue.toMap();
        if (comment.value(QStringLiteral("isReply")).toBool()) {
            ++replyCount;
        } else if (comment.value(QStringLiteral("replyCount")).toInt() > 0) {
            ++rootsWithReplies;
        }
    }
    QCOMPARE(replyCount, 1);
    QCOMPARE(rootsWithReplies, 1);
}

void PDFEditControllerTest::indexesCommentsAcrossDocument()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.15, 0.2)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("First comment"));
    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(2, QPointF(0.25, 0.3)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Third-page comment"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    const QVariantList comments = controller.commentItems();
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("page")).toInt(),
        0);
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("First comment"));
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("page")).toInt(),
        2);
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Third-page comment"));

    QCOMPARE(controller.selectRelativeComment(1), 0);
    QCOMPARE(controller.selectRelativeComment(1), 2);
    QCOMPARE(controller.selectRelativeComment(-1), 0);
}

void PDFEditControllerTest::reindexesCommentsAfterDiscardingPageDeletion()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-page-discard-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("comment-index-page-discard-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    connect(
        &controller,
        &PDFEditController::commentsChanged,
        &controller,
        &PDFEditController::requestCommentIndex);

    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(0, QPointF(0.15, 0.2)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("First-page comment"));
    controller.setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller.addStickyNote(1, QPointF(0.25, 0.3)).isEmpty());
    controller.setSelectedCommentText(QStringLiteral("Second-page comment"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    controller.requestCommentIndex();
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    QCOMPARE(
        controller.commentItems().at(0).toMap()
            .value(QStringLiteral("objectId")).toString(),
        QStringLiteral("annotation:0"));
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("objectId")).toString(),
        QStringLiteral("annotation:0"));

    document.setCurrentPage(0);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 1, 5000);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("Second-page comment"));

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(controller.commentItems().size(), 2, 5000);
    const QVariantList comments = controller.commentItems();
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("page")).toInt(),
        0);
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("First-page comment"));
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("page")).toInt(),
        1);
    QCOMPARE(
        comments.at(1).toMap().value(QStringLiteral("text")).toString(),
        QStringLiteral("Second-page comment"));
    QCOMPARE(
        comments.at(0).toMap().value(QStringLiteral("objectId")).toString(),
        comments.at(1).toMap().value(QStringLiteral("objectId")).toString());
    QVERIFY(controller.setCommentText(
        1,
        comments.at(1).toMap()
            .value(QStringLiteral("objectId")).toString(),
        QStringLiteral("Second-page identity edit")));
    QCOMPARE(
        controller.commentItems().at(0).toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("First-page comment"));
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("Second-page identity edit"));
    QCOMPARE(controller.undo(), -1);
    QCOMPARE(
        controller.commentItems().at(1).toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("Second-page comment"));
    QVERIFY(controller.removeComment(
        1,
        comments.at(1).toMap()
            .value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.commentItems().size(), 1);
    QCOMPARE(
        controller.commentItems().constFirst().toMap()
            .value(QStringLiteral("text")).toString(),
        QStringLiteral("First-page comment"));
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
    QVERIFY(inspection.hasAppearance);
    QVERIFY(!inspection.appearance.isEmpty());
    QVERIFY(inspection.appearanceObjectCount > 0);
    QCOMPARE(
        inspection.appearancePathCount,
        inspection.appearanceObjectCount);
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
