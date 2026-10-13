#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QStandardPaths>
#include <QUuid>

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::initTestCase()
{
    QVERIFY2(m_temporaryDirectory.isValid(), "Could not create the test directory");
    const QString cacheProbeApp = qEnvironmentVariable("TREPDF_FONT_CACHE_PROBE_APP");
    if (!cacheProbeApp.isEmpty()) {
        const QString prefix = QStringLiteral("TrePDF-font-test-");
        QVERIFY(cacheProbeApp.startsWith(prefix));
        QVERIFY(!QUuid(cacheProbeApp.mid(prefix.size())).isNull());
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(cacheProbeApp);
    } else {
        m_originalApplicationName = QCoreApplication::applicationName();
        m_originalTestMode = QStandardPaths::isTestModeEnabled();
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("TrePDF-fixture-test-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString root = QFileInfo(ApplicationFonts::downloadedFontDirectory()).absolutePath();
        QCOMPARE(QFileInfo(root).fileName(), QCoreApplication::applicationName());
        QVERIFY(!QFileInfo::exists(root));
        m_fontStoreRoot = root;
    }
    ApplicationFonts::registerBundledFonts();
    if (!cacheProbeApp.isEmpty())
        return;

    QString fontError;
    QVERIFY2(installTestFonts(fontError), qPrintable(fontError));

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

void PDFEditControllerTest::cleanupTestCase()
{
    if (m_fontStoreRoot.isEmpty())
        return;
    m_controller.setDocument(nullptr);
    m_document.close();
    QVERIFY(QDir(m_fontStoreRoot).removeRecursively());
    QCoreApplication::setApplicationName(m_originalApplicationName);
    QStandardPaths::setTestModeEnabled(m_originalTestMode);
}

QTEST_MAIN(PDFEditControllerTest)
