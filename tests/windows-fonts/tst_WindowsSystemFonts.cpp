#include "ApplicationFonts.h"
#include "PDFFontAliases.h"
#include "FontDownloadService.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRawFont>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QUuid>
#include <QtEndian>
#include <QtTest>

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <future>
#include <vector>

namespace {

using Source = ApplicationFonts::FontSource;
using Error = ApplicationFonts::FontFaceData::Error;

const QString kVietnamese = QStringLiteral("Tiếng Việt: Trường học, Nguyễn Đặng, Ắ ễ ộ ự.");
const QString kEnglish = QStringLiteral("Windows system font: Hello 0123456789.");

bool installed(const QString &family)
{
    return ApplicationFonts::systemFontFamilies().contains(family, Qt::CaseInsensitive);
}

// Check the complete sfnt directory and checksum, not only whether Qt silently
// finds a display fallback for the name. Collections must become standalone TTF.
bool isStandaloneTrueType(const QByteArray &data)
{
    if (data.size() < 12 || data.size() > 64 * 1024 * 1024
        || qFromBigEndian<quint32>(data.constData()) != 0x00010000)
        return false;
    const quint16 count = qFromBigEndian<quint16>(data.constData() + 4);
    if (count == 0 || 12 + 16 * qsizetype(count) > data.size())
        return false;
    bool hasGlyf = false;
    bool hasLoca = false;
    bool hasCmap = false;
    for (int i = 0; i < count; ++i) {
        const char *record = data.constData() + 12 + 16 * i;
        const QByteArray tag(record, 4);
        const quint32 offset = qFromBigEndian<quint32>(record + 8);
        const quint32 length = qFromBigEndian<quint32>(record + 12);
        if (quint64(offset) + length > quint64(data.size()) || (offset & 3) != 0)
            return false;
        if (tag == "fvar" || tag == "CFF " || tag == "CFF2" || tag == "COLR"
            || tag == "CBDT" || tag == "sbix" || tag == "SVG ")
            return false;
        hasGlyf |= tag == "glyf" && length > 0;
        hasLoca |= tag == "loca" && length > 0;
        hasCmap |= tag == "cmap" && length > 0;
    }
    quint32 checksum = 0;
    for (qsizetype i = 0; i < data.size(); i += 4) {
        quint32 word = 0;
        for (int byte = 0; byte < 4; ++byte) {
            word <<= 8;
            if (i + byte < data.size())
                word |= static_cast<quint8>(data[i + byte]);
        }
        checksum += word;
    }
    return hasGlyf && hasLoca && hasCmap && checksum == 0xb1b0afba;
}

struct MemoryWriter : FPDF_FILEWRITE {
    QByteArray data;
    MemoryWriter()
    {
        version = 1;
        WriteBlock = [](FPDF_FILEWRITE *writer, const void *bytes, unsigned long count) {
            auto *self = static_cast<MemoryWriter *>(writer);
            if (quint64(self->data.size()) + count > 128 * 1024 * 1024)
                return 0;
            self->data.append(static_cast<const char *>(bytes), qsizetype(count));
            return 1;
        };
    }
};

class FixtureReply final : public QNetworkReply {
public:
    FixtureReply(const QNetworkRequest &request, QByteArray data, QObject *parent)
        : QNetworkReply(parent), m_data(std::move(data))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_data.isEmpty() ? 404 : 200);
        setHeader(QNetworkRequest::ContentLengthHeader, m_data.size());
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
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
        return m_data.size() - m_offset + QNetworkReply::bytesAvailable();
    }
protected:
    qint64 readData(char *destination, qint64 maximum) override
    {
        const qint64 count = qMin(maximum, m_data.size() - m_offset);
        if (!count)
            return -1;
        memcpy(destination, m_data.constData() + m_offset, size_t(count));
        m_offset += count;
        return count;
    }
private:
    QByteArray m_data;
    qint64 m_offset = 0;
    bool m_aborted = false;
};

class FixtureNetwork final : public QNetworkAccessManager {
public:
    QHash<QUrl, QByteArray> responses;
    QList<QUrl> requests;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        requests.append(request.url());
        return new FixtureReply(request, responses.value(request.url()), this);
    }
};

} // namespace

class WindowsSystemFontsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void exactFaces_data();
    void exactFaces();
    void unknownFamilyDoesNotFallBack();
    void missingGlyphDoesNotFallBack();
    void unsupportedColorFont();
    void syntheticItalicIsRejected();
    void collectionFaceBecomesStandalone_data();
    void collectionFaceBecomesStandalone();
    void concurrentSnapshots();
    void downloadedCffAliases_data();
    void downloadedCffAliases();
    void automaticCtanDownload();
    void pdfiumRoundTrip_data();
    void pdfiumRoundTrip();

private:
    QString m_commonFamily;
    QString m_downloadDirectory;
};

void WindowsSystemFontsTest::initTestCase()
{
    FPDF_InitLibrary();
    m_downloadDirectory = ApplicationFonts::downloadedFontDirectory();
    QVERIFY(!QFileInfo::exists(m_downloadDirectory));
    for (const QString &family : {QStringLiteral("Arial"), QStringLiteral("Cambria")}) {
        if (installed(family)) {
            m_commonFamily = family;
            break;
        }
    }
    QVERIFY2(!m_commonFamily.isEmpty(), "Install the standard Windows Arial or Cambria font family");
    const auto face = ApplicationFonts::systemFontFace(m_commonFamily, false, false);
    QVERIFY2(face.isValid(), "An installed common Windows font must resolve; this test cannot pass by skipping all fonts");
}

void WindowsSystemFontsTest::cleanupTestCase()
{
    FPDF_DestroyLibrary();
    QFontDatabase::removeAllApplicationFonts();
    const QDir cache(m_downloadDirectory);
    for (const auto &name : cache.entryList({QStringLiteral("*.otf")}, QDir::Files))
        QVERIFY(QFile::remove(cache.filePath(name)));
    if (cache.exists())
        QVERIFY(QDir().rmdir(m_downloadDirectory));
    QDir().rmdir(QFileInfo(m_downloadDirectory).absolutePath());
}

void WindowsSystemFontsTest::exactFaces_data()
{
    QTest::addColumn<QString>("family");
    QTest::addColumn<bool>("bold");
    QTest::addColumn<bool>("italic");
    for (const QString &family : {QStringLiteral("Arial"), QStringLiteral("Georgia")}) {
        if (!installed(family))
            continue;
        for (bool bold : {false, true}) {
            for (bool italic : {false, true}) {
                const QByteArray row = QStringLiteral("%1-%2-%3")
                    .arg(family).arg(bold ? "bold" : "regular").arg(italic ? "italic" : "upright").toUtf8();
                QTest::newRow(row.constData()) << family << bold << italic;
            }
        }
    }
}

void WindowsSystemFontsTest::exactFaces()
{
    QFETCH(QString, family);
    QFETCH(bool, bold);
    QFETCH(bool, italic);
    QVERIFY(ApplicationFonts::isFontFaceSelectable(family, bold, italic));
    QVERIFY(ApplicationFonts::fontFaceSupportsText(family, bold, italic, kEnglish, Source::System));
    const auto face = ApplicationFonts::systemFontFace(family, bold, italic);
    QVERIFY(face.isValid());
    QCOMPARE(face.error, Error::None);
    QVERIFY(isStandaloneTrueType(face.data));
    QCOMPARE(face.family.compare(family, Qt::CaseInsensitive), 0);
    const QRawFont raw(face.data, 16.0, QFont::PreferNoHinting);
    QVERIFY(raw.isValid());
    QCOMPARE(raw.familyName().compare(family, Qt::CaseInsensitive), 0);
    QCOMPARE(raw.weight() >= QFont::Bold, bold);
    QCOMPARE(raw.style() != QFont::StyleNormal, italic);
    bool hasVietnamese = true;
    for (uint character : kVietnamese.toUcs4())
        hasVietnamese &= raw.supportsCharacter(character);
    // Georgia versions shipped with Windows can lack Vietnamese glyphs. The
    // resolver must report the selected face's coverage, never Qt's fallback.
    QCOMPARE(ApplicationFonts::fontFaceSupportsText(family, bold, italic, kVietnamese, Source::System), hasVietnamese);
    QCOMPARE(ApplicationFonts::systemFontFace(family.toLower(), bold, italic).data, face.data);
}

