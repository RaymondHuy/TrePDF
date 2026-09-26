#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFFontAliases.h"

using namespace PDFEditControllerTestSupport;

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

void PDFEditControllerTest::groupsMixedFontLineWithHistoryAndSave_data()
{
    QTest::addColumn<bool>("differentFamilies");
    QTest::newRow("same-family-emphasis-and-stretch") << false;
    QTest::newRow("different-families-emphasis-and-stretch") << true;
}

void PDFEditControllerTest::groupsMixedFontLineWithHistoryAndSave()
{
    QFETCH(bool, differentFamilies);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-line-%1.pdf").arg(differentFamilies));
    // Deliberately shuffled PDF object order. The ink bounds are disjoint, but
    // all four fragments share one visual baseline like the reported title.
    QVERIFY(createSourceParagraphPdf(sourcePath, {
        {QStringLiteral("other "), 131.0, 740.0,
         differentFamilies ? "Times-Italic" : "Helvetica-Oblique", 1.07, 12.0F},
        {QStringLiteral("Job "), 72.0, 740.0, "Helvetica-Bold", 1.0, 12.0F},
        {QStringLiteral("font"), 167.0, 740.0,
         differentFamilies ? "Courier-Bold" : "Helvetica-Bold", 1.13, 12.0F},
        {QStringLiteral("Title "), 98.0, 740.0, "Helvetica-BoldOblique", 1.06, 12.0F}}));
    const QStringList originalObjects = topLevelTextObjects(sourcePath);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString original = QStringLiteral("Job Title other font");
    const QVariantList candidates = waitForCandidates(controller, 0);
    QCOMPARE(candidates.size(), 1);
    const QVariantMap candidate = candidateForText(candidates, original);
    QVERIFY(!candidate.isEmpty());
    QCOMPARE(candidate.value(QStringLiteral("sourceRects")).toList().size(), 4);
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const auto formatAt = [&controller](qsizetype position) {
        for (const QVariant &value : controller.selectedCharacterSpans()) {
            const QVariantMap span = value.toMap();
            const qsizetype start = span.value(QStringLiteral("start")).toLongLong();
            const qsizetype end = start + span.value(QStringLiteral("length")).toLongLong();
            if (position >= start && position < end) {
                return span;
            }
        }
        return QVariantMap();
    };
    QVector<QVariantMap> formats;
    for (qsizetype position : {0, 4, 10, 16}) {
        formats.append(formatAt(position));
        QVERIFY(!formats.last().isEmpty());
    }
    QVERIFY(formats[0].value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!formats[0].value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(formats[1].value(QStringLiteral("fontBold")).toBool());
    QVERIFY(formats[1].value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(!formats[2].value(QStringLiteral("fontBold")).toBool());
    QVERIFY(formats[2].value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(formats[3].value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!formats[3].value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(formats[3].value(QStringLiteral("fontStretch")).toInt()
            - formats[0].value(QStringLiteral("fontStretch")).toInt() > 5);
    QCOMPARE(formats[0].value(QStringLiteral("fontFamily"))
                 != formats[2].value(QStringLiteral("fontFamily")), differentFamilies);
    QCOMPARE(formats[0].value(QStringLiteral("fontFamily"))
                 != formats[3].value(QStringLiteral("fontFamily")), differentFamilies);
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    const QRectF originalFrame = controller.selectedFrameRect();
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(original);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    QString edited = original + QStringLiteral(" a");
    controller.commitSelectedText(edited);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 4);
    const QVariantList editedSpans = controller.selectedCharacterSpans();
    controller.undo();
    QCOMPARE(controller.selectedText(), original);
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(controller.selectedText(), edited);
    QCOMPARE(controller.selectedCharacterSpans(), editedSpans);
    for (int cycle = 0; cycle < 2; ++cycle) {
        const QString savedPath = m_temporaryDirectory.filePath(
            QStringLiteral("mixed-line-%1-saved-%2.pdf").arg(differentFamilies).arg(cycle));
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
        const QVariantList reopened = waitForCandidates(controller, 0);
        QCOMPARE(reopened.size(), 1);
        const QVariantMap restored = candidateForText(reopened, edited);
        QVERIFY(!restored.isEmpty());
        QVERIFY(restored.value(QStringLiteral("objectId")).toString()
                    .startsWith(QStringLiteral("paragraph-layout:")));
        QVERIFY(controller.selectCandidate(0, restored.value(QStringLiteral("objectId")).toString()));
        for (int index = 0; index < 4; ++index) {
            const QVariantMap format = formatAt(QVector<int>{0, 4, 10, 16}[index]);
            QCOMPARE(PDFFontAliases::semanticFamily(format.value(QStringLiteral("fontFamily")).toString()),
                     PDFFontAliases::semanticFamily(formats[index].value(QStringLiteral("fontFamily")).toString()));
            for (const QString &key : {QStringLiteral("fontBold"),
                                       QStringLiteral("fontItalic"), QStringLiteral("fontStretch")}) {
                QCOMPARE(format.value(key), formats[index].value(key));
            }
        }
        const QVariantList restoredSpans = controller.selectedCharacterSpans();
        const QRectF restoredFrame = controller.selectedFrameRect();
        controller.beginSelectedTextEdit();
        controller.updateSelectedTextPreview(edited);
        controller.finishSelectedTextEdit();
        QCOMPARE(controller.selectedCharacterSpans(), restoredSpans);
        QCOMPARE(controller.selectedFrameRect(), restoredFrame);
        QVERIFY(!controller.hasDrafts());
        if (cycle == 0) {
            edited += QStringLiteral(" a");
            controller.commitSelectedText(edited);
        }
    }
    QCOMPARE(topLevelTextObjects(sourcePath), originalObjects);
}

void PDFEditControllerTest::keepsUnsafeMixedFontLineSeparate_data()
{
    QTest::addColumn<QByteArray>("secondTransform");
    QTest::addColumn<QByteArray>("extraContent");
    QTest::newRow("overlap") << QByteArray("1 0 0 1 80 740") << QByteArray();
    QTest::newRow("distant-column") << QByteArray("1 0 0 1 300 740") << QByteArray();
    QTest::newRow("different-color") << QByteArray("1 0 0 1 98 740") << QByteArray("1 0 0 rg\n");
    QTest::newRow("source-obstacle") << QByteArray("1 0 0 1 98 740")
                                   << QByteArray("q 2 0 0 12 94 738 cm "
                                                 "BI /W 1 /H 1 /CS /RGB /BPC 8 ID abc EI Q\n");
}

void PDFEditControllerTest::keepsUnsafeMixedFontLineSeparate()
{
    QFETCH(QByteArray, secondTransform);
    QFETCH(QByteArray, extraContent);
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("unsafe-mixed-line-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QByteArray content = QByteArray("BT /F1 12 Tf 1 0 0 1 72 740 Tm (Job) Tj ET\n")
        + extraContent + "BT /F2 12 Tf " + secondTransform + " Tm (Title) Tj ET\n";
    QVERIFY(writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << /Font << /F1 4 0 R /F2 6 0 R >> >> /Contents 5 0 R >>"),
        QByteArray("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream",
        QByteArray("<< /Type /Font /Subtype /Type1 /BaseFont /Times-Italic /Encoding /WinAnsiEncoding >>")}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    QVERIFY(!candidateForText(candidates, QStringLiteral("Job")).isEmpty());
    QVERIFY(!candidateForText(candidates, QStringLiteral("Title")).isEmpty());
}

void PDFEditControllerTest::groupsCompatibleSourceTextLinesIntoParagraph()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-discovery.pdf"));
    // PDF object order is not guaranteed to follow visual reading order.
    QVERIFY(createSourceParagraphPdf(sourcePath, {
        {QStringLiteral("Paragraph second line"), 72.0, 710.0},
        {QStringLiteral("world"), 140.0, 740.0},
        {QStringLiteral("Last paragraph line"), 72.0, 680.0},
        {QStringLiteral("Hello"), 72.0, 740.0}}));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kSourceParagraphText));
    QVERIFY2(!candidate.isEmpty(), "Multiline source paragraph was not discovered");
    QVERIFY(candidate.value(QStringLiteral("grouped")).toBool());
    QVERIFY(candidate.value(QStringLiteral("editable")).toBool());
    const QString objectId = candidate.value(QStringLiteral("objectId")).toString();
    QVERIFY(objectId.startsWith(QStringLiteral("paragraph:")));
    const QVariantList sourceRects = candidate.value(QStringLiteral("sourceRects")).toList();
    QCOMPARE(sourceRects.size(), 4);
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedText(), QString::fromLatin1(kSourceParagraphText));
    QCOMPARE(controller.selectedSourceRects().size(), 4);

    const QRectF sourceRect = controller.selectedSourceRect();
    const QRectF frameRect = controller.selectedFrameRect();
    QVERIFY(frameRect.contains(sourceRect));
    const QSizeF pageSize = document.pageSize(0);
    const QFont displayFont = controller.fontForDisplay(
        candidate.value(QStringLiteral("fontFamily")).toString(),
        candidate.value(QStringLiteral("fontStretch")).toInt(),
        candidate.value(QStringLiteral("fontBold")).toBool(),
        candidate.value(QStringLiteral("fontItalic")).toBool(),
        candidate.value(QStringLiteral("fontUnderline")).toBool(),
        candidate.value(QStringLiteral("displayFontSizeRatio")).toReal()
            * pageSize.height());
    const QFontMetricsF metrics(displayFont);
    const qreal availableWidth = (frameRect.right()
        - candidate.value(QStringLiteral("originX")).toReal()) * pageSize.width();
    qreal combinedLineWidth = 0.0;
    for (const QString &line : controller.selectedText().split(QLatin1Char('\n'))) {
        const qreal lineWidth = qMax(
            metrics.horizontalAdvance(line), metrics.boundingRect(line).width());
        QVERIFY(availableWidth >= lineWidth);
        combinedLineWidth += lineWidth;
    }
    // Initial sizing fits each row without measuring the paragraph as one long row.
    QVERIFY(availableWidth < combinedLineWidth * 0.75);
    for (const QVariant &value : sourceRects) {
        const QRectF partRect = rectFromCoordinateMap(value);
        // Reconstructing bottom/right after QRectF::united can differ by one ulp.
        QVERIFY2(sourceRect.adjusted(-1e-12, -1e-12, 1e-12, 1e-12).contains(partRect),
                 qPrintable(QStringLiteral("Paragraph [%1,%2,%3,%4], part [%5,%6,%7,%8]")
                     .arg(sourceRect.x(), 0, 'g', 17).arg(sourceRect.y(), 0, 'g', 17)
                     .arg(sourceRect.width(), 0, 'g', 17).arg(sourceRect.height(), 0, 'g', 17)
                     .arg(partRect.x(), 0, 'g', 17).arg(partRect.y(), 0, 'g', 17)
                     .arg(partRect.width(), 0, 'g', 17).arg(partRect.height(), 0, 'g', 17)));
        const QPointF hit = partRect.center();
        QCOMPARE(controller.candidateAt(0, hit.x(), hit.y())
                     .value(QStringLiteral("objectId")).toString(),
                 objectId);
    }
}

void PDFEditControllerTest::groupsSourceParagraphWithFractionalTransforms_data()
{
    QTest::addColumn<double>("secondScale");
    QTest::addColumn<bool>("grouped");
    QTest::newRow("same-stretch-at-fractional-origins") << 0.99999046 << true;
    QTest::newRow("one-percent-stretch-difference") << 1.008 << true;
    QTest::newRow("different-source-stretch") << 1.03 << false;
}

void PDFEditControllerTest::groupsSourceParagraphWithFractionalTransforms()
{
    QFETCH(double, secondScale);
    QFETCH(bool, grouped);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-fractional-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString address = QStringLiteral("123 Somewhere St, Melbourne VIC 3000");
    const QString phone = QStringLiteral("(03) 1234 5678");
    // Identical matrix axes at these invoice-style origins used to produce
    // stretches of 99 and 100 after rounding the one-point device vectors.
    QVERIFY(createSourceParagraphPdf(sourcePath, {
        {address, 24.5622654, 555.24945068, "Times-Bold", 0.99999046, 15.0F},
        {phone, 23.70289803, 536.89013672, "Times-Bold", secondScale, 15.0F}}));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    if (grouped) {
        QCOMPARE(candidates.size(), 1);
        const QString paragraph = address + QLatin1Char('\n') + phone;
        const QVariantMap candidate = candidateForText(candidates, paragraph);
        QVERIFY(!candidate.isEmpty());
        QVERIFY(candidate.value(QStringLiteral("grouped")).toBool());
        QCOMPARE(candidate.value(QStringLiteral("fontStretch")).toInt(), 100);
        QCOMPARE(candidate.value(QStringLiteral("sourceRects")).toList().size(), 2);
        QVERIFY(controller.selectCandidate(
            0, candidate.value(QStringLiteral("objectId")).toString()));
        QCOMPARE(controller.selectedText(), paragraph);
    } else {
        QCOMPARE(candidates.size(), 2);
        for (const QString &text : {address, phone}) {
            const QVariantMap candidate = candidateForText(candidates, text);
            QVERIFY(!candidate.isEmpty());
            QVERIFY(!candidate.value(QStringLiteral("grouped")).toBool());
        }
    }
}

void PDFEditControllerTest::preservesSourceLineEndHyphens_data()
{
    QTest::addColumn<bool>("separateHyphenObject");
    QTest::newRow("hyphen-in-line-object") << false;
    QTest::newRow("separate-hyphen-object") << true;
}

void PDFEditControllerTest::preservesSourceLineEndHyphens()
{
    QFETCH(bool, separateHyphenObject);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-line-end-hyphens-%1.pdf").arg(separateHyphenObject));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("saved-line-end-hyphens-%1.pdf").arg(separateHyphenObject));
    const QByteArray lineEnd = separateHyphenObject ? ") Tj (-) Tj" : "-) Tj";
    const QByteArray content = QByteArray("BT /F1 12 Tf 1 0 0 1 72 740 Tm (The night")
        + lineEnd + " ET\nBT /F1 12 Tf 1 0 0 1 72 726 Tm (time economy and QD"
        + lineEnd + " ET\nBT /F1 12 Tf 1 0 0 1 72 712 Tm (TTg 2020) Tj ET\n";
    QVERIFY(writePdfObjects(sourcePath, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        QByteArray("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                   "/Encoding /MacRomanEncoding >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream"}));
    // Prove this is PDFium's extraction marker, not an actual U+0002 encoded
    // by the fixture. Both visible '-' glyphs become flagged line-end markers.
    const auto markedHyphenCount = [](const QString &path) {
        const QByteArray encoded = QFile::encodeName(path);
        FPDF_DOCUMENT pdf = FPDF_LoadDocument(encoded.constData(), nullptr);
        FPDF_PAGE page = pdf ? FPDF_LoadPage(pdf, 0) : nullptr;
        FPDF_TEXTPAGE textPage = page ? FPDFText_LoadPage(page) : nullptr;
        int count = 0;
        if (textPage) {
            for (int index = 0; index < FPDFText_CountChars(textPage); ++index) {
                if (FPDFText_GetUnicode(textPage, index) == 0x0002U
                    && FPDFText_IsHyphen(textPage, index) == 1) {
                    ++count;
                }
            }
            FPDFText_ClosePage(textPage);
        }
        if (page) FPDF_ClosePage(page);
        if (pdf) FPDF_CloseDocument(pdf);
        return count;
    };
    QCOMPARE(markedHyphenCount(sourcePath), 2);
    const QString original = QStringLiteral("The night-\ntime economy and QD-\nTTg 2020");
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY2(!candidate.isEmpty(), "Editable text must restore visible hyphens, not U+0002 markers");
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(original);
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());

    // Reuse the same font/character repertoire to exercise the source-font
    // save path that previously accepted U+0002 and wrote 0xFF silently.
    const QString edited = QStringLiteral("The night-\ntime economy and QD-\nTTg 2022");
    controller.commitSelectedText(edited);
    controller.undo();
    QCOMPARE(controller.selectedText(), original);
    controller.redo();
    QCOMPARE(controller.selectedText(), edited);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!candidateForText(waitForCandidates(controller, 0), edited).isEmpty());
    QCOMPARE(markedHyphenCount(outputPath), 2);
    QVERIFY(!pageText(outputPath).contains(QChar(0x02c7)));
    QCOMPARE(markedHyphenCount(sourcePath), 2);
}

void PDFEditControllerTest::groupsMixedSourceParagraphWithStylesAndHistory()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-source-paragraph-history.pdf"));
    const QVector<SourceParagraphTextPart> parts = mixedSourceParagraphParts();
    const QString paragraph = sourceParagraphText(parts, 0, 6);
    const QString nextParagraph = sourceParagraphText(parts, 6, 2);
    QVERIFY(createSourceParagraphPdf(sourcePath, parts));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    QCOMPARE(candidates.size(), 2);
    const QVariantMap candidate = candidateForText(candidates, paragraph);
    QVERIFY2(!candidate.isEmpty(), "The bold first line and shorter final line must join the body");
    QCOMPARE(candidate.value(QStringLiteral("sourceRects")).toList().size(), 6);
    const QVariantMap nextCandidate = candidateForText(candidates, nextParagraph);
    QVERIFY2(!nextCandidate.isEmpty(), "The larger baseline gap must keep the next paragraph separate");
    QCOMPARE(nextCandidate.value(QStringLiteral("sourceRects")).toList().size(), 2);
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    const auto formatAt = [&controller](qsizetype position) {
        for (const QVariant &value : controller.selectedCharacterSpans()) {
            const QVariantMap span = value.toMap();
            const qsizetype start = span.value(QStringLiteral("start")).toLongLong();
            const qsizetype length = span.value(QStringLiteral("length")).toLongLong();
            if (position >= start && position < start + length) {
                return span;
            }
        }
        return QVariantMap();
    };
    qsizetype offset = 0;
    for (qsizetype index = 0; index < 6; ++index) {
        const QVariantMap format = formatAt(offset);
        QVERIFY(!format.isEmpty());
        QCOMPARE(format.value(QStringLiteral("fontBold")).toBool(), index == 0);
        QCOMPARE(format.value(QStringLiteral("fontStretch")).toInt(), index < 5 ? 101 : 100);
        QVERIFY(qAbs(format.value(QStringLiteral("fontSizeRatio")).toReal()
                     * document.pageSize(0).height() - 12.0) < 0.01);
        offset += parts.at(index).text.size() + 1;
    }

    const QRectF originalFrame = controller.selectedFrameRect();
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(paragraph);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    const QString edited = paragraph + QStringLiteral(" Revised.");
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(edited);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.selectedText(), edited);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 6);
    QVERIFY(formatAt(0).value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!formatAt(edited.size() - 1).value(QStringLiteral("fontBold")).toBool());
    QCOMPARE(formatAt(edited.size() - 1).value(QStringLiteral("fontStretch")).toInt(), 100);
    const QVariantList editedSpans = controller.selectedCharacterSpans();
    controller.undo();
    QCOMPARE(controller.selectedText(), paragraph);
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(controller.selectedText(), edited);
    QCOMPARE(controller.selectedCharacterSpans(), editedSpans);
    controller.discardDrafts();
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::savesMixedSourceParagraphStylesRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-source-paragraph-edit-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-source-paragraph-edit-saved.pdf"));
    const QVector<SourceParagraphTextPart> parts = mixedSourceParagraphParts();
    const QString paragraph = sourceParagraphText(parts, 0, 6);
    QVERIFY(createSourceParagraphPdf(sourcePath, parts));
    const QStringList originalObjects = topLevelTextObjects(sourcePath);
    FS_MATRIX nextOriginal{};
    QVERIFY(textObjectMatrix(sourcePath, parts.at(6).text, nextOriginal));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), paragraph);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    const QString edited = paragraph + QStringLiteral(" Revised.");
    controller.commitSelectedText(edited);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    const QString savedText = normalizedPageText(outputPath);
    QVERIFY2(savedText.contains(edited.simplified()), qPrintable(savedText));
    QVERIFY(savedText.contains(sourceParagraphText(parts, 6, 2).simplified()));
    for (const QString &word : {QStringLiteral("combination"), QStringLiteral("experiences"),
                                QStringLiteral("6:00")}) {
        const auto characters = textCharacterRangeInfo(outputPath, word);
        QCOMPARE(characters.size(), word.size());
        for (const TextCharacterInfo &character : characters) {
            if (word == QLatin1String("combination"))
                QVERIFY(character.font.weight >= 700);
            else
                QVERIFY(character.font.weight > 0 && character.font.weight < 700);
        }
    }
    FS_MATRIX nextSaved{};
    QVERIFY(textObjectMatrix(outputPath, parts.at(6).text, nextSaved));
    QCOMPARE(nextSaved.a, nextOriginal.a);
    QCOMPARE(nextSaved.d, nextOriginal.d);
    QCOMPARE(nextSaved.e, nextOriginal.e);
    QCOMPARE(nextSaved.f, nextOriginal.f);
    QCOMPARE(topLevelTextObjects(sourcePath), originalObjects);
    const QVariantList reopened = waitForCandidates(controller, 0);
    QVERIFY(!reopened.isEmpty());
    bool foundBold = false;
    bool foundRegular = false;
    for (const QVariant &value : reopened) {
        const QVariantMap reopenedCandidate = value.toMap();
        const QString text = reopenedCandidate.value(QStringLiteral("text")).toString();
        if (!text.contains(QStringLiteral("a combination"))
            && !text.contains(QStringLiteral("entertainment experiences"))) {
            continue;
        }
        QVERIFY(controller.selectCandidate(
            0, reopenedCandidate.value(QStringLiteral("objectId")).toString()));
        const QVariantList spans = controller.selectedCharacterSpans();
        for (const QString &phrase : {QStringLiteral("a combination"),
                                      QStringLiteral("entertainment experiences")}) {
            const qsizetype start = text.indexOf(phrase);
            if (start < 0)
                continue;
            const bool bold = phrase == QLatin1String("a combination");
            // Retained source advances can split one face into cluster spans.
            // Require the expected style on every character of the phrase.
            for (qsizetype position = start; position < start + phrase.size(); ++position) {
                bool covered = false;
                for (const QVariant &spanValue : spans) {
                    const QVariantMap span = spanValue.toMap();
                    const qsizetype spanStart = span.value(QStringLiteral("start")).toLongLong();
                    const qsizetype spanEnd = spanStart
                        + span.value(QStringLiteral("length")).toLongLong();
                    if (position >= spanStart && position < spanEnd) {
                        QCOMPARE(span.value(QStringLiteral("fontBold")).toBool(), bold);
                        covered = true;
                        break;
                    }
                }
                QVERIFY(covered);
            }
            (bold ? foundBold : foundRegular) = true;
        }
    }
    QVERIFY(foundBold);
    QVERIFY(foundRegular);
}

