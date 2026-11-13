#include "PDFEditControllerTestSupport.h"
#include "PDFUnicodeFont.h"

namespace PDFEditControllerTestSupport {

namespace {
quint16 sfntUInt16(const QByteArray &data, qsizetype offset)
{
    return (quint16(quint8(data.at(offset))) << 8)
        | quint8(data.at(offset + 1));
}

quint32 sfntUInt32(const QByteArray &data, qsizetype offset)
{
    return (quint32(sfntUInt16(data, offset)) << 16)
        | sfntUInt16(data, offset + 2);
}

void setSfntUInt32(QByteArray &data, qsizetype offset, quint32 value)
{
    for (int byte = 0; byte < 4; ++byte)
        data[offset + byte] = char(value >> (24 - byte * 8));
}

quint32 sfntChecksum(const QByteArray &data, qsizetype offset, qsizetype length)
{
    quint32 sum = 0;
    for (qsizetype start = 0; start < length; start += 4) {
        quint32 word = 0;
        for (int byte = 0; byte < 4; ++byte) {
            word <<= 8;
            if (start + byte < length)
                word |= quint8(data.at(offset + start + byte));
        }
        sum += word;
    }
    return sum;
}
} // namespace

QByteArray renamedTestFontData(const QString &family, bool bold, bool italic)
{
    const QString style = bold
        ? (italic ? QStringLiteral("BoldItalic") : QStringLiteral("Bold"))
        : (italic ? QStringLiteral("Italic") : QStringLiteral("Regular"));
    QFile file(QStringLiteral(":/trepdf/tests/fonts/liberation/LiberationSans-%1.ttf").arg(style));
    const QByteArray source = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    if (source.size() < 12 || family.isEmpty())
        return {};
    const auto setUInt16 = [](QByteArray &bytes, qsizetype offset, quint16 value) {
        bytes[offset] = char(value >> 8);
        bytes[offset + 1] = char(value);
    };
    const auto utf16be = [](const QString &text) {
        QByteArray bytes;
        for (QChar character : text) {
            bytes.append(char(character.unicode() >> 8));
            bytes.append(char(character.unicode()));
        }
        return bytes;
    };
    const int tableCount = sfntUInt16(source, 4);
    if (12 + tableCount * 16 > source.size())
        return {};
    QByteArray result = source.left(12 + tableCount * 16);
    qsizetype headOffset = -1;
    bool renamed = false;
    for (int table = 0; table < tableCount; ++table) {
        const qsizetype entry = 12 + table * 16;
        const qsizetype offset = sfntUInt32(source, entry + 8);
        const qsizetype length = sfntUInt32(source, entry + 12);
        if (offset > source.size() || length > source.size() - offset)
            return {};
        QByteArray data = source.mid(offset, length);
        const QByteArray tag = source.mid(entry, 4);
        if (tag == QByteArrayLiteral("name")) {
            if (data.size() < 6 || sfntUInt16(data, 0) != 0)
                return {};
            const int count = sfntUInt16(data, 2);
            const qsizetype storage = sfntUInt16(data, 4);
            if (6 + count * 12 > data.size() || storage > data.size())
                return {};
            QByteArray names = data.left(6 + count * 12);
            setUInt16(names, 4, quint16(names.size()));
            QByteArray strings;
            for (int index = 0; index < count; ++index) {
                const qsizetype record = 6 + index * 12;
                const qsizetype stringOffset = storage + sfntUInt16(data, record + 10);
                const qsizetype stringLength = sfntUInt16(data, record + 8);
                if (stringOffset > data.size() || stringLength > data.size() - stringOffset)
                    return {};
                QByteArray name = data.mid(stringOffset, stringLength);
                QString replacement = family;
                if (sfntUInt16(data, record + 6) == 6)
                    replacement.remove(QLatin1Char(' '));
                const QByteArray original = name;
                const QString sourceFamily = QStringLiteral("Liberation Sans");
                QString sourcePostScriptFamily = sourceFamily;
                sourcePostScriptFamily.remove(QLatin1Char(' '));
                QString replacementPostScriptFamily = family;
                replacementPostScriptFamily.remove(QLatin1Char(' '));
                // Rebuild the string storage because family names and aliases
                // need not have the same length. Preserve each face's suffix.
                name.replace(sourceFamily.toLatin1(), replacement.toLatin1());
                name.replace(utf16be(sourceFamily), utf16be(replacement));
                name.replace(sourcePostScriptFamily.toLatin1(), replacementPostScriptFamily.toLatin1());
                name.replace(utf16be(sourcePostScriptFamily), utf16be(replacementPostScriptFamily));
                renamed |= name != original;
                if (strings.size() + name.size() > 65535)
                    return {};
                setUInt16(names, record + 8, quint16(name.size()));
                setUInt16(names, record + 10, quint16(strings.size()));
                strings.append(name);
            }
            data = names + strings;
        }
        while (result.size() % 4)
            result.append('\0');
        if (tag == QByteArrayLiteral("head")) {
            if (data.size() < 12)
                return {};
            headOffset = result.size();
            setSfntUInt32(data, 8, 0);
        }
        setSfntUInt32(result, entry + 4, sfntChecksum(data, 0, data.size()));
        setSfntUInt32(result, entry + 8, quint32(result.size()));
        setSfntUInt32(result, entry + 12, quint32(data.size()));
        result.append(data);
    }
    if (!renamed || headOffset < 0)
        return {};
    setSfntUInt32(result, headOffset + 8,
                  quint32(0xb1b0afba) - sfntChecksum(result, 0, result.size()));
    return result;
}

QString testFontFamily()
{
    return QStringLiteral("TrePDFTestSans");
}

QByteArray testFontData(bool bold, bool italic)
{
    // A private family prevents system-installed Liberation Sans versions from
    // replacing the pinned fixture during layout or Save. Originals stay intact.
    static const std::array<QByteArray, 4> faces{
        renamedTestFontData(testFontFamily(), false, false),
        renamedTestFontData(testFontFamily(), false, true),
        renamedTestFontData(testFontFamily(), true, false),
        renamedTestFontData(testFontFamily(), true, true)};
    return faces[(bold ? 2 : 0) + (italic ? 1 : 0)];
}

bool installTestFonts(QString &error)
{
    QList<ApplicationFonts::DownloadedFontData> faces;
    for (int style = 0; style < 4; ++style) {
        const bool bold = style & 2;
        const bool italic = style & 1;
        faces.append({testFontData(bold, italic), testFontFamily(), bold, italic, uint('a')});
    }
    return ApplicationFonts::installDownloadedFonts(faces, error);
}

bool createFontSubsetCollisionSourcePdf(const QString &filePath)
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
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                "/Encoding /WinAnsiEncoding >>"),
            contentStream,
        });
}

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

