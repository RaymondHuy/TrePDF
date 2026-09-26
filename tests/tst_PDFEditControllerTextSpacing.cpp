#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QSet>

using namespace PDFEditControllerTestSupport;

namespace {

bool createPositionedTextPdf(const QString &path, const QByteArray &content)
{
    return writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        QByteArray("<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman "
                   "/Encoding /WinAnsiEncoding >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream"});
}

bool createPositionedUnicodeTextPdf(const QString &path, const QString &text,
                                   const QByteArray &fontData)
{
    // Use a real embedded Unicode face and explicit CID/ToUnicode mappings.
    // WinAnsi's accented Latin characters do not exercise Vietnamese glyphs.
    const QRawFont font(fontData, 1000.0, QFont::PreferNoHinting);
    if (!font.isValid() || text != text.normalized(QString::NormalizationForm_C))
        return false;
    const auto glyphs = font.glyphIndexesForString(text);
    const auto widths = font.advancesForGlyphIndexes(glyphs);
    if (glyphs.size() != text.size() || widths.size() != glyphs.size())
        return false;
    const auto hex = [](uint value) {
        return QByteArray::number(value, 16).rightJustified(4, '0');
    };
    QByteArray content("BT /F1 12 Tf 1 0 0 1 72 740 Tm [");
    QByteArray widthEntries;
    QByteArray unicodeEntries;
    QSet<uint> mappedGlyphs;
    for (int index = 0; index < text.size(); ++index) {
        const uint glyph = glyphs[index];
        if (!glyph || glyph > 0xffff || !qIsFinite(widths[index].x()))
            return false;
        content += "<" + hex(glyph) + ">";
        if (index == 1) content += " -40 ";
        if (index == 11) content += " 35 ";
        if (index == 22) content += " -60 ";
        if (mappedGlyphs.contains(glyph))
            continue;
        mappedGlyphs.insert(glyph);
        widthEntries += QByteArray::number(glyph) + " ["
            + QByteArray::number(widths[index].x(), 'f', 4) + "] ";
        unicodeEntries += "<" + hex(glyph) + "> <"
            + hex(text.at(index).unicode()) + ">\n";
    }
    content += "] TJ ET\n";
    const QByteArray toUnicode = QByteArray(
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /TrePDFUnicodeSpacing def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <ffff>\nendcodespacerange\n")
        + QByteArray::number(mappedGlyphs.size()) + " beginbfchar\n"
        + unicodeEntries + "endbfchar\nendcmap\n"
        "CMapName currentdict /CMap defineresource pop\nend\nend\n";
    const auto stream = [](const QByteArray &bytes, const QByteArray &entries = {}) {
        return QByteArray("<< /Length ") + QByteArray::number(bytes.size())
            + entries + " >>\nstream\n" + bytes + "\nendstream";
    };
    return writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        QByteArray("<< /Type /Font /Subtype /Type0 /BaseFont /ArialMT "
                   "/Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 9 0 R >>"),
        stream(content),
        QByteArray("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /ArialMT "
                   "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
                   "/FontDescriptor 7 0 R /CIDToGIDMap /Identity /DW 1000 /W [")
            + widthEntries + "] >>",
        QByteArray("<< /Type /FontDescriptor /FontName /ArialMT /Flags 32 "
                   "/FontBBox [-1000 -1000 2000 2000] /ItalicAngle 0 "
                   "/Ascent 1000 /Descent -300 /CapHeight 750 /StemV 80 /FontFile2 8 0 R >>"),
        stream(fontData, QByteArray(" /Length1 ") + QByteArray::number(fontData.size())),
        stream(toUnicode)});
}

QVector<qreal> visibleCharacterOrigins(const QString &path)
{
    PDFiumProcessLock lock;
    QVector<qreal> origins;
    const QByteArray encodedPath = QFile::encodeName(path);
    FPDF_DOCUMENT pdf = FPDF_LoadDocument(encodedPath.constData(), nullptr);
    if (!pdf) return origins;
    const auto closePdf = qScopeGuard([&] { FPDF_CloseDocument(pdf); });
    FPDF_PAGE page = FPDF_LoadPage(pdf, 0);
    if (!page) return origins;
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) return origins;
    const auto closeText = qScopeGuard([&] { FPDFText_ClosePage(textPage); });
    for (int index = 0; index < FPDFText_CountChars(textPage); ++index) {
        const QChar character(static_cast<char16_t>(FPDFText_GetUnicode(textPage, index)));
        if (FPDFText_IsGenerated(textPage, index) == 1 || character.isSpace()) continue;
        double x = 0.0, y = 0.0;
        if (!FPDFText_GetCharOrigin(textPage, index, &x, &y)) return {};
        origins.append(x);
    }
    return origins;
}

QVector<qreal> sourceAdvances(const QVariantList &spans, int length)
{
    QVector<qreal> advances(length, 0.0);
    for (const QVariant &value : spans) {
        const QVariantMap span = value.toMap();
        const int start = span.value(QStringLiteral("start")).toInt();
        const int end = qMin(length, start + span.value(QStringLiteral("length")).toInt());
        for (int index = start; index < end; ++index)
            advances[index] = span.value(QStringLiteral("sourceAdvanceRatio")).toReal();
    }
    return advances;
}

} // namespace

void PDFEditControllerTest::rejectsOversizedRetainedGlyphLineTransactionally()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("retained-glyph-limit-source.pdf"));
    const QString savedPath = m_temporaryDirectory.filePath(QStringLiteral("retained-glyph-limit-saved.pdf"));
    const QString destinationPath = m_temporaryDirectory.filePath(QStringLiteral("retained-glyph-limit-existing.pdf"));
    const QString original(1024, QLatin1Char('i'));
    QVERIFY(createPositionedTextPdf(sourcePath,
        QByteArray("BT /F1 1 Tf 1 0 0 1 72 740 Tm (") + original.toLatin1() + ") Tj ET\n"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.setSelectedLineSpacing(1.1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
    QCOMPARE(normalizedPageText(savedPath), original);
    QCOMPARE(visibleCharacterOrigins(savedPath).size(), 1024);
    const QVariantMap restored = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(!restored.isEmpty());
    QVERIFY(controller.selectCandidate(0, restored.value(QStringLiteral("objectId")).toString()));
    const QString edited = original + QLatin1Char('i');
    controller.commitSelectedText(edited);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    const int revision = document.renderRevision();
    const QByteArray existingBytes("Existing destination must remain byte-identical after a rejected layout.\n");
    QFile destination(destinationPath);
    QVERIFY(destination.open(QIODevice::WriteOnly));
    QCOMPARE(destination.write(existingBytes), existingBytes.size());
    destination.close();
    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(destinationPath)));
    QVERIFY(destination.open(QIODevice::ReadOnly));
    QCOMPARE(destination.readAll(), existingBytes);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(controller.selectedText(), edited);
    QCOMPARE(document.renderRevision(), revision);
    QCOMPARE(document.source().toLocalFile(), savedPath);
}

void PDFEditControllerTest::recoversPositionedSourceWordSpaces()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("positioned-word-spaces-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("positioned-word-spaces-edited.pdf"));
    // TeX-style word gaps are TJ displacements, without any encoded space.
    // The WinAnsi em dash stays adjacent to both words, unlike these gaps.
    QVERIFY(createPositionedTextPdf(sourcePath, QByteArray(
        "BT /F1 12 Tf 1 0 0 1 72 740 Tm "
        "[(to) -250 (Share\227to) -250 (copy,) -250 (distribute) "
        "-250 (and) -250 (transmit) -250 (the) -250 (work)] TJ ET\n")));
    const QString original = QStringLiteral(
        "to Share\u2014to copy, distribute and transmit the work");
    QCOMPARE(normalizedPageText(sourcePath), original);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY2(!candidate.isEmpty(), "Positioned word gaps must reach the editable source text");
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedText(), original);
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    const QRectF originalFrame = controller.selectedFrameRect();
    controller.beginSelectedTextEdit();
    QCOMPARE(controller.selectedText(), original);
    controller.cancelSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(original);
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    const QString edited = original + QStringLiteral(" freely");
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(edited);
    controller.finishSelectedTextEdit();
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(normalizedPageText(outputPath), edited);
    const QVariantMap reopened = candidateForText(waitForCandidates(controller, 0), edited);
    QVERIFY2(!reopened.isEmpty(), "Save/reopen must preserve all recovered word boundaries");
    QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedText(), edited);
    controller.beginSelectedTextEdit();
    QCOMPARE(controller.selectedText(), edited);
    controller.cancelSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::keepsGeneratedSeparatorsWithinSourceObjects()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("positioned-source-object-boundaries.pdf"));
    QVERIFY(createPositionedTextPdf(sourcePath, QByteArray(
        "BT /F1 12 Tf 1 0 0 1 72 740 Tm (left) Tj ET\n"
        "BT /F1 12 Tf 1 0 0 1 92 740 Tm (part) Tj ET\n"
        "BT /F1 12 Tf 1 0 0 1 72 726 Tm (next) Tj ET\n"
        "BT /F1 12 Tf 1 0 0 1 96 726 Tm (row) Tj ET\n"
        "BT /F1 12 Tf 6 Tc 1 0 0 1 72 600 Tm (ABCDE) Tj ET\n")));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    const QString paragraph = QStringLiteral("left part\nnext row");
    const QVariantMap grouped = candidateForText(candidates, paragraph);
    QVERIFY2(!grouped.isEmpty(), "Object and line grouping must insert each separator exactly once");
    QVERIFY(controller.selectCandidate(0, grouped.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedText(), paragraph);
    controller.beginSelectedTextEdit();
    QCOMPARE(controller.selectedText(), paragraph);
    controller.cancelSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());

    // Large character spacing is not evidence that individual letters are words.
    const QVariantMap tracked = candidateForText(candidates, QStringLiteral("ABCDE"));
    QVERIFY(!tracked.isEmpty());
    QVERIFY(controller.selectCandidate(0, tracked.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedText(), QStringLiteral("ABCDE"));
    QVERIFY(qAbs(controller.selectedLetterSpacing() - 6.0) < 0.02);
    QCOMPARE(controller.selectedWordSpacing(), 0.0);
}