void PDFEditControllerTest::savesMovedMixedSourceParagraphWithoutReflow()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-source-paragraph-move-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("mixed-source-paragraph-move-saved.pdf"));
    const QVector<SourceParagraphTextPart> parts = mixedSourceParagraphParts();
    QVERIFY(createSourceParagraphPdf(sourcePath, parts));
    const QStringList originalObjects = topLevelTextObjects(sourcePath);
    QVector<FS_MATRIX> originalMatrices;
    for (const SourceParagraphTextPart &part : parts) {
        FS_MATRIX matrix{};
        QVERIFY(textObjectMatrix(sourcePath, part.text, matrix));
        originalMatrices.append(matrix);
    }
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), sourceParagraphText(parts, 0, 6));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    const QPointF translation(0.02, -0.01);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(controller.selectedFrameRect().translated(translation));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.draftsForPage(0).constFirst().toMap()
                .value(QStringLiteral("sourceTextMoveOnly")).toBool());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(topLevelTextObjects(outputPath), originalObjects);
    constexpr qreal tolerance = 1.0 / 64.0 + 0.001;
    const QSizeF pageSize = document.pageSize(0);
    for (qsizetype index = 0; index < parts.size(); ++index) {
        FS_MATRIX matrix{};
        QVERIFY(textObjectMatrix(outputPath, parts.at(index).text, matrix));
        const FS_MATRIX &original = originalMatrices.at(index);
        QCOMPARE(matrix.a, original.a);
        QCOMPARE(matrix.b, original.b);
        QCOMPARE(matrix.c, original.c);
        QCOMPARE(matrix.d, original.d);
        const qreal expectedX = index < 6 ? translation.x() * pageSize.width() : 0.0;
        const qreal expectedY = index < 6 ? -translation.y() * pageSize.height() : 0.0;
        QVERIFY(qAbs(matrix.e - original.e - expectedX) < tolerance);
        QVERIFY(qAbs(matrix.f - original.f - expectedY) < tolerance);
        QCOMPARE(textObjectFontInfo(outputPath, parts.at(index).text).baseName,
                 textObjectFontInfo(sourcePath, parts.at(index).text).baseName);
    }
}