bool createGrayscaleFillPdf(const QString &filePath,
                            qreal gray,
                            bool namedColorSpace,
                            bool splitContents,
                            bool iccBased)
{
    const QByteArray iccProfile = iccBased
        ? QColorSpace(QPointF(0.3127, 0.3290), QColorSpace::TransferFunction::SRgb)
              .iccProfile()
        : QByteArray();
    if (iccBased && (!namedColorSpace || iccProfile.isEmpty()))
        return false;
    const QByteArray colorSpace = iccBased
        ? QByteArray("[/ICCBased ") + QByteArray::number(splitContents ? 7 : 6)
              + QByteArray(" 0 R]")
        : QByteArray("/DeviceGray");
    const QByteArray paint = (namedColorSpace ? QByteArray("/CS0 cs\n") : QByteArray())
        + QByteArray::number(gray, 'f', 3)
        + (namedColorSpace ? QByteArray(" scn\n") : QByteArray(" g\n"));
    const QByteArray pageContent(
        "/P <</MCID 0>> BDC\n"
        "20 20 160 80 re f*\n"
        "EMC\n"
        "0 g\n"
        "BT\n"
        "/F1 20 Tf\n"
        "20 160 Td\n"
        "(Grayscale fill) Tj\n"
        "ET\n");
    const auto contentStream = [](const QByteArray &content) {
        return QByteArray("<< /Length ")
            + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n")
            + content
            + QByteArray("endstream");
    };

    QVector<QByteArray> objects{
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /F1 4 0 R >> "
                "/ColorSpace << /CS0 ") + colorSpace
                + QByteArray(" >> >> /Contents ")
                + (splitContents ? QByteArray("[5 0 R 6 0 R]") : QByteArray("5 0 R"))
                + QByteArray(" >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
            contentStream(splitContents ? paint : paint + pageContent),
    };
    if (splitContents) {
        // Graphics state, including the named gray color, crosses stream boundaries.
        objects.append(contentStream(pageContent));
    }
    if (iccBased) {
        objects.append(QByteArray("<< /N 1 /Alternate /DeviceGray /Length ")
            + QByteArray::number(iccProfile.size())
            + QByteArray(" >>\nstream\n") + iccProfile + QByteArray("\nendstream"));
    }
    return writePdfObjects(filePath, objects);
}

