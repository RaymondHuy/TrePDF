#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <fpdf_ppo.h>
#include <QGlyphRun>
#include <QPainterPath>
#include "FontDownloadService.h"
#include "CtanFontProvider.h"
#include "GoogleFontsProvider.h"
#include "PDFFontAliases.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QFontInfo>
#include <QSemaphore>
#include <QThreadPool>
#include <zip.h>

using namespace PDFEditControllerTestSupport;

namespace {

bool textRangesShareFont(const QString &path, const QStringList &texts)
{
    PDFiumProcessLock lock;
    FPDF_DOCUMENT document = FPDF_LoadDocument(QFile::encodeName(path).constData(), nullptr);
    if (!document)
        return false;
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page)
        return false;
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage)
        return false;
    const auto closeTextPage = qScopeGuard([&] { FPDFText_ClosePage(textPage); });
    FPDF_FONT sharedFont = nullptr;
    for (const QString &text : texts) {
        FPDF_SCHHANDLE search = FPDFText_FindStart(textPage,
            reinterpret_cast<FPDF_WIDESTRING>(text.utf16()), FPDF_MATCHCASE, 0);
        if (!search)
            return false;
        const auto closeSearch = qScopeGuard([&] { FPDFText_FindClose(search); });
        if (!FPDFText_FindNext(search))
            return false;
        const int start = FPDFText_GetSchResultIndex(search);
        const int count = FPDFText_GetSchCount(search);
        QString recovered;
        for (int index = start; index < start + count; ++index) {
            const auto object = FPDFText_GetTextObject(textPage, index);
            const auto font = object ? FPDFTextObj_GetFont(object) : nullptr;
            if (!font || (sharedFont && sharedFont != font))
                return false;
            sharedFont = font;
            const char32_t unicode = FPDFText_GetUnicode(textPage, index);
            recovered.append(QString::fromUcs4(&unicode, 1));
        }
        if (recovered != text)
            return false;
    }
    return sharedFont != nullptr;
}

struct FontResponse {
    QByteArray body;
    int status = 200;
    QUrl redirect;
    qint64 contentLength = -1;
    int delayMs = 0;
    QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
};

class FontReply final : public QNetworkReply
{
public:
    FontReply(const QNetworkRequest &request, const FontResponse &response, QObject *parent)
        : QNetworkReply(parent), m_body(response.body)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        if (!response.redirect.isEmpty())
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, response.redirect);
        setHeader(QNetworkRequest::ContentLengthHeader,
                  response.contentLength >= 0 ? response.contentLength : m_body.size());
        open(QIODevice::ReadOnly);
        QTimer::singleShot(response.delayMs, this, [this, error = response.networkError]() {
            if (m_aborted)
                return;
            if (error != QNetworkReply::NoError)
                setError(error, QStringLiteral("Simulated transport error"));
            emit readyRead();
            if (!m_aborted) {
                setFinished(true);
                emit finished();
            }
        });
    }
    void abort() override { m_aborted = true; }
    qint64 bytesAvailable() const override
    {
        return m_body.size() - m_position + QNetworkReply::bytesAvailable();
    }
protected:
    qint64 readData(char *destination, qint64 maximum) override
    {
        const qint64 count = qMin(maximum, m_body.size() - m_position);
        if (!count)
            return -1;
        memcpy(destination, m_body.constData() + m_position, size_t(count));
        m_position += count;
        return count;
    }
private:
    QByteArray m_body;
    qint64 m_position = 0;
    bool m_aborted = false;
};

class FontNetwork final : public QNetworkAccessManager
{
public:
    QList<FontResponse> responses;
    QList<QNetworkRequest> requests;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        requests.append(request);
        return new FontReply(request, responses.isEmpty()
            ? FontResponse{{}, 404, {}, -1} : responses.takeFirst(), this);
    }
};

QByteArray fontMetadata(const QString &family, bool allStyles = false)
{
    return QJsonDocument(QJsonArray{QJsonObject{
        {QStringLiteral("family"), family}, {QStringLiteral("type"), QStringLiteral("google")},
        {QStringLiteral("weights"), allStyles ? QJsonArray{400, 700} : QJsonArray{400}},
        {QStringLiteral("styles"), allStyles
            ? QJsonArray{QStringLiteral("normal"), QStringLiteral("italic")}
            : QJsonArray{QStringLiteral("normal")}},
        {QStringLiteral("subsets"), QJsonArray{QStringLiteral("latin"), QStringLiteral("vietnamese")}}
    }}).toJson(QJsonDocument::Compact);
}

const QByteArray fullFontStylesheet = QByteArrayLiteral(
    "@font-face {src: url(https://fonts.gstatic.com/s/test/v1/complete.ttf) format('truetype');}");

QByteArray ctanTermesFontData(int style)
{
    static const char *const paths[] = {
        "fixtures/fonts/tex-gyre/texgyretermes-regular.otf",
        "fixtures/fonts/tex-gyre/texgyretermes-italic.otf",
        "fixtures/fonts/tex-gyre/texgyretermes-bold.otf",
        "fixtures/fonts/tex-gyre/texgyretermes-bolditalic.otf"
    };
    QFile file(QFINDTESTDATA(paths[style]));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Give each test its own standard application-data directory without touching
// the user's real TrePDF font store or changing the process home directory.
class IsolatedFontStore final
{
public:
    IsolatedFontStore()
        : m_name(QCoreApplication::applicationName()), m_testMode(QStandardPaths::isTestModeEnabled())
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("TrePDF-font-test-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces));
        path = ApplicationFonts::downloadedFontDirectory();
        fresh = QFileInfo(QFileInfo(path).absolutePath()).fileName()
            == QCoreApplication::applicationName()
            && !QFileInfo::exists(QFileInfo(path).absolutePath());
    }
    ~IsolatedFontStore()
    {
        if (fresh)
            QDir(QFileInfo(path).absolutePath()).removeRecursively();
        QCoreApplication::setApplicationName(m_name);
        QStandardPaths::setTestModeEnabled(m_testMode);
    }
    QString path;
    bool fresh = false;
private:
    QString m_name;
    bool m_testMode;
};

const QString kUninstalledSubsetFamily = QStringLiteral("QrZxy");

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

QByteArray uninstalledSubsetFontData(const QByteArray &alias = QByteArrayLiteral("QrZxy"),
                                   bool bold = false, bool italic = false)
{
    if (alias.size() != 5)
        return {};
    QByteArray wideAlias;
    for (char character : alias) {
        wideAlias.append('\0');
        wideAlias.append(character);
    }
    QByteArray data = ApplicationFonts::bundledFontData(
        QStringLiteral("Arial"), bold, italic);
    if (data.size() < 12)
        return {};
    const int tableCount = sfntUInt16(data, 4);
    if (12 + tableCount * 16 > data.size())
        return {};
    qsizetype headOffset = -1;
    bool renamed = false;
    for (int table = 0; table < tableCount; ++table) {
        const qsizetype entry = 12 + table * 16;
        const qsizetype offset = sfntUInt32(data, entry + 8);
        const qsizetype length = sfntUInt32(data, entry + 12);
        if (offset > data.size() || length > data.size() - offset)
            return {};
        if (data.mid(entry, 4) == QByteArrayLiteral("head"))
            headOffset = offset;
        if (data.mid(entry, 4) != QByteArrayLiteral("name"))
            continue;
        QByteArray names = data.mid(offset, length);
        // Equal-length replacements keep every name-record offset intact.
        names.replace(QByteArrayLiteral("Arial"), alias);
        names.replace(QByteArray("\0A\0r\0i\0a\0l", 10),
                      wideAlias);
        renamed = names != data.mid(offset, length);
        data.replace(offset, length, names);
    }
    if (!renamed || headOffset < 0 || headOffset + 12 > data.size())
        return {};
    setSfntUInt32(data, headOffset + 8, 0);
    for (int table = 0; table < tableCount; ++table) {
        const qsizetype entry = 12 + table * 16;
        setSfntUInt32(data, entry + 4,
            sfntChecksum(data, sfntUInt32(data, entry + 8),
                         sfntUInt32(data, entry + 12)));
    }
    setSfntUInt32(data, headOffset + 8,
        quint32(0xb1b0afba) - sfntChecksum(data, 0, data.size()));
    return data;
}

QByteArray renamedBundledFontData(const QString &family, bool bold = false, bool italic = false)
{
    const QByteArray source = ApplicationFonts::bundledFontData(QStringLiteral("Arial"), bold, italic);
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
                name.replace(QByteArrayLiteral("Arial"), replacement.toLatin1());
                name.replace(utf16be(QStringLiteral("Arial")), utf16be(replacement));
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

QByteArray fontArchive(const QList<QPair<QByteArray, QByteArray>> &entries)
{
    QTemporaryFile file;
    if (!file.open())
        return {};
    file.close();
    zipFile archive = zipOpen64(QFile::encodeName(file.fileName()).constData(), APPEND_STATUS_CREATE);
    if (!archive)
        return {};
    const auto closeArchive = qScopeGuard([&]() {
        if (archive)
            zipClose(archive, nullptr);
    });
    for (const auto &entry : entries) {
        if (zipOpenNewFileInZip64(archive, entry.first.constData(), nullptr,
                                  nullptr, 0, nullptr, 0, nullptr, 0, 0, 0) != ZIP_OK)
            return {};
        const int written = zipWriteInFileInZip(archive, entry.second.constData(),
                                                unsigned(entry.second.size()));
        const int closed = zipCloseFileInZip(archive);
        if (written != ZIP_OK || closed != ZIP_OK)
            return {};
    }
    const int result = zipClose(archive, nullptr);
    archive = nullptr;
    QFile contents(file.fileName());
    if (result != ZIP_OK || !contents.open(QIODevice::ReadOnly))
        return {};
    return contents.readAll();
}

QList<QPair<QByteArray, QByteArray>> aptosArchiveEntries()
{
    return {{QByteArrayLiteral("Aptos.ttf"), uninstalledSubsetFontData("Aptos")},
            {QByteArrayLiteral("Aptos-Italic.ttf"), uninstalledSubsetFontData("Aptos", false, true)},
            {QByteArrayLiteral("Aptos-Bold.ttf"), uninstalledSubsetFontData("Aptos", true, false)},
            {QByteArrayLiteral("Aptos-Bold-Italic.ttf"), uninstalledSubsetFontData("Aptos", true, true)},
            {QByteArrayLiteral("notes/readme.txt"), QByteArrayLiteral("Unrelated archive entry")}};
}

bool createUninstalledSubsetPdf(const QString &path,
                               const QString &objectText = QStringLiteral("2017"),
                               const QString &seedText = QStringLiteral("a "),
                               const QByteArray &alias = QByteArrayLiteral("QrZxy"))
{
    const QByteArray fontData = uninstalledSubsetFontData(alias);
    const QRawFont rawFont(fontData, 16.0, QFont::PreferNoHinting);
    if (!rawFont.isValid() || rawFont.familyName() != QString::fromLatin1(alias))
        return false;
    PDFiumProcessLock lock;
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document)
        return false;
    const auto closeDocument = qScopeGuard([&]() { FPDF_CloseDocument(document); });
    FPDF_FONT font = FPDFText_LoadFont(document,
        reinterpret_cast<const uint8_t *>(fontData.constData()),
        static_cast<uint32_t>(fontData.size()), FPDF_FONT_TRUETYPE, true);
    if (!font)
        return false;
    const auto closeFont = qScopeGuard([&]() { FPDFFont_Close(font); });
    FPDF_PAGE page = FPDFPage_New(document, 0, 595.0, 842.0);
    if (!page)
        return false;
    const auto closePage = qScopeGuard([&]() { FPDF_ClosePage(page); });
    if (!addTextObjectWithFont(document, page, font,
                              objectText, 72.0, 740.0)
        || !addTextObjectWithFont(document, page, font,
                                 seedText, 72.0, 680.0))
        return false;
    if (!FPDFPage_GenerateContent(page))
        return false;
    PdfFileWriter writer(path);
    return writer.open() && FPDF_SaveAsCopy(document, &writer,
        FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS);
}

bool combineSubsetPages(const QString &firstPath, const QString &secondPath,
                        const QString &outputPath)
{
    PDFiumProcessLock lock;
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    if (!document)
        return false;
    const auto closeDocument = qScopeGuard([&]() { FPDF_CloseDocument(document); });
    for (const QString &sourcePath : {firstPath, secondPath}) {
        FPDF_DOCUMENT source = FPDF_LoadDocument(
            QFile::encodeName(sourcePath).constData(), nullptr);
        if (!source)
            return false;
        const auto closeSource = qScopeGuard([&]() { FPDF_CloseDocument(source); });
        if (!FPDF_ImportPages(document, source, "1", FPDF_GetPageCount(document)))
            return false;
    }
    PdfFileWriter writer(outputPath);
    return writer.open() && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
}

QByteArray embeddedSourceFontData(const QString &path, const QString &text,
                                 int pageIndex = 0, bool matchPrefix = false)
{
    PDFiumProcessLock lock;
    FPDF_DOCUMENT document = FPDF_LoadDocument(
        QFile::encodeName(path).constData(), nullptr);
    if (!document)
        return {};
    const auto closeDocument = qScopeGuard([&]() { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDF_LoadPage(document, pageIndex);
    if (!page)
        return {};
    const auto closePage = qScopeGuard([&]() { FPDF_ClosePage(page); });
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage)
        return {};
    const auto closeTextPage = qScopeGuard([&]() { FPDFText_ClosePage(textPage); });
    for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
        if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT)
            continue;
        const QString objectText = pageObjectText(object, textPage).trimmed();
        if (matchPrefix ? !objectText.startsWith(text) : objectText != text)
            continue;
        FPDF_FONT font = FPDFTextObj_GetFont(object);
        size_t size = 0;
        if (!FPDFFont_GetFontData(font, nullptr, 0, &size) || size == 0)
            return {};
        QByteArray bytes(qsizetype(size), Qt::Uninitialized);
        size_t actual = 0;
        if (!FPDFFont_GetFontData(font,
                reinterpret_cast<uint8_t *>(bytes.data()), size, &actual)
            || actual != size)
            return {};
        return bytes;
    }
    return {};
}

bool preservesFontProgram(const QByteArray &saved, const QByteArray &original)
{
    const QRawFont savedFont(saved, 16.0, QFont::PreferNoHinting);
    const QRawFont originalFont(original, 16.0, QFont::PreferNoHinting);
    if (!savedFont.isValid() || !originalFont.isValid()
        || originalFont.fontTable("glyf").isEmpty())
        return false;
    // Save may give the resource a distinct PostScript name. The complete
    // outlines, Unicode coverage, advances and kerning must remain unchanged.
    for (const char *tag : {"glyf", "loca", "cmap", "maxp", "hmtx", "kern", "GPOS", "GSUB"}) {
        if (savedFont.fontTable(tag) != originalFont.fontTable(tag))
            return false;
    }
    return true;
}

bool rewriteTextWithSubsetFont(const QString &sourcePath, const QString &targetPath,
                               const QString &text, const QByteArray &fontData)
{
    PDFiumProcessLock lock;
    FPDF_DOCUMENT document = FPDF_LoadDocument(QFile::encodeName(sourcePath).constData(), nullptr);
    if (!document || fontData.isEmpty()) {
        if (document)
            FPDF_CloseDocument(document);
        return false;
    }
    const auto closeDocument = qScopeGuard([&]() { FPDF_CloseDocument(document); });
    FPDF_FONT font = FPDFText_LoadFont(document,
        reinterpret_cast<const uint8_t *>(fontData.constData()),
        static_cast<uint32_t>(fontData.size()), FPDF_FONT_TRUETYPE, true);
    if (!font)
        return false;
    const auto closeFont = qScopeGuard([&]() { FPDFFont_Close(font); });
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page)
        return false;
    const auto closePage = qScopeGuard([&]() { FPDF_ClosePage(page); });
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage)
        return false;
    FPDF_PAGEOBJECT original = nullptr;
    for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
        if (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_TEXT
            && pageObjectText(object, textPage).trimmed() == text) {
            original = object;
            break;
        }
    }
    FPDFText_ClosePage(textPage);
    float size = 0;
    FS_MATRIX matrix;
    if (!original || !FPDFTextObj_GetFontSize(original, &size)
        || !FPDFPageObj_GetMatrix(original, &matrix))
        return false;
    FPDF_PAGEOBJECT replacement = FPDFPageObj_CreateTextObj(document, font, size);
    if (!replacement)
        return false;
    const auto destroyReplacement = qScopeGuard([&]() {
        if (replacement)
            FPDFPageObj_Destroy(replacement);
    });
    if (!FPDFText_SetText(replacement, reinterpret_cast<const FPDF_WCHAR *>(text.utf16()))
        || !FPDFPageObj_SetMatrix(replacement, &matrix)
        || !FPDFPage_RemoveObject(page, original))
        return false;
    FPDFPageObj_Destroy(original);
    if (!FPDFPage_InsertObject(page, replacement))
        return false;
    replacement = nullptr;
    if (!FPDFPage_GenerateContent(page))
        return false;
    // These regressions deliberately need an imported subset; production Save
    // keeps complete programs to retain layout tables on later edits.
    PdfFileWriter writer(targetPath);
    return writer.open() && FPDF_SaveAsCopy(document, &writer,
        FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS);
}

bool createUnavailableType1FontPdf(const QString &path, const QByteArray &fontName)
{
    const QByteArray content("BT /F1 12 Tf 40 760 Td "
                             "(to Share\\227to copy, distribute the work) Tj ET\n");
    return writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        QByteArray("<< /Type /Font /Subtype /Type1 /BaseFont /") + fontName
            + QByteArray(" /Encoding /WinAnsiEncoding >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n") + content + QByteArray("endstream")
    });
}

