#include "ApplicationFonts.h"
#include "PDFDocument.h"
#include "PDFEditController.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <fpdf_edit.h>
#include <fpdf_save.h>

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

} // namespace

class PDFEditControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void registersBundledFonts();
    void detectsRegularAndBoldPdfFonts();
    void propagatesBoldThroughSelection();
    void groupsCompatibleTextObjectsOnSameLine();
    void keepsMixedStylesAsSeparateCandidates();
    void keepsDistantColumnsAsSeparateCandidates();
    void keepsDifferentFontFacesAsSeparateCandidates();
    void editsGroupedLineAsOneDraft();
    void changesFontFamilyForCurrentEdit();
    void appliesBoldToDisplayFont_data();
    void appliesBoldToDisplayFont();

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

void PDFEditControllerTest::detectsRegularAndBoldPdfFonts()
{
    QCOMPARE(m_regularCandidate.value(QStringLiteral("fontBold")).toBool(), false);
    QCOMPARE(m_boldCandidate.value(QStringLiteral("fontBold")).toBool(), true);
}

void PDFEditControllerTest::propagatesBoldThroughSelection()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(m_controller.selectedFontBold(), false);

    QVERIFY(m_controller.selectCandidate(
        0,
        m_boldCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(m_controller.selectedFontBold(), true);
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

void PDFEditControllerTest::appliesBoldToDisplayFont_data()
{
    QTest::addColumn<bool>("fontBold");

    QTest::newRow("regular") << false;
    QTest::newRow("bold") << true;
}

void PDFEditControllerTest::appliesBoldToDisplayFont()
{
    QFETCH(bool, fontBold);

    const QFont displayFont = m_controller.fontForDisplay(
        QString(),
        QFont::Unstretched,
        fontBold,
        18.0);

    QCOMPARE(displayFont.bold(), fontBold);
    QCOMPARE(displayFont.pixelSize(), 18);
    QCOMPARE(displayFont.stretch(), static_cast<int>(QFont::Unstretched));
}

QTEST_MAIN(PDFEditControllerTest)

#include "tst_PDFEditController.moc"
