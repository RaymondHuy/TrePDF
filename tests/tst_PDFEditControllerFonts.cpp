#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <fpdf_ppo.h>
#include "FontDownloadService.h"
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
#include <zip.h>

using namespace PDFEditControllerTestSupport;

namespace {

struct FontResponse {
    QByteArray body;
    int status = 200;
    QUrl redirect;
    qint64 contentLength = -1;
    int delayMs = 0;
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
        QTimer::singleShot(response.delayMs, this, [this]() {
            if (m_aborted)
                return;
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
                                 int pageIndex = 0)
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
        if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT
            || pageObjectText(object, textPage).trimmed() != text)
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
    QCOMPARE(candidate.value(QStringLiteral("fontFamily")).toString(),
             compatible ? fallback : QString());
    if (!compatible) {
        return;
    }
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedFontBold(), bold);
    QCOMPARE(controller.selectedFontItalic(), italic);
    controller.beginSelectedTextEdit();
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
    const QString editedText = expectedText + QStringLiteral(" again");
    controller.commitSelectedText(editedText);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY2(pageText(outputPath).contains(editedText), qPrintable(pageText(outputPath)));
    const TextObjectFontInfo savedFont = textObjectFontInfo(outputPath, QStringLiteral("to Share"));
    QVERIFY(savedFont.found);
    QCOMPARE(compactFontName(savedFont.familyName), compactFontName(fallback));
    QCOMPARE(savedFont.italic, italic);
    QCOMPARE(savedFont.embedded, 1);
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
    QCOMPARE(candidate.value(QStringLiteral("fontFamily")).toString(),
             kUninstalledSubsetFamily);
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
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
    QVERIFY(hasSubsetFontPrefix(outputInfo.baseName));
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

void PDFEditControllerTest::preservesFirstSpaceAfterSubsetSave()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("first-space-source.pdf"));
    const QString subsetPath = m_temporaryDirectory.filePath(
        QStringLiteral("first-space-subset.pdf"));
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
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(subsetPath)));
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
    const QStringList savedObjects = topLevelTextObjects(outputPath);
    QVERIFY2(savedObjects.contains(editedText),
             qPrintable(QStringLiteral("Saved text objects: %1")
                            .arg(savedObjects.join(QLatin1Char('|')))));
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
    QVERIFY(topLevelTextObjects(outputPath).contains(repeatedText));
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
    const QStringList savedObjects = topLevelTextObjects(outputPath);
    QCOMPARE(savedObjects, QStringList({groupedText}));
    QVERIFY(pageText(outputPath).contains(groupedText));
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