bool createMixedPreflightFontsPdf(const QString &path, const QByteArray &first,
                                 const QByteArray &second)
{
    const QByteArray content = "BT /F1 12 Tf 40 760 Td (Alpha ) Tj "
        "/F2 12 Tf (beta ) Tj /F3 12 Tf (gamma ) Tj /F1 12 Tf (delta) Tj ET\n"
        "BT /F1 12 Tf 40 700 Td (Other object) Tj ET\n";
    const auto font = [](const QByteArray &name) {
        return "<< /Type /Font /Subtype /Type1 /BaseFont /" + name
            + " /Encoding /WinAnsiEncoding >>";
    };
    return writePdfObjects(path, {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
            "/Resources << /Font << /F1 4 0 R /F2 5 0 R /F3 6 0 R >> >> /Contents 7 0 R >>",
        font(first + "-Regular"), font(second + "-Bold"), font(first + "-Italic"),
        "<< /Length " + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream"});
}

} // namespace

void PDFEditControllerTest::usesCompatibleFontForUnreadableType1Glyphs_data()
{
    QTest::addColumn<QByteArray>("fontName");
    QTest::addColumn<bool>("compatible");
    QTest::addColumn<bool>("bold");
    QTest::addColumn<bool>("italic");
    QTest::newRow("termes-regular")
        << QByteArray("TeXGyreTermes-Regular") << true << false << false;
    QTest::newRow("termes-bold-italic")
        << QByteArray("TeXGyreTermes-BoldItalic") << true << true << true;
    QTest::newRow("unrelated-family")
        << QByteArray("OtherTeXGyreTermes-Regular") << false << false << false;
}

void PDFEditControllerTest::usesCompatibleFontForUnreadableType1Glyphs()
{
    QFETCH(QByteArray, fontName);
    QFETCH(bool, compatible);
    QFETCH(bool, bold);
    QFETCH(bool, italic);
    const QString expectedText = QStringLiteral("to Share\u2014to copy, distribute the work");
    const QString fallback = QStringLiteral("Times New Roman");
    if (!ApplicationFonts::fontFaceSupportsText(fallback, bold, italic, expectedText)) {
        QSKIP("Times New Roman exact face is unavailable");
    }
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("unavailable-type1-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("compatible-type1-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createUnavailableType1FontPdf(sourcePath, fontName));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), expectedText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QString requestedFamily = compatible ? QStringLiteral("TeX Gyre Termes")
                                               : QStringLiteral("OtherTeXGyreTermes");
    QCOMPARE(controller.selectedFontFamily(), requestedFamily);
    QCOMPARE(controller.selectedFontBold(), bold);
    QCOMPARE(controller.selectedFontItalic(), italic);
    if (compatible) {
        const QString displayFamily = candidate.value(QStringLiteral("fontFamily")).toString();
        QVERIFY(displayFamily != requestedFamily);
        QFont display(displayFamily);
        display.setBold(bold);
        display.setItalic(italic);
        display.setPixelSize(16);
        const QRawFont displayed = QRawFont::fromFont(display);
        QVERIFY(displayed.isValid());
        const auto fallbackFace = ApplicationFonts::systemFontFace(fallback, bold, italic);
        const QRawFont expected(fallbackFace.isValid() ? fallbackFace.data
                                 : ApplicationFonts::bundledFontData(fallback, bold, italic),
                               16.0, QFont::PreferNoHinting);
        QVERIFY(expected.isValid());
        const auto displayedGlyphs = displayed.glyphIndexesForString(expectedText);
        const auto expectedGlyphs = expected.glyphIndexesForString(expectedText);
        QVERIFY(!displayedGlyphs.contains(0));
        QCOMPARE(displayedGlyphs, expectedGlyphs);
        QCOMPARE(displayed.advancesForGlyphIndexes(displayedGlyphs),
                 expected.advancesForGlyphIndexes(expectedGlyphs));
    }
    controller.beginSelectedTextEdit();
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
    const QString editedText = expectedText + QStringLiteral(" again");
    controller.commitSelectedText(editedText);
    QSignalSpy requested(&controller, &PDFEditController::fontDownloadRequested);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(outputPath)));
    QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 10000);
    QCOMPARE(controller.missingFontFamily(), requestedFamily);
    QVERIFY(controller.hasDrafts());
    QVERIFY(!QFileInfo::exists(outputPath));
    controller.respondToFontDownload(false);
    QVERIFY(!controller.saving());
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedText(), editedText);
    QVERIFY(!QFileInfo::exists(outputPath));
}

void PDFEditControllerTest::reembedsUninstalledSourceSubsetForNewObjectGlyphs()
{
    QVERIFY(!ApplicationFonts::systemFontFamilies().contains(kUninstalledSubsetFamily));
    QVERIFY(!ApplicationFonts::bundledFontFamilies().contains(kUninstalledSubsetFamily));
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("uninstalled-subset-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("uninstalled-subset-output.pdf"));
    QVERIFY(createUninstalledSubsetPdf(sourcePath));
    const TextObjectFontInfo sourceInfo = textObjectFontInfo(
        sourcePath, QStringLiteral("2017"));
    QVERIFY(sourceInfo.found);
    QCOMPARE(sourceInfo.familyName, kUninstalledSubsetFamily);
    QVERIFY(hasSubsetFontPrefix(sourceInfo.baseName));
    const QByteArray sourceData = embeddedSourceFontData(sourcePath, QStringLiteral("2017"));
    QVERIFY(!sourceData.isEmpty());
    QVERIFY(sourceData.size() < uninstalledSubsetFontData().size());
    const QRawFont sourceFont(sourceData, 16.0, QFont::PreferNoHinting);
    QVERIFY(sourceFont.isValid());
    QVERIFY(sourceFont.supportsCharacter(uint('a')));
    QVERIFY(!sourceFont.supportsCharacter(uint('z')));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("2017"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedFontFamily(), kUninstalledSubsetFamily);
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(QStringLiteral("2017 a"));
    controller.finishSelectedTextEdit();
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY2(pageText(outputPath).contains(QStringLiteral("2017 a")),
             qPrintable(pageText(outputPath)));
    const TextObjectFontInfo outputInfo = textObjectFontInfo(
        outputPath, QStringLiteral("2017 a"));
    QVERIFY(outputInfo.found);
    QCOMPARE(outputInfo.familyName, kUninstalledSubsetFamily);
    QCOMPARE(outputInfo.embedded, 1);
    QVERIFY(preservesFontProgram(
        embeddedSourceFontData(outputPath, QStringLiteral("2017 a")), sourceData));
}

void PDFEditControllerTest::preservesUnicodeAliasesWhenReembeddingSourceSubset()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-subset-unicode-aliases.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("saved-subset-unicode-aliases.pdf"));
    QVERIFY(createUninstalledSubsetPdf(sourcePath));
    const QRawFont sourceFont(
        embeddedSourceFontData(sourcePath, QStringLiteral("2017")),
        16.0, QFont::PreferNoHinting);
    QVERIFY(sourceFont.isValid());
    QVERIFY(sourceFont.supportsCharacter(uint(' ')));
    QVERIFY(sourceFont.supportsCharacter(0x00a0U));
    const auto aliasGlyphs = sourceFont.glyphIndexesForString(QStringLiteral(" \u00a0"));
    QCOMPARE(aliasGlyphs.size(), 2);
    QVERIFY(aliasGlyphs.first() != 0);
    QCOMPARE(aliasGlyphs.first(), aliasGlyphs.last());

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("2017"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    // The two spaces share an outline but must retain separate Unicode values.
    const QString editedText = QStringLiteral("2017 a\u00a0a");
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(editedText);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.selectedText(), editedText);
    QCOMPARE(controller.selectedFontFamily(), kUninstalledSubsetFamily);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY2(topLevelTextObjects(outputPath).contains(editedText),
             qPrintable(topLevelTextObjects(outputPath).join(QLatin1Char('|'))));
    const TextObjectFontInfo outputInfo = textObjectFontInfo(outputPath, editedText);
    QVERIFY(outputInfo.found);
    QCOMPARE(outputInfo.familyName, kUninstalledSubsetFamily);
    QCOMPARE(outputInfo.embedded, 1);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::preservesSourceSubsetLayoutControls_data()
{
    QTest::addColumn<bool>("wordSpacing");
    QTest::newRow("plain") << false;
    QTest::newRow("spaced") << true;
}

void PDFEditControllerTest::preservesSourceSubsetLayoutControls()
{
    QFETCH(bool, wordSpacing);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("subset-layout-controls-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("subset-layout-controls-output.pdf"));
    QVERIFY(createUninstalledSubsetPdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("2017"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(QStringLiteral("2017 a\na\t2017"));
    if (wordSpacing) {
        controller.setSelectedTextRange(0, 13);
        controller.setSelectedWordSpacing(2.0);
    }
    // Each control has its own span; neither needs a font glyph. A styled
    // ordinary space must still be encoded if layout attaches it to a cluster.
    for (int position : {4, 6, 8}) {
        controller.setSelectedTextRange(position, 1);
        controller.setSelectedColor(QColor(QStringLiteral("#1565c0")));
    }
    controller.finishSelectedTextEdit();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    const QStringList objects = topLevelTextObjects(outputPath);
    QString joined = objects.join(QString());
    joined.remove(QLatin1Char(' '));
    // Includes the unchanged "a " seed object below the edited paragraph.
    QCOMPARE(joined, QStringLiteral("2017aa2017a"));
    for (const QString &text : objects) {
        const auto info = textObjectFontInfo(outputPath, text.trimmed());
        QVERIFY(info.found);
        QCOMPARE(info.familyName, kUninstalledSubsetFamily);
        QCOMPARE(info.embedded, 1);
    }
}

void PDFEditControllerTest::rejectsMissingSourceSubsetGlyphWithoutReplacingDestination()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("missing-subset-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("missing-subset-output.pdf"));
    QVERIFY(createUninstalledSubsetPdf(sourcePath));
    const QRawFont sourceFont(
        embeddedSourceFontData(sourcePath, QStringLiteral("2017")),
        16.0, QFont::PreferNoHinting);
    QVERIFY(sourceFont.isValid());
    QVERIFY(!sourceFont.supportsCharacter(uint('z')));
    const QByteArray existingBytes("existing destination must survive\n");
    QFile output(outputPath);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write(existingBytes), existingBytes.size());
    output.close();
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("2017"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("2017 z"));
    const int originalRevision = document.renderRevision();
    const auto result = PDFSaveWriter::save(controller, outputPath);
    QCOMPARE(result.error, PDFSaveWriter::Error::FontUnavailable);
    QCOMPARE(result.unsupportedCodePoint, uint('z'));
    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), existingBytes);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(document.renderRevision(), originalRevision);
    QCOMPARE(document.source().toLocalFile(), sourcePath);
    QVERIFY(!pageText(sourcePath).contains(QStringLiteral("2017 z")));
}

void PDFEditControllerTest::separatesSameFamilySourceSubsetCaches()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("distinct-subsets-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("distinct-subsets-output.pdf"));
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("distinct-subsets-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("distinct-subsets-second.pdf"));
    QVERIFY(createUninstalledSubsetPdf(firstPath));
    QVERIFY(createUninstalledSubsetPdf(
        secondPath, QStringLiteral("2018"), QStringLiteral("b ")));
    QVERIFY(combineSubsetPages(firstPath, secondPath, sourcePath));
    const QByteArray firstData = embeddedSourceFontData(sourcePath, QStringLiteral("2017"));
    const QByteArray secondData = embeddedSourceFontData(
        sourcePath, QStringLiteral("2018"), 1);
    QVERIFY(!firstData.isEmpty());
    QVERIFY(!secondData.isEmpty());
    QVERIFY(firstData != secondData);
    const QRawFont firstFont(firstData, 16.0, QFont::PreferNoHinting);
    const QRawFont secondFont(secondData, 16.0, QFont::PreferNoHinting);
    QCOMPARE(firstFont.familyName(), secondFont.familyName());
    QVERIFY(firstFont.supportsCharacter(uint('a')));
    QVERIFY(!firstFont.supportsCharacter(uint('b')));
    QVERIFY(secondFont.supportsCharacter(uint('b')));
    QVERIFY(!secondFont.supportsCharacter(uint('a')));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    int page = 0;
    for (const auto &entry : {
             std::pair{QStringLiteral("2017"), QStringLiteral("2017a")},
             std::pair{QStringLiteral("2018"), QStringLiteral("2018b")}}) {
        const QVariantMap candidate = candidateForText(
            waitForCandidates(controller, page), entry.first);
        QVERIFY(!candidate.isEmpty());
        QVERIFY(controller.selectCandidate(
            page, candidate.value(QStringLiteral("objectId")).toString()));
        controller.beginSelectedTextEdit();
        controller.updateSelectedTextPreview(entry.second);
        controller.finishSelectedTextEdit();
        ++page;
    }
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(pageText(outputPath).contains(QStringLiteral("2017a")));
    QVERIFY(pageText(outputPath, 1).contains(QStringLiteral("2018b")));
    const QRawFont outputFirst(
        embeddedSourceFontData(outputPath, QStringLiteral("2017a")),
        16.0, QFont::PreferNoHinting);
    const QRawFont outputSecond(
        embeddedSourceFontData(outputPath, QStringLiteral("2018b"), 1),
        16.0, QFont::PreferNoHinting);
    QCOMPARE(outputFirst.familyName(), kUninstalledSubsetFamily);
    QCOMPARE(outputSecond.familyName(), kUninstalledSubsetFamily);
    QVERIFY(outputFirst.supportsCharacter(uint('a')));
    QVERIFY(!outputFirst.supportsCharacter(uint('b')));
    QVERIFY(outputSecond.supportsCharacter(uint('b')));
    QVERIFY(!outputSecond.supportsCharacter(uint('a')));
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

void PDFEditControllerTest::isolatesEmbeddedSubsetFromCachedCompleteFace()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("DlIso");
    const QByteArray complete = uninstalledSubsetFontData("DlIso");
    QString error;
    QVERIFY2(ApplicationFonts::installDownloadedFont(complete, family, false, false, uint('z'), error),
             qPrintable(error));
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("font-isolation-source.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("font-isolation-output.pdf"));
    const QString resaved = m_temporaryDirectory.filePath(QStringLiteral("font-isolation-resaved.pdf"));
    QVERIFY(createUninstalledSubsetPdf(source, QStringLiteral("2017"), QStringLiteral("a "), "DlIso"));
    const QRawFont original(embeddedSourceFontData(source, QStringLiteral("2017")),
                            16.0, QFont::PreferNoHinting);
    QVERIFY(original.isValid());
    QVERIFY(original.supportsCharacter(uint('a')));
    QVERIFY(!original.supportsCharacter(uint('z')));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    const auto candidate = candidateForText(waitForCandidates(controller, 0), QStringLiteral("2017"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedFontFamily(), family);
    controller.beginSelectedTextEdit();
    const QVariantList spans = controller.selectedCharacterSpans();
    QVERIFY(!spans.isEmpty());
    const QString displayFamily = spans.first().toMap().value(QStringLiteral("fontFamily")).toString();
    QVERIFY(!displayFamily.isEmpty());
    QVERIFY(displayFamily != family);
    QFont displayFont(displayFamily);
    displayFont.setPixelSize(16);
    const QRawFont displayed = QRawFont::fromFont(displayFont);
    QVERIFY(displayed.isValid());
    QVERIFY(displayed.supportsCharacter(uint('a')));
    QVERIFY(!displayed.supportsCharacter(uint('z')));
    QFont cachedFont(family);
    cachedFont.setPixelSize(16);
    QVERIFY(QRawFont::fromFont(cachedFont).supportsCharacter(uint('z')));

    controller.updateSelectedTextPreview(QStringLiteral("2017 a"));
    controller.finishSelectedTextEdit();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const QRawFont preserved(embeddedSourceFontData(output, QStringLiteral("2017 a")),
                            16.0, QFont::PreferNoHinting);
    QVERIFY(preserved.isValid());
    QCOMPARE(preserved.familyName(), family);
    QVERIFY(!preserved.supportsCharacter(uint('z')));
    const auto reopened = candidateForText(waitForCandidates(controller, 0), QStringLiteral("2017 a"));
    QVERIFY(!reopened.isEmpty());
    QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedFontFamily(), family);
    controller.commitSelectedText(QStringLiteral("2017 az"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(resaved)));
    QVERIFY2(normalizedPageText(resaved).contains(QStringLiteral("2017 az")), qPrintable(pageText(resaved)));
    const auto savedFont = textObjectFontInfo(resaved, QStringLiteral("z"));
    QVERIFY(savedFont.found);
    QCOMPARE(savedFont.familyName, family);
    QCOMPARE(savedFont.embedded, 1);
    QCOMPARE(ApplicationFonts::downloadedFontFace(family, false, false).data, complete);
}

void PDFEditControllerTest::keepsArialLayoutStableAfterEmbeddedSubsetCollision()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("arial-collision-source.pdf"));
    const QString collisionPath = m_temporaryDirectory.filePath(
        QStringLiteral("arial-collision-subset.pdf"));
    const QString fullPath = m_temporaryDirectory.filePath(
        QStringLiteral("arial-collision-full.pdf"));
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
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(fullPath)));
    QVERIFY(rewriteTextWithSubsetFont(fullPath, collisionPath, QStringLiteral("Á"),
        ApplicationFonts::bundledFontData(QStringLiteral("Arial"), false, false)));
    QVERIFY(document.open(QUrl::fromLocalFile(collisionPath)));

    const TextObjectFontInfo embeddedFont = textObjectFontInfo(
        collisionPath,
        QStringLiteral("Á"));
    QVERIFY(embeddedFont.found);
    QCOMPARE(embeddedFont.embedded, 1);
    QVERIFY(hasSubsetFontPrefix(embeddedFont.baseName));
    QCOMPARE(embeddedFont.familyName, QStringLiteral("Arial"));
    const QRawFont subsetProgram(embeddedSourceFontData(collisionPath, QStringLiteral("Á")),
                                16.0, QFont::PreferNoHinting);
    QVERIFY(subsetProgram.isValid());
    QVERIFY(!subsetProgram.supportsCharacter(uint('W')));

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

void PDFEditControllerTest::preservesFirstSpaceAfterSubsetSave()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("first-space-source.pdf"));
    const QString subsetPath = m_temporaryDirectory.filePath(
        QStringLiteral("first-space-subset.pdf"));
    const QString fullPath = m_temporaryDirectory.filePath(
        QStringLiteral("first-space-full.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("first-space-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(!controller.addText(0, 0.2, 0.65).isEmpty());
    const QString originalText = QStringLiteral("ashdljasdsdadasd");
    controller.updateSelectedTextPreview(originalText);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.2, 0.65, 0.7, 0.15));
    controller.finishSelectedFrameResize();
    controller.finishSelectedTextEdit();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(fullPath)));
    const QByteArray completeFont = embeddedSourceFontData(fullPath, originalText);
    QVERIFY(!completeFont.isEmpty());
    QVERIFY(rewriteTextWithSubsetFont(fullPath, subsetPath, originalText, completeFont));
    QVERIFY(document.open(QUrl::fromLocalFile(subsetPath)));
    const TextObjectFontInfo subsetFont = textObjectFontInfo(
        subsetPath, originalText);
    QVERIFY2(subsetFont.found,
             qPrintable(topLevelTextObjects(subsetPath).join(QLatin1Char('|'))));
    QCOMPARE(subsetFont.embedded, 1);
    QVERIFY(hasSubsetFontPrefix(subsetFont.baseName));

    const QString verificationPath = qEnvironmentVariable(
        "TREPDF_FIRST_SPACE_TEST_OUTPUT");
    if (!verificationPath.isEmpty()) {
        const QString subsetVerificationPath
            = verificationPath + QStringLiteral(".subset.pdf");
        QFile::remove(subsetVerificationPath);
        QVERIFY(QFile::copy(subsetPath, subsetVerificationPath));
    }

    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), originalText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    // Only the space is new. Introducing a new visible letter would already
    // force embedding and hide the missing-space CID in the original subset.
    const QString editedText = originalText + QStringLiteral(" ddads");
    controller.beginSelectedTextEdit();
    controller.commitSelectedText(editedText);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    if (!verificationPath.isEmpty()) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(outputPath, verificationPath));
    }
    // Retained spacing may split the text into glyph-cluster objects. Every
    // character, including the new space, must still have a real text object
    // and font; a space merely inferred by PDFium would fail this inspection.
    QCOMPARE(textCharacterRangeInfo(outputPath, editedText).size(), editedText.size());
    QVERIFY(pageText(outputPath).contains(editedText));

    const QVariantMap reopenedCandidate = candidateForText(
        waitForCandidates(controller, 0), editedText);
    QVERIFY(!reopenedCandidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, reopenedCandidate.value(QStringLiteral("objectId")).toString()));
    const QString repeatedText = editedText + QStringLiteral(" asd");
    controller.beginSelectedTextEdit();
    controller.commitSelectedText(repeatedText);
    QVERIFY(controller.save());
    QCOMPARE(textCharacterRangeInfo(outputPath, repeatedText).size(), repeatedText.size());
    QVERIFY(pageText(outputPath).contains(repeatedText));
    QVERIFY(!candidateForText(
        waitForCandidates(controller, 0), repeatedText).isEmpty());
}