void PDFEditControllerTest::changesParagraphSpacingOutsideEditorWithHistory()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("paragraph-spacing-history.pdf"));
    QVERIFY(createSourceSpacingPdf(path, 0.0, 0.0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Alpha beta gamma delta"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedLineSpacing(), 1.0);
    controller.setSelectedLineSpacing(1.5);
    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedLineSpacing(), 1.5);
    controller.undo();
    QCOMPARE(controller.selectedLineSpacing(), 1.0);
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(controller.selectedLineSpacing(), 1.5);
    controller.setSelectedParagraphSpaceBefore(6.0);
    controller.setSelectedParagraphSpaceAfter(10.0);
    QCOMPARE(controller.selectedParagraphSpaceBefore(), 6.0);
    QCOMPARE(controller.selectedParagraphSpaceAfter(), 10.0);
    controller.undo();
    QCOMPARE(controller.selectedParagraphSpaceAfter(), 0.0);
    QCOMPARE(controller.selectedParagraphSpaceBefore(), 6.0);
    controller.redo();
    controller.setSelectedLineSpacing(0.49);
    controller.setSelectedParagraphSpaceBefore(-1.0);
    controller.setSelectedParagraphSpaceAfter(145.0);
    QCOMPARE(controller.selectedLineSpacing(), 1.5);
    QCOMPARE(controller.selectedParagraphSpaceBefore(), 6.0);
    QCOMPARE(controller.selectedParagraphSpaceAfter(), 10.0);
    controller.discardDrafts();
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedLineSpacing(), 1.0);
    QCOMPARE(controller.selectedParagraphSpaceBefore(), 0.0);
    QCOMPARE(controller.selectedParagraphSpaceAfter(), 0.0);

    const QString groupedPath = m_temporaryDirectory.filePath(QStringLiteral("paragraph-spacing-grouped.pdf"));
    QVERIFY(createSourceParagraphPdf(groupedPath, {
        {QStringLiteral("First source line"), 72.0, 740.0},
        {QStringLiteral("Second source line"), 72.0, 710.0}}));
    QVERIFY(document.open(QUrl::fromLocalFile(groupedPath)));
    const QString sourceText = QStringLiteral("First source line\nSecond source line");
    const QVariantMap grouped = candidateForText(waitForCandidates(controller, 0), sourceText);
    QVERIFY(!grouped.isEmpty());
    QVERIFY(controller.selectCandidate(0, grouped.value(QStringLiteral("objectId")).toString()));
    controller.setSelectedLineSpacing(1.0);
    controller.setSelectedParagraphSpaceBefore(0.0);
    QCOMPARE(controller.selectedText(), sourceText);
    QVERIFY(!controller.hasDrafts());
    controller.beginSelectedTextEdit();
    // Source row breaks reflow together; a user's Enter remains a distinct paragraph.
    const QString explicitText = sourceText + QStringLiteral("\nUser paragraph");
    controller.updateSelectedTextPreview(explicitText);
    controller.setSelectedTextRange(20, 0);
    controller.setSelectedLineSpacing(1.5);
    QCOMPARE(controller.selectedText(), QStringLiteral("First source line Second source line\nUser paragraph"));
    const QVariantList formats = controller.selectedParagraphFormats();
    QCOMPARE(formats.size(), 2);
    QCOMPARE(formats[0].toMap().value(QStringLiteral("lineSpacing")).toReal(), 1.5);
    QCOMPARE(formats[1].toMap().value(QStringLiteral("lineSpacing")).toReal(), 1.0);
    controller.finishSelectedTextEdit();
    controller.undo();
    QCOMPARE(controller.selectedText(), sourceText);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::savesBlankParagraphFontSizeThroughSpacingRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("blank-paragraph-font-source.pdf"));
    const QString firstPath = m_temporaryDirectory.filePath(QStringLiteral("blank-paragraph-font-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(QStringLiteral("blank-paragraph-font-second.pdf"));
    QVERIFY(createSourceSpacingPdf(sourcePath, 0.0, 0.0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap source = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Alpha beta gamma delta"));
    QVERIFY(!source.isEmpty());
    QVERIFY(controller.selectCandidate(0, source.value(QStringLiteral("objectId")).toString()));
    const QString text = QStringLiteral("Alpha\n\nBeta");
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(text);
    controller.setSelectedTextRange(0, text.size());
    controller.setSelectedFontSize(12);
    controller.setSelectedLineSpacing(1.5);
    controller.setSelectedParagraphSpaceBefore(6.0);
    // The blank paragraph has no painted glyph to carry its independent font
    // size. Losing this separator's format changes the next paragraph's y.
    controller.setSelectedTextRange(6, 1);
    controller.setSelectedFontSize(32);
    QCOMPARE(controller.selectedFontSize(), 32.0);
    const QVariantList expectedParagraphs = controller.selectedParagraphFormats();
    controller.finishSelectedTextEdit();

    const auto readOrigins = [](const QString &path) {
        QVector<QPointF> origins;
        const QByteArray encodedPath = QFile::encodeName(path);
        FPDF_DOCUMENT pdf = FPDF_LoadDocument(encodedPath.constData(), nullptr);
        if (!pdf) return origins;
        const auto closePdf = qScopeGuard([&] { FPDF_CloseDocument(pdf); });
        FPDF_PAGE page = FPDF_LoadPage(pdf, 0);
        if (!page) return origins;
        const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        if (!textPage) return origins;
        const auto closeText = qScopeGuard([&] { FPDFText_ClosePage(textPage); });
        for (int index = 0; index < FPDFText_CountChars(textPage); ++index) {
            const QChar character(static_cast<char16_t>(FPDFText_GetUnicode(textPage, index)));
            if (FPDFText_IsGenerated(textPage, index) == 1 || character.isSpace()) continue;
            double x = 0.0, y = 0.0;
            if (!FPDFText_GetCharOrigin(textPage, index, &x, &y)) return QVector<QPointF>();
            origins.append(QPointF(x, FPDF_GetPageHeightF(page) - y));
        }
        return origins;
    };
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(firstPath)));
    const QVector<QPointF> firstOrigins = readOrigins(firstPath);
    QCOMPARE(firstOrigins.size(), 9);
    for (int pass = 0; pass < 2; ++pass) {
        const QVariantMap reopened = candidateForText(waitForCandidates(controller, 0), text);
        QVERIFY2(!reopened.isEmpty(), "A blank paragraph must remain inside the saved text group");
        QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
        QCOMPARE(controller.selectedText(), text);
        QCOMPARE(controller.selectedParagraphFormats(), expectedParagraphs);
        controller.beginSelectedTextEdit();
        controller.setSelectedTextRange(0, 5);
        QCOMPARE(controller.selectedFontSize(), 12.0);
        controller.setSelectedTextRange(6, 1);
        QCOMPARE(controller.selectedFontSize(), 32.0);
        QCOMPARE(controller.selectedLineSpacing(), 1.5);
        QVERIFY(qAbs(controller.selectedParagraphSpaceBefore() - 6.0) < 0.001);
        controller.setSelectedTextRange(7, 4);
        QCOMPARE(controller.selectedFontSize(), 12.0);
        controller.cancelSelectedTextEdit();
        QVERIFY(!controller.hasDrafts());
        if (pass == 0) {
            controller.setSelectedColor(Qt::blue);
            QVERIFY(controller.saveAs(QUrl::fromLocalFile(secondPath)));
            const QVector<QPointF> secondOrigins = readOrigins(secondPath);
            QCOMPARE(secondOrigins.size(), firstOrigins.size());
            for (int index = 0; index < secondOrigins.size(); ++index) {
                QVERIFY2(QLineF(secondOrigins.at(index), firstOrigins.at(index)).length() < 0.08,
                    qPrintable(QStringLiteral("Glyph %1 moved after color-only save: (%2,%3) -> (%4,%5)")
                        .arg(index).arg(firstOrigins.at(index).x()).arg(firstOrigins.at(index).y())
                        .arg(secondOrigins.at(index).x()).arg(secondOrigins.at(index).y())));
            }
        }
    }
}

void PDFEditControllerTest::savesParagraphSpacingWithBlankLinesAndMovement_data()
{
    QTest::addColumn<bool>("added");
    QTest::addColumn<qreal>("letterSpacing");
    QTest::newRow("source-normal") << false << 0.0;
    QTest::newRow("source-spaced") << false << 0.2;
    QTest::newRow("added-normal") << true << 0.0;
    QTest::newRow("added-spaced") << true << 0.2;
}

void PDFEditControllerTest::savesParagraphSpacingWithBlankLinesAndMovement()
{
    QFETCH(bool, added);
    QFETCH(qreal, letterSpacing);
    const qreal wordSpacing = letterSpacing > 0.0 ? 1.0 : 0.0;
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("paragraph-spacing-source.pdf"));
    const QString firstPath = m_temporaryDirectory.filePath(QStringLiteral("paragraph-spacing-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(QStringLiteral("paragraph-spacing-second.pdf"));
    const QString movedPath = m_temporaryDirectory.filePath(QStringLiteral("paragraph-spacing-moved.pdf"));
    QVERIFY(createSourceSpacingPdf(sourcePath, 0.0, 0.0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Alpha beta gamma delta"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    if (added) {
        controller.commitSelectedText(QString());
        QVERIFY(!controller.addText(0, 0.2, 0.25).isEmpty());
    }
    const QString text = QStringLiteral("\nAlpha\tbeta gamma delta epsilon zeta eta theta iota kappa lambda mu.\n\nSecond paragraph with words that wrap onto another line.");
    const std::array<qreal, 4> factors{1.0, 1.15, 0.75, 1.5};
    const std::array<qreal, 4> before{8.0, 6.0, 4.0, 12.0};
    const std::array<qreal, 4> after{2.0, 10.0, 3.0, 8.0};
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(text);
    controller.setSelectedTextRange(0, text.size());
    controller.setSelectedFontSize(12);
    controller.setSelectedLetterSpacing(letterSpacing);
    controller.setSelectedWordSpacing(wordSpacing);
    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    int start = 0;
    for (int index = 0; index < paragraphs.size(); ++index) {
        controller.setSelectedTextRange(start, 0);
        controller.setSelectedLineSpacing(factors.at(index));
        controller.setSelectedParagraphSpaceBefore(before.at(index));
        controller.setSelectedParagraphSpaceAfter(after.at(index));
        start += paragraphs.at(index).size() + 1;
    }
    controller.setSelectedTextRange(0, text.size());
    QVERIFY(qIsNaN(controller.selectedLineSpacing()));
    QVERIFY(qIsNaN(controller.selectedParagraphSpaceBefore()));
    const QSizeF pageSize = document.pageSize(0);
    QRectF frame = controller.selectedFrameRect();
    frame.setWidth(230.0 / pageSize.width());
    frame.setHeight(20.0 / pageSize.height());
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(frame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), frame);
    const QPointF originalOrigin(controller.selectedOriginX(), controller.selectedBaselineY());
    const QVariantList expectedParagraphs = controller.selectedParagraphFormats();
    const QVariantList expectedCharacters = controller.selectedCharacterSpans();
    QFont font = controller.fontForDisplay(controller.selectedFontFamily(),
        controller.selectedFontStretch(), controller.selectedFontBold(),
        controller.selectedFontItalic(), controller.selectedFontUnderline(),
        controller.selectedDisplayFontSizeRatio() * pageSize.height(), letterSpacing, wordSpacing);
    controller.finishSelectedTextEdit();

    // Use QTextDocument's actual block layout as an independent baseline oracle,
    // including its blank blocks, fractional advances and collapsed margins.
    QTextDocument reference;
    reference.setDocumentMargin(0.0);
    reference.setDefaultFont(font);
    reference.setPlainText(text);
    reference.setTextWidth((frame.right() - (added ? frame.x() : originalOrigin.x())) * pageSize.width());
    int paragraphIndex = 0;
    for (QTextBlock block = reference.begin(); block.isValid(); block = block.next(), ++paragraphIndex) {
        QTextBlockFormat format;
        format.setLineHeight(factors.at(paragraphIndex) * 100.0, QTextBlockFormat::ProportionalHeight);
        format.setTopMargin(before.at(paragraphIndex));
        format.setBottomMargin(after.at(paragraphIndex));
        QTextCursor(block).setBlockFormat(format);
    }
    reference.size();
    const qreal firstAscent = reference.begin().layout()->lineAt(0).ascent();
    const qreal textTop = added ? frame.y() * pageSize.height() + 1.0
        : originalOrigin.y() * pageSize.height() - firstAscent;
    QVector<qreal> expectedBaselines;
    for (QTextBlock block = reference.begin(); block.isValid(); block = block.next()) {
        for (int index = 0; index < block.text().size(); ++index) {
            if (!block.text().at(index).isSpace()) {
                const QTextLine line = block.layout()->lineForTextPosition(index);
                expectedBaselines.append(textTop
                    + before.front() + block.layout()->position().y() + line.y() + line.ascent());
            }
        }
    }
    const auto readOrigins = [](const QString &path, QVector<QPointF> &origins) {
        const QByteArray encodedPath = QFile::encodeName(path);
        FPDF_DOCUMENT pdf = FPDF_LoadDocument(encodedPath.constData(), nullptr);
        if (!pdf) return false;
        const auto closePdf = qScopeGuard([&] { FPDF_CloseDocument(pdf); });
        FPDF_PAGE page = FPDF_LoadPage(pdf, 0);
        if (!page) return false;
        const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        if (!textPage) return false;
        const auto closeText = qScopeGuard([&] { FPDFText_ClosePage(textPage); });
        QHash<FPDF_PAGEOBJECT, int> order;
        for (int index = 0; index < FPDFPage_CountObjects(page); ++index)
            order.insert(FPDFPage_GetObject(page, index), index);
        QVector<int> characters;
        for (int index = 0; index < FPDFText_CountChars(textPage); ++index) {
            const QChar character(static_cast<char16_t>(FPDFText_GetUnicode(textPage, index)));
            if (FPDFText_IsGenerated(textPage, index) != 1 && !character.isSpace())
                characters.append(index);
        }
        std::stable_sort(characters.begin(), characters.end(), [&](int a, int b) {
            return order.value(FPDFText_GetTextObject(textPage, a))
                < order.value(FPDFText_GetTextObject(textPage, b));
        });
        for (int index : characters) {
            double x = 0.0, y = 0.0;
            if (!FPDFText_GetCharOrigin(textPage, index, &x, &y)) return false;
            origins.append(QPointF(x, FPDF_GetPageHeightF(page) - y));
        }
        return true;
    };
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(firstPath)));
    QVector<QPointF> firstOrigins;
    QVERIFY(readOrigins(firstPath, firstOrigins));
    QCOMPARE(firstOrigins.size(), expectedBaselines.size());
    for (int index = 0; index < firstOrigins.size(); ++index)
        QVERIFY2(qAbs(firstOrigins.at(index).y() - expectedBaselines.at(index)) < 0.08,
                 qPrintable(QStringLiteral("Character %1: saved %2, Qt %3")
                    .arg(index).arg(firstOrigins.at(index).y()).arg(expectedBaselines.at(index))));
    const QPointF translation(0.02, 0.025);
    for (int pass = 0; pass < 3; ++pass) {
        const QVariantMap reopened = candidateForText(waitForCandidates(controller, 0), text.trimmed());
        QVERIFY2(!reopened.isEmpty(), "Saved paragraph must rehydrate as one editable candidate");
        QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
        QCOMPARE(controller.selectedText(), text);
        QCOMPARE(controller.selectedParagraphFormats(), expectedParagraphs);
        for (const QVariant &span : controller.selectedCharacterSpans()) {
            QCOMPARE(span.toMap().value(QStringLiteral("fontStretch")),
                     expectedCharacters.first().toMap().value(QStringLiteral("fontStretch")));
        }
        const QRectF expectedFrame = pass == 2 ? frame.translated(translation) : frame;
        QVERIFY(QLineF(controller.selectedFrameRect().topLeft(), expectedFrame.topLeft()).length() < 0.0001);
        QVERIFY(qAbs(controller.selectedFrameRect().width() - expectedFrame.width()) < 0.0001);
        QVERIFY(controller.selectedContentBounds().bottom()
            >= controller.selectedSourceRect().bottom() - 0.0001);
        controller.beginSelectedTextEdit();
        start = 0;
        for (int index = 0; index < paragraphs.size(); ++index) {
            controller.setSelectedTextRange(start, 0);
            QCOMPARE(controller.selectedLineSpacing(), factors.at(index));
            QVERIFY(qAbs(controller.selectedParagraphSpaceBefore() - before.at(index)) < 0.001);
            QVERIFY(qAbs(controller.selectedParagraphSpaceAfter() - after.at(index)) < 0.001);
            start += paragraphs.at(index).size() + 1;
        }
        controller.cancelSelectedTextEdit();
        QVERIFY(!controller.hasDrafts());
        if (pass == 0) {
            controller.setSelectedColor(Qt::blue);
            QVERIFY(controller.saveAs(QUrl::fromLocalFile(secondPath)));
        } else if (pass == 1) {
            controller.beginSelectedFrameResize();
            controller.updateSelectedFrameRect(controller.selectedFrameRect().translated(translation));
            controller.finishSelectedFrameResize();
            QVERIFY(controller.draftsForPage(0).first().toMap().value(QStringLiteral("sourceTextMoveOnly")).toBool());
            QVERIFY(controller.saveAs(QUrl::fromLocalFile(movedPath)));
        }
        if (pass < 2) {
            QVector<QPointF> savedOrigins;
            QVERIFY(readOrigins(pass == 0 ? secondPath : movedPath, savedOrigins));
            QCOMPARE(savedOrigins.size(), firstOrigins.size());
            const QPointF delta = pass == 0 ? QPointF()
                : QPointF(translation.x() * pageSize.width(), translation.y() * pageSize.height());
            for (int index = 0; index < savedOrigins.size(); ++index)
                QVERIFY2(QLineF(savedOrigins.at(index), firstOrigins.at(index) + delta).length() < 0.08,
                    qPrintable(QStringLiteral("Pass %1 char %2: (%3,%4) vs (%5,%6)")
                        .arg(pass).arg(index).arg(savedOrigins.at(index).x()).arg(savedOrigins.at(index).y())
                        .arg((firstOrigins.at(index) + delta).x()).arg((firstOrigins.at(index) + delta).y())));
        }
    }
    // Resetting all spacing to the defaults still keeps the explicit blank
    // paragraphs and frame instead of falling back to source-line heuristics.
    controller.setSelectedLineSpacing(1.0);
    controller.setSelectedParagraphSpaceBefore(0.0);
    controller.setSelectedParagraphSpaceAfter(0.0);
    const QVariantList resetParagraphs = controller.selectedParagraphFormats();
    const QRectF resetFrame = controller.selectedFrameRect();
    const QString resetPath = m_temporaryDirectory.filePath(
        QStringLiteral("paragraph-spacing-reset.pdf"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(resetPath)));
    const QVariantMap resetCandidate = candidateForText(
        waitForCandidates(controller, 0), text.trimmed());
    QVERIFY2(!resetCandidate.isEmpty(), "Reset spacing must retain the paragraph group");
    QVERIFY(controller.selectCandidate(
        0, resetCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedText(), text);
    QCOMPARE(controller.selectedParagraphFormats(), resetParagraphs);
    QCOMPARE(controller.selectedLineSpacing(), 1.0);
    QCOMPARE(controller.selectedParagraphSpaceBefore(), 0.0);
    QCOMPARE(controller.selectedParagraphSpaceAfter(), 0.0);
    QVERIFY(QLineF(controller.selectedFrameRect().topLeft(), resetFrame.topLeft()).length() < 0.0001);
    QVERIFY(qAbs(controller.selectedFrameRect().width() - resetFrame.width()) < 0.0001);
}

void PDFEditControllerTest::infersConstantSourceTextSpacing_data()
{
    QTest::addColumn<qreal>("letterSpacing");
    QTest::addColumn<qreal>("wordSpacing");
    QTest::addColumn<qreal>("matrixScale");
    QTest::newRow("positive-character-and-word") << 1.25 << 2.5 << 1.0;
    QTest::newRow("negative-character") << -0.2 << 1.0 << 1.0;
    QTest::newRow("word-only") << 0.0 << 2.0 << 1.0;
    QTest::newRow("unit-font-scaled-matrix") << 1.25 << 2.5 << 12.0;
    QTest::newRow("ordinary-text") << 0.0 << 0.0 << 1.0;
}

void PDFEditControllerTest::infersConstantSourceTextSpacing()
{
    QFETCH(qreal, letterSpacing);
    QFETCH(qreal, wordSpacing);
    QFETCH(qreal, matrixScale);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-spacing-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createSourceSpacingPdf(sourcePath, letterSpacing, wordSpacing, matrixScale));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Alpha beta gamma delta"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedFontSize(), 12.0);
    QCOMPARE(controller.selectedFontStretch(), 100);
    QVERIFY(qAbs(controller.selectedLetterSpacing() - letterSpacing) < 0.02);
    QVERIFY(qAbs(controller.selectedWordSpacing() - wordSpacing) < 0.02);
    const QVariantMap span = controller.selectedCharacterSpans().constFirst().toMap();
    QVERIFY(qAbs(span.value(QStringLiteral("letterSpacingRatio")).toReal()
                 * document.pageSize(0).height() - letterSpacing) < 0.02);
    QVERIFY(qAbs(span.value(QStringLiteral("wordSpacingRatio")).toReal()
                 * document.pageSize(0).height() - wordSpacing) < 0.02);
    const QRectF originalFrame = controller.selectedFrameRect();
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(controller.selectedText());
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
}