void PDFEditControllerTest::justifiesSourceParagraphThroughQmlOverlay()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-justify-editor.pdf"));
    QVERIFY(createSourceParagraphPdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QString original = QString::fromLatin1(kSourceParagraphText);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(
        QFINDTESTDATA("../src/Components/EditOverlay.qml")));
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties({
        {QStringLiteral("page"), 0}, {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), false},
        {QStringLiteral("draftTextureReady"), false},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), 1.0},
        {QStringLiteral("width"), 595.0}, {QStringLiteral("height"), 842.0}}));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    QQuickWindow window;
    window.resize(595, 842);
    qobject_cast<QQuickItem *>(overlay.get())->setParentItem(window.contentItem());
    window.show();
    QTRY_VERIFY(window.isExposed());
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginEditing",
                                     Q_ARG(QVariant, QVariant(candidate))));
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
    QTextDocument *textDocument = quickDocument->textDocument();
    QTRY_COMPARE(textDocument->blockCount(), 3);
    QTRY_COMPARE(editor->property("cursorPosition").toInt(), original.size());
    controller.setSelectedParagraphAlignment(PDFEditController::JustifyAlignment);
    QString expected = original;
    expected.replace(QLatin1Char('\n'), QLatin1Char(' '));
    QTRY_COMPARE(textDocument->toPlainText(), expected);
    QCOMPARE(controller.selectedText(), expected);
    QCOMPARE(controller.selectedParagraphFormats().size(), 1);
    QTRY_COMPARE(textDocument->blockCount(), 1);
    const QTextBlock block = textDocument->begin();
    QCOMPARE(block.blockFormat().alignment(), Qt::AlignJustify);
    QTRY_VERIFY(block.layout()->lineCount() >= 2);
    for (int index = 0; index + 1 < block.layout()->lineCount(); ++index) {
        const QTextLine line = block.layout()->lineAt(index);
        int end = line.textStart() + line.textLength();
        while (end > line.textStart() && expected.at(end - 1).isSpace()) {
            --end;
        }
        QVERIFY(qAbs(line.cursorToX(end) - line.width()) < 0.5);
    }
    const QTextLine lastLine = block.layout()->lineAt(block.layout()->lineCount() - 1);
    QVERIFY(lastLine.cursorToX(expected.size()) < lastLine.width() - 1.0);
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    controller.undo();
    QCOMPARE(controller.selectedText(), original);
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(controller.selectedText(), expected);
    controller.discardDrafts();
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedText(), original);
    // The same action also works when selected without an active inline editor.
    controller.setSelectedParagraphAlignment(PDFEditController::JustifyAlignment);
    QCOMPARE(controller.selectedText(), expected);
    QCOMPARE(controller.selectedParagraphFormats().size(), 1);
}

