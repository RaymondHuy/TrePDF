#include "ApplicationFonts.h"
#include "DraftImageItem.h"
#include "PDFDocument.h"
#include "PDFEditController.h"
#include "PDFSaveWriter.h"
#include "PDFTabModel.h"

#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QRawFont>
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

#include <algorithm>
#include <cmath>

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

struct ImageSaveInspection {
    bool readable = false;
    bool pixelsReadable = false;
    int count = 0;
    unsigned int pixelWidth = 0;
    unsigned int pixelHeight = 0;
    QColor topLeftPixel;
    QColor bottomLeftPixel;
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

} // namespace

class PDFEditControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void registersBundledFonts();
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
    void changesFontStylesForCurrentEdit();
    void formatsSelectedRangeInMultilineDraft();
    void calibratesDisplayStretchToPdfScale_data();
    void calibratesDisplayStretchToPdfScale();
    void appliesFontStylesToDisplayFont_data();
    void appliesFontStylesToDisplayFont();
    void savesEditedTextRoundTrip();
    void savesVietnameseEditWithEmbeddedArial_data();
    void savesVietnameseEditWithEmbeddedArial();
    void savesVietnameseEditWithEmbeddedSystemFont_data();
    void savesVietnameseEditWithEmbeddedSystemFont();
    void preservesInheritedSystemFontForVietnamese();
    void rejectsUnsupportedSystemFontForVietnamese_data();
    void rejectsUnsupportedSystemFontForVietnamese();
    void preservesEmbeddedFontForFormatOnlyVietnameseEdit();
    void savesNormalizedVietnameseRichText();
    void savesCurrentFileRoundTrip();
    void savesUnchangedCopyRoundTrip();
    void savesMultipleDraftsOnSamePage();
    void savesExpandedFrameBeforeRightColumn();
    void savesDeletedTextRoundTrip();
    void savesAddedTextRoundTrip();
    void rendersAddedImageSnapshot();
    void addsImageWithUndoRedo();
    void savesAddedImageRoundTrip();
    void preservesAddedTextLayerOrder();
    void savesRichMultilineTextRoundTrip();
    void savesOffsetCropCoordinates();
    void savesAddedTextOnRotatedPage();
    void preservesStrokeAppearanceAndRejectsClipping();
    void savesAsynchronously();
    void keepsDraftsWhenSaveFails();
    void opensDocumentsInIndependentTabs();
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
    QVERIFY(containsObject(m_controller.candidatesForPage(0)));
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
    QVERIFY(!containsObject(m_controller.candidatesForPage(0)));
    QVERIFY(!m_controller.selectCandidate(0, objectId));
    QVERIFY(m_controller.selectedObjectId().isEmpty());

    m_controller.undo();
    QCOMPARE(
        m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(containsObject(m_controller.candidatesForPage(0)));
    QVERIFY(m_controller.selectCandidate(0, objectId));

    m_controller.redo();
    QVERIFY(m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
    QVERIFY(!containsObject(m_controller.candidatesForPage(0)));
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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
            controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
                controller.candidatesForPage(0),
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
            controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
        controller.candidatesForPage(0),
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
    const QVariantList candidates = controller.candidatesForPage(0);
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
    const QVariantList afterUndo = controller.candidatesForPage(0);
    QVERIFY(std::none_of(
        afterUndo.cbegin(),
        afterUndo.cend(),
        [&objectId](const QVariant &value) {
            return value.toMap().value(QStringLiteral("objectId")).toString()
                == objectId;
        }));

    controller.redo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    const QVariantList afterRedo = controller.candidatesForPage(0);
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

    const QVariantList savedCandidates = controller.candidatesForPage(0);
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
    const QString expectedText = QStringLiteral("Lưu tiếng Việt");
    const QString editedText = expectedText.normalized(
        QString::NormalizationForm_D);
    controller.commitSelectedText(editedText);

    QSignalSpy saveFinishedSpy(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(controller.saving());
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
        firstController->candidatesForPage(0),
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
            controller->candidatesForPage(0),
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