void PDFEditControllerTest::rejectsIrregularSourceTextSpacing()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("irregular-spacing.pdf"));
    QVERIFY(createSourceSpacingPdf(sourcePath, 0.0, 0.0, 1.0, true));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    QVERIFY(!candidates.isEmpty());
    for (const QVariant &value : candidates) {
        const QVariantMap candidate = value.toMap();
        if (candidate.value(QStringLiteral("kind")).toString() != QLatin1String("text")) {
            continue;
        }
        QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
        // A few TJ adjustments do not establish uniform character or word
        // spacing. Do not spread their average across all other characters.
        QVERIFY(qAbs(controller.selectedLetterSpacing()) < 0.02);
        QVERIFY(qAbs(controller.selectedWordSpacing()) < 0.02);
        QVERIFY(!controller.hasDrafts());
    }
}

void PDFEditControllerTest::preservesSourceClusterPositionsThroughEditorAndSave_data()
{
    QTest::addColumn<QByteArray>("content");
    QTest::addColumn<QString>("original");
    QTest::addColumn<qreal>("zoom");
    const QByteArray irregular("BT /F1 12 Tf 1 0 0 1 72 740 Tm "
        "[(un) -40 (der the fo) 35 (llowing con) -60 (ditions)] TJ ET\n");
    QTest::newRow("irregular-75-percent") << irregular
        << QStringLiteral("under the following conditions") << 0.75;
    QTest::newRow("irregular-155-percent") << irregular
        << QStringLiteral("under the following conditions") << 1.55;
    QTest::newRow("generated-word-gaps")
        << QByteArray("BT /F1 12 Tf 1 0 0 1 72 740 Tm "
                      "[(with) -250 (the) -400 (understanding) -300 (that:)] TJ ET\n")
        << QStringLiteral("with the understanding that:") << 1.55;
    QTest::newRow("precomposed-latin")
        << QByteArray("BT /F1 12 Tf 1 0 0 1 72 740 Tm "
                      "[(Caf\351) -80 ( du) 35 ( monde entier)] TJ ET\n")
        << QStringLiteral("Café du monde entier") << 1.0;
    QTest::newRow("natural-kerning")
        << QByteArray("BT /F1 12 Tf 1 0 0 1 72 740 Tm (AVATAR WAVE following) Tj ET\n")
        << QStringLiteral("AVATAR WAVE following") << 1.55;
    QTest::newRow("cumulative-small-adjustments")
        << QByteArray("BT /F1 12 Tf 1 0 0 1 72 740 Tm "
                      "[(u) -5 (n) -5 (d) -5 (e) -5 (r) -5 ( t) -5 (h) -5 (e) -5 "
                      "( fo) 5 (l) 5 (l) 5 (o) 5 (w) 5 (i) 5 (n) 5 (g conditions)] TJ ET\n")
        << QStringLiteral("under the following conditions") << 1.55;
    QTest::newRow("joined-source-object-gap")
        << QByteArray("BT /F1 12 Tf 1 0 0 1 72 740 Tm "
                      "[(un) -40 (der the)] TJ ET\n"
                      "BT /F1 12 Tf 1 0 0 1 128 740 Tm "
                      "[(fo) 35 (llowing con) -60 (ditions)] TJ ET\n")
        << QStringLiteral("under the following conditions") << 1.55;
    QTest::newRow("joined-plain-source-object-gap")
        << QByteArray("BT /F1 12 Tf 1 0 0 1 72 740 Tm (under the) Tj ET\n"
                      "BT /F1 12 Tf 1 0 0 1 128 740 Tm (following conditions) Tj ET\n")
        << QStringLiteral("under the following conditions") << 1.55;
    const QString vietnamese = QStringLiteral("Tiếng Việt giữ khoảng cách chữ nguồn");
    QTest::newRow("vietnamese-nfc-75-percent") << QByteArray() << vietnamese << 0.75;
    QTest::newRow("vietnamese-nfc-155-percent") << QByteArray() << vietnamese << 1.55;
}