bool createColoredFillPdf(const QString &filePath,
                          const QColor &color,
                          SolidFillColorSpace colorSpace,
                          bool splitContents)
{
    if (!color.isValid() || color.alpha() != 255)
        return false;
    const bool iccBased = colorSpace == SolidFillColorSpace::IccRgb;
    const bool named = colorSpace == SolidFillColorSpace::NamedRgb || iccBased;
    const QByteArray iccProfile = iccBased
        ? QColorSpace(QColorSpace::SRgb).iccProfile() : QByteArray();
    if (iccBased && iccProfile.isEmpty())
        return false;
    const QByteArray space = iccBased
        ? QByteArray("[/ICCBased ") + QByteArray::number(splitContents ? 7 : 6) + " 0 R]"
        : QByteArray("/DeviceRGB");
    const auto number = [](qreal value) { return QByteArray::number(value, 'f', 6); };
    QByteArray paint;
    if (colorSpace == SolidFillColorSpace::DeviceCmyk) {
        const QColor cmyk = color.toCmyk();
        paint = number(cmyk.cyanF()) + ' ' + number(cmyk.magentaF()) + ' '
            + number(cmyk.yellowF()) + ' ' + number(cmyk.blackF()) + " k\n";
    } else {
        paint = (named ? QByteArray("/CS0 cs\n") : QByteArray())
            + number(color.redF()) + ' ' + number(color.greenF()) + ' '
            + number(color.blueF()) + (named ? " scn\n" : " rg\n");
    }
    const QByteArray pageContent(
        "/P <</MCID 0>> BDC\n"
        "20 20 160 80 re f*\n"
        "EMC\n"
        "0 g\n"
        "BT /F1 20 Tf 20 160 Td (Colored fill) Tj ET\n");
    const auto stream = [](const QByteArray &content) {
        return QByteArray("<< /Length ") + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "\nendstream";
    };
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
            "/Resources << /Font << /F1 4 0 R >> /ColorSpace << /CS0 ")
            + space + " >> >> /Contents "
            + (splitContents ? "[5 0 R 6 0 R]" : "5 0 R") + " >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        stream(splitContents ? paint : paint + pageContent)
    };
    if (splitContents)
        objects.append(stream(pageContent));
    if (iccBased) {
        objects.append(QByteArray("<< /N 3 /Alternate /DeviceRGB /Length ")
            + QByteArray::number(iccProfile.size()) + " >>\nstream\n"
            + iccProfile + "\nendstream");
    }
    return writePdfObjects(filePath, objects);
}

bool createIccBasedTextPdf(const QString &filePath)
{
    const QByteArray iccProfile = QColorSpace(QColorSpace::SRgb).iccProfile();
    if (iccProfile.isEmpty()) {
        return false;
    }

    const QByteArray pageContent(
        "/CS1 cs\n"
        "0 0.435294 0.752941 scn\n"
        "BT\n"
        "/F1 20 Tf\n"
        "20 150 Td\n"
        "(Untouched ICC blue) Tj\n"
        "ET\n"
        "0 g\n"
        "BT\n"
        "/F1 20 Tf\n"
        "20 80 Td\n"
        "(Editable sibling) Tj\n"
        "ET\n");
    const auto contentStream = [](const QByteArray &dictionary,
                                  const QByteArray &content) {
        return dictionary + " /Length " + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "\nendstream";
    };

    return writePdfObjects(
        filePath,
        {
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                "/Resources << /Font << /F1 4 0 R >> "
                "/ColorSpace << /CS1 [/ICCBased 6 0 R] >> >> "
                "/Contents 5 0 R >>"),
            QByteArray(
                "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
            contentStream("<<", pageContent),
            contentStream(
                "<< /N 3 /Alternate /DeviceRGB",
                iccProfile),
        });
}