void PDFEditControllerTest::preservesExplicitParagraphBreaksWhenJustifyingSource()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-justify-explicit-break.pdf"));
    QVERIFY(createSourceParagraphPdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString original = QString::fromLatin1(kSourceParagraphText);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    // Inserting Enter adjacent to a source break must retain its own identity.
    const qsizetype split = original.indexOf(QLatin1Char('\n'));
    QString edited = original;
    edited.insert(split, QLatin1Char('\n'));
    controller.updateSelectedTextPreview(edited, split + 1);
    controller.setSelectedTextRange(0, edited.size());
    controller.setSelectedParagraphAlignment(PDFEditController::JustifyAlignment);
    QString expected = original;
    expected.replace(QLatin1Char('\n'), QLatin1Char(' '));
    expected.insert(split, QLatin1Char('\n'));
    QCOMPARE(controller.selectedText(), expected);
    QCOMPARE(controller.selectedParagraphFormats().size(), 2);
    controller.cancelSelectedTextEdit();
    QCOMPARE(controller.selectedText(), original);
    QVERIFY(!controller.hasDrafts());
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(edited, split + 1);
    controller.updateSelectedTextPreview(original, split);
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
    controller.setSelectedParagraphAlignment(PDFEditController::JustifyAlignment);
    expected = original;
    expected.replace(QLatin1Char('\n'), QLatin1Char(' '));
    QCOMPARE(controller.selectedText(), expected);
    controller.undo();
    controller.beginSelectedTextEdit();
    edited = original;
    edited.remove(split, 1);
    controller.updateSelectedTextPreview(edited, split);
    controller.updateSelectedTextPreview(original, split + 1);
    controller.setSelectedTextRange(0, original.size());
    controller.setSelectedParagraphAlignment(PDFEditController::JustifyAlignment);
    expected[split] = QLatin1Char('\n');
    QCOMPARE(controller.selectedText(), expected);
    QCOMPARE(controller.selectedParagraphFormats().size(), 2);
}