void PDFEditControllerTest::preservesSourceClusterPositionsThroughEditorAndSave()
{
    QFETCH(QByteArray, content);
    QFETCH(QString, original);
    QFETCH(qreal, zoom);
    const QString tag = QString::fromLatin1(QTest::currentDataTag());
    const bool embeddedUnicode = content.isEmpty();
    const QString sourcePath = m_temporaryDirectory.filePath(tag + QStringLiteral("-clusters.pdf"));
    const QString firstPath = m_temporaryDirectory.filePath(tag + QStringLiteral("-clusters-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(tag + QStringLiteral("-clusters-second.pdf"));
    if (embeddedUnicode) {
        QFile fontFile(QFINDTESTDATA("../src/assets/fonts/Arial/Arial-Regular.TTF"));
        QVERIFY(fontFile.open(QIODevice::ReadOnly));
        QVERIFY(createPositionedUnicodeTextPdf(sourcePath, original, fontFile.readAll()));
        QCOMPARE(original, original.normalized(QString::NormalizationForm_C));
        PDFiumProcessLock lock;
        const auto sourceFont = textObjectFontInfo(sourcePath, original);
        QVERIFY(sourceFont.found);
        QCOMPARE(sourceFont.embedded, 1);
        QVERIFY(sourceFont.dataSize > 0);
    } else {
        QVERIFY(createPositionedTextPdf(sourcePath, content));
    }
    const QVector<qreal> originalOrigins = visibleCharacterOrigins(sourcePath);
    QString visible = original;
    visible.remove(QLatin1Char(' '));
    QCOMPARE(originalOrigins.size(), visible.size());
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedLetterSpacing(), 0.0);
    QCOMPARE(controller.selectedWordSpacing(), 0.0);
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    const QVector<qreal> advances = sourceAdvances(originalSpans, original.size());
    QVERIFY2(std::any_of(advances.cbegin(), advances.cend(), [](qreal value) {
        return qAbs(value) > 0.00001;
    }), "Irregular source positions must reach the editor as cluster advances");

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
    QTRY_COMPARE(textDocument->toPlainText(), original);
    QTRY_COMPARE(editor->property("lineCount").toInt(), 1);
    const QTextLine line = textDocument->begin().layout()->lineAt(0);
    QVERIFY(line.isValid());
    int glyph = 0;
    QVector<qreal> initialEditorOrigins;
    const qreal originX = controller.selectedOriginX() * pageSize.width();
    for (int index = 0; index < original.size(); ++index) {
        if (original.at(index).isSpace()) continue;
        const qreal editorOrigin = originX + line.cursorToX(index);
        initialEditorOrigins.append(editorOrigin);
        QVERIFY2(qAbs(editorOrigin - originalOrigins[glyph]) < 0.15,
                 qPrintable(QStringLiteral("Editor character %1: source=%2, editor=%3")
                     .arg(index).arg(originalOrigins[glyph]).arg(editorOrigin)));
        ++glyph;
    }
    // Native typing goes through TextEdit's event/undo path. New text must not
    // copy a source boundary correction, and one local undo restores it.
    const int insertion = 12;
    const QString typed = original.left(insertion) + QStringLiteral("x") + original.mid(insertion);
    auto *editorItem = qobject_cast<QQuickItem *>(editor);
    QVERIFY(editorItem);
    editorItem->forceActiveFocus();
    QTRY_VERIFY(editorItem->hasActiveFocus());
    QVERIFY(editor->setProperty("cursorPosition", insertion));
    QTest::keyClick(&window, Qt::Key_X);
    QTRY_COMPARE(textDocument->toPlainText(), typed);
    QTRY_COMPARE(controller.selectedText(), typed);
    constexpr int sourceAdvanceProperty = QTextFormat::UserProperty + 8;
    QTextCursor typedCursor(textDocument);
    for (int index : {insertion - 1, insertion}) {
        typedCursor.setPosition(index);
        typedCursor.setPosition(index + 1, QTextCursor::KeepAnchor);
        QCOMPARE(typedCursor.charFormat().doubleProperty(sourceAdvanceProperty), 0.0);
    }
    // A no-op native delete must not be mistaken for a new edit when undo
    // restores the original source corrections, even without an event-loop turn.
    QVERIFY(editor->setProperty("cursorPosition", typed.size()));
    QTest::keyClick(&window, Qt::Key_Delete);
    QCOMPARE(textDocument->toPlainText(), typed);
    QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
    QTRY_COMPARE(textDocument->toPlainText(), original);
    QTRY_COMPARE(controller.selectedText(), original);
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QVERIFY(QMetaObject::invokeMethod(editor, "redo"));
    QTRY_COMPARE(textDocument->toPlainText(), typed);
    QTRY_COMPARE(controller.selectedText(), typed);
    QVERIFY(editor->setProperty("cursorPosition", 0));
    QTest::keyClick(&window, Qt::Key_Backspace);
    QCoreApplication::processEvents();
    QCOMPARE(textDocument->toPlainText(), typed);
    QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
    QTRY_COMPARE(textDocument->toPlainText(), original);
    QTRY_COMPARE(controller.selectedText(), original);
    if (embeddedUnicode) {
        // This boundary carries a real TJ adjustment, so clearing it during
        // native input and recovering it with undo cannot pass vacuously.
        QVERIFY(qAbs(advances[insertion - 1]) > 0.00001);
        for (int operation = 0; operation < 3; ++operation) {
            const int removed = operation == 0 ? 0 : 3;
            const QString inserted = operation == 2 ? QString() : QStringLiteral("ế");
            const QString expected = original.left(insertion) + inserted
                + original.mid(insertion + removed);
            if (operation == 0) {
                QVERIFY(editor->setProperty("cursorPosition", insertion));
                QInputMethodEvent preedit(QStringLiteral("e"), {});
                QCoreApplication::sendEvent(editorItem, &preedit);
                QCOMPARE(textDocument->toPlainText(), original);
                QCOMPARE(controller.selectedText(), original);
                QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
                QInputMethodEvent commit;
                commit.setCommitString(inserted);
                QCoreApplication::sendEvent(editorItem, &commit);
            } else if (operation == 1) {
                QVERIFY(editor->setProperty("cursorPosition", insertion + 2));
                QInputMethodEvent replacement;
                replacement.setCommitString(inserted, -2, removed);
                QCoreApplication::sendEvent(editorItem, &replacement);
            } else {
                QVERIFY(QMetaObject::invokeMethod(editor, "select",
                    Q_ARG(int, insertion), Q_ARG(int, insertion + removed)));
                QTest::keyClick(&window, Qt::Key_Backspace);
            }
            QTRY_COMPARE(textDocument->toPlainText(), expected);
            QTRY_COMPARE(controller.selectedText(), expected);
            const QVariantList editedSpans = controller.selectedCharacterSpans();
            const auto editedAdvances = sourceAdvances(editedSpans, expected.size());
            for (int index = 0; index < insertion - 1; ++index)
                QCOMPARE(editedAdvances[index], advances[index]);
            for (int index = insertion - 1; index < insertion + inserted.size(); ++index) {
                QCOMPARE(editedAdvances[index], 0.0);
                typedCursor.setPosition(index);
                typedCursor.setPosition(index + 1, QTextCursor::KeepAnchor);
                QCOMPARE(typedCursor.charFormat().doubleProperty(sourceAdvanceProperty), 0.0);
            }
            for (int index = insertion + removed; index < original.size(); ++index)
                QCOMPARE(editedAdvances[index - removed + inserted.size()], advances[index]);

            QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
            QTRY_COMPARE(textDocument->toPlainText(), original);
            QTRY_COMPARE(controller.selectedText(), original);
            QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
            const QTextLine restoredLine = textDocument->begin().layout()->lineAt(0);
            QVERIFY(restoredLine.isValid());
            int restoredGlyph = 0;
            for (int index = 0; index < original.size(); ++index) {
                if (original.at(index).isSpace()) continue;
                QVERIFY(qAbs(originX + restoredLine.cursorToX(index)
                             - originalOrigins[restoredGlyph++]) < 0.15);
            }
            QVERIFY(QMetaObject::invokeMethod(editor, "redo"));
            QTRY_COMPARE(textDocument->toPlainText(), expected);
            QTRY_COMPARE(controller.selectedText(), expected);
            QCOMPARE(controller.selectedCharacterSpans(), editedSpans);
            QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
            QTRY_COMPARE(textDocument->toPlainText(), original);
            QTRY_COMPARE(controller.selectedText(), original);
            QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
        }
    }
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);

    // Color forces actual object replacement while preserving source advances.
    controller.setSelectedColor(Qt::blue);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(firstPath)));
    QCOMPARE(normalizedPageText(firstPath), original);
    const QVector<qreal> firstOrigins = visibleCharacterOrigins(firstPath);
    QCOMPARE(firstOrigins.size(), originalOrigins.size());
    for (int index = 0; index < firstOrigins.size(); ++index) {
        QVERIFY2(qAbs(firstOrigins[index] - initialEditorOrigins[index]) < 0.15,
                 qPrintable(QStringLiteral("First Save character %1: source=%2, saved=%3")
                     .arg(index).arg(initialEditorOrigins[index]).arg(firstOrigins[index])));
    }
    const QVariantMap reopened = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY2(!reopened.isEmpty(), "Positioned clusters must reopen as one editable text group");
    QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedLetterSpacing(), 0.0);
    QCOMPARE(controller.selectedWordSpacing(), 0.0);
    const QVector<qreal> reopenedAdvances = sourceAdvances(controller.selectedCharacterSpans(), original.size());
    for (int index = 0; index < advances.size(); ++index)
        QVERIFY(qAbs((reopenedAdvances[index] - advances[index]) * pageSize.height()) < 0.001);
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginEditing",
                                     Q_ARG(QVariant, QVariant(reopened))));
    editorItem->forceActiveFocus();
    QVERIFY(editor->setProperty("cursorPosition", original.size()));
    for (Qt::Key key : {Qt::Key_Space, Qt::Key_M, Qt::Key_O, Qt::Key_R, Qt::Key_E}) {
        QTest::keyClick(&window, key);
        QCoreApplication::processEvents();
    }
    QTRY_COMPARE(controller.selectedText(), original + QStringLiteral(" more"));
    const QTextLine editedLine = textDocument->begin().layout()->lineAt(0);
    QVector<qreal> editorOrigins;
    for (int index = 0; index < controller.selectedText().size(); ++index) {
        if (!controller.selectedText().at(index).isSpace())
            editorOrigins.append(controller.selectedOriginX() * pageSize.width()
                                 + editedLine.cursorToX(index));
    }
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(secondPath)));
    {
        PDFiumProcessLock lock;
        QCOMPARE(normalizedPageText(secondPath), original + QStringLiteral(" more"));
    }
    const QVector<qreal> secondOrigins = visibleCharacterOrigins(secondPath);
    QCOMPARE(secondOrigins.size(), originalOrigins.size() + 4);
    QCOMPARE(secondOrigins.size(), editorOrigins.size());
    for (int index = 0; index < secondOrigins.size(); ++index)
        QVERIFY(qAbs(secondOrigins[index] - editorOrigins[index]) < 0.15);
    for (int index = 0; index < originalOrigins.size(); ++index)
        QVERIFY(qAbs(secondOrigins[index] - originalOrigins[index]) < 0.15);
}