void PDFEditControllerTest::preservesInferredSpaceAfterGroupedSubsetEdit()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("grouped-subset-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("grouped-subset-output.pdf"));
    QVERIFY(createGroupedSubsetFontPdf(sourcePath));
    const QStringList sourceObjects = topLevelTextObjects(sourcePath);
    QCOMPARE(sourceObjects.size(), 2);
    // PDFium's extraction can append a generated space for the visual gap;
    // the fixture passes only the letters to FPDFText_SetText().
    QCOMPARE(sourceObjects.at(0).trimmed(), QStringLiteral("Hello"));
    QCOMPARE(sourceObjects.at(1).trimmed(), QStringLiteral("world"));
    const TextObjectFontInfo sourceFont = textObjectFontInfo(
        sourcePath, QStringLiteral("Hello"));
    QVERIFY(sourceFont.found);
    QCOMPARE(sourceFont.embedded, 1);
    QVERIFY(hasSubsetFontPrefix(sourceFont.baseName));
    QVERIFY(textObjectsShareFont(
        sourcePath, QStringLiteral("Hello"), QStringLiteral("world")));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString groupedText = QString::fromLatin1(kGroupedText);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), groupedText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(candidate.value(QStringLiteral("grouped")).toBool());
    QCOMPARE(candidate.value(QStringLiteral("sourceRects")).toList().size(), 2);
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    // Formatting materializes the inferred separator even though the user
    // has not introduced any new character into the displayed candidate.
    controller.setSelectedColor(QColor(QStringLiteral("#c62828")));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(normalizedPageText(outputPath), groupedText);
    const auto characters = textCharacterRangeInfo(outputPath, groupedText);
    QCOMPARE(characters.size(), groupedText.size());
    QCOMPARE(characters.at(5).text, QStringLiteral(" "));
    for (const auto &character : characters)
        QCOMPARE(character.color, QColor(QStringLiteral("#c62828")));
    QVERIFY(!candidateForText(
        waitForCandidates(controller, 0), groupedText).isEmpty());
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

        const auto characters = textCharacterRangeInfo(outputPath, text);
        QCOMPARE(characters.size(), text.size());
        for (qsizetype index = 0; index < characters.size(); ++index) {
            const qreal savedScale = horizontalScale(characters[index].matrix);
            QVERIFY2(
                qAbs(savedScale - expectedScale) <= 0.01,
                qPrintable(QStringLiteral(
                    "Round %1 character %2 expected source scale %3, got %4")
                               .arg(round).arg(index)
                               .arg(expectedScale, 0, 'f', 6)
                               .arg(savedScale, 0, 'f', 6)));
        }
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

    const auto characters = textCharacterRangeInfo(outputPath, text);
    QCOMPARE(characters.size(), text.size());
    for (qsizetype index = 0; index < characters.size(); ++index) {
        const qreal scale = horizontalScale(characters[index].matrix);
        if (index >= 1 && index < 4) {
            QVERIFY2(scale >= 0.85 && scale <= 1.20,
                qPrintable(QStringLiteral("Changed Arial character %1 scale is %2")
                    .arg(index).arg(scale, 0, 'f', 6)));
        } else {
            QVERIFY2(qAbs(scale - 2.0) <= 0.01,
                qPrintable(QStringLiteral("Unchanged source character %1 scale is %2")
                    .arg(index).arg(scale, 0, 'f', 6)));
        }
    }
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

        const auto characters = textCharacterRangeInfo(finalOutputPath, text);
        QCOMPARE(characters.size(), text.size());
        const qreal savedScale = horizontalScale(characters.constFirst().matrix);
        for (qsizetype index = 0; index < characters.size(); ++index) {
            QVERIFY2(qAbs(horizontalScale(characters[index].matrix) - savedScale) <= 0.01,
                qPrintable(QStringLiteral("Round %1 character %2 has a different Didot scale")
                               .arg(round).arg(index)));
        }
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

    const auto systemFace = ApplicationFonts::systemFontFace(
        QStringLiteral("Arial"), expectedBold, false);
    const QByteArray fullFontData = systemFace.isValid() ? systemFace.data
        : ApplicationFonts::bundledFontData(QStringLiteral("Arial"), expectedBold, false);
    QVERIFY(!fullFontData.isEmpty());
    const auto sourceCharacters = textCharacterRangeInfo(sourcePath, sourceText);
    QCOMPARE(sourceCharacters.size(), sourceText.size());
    const auto editedCharacters = textCharacterRangeInfo(outputPath, expectedText);
    QCOMPARE(editedCharacters.size(), expectedText.size());
    for (qsizetype index = 0; index < editedCharacters.size(); ++index) {
        const auto &character = editedCharacters[index];
        if (expectedBold && index == editedCharacters.size() - 1) {
            // The unchanged final 't' retains its source correction/span, so
            // it can reuse Helvetica-Bold independently of the new Unicode run.
            const auto &sourceCharacter = sourceCharacters.constLast();
            QCOMPARE(character.text, QStringLiteral("t"));
            QCOMPARE(character.text, sourceCharacter.text);
            QCOMPARE(character.font.baseName, sourceCharacter.font.baseName);
            QCOMPARE(character.font.familyName, sourceCharacter.font.familyName);
            QCOMPARE(character.font.embedded, sourceCharacter.font.embedded);
            QCOMPARE(character.font.dataSize, sourceCharacter.font.dataSize);
            QCOMPARE(character.font.weight, sourceCharacter.font.weight);
            QCOMPARE(character.font.italic, sourceCharacter.font.italic);
            QCOMPARE(character.pointSize, sourceCharacter.pointSize);
            QVERIFY(qAbs(horizontalScale(character.matrix)
                         - horizontalScale(sourceCharacter.matrix)) < 0.01);
            continue;
        }
        QCOMPARE(character.font.familyName, QStringLiteral("Arial"));
        QVERIFY2(character.font.embedded == 1,
            qPrintable(QStringLiteral("Character '%1' uses %2 / %3 (embedded=%4)")
                           .arg(character.text, character.font.familyName, character.font.baseName)
                           .arg(character.font.embedded)));
        QVERIFY(character.font.dataSize > 0);
        const QRawFont savedFont(character.fontData, 16.0, QFont::PreferNoHinting);
        QVERIFY(savedFont.isValid());
        QCOMPARE(savedFont.weight() >= QFont::Bold, expectedBold);
        QVERIFY(preservesFontProgram(character.fontData, fullFontData));
    }

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
    // face above the old 32 MiB limit reaches PDFium and survives save intact.
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
        compactFontName(editedFont.familyName).contains(compactExpectedFamily),
        qPrintable(QStringLiteral("Saved font: %1 / %2")
                       .arg(editedFont.familyName, editedFont.baseName)));
    const QByteArray savedFontData = embeddedSourceFontData(outputPath, expectedText.left(3), 0, true);
    const QRawFont savedFont(savedFontData, 16.0, QFont::PreferNoHinting);
    QVERIFY(savedFont.isValid());
    QCOMPARE(savedFont.weight() >= QFont::Bold, fontBold);
    QCOMPARE(editedFont.italic, fontItalic);
    QCOMPARE(editedFont.embedded, 1);
    QVERIFY(editedFont.dataSize > 0);
    QVERIFY(preservesFontProgram(savedFontData, systemFace.data));

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
    const auto editedCharacters = textCharacterRangeInfo(outputPath, expectedText);
    QCOMPARE(editedCharacters.size(), expectedText.size());
    const QString compactExpectedFamily = compactFontName(fontFamily);
    for (const auto &character : editedCharacters) {
        QVERIFY2(
            compactFontName(character.font.familyName).contains(compactExpectedFamily),
            qPrintable(QStringLiteral("Saved font: %1 / %2")
                           .arg(character.font.familyName, character.font.baseName)));
        QCOMPARE(character.font.embedded, 1);
        QVERIFY(character.font.dataSize > 0);
        QVERIFY(preservesFontProgram(character.fontData, systemFace.data));
    }
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
    // A color-only edit must reuse the source font resource for every cluster,
    // including spaces, even when retained spacing splits the original object.
    QVERIFY(textRangesShareFont(outputPath, {editedText, untouchedText}));
    const auto editedCharacters = textCharacterRangeInfo(outputPath, editedText);
    const auto untouchedCharacters = textCharacterRangeInfo(outputPath, untouchedText);
    QCOMPARE(editedCharacters.size(), editedText.size());
    QCOMPARE(untouchedCharacters.size(), untouchedText.size());
    for (const auto &character : editedCharacters) {
        QCOMPARE(character.font.embedded, 1);
        QCOMPARE(character.color, QColor(QStringLiteral("#c62828")));
    }
    for (const auto &character : untouchedCharacters) {
        QCOMPARE(character.font.embedded, 1);
        QCOMPARE(character.color, QColor(Qt::black));
    }
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

    const auto bold = textCharacterRangeInfo(outputPath, QStringLiteral("Tiếng"));
    const auto regular = textCharacterRangeInfo(outputPath, QStringLiteral("thường"));
    const auto secondLine = textCharacterRangeInfo(outputPath, QStringLiteral("Việt"));
    QCOMPARE(bold.size(), QStringLiteral("Tiếng").size());
    QCOMPARE(regular.size(), QStringLiteral("thường").size());
    QCOMPARE(secondLine.size(), QStringLiteral("Việt").size());
    for (const auto &characters : {bold, regular, secondLine}) {
        const bool expectedBold = characters.first().text == QStringLiteral("T");
        for (const TextCharacterInfo &character : characters) {
            QCOMPARE(character.font.embedded, 1);
            const QRawFont rawFont(character.fontData, 16.0, QFont::PreferNoHinting);
            QVERIFY(rawFont.isValid());
            QCOMPARE(rawFont.weight() >= QFont::Bold, expectedBold);
        }
    }
    for (const TextCharacterInfo &character : secondLine) {
        for (const TextCharacterInfo &firstLineCharacter : bold)
            QVERIFY(character.matrix.f < firstLineCharacter.matrix.f - 1.0F);
    }
}

void PDFEditControllerTest::cancelsMissingFontSaveWithoutLosingDrafts()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("font-consent-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("font-consent-target.pdf"));
    QVERIFY(createUninstalledSubsetPdf(source));
    QFile destination(target);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    const QByteArray original("keep existing destination");
    QCOMPARE(destination.write(original), original.size());
    destination.close();
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    const auto candidate = candidateForText(waitForCandidates(controller, 0), QStringLiteral("2017"));
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("2017 z"));
    QSignalSpy requested(&controller, &PDFEditController::fontDownloadRequested);
    QSignalSpy finished(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(target)));
    QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 10000);
    QVERIFY(controller.fontDownloadPending());
    QVERIFY(controller.saving());
    QVERIFY(!controller.downloadingFont());
    QCOMPARE(controller.missingFontFamily(), kUninstalledSubsetFamily);
    QCOMPARE(finished.size(), 0);
    QVERIFY(destination.open(QIODevice::ReadOnly));
    QCOMPARE(destination.readAll(), original);
    destination.close();
    controller.respondToFontDownload(false);
    QCOMPARE(finished.size(), 1);
    QCOMPARE(finished.first().first().toBool(), false);
    QVERIFY(!controller.saving());
    QVERIFY(!controller.fontDownloadPending());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(document.source().toLocalFile(), source);
    // A new attempt may ask again, but changing the document invalidates it.
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(target)));
    QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 10000);
    document.close();
    QVERIFY(!controller.fontDownloadPending());
    QVERIFY(!controller.saving());
    controller.respondToFontDownload(true);
    QCOMPARE(finished.size(), 2);
    QVERIFY(destination.open(QIODevice::ReadOnly));
    QCOMPARE(destination.readAll(), original);
}

void PDFEditControllerTest::rejectsUnsafeFontDownloads_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("font-not-found") << 0;
    QTest::newRow("network-error") << 1;
    QTest::newRow("redirect-outside-allowlist") << 2;
    QTest::newRow("oversized-response") << 3;
    QTest::newRow("multiple-language-subsets") << 4;
    QTest::newRow("untrusted-font-url") << 5;
    QTest::newRow("malformed-font") << 6;
    QTest::newRow("different-family") << 7;
    QTest::newRow("different-style") << 8;
    QTest::newRow("missing-required-glyph") << 9;
}

void PDFEditControllerTest::rejectsUnsafeFontDownloads()
{
    QFETCH(int, scenario);
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    FontNetwork network;
    const QString family = QStringLiteral("DlBad");
    network.responses = {{fontMetadata(family)}, {fullFontStylesheet}, {uninstalledSubsetFontData("DlBad")}};
    switch (scenario) {
    case 0: network.responses = {{QByteArrayLiteral("[]")}}; break;
    case 1: network.responses = {{{}, 503}}; break;
    case 2: network.responses = {{{}, 302, QUrl(QStringLiteral("https://example.com/font.ttf"))}}; break;
    case 3: network.responses = {{{}, 200, {}, 2 * 1024 * 1024}}; break;
    case 4: network.responses[1].body = fullFontStylesheet + fullFontStylesheet; break;
    case 5: network.responses[1].body = QByteArrayLiteral("src: url(https://example.com/font.ttf)"); break;
    case 6: network.responses[2].body = QByteArrayLiteral("not a font"); break;
    case 7: network.responses[2].body = uninstalledSubsetFontData("Wrong"); break;
    case 8: network.responses[0].body = fontMetadata(family, true); break;
    default: break;
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, scenario == 8, false, scenario == 9 ? 0x10fffd : uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!finished.first().at(0).toBool());
    QVERIFY(!finished.first().at(1).toString().isEmpty());
    QCOMPARE(network.requests.size(), scenario < 4 ? 1 : scenario < 6 ? 2 : 3);
    QVERIFY(!QFileInfo::exists(store.path));
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, false).isValid());
}

void PDFEditControllerTest::cancelsFontDownloadBeforeInstallation()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    FontNetwork network;
    network.responses = {{fontMetadata(QStringLiteral("DlCan"))}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(QStringLiteral("DlCan"), false, false, uint('z'));
    service.cancel();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(finished.size(), 0);
    QVERIFY(!QFileInfo::exists(store.path));
}

void PDFEditControllerTest::downloadsEveryAvailableFontStyle()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("DlAll");
    FontNetwork network;
    network.responses = {{fontMetadata(family, true)}};
    // Start with the face needed for the pending save; then fetch every other
    // advertised style, including faces that are not used by that document.
    const QList<int> order{3, 0, 1, 2};
    for (int style : order) {
        const QByteArray font = uninstalledSubsetFontData("DlAll", style & 2, style & 1);
        QVERIFY(!font.isEmpty());
        network.responses.append(FontResponse{fullFontStylesheet});
        network.responses.append(FontResponse{font});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, true, true, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(network.requests.size(), 9);
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.ttf")}, QDir::Files).size(), 4);
    for (int index = 0; index < order.size(); ++index) {
        const int style = order.at(index);
        QString face = family + ((style & 2) ? QStringLiteral(":700") : QStringLiteral(":400"));
        if (style & 1)
            face += QStringLiteral("italic");
        QCOMPARE(QUrlQuery(network.requests.at(1 + index * 2).url())
            .queryItemValue(QStringLiteral("family")), face);
        const auto installed = ApplicationFonts::downloadedFontFace(family, style & 2, style & 1);
        QVERIFY(installed.isValid());
        QCOMPARE(installed.data, uninstalledSubsetFontData("DlAll", style & 2, style & 1));
        QVERIFY(ApplicationFonts::isFontFaceSelectable(family, style & 2, style & 1));
    }
}

void PDFEditControllerTest::downloadsOnlyAdvertisedFontStyles()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("DlOne");
    const QByteArray font = uninstalledSubsetFontData("DlOne");
    FontNetwork network;
    network.responses = {{fontMetadata(family)}, {fullFontStylesheet}, {font}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(network.requests.size(), 3);
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.ttf")}, QDir::Files).size(), 1);
    QCOMPARE(ApplicationFonts::downloadedFontFace(family, false, false).data, font);
    for (int style = 1; style < 4; ++style)
        QVERIFY(!ApplicationFonts::downloadedFontFace(family, style & 2, style & 1).isValid());

    // A complete required face is sufficient: a family that only advertises
    // regular must remain usable offline without probing for three other faces.
    FontNetwork cachedNetwork;
    FontDownloadService cachedService(nullptr, &cachedNetwork);
    QSignalSpy cachedFinished(&cachedService, &FontDownloadService::finished);
    cachedService.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(cachedFinished.size(), 1, 5000);
    QVERIFY2(cachedFinished.first().first().toBool(),
             qPrintable(cachedFinished.first().at(1).toString()));
    QVERIFY(cachedNetwork.requests.isEmpty());
}

void PDFEditControllerTest::skipsCachedFontStylesWhenDownloadingFamily()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("DlCac");
    const QByteArray cached = uninstalledSubsetFontData("DlCac", true, true);
    QString error;
    QVERIFY2(ApplicationFonts::installDownloadedFont(cached, family, true, true, uint('z'), error),
             qPrintable(error));
    FontNetwork network;
    network.responses = {{fontMetadata(family, true)}};
    for (int style = 0; style < 3; ++style) {
        network.responses.append(FontResponse{fullFontStylesheet});
        network.responses.append(FontResponse{uninstalledSubsetFontData("DlCac", style & 2, style & 1)});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(network.requests.size(), 7);
    for (const auto &request : network.requests)
        QVERIFY(!QUrlQuery(request.url()).queryItemValue(QStringLiteral("family"))
            .endsWith(QStringLiteral(":700italic")));
    QCOMPARE(ApplicationFonts::downloadedFontFace(family, true, true).data, cached);
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.ttf")}, QDir::Files).size(), 4);
}