void WindowsSystemFontsTest::unknownFamilyDoesNotFallBack()
{
    const QString missing = QStringLiteral("TrePDF deliberately nonexistent family 09f2274b");
    QVERIFY(!installed(missing));
    QVERIFY(!ApplicationFonts::isFontFaceSelectable(missing, false, false));
    QVERIFY(!ApplicationFonts::fontFaceSupportsText(missing, false, false, QStringLiteral("abc"), Source::System));
    const auto face = ApplicationFonts::systemFontFace(missing, false, false);
    QVERIFY(!face.isValid());
    QVERIFY(face.data.isEmpty());
    QCOMPARE(face.error, Error::Unavailable);
}

void WindowsSystemFontsTest::missingGlyphDoesNotFallBack()
{
    const char32_t absent = 0x10ffff;
    const QString text = kVietnamese + QString::fromUcs4(&absent, 1);
    QVERIFY(!ApplicationFonts::fontFaceSupportsText(m_commonFamily, false, false, text, Source::System));
    // A failed coverage request must not poison the cached face.
    QVERIFY(ApplicationFonts::fontFaceSupportsText(m_commonFamily, false, false, kVietnamese, Source::System));
}

void WindowsSystemFontsTest::unsupportedColorFont()
{
    const QString family = QStringLiteral("Segoe UI Emoji");
    if (!installed(family))
        QSKIP("Segoe UI Emoji is not installed");
    QVERIFY(!ApplicationFonts::isFontFaceSelectable(family, false, false));
    QVERIFY(!ApplicationFonts::fontFaceSupportsText(family, false, false, QStringLiteral("A"), Source::System));
    const auto face = ApplicationFonts::systemFontFace(family, false, false);
    QVERIFY(!face.isValid());
    QCOMPARE(face.error, Error::UnsupportedFormat);
}

void WindowsSystemFontsTest::syntheticItalicIsRejected()
{
    const QString family = QStringLiteral("Segoe UI Symbol");
    if (!installed(family))
        QSKIP("Segoe UI Symbol is not installed");
    QVERIFY(ApplicationFonts::systemFontFace(family, false, false).isValid());
    QVERIFY(!ApplicationFonts::isFontFaceSelectable(family, false, true));
    QVERIFY(!ApplicationFonts::fontFaceSupportsText(family, false, true, QStringLiteral("A"), Source::System));
    const auto face = ApplicationFonts::systemFontFace(family, false, true);
    QVERIFY(!face.isValid());
    QVERIFY(face.data.isEmpty());
}

void WindowsSystemFontsTest::collectionFaceBecomesStandalone_data()
{
    QTest::addColumn<QString>("family");
    QTest::newRow("Cambria") << QStringLiteral("Cambria");
    // Cambria Math is a separate face in the same TTC. Resolving it must not
    // accidentally export the first face in the collection.
    QTest::newRow("Cambria-Math") << QStringLiteral("Cambria Math");
}