void PDFEditControllerTest::rejectsDamagedSourceClusterMetadata()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("cluster-metadata-source.pdf"));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("cluster-metadata-valid.pdf"));
    const QString damaged = m_temporaryDirectory.filePath(QStringLiteral("cluster-metadata-damaged.pdf"));
    QVERIFY(createPositionedTextPdf(source, QByteArray(
        "BT /F1 12 Tf 1 0 0 1 72 740 Tm [(under) -40 ( the following) 35 ( conditions)] TJ ET\n")));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    const auto candidate = candidateForText(waitForCandidates(controller, 0),
                                            QStringLiteral("under the following conditions"));
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.setSelectedColor(Qt::blue);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    const auto valid = candidateForText(waitForCandidates(controller, 0),
                                        QStringLiteral("under the following conditions"));
    QVERIFY(valid.value(QStringLiteral("objectId")).toString().startsWith(QLatin1String("paragraph-layout:")));
    {
        PDFiumProcessLock lock;
        FPDF_DOCUMENT pdf = FPDF_LoadDocument(QFile::encodeName(saved).constData(), nullptr);
        QVERIFY(pdf);
        const auto closePdf = qScopeGuard([&] { FPDF_CloseDocument(pdf); });
        FPDF_PAGE page = FPDF_LoadPage(pdf, 0);
        QVERIFY(page);
        const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
        bool changed = false;
        for (int i = 0; i < FPDFPage_CountObjects(page) && !changed; ++i) {
            const auto object = FPDFPage_GetObject(page, i);
            for (int j = 0; j < FPDFPageObj_CountMarks(object); ++j) {
                const auto mark = FPDFPageObj_GetMark(object, j);
                float advance = 0.0F;
                if (FPDFPageObjMark_GetParamFloatValue(mark, "SourceAdvance", &advance)) {
                    QVERIFY(FPDFPageObjMark_SetFloatParam(pdf, object, mark, "SourceAdvance", 513.0F));
                    changed = true;
                    break;
                }
            }
        }
        QVERIFY(changed);
        QVERIFY(FPDFPage_GenerateContent(page));
        PdfFileWriter writer(damaged);
        QVERIFY(writer.open());
        QVERIFY(FPDF_SaveAsCopy(pdf, &writer, FPDF_NO_INCREMENTAL));
    }
    QVERIFY(document.open(QUrl::fromLocalFile(damaged)));
    const auto candidates = waitForCandidates(controller, 0);
    QVERIFY(!candidates.isEmpty());
    for (const auto &value : candidates) {
        QVERIFY(!value.toMap().value(QStringLiteral("objectId")).toString()
                     .startsWith(QLatin1String("paragraph-layout:")));
    }
}

