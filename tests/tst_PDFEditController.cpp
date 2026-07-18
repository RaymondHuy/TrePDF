#include "ApplicationFonts.h"
#include "PDFDocument.h"
#include "PDFEditController.h"

#include <QFile>
#include <QFontMetricsF>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>

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

bool addTextObject(FPDF_DOCUMENT document,
                   FPDF_PAGE page,
                   const char *fontName,
                   const QString &text,
                   double x,
                   double y)
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

} // namespace

class PDFEditControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void registersBundledFonts();
    void detectsPdfFontStyles();
    void propagatesSourceStylesThroughSelection();
    void groupsCompatibleTextObjectsOnSameLine();
    void seedsGroupedFrameFromQtTextWidth();
    void keepsMixedStylesAsSeparateCandidates();
    void keepsDistantColumnsAsSeparateCandidates();
    void keepsDifferentFontFacesAsSeparateCandidates();
    void editsGroupedLineAsOneDraft();
    void resizesDraftFrameWithUndoRedo();
    void storesReflowedContentBoundsWithUndoRedo();
    void changesFontFamilyForCurrentEdit();
    void changesFontStylesForCurrentEdit();
    void formatsSelectedRangeInMultilineDraft();
    void appliesFontStylesToDisplayFont_data();
    void appliesFontStylesToDisplayFont();
    void savesEditedTextRoundTrip();
    void savesCurrentFileRoundTrip();
    void savesUnchangedCopyRoundTrip();
    void savesMultipleDraftsOnSamePage();
    void savesDeletedTextRoundTrip();
    void savesAddedTextRoundTrip();
    void preservesAddedTextLayerOrder();
    void savesRichMultilineTextRoundTrip();
    void savesOffsetCropCoordinates();
    void savesAddedTextOnRotatedPage();
    void preservesStrokeAppearanceAndRejectsClipping();
    void savesAsynchronously();
    void keepsDraftsWhenSaveFails();

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
    const QVariantList candidates = m_controller.candidatesForPage(0);
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
    QVERIFY(availableFamilies.size()
            >= ApplicationFonts::systemFontFamilies().size());
    QCOMPARE(
        availableFamilies.first(),
        ApplicationFonts::bundledFontFamilies().first());
    for (const QString &family : ApplicationFonts::systemFontFamilies()) {
        QVERIFY(availableFamilies.contains(family, Qt::CaseInsensitive));
    }

    QStringList uniqueFamilies;
    for (const QString &family : availableFamilies) {
        QVERIFY(!uniqueFamilies.contains(family, Qt::CaseInsensitive));
        uniqueFamilies.append(family);
    }
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

    const QRectF frameRect = m_controller.selectedFrameRect();
    const QRectF contentBounds(
        frameRect.x(),
        frameRect.y(),
        frameRect.width(),
        qMin(1.0 - frameRect.y(), frameRect.height() + 0.05));
    const QString multilineText = QStringLiteral("First line\nSecond line");

    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(multilineText);
    m_controller.updateSelectedContentBounds(contentBounds);
    QCOMPARE(m_controller.selectedFrameRect(), frameRect);
    QCOMPARE(m_controller.selectedContentBounds(), contentBounds);
    m_controller.finishSelectedTextEdit();

    QCOMPARE(m_controller.draftsForPage(0).size(), 1);
    m_controller.undo();
    QCOMPARE(m_controller.selectedFrameRect(), frameRect);
    QCOMPARE(m_controller.selectedContentBounds(), frameRect);
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

void PDFEditControllerTest::changesFontStylesForCurrentEdit()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!m_controller.selectedFontBold());
    QVERIFY(!m_controller.selectedFontItalic());
    QVERIFY(!m_controller.selectedFontUnderline());

    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedFontItalic(true);
    m_controller.setSelectedFontUnderline(true);
    QVERIFY(m_controller.textEditActive());
    QVERIFY(m_controller.selectedFontBold());
    QVERIFY(m_controller.selectedFontItalic());
    QVERIFY(m_controller.selectedFontUnderline());

    QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap styledDraft = drafts.constFirst().toMap();
    QCOMPARE(styledDraft.value(QStringLiteral("fontBold")).toBool(), true);
    QCOMPARE(styledDraft.value(QStringLiteral("fontItalic")).toBool(), true);
    QCOMPARE(styledDraft.value(QStringLiteral("fontUnderline")).toBool(), true);

    m_controller.finishSelectedTextEdit();
    QVERIFY(m_controller.canUndo());

    m_controller.undo();
    QVERIFY(!m_controller.selectedFontBold());
    QVERIFY(!m_controller.selectedFontItalic());
    QVERIFY(!m_controller.selectedFontUnderline());
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QVERIFY(m_controller.selectedFontBold());
    QVERIFY(m_controller.selectedFontItalic());
    QVERIFY(m_controller.selectedFontUnderline());

    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedFontBold(false);
    m_controller.setSelectedFontItalic(false);
    m_controller.setSelectedFontUnderline(false);
    QVERIFY(!m_controller.selectedFontBold());
    QVERIFY(!m_controller.selectedFontItalic());
    QVERIFY(!m_controller.selectedFontUnderline());
    m_controller.cancelSelectedTextEdit();
    QVERIFY(m_controller.selectedFontBold());
    QVERIFY(m_controller.selectedFontItalic());
    QVERIFY(m_controller.selectedFontUnderline());

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
    m_controller.setSelectedTextRange(0, 3);
    m_controller.setSelectedColor(rangeColor);
    m_controller.setSelectedFontBold(true);

    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap draft = drafts.constFirst().toMap();
    QCOMPARE(draft.value(QStringLiteral("text")).toString(), text);

    const QVariantList spans = draft.value(QStringLiteral("characterSpans")).toList();
    QCOMPARE(spans.size(), 2);
    const QVariantMap firstSpan = spans.at(0).toMap();
    QCOMPARE(firstSpan.value(QStringLiteral("start")).toLongLong(), 0);
    QCOMPARE(firstSpan.value(QStringLiteral("length")).toLongLong(), 3);
    QCOMPARE(firstSpan.value(QStringLiteral("fontBold")).toBool(), true);
    QCOMPARE(firstSpan.value(QStringLiteral("color")).value<QColor>(), rangeColor);
    const QVariantMap remainingSpan = spans.at(1).toMap();
    QCOMPARE(remainingSpan.value(QStringLiteral("start")).toLongLong(), 3);
    QCOMPARE(
        remainingSpan.value(QStringLiteral("length")).toLongLong(),
        text.size() - 3);
    QCOMPARE(remainingSpan.value(QStringLiteral("fontBold")).toBool(), false);
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
        2);

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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
    const QVariantList candidates = controller.candidatesForPage(0);
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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
    const QVariantList candidates = controller.candidatesForPage(0);
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
        controller.candidatesForPage(0),
        QString::fromLatin1(kRegularText));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Async saved text"));

    QSignalSpy saveFinishedSpy(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(controller.saving());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 5000);
    QCOMPARE(saveFinishedSpy.constFirst().constFirst().toBool(), true);
    QVERIFY(!controller.saving());
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(normalizedPageText(outputPath).contains(
        QStringLiteral("Async saved text")));
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
        controller.candidatesForPage(0),
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

QTEST_MAIN(PDFEditControllerTest)

#include "tst_PDFEditController.moc"