void PDFEditControllerTest::limitsCtanCatalogueToReviewedFamilies()
{
    CtanFontProvider provider;
    const QStringList families{QStringLiteral("TeX Gyre Termes"), QStringLiteral("TeX Gyre Heros"),
                               QStringLiteral("TeX Gyre Pagella"), QStringLiteral("TeX Gyre Schola"),
                               QStringLiteral("TeX Gyre Cursor")};
    const QStringList stems{QStringLiteral("termes"), QStringLiteral("heros"),
                            QStringLiteral("pagella"), QStringLiteral("schola"),
                            QStringLiteral("cursor")};
    const QStringList styles{QStringLiteral("regular"), QStringLiteral("italic"),
                             QStringLiteral("bold"), QStringLiteral("bolditalic")};
    for (int index = 0; index < families.size(); ++index) {
        const auto family = provider.resolve(families.at(index).toLower(), {});
        QCOMPARE(family.availability, IFontProvider::Availability::Found);
        QCOMPARE(family.name, families.at(index));
        QCOMPARE(family.license, QStringLiteral("GUST Font License 1.0 / LPPL-1.3c-or-later"));
        QCOMPARE(family.source, QUrl(QStringLiteral("https://ctan.org/pkg/tex-gyre")));
        QVERIFY(family.vietnamese);
        QVERIFY(provider.metadataUrl(family.name).isEmpty());
        QCOMPARE(provider.faceResource(), IFontProvider::Resource::Font);
        for (int style = 0; style < 4; ++style) {
            QVERIFY(family.styles[style]);
            const QUrl url = provider.faceUrl(family, style & 2, style & 1);
            QCOMPARE(url, QUrl(QStringLiteral(
                "https://mirror.ctan.org/fonts/tex-gyre/opentype/texgyre%1-%2.otf")
                .arg(stems.at(index), styles.at(style))));
            QVERIFY(provider.allowsUrl(url, IFontProvider::Resource::Font));
            QVERIFY(!provider.allowsUrl(url, IFontProvider::Resource::Metadata));
            const QList<QUrl> backups = provider.fallbackFaceUrls(family, style & 2, style & 1);
            QCOMPARE(backups.size(), 2);
            QCOMPARE(backups.at(0), QUrl(QStringLiteral("https://ftp.fau.de/ctan") + url.path()));
            QCOMPARE(backups.at(1), QUrl(QStringLiteral("https://ftp.jaist.ac.jp/pub/CTAN") + url.path()));
            for (const QUrl &backup : backups)
                QVERIFY(provider.allowsUrl(backup, IFontProvider::Resource::Font));
        }
    }
    for (const QString &name : {QStringLiteral("TrePDF Uncatalogued Font"),
                               QStringLiteral("TeX Gyre Termes Extra"),
                               QStringLiteral("texgyretermes"), QStringLiteral("Arial")}) {
        const auto family = provider.resolve(name, {});
        QCOMPARE(family.availability, IFontProvider::Availability::NotFound);
        QVERIFY(family.license.isEmpty());
        QVERIFY(provider.faceUrl(family, false, false).isEmpty());
        QVERIFY(provider.fallbackFaceUrls(family, false, false).isEmpty());
    }
}

void PDFEditControllerTest::loadsEveryCtanCatalogueEntry()
{
    QFile catalogue(QStringLiteral(":/trepdf/font-catalog/ctan-catalog.json"));
    QVERIFY(catalogue.open(QIODevice::ReadOnly));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(catalogue.readAll(), &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    const QJsonObject root = document.object();
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 1);
    const QJsonArray entries = root.value(QStringLiteral("families")).toArray();
    QVERIFY(entries.size() > 5);
    const QStringList styles{QStringLiteral("regular"), QStringLiteral("italic"),
                             QStringLiteral("bold"), QStringLiteral("bolditalic")};
    QSet<QString> familyNames;
    QSet<QString> fontPaths;
    CtanFontProvider provider;
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        QVERIFY(!name.isEmpty());
        QVERIFY2(!familyNames.contains(name.toCaseFolded()), qPrintable(name));
        familyNames.insert(name.toCaseFolded());
        const auto family = provider.resolve(QStringLiteral(" ") + name.toLower()
                                                 + QStringLiteral(" "), {});
        QCOMPARE(family.availability, IFontProvider::Availability::Found);
        QCOMPARE(family.name, name);
        QCOMPARE(family.license, entry.value(QStringLiteral("license")).toString());
        QVERIFY(!family.license.isEmpty());
        QCOMPARE(family.source, QUrl(entry.value(QStringLiteral("source")).toString()));
        QStringList subsets;
        for (const QJsonValue &subset : entry.value(QStringLiteral("subsets")).toArray())
            subsets.append(subset.toString());
        QCOMPARE(family.subsets, subsets.join(QLatin1Char(',')));
        QCOMPARE(family.vietnamese, subsets.contains(QStringLiteral("vietnamese")));
        const QJsonObject faces = entry.value(QStringLiteral("faces")).toObject();
        QVERIFY(!faces.isEmpty());
        for (int style = 0; style < styles.size(); ++style) {
            const QString path = faces.value(styles.at(style)).toString();
            QCOMPARE(family.styles[style], !path.isEmpty());
            const QUrl url = provider.faceUrl(family, style & 2, style & 1);
            const QList<QUrl> backups = provider.fallbackFaceUrls(family, style & 2, style & 1);
            if (path.isEmpty()) {
                QVERIFY(url.isEmpty());
                QVERIFY(backups.isEmpty());
                continue;
            }
            QVERIFY2(!fontPaths.contains(path), qPrintable(path));
            fontPaths.insert(path);
            QCOMPARE(url, QUrl(QStringLiteral("https://mirror.ctan.org") + path));
            QVERIFY(provider.allowsUrl(url, IFontProvider::Resource::Font));
            QVERIFY(!provider.allowsUrl(url, IFontProvider::Resource::Metadata));
            QVERIFY(!provider.allowsUrl(url, IFontProvider::Resource::Stylesheet));
            QCOMPARE(backups.size(), 2);
            for (const QUrl &backup : backups)
                QVERIFY(provider.allowsUrl(backup, IFontProvider::Resource::Font));
            const QUrl mirror(QStringLiteral("https://mirror.example.org/tex-archive") + path);
            QVERIFY(provider.allowsRedirect(url, mirror, IFontProvider::Resource::Font));
            QVERIFY(!provider.allowsUrl(mirror, IFontProvider::Resource::Font));
            QVERIFY(!provider.allowsRedirect(backups.first(), mirror, IFontProvider::Resource::Font));
            for (const QString &unsafe : {
                     QStringLiteral("http://mirror.ctan.org") + path,
                     QStringLiteral("https://mirror.ctan.org") + path + QStringLiteral("?download=1"),
                     QStringLiteral("https://mirror.ctan.org") + path + QStringLiteral("#font"),
                     QStringLiteral("https://mirror.ctan.org/../") + path.mid(1),
                     QStringLiteral("https://mirror.ctan.org/%2e%2e") + path,
                     QStringLiteral("https://127.0.0.1") + path}) {
                const QUrl unsafeUrl(unsafe);
                QVERIFY2(!provider.allowsUrl(unsafeUrl, IFontProvider::Resource::Font),
                         qPrintable(unsafe));
                QVERIFY2(!provider.allowsRedirect(url, unsafeUrl, IFontProvider::Resource::Font),
                         qPrintable(unsafe));
            }
        }
    }
    const auto inconsolata = provider.resolve(QStringLiteral("Inconsolatazi4"), {});
    QCOMPARE(inconsolata.availability, IFontProvider::Availability::Found);
    QVERIFY(inconsolata.styles[0]);
    QVERIFY(!inconsolata.styles[1]);
    QVERIFY(inconsolata.styles[2]);
    QVERIFY(!inconsolata.styles[3]);
    GoogleFontsProvider google;
    QCOMPARE(google.resolve(inconsolata.name, fontMetadata(QStringLiteral("Inconsolata")))
                 .availability, IFontProvider::Availability::NotFound);
}

void PDFEditControllerTest::validatesExternalCtanCatalogFaces()
{
    const QString cachePath = qEnvironmentVariable("TREPDF_TEST_CTAN_CACHE");
    if (cachePath.isEmpty())
        QSKIP("Set TREPDF_TEST_CTAN_CACHE to the generated CTAN catalogue's download cache.");
    QVERIFY(QFileInfo(cachePath).isDir());
    QFile catalogue(QStringLiteral(":/trepdf/font-catalog/ctan-catalog.json"));
    QVERIFY(catalogue.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(catalogue.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    const QJsonArray entries = document.object().value(QStringLiteral("families")).toArray();
    QVERIFY(!entries.isEmpty());
    const QDir files(cachePath);
    const QStringList styles{QStringLiteral("regular"), QStringLiteral("italic"),
                             QStringLiteral("bold"), QStringLiteral("bolditalic")};
    const QString accents = QStringLiteral(
        "\u00c0\u00c1\u00c2\u00c3\u00c8\u00c9\u00ca\u00cc\u00cd\u00d2\u00d3\u00d4\u00d5\u00d9\u00da\u00dd"
        "\u00e0\u00e1\u00e2\u00e3\u00e8\u00e9\u00ea\u00ec\u00ed\u00f2\u00f3\u00f4\u00f5\u00f9\u00fa\u00fd"
        "\u0102\u0103\u0110\u0111\u0128\u0129\u0168\u0169\u01a0\u01a1\u01af\u01b0");
    QStringList failures;
    int faceCount = 0;
    CtanFontProvider provider;
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        const auto family = provider.resolve(name, {});
        if (family.availability != IFontProvider::Availability::Found) {
            failures.append(name + QStringLiteral(": provider did not resolve catalogued family"));
            continue;
        }
        const QJsonObject faces = entry.value(QStringLiteral("faces")).toObject();
        for (int style = 0; style < styles.size(); ++style) {
            const QString path = faces.value(styles.at(style)).toString();
            if (path.isEmpty())
                continue;
            ++faceCount;
            const QString label = name + QLatin1Char('/') + styles.at(style);
            QStringList faceErrors;
            const auto record = qScopeGuard([&] {
                if (!faceErrors.isEmpty())
                    failures.append(label + QStringLiteral(": ") + faceErrors.join(QStringLiteral("; ")));
            });
            QFile fontFile(files.filePath(path.mid(1)));
            if (!fontFile.open(QIODevice::ReadOnly)) {
                faceErrors.append(QStringLiteral("cannot read cached ") + path);
                continue;
            }
            if (fontFile.size() <= 0 || fontFile.size() > 16 * 1024 * 1024) {
                faceErrors.append(QStringLiteral("cached font is empty or exceeds the 16 MiB limit"));
                continue;
            }
            const QByteArray bytes = fontFile.readAll();
            const QRawFont font(bytes, 16.0, QFont::PreferNoHinting);
            QString error;
            if (!ApplicationFonts::validateDownloadedFontData(
                    bytes, name, style & 2, style & 1, uint('z'), error)) {
                faceErrors.append(error + QStringLiteral(" [Qt family='%1', style='%2', weight=%3, italic=%4]")
                                             .arg(font.familyName(), font.styleName())
                                             .arg(int(font.weight())).arg(int(font.style())));
            }
            if (!font.isValid()) {
                faceErrors.append(QStringLiteral("invalid Qt raw font"));
                continue;
            }
            uint missing = 0;
            for (uint character = 32; character <= 126 && !missing; ++character) {
                if (!font.supportsCharacter(character))
                    missing = character;
            }
            if (family.vietnamese) {
                for (QChar character : accents) {
                    if (!missing && !font.supportsCharacter(character.unicode()))
                        missing = character.unicode();
                }
                for (uint character = 0x1ea0; character <= 0x1ef9 && !missing; ++character) {
                    if (!font.supportsCharacter(character))
                        missing = character;
                }
            }
            if (missing)
                faceErrors.append(QStringLiteral("missing advertised glyph U+")
                                      + QString::number(missing, 16).toUpper());
        }
    }
    qInfo().noquote() << QStringLiteral("CTAN validation: %1 families, %2 faces, %3 failures")
                            .arg(entries.size()).arg(faceCount).arg(failures.size());
    for (const QString &failure : failures)
        qWarning().noquote() << failure;
    QVERIFY2(failures.isEmpty(), qPrintable(failures.mid(0, 50).join(QLatin1Char('\n'))));
}

void PDFEditControllerTest::downloadsInconsolataCtanStyles()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("Inconsolatazi4");
    const QStringList filenames{QStringLiteral("Inconsolatazi4-Bold.otf"),
                                QStringLiteral("Inconsolatazi4-Regular.otf")};
    const QList<int> order{2, 0};
    const QList<QByteArray> digests{
        QByteArrayLiteral("515b21ca6b972d929b7362dda30548764f7541eb31cba0dabbb666433cd6552f"),
        QByteArrayLiteral("fa4d0728e461e228e4c3b706cc0836f72cb146852a757127c92647d8c405b638")};
    FontNetwork network;
    network.responses = {{QByteArrayLiteral("[]")}};
    QList<QByteArray> fonts;
    for (int index = 0; index < filenames.size(); ++index) {
        const QString path = QStringLiteral("assets/inconsolata/") + filenames.at(index);
        QFile file(QFINDTESTDATA(path));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.startsWith("OTTO"));
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                 digests.at(index));
        QString error;
        QVERIFY2(ApplicationFonts::validateDownloadedFontData(
                     bytes, family, order.at(index) & 2, false, uint('t'), error),
                 qPrintable(error));
        fonts.append(bytes);
        network.responses.append(FontResponse{bytes});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, true, false, uint('t'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(network.requests.size(), 3);
    QCOMPARE(network.requests.first().url().host(), QStringLiteral("api.fontsource.org"));
    for (int index = 0; index < filenames.size(); ++index) {
        QCOMPARE(network.requests.at(index + 1).url(),
                 QUrl(QStringLiteral("https://mirror.ctan.org/fonts/inconsolata/opentype/")
                      + filenames.at(index)));
        const auto installed = ApplicationFonts::downloadedFontFace(family, order.at(index) & 2, false);
        QVERIFY(installed.isValid());
        QCOMPARE(installed.data, fonts.at(index));
    }
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.otf")}, QDir::Files).size(), 2);
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, true).isValid());
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, true, true).isValid());

    FontNetwork cachedNetwork;
    FontDownloadService cachedService(nullptr, &cachedNetwork);
    QSignalSpy cachedFinished(&cachedService, &FontDownloadService::finished);
    cachedService.start(family, false, false, uint('t'));
    QTRY_COMPARE_WITH_TIMEOUT(cachedFinished.size(), 1, 5000);
    QVERIFY2(cachedFinished.first().first().toBool(),
             qPrintable(cachedFinished.first().at(1).toString()));
    QVERIFY(cachedNetwork.requests.isEmpty());

    FontNetwork italicNetwork;
    italicNetwork.responses = {{QByteArrayLiteral("[]")}};
    FontDownloadService italicService(nullptr, &italicNetwork);
    QSignalSpy italicFinished(&italicService, &FontDownloadService::finished);
    italicService.start(family, false, true, uint('t'));
    QTRY_COMPARE_WITH_TIMEOUT(italicFinished.size(), 1, 5000);
    QVERIFY(!italicFinished.first().first().toBool());
    QCOMPARE(italicNetwork.requests.size(), 1);

    const QString editedText = QStringLiteral("http://amyrbrown.ca tt");
    for (int style : order) {
        const QString source = m_temporaryDirectory.filePath(
            QStringLiteral("inconsolata-%1-source.pdf").arg(style));
        const QString output = m_temporaryDirectory.filePath(
            QStringLiteral("inconsolata-%1-output.pdf").arg(style));
        QVERIFY(createFontStylePdf(source));
        PDFDocument document;
        PDFEditController controller;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        controller.setDocument(&document);
        const auto candidate = candidateForText(waitForCandidates(controller, 0),
                                               QString::fromLatin1(kRegularText));
        QVERIFY(!candidate.isEmpty());
        QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
        controller.beginSelectedTextEdit();
        controller.updateSelectedTextPreview(editedText);
        controller.setSelectedTextRange(0, editedText.size());
        controller.setSelectedFontFamily(family);
        controller.setSelectedFontBold(style & 2);
        controller.finishSelectedTextEdit();
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
        QVERIFY2(normalizedPageText(output).contains(editedText), qPrintable(pageText(output)));
        const auto savedFont = textObjectFontInfo(output, QStringLiteral("http://"));
        QVERIFY(savedFont.found);
        QCOMPARE(savedFont.embedded, 1);
        QCOMPARE(compactFontName(savedFont.familyName), compactFontName(family));
        const auto reopened = candidateForText(waitForCandidates(controller, 0), editedText);
        QVERIFY(!reopened.isEmpty());
        QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
        QCOMPARE(controller.selectedFontFamily(), family);
        const QImage rendered = renderFirstPage(output, QSize(595, 842));
        QVERIFY(!rendered.isNull());
        const QRectF frame = controller.selectedFrameRect();
        const QRect region = QRectF(frame.x() * rendered.width(), frame.y() * rendered.height(),
                                     frame.width() * rendered.width(), frame.height() * rendered.height())
                                  .toAlignedRect().intersected(rendered.rect());
        QVERIFY(!region.isEmpty());
        qsizetype inkPixels = 0;
        for (int y = region.top(); y <= region.bottom(); ++y) {
            for (int x = region.left(); x <= region.right(); ++x) {
                if (qGray(rendered.pixel(x, y)) < 128)
                    ++inkPixels;
            }
        }
        QVERIFY2(inkPixels > 50, "Downloaded Inconsolata must render visible glyphs after Save/reopen.");
    }
}

void PDFEditControllerTest::limitsGoogleCatalogueToOpenSourceFonts()
{
    GoogleFontsProvider provider;
    const QString name = QStringLiteral("Test Font");
    const auto family = provider.resolve(name, fontMetadata(name));
    QCOMPARE(family.availability, IFontProvider::Availability::Found);
    QCOMPARE(family.license, QStringLiteral("Google Fonts open-source catalogue"));
    QCOMPARE(family.source, QUrl(QStringLiteral("https://fonts.google.com/")));
    for (const QString &type : {QStringLiteral("other"), QStringLiteral("commercial"), QString()}) {
        QJsonArray metadata = QJsonDocument::fromJson(fontMetadata(name)).array();
        QJsonObject entry = metadata.first().toObject();
        entry.insert(QStringLiteral("type"), type);
        metadata[0] = entry;
        const auto rejected = provider.resolve(name, QJsonDocument(metadata).toJson());
        QCOMPARE(rejected.availability, IFontProvider::Availability::NotFound);
        QVERIFY(rejected.license.isEmpty());
    }
}