void PDFEditControllerTest::keepsUnsafeSourceTextLinesSeparate_data()
{
    QTest::addColumn<double>("secondX");
    QTest::addColumn<double>("secondY");
    QTest::addColumn<QByteArray>("secondFont");
    QTest::newRow("overlapping-lines") << 72.0 << 732.0 << QByteArray("Helvetica");
    QTest::newRow("overprinted-text") << 72.0 << 740.0 << QByteArray("Helvetica");
    QTest::newRow("different-column") << 360.0 << 710.0 << QByteArray("Helvetica");
    QTest::newRow("large-paragraph-gap") << 72.0 << 680.0 << QByteArray("Helvetica");
    QTest::newRow("different-font") << 72.0 << 710.0 << QByteArray("Courier");
}

void PDFEditControllerTest::keepsUnsafeSourceTextLinesSeparate()
{
    QFETCH(double, secondX);
    QFETCH(double, secondY);
    QFETCH(QByteArray, secondFont);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-unsafe-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString firstText = QStringLiteral("Upper source line");
    const QString secondText = QStringLiteral("Lower source line");
    QVERIFY(createSourceParagraphPdf(sourcePath, {
        {firstText, 72.0, 740.0},
        {secondText, secondX, secondY, secondFont.constData()}}));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    for (const QString &text : {firstText, secondText}) {
        const QVariantMap candidate = candidateForText(candidates, text);
        QVERIFY2(!candidate.isEmpty(), qPrintable(text));
        QVERIFY(!candidate.value(QStringLiteral("grouped")).toBool());
        QVERIFY(!candidate.value(QStringLiteral("objectId")).toString()
                     .startsWith(QStringLiteral("paragraph:")));
    }
}