bool addTextObject(FPDF_DOCUMENT document,
                   FPDF_PAGE page,
                   const char *fontName,
                   const QString &text,
                   double x,
                   double y,
                   double horizontalScale,
                   float fontSize)
{
    FPDF_PAGEOBJECT textObject = FPDFPageObj_NewTextObj(document, fontName, fontSize);
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
                           double y,
                           const QVector<char32_t> &characters)
{
    FPDF_PAGEOBJECT textObject = FPDFPageObj_CreateTextObj(
        document,
        font,
        24.0F);
    if (!textObject) {
        return false;
    }
    const auto *wideText = reinterpret_cast<const FPDF_WCHAR *>(text.utf16());
    if (!(characters.isEmpty() ? FPDFText_SetText(textObject, wideText)
                              : PDFUnicodeFont::setText(textObject, characters, text))) {
        FPDFPageObj_Destroy(textObject);
        return false;
    }
    FPDFPageObj_Transform(textObject, 1.0, 0.0, 0.0, 1.0, x, y);
    return FPDFPage_InsertObject(page, textObject);
}

QVector<char32_t> sortedUniqueFontCharacters(const QString &text)
{
    auto characters = text.toUcs4();
    std::sort(characters.begin(), characters.end());
    characters.erase(std::unique(characters.begin(), characters.end()), characters.end());
    return QVector<char32_t>(characters.cbegin(), characters.cend());
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
                         260.0,
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
                         300.0,
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

QVector<SourceParagraphTextPart> mixedSourceParagraphParts()
{
    // Match the mixed face, small stretch differences, and baseline gaps on
    // page 5 of the reported article. The 19-point gap starts a new paragraph.
    return {
        {QStringLiteral("a combination of economic and cultural activities that take place at night, mainly"),
         72.0, 612.34, "Helvetica-Bold", 1.008, 12.0F},
        {QStringLiteral("entertainment experiences (excluding the prostitution industry). In the US, the night-time"),
         72.0, 598.53, "Helvetica", 1.008, 12.0F},
        {QStringLiteral("economy includes 5 main areas including: Arts, bars, culinary services, sports and"),
         72.0, 584.53, "Helvetica", 1.008, 12.0F},
        {QStringLiteral("entertainment. Most European countries define the night-time economy in a narrow sense"),
         72.0, 570.53, "Helvetica", 1.008, 12.0F},
        {QStringLiteral("and define the night-time economic operating time frame from 6:00 p.m. the previous day"),
         72.0, 556.53, "Helvetica", 1.008, 12.0F},
        {QStringLiteral("to 6:00 a.m. the next morning (Field 2008; Le et al., 2022)."),
         71.53, 543.75, "Helvetica", 1.0008, 12.0F},
        {QStringLiteral("The concept of Night Economy is service activities that take place after 6:00 pm the"),
         72.0, 524.73, "Helvetica", 1.0, 12.0F},
        {QStringLiteral("previous night until 6:00 am the next morning, including: Shopping at night markets,"),
         72.0, 510.73, "Helvetica", 1.0, 12.0F}};
}

QString sourceParagraphText(const QVector<SourceParagraphTextPart> &parts,
                            qsizetype start,
                            qsizetype count)
{
    QStringList lines;
    for (qsizetype index = start; index < start + count; ++index) {
        lines.append(parts.at(index).text);
    }
    return lines.join(QLatin1Char('\n'));
}

bool createSourceParagraphPdf(
    const QString &filePath,
    const QVector<SourceParagraphTextPart> &parts)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }
    FPDF_PAGE page = FPDFPage_New(document, 0, 595.0, 842.0);
    bool succeeded = page != nullptr;
    for (const SourceParagraphTextPart &part : parts) {
        if (!succeeded) {
            break;
        }
        succeeded = addTextObject(
            document, page, part.fontName, part.text, part.x, part.y,
            part.horizontalScale, part.fontSize);
    }
    succeeded = succeeded && FPDFPage_GenerateContent(page);
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