void PDFEditControllerTest::fallsBackToCtanAndReusesCachedFamily()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("TeX Gyre Termes");
    const QStringList styles{QStringLiteral("regular"), QStringLiteral("italic"),
                             QStringLiteral("bold"), QStringLiteral("bolditalic")};
    const QList<int> order{3, 0, 1, 2};
    const QByteArray regularFont = ctanTermesFontData(0);
    QVERIFY(regularFont.startsWith("OTTO"));
    QCOMPARE(QCryptographicHash::hash(regularFont, QCryptographicHash::Sha256).toHex(),
             QByteArrayLiteral("cc3fe7c707b81428d23d54df3eadd9228a2bf6a4d43125d94df56f5f63134659"));
    // The reviewed GFL exception is for the exact upstream bytes. Changing a
    // harmless checksum word must not grant the same embedding exception.
    QByteArray modifiedFont = regularFont;
    bool changedChecksum = false;
    for (int table = 0; table < sfntUInt16(modifiedFont, 4); ++table) {
        const qsizetype entry = 12 + table * 16;
        if (modifiedFont.mid(entry, 4) == QByteArrayLiteral("head")) {
            const qsizetype checksum = sfntUInt32(modifiedFont, entry + 8) + 8;
            modifiedFont[checksum] = char(quint8(modifiedFont.at(checksum)) ^ 1U);
            changedChecksum = true;
            break;
        }
    }
    QVERIFY(changedChecksum);
    QVERIFY(QRawFont(modifiedFont, 16.0, QFont::PreferNoHinting).isValid());
    QString modifiedError;
    QVERIFY(!ApplicationFonts::validateDownloadedFontData(
        modifiedFont, family, false, false, uint('z'), modifiedError));
    QVERIFY(!modifiedError.isEmpty());
    FontNetwork network;
    network.responses = {{QByteArrayLiteral("[]")}};
    for (int style : order) {
        const QByteArray data = ctanTermesFontData(style);
        const QRawFont font(data, 16.0, QFont::PreferNoHinting);
        QVERIFY(font.isValid());
        QCOMPARE(font.familyName(), family);
        QString validationError;
        QVERIFY2(ApplicationFonts::validateDownloadedFontData(
                     data, family, style & 2, style & 1, uint('z'), validationError),
                 qPrintable(validationError));
        network.responses.append(FontResponse{data});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, true, true, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(network.requests.size(), 5);
    QCOMPARE(network.requests.first().url().host(), QStringLiteral("api.fontsource.org"));
    for (int index = 0; index < order.size(); ++index) {
        const int style = order.at(index);
        QCOMPARE(network.requests.at(index + 1).url(), QUrl(QStringLiteral(
            "https://mirror.ctan.org/fonts/tex-gyre/opentype/texgyretermes-%1.otf")
            .arg(styles.at(style))));
        const auto installed = ApplicationFonts::downloadedFontFace(family, style & 2, style & 1);
        QVERIFY(installed.isValid());
        QCOMPARE(installed.data, ctanTermesFontData(style));
        QVERIFY(ApplicationFonts::isFontFaceSelectable(family, style & 2, style & 1));
    }
    // Every style is the original reviewed OpenType/CFF program from CTAN.
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.ttf")}, QDir::Files).size(), 0);
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.otf")}, QDir::Files).size(), 4);
    FontNetwork cachedNetwork;
    FontDownloadService cachedService(nullptr, &cachedNetwork);
    QSignalSpy cachedFinished(&cachedService, &FontDownloadService::finished);
    cachedService.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(cachedFinished.size(), 1, 5000);
    QVERIFY(cachedFinished.first().first().toBool());
    QVERIFY(cachedNetwork.requests.isEmpty());

    // A separate process has no in-memory registry from this download. Its
    // startup must validate/register the persisted OTF and serve it offline.
    QProcess cacheProbe;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TREPDF_FONT_CACHE_PROBE_APP"),
                       QCoreApplication::applicationName());
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    cacheProbe.setProcessEnvironment(environment);
    cacheProbe.setProcessChannelMode(QProcess::MergedChannels);
    cacheProbe.start(QCoreApplication::applicationFilePath(),
                     {QStringLiteral("loadsCachedFontInFreshProcess")});
    QVERIFY2(cacheProbe.waitForStarted(10000), qPrintable(cacheProbe.errorString()));
    QVERIFY2(cacheProbe.waitForFinished(30000), qPrintable(cacheProbe.errorString()));
    const QByteArray probeOutput = cacheProbe.readAll();
    QVERIFY2(cacheProbe.exitStatus() == QProcess::NormalExit && cacheProbe.exitCode() == 0,
             probeOutput.constData());

    const QString source = m_temporaryDirectory.filePath(QStringLiteral("ctan-otf-source.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("ctan-otf-output.pdf"));
    const QString resaved = m_temporaryDirectory.filePath(QStringLiteral("ctan-otf-resaved.pdf"));
    const QString editedText = QStringLiteral("TeX Gyre — tiếng Việt 123456789");
    const QString secondText = QStringLiteral("TeX Gyre — tiếng Việt xyz");
    QVERIFY(ApplicationFonts::fontFaceSupportsText(family, false, false, editedText));
    QVERIFY(createFontStylePdf(source));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    const auto candidate = candidateForText(waitForCandidates(controller, 0),
                                           QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(editedText);
    controller.setSelectedTextRange(0, editedText.size());
    controller.setSelectedFontFamily(family);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.selectedFontFamily(), family);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    QVERIFY2(normalizedPageText(output).contains(editedText), qPrintable(pageText(output)));
    QFile savedPdf(output);
    QVERIFY(savedPdf.open(QIODevice::ReadOnly));
    const QByteArray savedBytes = savedPdf.readAll();
    QVERIFY(savedBytes.contains("/FontFile3"));
    QVERIFY(QRegularExpression(QStringLiteral("/Subtype\\s*/OpenType"))
                .match(QString::fromLatin1(savedBytes)).hasMatch());
    const auto savedFont = textObjectFontInfo(output, QStringLiteral("TeX"));
    QVERIFY(savedFont.found);
    QCOMPARE(savedFont.embedded, 1);
    QCOMPARE(compactFontName(savedFont.familyName), compactFontName(family));

    // Saving reopens the result; the embedded CFF face must remain editable and
    // the private display alias must never escape into user-visible families.
    const auto reopened = candidateForText(waitForCandidates(controller, 0), editedText);
    QVERIFY(!reopened.isEmpty());
    QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedFontFamily(), family);
    const QImage rendered = renderFirstPage(output, QSize(595, 842));
    QVERIFY(!rendered.isNull());
    const QRectF frame = controller.selectedFrameRect();
    const QRect textRegion = QRectF(frame.x() * rendered.width(), frame.y() * rendered.height(),
                                    frame.width() * rendered.width(), frame.height() * rendered.height())
                                 .toAlignedRect().intersected(rendered.rect());
    QVERIFY(!textRegion.isEmpty());
    qsizetype inkPixels = 0;
    for (int y = textRegion.top(); y <= textRegion.bottom(); ++y) {
        for (int x = textRegion.left(); x <= textRegion.right(); ++x) {
            if (qGray(rendered.pixel(x, y)) < 128)
                ++inkPixels;
        }
    }
    QVERIFY2(inkPixels > 50, "The embedded CFF text must render visible glyphs, not just extract text.");
    const QString cachedRegular = QDir(store.path).filePath(QString::fromLatin1(
        QCryptographicHash::hash(regularFont, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".otf"));
    QVERIFY(QFile::remove(cachedRegular));
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, false).isValid());
    controller.commitSelectedText(secondText);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(resaved)));
    QVERIFY2(normalizedPageText(resaved).contains(secondText),
             qPrintable(pageText(resaved)));
    QVERIFY(cachedNetwork.requests.isEmpty());

    const QString encrypted = m_temporaryDirectory.filePath(QStringLiteral("ctan-otf-protected.pdf"));
    const QString encryptedEdited = m_temporaryDirectory.filePath(QStringLiteral("ctan-otf-protected-edited.pdf"));
    const QString password = QStringLiteral("ctan-cff-save-password");
    QVERIFY(controller.setOpenPassword(password));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(encrypted)));
    QVERIFY(document.passwordProtected());
    const auto protectedCandidate = candidateForText(waitForCandidates(controller, 0),
                                                     secondText);
    QVERIFY(!protectedCandidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, protectedCandidate.value(QStringLiteral("objectId")).toString()));
    const QString protectedText = QStringLiteral("TeX Gyre — tiếng Việt Ω");
    controller.commitSelectedText(protectedText);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(encryptedEdited)));
    PDFDocument protectedReopened;
    QVERIFY(!protectedReopened.open(QUrl::fromLocalFile(encryptedEdited)));
    QVERIFY(protectedReopened.passwordRequired());
    QVERIFY(protectedReopened.open(QUrl::fromLocalFile(encryptedEdited), password));
    PDFEditController protectedController;
    protectedController.setDocument(&protectedReopened);
    const auto verified = candidateForText(waitForCandidates(protectedController, 0), protectedText);
    QVERIFY(!verified.isEmpty());
    QVERIFY(protectedController.selectCandidate(0, verified.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(protectedController.selectedFontFamily(), family);
}

void PDFEditControllerTest::loadsCachedFontInFreshProcess()
{
    if (!qEnvironmentVariableIsSet("TREPDF_FONT_CACHE_PROBE_APP"))
        QSKIP("The CTAN download test invokes this probe in a fresh process.");
    QVERIFY(QStandardPaths::isTestModeEnabled());
    QCOMPARE(QFileInfo(QFileInfo(ApplicationFonts::downloadedFontDirectory()).absolutePath()).fileName(),
             QCoreApplication::applicationName());
    const QString family = QStringLiteral("TeX Gyre Termes");
    const auto cached = ApplicationFonts::downloadedFontFace(family, false, false);
    QVERIFY(cached.isValid());
    QVERIFY(cached.data.startsWith("OTTO"));
    QCOMPARE(QCryptographicHash::hash(cached.data, QCryptographicHash::Sha256).toHex(),
             QByteArrayLiteral("cc3fe7c707b81428d23d54df3eadd9228a2bf6a4d43125d94df56f5f63134659"));
    QCOMPARE(QRawFont(cached.data, 16.0, QFont::PreferNoHinting).familyName(), family);
    QVERIFY(ApplicationFonts::fontFaceSupportsText(
        family, false, false, QStringLiteral("tiếng Việt z"), ApplicationFonts::FontSource::Downloaded));
    FontNetwork network;
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QCOMPARE(finished.size(), 1);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QVERIFY(network.requests.isEmpty());
}

void PDFEditControllerTest::preservesDownloadedFontSpacingOnReusedSaveWorker()
{
    // Font registrations and Qt's per-thread font caches live for the process
    // lifetime. Start with no cached Termes, independently of test order.
    if (!qEnvironmentVariableIsSet("TREPDF_FONT_WORKER_SPACING_PROBE")) {
        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("TREPDF_FONT_WORKER_SPACING_PROBE"), QStringLiteral("1"));
        environment.insert(QStringLiteral("TREPDF_FONT_CACHE_PROBE_APP"),
            QStringLiteral("TrePDF-font-test-")
                + QUuid::createUuid().toString(QUuid::WithoutBraces));
        probe.setProcessEnvironment(environment);
        probe.setProcessChannelMode(QProcess::MergedChannels);
        probe.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("preservesDownloadedFontSpacingOnReusedSaveWorker")});
        QVERIFY2(probe.waitForStarted(10000), qPrintable(probe.errorString()));
        QVERIFY2(probe.waitForFinished(30000), qPrintable(probe.errorString()));
        const QByteArray output = probe.readAll();
        QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                 output.constData());
        return;
    }
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("TeX Gyre Termes");
    QVERIFY(!QFontDatabase::families().contains(family));
    const auto faceFont = [&family](int style) {
        QFont font(family);
        font.setPixelSize(9);
        font.setBold(style & 2);
        font.setItalic(style & 1);
        font.setStretch(100);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
        font.setWordSpacing(0.0);
        return font;
    };
    // Match the application's save executor: a pooled thread with no event
    // loop, retained across all source-font unregister/re-register cycles.
    QThreadPool workerPool;
    workerPool.setMaxThreadCount(1);
    workerPool.setExpiryTimeout(-1);
    const auto runOnWorker = [&workerPool](const std::function<void()> &task) {
        QSemaphore finished;
        workerPool.start([&] {
            task();
            finished.release();
        });
        finished.acquire();
        return true;
    };
    bool warmedFallbacks = true;
    QVERIFY(runOnWorker([&] {
        for (int style = 0; style < 4; ++style) {
            const QFont font = faceFont(style);
            warmedFallbacks &= QFontInfo(font).family() != family;
            warmedFallbacks &= QFontMetricsF(font).horizontalAdvance(
                QStringLiteral("under the following conditions: ")) > 0.0;
        }
    }));
    QVERIFY(warmedFallbacks);

    const QStringList segments{QStringLiteral("under the following conditions: "),
                               QStringLiteral("slanted words "), QStringLiteral("bold words "),
                               QStringLiteral("both styles")};
    const QString text = segments.join(QString());
    PDFSaveWriter::Result result;
    const auto verifyWordOrigins = [&](const QString &path, const QString &expectedText,
                                       QTextDocument &editor, qreal frameOrigin) {
        editor.size();
        QCOMPARE(editor.begin().layout()->lineCount(), 1);
        const QTextLine editorLine = editor.begin().layout()->lineAt(0);
        PDFiumProcessLock lock;
        FPDF_DOCUMENT saved = FPDF_LoadDocument(path.toUtf8().constData(), nullptr);
        QVERIFY(saved);
        const auto closeDocument = qScopeGuard([saved] { FPDF_CloseDocument(saved); });
        FPDF_PAGE page = FPDF_LoadPage(saved, 0);
        QVERIFY(page);
        const auto closePage = qScopeGuard([page] { FPDF_ClosePage(page); });
        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        QVERIFY(textPage);
        const auto closeText = qScopeGuard([textPage] { FPDFText_ClosePage(textPage); });
        // PDFium can synthesize whitespace at a font-span boundary. Match the
        // actual characters, retaining their indices for the geometry check.
        QString extracted;
        QVector<int> characterIndices;
        for (int index = 0; index < FPDFText_CountChars(textPage); ++index) {
            const QChar character(static_cast<char16_t>(FPDFText_GetUnicode(textPage, index)));
            if (!character.isSpace()) {
                extracted.append(character);
                characterIndices.append(index);
            }
        }
        QString expectedCharacters;
        for (const QChar character : expectedText) {
            if (!character.isSpace())
                expectedCharacters.append(character);
        }
        const qsizetype offset = extracted.indexOf(expectedCharacters);
        QVERIFY2(offset >= 0, qPrintable(extracted));
        qsizetype characterOffset = 0;
        for (qsizetype index = 0; index < expectedText.size(); ++index) {
            if (expectedText.at(index).isSpace())
                continue;
            const int pdfIndex = characterIndices.at(offset + characterOffset++);
            if (index != 0 && !expectedText.at(index - 1).isSpace())
                continue;
            double x = 0.0;
            double y = 0.0;
            QVERIFY(FPDFText_GetCharOrigin(textPage, pdfIndex, &x, &y));
            const qreal expected = frameOrigin + editorLine.cursorToX(index);
            QVERIFY2(qAbs(x - expected) < 0.15,
                qPrintable(QStringLiteral("%1: word at %2: editor x=%3, saved x=%4")
                    .arg(QFileInfo(path).fileName()).arg(index).arg(expected).arg(x)));
        }
    };
    const auto repeatSaves = [&](const QString &savedPath) {
        PDFDocument reopened;
        PDFEditController restored;
        QVERIFY(reopened.open(QUrl::fromLocalFile(savedPath)));
        restored.setDocument(&reopened);
        const auto candidate = candidateForText(waitForCandidates(restored, 0), text);
        QVERIFY(!candidate.isEmpty());
        QVERIFY(restored.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
        QCOMPARE(restored.selectedFontFamily(), family);
        QCOMPARE(restored.selectedLetterSpacing(), 0.0);
        QCOMPARE(restored.selectedWordSpacing(), 0.0);
        restored.beginSelectedTextEdit();
        restored.finishSelectedTextEdit();
        QVERIFY(!restored.hasDrafts());

        // Reopening replaces the controller's application-font registrations. Save
        // on the same worker again: its source-alias metrics must still match the
        // GUI editor, both for existing glyphs and a new glyph from the full face.
        const QSizeF pageSize = reopened.pageSize(0);
        QTextDocument reference;
        reference.setDocumentMargin(0.0);
        QTextOption option = reference.defaultTextOption();
        option.setUseDesignMetrics(true);
        reference.setDefaultTextOption(option);
        QString editedText = text;
        const QStringList suffixes{QStringLiteral(" words"), QStringLiteral(" z"),
                                   QStringLiteral(" words"), QStringLiteral(" y")};
        for (int round = 0; round < suffixes.size(); ++round) {
            const auto sourceCandidate = candidateForText(waitForCandidates(restored, 0), editedText);
            QVERIFY(!sourceCandidate.isEmpty());
            QVERIFY(restored.selectCandidate(
                0, sourceCandidate.value(QStringLiteral("objectId")).toString()));
            const QString suffix = suffixes.at(round);
            editedText += suffix;
            restored.beginSelectedTextEdit();
            restored.updateSelectedTextPreview(editedText);
            if (round % 2 == 0) {
                // Blur before Save commits the draft and leaves its correct GUI
                // preview visible; it must not change the worker's saved spacing.
                restored.finishSelectedTextEdit();
                QVERIFY(restored.hasDrafts());
            }
            // Save also commits an editor that is still active.
            restored.finishSelectedTextEdit();
            // Rebuild from the current editor spans: reloading releases the old
            // private families, and this round must measure the newly registered
            // source faces rather than retain the previous GUI font engine.
            reference.setPlainText(editedText);
            reference.setTextWidth(restored.selectedFrameRect().width() * pageSize.width());
            for (const QVariant &value : restored.selectedCharacterSpans()) {
                const QVariantMap span = value.toMap();
                QFont font(span.value(QStringLiteral("fontFamily")).toString());
                font.setPixelSize(qRound(span.value(QStringLiteral("fontSizeRatio")).toDouble()
                                        * pageSize.height()));
                font.setBold(span.value(QStringLiteral("fontBold")).toBool());
                font.setItalic(span.value(QStringLiteral("fontItalic")).toBool());
                font.setStretch(span.value(QStringLiteral("fontStretch"), 100).toInt());
                font.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
                font.setWordSpacing(0.0);
                QTextCursor cursor(&reference);
                cursor.setPosition(span.value(QStringLiteral("start")).toInt());
                cursor.setPosition(cursor.position() + span.value(QStringLiteral("length")).toInt(),
                                   QTextCursor::KeepAnchor);
                QTextCharFormat format;
                format.setFont(font);
                cursor.setCharFormat(format);
            }
            const qreal restoredOrigin = restored.selectedFrameRect().x() * pageSize.width();
            const QString resaved = m_temporaryDirectory.filePath(
                QStringLiteral("font-worker-resaved-%1.pdf").arg(round));
            const auto repeatedRequest = PDFSaveWriter::prepare(restored, resaved);
            QVERIFY(repeatedRequest.isValid());
            QVERIFY(runOnWorker([&] {
                result = PDFSaveWriter::save(repeatedRequest);
            }));
            QVERIFY2(result.succeeded(), qPrintable(result.detail));
            verifyWordOrigins(resaved, editedText, reference, restoredOrigin);
            QVERIFY(!QTest::currentTestFailed());
            QVERIFY(reopened.open(QUrl::fromLocalFile(resaved)));
        }
    };

    const QString embeddedSource = qEnvironmentVariable("TREPDF_FONT_SOURCE_SPACING_PROBE");
    if (!embeddedSource.isEmpty()) {
        QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, false).isValid());
        repeatSaves(embeddedSource);
        return;
    }

    QList<ApplicationFonts::DownloadedFontData> faces;
    for (int style = 0; style < 4; ++style)
        faces.append({ctanTermesFontData(style), family, bool(style & 2), bool(style & 1), uint('z')});
    QString error;
    QVERIFY2(ApplicationFonts::installDownloadedFonts(faces, error), qPrintable(error));

    const QString source = m_temporaryDirectory.filePath(QStringLiteral("font-worker-source.pdf"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("font-worker-saved.pdf"));
    QVERIFY(createFontStylePdf(source));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(!controller.addText(0, 0.1, 0.65).isEmpty());
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(text);
    controller.setSelectedTextRange(0, text.size());
    controller.setSelectedFontFamily(family);
    controller.setSelectedFontSize(9);
    controller.setSelectedFontBold(false);
    controller.setSelectedFontItalic(false);
    qsizetype start = 0;
    for (int style = 0; style < 4; ++style) {
        controller.setSelectedTextRange(start, segments.at(style).size());
        controller.setSelectedFontBold(style & 2);
        controller.setSelectedFontItalic(style & 1);
        start += segments.at(style).size();
    }
    const QSizeF pageSize = document.pageSize(0);
    QRectF frame = controller.selectedFrameRect();
    frame.setWidth(0.85);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(frame);
    controller.finishSelectedFrameResize();
    controller.finishSelectedTextEdit();
    const qreal origin = controller.selectedFrameRect().x() * pageSize.width();

    // QTextDocument is the inline editor's layout engine. Its GUI-thread
    // font lookup sees the newly installed face; the old worker must too.
    QTextDocument reference;
    reference.setDocumentMargin(0.0);
    reference.setDefaultFont(faceFont(0));
    reference.setPlainText(text);
    reference.setTextWidth(0.85 * pageSize.width());
    QTextOption option = reference.defaultTextOption();
    option.setUseDesignMetrics(true);
    reference.setDefaultTextOption(option);
    start = 0;
    for (int style = 0; style < 4; ++style) {
        QCOMPARE(QFontInfo(faceFont(style)).family(), family);
        QTextCursor cursor(&reference);
        cursor.setPosition(start);
        start += segments.at(style).size();
        cursor.setPosition(start, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setFont(faceFont(style));
        cursor.setCharFormat(format);
    }
    reference.size();
    QCOMPARE(reference.begin().layout()->lineCount(), 1);
    const auto request = PDFSaveWriter::prepare(controller, output);
    QVERIFY(request.isValid());
    QVERIFY(runOnWorker([&] {
        result = PDFSaveWriter::save(request);
    }));
    QVERIFY2(result.succeeded(), qPrintable(result.detail));

    verifyWordOrigins(output, text, reference, origin);
    QVERIFY(!QTest::currentTestFailed());
    repeatSaves(output);
    QVERIFY(!QTest::currentTestFailed());

    // Open the same fully embedded fixture in a clean process without the
    // downloaded font cache. The source controller is then the sole owner of
    // the private Qt registrations, so every Save/reopen retires those names.
    QProcess sourceProbe;
    auto sourceEnvironment = QProcessEnvironment::systemEnvironment();
    sourceEnvironment.insert(QStringLiteral("TREPDF_FONT_SOURCE_SPACING_PROBE"), output);
    sourceEnvironment.insert(QStringLiteral("TREPDF_FONT_CACHE_PROBE_APP"),
        QStringLiteral("TrePDF-font-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    sourceProbe.setProcessEnvironment(sourceEnvironment);
    sourceProbe.setProcessChannelMode(QProcess::MergedChannels);
    sourceProbe.start(QCoreApplication::applicationFilePath(),
                     {QStringLiteral("preservesDownloadedFontSpacingOnReusedSaveWorker")});
    QVERIFY2(sourceProbe.waitForStarted(10000), qPrintable(sourceProbe.errorString()));
    QVERIFY2(sourceProbe.waitForFinished(30000), qPrintable(sourceProbe.errorString()));
    const QByteArray sourceOutput = sourceProbe.readAll();
    QVERIFY2(sourceProbe.exitStatus() == QProcess::NormalExit && sourceProbe.exitCode() == 0,
             sourceOutput.constData());
}

void PDFEditControllerTest::prefersGoogleForCtanFamily()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("TeX Gyre Heros");
    const QByteArray font = renamedBundledFontData(family);
    QVERIFY(!font.isEmpty());
    FontNetwork network;
    network.responses = {{fontMetadata(family)}, {fullFontStylesheet}, {font}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(network.requests.size(), 3);
    QCOMPARE(network.requests.at(0).url().host(), QStringLiteral("api.fontsource.org"));
    QCOMPARE(network.requests.at(1).url().host(), QStringLiteral("fonts.googleapis.com"));
    QCOMPARE(network.requests.at(2).url().host(), QStringLiteral("fonts.gstatic.com"));
    QCOMPARE(ApplicationFonts::downloadedFontFace(family, false, false).data, font);
}

void PDFEditControllerTest::doesNotProbeCtanForUnreviewedFamily()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    FontNetwork network;
    network.responses = {{QByteArrayLiteral("[]")}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(QStringLiteral("TrePDF Uncatalogued Font"), false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(!finished.first().at(1).toString().isEmpty());
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(network.requests.first().url().host(), QStringLiteral("api.fontsource.org"));
    QVERIFY(!QFileInfo::exists(store.path));
}

void PDFEditControllerTest::doesNotFallBackToCtanAfterGoogleFailure_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("http-error") << 0;
    QTest::newRow("malformed-json") << 1;
    QTest::newRow("wrong-json-type") << 2;
}

void PDFEditControllerTest::doesNotFallBackToCtanAfterGoogleFailure()
{
    QFETCH(int, scenario);
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    FontNetwork network;
    if (scenario == 0)
        network.responses = {{{}, 503}};
    else if (scenario == 1)
        network.responses = {{QByteArrayLiteral("{broken")}};
    else
        network.responses = {{QByteArrayLiteral("{}")}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(QStringLiteral("TeX Gyre Schola"), false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!finished.first().first().toBool());
    QCOMPARE(network.requests.size(), 1);
    QVERIFY(!QFileInfo::exists(store.path));
}

void PDFEditControllerTest::followsCtanDirectorToMirror()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("TeX Gyre Cursor");
    const QStringList styles{QStringLiteral("regular"), QStringLiteral("italic"),
                             QStringLiteral("bold"), QStringLiteral("bolditalic")};
    FontNetwork network;
    network.responses = {{QByteArrayLiteral("[]")}};
    for (int style = 0; style < 4; ++style) {
        const QUrl mirror(QStringLiteral(
            "https://ctan.example.net/tex-archive/fonts/tex-gyre/opentype/texgyrecursor-%1.otf")
            .arg(styles.at(style)));
        network.responses.append(FontResponse{{}, 302, mirror});
        network.responses.append(FontResponse{renamedBundledFontData(family, style & 2, style & 1)});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(network.requests.size(), 9);
    for (int style = 0; style < 4; ++style) {
        QCOMPARE(network.requests.at(1 + style * 2).url().host(), QStringLiteral("mirror.ctan.org"));
        QCOMPARE(network.requests.at(2 + style * 2).url().host(), QStringLiteral("ctan.example.net"));
        QCOMPARE(ApplicationFonts::downloadedFontFace(family, style & 2, style & 1).data,
                 renamedBundledFontData(family, style & 2, style & 1));
    }
}

void PDFEditControllerTest::retriesCtanFromReviewedMirrors_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<bool>("transportError");
    QTest::addColumn<int>("backupIndex");
    QTest::addColumn<bool>("directorRedirect");
    QTest::newRow("forbidden-after-director-redirect") << 403 << false << 1 << true;
    QTest::newRow("not-found") << 404 << false << 1 << false;
    QTest::newRow("request-timeout") << 408 << false << 1 << false;
    QTest::newRow("too-many-requests") << 429 << false << 1 << false;
    QTest::newRow("server-error") << 500 << false << 1 << false;
    QTest::newRow("unavailable") << 503 << false << 1 << false;
    QTest::newRow("transport-error") << 0 << true << 1 << false;
    QTest::newRow("transport-error-after-headers") << 200 << true << 1 << false;
    QTest::newRow("second-backup") << 403 << false << 2 << false;
}

void PDFEditControllerTest::retriesCtanFromReviewedMirrors()
{
    // Every row installs the same immutable upstream family. Qt and the app
    // retain registrations for the process lifetime, whereas IsolatedFontStore
    // removes its files after each test. Use a fresh process so rows cannot
    // inherit a registration pointing into an earlier test's deleted cache.
    if (!qEnvironmentVariableIsSet("TREPDF_CTAN_MIRROR_PROBE")) {
        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("TREPDF_CTAN_MIRROR_PROBE"), QStringLiteral("1"));
        probe.setProcessEnvironment(environment);
        probe.setProcessChannelMode(QProcess::MergedChannels);
        probe.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("retriesCtanFromReviewedMirrors:")
                        + QString::fromLatin1(QTest::currentDataTag())});
        QVERIFY2(probe.waitForStarted(10000), qPrintable(probe.errorString()));
        QVERIFY2(probe.waitForFinished(30000), qPrintable(probe.errorString()));
        const QByteArray output = probe.readAll();
        QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                 output.constData());
        return;
    }
    QFETCH(int, status);
    QFETCH(bool, transportError);
    QFETCH(int, backupIndex);
    QFETCH(bool, directorRedirect);
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("TeX Gyre Termes");
    const QStringList styles{QStringLiteral("regular"), QStringLiteral("italic"),
                             QStringLiteral("bold"), QStringLiteral("bolditalic")};
    const QList<int> order{3, 0, 1, 2};
    FontNetwork network;
    network.responses = {{QByteArrayLiteral("[]")}};
    const QUrl directedMirror(QStringLiteral(
        "https://sg.mirrors.cicku.me/ctan/fonts/tex-gyre/opentype/texgyretermes-bolditalic.otf"));
    if (directorRedirect)
        network.responses.append(FontResponse{{}, 307, directedMirror});
    network.responses.append(FontResponse{{}, status, {}, -1, 0, transportError
        ? QNetworkReply::RemoteHostClosedError : QNetworkReply::NoError});
    if (backupIndex == 2)
        network.responses.append(FontResponse{{}, 503});
    for (int style : order) {
        const QByteArray font = ctanTermesFontData(style);
        QVERIFY(!font.isEmpty());
        network.responses.append(FontResponse{font});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, true, true, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    const int redirectCount = directorRedirect ? 1 : 0;
    QCOMPARE(network.requests.size(), 5 + backupIndex + redirectCount);
    QCOMPARE(network.requests.at(0).url().host(), QStringLiteral("api.fontsource.org"));
    QCOMPARE(network.requests.at(1).url(), QUrl(QStringLiteral(
        "https://mirror.ctan.org/fonts/tex-gyre/opentype/texgyretermes-bolditalic.otf")));
    if (directorRedirect)
        QCOMPARE(network.requests.at(2).url(), directedMirror);
    if (backupIndex == 2)
        QCOMPARE(network.requests.at(2 + redirectCount).url().host(), QStringLiteral("ftp.fau.de"));
    const QString mirror = backupIndex == 1 ? QStringLiteral("https://ftp.fau.de/ctan")
                                            : QStringLiteral("https://ftp.jaist.ac.jp/pub/CTAN");
    for (int index = 0; index < order.size(); ++index) {
        const int style = order.at(index);
        // Once a backup succeeds, remaining styles avoid the failed director.
        QCOMPARE(network.requests.at(index + 1 + backupIndex + redirectCount).url(), QUrl(mirror + QStringLiteral(
            "/fonts/tex-gyre/opentype/texgyretermes-%1.otf")
            .arg(styles.at(style))));
        QCOMPARE(ApplicationFonts::downloadedFontFace(family, style & 2, style & 1).data,
                 ctanTermesFontData(style));
    }
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.otf")}, QDir::Files).size(), 4);
    FontNetwork cachedNetwork;
    FontDownloadService cachedService(nullptr, &cachedNetwork);
    QSignalSpy cachedFinished(&cachedService, &FontDownloadService::finished);
    for (int style = 0; style < 4; ++style) {
        cachedService.start(family, style & 2, style & 1, uint('z'));
        QCOMPARE(cachedFinished.size(), style + 1);
        QVERIFY(cachedFinished.last().first().toBool());
    }
    QVERIFY(cachedNetwork.requests.isEmpty());
}