void PDFEditControllerTest::groupsParagraphsWithinSeparateColumns()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-two-columns.pdf"));
    QVERIFY(createSourceParagraphPdf(sourcePath, {
        {QStringLiteral("Left first"), 72.0, 740.0},
        {QStringLiteral("Right first"), 360.0, 740.0},
        {QStringLiteral("Right second"), 360.0, 710.0},
        {QStringLiteral("Left second"), 72.0, 710.0}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    for (const QString &text : {QStringLiteral("Left first\nLeft second"),
                                QStringLiteral("Right first\nRight second")}) {
        const QVariantMap candidate = candidateForText(candidates, text);
        QVERIFY2(!candidate.isEmpty(), qPrintable(text));
        QVERIFY(candidate.value(QStringLiteral("objectId")).toString()
                    .startsWith(QStringLiteral("paragraph:")));
        QCOMPARE(candidate.value(QStringLiteral("sourceRects")).toList().size(), 2);
    }
}

void PDFEditControllerTest::keepsSourceParagraphFrameClearOfOtherText()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-obstruction.pdf"));
    const QString firstText = QStringLiteral("Wide source line across the page");
    const QString secondText = QStringLiteral("Left next");
    const QString thirdText = QStringLiteral("Left last");
    const QString sideText = QStringLiteral("Side note");
    QVERIFY(createSourceParagraphPdf(sourcePath, {
        {firstText, 72.0, 740.0},
        {secondText, 72.0, 710.0},
        {thirdText, 72.0, 680.0},
        {sideText, 360.0, 680.0}}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const QVariantMap sideCandidate = candidateForText(candidates, sideText);
    QVERIFY(!sideCandidate.isEmpty());
    const QRectF sideRect = sideCandidate.value(QStringLiteral("sourceRect")).toRectF();
    bool foundParagraph = false;
    for (const QVariant &value : candidates) {
        const QVariantMap candidate = value.toMap();
        if (!candidate.value(QStringLiteral("objectId")).toString()
                 .startsWith(QStringLiteral("paragraph:"))) {
            continue;
        }
        foundParagraph = true;
        const QRectF sourceRect = candidate.value(QStringLiteral("sourceRect")).toRectF();
        QVERIFY2(!sourceRect.intersects(sideRect),
                 "A wide first line must not absorb later rows beside another column");
    }
    QVERIFY(foundParagraph);
    QVERIFY(candidateForText(candidates,
        firstText + QLatin1Char('\n') + secondText + QLatin1Char('\n') + thirdText).isEmpty());
}

void PDFEditControllerTest::editsSourceParagraphWithNoOpCancelAndHistory()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-history.pdf"));
    QVERIFY(createSourceParagraphPdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString originalText = QString::fromLatin1(kSourceParagraphText);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), originalText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    const QRectF originalFrame = controller.selectedFrameRect();
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(originalText);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    const QString editedText = QStringLiteral(
        "A longer replacement paragraph that wraps inside its editing frame.\n"
        "The next line remains part of the same editable source paragraph.");
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(editedText);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 4);
    controller.cancelSelectedTextEdit();
    QCOMPARE(controller.selectedText(), originalText);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.draftedTextObjectPathsForPage(0).isEmpty());

    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(editedText);
    controller.finishSelectedTextEdit();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 4);
    QVERIFY(controller.allDraftedTextObjectsSuppressibleForPage(0));
    QVERIFY(controller.canUndo());
    controller.undo();
    QCOMPARE(controller.selectedText(), originalText);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(controller.selectedText(), editedText);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 4);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(!candidateForText(waitForCandidates(controller, 0), originalText).isEmpty());
}

void PDFEditControllerTest::editsSourceParagraphThroughQmlOverlay()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-qml-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-qml-saved.pdf"));
    QVERIFY(createSourceParagraphPdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QString originalText = QString::fromLatin1(kSourceParagraphText);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), originalText);
    QVERIFY(!candidate.isEmpty());
    const QRectF originalFrame = candidate.value(QStringLiteral("frameRect")).toRectF();
    const QString themePath = QFINDTESTDATA("../src/Components/AppTheme.qml");
    const QString overlayPath = QFINDTESTDATA("../src/Components/EditOverlay.qml");
    QVERIFY(registerQmlTestTypes(themePath));
    QQuickWindow window;
    window.resize(595, 842);
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent overlayComponent(&engine, QUrl::fromLocalFile(overlayPath));
    const QSizeF pageSize = document.pageSize(0);
    const QVariantMap properties{
        {QStringLiteral("page"), 0},
        {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), false},
        {QStringLiteral("draftTextureReady"), false},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), 1.0},
        {QStringLiteral("width"), pageSize.width()},
        {QStringLiteral("height"), pageSize.height()}};
    std::unique_ptr<QObject> overlay(
        overlayComponent.createWithInitialProperties(properties));
    QVERIFY2(overlay, qPrintable(overlayComponent.errorString()));
    auto *overlayItem = qobject_cast<QQuickItem *>(overlay.get());
    QVERIFY(overlayItem);
    overlayItem->setParentItem(window.contentItem());
    window.show();
    QTRY_VERIFY(window.isExposed());
    const auto beginEditing = [&]() {
        return QMetaObject::invokeMethod(overlay.get(), "beginEditing",
                                        Q_ARG(QVariant, QVariant(candidate)));
    };
    QVERIFY(beginEditing());
    QVERIFY(overlay->property("editing").toBool());
    QVERIFY(controller.textEditActive());
    QCOMPARE(controller.selectedSourceRects().size(), 4);
    QObject *textEdit = nullptr;
    for (QObject *child : overlay->findChildren<QObject *>()) {
        if (child->metaObject()->indexOfProperty("textDocument") >= 0) {
            textEdit = child;
            break;
        }
    }
    QVERIFY(textEdit);
    auto *quickDocument = textEdit->property("textDocument").value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *editorDocument = quickDocument->textDocument();
    QVERIFY(editorDocument);
    QTRY_COMPARE(editorDocument->toPlainText(), originalText);
    QTRY_COMPARE(textEdit->property("lineCount").toInt(), 3);
    QVERIFY(controller.selectedFrameRect().width() >= originalFrame.width());
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QVERIFY(!overlay->property("editing").toBool());
    QVERIFY(!controller.textEditActive());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);

    QVERIFY(beginEditing());
    QTRY_COMPARE(editorDocument->toPlainText(), originalText);
    auto *editorItem = qobject_cast<QQuickItem *>(textEdit);
    QVERIFY(editorItem);
    editorItem->forceActiveFocus();
    QTRY_VERIFY(editorItem->hasActiveFocus());
    // Exercise actual key edits so TextEdit also schedules layout bounds updates.
    QVERIFY(QMetaObject::invokeMethod(textEdit, "selectAll"));
    const QString replacement = QStringLiteral(
        "This longer replacement paragraph must wrap naturally inside the source editing "
        "frame while preserving every word as editable text.\n"
        "An explicit line break also remains intact after committing and saving the document.");
    for (const QChar character : replacement) {
        if (character == QLatin1Char('\n')) {
            QTest::keyClick(&window, Qt::Key_Return);
        } else {
            QTest::keyClick(&window, character.toLatin1());
        }
    }
    QTRY_COMPARE(editorDocument->toPlainText(), replacement);
    QTRY_COMPARE(controller.selectedText(), replacement);
    QTRY_VERIFY(textEdit->property("lineCount").toInt() > 3);
    QTRY_VERIFY(controller.selectedContentBounds().height() > originalFrame.height());
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 4);
    QVERIFY(controller.canUndo());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(normalizedPageText(outputPath), replacement.simplified());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::preservesMultilineSourceClusterSpacingThroughQmlEditing()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("multiline-source-clusters.pdf"));
    const QByteArray content("BT /F1 12 Tf 1 0 0 1 72 740 Tm "
        "[(un) -40 (der the fo) 35 (llowing con) -60 (ditions)] TJ ET\n"
        "BT /F1 12 Tf 1 0 0 1 72 726 Tm "
        "[(w) -80 (ith the under) 25 (standing that:)] TJ ET\n");
    QVERIFY(writePdfObjects(sourcePath, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        QByteArray("<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman "
                   "/Encoding /WinAnsiEncoding >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream"}));
    const QString original = QStringLiteral("under the following conditions\nwith the understanding that:");
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(candidate.value(QStringLiteral("objectId")).toString().startsWith(QStringLiteral("paragraph:")));
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    const QRectF originalFrame = controller.selectedFrameRect();
    const auto sourceAdvanceAt = [&](int position) {
        for (const QVariant &value : originalSpans) {
            const QVariantMap span = value.toMap();
            const int start = span.value(QStringLiteral("start")).toInt();
            if (position >= start && position < start + span.value(QStringLiteral("length")).toInt())
                return span.value(QStringLiteral("sourceAdvanceRatio")).toDouble();
        }
        return 0.0;
    };
    const int secondLineStart = original.indexOf(QLatin1Char('\n')) + 1;
    QVERIFY(qAbs(sourceAdvanceAt(secondLineStart)) > 0.0001);
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Components/EditOverlay.qml")));
    const QSizeF pageSize = document.pageSize(0);
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties({
        {QStringLiteral("page"), 0}, {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), false}, {QStringLiteral("draftTextureReady"), false},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), 1.55},
        {QStringLiteral("width"), pageSize.width() * 1.55},
        {QStringLiteral("height"), pageSize.height() * 1.55}}));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    QQuickWindow window;
    window.resize(qCeil(pageSize.width() * 1.55), qCeil(pageSize.height() * 1.55));
    qobject_cast<QQuickItem *>(overlay.get())->setParentItem(window.contentItem());
    window.show();
    QTRY_VERIFY(window.isExposed());
    const auto beginEditing = [&]() {
        return QMetaObject::invokeMethod(overlay.get(), "beginEditing", Q_ARG(QVariant, QVariant(candidate)));
    };
    QVERIFY(beginEditing());
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
    QTextDocument *textDocument = quickDocument->textDocument();
    QTRY_COMPARE(textDocument->toPlainText(), original);
    QTRY_COMPARE(editor->property("lineCount").toInt(), 2);
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    QVERIFY(beginEditing());
    QTRY_COMPARE(textDocument->toPlainText(), original);
    auto *editorItem = qobject_cast<QQuickItem *>(editor);
    QVERIFY(editorItem);
    editorItem->forceActiveFocus();
    QTRY_VERIFY(editorItem->hasActiveFocus());
    QVERIFY(editor->setProperty("cursorPosition", secondLineStart + 1));
    QTest::keyClick(&window, Qt::Key_X);
    QString edited = original;
    edited.insert(secondLineStart + 1, QLatin1Char('x'));
    QTRY_COMPARE(textDocument->toPlainText(), edited);
    QTRY_COMPARE(controller.selectedText(), edited);
    QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
    QTRY_COMPARE(textDocument->toPlainText(), original);
    QTRY_COMPARE(controller.selectedText(), original);
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::keepsSourceParagraphLayoutStableAtFractionalZoom_data()
{
    QTest::addColumn<qreal>("zoom");
    QTest::addColumn<bool>("mixedStyles");
    QTest::newRow("75-percent") << 0.75 << false;
    QTest::newRow("100-percent") << 1.0 << false;
    QTest::newRow("124-percent") << 1.24 << false;
    QTest::newRow("155-percent") << 1.55 << false;
    QTest::newRow("200-percent") << 2.0 << false;
    QTest::newRow("mixed-styles-75-percent") << 0.75 << true;
    QTest::newRow("mixed-styles-155-percent") << 1.55 << true;
    QTest::newRow("mixed-styles-200-percent") << 2.0 << true;
}