bool createSourceSpacingPdf(const QString &filePath,
                            qreal letterSpacing,
                            qreal wordSpacing,
                            qreal textMatrixScale,
                            bool irregularAdjustments)
{
    const auto number = [](qreal value) { return QByteArray::number(value, 'f', 8); };
    const QByteArray content = QByteArray("BT /F1 ") + number(12.0 / textMatrixScale)
        + " Tf " + number(letterSpacing / textMatrixScale) + " Tc "
        + number(wordSpacing / textMatrixScale) + " Tw "
        + number(textMatrixScale) + " 0 0 " + number(textMatrixScale)
        + " 72 740 Tm "
        + (irregularAdjustments
               ? QByteArray("[(Al) -180 (pha beta) 130 ( gamma del) -320 (ta)] TJ")
               : QByteArray("(Alpha beta gamma delta) Tj"))
        + " ET\n";
    return writePdfObjects(filePath, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        QByteArray("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                   "/Encoding /WinAnsiEncoding >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream"});
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

bool createStretchedTextPdf(const QString &filePath,
                            qreal horizontalScale)
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

bool createEmbeddedTestFontPdf(const QString &filePath)
{
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }

    const QByteArray fontData = testFontData();
    const auto characters = sortedUniqueFontCharacters(
        QStringLiteral("Tiếng Việt gốcKhông đổi"));
    const QRawFont rawFont(fontData, 16.0, QFont::PreferNoHinting);
    FPDF_FONT font = PDFUnicodeFont::loadTrueType(document, fontData, rawFont, characters);
    FPDF_PAGE page = font ? FPDFPage_New(document, 0, 595.0, 842.0) : nullptr;
    bool succeeded = page
        && addTextObjectWithFont(
            document,
            page,
            font,
            QStringLiteral("Tiếng Việt gốc"),
            72.0,
            740.0,
            characters)
        && addTextObjectWithFont(
            document,
            page,
            font,
            QStringLiteral("Không đổi"),
            72.0,
            680.0,
            characters)
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

    const auto characters = sortedUniqueFontCharacters(text);
    const QRawFont rawFont(fontData, 16.0, QFont::PreferNoHinting);
    FPDF_FONT font = PDFUnicodeFont::loadTrueType(document, fontData, rawFont, characters);
    FPDF_PAGE page = font ? FPDFPage_New(document, 0, 595.0, 842.0) : nullptr;
    bool succeeded = page
        && addTextObjectWithFont(document, page, font, text, 72.0, 740.0, characters)
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

bool createGroupedSubsetFontPdf(const QString &filePath)
{
    const QByteArray fontData = testFontData();
    if (fontData.isEmpty()) {
        return false;
    }

    PDFiumProcessLock pdfiumLock;
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document) {
        return false;
    }
    const auto characters = sortedUniqueFontCharacters(QStringLiteral("Helloworld"));
    const QRawFont rawFont(fontData, 16.0, QFont::PreferNoHinting);
    FPDF_FONT font = PDFUnicodeFont::loadTrueType(document, fontData, rawFont, characters);
    FPDF_PAGE page = font ? FPDFPage_New(document, 0, 595.0, 842.0) : nullptr;
    // The visible word gap comes entirely from object positions. Neither
    // source object encodes a space, so the subset has no space mapping.
    bool succeeded = page
        && addTextObjectWithFont(
            document, page, font, QStringLiteral("Hello"), 72.0, 740.0, characters)
        && addTextObjectWithFont(
            document, page, font, QStringLiteral("world"), 140.0, 740.0, characters)
        && FPDFPage_GenerateContent(page);
    if (page) {
        FPDF_ClosePage(page);
    }
    if (succeeded) {
        PdfFileWriter writer(filePath);
        succeeded = writer.open()
            && FPDF_SaveAsCopy(
                document,
                &writer,
                FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS);
    }
    if (font) {
        FPDFFont_Close(font);
    }
    FPDF_CloseDocument(document);
    return succeeded;
}

QRectF normalizedGlyphBoundsForText(const QString &filePath,
                                    const QString &text,
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

QString pageText(const QString &filePath, int pageIndex)
{
    // Save/reopen can start an asynchronous candidate scan immediately. Text
    // assertions must serialize with that worker just like the application.
    std::unique_ptr<PDFiumProcessLock> lock;
    if (!pdfiumProcessLockOwnedByCurrentThread())
        lock = std::make_unique<PDFiumProcessLock>();
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

QString normalizedPageText(const QString &filePath, int pageIndex)
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

TextPaintInspection inspectTopLevelTextPaint(const QString &filePath,
                                             const QString &targetText)
{
    PDFiumProcessLock pdfiumLock;
    TextPaintInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        encodedPath.constData(),
        nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
    inspection.readable = document && page && textPage;
    if (inspection.readable) {
        const int objectCount = FPDFPage_CountObjects(page);
        for (int index = 0; index < objectCount; ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (!object) {
                continue;
            }
            const int objectType = FPDFPageObj_GetType(object);
            if (objectType != FPDF_PAGEOBJ_TEXT
                || !pageObjectText(object, textPage).contains(targetText)) {
                continue;
            }

            inspection.found = true;
            inspection.objectType = objectType;
            inspection.renderMode = FPDFTextObj_GetTextRenderMode(object);
            unsigned int red = 0;
            unsigned int green = 0;
            unsigned int blue = 0;
            unsigned int alpha = 0;
            inspection.hasFillColor = FPDFPageObj_GetFillColor(
                object,
                &red,
                &green,
                &blue,
                &alpha);
            if (inspection.hasFillColor) {
                inspection.fillColor = QColor::fromRgb(
                    red,
                    green,
                    blue,
                    alpha);
            }
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
    return inspection;
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

static TextObjectFontInfo readTextObjectFontInfo(FPDF_PAGEOBJECT object)
{
    TextObjectFontInfo info;
    const FPDF_FONT font = FPDFTextObj_GetFont(object);
    if (!font)
        return info;
    info.found = true;
    info.baseName = baseFontName(object);
    info.familyName = fontFamilyName(font);
    info.embedded = FPDFFont_GetIsEmbedded(font);
    FPDFFont_GetFontData(font, nullptr, 0, &info.dataSize);
    if (info.embedded == 1) {
        if (info.dataSize == 0 || info.dataSize > 64 * 1024 * 1024)
            return info;
        QByteArray data(qsizetype(info.dataSize), Qt::Uninitialized);
        size_t actual = 0;
        if (!FPDFFont_GetFontData(font, reinterpret_cast<uint8_t *>(data.data()),
                                  info.dataSize, &actual) || actual != info.dataSize)
            return info;
        const QRawFont rawFont(data, 16.0, QFont::PreferNoHinting);
        if (rawFont.isValid()) {
            // PDFium may report a default descriptor weight even when the
            // embedded program's OS/2 table identifies the correct bold face.
            info.weight = rawFont.weight();
            info.italic = rawFont.style() != QFont::StyleNormal;
        }
        return info;
    }
    info.weight = FPDFFont_GetWeight(font);
    if (info.baseName.contains(QStringLiteral("Bold"), Qt::CaseInsensitive))
        info.weight = qMax(info.weight, 700);
    int angle = 0;
    const int flags = FPDFFont_GetFlags(font);
    info.italic = (FPDFFont_GetItalicAngle(font, &angle) && angle != 0)
        || (flags >= 0 && (flags & (1 << 6)))
        || info.baseName.contains(QStringLiteral("Italic"), Qt::CaseInsensitive)
        || info.baseName.contains(QStringLiteral("Oblique"), Qt::CaseInsensitive);
    return info;
}

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

            info = readTextObjectFontInfo(object);
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

int topLevelPageObjectCount(const QString &filePath, int pageIndex)
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

bool createTextUnderlineSourcePdf(const QString &filePath,
                                 const QByteArray &underlineCommands)
{
    const QByteArray content = QByteArray(
        "BT /F1 24 Tf 1 0 0 1 72 640 Tm (Hello world) Tj ET\n")
        + underlineCommands
        + QByteArray("q 0 0 0 RG 1 w 72 740 m 300 740 l S Q\n");
    return writePdfObjects(filePath, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        QByteArray("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n") + content + QByteArray("endstream"),
    });
}

bool createSavedTextUnderlinePdf(const QString &filePath, bool legacy)
{
    if (legacy) {
        // Previous TrePDF versions emitted an unmarked path immediately after
        // the text. Helvetica's advance for "Hello world" at 24 pt is 118.68 pt.
        return createTextUnderlineSourcePdf(filePath, QByteArray(
            "q 0 0 0 RG 1.2 w 72 637.6 m 190.68 637.6 l S Q\n"));
    }

    const QString sourcePath = filePath + QStringLiteral(".source.pdf");
    if (!createTextUnderlineSourcePdf(sourcePath)) {
        return false;
    }
    PDFDocument document;
    PDFEditController controller;
    if (!document.open(QUrl::fromLocalFile(sourcePath))) {
        return false;
    }
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Hello world"));
    if (!controller.selectCandidate(
            0, candidate.value(QStringLiteral("objectId")).toString())) {
        return false;
    }
    controller.beginSelectedTextEdit();
    controller.setSelectedTextRange(0, 11);
    controller.setSelectedFontUnderline(true);
    controller.finishSelectedTextEdit();
    return controller.saveAs(QUrl::fromLocalFile(filePath));
}

TextUnderlineInspection inspectTextUnderlinePaths(const QString &filePath)
{
    PDFiumProcessLock pdfiumLock;
    TextUnderlineInspection inspection;
    const QByteArray encodedPath = QFile::encodeName(filePath);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, 0) : nullptr;
    inspection.readable = page != nullptr;
    if (page) {
        for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            const int type = FPDFPageObj_GetType(object);
            if (type == FPDF_PAGEOBJ_TEXT) {
                inspection.textIndex = index;
                ++inspection.textCount;
                FS_MATRIX matrix{};
                if (FPDFPageObj_GetMatrix(object, &matrix)) {
                    inspection.textOrigin = QPointF(matrix.e, matrix.f);
                }
                continue;
            }
            if (type != FPDF_PAGEOBJ_PATH
                || FPDFPath_CountSegments(object) != 2) {
                continue;
            }
            FS_MATRIX matrix{};
            float x1 = 0.0F;
            float y1 = 0.0F;
            float x2 = 0.0F;
            float y2 = 0.0F;
            if (!FPDFPageObj_GetMatrix(object, &matrix)
                || !FPDFPathSegment_GetPoint(
                    FPDFPath_GetPathSegment(object, 0), &x1, &y1)
                || !FPDFPathSegment_GetPoint(
                    FPDFPath_GetPathSegment(object, 1), &x2, &y2)) {
                inspection.readable = false;
                break;
            }
            const auto transform = [&matrix](qreal x, qreal y) {
                return QPointF(matrix.a * x + matrix.c * y + matrix.e,
                               matrix.b * x + matrix.d * y + matrix.f);
            };
            const QLineF line(transform(x1, y1), transform(x2, y2));
            if (qAbs(line.y1() - 740.0) < 0.01) {
                inspection.unrelatedLines.append(line);
            } else {
                inspection.underlineIndexes.append(index);
                inspection.underlines.append(line);
            }
        }
        FPDF_ClosePage(page);
    }
    if (document) {
        FPDF_CloseDocument(document);
    }
    return inspection;
}

void addTextUnderlineFixtureRows()
{
    QTest::addColumn<bool>("legacy");
    QTest::newRow("saved-marked-underline") << false;
    QTest::newRow("legacy-unmarked-underline") << true;
}

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
        for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
            if (FPDFPageObj_GetType(FPDFPage_GetObject(page, index)) == FPDF_PAGEOBJ_PATH)
                inspection.underlinePath = true;
        }
        QString text;
        for (int index = 0; index < FPDFText_CountChars(textPage); ++index)
            text.append(QChar(static_cast<char16_t>(FPDFText_GetUnicode(textPage, index))));
        const auto inspectWord = [&](const QString &word, qreal &baseline, bool checkRed) {
            const int start = text.indexOf(word);
            if (start < 0)
                return false;
            bool redRun = true;
            bool boldItalic = true;
            for (int index = start; index < start + word.size(); ++index) {
                const auto object = FPDFText_GetTextObject(textPage, index);
                FS_MATRIX matrix{};
                if (!object || !FPDFPageObj_GetMatrix(object, &matrix))
                    return false;
                if (index == start)
                    baseline = matrix.f;
                else if (qAbs(baseline - matrix.f) >= 0.5)
                    return false;
                if (checkRed) {
                    unsigned int r = 0, g = 0, b = 0, a = 0;
                    redRun = redRun && FPDFPageObj_GetFillColor(object, &r, &g, &b, &a)
                        && r == 0xc6 && g == 0x28 && b == 0x28 && a == 0xff;
                    const auto font = readTextObjectFontInfo(object);
                    boldItalic = boldItalic && font.weight >= 700 && font.italic;
                }
            }
            if (checkRed) {
                inspection.redRun = redRun;
                inspection.boldItalicRun = boldItalic;
            }
            return true;
        };
        foundRedBaseline = inspectWord(QStringLiteral("Red"), redBaseline, true);
        foundBlueBaseline = inspectWord(QStringLiteral("blue"), blueBaseline, false);
        foundGreenBaseline = inspectWord(QStringLiteral("Green"), greenBaseline, false);
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

} // namespace PDFEditControllerTestSupport