void PDFEditControllerTest::keepsCtanFamilyUninstalledAfterMirrorExhaustion()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("TeX Gyre Termes");
    const QByteArray regular = ctanTermesFontData(0);
    QVERIFY(!regular.isEmpty());
    FontNetwork network;
    // The first face is staged successfully; the next fails on all three sources.
    network.responses = {{QByteArrayLiteral("[]")}, {regular},
                         {{}, 403}, {{}, 503}, {{}, 404}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(!finished.first().at(1).toString().isEmpty());
    QCOMPARE(network.requests.size(), 5);
    QCOMPARE(network.requests.at(2).url().host(), QStringLiteral("mirror.ctan.org"));
    QCOMPARE(network.requests.at(3).url().host(), QStringLiteral("ftp.fau.de"));
    QCOMPARE(network.requests.at(4).url().host(), QStringLiteral("ftp.jaist.ac.jp"));
    QVERIFY(!QFileInfo::exists(store.path));
    for (int style = 0; style < 4; ++style)
        QVERIFY(!ApplicationFonts::downloadedFontFace(family, style & 2, style & 1).isValid());
}

void PDFEditControllerTest::cancelsCtanMirrorRetry()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("TeX Gyre Termes");
    const QByteArray regular = ctanTermesFontData(0);
    QVERIFY(!regular.isEmpty());
    FontNetwork network;
    network.responses = {{QByteArrayLiteral("[]")}, {regular},
                         {{}, 403}, {{}, 503, {}, -1, 250}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 4, 5000);
    QCOMPARE(network.requests.last().url().host(), QStringLiteral("ftp.fau.de"));
    QVERIFY(!QFileInfo::exists(store.path));
    service.cancel();
    QTest::qWait(300);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(network.requests.size(), 4);
    QCOMPARE(finished.size(), 0);
    QVERIFY(!QFileInfo::exists(store.path));
    for (int style = 0; style < 4; ++style)
        QVERIFY(!ApplicationFonts::downloadedFontFace(family, style & 2, style & 1).isValid());
}

void PDFEditControllerTest::rejectsCrossHostRedirectFromCtanBackup()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("TeX Gyre Termes");
    FontNetwork network;
    network.responses = {{QByteArrayLiteral("[]")}, {{}, 403},
        {{}, 302, QUrl(QStringLiteral(
            "https://untrusted.example.net/ctan/fonts/tex-gyre/opentype/texgyretermes-regular.otf"))}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(!finished.first().at(1).toString().isEmpty());
    QCOMPARE(network.requests.size(), 3);
    QCOMPARE(network.requests.last().url().host(), QStringLiteral("ftp.fau.de"));
    QVERIFY(!QFileInfo::exists(store.path));
    for (int style = 0; style < 4; ++style)
        QVERIFY(!ApplicationFonts::downloadedFontFace(family, style & 2, style & 1).isValid());
}

void PDFEditControllerTest::doesNotRetryCtanClientErrors_data()
{
    QTest::addColumn<int>("status");
    QTest::newRow("bad-request") << 400;
    QTest::newRow("unauthorized") << 401;
    QTest::newRow("method-not-allowed") << 405;
}

void PDFEditControllerTest::doesNotRetryCtanClientErrors()
{
    QFETCH(int, status);
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    FontNetwork network;
    network.responses = {{QByteArrayLiteral("[]")}, {{}, status}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(QStringLiteral("TeX Gyre Termes"), false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!finished.first().first().toBool());
    QCOMPARE(network.requests.size(), 2);
    QVERIFY(!QFileInfo::exists(store.path));
}

void PDFEditControllerTest::rejectsUnsafeCtanDownloads_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("http-redirect") << 0;
    QTest::newRow("redirect-credentials") << 1;
    QTest::newRow("redirect-query") << 2;
    QTest::newRow("redirect-fragment") << 3;
    QTest::newRow("redirect-port") << 4;
    QTest::newRow("redirect-outside-package") << 5;
    QTest::newRow("redirect-other-family") << 6;
    QTest::newRow("redirect-other-style") << 7;
    QTest::newRow("redirect-second-mirror") << 8;
    QTest::newRow("malformed-font") << 9;
    QTest::newRow("wrong-font-family") << 10;
    QTest::newRow("wrong-font-style") << 11;
    QTest::newRow("redirect-loopback-address") << 12;
    QTest::newRow("redirect-local-host") << 13;
    QTest::newRow("redirect-encoded-path") << 14;
    QTest::newRow("redirect-path-traversal") << 15;
}

void PDFEditControllerTest::rejectsUnsafeCtanDownloads()
{
    QFETCH(int, scenario);
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("TeX Gyre Pagella");
    QUrl mirror(QStringLiteral(
        "https://ctan.example.net/tex-archive/fonts/tex-gyre/opentype/texgyrepagella-regular.otf"));
    FontNetwork network;
    network.responses = {{QByteArrayLiteral("[]")}};
    switch (scenario) {
    case 0: mirror.setScheme(QStringLiteral("http")); break;
    case 1: mirror.setUserName(QStringLiteral("user")); break;
    case 2: mirror.setQuery(QStringLiteral("download=1")); break;
    case 3: mirror.setFragment(QStringLiteral("font")); break;
    case 4: mirror.setPort(443); break;
    case 5: mirror.setPath(QStringLiteral("/uploads/texgyrepagella-regular.otf")); break;
    case 6: mirror.setPath(QStringLiteral("/fonts/tex-gyre/opentype/texgyretermes-regular.otf")); break;
    case 7: mirror.setPath(QStringLiteral("/fonts/tex-gyre/opentype/texgyrepagella-bold.otf")); break;
    case 12: mirror.setHost(QStringLiteral("127.0.0.1")); break;
    case 13: mirror.setHost(QStringLiteral("fonts.local")); break;
    case 14: mirror = QUrl(QStringLiteral("https://ctan.example.net/%2f/fonts/tex-gyre/opentype/texgyrepagella-regular.otf")); break;
    case 15: mirror.setPath(QStringLiteral("/../fonts/tex-gyre/opentype/texgyrepagella-regular.otf")); break;
    default: break;
    }
    if (scenario < 9 || scenario >= 12) {
        network.responses.append(FontResponse{{}, 302, mirror});
        if (scenario == 8) {
            mirror.setHost(QStringLiteral("another.example.net"));
            network.responses.append(FontResponse{{}, 302, mirror});
        }
    } else if (scenario == 9) {
        network.responses.append(FontResponse{QByteArrayLiteral("not a font")});
    } else if (scenario == 10) {
        network.responses.append(FontResponse{renamedBundledFontData(QStringLiteral("Different Font"))});
    } else {
        network.responses.append(FontResponse{renamedBundledFontData(family, true, false)});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(!finished.first().at(1).toString().isEmpty());
    QCOMPARE(network.requests.size(), scenario == 8 ? 3 : 2);
    QVERIFY(!QFileInfo::exists(store.path));
    for (int style = 0; style < 4; ++style)
        QVERIFY(!ApplicationFonts::downloadedFontFace(family, style & 2, style & 1).isValid());
}

void PDFEditControllerTest::keepsFontFamilyUninstalledWhenOptionalDownloadFails_data()
{
    QTest::addColumn<bool>("invalidFont");
    QTest::newRow("optional-network-error") << false;
    QTest::newRow("optional-wrong-family") << true;
}

void PDFEditControllerTest::keepsFontFamilyUninstalledWhenOptionalDownloadFails()
{
    QFETCH(bool, invalidFont);
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("DlFai");
    FontNetwork network;
    network.responses = {{fontMetadata(family, true)}, {fullFontStylesheet},
                         {uninstalledSubsetFontData("DlFai")}};
    if (invalidFont) {
        network.responses.append(FontResponse{fullFontStylesheet});
        network.responses.append(FontResponse{uninstalledSubsetFontData("Wrong", false, true)});
    } else {
        network.responses.append(FontResponse{{}, 503});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(!finished.first().at(1).toString().isEmpty());
    QCOMPARE(network.requests.size(), invalidFont ? 5 : 4);
    QVERIFY(!QFileInfo::exists(store.path));
    for (int style = 0; style < 4; ++style)
        QVERIFY(!ApplicationFonts::downloadedFontFace(family, style & 2, style & 1).isValid());
}

void PDFEditControllerTest::cancelsFontFamilyDownloadAfterFirstFace()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("DlPar");
    FontNetwork network;
    network.responses = {{fontMetadata(family, true)}, {fullFontStylesheet},
                         {uninstalledSubsetFontData("DlPar")},
                         {fullFontStylesheet, 200, {}, -1, 200}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 4, 5000);
    // The required regular face has arrived, but it must remain staged until
    // the rest of the family is ready. Cancellation drops those staged bytes.
    QVERIFY(!QFileInfo::exists(store.path));
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, false).isValid());
    service.cancel();
    QTest::qWait(250);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(network.requests.size(), 4);
    QCOMPARE(finished.size(), 0);
    QVERIFY(!QFileInfo::exists(store.path));
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, false).isValid());
}

