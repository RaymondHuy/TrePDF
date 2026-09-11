#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

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

void PDFEditControllerTest::preservesIccBasedTextColorWhenEditingSibling()
{
    const QString untouchedText = QStringLiteral("Untouched ICC blue");
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("icc-text-edit-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("icc-text-edit-output.pdf"));
    QVERIFY(createIccBasedTextPdf(sourcePath));

    const auto verifyUntouchedBlueText = [&untouchedText](
                                             const QString &filePath) {
        const TextPaintInspection inspection = inspectTopLevelTextPaint(
            filePath,
            untouchedText);
        QVERIFY(inspection.readable);
        QVERIFY(inspection.found);
        QCOMPARE(inspection.objectType, FPDF_PAGEOBJ_TEXT);
        QCOMPARE(inspection.renderMode, FPDF_TEXTRENDERMODE_FILL);
        QVERIFY(inspection.hasFillColor);
        QVERIFY2(
            qAbs(inspection.fillColor.red() - 0) <= 2
                && qAbs(inspection.fillColor.green() - 111) <= 2
                && qAbs(inspection.fillColor.blue() - 192) <= 2
                && inspection.fillColor.alpha() == 255,
            qPrintable(QStringLiteral("Unexpected text fill: %1")
                           .arg(inspection.fillColor.name(QColor::HexArgb))));
        QVERIFY(normalizedPageText(filePath).contains(untouchedText));
    };
    verifyUntouchedBlueText(sourcePath);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QStringLiteral("Editable sibling"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("Edited sibling"));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    verifyUntouchedBlueText(outputPath);
    QVERIFY(normalizedPageText(outputPath).contains(
        QStringLiteral("Edited sibling")));
    const QVariantMap savedCandidate = candidateForText(
        waitForCandidates(controller, 0),
        untouchedText);
    QVERIFY(!savedCandidate.isEmpty());
    QCOMPARE(
        savedCandidate.value(QStringLiteral("kind")).toString(),
        QStringLiteral("text"));

    const QString verificationPath = qEnvironmentVariable(
        "TREPDF_ICC_TEXT_TEST_OUTPUT");
    if (!verificationPath.isEmpty()) {
        QFile::remove(verificationPath);
        QVERIFY(QFile::copy(outputPath, verificationPath));
    }
}

void PDFEditControllerTest::preservesIccBasedTextColorWhenAddingText()
{
    const QString untouchedText = QStringLiteral("Untouched ICC blue");
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("icc-text-add-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("icc-text-add-output.pdf"));
    QVERIFY(createIccBasedTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(!controller.addText(0, 0.2, 0.55).isEmpty());
    controller.updateSelectedTextPreview(QStringLiteral("Added text"));
    controller.finishSelectedTextEdit();

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const TextPaintInspection inspection = inspectTopLevelTextPaint(
        outputPath,
        untouchedText);
    QVERIFY(inspection.readable);
    QVERIFY(inspection.found);
    QCOMPARE(inspection.objectType, FPDF_PAGEOBJ_TEXT);
    QCOMPARE(inspection.renderMode, FPDF_TEXTRENDERMODE_FILL);
    QVERIFY(inspection.hasFillColor);
    QVERIFY(qAbs(inspection.fillColor.red() - 0) <= 2);
    QVERIFY(qAbs(inspection.fillColor.green() - 111) <= 2);
    QVERIFY(qAbs(inspection.fillColor.blue() - 192) <= 2);
    QCOMPARE(inspection.fillColor.alpha(), 255);
    const QString savedText = normalizedPageText(outputPath);
    QVERIFY(savedText.contains(untouchedText));
    QVERIFY(savedText.contains(QStringLiteral("Added text")));
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

void PDFEditControllerTest::savesJustifiedWordPositions_data()
{
    QTest::addColumn<bool>("added");
    QTest::addColumn<bool>("mixedSpans");
    QTest::addColumn<QString>("text");
    const QString text = QStringLiteral(
        "Alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu");
    QTest::newRow("source-font") << false << false << text;
    QTest::newRow("added-font") << true << false << text;
    QTest::newRow("mixed-spans") << true << true << text;
    QTest::newRow("spaces-tabs-hard-break") << true << false << QStringLiteral(
        "Alpha  beta\tgamma delta epsilon zeta eta theta iota kappa lambda mu\n"
        "Last paragraph stays short\nShort\tend");
}

void PDFEditControllerTest::savesJustifiedWordPositions()
{
    QFETCH(bool, added);
    QFETCH(bool, mixedSpans);
    QFETCH(QString, text);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("justify-%1-source.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("justify-%1-output.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString sourceText = added ? QStringLiteral("Original text") : text;
    QVERIFY(createSourceParagraphPdf(sourcePath, {
        {sourceText, 72.0, 740.0, "Helvetica", 1.0, 12.0F}}));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), sourceText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    if (added) {
        controller.deleteSelectedObject();
        QVERIFY(!controller.addText(0, 0.12, 0.2).isEmpty());
        controller.updateSelectedTextPreview(text);
        controller.setSelectedTextRange(0, text.size());
        controller.setSelectedFontSize(12.0);
    } else {
        controller.beginSelectedTextEdit();
    }
    if (mixedSpans) {
        // A format boundary within a word must survive positioning that word.
        controller.setSelectedTextRange(text.indexOf(QStringLiteral("gamma")) + 2, 2);
        controller.setSelectedColor(Qt::red);
    }
    controller.setSelectedTextRange(0, text.size());
    controller.setSelectedParagraphAlignment(PDFEditController::JustifyAlignment);
    const QSizeF pageSize = document.pageSize(0);
    const qreal contentLeft = (controller.selectedHasBaseline()
        ? controller.selectedOriginX() : controller.selectedFrameRect().left()) * pageSize.width();
    constexpr qreal layoutWidth = 175.0;
    QRectF frame = controller.selectedFrameRect();
    frame.setRight((contentLeft + layoutWidth) / pageSize.width());
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(frame);
    controller.finishSelectedFrameResize();
    const QFont font = controller.fontForDisplay(
        controller.selectedFontFamily(), controller.selectedFontStretch(),
        controller.selectedFontBold(), controller.selectedFontItalic(),
        controller.selectedFontUnderline(),
        controller.selectedDisplayFontSizeRatio() * pageSize.height());
    controller.finishSelectedTextEdit();

    // Compare each word's saved first-glyph origin with Qt's justified layout,
    // rather than checking only the line's overall bounding box.
    QVector<QPair<qsizetype, qreal>> expectedStarts;
    qsizetype denseIndex = 0;
    int wrappedLines = 0;
    for (const QString &paragraph : text.split(QLatin1Char('\n'))) {
        QTextLayout layout(paragraph, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(Qt::AlignJustify);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(layoutWidth);
            ++wrappedLines;
            const int end = line.textStart() + line.textLength();
            const bool positionsSeparated = end < paragraph.size()
                || paragraph.mid(line.textStart(), line.textLength()).contains(QLatin1Char('\t'));
            for (int index = line.textStart(); index < end; ++index) {
                if (paragraph[index].isSpace()) {
                    continue;
                }
                if (index == line.textStart()
                    || (positionsSeparated && paragraph[index - 1].isSpace())) {
                    expectedStarts.append({denseIndex, contentLeft + line.cursorToX(index)});
                }
                ++denseIndex;
            }
        }
        layout.endLayout();
    }
    QVERIFY(wrappedLines >= 3);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QByteArray encodedPath = QFile::encodeName(outputPath);
    FPDF_DOCUMENT saved = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    QVERIFY(saved);
    const auto closeDocument = qScopeGuard([&]() { FPDF_CloseDocument(saved); });
    FPDF_PAGE page = FPDF_LoadPage(saved, 0);
    QVERIFY(page);
    const auto closePage = qScopeGuard([&]() { FPDF_ClosePage(page); });
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    QVERIFY(textPage);
    const auto closeTextPage = qScopeGuard([&]() { FPDFText_ClosePage(textPage); });
    QVector<QPointF> origins;
    QString savedDenseText;
    for (int index = 0; index < FPDFText_CountChars(textPage); ++index) {
        const QChar character(static_cast<char16_t>(FPDFText_GetUnicode(textPage, index)));
        if (character.isSpace()) {
            continue;
        }
        double x = 0.0;
        double y = 0.0;
        QVERIFY(FPDFText_GetCharOrigin(textPage, index, &x, &y));
        origins.append(QPointF(x, y));
        savedDenseText.append(character);
    }
    QString denseText = text;
    denseText.remove(QRegularExpression(QStringLiteral("\\s")));
    QCOMPARE(savedDenseText, denseText);
    for (const auto &[index, expectedX] : expectedStarts) {
        QVERIFY2(qAbs(origins.at(index).x() - expectedX) < 0.1,
                 qPrintable(QStringLiteral("Word at %1: expected x=%2, saved x=%3")
                     .arg(index).arg(expectedX).arg(origins.at(index).x())));
    }
    if (!added) {
        FS_MATRIX matrix{};
        QVERIFY(textObjectMatrix(outputPath, QStringLiteral("Alpha"), matrix));
        QVERIFY(qAbs(horizontalScale(matrix) - 1.0) < 0.0001);
    } else {
        FS_MATRIX matrix{};
        QVERIFY(textObjectMatrix(outputPath, QStringLiteral("iota"), matrix));
        QVERIFY(qAbs(horizontalScale(matrix) - 1.0) < 0.0001);
    }
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

void PDFEditControllerTest::shortensSavedTextUnderline_data()
{
    addTextUnderlineFixtureRows();
}

void PDFEditControllerTest::shortensSavedTextUnderline()
{
    QFETCH(bool, legacy);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("shorten-text-underline-%1.pdf").arg(legacy));
    const QString outputPath = sourcePath + QStringLiteral(".shortened.pdf");
    QVERIFY(createSavedTextUnderlinePdf(sourcePath, legacy));
    const TextUnderlineInspection original = inspectTextUnderlinePaths(sourcePath);
    QVERIFY(original.readable);
    QCOMPARE(original.textCount, 1);
    QCOMPARE(original.underlines.size(), 1);
    QCOMPARE(original.unrelatedLines.size(), 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Hello world"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(candidate.value(QStringLiteral("fontUnderline")).toBool());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedFontUnderline());
    controller.commitSelectedText(QStringLiteral("Hello"));

    const QVector<QVector<int>> suppressed
        = controller.draftedTextObjectPathsForPage(0);
    QCOMPARE(suppressed.size(), 2);
    QVERIFY(suppressed.contains(QVector<int>{original.textIndex}));
    QVERIFY(suppressed.contains(QVector<int>{original.underlineIndexes.constFirst()}));
    QVERIFY(controller.allDraftedTextObjectsSuppressibleForPage(0));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const TextUnderlineInspection saved = inspectTextUnderlinePaths(outputPath);
    QVERIFY(saved.readable);
    QCOMPARE(saved.textCount, 1);
    QCOMPARE(saved.underlines.size(), 1);
    QVERIFY(saved.underlines.constFirst().length() > 1.0);
    QVERIFY(saved.underlines.constFirst().length()
        < original.underlines.constFirst().length() * 0.7);
    QCOMPARE(saved.unrelatedLines, original.unrelatedLines);
    QCOMPARE(normalizedPageText(outputPath), QStringLiteral("Hello"));
    const QVariantMap reopened = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Hello"));
    QVERIFY(reopened.value(QStringLiteral("fontUnderline")).toBool());
}

void PDFEditControllerTest::deletesSavedTextUnderlineWithUndoRedoDiscard_data()
{
    QTest::addColumn<bool>("legacy");
    QTest::addColumn<bool>("inlineDeletion");
    QTest::newRow("marked-delete-object") << false << false;
    QTest::newRow("legacy-delete-object") << true << false;
    QTest::newRow("marked-clear-editor") << false << true;
    QTest::newRow("legacy-clear-editor") << true << true;
}

void PDFEditControllerTest::deletesSavedTextUnderlineWithUndoRedoDiscard()
{
    QFETCH(bool, legacy);
    QFETCH(bool, inlineDeletion);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-text-underline-%1.pdf").arg(legacy));
    const QString outputPath = sourcePath + QStringLiteral(".deleted.pdf");
    QVERIFY(createSavedTextUnderlinePdf(sourcePath, legacy));
    const TextUnderlineInspection original = inspectTextUnderlinePaths(sourcePath);
    QCOMPARE(original.underlines.size(), 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Hello world"));
    const QString objectId = candidate.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectCandidate(0, objectId));
    QVERIFY(controller.selectedFontUnderline());
    const auto deleteText = [&controller, inlineDeletion]() {
        if (inlineDeletion) {
            controller.beginSelectedTextEdit();
            controller.updateSelectedTextPreview(QString());
            controller.finishSelectedTextEdit();
        } else {
            controller.deleteSelectedObject();
        }
    };
    deleteText();
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 2);
    QVERIFY(controller.allDraftedTextObjectsSuppressibleForPage(0));
    QVERIFY(!controller.selectCandidate(0, objectId));

    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.draftedTextObjectPathsForPage(0).isEmpty());
    QVERIFY(controller.selectCandidate(0, objectId));
    QVERIFY(controller.selectedFontUnderline());
    controller.redo();
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 2);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.draftedTextObjectPathsForPage(0).isEmpty());
    QVERIFY(controller.selectCandidate(0, objectId));
    QVERIFY(controller.selectedFontUnderline());

    deleteText();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const TextUnderlineInspection saved = inspectTextUnderlinePaths(outputPath);
    QVERIFY(saved.readable);
    QCOMPARE(saved.textCount, 0);
    QVERIFY(saved.underlines.isEmpty());
    QCOMPARE(saved.unrelatedLines, original.unrelatedLines);
    QVERIFY(normalizedPageText(outputPath).isEmpty());
}

void PDFEditControllerTest::disablesSavedTextUnderline_data()
{
    addTextUnderlineFixtureRows();
}

void PDFEditControllerTest::disablesSavedTextUnderline()
{
    QFETCH(bool, legacy);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("disable-text-underline-%1.pdf").arg(legacy));
    const QString outputPath = sourcePath + QStringLiteral(".disabled.pdf");
    QVERIFY(createSavedTextUnderlinePdf(sourcePath, legacy));
    const TextUnderlineInspection original = inspectTextUnderlinePaths(sourcePath);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Hello world"));
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.setSelectedTextRange(0, 11);
    QVERIFY(controller.selectedFontUnderline());
    controller.setSelectedFontUnderline(false);
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.selectedFontUnderline());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 2);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const TextUnderlineInspection saved = inspectTextUnderlinePaths(outputPath);
    QVERIFY(saved.readable);
    QCOMPARE(saved.textCount, 1);
    QVERIFY(saved.underlines.isEmpty());
    QCOMPARE(saved.unrelatedLines, original.unrelatedLines);
    QCOMPARE(normalizedPageText(outputPath), QStringLiteral("Hello world"));
    const QVariantMap reopened = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Hello world"));
    QVERIFY(!reopened.isEmpty());
    QVERIFY(!reopened.value(QStringLiteral("fontUnderline")).toBool());
}