void PDFEditControllerTest::keepsSourceParagraphLayoutStableAtFractionalZoom()
{
    QFETCH(qreal, zoom);
    QFETCH(bool, mixedStyles);
    QStringList lines{
        QStringLiteral("entertainment experiences (excluding the prostitution industry). In the US, the night-time "),
        QStringLiteral("economy includes 5 main areas including: Arts, bars, culinary services, sports and "),
        QStringLiteral("entertainment. Most European countries define the night-time economy in a narrow sense "),
        QStringLiteral("and define the night-time economic operating time frame from 6:00 p.m. the previous day "),
        QStringLiteral("to 6:00 a.m. the next morning (Field 2008; Le et al., 2022).")};
    if (mixedStyles) {
        lines.prepend(QStringLiteral(
            "a combination of economic and cultural activities that take place at night, mainly"));
    }
    QVector<SourceParagraphTextPart> parts;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        const char *font = mixedStyles && index == 0 ? "Helvetica-Bold" : "Helvetica";
        const double stretch = mixedStyles
            ? (index < lines.size() - 1 ? 1.008 : 1.0008) : 1.0;
        parts.append({lines.at(index), 72.0, 620.0 - index * 14.0, font, stretch, 12.0F});
    }
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("paragraph-zoom-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createSourceParagraphPdf(sourcePath, parts));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString original = lines.join(QLatin1Char('\n'));
    controller.setTool(PDFEditController::SelectTool);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(
        QFINDTESTDATA("../src/Components/EditOverlay.qml")));
    const QSizeF pageSize = document.pageSize(0);
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties({
        {QStringLiteral("page"), 0}, {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), false},
        {QStringLiteral("draftTextureReady"), false},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), zoom},
        {QStringLiteral("width"), pageSize.width() * zoom},
        {QStringLiteral("height"), pageSize.height() * zoom}}));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    QQuickWindow window;
    window.resize(qCeil(pageSize.width() * zoom), qCeil(pageSize.height() * zoom));
    auto *overlayItem = qobject_cast<QQuickItem *>(overlay.get());
    overlayItem->setParentItem(window.contentItem());
    window.show();
    QTRY_VERIFY(window.isExposed());
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginEditing",
                                     Q_ARG(QVariant, QVariant(candidate))));
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
    QTextDocument *textDocument = quickDocument->textDocument();
    QTRY_COMPARE(textDocument->toPlainText(), original);
    QTRY_COMPARE(editor->property("lineCount").toInt(), lines.size());
    for (QTextBlock block = textDocument->begin(); block.isValid(); block = block.next()) {
        QCOMPARE(block.layout()->lineCount(), 1);
        QTextCursor lineCursor(block);
        lineCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        QCOMPARE(lineCursor.charFormat().font().bold(), mixedStyles && block.blockNumber() == 0);
    }
    const qreal baseline = editorItem->mapToItem(overlayItem,
        QPointF(0.0, editor->property("baselineOffset").toReal())).y();
    QVERIFY(qAbs(baseline / zoom - controller.selectedBaselineY() * pageSize.height()) < 0.01);
    const qreal displayedHeight = editorItem->mapRectToItem(overlayItem,
        QRectF(0.0, 0.0, 1.0, editor->property("contentHeight").toReal())).height();
    // Every source row fits at the same physical height regardless of zoom.
    QVERIFY(qAbs(displayedHeight / zoom - 14.0 * lines.size()) < 0.1);
    const QRectF originalFrame = candidate.value(QStringLiteral("frameRect")).toRectF();
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);

    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginEditing",
                                     Q_ARG(QVariant, QVariant(candidate))));
    QTRY_VERIFY(editorItem->hasActiveFocus());
    QVERIFY(QMetaObject::invokeMethod(editor, "selectAll"));
    QTRY_COMPARE(editor->property("selectionEnd").toInt(), original.size());
    controller.setSelectedTextRange(0, original.size());
    controller.setSelectedFontSize(18);
    QTextCursor cursor(textDocument);
    cursor.setPosition(0);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QCOMPARE(cursor.charFormat().font().pixelSize() * editorItem->scale(), 18.0 * zoom);
    QCOMPARE(controller.selectedFontSize(), 18.0);
    QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
    QTRY_COMPARE(controller.selectedFontSize(), 12.0);
    QTRY_COMPARE(editor->property("lineCount").toInt(), lines.size());
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "cancelEditing"));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
}