void PDFEditControllerTest::rejectsInvalidAptosFontArchives_data()
{
    QTest::addColumn<int>("scenario");
    QTest::newRow("optional-face-has-wrong-style") << 0;
    QTest::newRow("duplicate-face-entry") << 1;
    QTest::newRow("required-face-is-missing") << 2;
}

void PDFEditControllerTest::rejectsInvalidAptosFontArchives()
{
    QFETCH(int, scenario);
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    auto entries = aptosArchiveEntries();
    if (scenario == 0)
        entries[1].second = entries.first().second;
    else if (scenario == 1)
        entries.append(entries.at(1));
    else
        entries.removeAt(3);
    const QByteArray archive = fontArchive(entries);
    QVERIFY(!archive.isEmpty());
    FontNetwork network;
    network.responses = {{archive}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(QStringLiteral("Aptos"), true, true, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(!finished.first().at(1).toString().isEmpty());
    QCOMPARE(network.requests.size(), 1);
    QVERIFY(!QFileInfo::exists(store.path));
    for (int style = 0; style < 4; ++style)
        QVERIFY(!ApplicationFonts::downloadedFontFace(QStringLiteral("Aptos"), style & 2, style & 1).isValid());
}

void PDFEditControllerTest::downloadsAllAptosStylesFromSingleArchive()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("Aptos");
    const auto entries = aptosArchiveEntries();
    const QByteArray archive = fontArchive(entries);
    QVERIFY(!archive.isEmpty());
    FontNetwork network;
    network.responses = {{archive}};
    FontDownloadService service(nullptr, &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, true, true, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(network.requests.first().url().host(), QStringLiteral("download.microsoft.com"));
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.ttf")}, QDir::Files).size(), 4);
    QVERIFY(!QFileInfo::exists(QDir(store.path).filePath(QStringLiteral("notes"))));
    for (int style = 0; style < 4; ++style) {
        const auto installed = ApplicationFonts::downloadedFontFace(family, style & 2, style & 1);
        QVERIFY(installed.isValid());
        QCOMPARE(installed.data, entries.at(style).second);
    }
}

void PDFEditControllerTest::downloadsPrivateFontAndResumesOriginalSave()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("DlRet");
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("font-retry-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("font-retry-target.pdf"));
    QVERIFY(createUninstalledSubsetPdf(source, QStringLiteral("2017"), QStringLiteral("a "), "DlRet"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    const auto candidate = candidateForText(waitForCandidates(controller, 0), QStringLiteral("2017"));
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("2017 z"));
    QSignalSpy requested(&controller, &PDFEditController::fontDownloadRequested);
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(target)));
    QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 10000);
    FontNetwork network;
    const QByteArray font = uninstalledSubsetFontData("DlRet");
    network.responses = {{fontMetadata(family, true)}};
    for (int style = 0; style < 4; ++style) {
        network.responses.append(FontResponse{fullFontStylesheet});
        network.responses.append(FontResponse{uninstalledSubsetFontData("DlRet", style & 2, style & 1)});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy downloaded(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('z'));
    QTRY_COMPARE_WITH_TIMEOUT(downloaded.size(), 1, 5000);
    QVERIFY2(downloaded.first().first().toBool(), qPrintable(downloaded.first().at(1).toString()));
    QCOMPARE(network.requests.size(), 9);
    for (const auto &request : network.requests) {
        QVERIFY(!request.url().toString().contains(source));
        QVERIFY(!QUrlQuery(request.url()).hasQueryItem(QStringLiteral("text")));
    }
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.ttf")}, QDir::Files).size(), 4);
    QCOMPARE(ApplicationFonts::downloadedFontFace(family, false, false).data, font);
    QVERIFY(ApplicationFonts::isFontFaceSelectable(family, false, false));
    for (int style = 0; style < 4; ++style)
        QVERIFY(ApplicationFonts::downloadedFontFace(family, style & 2, style & 1).isValid());
    QVERIFY(!ApplicationFonts::systemFontFamilies().contains(family));
    controller.respondToFontDownload(true);
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 15000);
    QVERIFY(saved.first().first().toBool());
    QCOMPARE(requested.size(), 1);
    QVERIFY(!controller.saving());
    QVERIFY(!controller.fontDownloadPending());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.source().toLocalFile(), target);
    QVERIFY(normalizedPageText(target).contains(QStringLiteral("2017 z")));
    QVERIFY(!pageText(source).contains(QStringLiteral("2017 z")));
    const auto savedFont = textObjectFontInfo(target, QStringLiteral("z"));
    QVERIFY(savedFont.found);
    QCOMPARE(savedFont.familyName, family);
    QCOMPARE(savedFont.embedded, 1);
    // A cache file altered after installation must never become save input.
    QFile cached(QDir(store.path).filePath(QString::fromLatin1(
        QCryptographicHash::hash(font, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".ttf")));
    QVERIFY(cached.open(QIODevice::WriteOnly | QIODevice::Truncate));
    cached.write("corrupted");
    cached.close();
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, false).isValid());
    QString error;
    QVERIFY2(ApplicationFonts::installDownloadedFont(font, family, false, false, uint('z'), error), qPrintable(error));
    QCOMPARE(ApplicationFonts::downloadedFontFace(family, false, false).data, font);
    QVERIFY2(ApplicationFonts::installDownloadedFont(font, family, false, false, uint('z'), error), qPrintable(error));
    QCOMPARE(QDir(store.path).entryList({QStringLiteral("*.ttf")}, QDir::Files).size(), 4);
}

void PDFEditControllerTest::validatesFontFamilyBeforeInstallation()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("DlBat");
    const QByteArray regular = uninstalledSubsetFontData("DlBat");
    QVERIFY(!regular.isEmpty());
    // Even though the first face is valid, the second face is regular data
    // advertised as italic. The batch must not register or persist either one.
    const QList<ApplicationFonts::DownloadedFontData> faces{
        {regular, family, false, false, uint('z')},
        {regular, family, false, true, 0}};
    QString error;
    QVERIFY(!ApplicationFonts::installDownloadedFonts(faces, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(store.path));
    QVERIFY(!ApplicationFonts::downloadedFontFamilies().contains(family));
    QVERIFY(!QFontDatabase::families().contains(family));
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, false).isValid());
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, true).isValid());
}

void PDFEditControllerTest::rollsBackFontFamilyWhenOptionalTargetBlocked()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QStringLiteral("DlBlk");
    const QByteArray regular = uninstalledSubsetFontData("DlBlk");
    const QByteArray italic = uninstalledSubsetFontData("DlBlk", false, true);
    QVERIFY(!regular.isEmpty());
    QVERIFY(!italic.isEmpty());
    const QString blocked = QDir(store.path).filePath(QString::fromLatin1(
        QCryptographicHash::hash(italic, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".ttf"));
    QVERIFY(QDir().mkpath(blocked));
    QFile marker(QDir(blocked).filePath(QStringLiteral("preserve.txt")));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    QCOMPARE(marker.write("existing directory"), 18);
    marker.close();
    const QList<ApplicationFonts::DownloadedFontData> faces{
        {regular, family, false, false, uint('z')},
        {italic, family, false, true, 0}};
    QString error;
    QVERIFY(!ApplicationFonts::installDownloadedFonts(faces, error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QDir(store.path).entryList(QDir::Files).isEmpty());
    QVERIFY(QFileInfo(blocked).isDir());
    QVERIFY(marker.open(QIODevice::ReadOnly));
    QCOMPARE(marker.readAll(), QByteArrayLiteral("existing directory"));
    QVERIFY(!ApplicationFonts::downloadedFontFamilies().contains(family));
    QVERIFY(!QFontDatabase::families().contains(family));
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, false).isValid());
    QVERIFY(!ApplicationFonts::downloadedFontFace(family, false, true).isValid());
}

void PDFEditControllerTest::savesExternalPageFourOnSamePath()
{
    const QString external = qEnvironmentVariable("TREPDF_TEST_MIXED_FONT_PDF");
    if (external.isEmpty())
        QSKIP("Set TREPDF_TEST_MIXED_FONT_PDF to the 500 Lines or Less PDF for this probe");
    // Font registration outlives IsolatedFontStore. Keep this opt-in probe in
    // its own process so a full suite run does not inherit stale registrations.
    if (!qEnvironmentVariableIsSet("TREPDF_EXTERNAL_SAVE_PROBE")) {
        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("TREPDF_EXTERNAL_SAVE_PROBE"), QStringLiteral("1"));
        probe.setProcessEnvironment(environment);
        probe.setProcessChannelMode(QProcess::MergedChannels);
        const QString logPath = m_temporaryDirectory.filePath(QStringLiteral("external-save.txt"));
        probe.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("savesExternalPageFourOnSamePath"),
                     QStringLiteral("-o"), logPath + QStringLiteral(",txt")});
        QVERIFY2(probe.waitForStarted(10000), qPrintable(probe.errorString()));
        QVERIFY2(probe.waitForFinished(60000), qPrintable(probe.errorString()));
        QFile log(logPath);
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QByteArray output = log.readAll();
        QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                 output.constData());
        return;
    }
    QFile original(external);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = original.readAll();
    original.close();
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("page-four-save-copy.pdf"));
    QVERIFY(QFile::copy(external, path));

    IsolatedFontStore store;
    QVERIFY(store.fresh);
    QList<ApplicationFonts::DownloadedFontData> faces;
    for (int style = 0; style < 4; ++style)
        faces.append({ctanTermesFontData(style), QStringLiteral("TeX Gyre Termes"),
                      bool(style & 2), bool(style & 1), uint('e')});
    QString error;
    QVERIFY2(ApplicationFonts::installDownloadedFonts(faces, error), qPrintable(error));

    PDFTabModel tabs;
    PDFDocumentSession *session = tabs.open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    const int pageCount = session->document()->pageCount();
    QVERIFY(pageCount >= 4);
    auto *controller = session->editController();
    QSignalSpy saved(controller, &PDFEditController::saveFinished);
    for (int pass = 1; pass <= 2; ++pass) {
        QVariantMap candidate;
        for (const QVariant &value : waitForCandidates(*controller, 3)) {
            const QVariantMap item = value.toMap();
            if (item.value(QStringLiteral("text")).toString().contains(QStringLiteral("to Share"))) {
                candidate = item;
                break;
            }
        }
        QVERIFY2(!candidate.isEmpty(), "Page-four Share paragraph was not discovered");
        QVERIFY(controller->selectCandidate(3, candidate.value(QStringLiteral("objectId")).toString()));
        const QString marker = QStringLiteral(" save verification %1").arg(pass);
        controller->commitSelectedText(controller->selectedText() + marker);
        QVERIFY(controller->hasDrafts());
        QVERIFY(tabs.requestSave(session));
        QTRY_COMPARE_WITH_TIMEOUT(saved.count(), pass, 30000);
        QVERIFY(saved.constLast().constFirst().toBool());
        QVERIFY(!controller->hasDrafts());
        QVERIFY(!controller->canUndo());
        QCOMPARE(session->document()->source(), QUrl::fromLocalFile(path));
        QCOMPARE(session->document()->pageCount(), pageCount);
        QVERIFY(normalizedPageText(path, 3).contains(marker.trimmed()));
    }
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), originalBytes);
}

void PDFEditControllerTest::preservesType1UrlThroughInlineEditing_data()
{
    QTest::addColumn<qreal>("zoom");
    QTest::addColumn<QString>("probePath");
    QTest::newRow("mixed-font-75-percent") << 0.75 << QString();
    QTest::newRow("mixed-font-155-percent") << 1.55 << QString();
    const QString probe = qEnvironmentVariable("TREPDF_TEST_MIXED_FONT_PDF");
    if (!probe.isEmpty())
        QTest::newRow("external-page-four") << 1.55 << probe;
}

void PDFEditControllerTest::preservesType1UrlThroughInlineEditing()
{
    QFETCH(qreal, zoom);
    QFETCH(QString, probePath);
    // Downloaded registrations live for the process lifetime. Keep this
    // optional probe's temporary Termes store out of subsequent font tests.
    if (!probePath.isEmpty() && !qEnvironmentVariableIsSet("TREPDF_TYPE1_URL_PROBE")) {
        QProcess probe;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("TREPDF_TYPE1_URL_PROBE"), QStringLiteral("1"));
        probe.setProcessEnvironment(environment);
        probe.setProcessChannelMode(QProcess::MergedChannels);
        probe.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("preservesType1UrlThroughInlineEditing:external-page-four")});
        QVERIFY2(probe.waitForStarted(10000), qPrintable(probe.errorString()));
        QVERIFY2(probe.waitForFinished(30000), qPrintable(probe.errorString()));
        const QByteArray output = probe.readAll();
        QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                 output.constData());
        return;
    }
    IsolatedFontStore fontStore;
    QVERIFY(fontStore.fresh);
    if (!probePath.isEmpty()) {
        // Match the application's complete body face without touching its cache.
        // The URL must still use the embedded Type 1 face from the PDF.
        QList<ApplicationFonts::DownloadedFontData> faces;
        for (int style = 0; style < 4; ++style) {
            const QByteArray bytes = ctanTermesFontData(style);
            QVERIFY(!bytes.isEmpty());
            faces.append({bytes, QStringLiteral("TeX Gyre Termes"),
                          bool(style & 2), bool(style & 1), uint('e')});
        }
        QString error;
        QVERIFY2(ApplicationFonts::installDownloadedFonts(faces, error), qPrintable(error));
    }
    const QString url = QStringLiteral("http://creativecommons.org/licenses/by/3.0/.");
    const QString prefix = QStringLiteral("best way to do this is with a link to ");
    const int pageIndex = probePath.isEmpty() ? 0 : 3;
    const QString sourcePath = probePath.isEmpty()
        ? m_temporaryDirectory.filePath(QStringLiteral("type1-inline-%1.pdf").arg(zoom)) : probePath;
    if (probePath.isEmpty()) {
        QFile fixture(QFINDTESTDATA("fixtures/fonts/type1/trepdf-test-mono.pfa"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const QByteArray font = fixture.readAll();
        const auto stream = [](const QByteArray &bytes, const QByteArray &extra = {}) {
            return "<< /Length " + QByteArray::number(bytes.size()) + extra
                + " >>\nstream\n" + bytes + "\nendstream";
        };
        const QByteArray content = "BT /F1 9 Tf 72 740 Td (" + prefix.toLatin1()
            + ") Tj /F2 9 Tf (" + url.toLatin1() + ") Tj ET\n";
        QByteArray widths;
        for (int c = 32; c <= 126; ++c)
            widths += "500 ";
        QVERIFY(writePdfObjects(sourcePath, {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                "/Resources << /Font << /F1 4 0 R /F2 5 0 R >> >> /Contents 6 0 R >>",
            "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /Encoding /WinAnsiEncoding >>",
            "<< /Type /Font /Subtype /Type1 /BaseFont /TrePDFTestMono-Regular "
                "/Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 126 /Widths [" + widths
                + "] /FontDescriptor 7 0 R >>",
            stream(content),
            "<< /Type /FontDescriptor /FontName /TrePDFTestMono-Regular /Flags 33 "
                "/FontBBox [0 0 500 700] /ItalicAngle 0 /Ascent 700 /Descent 0 "
                "/CapHeight 700 /StemV 80 /FontFile 8 0 R >>",
            stream(font, " /Length1 " + QByteArray::number(font.indexOf("currentfile eexec") + 18))}));
    }
    PDFDocument pdf;
    PDFEditController controller;
    QVERIFY(pdf.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&pdf);
    controller.setTool(PDFEditController::SelectTool);
    QVariantMap candidate;
    for (const QVariant &value : waitForCandidates(controller, pageIndex)) {
        const QVariantMap item = value.toMap();
        if (item.value(QStringLiteral("text")).toString().contains(prefix.trimmed())) {
            candidate = item;
            break;
        }
    }
    QVERIFY2(!candidate.isEmpty(), "The mixed serif/monospace line must remain one candidate");
    QVERIFY(controller.selectCandidate(pageIndex, candidate.value(QStringLiteral("objectId")).toString()));
    const QString original = controller.selectedText();
    const int urlStart = original.indexOf(url);
    QVERIFY2(urlStart >= prefix.size() - 1, qPrintable(original));
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    const QRectF originalFrame = controller.selectedFrameRect();
    QVector<qreal> sourceOrigins;
    QByteArray sourceFont;
    {
        PDFiumProcessLock lock;
        FPDF_DOCUMENT doc = FPDF_LoadDocument(QFile::encodeName(sourcePath).constData(), nullptr);
        QVERIFY(doc);
        const auto closeDoc = qScopeGuard([&] { FPDF_CloseDocument(doc); });
        FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
        QVERIFY(page);
        const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
        FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
        QVERIFY(text);
        const auto closeText = qScopeGuard([&] { FPDFText_ClosePage(text); });
        FPDF_SCHHANDLE search = FPDFText_FindStart(text,
            reinterpret_cast<FPDF_WIDESTRING>(original.utf16()), FPDF_MATCHCASE, 0);
        QVERIFY(search);
        const auto closeSearch = qScopeGuard([&] { FPDFText_FindClose(search); });
        QVERIFY(FPDFText_FindNext(search));
        const int start = FPDFText_GetSchResultIndex(search) + urlStart;
        for (int i = 0; i < url.size(); ++i) {
            double x = 0.0, y = 0.0;
            QVERIFY(FPDFText_GetCharOrigin(text, start + i, &x, &y));
            sourceOrigins.append(x);
        }
        FPDF_FONT font = FPDFTextObj_GetFont(FPDFText_GetTextObject(text, start));
        size_t size = 0;
        QVERIFY(FPDFFont_GetFontData(font, nullptr, 0, &size));
        QVERIFY(size > 0 && size < 1024 * 1024);
        sourceFont.resize(qsizetype(size));
        QVERIFY(FPDFFont_GetFontData(font, reinterpret_cast<uint8_t *>(sourceFont.data()), size, &size));
        QVERIFY(sourceFont.startsWith("%!"));
    }
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQuickWindow window;
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Components/EditOverlay.qml")));
    const QSizeF pageSize = pdf.pageSize(pageIndex);
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties({
        {QStringLiteral("page"), pageIndex}, {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), false}, {QStringLiteral("draftTextureReady"), false},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), zoom},
        {QStringLiteral("width"), pageSize.width() * zoom},
        {QStringLiteral("height"), pageSize.height() * zoom}}));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    auto *overlayItem = qobject_cast<QQuickItem *>(overlay.get());
    overlayItem->setParentItem(window.contentItem());
    window.resize(qCeil(pageSize.width() * zoom), qCeil(pageSize.height() * zoom));
    window.show();
    QTRY_VERIFY(window.isExposed());
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginSelectedEditing"));
    QObject *editor = nullptr;
    for (QObject *child : overlay->findChildren<QObject *>()) {
        if (child->metaObject()->indexOfProperty("textDocument") >= 0) {
            editor = child;
            break;
        }
    }
    QVERIFY(editor);
    auto *editorItem = qobject_cast<QQuickItem *>(editor);
    auto *quickDocument = editor->property("textDocument").value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *document = quickDocument->textDocument();
    QTRY_COMPARE(document->toPlainText(), original);
    QString error;
    const auto keepsUrl = [&]() {
        document->size();
        const QTextBlock block = document->findBlock(urlStart);
        const QTextLine line = block.layout()->lineForTextPosition(urlStart - block.position());
        if (!line.isValid()) return false;
        int checked = 0;
        for (const QGlyphRun &run : block.layout()->glyphRuns(urlStart - block.position(), url.size(),
                  QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveStringIndexes
                      | QTextLayout::RetrieveGlyphPositions)) {
            const QRawFont raw = run.rawFont();
            const QRawFont expected(sourceFont, raw.pixelSize(), QFont::PreferNoHinting);
            const auto glyphs = expected.glyphIndexesForString(url);
            for (int i = 0; i < run.glyphIndexes().size(); ++i) {
                const int index = run.stringIndexes()[i] + block.position() - urlStart;
                // The punctuation after the URL belongs to the surrounding serif run.
                if (index < 0 || index >= url.size() - 1) continue;
                if (run.glyphIndexes()[i] != glyphs[index]
                    || raw.pathForGlyph(run.glyphIndexes()[i]) != expected.pathForGlyph(glyphs[index])) {
                    error = QStringLiteral("URL character %1 uses %2 instead of the original Type 1 outlines")
                        .arg(index).arg(raw.familyName());
                    return false;
                }
                const qreal x = editorItem->mapToItem(overlayItem,
                    QPointF(editor->property("leftPadding").toReal()
                        + block.layout()->position().x() + run.positions()[i].x(), 0)).x() / zoom;
                if (qAbs(x - sourceOrigins[index]) > 0.15) {
                    error = QStringLiteral("URL character %1: editor x=%2, PDF x=%3")
                        .arg(index).arg(x).arg(sourceOrigins[index]);
                    return false;
                }
                ++checked;
            }
        }
        return checked == url.size() - 1;
    };
    QTRY_VERIFY2(keepsUrl(), qPrintable(error));
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginSelectedEditing"));
    editor->setProperty("cursorPosition", original.size());
    editorItem->forceActiveFocus();
    QTest::keyClick(&window, Qt::Key_E);
    QTRY_COMPARE(controller.selectedText(), original + QLatin1Char('e'));
    QTRY_VERIFY2(keepsUrl(), qPrintable(error));
    QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
    QTRY_COMPARE(controller.selectedText(), original);
    QTRY_VERIFY2(keepsUrl(), qPrintable(error));
    QVERIFY(QMetaObject::invokeMethod(editor, "redo"));
    QTRY_COMPARE(controller.selectedText(), original + QLatin1Char('e'));
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("type1-inline-saved-%1.pdf").arg(zoom));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    const QVariantMap reopened = candidateForText(waitForCandidates(controller, pageIndex), original + QLatin1Char('e'));
    QVERIFY(!reopened.isEmpty());
    QVERIFY(controller.selectCandidate(pageIndex, reopened.value(QStringLiteral("objectId")).toString()));
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginSelectedEditing"));
    QTRY_VERIFY2(keepsUrl(), qPrintable(error));
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QVERIFY(!controller.hasDrafts());
    if (probePath.isEmpty()) {
        // 'w' occurs in the serif prefix and in the Type 1 program, but not
        // among the URL font's observed PDF character mappings. Coverage from
        // the other font must not authorize reuse or a silent Courier fallback.
        const QString edited = controller.selectedText() + QLatin1Char('w');
        controller.commitSelectedText(edited);
        const QString protectedPath = m_temporaryDirectory.filePath(
            QStringLiteral("type1-unsupported-%1.pdf").arg(zoom));
        const QByteArray sentinel("Existing destination must remain unchanged.");
        QFile destination(protectedPath);
        QVERIFY(destination.open(QIODevice::WriteOnly));
        QCOMPARE(destination.write(sentinel), sentinel.size());
        destination.close();
        QVERIFY(!controller.saveAs(QUrl::fromLocalFile(protectedPath)));
        QVERIFY(controller.hasDrafts());
        QCOMPARE(controller.selectedText(), edited);
        QVERIFY(destination.open(QIODevice::ReadOnly));
        QCOMPARE(destination.readAll(), sentinel);
    }
}