void PDFEditControllerTest::editsSourceClusterSpacingWithUndo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("cluster-edit-history.pdf"));
    QVERIFY(createPositionedTextPdf(sourcePath, QByteArray(
        "BT /F1 12 Tf 1 0 0 1 72 740 Tm "
        "[(un) -40 (der the fo) 35 (llowing con) -60 (ditions)] TJ ET\n")));
    const QString original = QStringLiteral("under the following conditions");
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    const QVector<qreal> originalAdvances = sourceAdvances(originalSpans, original.size());
    QVERIFY(std::any_of(originalAdvances.cbegin(), originalAdvances.cend(), [](qreal v) {
        return qAbs(v) > 0.00001;
    }));
    const int insertion = 12;
    const QString edited = original.left(insertion) + QStringLiteral("XX") + original.mid(insertion);
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(edited, insertion + 2);
    controller.finishSelectedTextEdit();
    const QVariantList editedSpans = controller.selectedCharacterSpans();
    const QVector<qreal> editedAdvances = sourceAdvances(editedSpans, edited.size());
    for (int index = 0; index < insertion - 1; ++index)
        QCOMPARE(editedAdvances[index], originalAdvances[index]);
    for (int index = insertion - 1; index < insertion + 2; ++index)
        QCOMPARE(editedAdvances[index], 0.0);
    for (int index = insertion; index < original.size(); ++index)
        QCOMPARE(editedAdvances[index + 2], originalAdvances[index]);
    controller.undo();
    QCOMPARE(controller.selectedText(), original);
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(controller.selectedText(), edited);
    QCOMPARE(controller.selectedCharacterSpans(), editedSpans);

    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(original, insertion);
    controller.finishSelectedTextEdit();
    const QVector<qreal> deletedAdvances = sourceAdvances(controller.selectedCharacterSpans(), original.size());
    QCOMPARE(deletedAdvances[insertion - 1], 0.0);
    for (int index = insertion; index < original.size(); ++index)
        QCOMPARE(deletedAdvances[index], originalAdvances[index]);
    controller.undo();
    QCOMPARE(controller.selectedText(), edited);
    QCOMPARE(controller.selectedCharacterSpans(), editedSpans);
    controller.discardDrafts();
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.setSelectedLetterSpacing(1.25);
    const QVector<qreal> explicitAdvances = sourceAdvances(controller.selectedCharacterSpans(), original.size());
    QVERIFY(std::all_of(explicitAdvances.cbegin(), explicitAdvances.cend(), [](qreal v) { return v == 0.0; }));
    controller.undo();
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::editsSpacingInSelectedRangeAndAtCaret()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0, m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const QString original = m_controller.selectedText();
    const QVariantList originalSpans = m_controller.selectedCharacterSpans();
    const qreal originalLetter = m_controller.selectedLetterSpacing();
    const qreal originalWord = m_controller.selectedWordSpacing();
    for (qreal invalid : {-10.01, 50.01, std::numeric_limits<qreal>::infinity(),
                          std::numeric_limits<qreal>::quiet_NaN()}) {
        m_controller.setSelectedLetterSpacing(invalid);
        m_controller.setSelectedWordSpacing(invalid);
        QCOMPARE(m_controller.selectedCharacterSpans(), originalSpans);
        QVERIFY(!m_controller.hasDrafts());
    }
    m_controller.setSelectedLetterSpacing(originalLetter);
    m_controller.setSelectedWordSpacing(originalWord);
    QVERIFY(!m_controller.hasDrafts());
    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(8, 3);
    m_controller.setSelectedLetterSpacing(1.25);
    m_controller.setSelectedWordSpacing(2.5);
    QCOMPARE(m_controller.selectedLetterSpacing(), 1.25);
    QCOMPARE(m_controller.selectedWordSpacing(), 2.5);
    const QVariantList formatted = m_controller.selectedCharacterSpans();
    QCOMPARE(formatted.size(), 3);
    QCOMPARE(formatted.at(1).toMap().value(QStringLiteral("start")).toInt(), 8);
    QCOMPARE(formatted.at(1).toMap().value(QStringLiteral("length")).toInt(), 3);
    m_controller.setSelectedTextRange(0, original.size());
    QVERIFY(qIsNaN(m_controller.selectedLetterSpacing()));
    QVERIFY(qIsNaN(m_controller.selectedWordSpacing()));
    m_controller.setSelectedTextRange(1, 2);
    QCOMPARE(m_controller.selectedLetterSpacing(), originalLetter);
    QCOMPARE(m_controller.selectedWordSpacing(), originalWord);
    m_controller.finishSelectedTextEdit();
    m_controller.undo();
    QVERIFY(!m_controller.hasDrafts());
    QCOMPARE(m_controller.selectedCharacterSpans(), originalSpans);
    m_controller.redo();
    QCOMPARE(m_controller.selectedCharacterSpans(), formatted);
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0, m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(8, 0);
    m_controller.setSelectedLetterSpacing(-0.25);
    m_controller.setSelectedWordSpacing(3.0);
    QCOMPARE(m_controller.selectedLetterSpacing(), -0.25);
    QCOMPARE(m_controller.selectedWordSpacing(), 3.0);
    QVERIFY(!m_controller.hasDrafts());
    m_controller.finishSelectedTextEdit();
    QVERIFY(!m_controller.hasDrafts());
    QCOMPARE(m_controller.selectedCharacterSpans(), originalSpans);
    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(8, 0);
    m_controller.setSelectedLetterSpacing(-0.25);
    m_controller.setSelectedWordSpacing(3.0);
    const QString edited = original.left(8) + QStringLiteral("new") + original.mid(8);
    m_controller.updateSelectedTextPreview(edited, 11);
    m_controller.setSelectedTextRange(8, 3);
    QCOMPARE(m_controller.selectedLetterSpacing(), -0.25);
    QCOMPARE(m_controller.selectedWordSpacing(), 3.0);
    m_controller.finishSelectedTextEdit();
    m_controller.undo();
    QCOMPARE(m_controller.selectedText(), original);
    QVERIFY(!m_controller.hasDrafts());
    m_controller.redo();
    QCOMPARE(m_controller.selectedText(), edited);
    m_controller.discardDrafts();
}