void PDFEditControllerTest::savesEditedSourceParagraphRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-edit-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-edit-saved.pdf"));
    QVERIFY(createSourceParagraphPdf(sourcePath));
    const QStringList originalObjects = topLevelTextObjects(sourcePath);
    QCOMPARE(originalObjects.size(), 4);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QString::fromLatin1(kSourceParagraphText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    const QString replacement = QStringLiteral(
        "Replacement content is long enough to wrap across several visual lines.\n"
        "An explicit line break and every remaining word must survive saving.");
    controller.commitSelectedText(replacement);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 4);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    const QString savedText = normalizedPageText(outputPath).simplified();
    QCOMPARE(savedText, replacement.simplified());
    for (const QString &original : originalObjects) {
        QVERIFY2(!savedText.contains(original), qPrintable(original));
    }
    QCOMPARE(topLevelTextObjects(sourcePath), originalObjects);
    QVERIFY(!waitForCandidates(controller, 0).isEmpty());
}

void PDFEditControllerTest::savesMovedSourceParagraphWithoutReflow()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-move-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("source-paragraph-move-saved.pdf"));
    QVERIFY(createSourceParagraphPdf(sourcePath));
    const QStringList originalObjects = topLevelTextObjects(sourcePath);
    QVector<FS_MATRIX> originalMatrices;
    for (const QString &text : originalObjects) {
        FS_MATRIX matrix{};
        QVERIFY(textObjectMatrix(sourcePath, text, matrix));
        originalMatrices.append(matrix);
    }
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QString paragraphText = QString::fromLatin1(kSourceParagraphText);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), paragraphText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    const QPointF translation(0.04, 0.05);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(controller.selectedFrameRect().translated(translation));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.draftsForPage(0).constFirst().toMap()
                .value(QStringLiteral("sourceTextMoveOnly")).toBool());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(topLevelTextObjects(outputPath), originalObjects);
    const QSizeF pageSize = document.pageSize(0);
    QPointF savedTranslation;
    // Save maps both endpoints onto the 1/64-PDF-point coordinate grid.
    constexpr qreal translationTolerance = 1.0 / 64.0 + 0.001;
    for (qsizetype index = 0; index < originalObjects.size(); ++index) {
        FS_MATRIX matrix{};
        QVERIFY(textObjectMatrix(outputPath, originalObjects.at(index), matrix));
        const FS_MATRIX &original = originalMatrices.at(index);
        QCOMPARE(matrix.a, original.a);
        QCOMPARE(matrix.b, original.b);
        QCOMPARE(matrix.c, original.c);
        QCOMPARE(matrix.d, original.d);
        const QPointF partTranslation(matrix.e - original.e, matrix.f - original.f);
        QVERIFY(qAbs(partTranslation.x() - translation.x() * pageSize.width())
                < translationTolerance);
        QVERIFY(qAbs(partTranslation.y() + translation.y() * pageSize.height())
                < translationTolerance);
        if (index == 0) {
            savedTranslation = partTranslation;
        } else {
            // Every source object receives the same translation; no line is reflowed.
            QVERIFY(qAbs(partTranslation.x() - savedTranslation.x()) < 0.001);
            QVERIFY(qAbs(partTranslation.y() - savedTranslation.y()) < 0.001);
        }
    }
    QVERIFY(!candidateForText(waitForCandidates(controller, 0), paragraphText).isEmpty());
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

void PDFEditControllerTest::persistsExpandedFrameForParagraphAlignment()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF sourceRect = m_controller.selectedSourceRect();
    const QRectF originalFrameRect = m_controller.selectedFrameRect();
    m_controller.beginSelectedTextEdit();
    const QRectF expandedFrameRect = m_controller.selectedFrameRect();
    QVERIFY(expandedFrameRect.right() > originalFrameRect.right());

    m_controller.setSelectedParagraphAlignment(
        PDFEditController::RightAlignment);
    m_controller.finishSelectedTextEdit();

    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    QCOMPARE(
        m_controller.selectedParagraphAlignment(),
        static_cast<int>(PDFEditController::RightAlignment));
    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap draft = drafts.constFirst().toMap();
    QCOMPARE(
        draft.value(QStringLiteral("frameRect")).toRectF(),
        expandedFrameRect);
    QCOMPARE(
        draft.value(QStringLiteral("paragraphFormats"))
            .toList()
            .constFirst()
            .toMap()
            .value(QStringLiteral("alignment"))
            .toString(),
        QStringLiteral("right"));

    m_controller.undo();
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QCOMPARE(m_controller.selectedFrameRect(), expandedFrameRect);
    QCOMPARE(
        m_controller.selectedParagraphAlignment(),
        static_cast<int>(PDFEditController::RightAlignment));
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

void PDFEditControllerTest::keepsDistantMixedStylesAsSeparateCandidates()
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

void PDFEditControllerTest::keepsDistantFontFacesAsSeparateCandidates()
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