void WindowsSystemFontsTest::collectionFaceBecomesStandalone()
{
    QFETCH(QString, family);
    if (!installed(family))
        QSKIP("This Cambria family is not installed");
    QFile collection(QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/cambria.ttc")));
    if (!collection.open(QIODevice::ReadOnly) || collection.read(4) != QByteArrayLiteral("ttcf"))
        QSKIP("This Windows installation does not provide Cambria as a system TTC");
    const auto face = ApplicationFonts::systemFontFace(family, false, false);
    QVERIFY(face.isValid());
    QVERIFY(isStandaloneTrueType(face.data));
    const QRawFont raw(face.data, 16.0, QFont::PreferNoHinting);
    QVERIFY(raw.isValid());
    QCOMPARE(raw.familyName(), family);
    QVERIFY(ApplicationFonts::fontFaceSupportsText(family, false, false, kVietnamese, Source::System));
}

void WindowsSystemFontsTest::concurrentSnapshots()
{
    const QString family = m_commonFamily;
    const auto regular = ApplicationFonts::systemFontFace(family, false, false);
    const auto bold = ApplicationFonts::systemFontFace(family, true, false);
    QVERIFY(regular.isValid());
    QVERIFY(bold.isValid());
    auto worker = [family](bool useBold, const QByteArray &expected) {
        for (int iteration = 0; iteration < 12; ++iteration) {
            const auto face = ApplicationFonts::systemFontFace(family, useBold, false);
            if (!face.isValid() || face.data != expected)
                return false;
        }
        return true;
    };
    auto regularWorker = std::async(std::launch::async, worker, false, regular.data);
    auto boldWorker = std::async(std::launch::async, worker, true, bold.data);
    bool guiCoverage = true;
    for (int iteration = 0; iteration < 24; ++iteration) {
        guiCoverage &= ApplicationFonts::isFontFaceSelectable(family, iteration % 2, false);
        guiCoverage &= ApplicationFonts::fontFaceSupportsText(
            family, iteration % 2, false, kVietnamese, Source::System);
    }
    QVERIFY(regularWorker.get());
    QVERIFY(boldWorker.get());
    QVERIFY(guiCoverage);
}

void WindowsSystemFontsTest::downloadedCffAliases_data()
{
    QTest::addColumn<QString>("style");
    QTest::addColumn<bool>("bold");
    QTest::addColumn<bool>("italic");
    QTest::newRow("regular") << QStringLiteral("regular") << false << false;
    QTest::newRow("italic") << QStringLiteral("italic") << false << true;
    QTest::newRow("bold") << QStringLiteral("bold") << true << false;
    QTest::newRow("bolditalic") << QStringLiteral("bolditalic") << true << true;
}

void WindowsSystemFontsTest::downloadedCffAliases()
{
    QFETCH(QString, style);
    QFETCH(bool, bold);
    QFETCH(bool, italic);
    const QString fixture = QStringLiteral("../fixtures/fonts/tex-gyre/texgyretermes-%1.otf")
        .arg(style);
    QFile file(QFINDTESTDATA(qPrintable(fixture)));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
    const QByteArray data = file.readAll();
    QVERIFY(data.startsWith("OTTO"));
    const QString family = QStringLiteral("TeX Gyre Termes");
    QString validationError;
    QVERIFY2(ApplicationFonts::validateDownloadedFontData(
        data, family, bold, italic, uint('z'), validationError), qPrintable(validationError));
    const QRawFont source(data, 16.0, QFont::PreferNoHinting);
    QVERIFY(source.isValid());
    QVERIFY(source.familyName() == family || source.familyName() == QStringLiteral("TeXGyreTermes"));
    QCOMPARE(source.weight() >= QFont::Bold, bold);
    QCOMPARE(source.style() != QFont::StyleNormal, italic);
    // Save revalidates against QRawFont's platform name; both names must
    // identify this exact face, while unrelated names/styles remain rejected.
    QVERIFY2(ApplicationFonts::validateDownloadedFontData(
        data, source.familyName(), bold, italic, uint('z'), validationError), qPrintable(validationError));
    QVERIFY(!ApplicationFonts::validateDownloadedFontData(
        data, QStringLiteral("TeX Gyre Heros"), bold, italic, uint('z'), validationError));
    QVERIFY(!ApplicationFonts::validateDownloadedFontData(
        data, family, !bold, italic, uint('z'), validationError));
    QVERIFY(!ApplicationFonts::validateDownloadedFontData(
        data, QStringLiteral("TeX  Gyre  Termes"), bold, italic, uint('z'), validationError));

    // Downloaded CFF faces must become private, selectable Qt aliases on the
    // native Windows platform, before installDownloadedFonts can commit them.
    // Register only in process memory; do not touch the user's font cache.
    const auto alias = PDFFontAliases::isolate(data, family);
    QVERIFY2(!alias.data.isEmpty(), "Could not isolate the original downloaded CFF face");
    QVERIFY(!alias.family.isEmpty());
    QVERIFY2(PDFFontAliases::retain(alias), "Windows could not register the private CFF alias");
    const auto release = qScopeGuard([&] { PDFFontAliases::release(alias.family); });
    QVERIFY(PDFFontAliases::matchesStyle(alias.family, bold, italic));
    QCOMPARE(PDFFontAliases::semanticFamily(alias.family), source.familyName());

    QFont layout(alias.family);
    layout.setPixelSize(16);
    layout.setBold(bold);
    layout.setItalic(italic);
    layout.setHintingPreference(QFont::PreferNoHinting);
    const QRawFont selected = QRawFont::fromFont(layout);
    QVERIFY(selected.isValid());
    QCOMPARE(selected.familyName(), alias.family);
    QCOMPARE(selected.weight() >= QFont::Bold, bold);
    QCOMPARE(selected.style() != QFont::StyleNormal, italic);
    const QVector<quint32> sourceGlyphs = source.glyphIndexesForString(kVietnamese);
    QVERIFY(!sourceGlyphs.contains(0));
    QCOMPARE(selected.glyphIndexesForString(kVietnamese), sourceGlyphs);
    QCOMPARE(selected.advancesForGlyphIndexes(sourceGlyphs),
             source.advancesForGlyphIndexes(sourceGlyphs));
}

void WindowsSystemFontsTest::pdfiumRoundTrip_data()
{
    exactFaces_data();
    if (installed(QStringLiteral("Cambria")))
        QTest::newRow("Cambria-collection") << QStringLiteral("Cambria") << false << false;
}

void WindowsSystemFontsTest::automaticCtanDownload()
{
    FixtureNetwork network;
    const bool live = qEnvironmentVariableIsSet("TREPDF_TEST_LIVE_FONT_DOWNLOAD");
    const QString family = QStringLiteral("TeX Gyre Termes");
    network.responses.insert(QUrl(QStringLiteral(
        "https://api.fontsource.org/v1/fonts?family=TeX%20Gyre%20Termes")), "[]");
    const QStringList styles{QStringLiteral("regular"), QStringLiteral("italic"),
                             QStringLiteral("bold"), QStringLiteral("bolditalic")};
    QList<QByteArray> originals;
    for (const QString &style : styles) {
        QFile fixture(QFINDTESTDATA(qPrintable(QStringLiteral(
            "../fixtures/fonts/tex-gyre/texgyretermes-%1.otf").arg(style))));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        originals.append(fixture.readAll());
        network.responses.insert(QUrl(QStringLiteral(
            "https://mirror.ctan.org/fonts/tex-gyre/opentype/texgyretermes-%1.otf").arg(style)),
            originals.constLast());
    }
    FontDownloadService service(nullptr, live ? nullptr : &network);
    QSignalSpy finished(&service, &FontDownloadService::finished);
    service.start(family, false, false, uint('T'));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, live ? 125000 : 5000);
    QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().at(1).toString()));
    if (!live)
        QCOMPARE(network.requests.size(), 5); // Metadata miss, then all four CTAN faces.
    QCOMPARE(QDir(m_downloadDirectory).entryList({QStringLiteral("*.otf")}, QDir::Files).size(), 4);
    const QString text = QStringLiteral("This work is licensed under the Creative Commons Attribution 3.0");
    for (int index = 0; index < styles.size(); ++index) {
        const bool bold = index & 2;
        const bool italic = index & 1;
        const auto face = ApplicationFonts::downloadedFontFace(family, bold, italic);
        QVERIFY(face.isValid());
        QCOMPARE(face.data, originals[index]);
        QCOMPARE(face.family, family);
        QCOMPARE(PDFFontAliases::semanticFamily(face.layoutFamily), family);
        QCOMPARE(ApplicationFonts::displayFamilyForStyle(family, bold, italic, text), face.layoutFamily);
        QVERIFY(ApplicationFonts::fontFaceSupportsText(family, bold, italic, text, Source::Downloaded));
    }
    service.start(family, true, true, uint('T'));
    QCOMPARE(finished.size(), 2);
    QVERIFY(finished.last().first().toBool());
    if (!live)
        QCOMPARE(network.requests.size(), 5); // A second edit uses the installed cache.
}