void PDFEditControllerTest::keepsSourceSpacingThroughQmlEditing_data()
{
    QTest::addColumn<qreal>("zoom");
    QTest::newRow("75-percent") << 0.75;
    QTest::newRow("155-percent") << 1.55;
    QTest::newRow("200-percent") << 2.0;
}

void PDFEditControllerTest::keepsSourceSpacingThroughQmlEditing()
{
    QFETCH(qreal, zoom);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("source-spacing-qml-%1.pdf").arg(zoom));
    QVERIFY(createSourceSpacingPdf(sourcePath, 1.25, 2.5));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QString original = QStringLiteral("Alpha beta gamma delta");
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), original);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QVariantList originalSpans = controller.selectedCharacterSpans();
    const QRectF originalFrame = controller.selectedFrameRect();
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
    QTRY_COMPARE(textDocument->toPlainText(), original);
    QTRY_COMPARE(editor->property("lineCount").toInt(), 1);
    QTextCursor cursor(textDocument);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    const QFont sourceFont = cursor.charFormat().font();
    QCOMPARE(sourceFont.letterSpacingType(), QFont::AbsoluteSpacing);
    QVERIFY(qAbs(sourceFont.letterSpacing() - 1.25) < 0.02);
    QVERIFY(qAbs(sourceFont.wordSpacing() - 2.5) < 0.02);
    QVERIFY(qAbs(sourceFont.pixelSize() * qobject_cast<QQuickItem *>(editor)->scale()
                 - 12.0 * zoom) < 0.02);
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
    QCOMPARE(controller.selectedFrameRect(), originalFrame);

    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginEditing",
                                     Q_ARG(QVariant, QVariant(candidate))));
    QVERIFY(QMetaObject::invokeMethod(editor, "selectAll"));
    controller.setSelectedTextRange(0, original.size());
    controller.setSelectedLetterSpacing(2.0);
    cursor.setPosition(0);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QVERIFY(qAbs(cursor.charFormat().font().letterSpacing() - 2.0) < 0.02);
    QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
    QTRY_VERIFY(qAbs(controller.selectedLetterSpacing() - 1.25) < 0.02);
    QVERIFY(QMetaObject::invokeMethod(editor, "redo"));
    QTRY_VERIFY(qAbs(controller.selectedLetterSpacing() - 2.0) < 0.02);
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "cancelEditing"));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);
}

void PDFEditControllerTest::savesTextSpacingWithoutStretchingGlyphs_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("sourceSpacing");
    QTest::addColumn<qreal>("letterSpacing");
    QTest::addColumn<qreal>("wordSpacing");
    QTest::addColumn<bool>("mixedSpaces");
    QTest::newRow("ascii") << QStringLiteral("Alpha beta gamma delta") << false << 1.25 << 2.5 << false;
    QTest::newRow("source-spacing") << QStringLiteral("Alpha beta gamma delta") << true << 1.25 << 2.5 << false;
    QTest::newRow("vietnamese") << QStringLiteral("Tiếng Việt hòa bình") << false << 1.25 << 2.5 << false;
    QTest::newRow("combining-vietnamese")
        << QStringLiteral("Tie\u0302\u0301ng Vie\u0323\u0302t ho\u0300a bi\u0300nh") << false << 1.25 << 2.5 << false;
    QTest::newRow("word-only") << QStringLiteral("Alpha beta gamma delta") << false << 0.0 << 2.5 << false;
    QTest::newRow("negative-character") << QStringLiteral("Alpha beta gamma delta") << false << -0.25 << 0.0 << false;
    QTest::newRow("overlapping-negative-character") << QStringLiteral("Alpha beta gamma delta") << false << -3.0 << 0.0 << false;
    QTest::newRow("leading-spaces-and-tab") << QStringLiteral("  \tAlpha beta gamma delta") << false << 1.25 << 2.5 << false;
    QTest::newRow("mixed-space-format") << QStringLiteral("Alpha  beta gamma delta  ") << false << 0.0 << 0.0 << true;
}

