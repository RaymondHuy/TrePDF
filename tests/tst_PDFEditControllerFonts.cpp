#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

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