void WindowsSystemFontsTest::pdfiumRoundTrip()
{
    QFETCH(QString, family);
    QFETCH(bool, bold);
    QFETCH(bool, italic);
    const auto face = ApplicationFonts::systemFontFace(family, bold, italic);
    QVERIFY(face.isValid());
    const QString content = family == QStringLiteral("Georgia") ? kEnglish : kVietnamese;
    QVERIFY(ApplicationFonts::fontFaceSupportsText(family, bold, italic, content, Source::System));
    FPDF_DOCUMENT document = FPDF_CreateNewDocument();
    QVERIFY(document);
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    FPDF_FONT font = FPDFText_LoadFont(document,
        reinterpret_cast<const uint8_t *>(face.data.constData()),
        static_cast<uint32_t>(face.data.size()), FPDF_FONT_TRUETYPE, true);
    QVERIFY(font);
    const auto closeFont = qScopeGuard([&] { FPDFFont_Close(font); });
    FPDF_PAGE page = FPDFPage_New(document, 0, 595.0, 842.0);
    QVERIFY(page);
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    FPDF_PAGEOBJECT object = FPDFPageObj_CreateTextObj(document, font, 16.0f);
    QVERIFY(object);
    bool inserted = false;
    const auto destroyUninserted = qScopeGuard([&] {
        if (!inserted)
            FPDFPageObj_Destroy(object);
    });
    QVERIFY(FPDFText_SetText(object, reinterpret_cast<FPDF_WIDESTRING>(content.utf16())));
    FPDFPageObj_Transform(object, 1, 0, 0, 1, 40, 740);
    FPDFPage_InsertObject(page, object);
    inserted = true;
    QVERIFY(FPDFPage_GenerateContent(page));
    MemoryWriter saved;
    QVERIFY(FPDF_SaveAsCopy(document, &saved, FPDF_NO_INCREMENTAL));
    QVERIFY(saved.data.startsWith("%PDF-"));

    FPDF_DOCUMENT reopened = FPDF_LoadMemDocument64(saved.data.constData(), size_t(saved.data.size()), nullptr);
    QVERIFY(reopened);
    const auto closeReopened = qScopeGuard([&] { FPDF_CloseDocument(reopened); });
    QCOMPARE(FPDF_GetPageCount(reopened), 1);
    FPDF_PAGE reopenedPage = FPDF_LoadPage(reopened, 0);
    QVERIFY(reopenedPage);
    const auto closeReopenedPage = qScopeGuard([&] { FPDF_ClosePage(reopenedPage); });
    FPDF_TEXTPAGE text = FPDFText_LoadPage(reopenedPage);
    QVERIFY(text);
    const auto closeText = qScopeGuard([&] { FPDFText_ClosePage(text); });
    const int count = FPDFText_CountChars(text);
    QVERIFY(count > 0 && count < 1024);
    std::vector<unsigned short> extracted(size_t(count) + 1);
    const int copied = FPDFText_GetText(text, 0, count, extracted.data());
    QVERIFY(copied > 0);
    QCOMPARE(QString::fromUtf16(reinterpret_cast<const char16_t *>(extracted.data()), copied - 1), content);

    QCOMPARE(FPDFPage_CountObjects(reopenedPage), 1);
    FPDF_FONT embedded = FPDFTextObj_GetFont(FPDFPage_GetObject(reopenedPage, 0));
    QVERIFY(embedded);
    QCOMPARE(FPDFFont_GetIsEmbedded(embedded), 1);
    size_t bytes = 0;
    QVERIFY(FPDFFont_GetFontData(embedded, nullptr, 0, &bytes));
    QCOMPARE(bytes, size_t(face.data.size()));
    QByteArray embeddedData(qsizetype(bytes), Qt::Uninitialized);
    size_t actual = 0;
    QVERIFY(FPDFFont_GetFontData(embedded, reinterpret_cast<uint8_t *>(embeddedData.data()), bytes, &actual));
    QCOMPARE(actual, bytes);
    QVERIFY(isStandaloneTrueType(embeddedData));
    QCOMPARE(embeddedData, face.data);
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TrePDFTests"));
    QCoreApplication::setApplicationName(QStringLiteral("WindowsSystemFonts-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QStandardPaths::setTestModeEnabled(true);
    WindowsSystemFontsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_WindowsSystemFonts.moc"