void PDFEditControllerTest::movesSavedTextUnderline_data()
{
    addTextUnderlineFixtureRows();
}

void PDFEditControllerTest::movesSavedTextUnderline()
{
    QFETCH(bool, legacy);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-text-underline-%1.pdf").arg(legacy));
    const QString outputPath = sourcePath + QStringLiteral(".moved.pdf");
    QVERIFY(createSavedTextUnderlinePdf(sourcePath, legacy));
    const TextUnderlineInspection original = inspectTextUnderlinePaths(sourcePath);
    QCOMPARE(original.underlines.size(), 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Hello world"));
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    const QRectF originalFrame = controller.selectedFrameRect();
    const QPointF translation(0.05, 0.06);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(originalFrame.translated(translation));
    controller.finishSelectedFrameResize();
    QVERIFY(controller.draftedTextObjectPathsForPage(0).isEmpty());
    const QVector<PDFEditController::DraftObjectMove> moves
        = controller.draftedObjectMovesForPage(0);
    QCOMPARE(moves.size(), 2);
    QCOMPARE(std::count_if(moves.cbegin(), moves.cend(), [](const auto &move) {
        return move.kind == PDFEditController::DraftObjectMove::Kind::Text;
    }), 1);
    QCOMPARE(std::count_if(moves.cbegin(), moves.cend(), [](const auto &move) {
        return move.kind == PDFEditController::DraftObjectMove::Kind::Path;
    }), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    const TextUnderlineInspection saved = inspectTextUnderlinePaths(outputPath);
    QVERIFY(saved.readable);
    QCOMPARE(saved.underlines.size(), 1);
    QCOMPARE(saved.unrelatedLines, original.unrelatedLines);
    const QPointF pdfTranslation(translation.x() * 595.0,
                                 -translation.y() * 842.0);
    const QLineF expected = original.underlines.constFirst().translated(pdfTranslation);
    // Save maps coordinates at 1/64 pt; independently rounded endpoints can
    // differ from the ideal delta by up to 1/64 pt per axis.
    QVERIFY(QLineF(saved.underlines.constFirst().p1(), expected.p1()).length() < 0.03);
    QVERIFY(QLineF(saved.underlines.constFirst().p2(), expected.p2()).length() < 0.03);
    const QPointF textDelta = saved.textOrigin - original.textOrigin;
    const QPointF underlineDelta = saved.underlines.constFirst().p1()
        - original.underlines.constFirst().p1();
    QVERIFY(QLineF(textDelta, underlineDelta).length() < 0.001);
    const QVariantMap reopened = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Hello world"));
    QVERIFY(reopened.value(QStringLiteral("fontUnderline")).toBool());
}

void PDFEditControllerTest::preservesUnrelatedPathsNearText_data()
{
    QTest::addColumn<QByteArray>("pathCommands");
    QTest::newRow("different-color")
        << QByteArray("q 1 0 0 RG 1.2 w 72 637.6 m 190.68 637.6 l S Q\n");
    QTest::newRow("different-width")
        << QByteArray("q 0 0 0 RG 1.2 w 72 637.6 m 300 637.6 l S Q\n");
    QTest::newRow("dashed-line")
        << QByteArray("q 0 0 0 RG 1.2 w [3 2] 0 d 72 637.6 m 190.68 637.6 l S Q\n");
}

void PDFEditControllerTest::preservesUnrelatedPathsNearText()
{
    QFETCH(QByteArray, pathCommands);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("unrelated-text-path-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = sourcePath + QStringLiteral(".deleted.pdf");
    QVERIFY(createTextUnderlineSourcePdf(sourcePath, pathCommands));
    const TextUnderlineInspection original = inspectTextUnderlinePaths(sourcePath);
    QCOMPARE(original.underlines.size(), 1);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Hello world"));
    QVERIFY(!candidate.value(QStringLiteral("fontUnderline")).toBool());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.deleteSelectedObject();
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const TextUnderlineInspection saved = inspectTextUnderlinePaths(outputPath);
    QVERIFY(saved.readable);
    QCOMPARE(saved.textCount, 0);
    QCOMPARE(saved.underlines, original.underlines);
    QCOMPARE(saved.unrelatedLines, original.unrelatedLines);
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