void PDFEditControllerTest::checksAllFontsBeforeInlineEditing()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString first = QStringLiteral("PfAll");
    const QString second = QStringLiteral("PfTwo");
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("preflight-mixed.pdf"));
    QVERIFY(createMixedPreflightFontsPdf(path, first.toLatin1(), second.toLatin1()));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const auto candidate = candidateForText(waitForCandidates(controller, 0),
                                           QStringLiteral("Alpha beta gamma delta"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QString objectId = controller.selectedObjectId();
    const auto originalFrame = controller.selectedFrameRect();
    QSignalSpy requested(&controller, &PDFEditController::fontDownloadRequested);
    QSignalSpy prepared(&controller, &PDFEditController::textEditPreparationReady);
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QVERIFY(!controller.requestSelectedTextEdit(QStringLiteral("underline")));
    QVERIFY(controller.fontDownloadPending());
    QVERIFY(controller.fontDownloadForEdit());
    QVERIFY(!controller.downloadingFont());
    QVERIFY(!controller.saving());
    QVERIFY(!controller.textEditActive());
    QVERIFY(!controller.hasDrafts());
    QStringList missing = controller.missingFontFamilies();
    missing.sort();
    QCOMPARE(missing, QStringList({first, second}));
    QCOMPARE(requested.size(), 1);

    // Regular alone does not satisfy a Bold span; all original spans must be
    // checked even when the first family already has every required face.
    QList<ApplicationFonts::DownloadedFontData> faces;
    for (int style = 0; style < 4; ++style)
        faces.append({uninstalledSubsetFontData(first.toLatin1(), style & 2, style & 1),
                      first, bool(style & 2), bool(style & 1), uint('a')});
    QString error;
    QVERIFY2(ApplicationFonts::installDownloadedFonts(faces, error), qPrintable(error));
    QVERIFY2(ApplicationFonts::installDownloadedFonts(
        {{uninstalledSubsetFontData(second.toLatin1()), second, false, false, uint('a')}}, error),
        qPrintable(error));
    controller.cancelTextEditPreparation();
    QVERIFY(!controller.fontDownloadPending());
    QVERIFY(!controller.requestSelectedTextEdit(QStringLiteral("underline")));
    QCOMPARE(controller.missingFontFamilies(), QStringList({second}));
    QCOMPARE(prepared.size(), 0);
    QVERIFY(!controller.textEditActive());
    faces.clear();
    for (int style = 1; style < 4; ++style)
        faces.append({uninstalledSubsetFontData(second.toLatin1(), style & 2, style & 1),
                      second, bool(style & 2), bool(style & 1), uint('a')});
    QVERIFY2(ApplicationFonts::installDownloadedFonts(faces, error), qPrintable(error));
    controller.respondToFontDownload(true);
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 5000);
    QCOMPARE(prepared.first().at(0).toInt(), 0);
    QCOMPARE(prepared.first().at(1).toString(), objectId);
    QCOMPARE(prepared.first().at(2).toString(), QStringLiteral("underline"));
    QVERIFY(!controller.fontDownloadPending());
    QVERIFY(!controller.fontDownloadForEdit());
    QVERIFY(!controller.downloadingFont());
    QVERIFY(!controller.saving());
    QVERIFY(!controller.textEditActive());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(saved.size(), 0);
    QVERIFY(controller.requestSelectedTextEdit());
    QCOMPARE(prepared.size(), 1); // Ready is synchronous when no download is required.
    controller.beginSelectedTextEdit();
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::skipsInlineFontDownloadForEmbeddedAndCachedFaces()
{
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString embeddedPath = m_temporaryDirectory.filePath(QStringLiteral("preflight-embedded.pdf"));
    QVERIFY(createUninstalledSubsetPdf(embeddedPath, QStringLiteral("2017"), QStringLiteral("a "), "PfEmb"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(embeddedPath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    auto candidate = candidateForText(waitForCandidates(controller, 0), QStringLiteral("2017"));
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const auto originalSpans = controller.selectedCharacterSpans();
    QSignalSpy requested(&controller, &PDFEditController::fontDownloadRequested);
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSelectedTextEdit());
    QCOMPARE(requested.size(), 0);
    QVERIFY(!controller.textEditActive());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    // A Regular embedded subset cannot stand in for the requested Bold face.
    QVERIFY(!controller.requestSelectedTextEdit(QStringLiteral("bold")));
    QCOMPARE(controller.missingFontFamilies(), QStringList({QStringLiteral("PfEmb")}));
    controller.respondToFontDownload(false);
    QVERIFY(!controller.fontDownloadPending());
    QCOMPARE(saved.size(), 0);

    const QString family = QStringLiteral("PfCac");
    QList<ApplicationFonts::DownloadedFontData> faces;
    for (int style = 0; style < 4; ++style)
        faces.append({uninstalledSubsetFontData(family.toLatin1(), style & 2, style & 1),
                      family, bool(style & 2), bool(style & 1), uint('a')});
    QString error;
    QVERIFY2(ApplicationFonts::installDownloadedFonts(faces, error), qPrintable(error));
    const QString cachedPath = m_temporaryDirectory.filePath(QStringLiteral("preflight-cached.pdf"));
    QVERIFY(createUnavailableType1FontPdf(cachedPath, "PfCac-Regular"));
    QVERIFY(document.open(QUrl::fromLocalFile(cachedPath)));
    controller.setTool(PDFEditController::SelectTool);
    candidate = candidateForText(waitForCandidates(controller, 0),
        QStringLiteral("to Share\u2014to copy, distribute the work"));
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const int priorRequests = requested.size();
    QVERIFY(controller.requestSelectedTextEdit(QStringLiteral("bold")));
    QVERIFY(controller.requestSelectedTextEdit());
    QCOMPARE(requested.size(), priorRequests);
    QVERIFY(!controller.fontDownloadPending());
    QVERIFY(!controller.textEditActive());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(saved.size(), 0);
}

void PDFEditControllerTest::cancelsStaleInlineFontPreparation_data()
{
    QTest::addColumn<QString>("action");
    for (const char *action : {"decline", "cancel", "selection", "tool", "document", "history",
                               "deactivate", "request-reset", "download-start"})
        QTest::newRow(action) << QString::fromLatin1(action);
}

void PDFEditControllerTest::cancelsStaleInlineFontPreparation()
{
    QFETCH(QString, action);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("preflight-cancel-%1.pdf").arg(action));
    QVERIFY(createMixedPreflightFontsPdf(path, "PfCan", "PfOth"));
    PDFDocumentSession session;
    QVERIFY(session.open(QUrl::fromLocalFile(path)));
    auto &controller = *session.editController();
    controller.setTool(PDFEditController::SelectTool);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const auto candidate = candidateForText(candidates, QStringLiteral("Alpha beta gamma delta"));
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    if (action == QLatin1String("history"))
        controller.commitSelectedText(controller.selectedText() + QLatin1Char('!'));
    QSignalSpy prepared(&controller, &PDFEditController::textEditPreparationReady);
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QVERIFY(!controller.requestSelectedTextEdit());
    QVERIFY(controller.fontDownloadPending());
    QVERIFY(controller.fontDownloadForEdit());
    if (action == QLatin1String("decline"))
        controller.respondToFontDownload(false);
    else if (action == QLatin1String("cancel"))
        controller.cancelFontDownload();
    else if (action == QLatin1String("selection")) {
        const auto other = candidateForText(candidates, QStringLiteral("Other object"));
        QVERIFY(controller.selectCandidate(0, other.value(QStringLiteral("objectId")).toString()));
    } else if (action == QLatin1String("tool"))
        controller.setTool(PDFEditController::BrowseTool);
    else if (action == QLatin1String("document"))
        session.document()->close();
    else if (action == QLatin1String("history"))
        controller.undo();
    else if (action == QLatin1String("request-reset")) {
        connect(&controller, &PDFEditController::fontDownloadStateChanged, &session, [&]() {
            if (!controller.fontDownloadPending() && session.document()->isLoaded())
                session.document()->close();
        });
        QVERIFY(!controller.requestSelectedTextEdit(QStringLiteral("bold")));
    } else if (action == QLatin1String("download-start")) {
        connect(&controller, &PDFEditController::fontDownloadStateChanged, &session, [&]() {
            if (controller.downloadingFont())
                controller.cancelTextEditPreparation();
        });
        controller.respondToFontDownload(true);
    }
    else
        session.prepareForDeactivation();
    QVERIFY(!controller.fontDownloadPending());
    QVERIFY(!controller.fontDownloadForEdit());
    QVERIFY(!controller.downloadingFont());
    QVERIFY(!controller.saving());
    QVERIFY(!controller.textEditActive());
    QVERIFY(!controller.hasDrafts());
    controller.respondToFontDownload(true); // A late response must not revive the editor.
    QCoreApplication::processEvents();
    QCOMPARE(prepared.size(), 0);
    QCOMPARE(saved.size(), 0);
}

void PDFEditControllerTest::opensInlineEditorAfterFontPreparation_data()
{
    QTest::addColumn<QString>("styleAction");
    QTest::addColumn<QByteArray>("familyBytes");
    QTest::newRow("plain-entry") << QString() << QByteArray("PfQml");
    QTest::newRow("bold-entry") << QStringLiteral("bold") << QByteArray("PfBld");
}

void PDFEditControllerTest::opensInlineEditorAfterFontPreparation()
{
    QFETCH(QString, styleAction);
    QFETCH(QByteArray, familyBytes);
    IsolatedFontStore store;
    QVERIFY(store.fresh);
    const QString family = QString::fromLatin1(familyBytes);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("preflight-qml-%1.pdf").arg(family));
    QVERIFY(createUnavailableType1FontPdf(path, familyBytes + "-Regular"));
    PDFDocument pdf;
    PDFEditController controller;
    QVERIFY(pdf.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&pdf);
    controller.setTool(PDFEditController::SelectTool);
    const QString original = QStringLiteral("to Share\u2014to copy, distribute the work");
    const auto candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QString objectId = controller.selectedObjectId();
    QSignalSpy prepared(&controller, &PDFEditController::textEditPreparationReady);
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQuickWindow window;
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Components/EditOverlay.qml")));
    const QSizeF size = pdf.pageSize(0);
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties({
        {QStringLiteral("page"), 0}, {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), false}, {QStringLiteral("draftTextureReady"), false},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), 1.0},
        {QStringLiteral("width"), size.width()}, {QStringLiteral("height"), size.height()}}));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    qobject_cast<QQuickItem *>(overlay.get())->setParentItem(window.contentItem());
    window.resize(qCeil(size.width()), qCeil(size.height()));
    window.show();
    QTRY_VERIFY(window.isExposed());
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginSelectedEditingWithStyle",
                                     Q_ARG(QVariant, QVariant(styleAction))));
    QVERIFY(controller.fontDownloadPending());
    QVERIFY(controller.fontDownloadForEdit());
    QVERIFY(!overlay->property("editing").toBool());
    QVERIFY(!controller.textEditActive());
    QVERIFY(!controller.hasDrafts());

    // Use the real downloader with a deterministic transport; responding after
    // installation exercises the controller resume path without network access.
    FontNetwork network;
    network.responses = {{fontMetadata(family, true)}};
    const QList<int> faceOrder = styleAction == QLatin1String("bold")
        ? QList<int>{2, 0, 1, 3} : QList<int>{0, 1, 2, 3};
    for (int style : faceOrder) {
        network.responses.append(FontResponse{fullFontStylesheet});
        network.responses.append(FontResponse{uninstalledSubsetFontData(familyBytes, style & 2, style & 1)});
    }
    FontDownloadService service(nullptr, &network);
    QSignalSpy downloaded(&service, &FontDownloadService::finished);
    service.start(family, styleAction == QLatin1String("bold"), false, uint('a'));
    QTRY_COMPARE_WITH_TIMEOUT(downloaded.size(), 1, 5000);
    QVERIFY2(downloaded.first().first().toBool(), qPrintable(downloaded.first().at(1).toString()));
    controller.respondToFontDownload(true);
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 5000);
    QCOMPARE(prepared.first().at(1).toString(), objectId);
    QCOMPARE(prepared.first().at(2).toString(), styleAction);
    QTRY_VERIFY(overlay->property("editing").toBool());
    QVERIFY(controller.textEditActive());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.saving());
    QCOMPARE(saved.size(), 0);
    QObject *editor = nullptr;
    for (QObject *child : overlay->findChildren<QObject *>()) {
        if (child->metaObject()->indexOfProperty("textDocument") >= 0) {
            editor = child;
            break;
        }
    }
    QVERIFY(editor);
    auto *quickDocument = editor->property("textDocument").value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *document = quickDocument->textDocument();
    QCOMPARE(document->toPlainText(), original);
    QTRY_VERIFY(document->begin().layout()->lineCount() > 0);
    const QRawFont expected(ApplicationFonts::downloadedFontFace(family, false, false).data,
                            12.0, QFont::PreferNoHinting);
    QVERIFY(expected.isValid());
    const auto runs = document->begin().layout()->glyphRuns(0, original.size(),
        QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveGlyphPositions);
    QVERIFY(!runs.isEmpty());
    for (const QGlyphRun &run : runs) {
        const auto raw = run.rawFont();
        QCOMPARE(raw.fontTable("glyf"), expected.fontTable("glyf"));
    }
    if (!styleAction.isEmpty()) {
        QCOMPARE(editor->property("cursorPosition").toInt(), original.size());
        QVERIFY(controller.selectedFontBold());
        qobject_cast<QQuickItem *>(editor)->forceActiveFocus();
        QTest::keyClick(&window, Qt::Key_X);
        QTRY_COMPARE(controller.selectedText(), original + QLatin1Char('x'));
        QTextCursor cursor(document);
        cursor.setPosition(original.size());
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        QCOMPARE(cursor.charFormat().fontWeight(), int(QFont::Bold));
        QTRY_VERIFY(!document->begin().layout()->glyphRuns(original.size(), 1).isEmpty());
        const auto typed = document->begin().layout()->glyphRuns(original.size(), 1);
        QVERIFY(!typed.isEmpty());
        const QByteArray os2 = typed.first().rawFont().fontTable("OS/2");
        QVERIFY(os2.size() > 5);
        QVERIFY(((quint8(os2[4]) << 8) | quint8(os2[5])) >= 700);
        QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
        QTRY_COMPARE(controller.selectedText(), original);
    }
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(pdf.source().toLocalFile(), path);
    QCOMPARE(saved.size(), 0);
}