void PDFEditControllerTest::savesTextSpacingWithoutStretchingGlyphs()
{
    QFETCH(QString, text);
    QFETCH(bool, sourceSpacing);
    QFETCH(qreal, letterSpacing);
    QFETCH(qreal, wordSpacing);
    QFETCH(bool, mixedSpaces);
    const QString tag = QString::fromLatin1(QTest::currentDataTag());
    const QString sourcePath = m_temporaryDirectory.filePath(tag + QStringLiteral("-spacing-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(tag + QStringLiteral("-spacing-output.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(tag + QStringLiteral("-spacing-second.pdf"));
    QVERIFY(createSourceSpacingPdf(sourcePath, sourceSpacing ? 1.25 : 0.0,
                                  sourceSpacing ? 2.5 : 0.0));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QStringLiteral("Alpha beta gamma delta"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(text);
    controller.setSelectedTextRange(0, controller.selectedText().size());
    if (sourceSpacing) {
        // Force source object replacement while retaining automatically read
        // spacing, rather than merely copying the unchanged content stream.
        controller.setSelectedColor(Qt::blue);
    } else {
        controller.setSelectedLetterSpacing(letterSpacing);
        controller.setSelectedWordSpacing(wordSpacing);
    }
    if (mixedSpaces) {
        // A format change on the separator itself must survive even though
        // PDF output stores that separator with the preceding visible cluster.
        controller.setSelectedTextRange(5, 2);
        controller.setSelectedLetterSpacing(0.5);
        controller.setSelectedWordSpacing(3.0);
        controller.setSelectedTextRange(text.size() - 2, 2);
        controller.setSelectedLetterSpacing(1.5);
        controller.setSelectedWordSpacing(4.0);
        controller.setSelectedTextRange(0, text.size());
    }
    const QVariantList expectedSpans = controller.selectedCharacterSpans();
    const QSizeF pageSize = document.pageSize(0);
    const qreal contentLeft = controller.selectedOriginX() * pageSize.width();
    QFont font = controller.fontForDisplay(
        controller.selectedFontFamily(), controller.selectedFontStretch(),
        controller.selectedFontBold(), controller.selectedFontItalic(),
        controller.selectedFontUnderline(),
        controller.selectedDisplayFontSizeRatio() * pageSize.height());
    font.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
    font.setWordSpacing(wordSpacing);
    const qreal expectedHorizontalScale = controller.selectedFontStretch() / 100.0;
    controller.finishSelectedTextEdit();
    const QString normalized = text.normalized(QString::NormalizationForm_C);
    QTextLayout layout(normalized, font);
    if (mixedSpaces) {
        QList<QTextLayout::FormatRange> formats;
        for (const QVariant &value : expectedSpans) {
            const QVariantMap span = value.toMap();
            QFont spanFont = font;
            spanFont.setLetterSpacing(QFont::AbsoluteSpacing,
                span.value(QStringLiteral("letterSpacingRatio")).toReal() * pageSize.height());
            spanFont.setWordSpacing(
                span.value(QStringLiteral("wordSpacingRatio")).toReal() * pageSize.height());
            QTextLayout::FormatRange range;
            range.start = span.value(QStringLiteral("start")).toInt();
            range.length = span.value(QStringLiteral("length")).toInt();
            range.format.setFont(spanFont);
            formats.append(range);
        }
        layout.setFormats(formats);
    }
    layout.beginLayout();
    QTextLine line = layout.createLine();
    QVERIFY(line.isValid());
    line.setLineWidth(1000.0);
    layout.endLayout();
    QVector<qreal> expectedOrigins;
    for (int index = 0; index < normalized.size(); ++index) {
        if (!normalized.at(index).isSpace()) {
            expectedOrigins.append(contentLeft + line.cursorToX(index));
        }
    }
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVector<qreal> firstSavedOrigins;
    for (const QString &path : {outputPath, secondPath}) {
        if (path == secondPath) {
            // A second save after rehydration must retain the positions and
            // semantic spacing of the first, without accumulating widening.
            const QVariantList reopenedCandidates = waitForCandidates(controller, 0);
            const QVariantMap reopened = candidateForText(reopenedCandidates, normalized.trimmed());
            QVERIFY(!reopened.isEmpty());
            QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
            QCOMPARE(controller.selectedText(), normalized);
            controller.beginSelectedTextEdit();
            if (mixedSpaces) {
                controller.setSelectedTextRange(5, 2);
                QVERIFY(qAbs(controller.selectedLetterSpacing() - 0.5) < 0.02);
                QVERIFY(qAbs(controller.selectedWordSpacing() - 3.0) < 0.02);
                controller.setSelectedTextRange(normalized.size() - 2, 2);
                QVERIFY(qAbs(controller.selectedLetterSpacing() - 1.5) < 0.02);
                QVERIFY(qAbs(controller.selectedWordSpacing() - 4.0) < 0.02);
                controller.setSelectedTextRange(0, 5);
                QVERIFY(qAbs(controller.selectedLetterSpacing()) < 0.02);
                QVERIFY(qAbs(controller.selectedWordSpacing()) < 0.02);
            } else {
                QVERIFY(qAbs(controller.selectedLetterSpacing() - letterSpacing) < 0.02);
                QVERIFY(qAbs(controller.selectedWordSpacing() - wordSpacing) < 0.02);
            }
            controller.updateSelectedTextPreview(controller.selectedText());
            controller.finishSelectedTextEdit();
            QVERIFY(!controller.hasDrafts());
            controller.setSelectedColor(Qt::green);
            QVERIFY(controller.hasDrafts());
            QVERIFY(controller.saveAs(QUrl::fromLocalFile(secondPath)));
        }
        const QByteArray encodedPath = QFile::encodeName(path);
        FPDF_DOCUMENT saved = FPDF_LoadDocument(encodedPath.constData(), nullptr);
        QVERIFY(saved);
        const auto closeDocument = qScopeGuard([&]() { FPDF_CloseDocument(saved); });
        FPDF_PAGE page = FPDF_LoadPage(saved, 0);
        QVERIFY(page);
        const auto closePage = qScopeGuard([&]() { FPDF_ClosePage(page); });
        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        QVERIFY(textPage);
        const auto closeTextPage = qScopeGuard([&]() { FPDFText_ClosePage(textPage); });
        QString denseText;
        QVector<qreal> origins;
        QHash<FPDF_PAGEOBJECT, int> objectOrder;
        for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
            objectOrder.insert(FPDFPage_GetObject(page, index), index);
        }
        QVector<int> characters;
        for (int index = 0; index < FPDFText_CountChars(textPage); ++index) {
            if (FPDFText_IsGenerated(textPage, index) != 1) {
                characters.append(index);
            }
        }
        // The extraction engine can reorder overlapping glyphs spatially.
        // Verify their persisted content order and positions independently.
        std::stable_sort(characters.begin(), characters.end(), [&](int a, int b) {
            return objectOrder.value(FPDFText_GetTextObject(textPage, a))
                < objectOrder.value(FPDFText_GetTextObject(textPage, b));
        });
        for (int index : std::as_const(characters)) {
            const QChar character(static_cast<char16_t>(FPDFText_GetUnicode(textPage, index)));
            if (character.isSpace()) {
                continue;
            }
            double x = 0.0;
            double y = 0.0;
            QVERIFY(FPDFText_GetCharOrigin(textPage, index, &x, &y));
            origins.append(x);
            denseText.append(character);
        }
        QString expectedDense = normalized;
        expectedDense.remove(QRegularExpression(QStringLiteral("\\s")));
        QCOMPARE(denseText.normalized(QString::NormalizationForm_C), expectedDense);
        QCOMPARE(origins.size(), expectedOrigins.size());
        for (qsizetype index = 0; index < origins.size(); ++index) {
            QVERIFY2(qAbs(origins.at(index) - expectedOrigins.at(index)) < 0.15,
                     qPrintable(QStringLiteral("%1 character %2: expected x=%3, saved x=%4")
                         .arg(path).arg(index).arg(expectedOrigins.at(index)).arg(origins.at(index))));
        }
        // Text spacing changes advances between glyphs, not their outlines.
        for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_TEXT) {
                continue;
            }
            FS_MATRIX matrix{};
            QVERIFY(FPDFPageObj_GetMatrix(object, &matrix));
            QVERIFY(qAbs(horizontalScale(matrix) - expectedHorizontalScale) < 0.001);
        }
        if (firstSavedOrigins.isEmpty()) {
            firstSavedOrigins = origins;
        } else {
            QCOMPARE(origins, firstSavedOrigins);
        }
    }
}

void PDFEditControllerTest::restoresSpacingOnDensePage()
{
    // A normal dense page can contain thousands of individually positioned
    // clusters. Unrelated rows must not consume its grouping comparison budget.
    QByteArray content;
    for (int row = 0; row < 80; ++row) {
        const QByteArray group = QStringLiteral("00000000-0000-4000-8000-%1")
            .arg(row + 1, 12, 10, QLatin1Char('0')).toLatin1();
        for (int column = 0; column < 80; ++column) {
            content += "/TrePDFTextSpacing << /Version 1 /Group (" + group
                + ") /Index " + QByteArray::number(column)
                + " /Count 80 /Start " + QByteArray::number(column)
                + " /Text (A) /LetterSpacing 1.25 /WordSpacing 0 >> BDC\n"
                + "BT /F1 6 Tf 1 0 0 1 " + QByteArray::number(72.0 + column * 5.252, 'f', 3)
                + " " + QByteArray::number(820.0 - row * 9.6, 'f', 3)
                + " Tm (A) Tj ET EMC\n";
        }
    }
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("dense-spacing.pdf"));
    QVERIFY(writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                   "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>"),
        QByteArray("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                   "/Encoding /WinAnsiEncoding >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream"}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 0);
    QVERIFY(!candidates.isEmpty());
    int lineCount = 0;
    for (const QVariant &value : candidates) {
        const QVariantMap candidate = value.toMap();
        const QStringList lines = candidate.value(QStringLiteral("text")).toString()
            .split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            QCOMPARE(line, QString(80, QLatin1Char('A')));
            ++lineCount;
        }
        QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
        QVERIFY(qAbs(controller.selectedLetterSpacing() - 1.25) < 0.02);
        QCOMPARE(controller.selectedWordSpacing(), 0.0);
    }
    QCOMPARE(lineCount, 80);
}
