#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFFontAliases.h"

#include <QCryptographicHash>
#include <QGlyphRun>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::isolatesType1FontNames()
{
    QFile fixture(QFINDTESTDATA("fixtures/fonts/type1/trepdf-test-mono.pfa"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const QByteArray source = fixture.readAll();
    const QString semanticFamily = QStringLiteral("TrePDF Test Mono");
    const QRawFont original(source, 16.0, QFont::PreferNoHinting);
    QVERIFY(original.isValid());
    QCOMPARE(original.familyName(), semanticFamily);
    const auto alias = PDFFontAliases::isolate(source, semanticFamily);
    QVERIFY(!alias.data.isEmpty());
    QVERIFY(alias.family.startsWith(QLatin1String("TrePDFE")));
    QCOMPARE(alias.semanticFamily, semanticFamily);
    QVERIFY(alias.source);
    QCOMPARE(alias.fingerprint, QCryptographicHash::hash(source, QCryptographicHash::Sha256));
    const qsizetype encryptedStart = source.indexOf("currentfile eexec\n");
    QVERIFY(encryptedStart > 0);
    QCOMPARE(alias.data.mid(alias.data.indexOf("currentfile eexec\n")),
             source.mid(encryptedStart));
    QVERIFY(alias.data.contains("/FontName /" + alias.family.toLatin1() + " def"));
    QVERIFY(alias.data.contains("/FamilyName (" + alias.family.toLatin1() + ") readonly def"));
    QVERIFY(alias.data.contains("/FullName (" + alias.family.toLatin1() + ") readonly def"));
    const QRawFont renamed(alias.data, 16.0, QFont::PreferNoHinting);
    QVERIFY(renamed.isValid());
    QCOMPARE(renamed.familyName(), alias.family);
    const QString text = QStringLiteral("http://creativecommons.org/licenses/by/3.0/.");
    const auto originalGlyphs = original.glyphIndexesForString(text);
    QCOMPARE(renamed.glyphIndexesForString(text), originalGlyphs);
    QCOMPARE(renamed.advancesForGlyphIndexes(originalGlyphs),
             original.advancesForGlyphIndexes(originalGlyphs));
    for (quint32 glyph : originalGlyphs) {
        QVERIFY(glyph != 0);
        QCOMPARE(renamed.pathForGlyph(glyph), original.pathForGlyph(glyph));
    }

    QStringList retained;
    const auto releaseFonts = qScopeGuard([&] {
        for (const QString &family : std::as_const(retained))
            PDFFontAliases::release(family);
    });
    QVERIFY(PDFFontAliases::retain(alias));
    retained.append(alias.family);
    QVERIFY(PDFFontAliases::retain(alias));
    retained.append(alias.family);
    QCOMPARE(PDFFontAliases::semanticFamily(alias.family), semanticFamily);
    QCOMPARE(PDFFontAliases::sourceFingerprint(alias.family), alias.fingerprint);
    QVERIFY(PDFFontAliases::matchesStyle(alias.family, false, false));
    QCOMPARE(PDFFontAliases::isolate(source, semanticFamily).family, alias.family);
    PDFFontAliases::release(retained.takeLast());
    QCOMPARE(PDFFontAliases::sourceFingerprint(alias.family), alias.fingerprint);
    QCOMPARE(PDFFontAliases::isolate(source, semanticFamily).family, alias.family);

    QByteArray widerProgram = source;
    widerProgram.replace("/FontMatrix [0.001 0 0 0.001 0 0]",
                         "/FontMatrix [0.002 0 0 0.001 0 0]");
    QVERIFY(widerProgram != source);
    const QRawFont wider(widerProgram, 16.0, QFont::PreferNoHinting);
    QVERIFY(wider.isValid());
    QCOMPARE(wider.familyName(), semanticFamily);
    const auto widerAlias = PDFFontAliases::isolate(widerProgram, semanticFamily);
    QVERIFY(!widerAlias.data.isEmpty());
    QVERIFY(widerAlias.family != alias.family);
    QVERIFY(widerAlias.fingerprint != alias.fingerprint);
    QVERIFY(PDFFontAliases::retain(widerAlias));
    retained.append(widerAlias.family);
    QCOMPARE(PDFFontAliases::sourceFingerprint(alias.family), alias.fingerprint);
    QCOMPARE(PDFFontAliases::sourceFingerprint(widerAlias.family), widerAlias.fingerprint);
    PDFFontAliases::release(retained.takeFirst());
    QVERIFY(PDFFontAliases::sourceFingerprint(alias.family).isEmpty());
    const auto reloaded = PDFFontAliases::isolate(source, semanticFamily);
    QVERIFY(!reloaded.data.isEmpty());
    QVERIFY(reloaded.family != alias.family);
    QCOMPARE(reloaded.fingerprint, alias.fingerprint);

    // Tokens inside comments/strings must not be treated as executable names
    // or as the boundary of the encrypted Type 1 program.
    QByteArray decoys = source;
    decoys.replace("10 dict begin\n", "10 dict begin\n"
        "% /FontName /Fake def /FamilyName (Fake) def currentfile eexec\n");
    decoys.replace("(Synthetic TrePDF regression font)",
        "(/FontName /Fake def /FamilyName (Fake) def currentfile eexec)");
    QVERIFY(!PDFFontAliases::isolate(decoys, semanticFamily).data.isEmpty());

    const QList<QPair<QByteArray, QByteArray>> invalidHeaders{
        {"/FontName /TrePDFTestMono-Regular def", ""},
        {"/FontName /TrePDFTestMono-Regular def", "/FontName /First def /FontName /Second def"},
        {"/FontName /TrePDFTestMono-Regular def", "/FontName (TrePDFTestMono-Regular) cvn def"},
        {"/FamilyName (TrePDF Test Mono) readonly def", "/FamilyName FamilyName def"},
        {"/Weight (Regular) readonly def", "/Weight (Regular) readonly def /SavedName FontName def"},
        {"/FullName (TrePDF Test Mono Regular) readonly def", "/FullName (unterminated"},
        {"/FontType 1 def", "/FontType 42 def"},
        {"/FontName /TrePDFTestMono-Regular def", "{/FontName /TrePDFTestMono-Regular def} exec"},
        {"/FontMatrix [0.001 0 0 0.001 0 0]", "/FontMatrix [0.001 0 0 0.001 0 0}"},
        {"currentfile eexec\n", "currentfile\n"}};
    for (const auto &replacement : invalidHeaders) {
        QByteArray malformed = source;
        malformed.replace(replacement.first, replacement.second);
        QVERIFY(malformed != source);
        QVERIFY2(PDFFontAliases::isolate(malformed, semanticFamily).data.isEmpty(),
                 replacement.second.constData());
    }
    QByteArray oversized = source;
    oversized.insert(source.indexOf("10 dict begin"), '%' + QByteArray(64 * 1024, 'x') + '\n');
    QVERIFY(PDFFontAliases::isolate(oversized, semanticFamily).data.isEmpty());
}

void PDFEditControllerTest::keepsParagraphSpacingThroughQmlEditing_data()
{
    QTest::addColumn<qreal>("zoom");
    QTest::newRow("75-percent") << 0.75;
    QTest::newRow("155-percent") << 1.55;
}

void PDFEditControllerTest::keepsParagraphSpacingThroughQmlEditing()
{
    QFETCH(qreal, zoom);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("paragraph-spacing-qml-%1.pdf").arg(zoom));
    QVERIFY(createSourceSpacingPdf(sourcePath, 1.25, 2.5));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0),
        QStringLiteral("Alpha beta gamma delta"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QString first = QStringLiteral("Alpha beta gamma delta and more words to wrap the first paragraph.");
    const QString text = first + QStringLiteral("\n\nSecond paragraph keeps its own spacing.");
    controller.beginSelectedTextEdit();
    controller.updateSelectedTextPreview(text);
    controller.finishSelectedTextEdit();
    const QVariantList originalSpans = controller.selectedCharacterSpans();

    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(
        editOverlayQmlPath()));
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
    QTRY_COMPARE(textDocument->toPlainText(), text);
    QCOMPARE(textDocument->blockCount(), 3);
    const auto select = [&](int start, int end) {
        return QMetaObject::invokeMethod(editor, "select", Q_ARG(int, start), Q_ARG(int, end))
            && QMetaObject::invokeMethod(overlay.get(), "syncEditorSelection");
    };
    // A selection ending at the next block's start formats only the first block.
    QVERIFY(select(0, first.size() + 1));
    controller.setSelectedLineSpacing(1.5);
    controller.setSelectedParagraphSpaceBefore(6.0);
    controller.setSelectedParagraphSpaceAfter(10.0);
    auto firstFormat = textDocument->firstBlock().blockFormat();
    QCOMPARE(firstFormat.lineHeightType(), int(QTextBlockFormat::ProportionalHeight));
    QCOMPARE(firstFormat.lineHeight(), 150.0);
    QCOMPARE(firstFormat.topMargin(), 6.0);
    QCOMPARE(firstFormat.bottomMargin(), 10.0);
    QCOMPARE(textDocument->findBlockByNumber(1).blockFormat().lineHeight(), 100.0);
    QCOMPARE(textDocument->lastBlock().blockFormat().topMargin(), 0.0);

    const int lastStart = first.size() + 2;
    QVERIFY(select(lastStart + 1, lastStart + 1));
    QCOMPARE(controller.selectedLineSpacing(), 1.0);
    controller.setSelectedLineSpacing(2.0);
    controller.setSelectedParagraphSpaceBefore(4.0);
    controller.setSelectedParagraphSpaceAfter(8.0);
    QCOMPARE(textDocument->lastBlock().blockFormat().lineHeight(), 200.0);
    QCOMPARE(textDocument->lastBlock().blockFormat().topMargin(), 4.0);
    QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
    QTRY_COMPARE(controller.selectedParagraphSpaceAfter(), 0.0);
    QVERIFY(QMetaObject::invokeMethod(editor, "redo"));
    QTRY_COMPARE(controller.selectedParagraphSpaceAfter(), 8.0);
    QCOMPARE(controller.selectedCharacterSpans(), originalSpans);

    QVERIFY(select(first.size() + 1, first.size() + 1));
    controller.setSelectedLineSpacing(1.25);
    QCOMPARE(textDocument->findBlockByNumber(1).blockFormat().lineHeight(), 125.0);
    QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
    QTRY_COMPARE(controller.selectedLineSpacing(), 1.0);
    QVERIFY(QMetaObject::invokeMethod(editor, "redo"));
    QTRY_COMPARE(controller.selectedLineSpacing(), 1.25);
    QVERIFY(select(0, text.size()));
    QVERIFY(qIsNaN(controller.selectedLineSpacing()));
    QVERIFY(qIsNaN(controller.selectedParagraphSpaceBefore()));
    const QVariantList formats = controller.selectedParagraphFormats();
    controller.setSelectedLineSpacing(0.0);
    controller.setSelectedParagraphSpaceBefore(-1.0);
    controller.setSelectedParagraphSpaceAfter(145.0);
    QCOMPARE(controller.selectedParagraphFormats(), formats);
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
    QCOMPARE(controller.selectedText(), text);
    controller.undo();
    QCOMPARE(controller.selectedLineSpacing(), 1.0);
    QCOMPARE(controller.selectedParagraphSpaceBefore(), 0.0);
    controller.redo();
    QCOMPARE(controller.selectedParagraphFormats(), formats);
}

void PDFEditControllerTest::hidesDeletedTextFromInteraction()
{
    m_controller.discardDrafts();
    const QString objectId = m_regularCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QPointF hitPoint = m_regularCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .center();
    const auto containsObject = [&objectId](const QVariantList &candidates) {
        return std::any_of(
            candidates.cbegin(),
            candidates.cend(),
            [&objectId](const QVariant &candidate) {
                return candidate.toMap()
                    .value(QStringLiteral("objectId"))
                    .toString() == objectId;
            });
    };

    QCOMPARE(
        m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(containsObject(waitForCandidates(m_controller, 0)));
    QVERIFY(m_controller.selectCandidate(0, objectId));
    m_controller.commitSelectedText(QString());

    const QVariantList deletedDrafts = m_controller.draftsForPage(0);
    QCOMPARE(deletedDrafts.size(), 1);
    QVERIFY(deletedDrafts.constFirst().toMap()
        .value(QStringLiteral("text"))
        .toString()
        .isEmpty());
    QVERIFY(m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
    QVERIFY(!containsObject(waitForCandidates(m_controller, 0)));
    QVERIFY(!m_controller.selectCandidate(0, objectId));
    QVERIFY(m_controller.selectedObjectId().isEmpty());

    m_controller.undo();
    QCOMPARE(
        m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(containsObject(waitForCandidates(m_controller, 0)));
    QVERIFY(m_controller.selectCandidate(0, objectId));

    m_controller.redo();
    QVERIFY(m_controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
    QVERIFY(!containsObject(waitForCandidates(m_controller, 0)));
    QVERIFY(m_controller.selectedObjectId().isEmpty());
    m_controller.discardDrafts();
}

void PDFEditControllerTest::resizesDraftFrameWithUndoRedo()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF originalFrameRect = m_controller.selectedFrameRect();
    const qreal originalOriginX = m_controller.selectedOriginX();
    const qreal originalBaselineY = m_controller.selectedBaselineY();
    const qreal horizontalShift = qMin(0.01, originalFrameRect.width() / 4.0);
    const qreal verticalShift = qMin(0.01, originalFrameRect.height() / 4.0);
    const QRectF resizedFrameRect(
        originalFrameRect.x() + horizontalShift,
        originalFrameRect.y() + verticalShift,
        qMin(
            1.0 - originalFrameRect.x() - horizontalShift,
            originalFrameRect.width() + 0.08),
        qMin(
            1.0 - originalFrameRect.y() - verticalShift,
            originalFrameRect.height() + 0.04));
    QVERIFY(resizedFrameRect != originalFrameRect);

    m_controller.beginSelectedFrameResize();
    QVERIFY(m_controller.textEditActive());
    m_controller.updateSelectedFrameRect(resizedFrameRect);
    QCOMPARE(m_controller.selectedFrameRect(), resizedFrameRect);
    QCOMPARE(m_controller.selectedContentBounds(), resizedFrameRect);
    QCOMPARE(m_controller.selectedOriginX(), originalOriginX + horizontalShift);
    QCOMPARE(m_controller.selectedBaselineY(), originalBaselineY + verticalShift);
    m_controller.finishSelectedFrameResize();

    QVERIFY(!m_controller.textEditActive());
    QVERIFY(m_controller.canUndo());
    QCOMPARE(m_controller.draftsForPage(0).size(), 1);
    QCOMPARE(
        m_controller.draftsForPage(0).constFirst().toMap()
            .value(QStringLiteral("frameRect")).toRectF(),
        resizedFrameRect);

    m_controller.undo();
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QCOMPARE(m_controller.selectedFrameRect(), resizedFrameRect);
    QCOMPARE(m_controller.selectedContentBounds(), resizedFrameRect);

    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    m_controller.beginSelectedFrameResize();
    m_controller.updateSelectedFrameRect(resizedFrameRect);
    m_controller.cancelSelectedFrameResize();
    QVERIFY(!m_controller.textEditActive());
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    const QString editedText = QStringLiteral("Text and geometry in one edit");
    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(editedText);
    m_controller.beginSelectedFrameResize();
    m_controller.updateSelectedFrameRect(resizedFrameRect);
    m_controller.finishSelectedFrameResize();
    QVERIFY(m_controller.textEditActive());
    QCOMPARE(m_controller.selectedText(), editedText);
    QCOMPARE(m_controller.selectedFrameRect(), resizedFrameRect);

    m_controller.cancelSelectedTextEdit();
    QCOMPARE(
        m_controller.selectedText(),
        m_regularCandidate.value(QStringLiteral("text")).toString());
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
}

void PDFEditControllerTest::storesReflowedContentBoundsWithUndoRedo()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QRectF originalFrameRect = m_controller.selectedFrameRect();
    m_controller.beginSelectedTextEdit();
    const QRectF frameRect = m_controller.selectedFrameRect();
    const QRectF contentBounds(
        frameRect.x(),
        frameRect.y(),
        frameRect.width(),
        qMin(1.0 - frameRect.y(), frameRect.height() + 0.05));
    const QString multilineText = QStringLiteral("First line\nSecond line");

    m_controller.updateSelectedTextPreview(multilineText);
    m_controller.updateSelectedContentBounds(contentBounds);
    QCOMPARE(m_controller.selectedFrameRect(), frameRect);
    QCOMPARE(m_controller.selectedContentBounds(), contentBounds);
    m_controller.finishSelectedTextEdit();

    QCOMPARE(m_controller.draftsForPage(0).size(), 1);
    m_controller.undo();
    QCOMPARE(m_controller.selectedFrameRect(), originalFrameRect);
    QCOMPARE(m_controller.selectedContentBounds(), originalFrameRect);
    QVERIFY(m_controller.draftsForPage(0).isEmpty());

    m_controller.redo();
    QCOMPARE(m_controller.selectedFrameRect(), frameRect);
    QCOMPARE(m_controller.selectedContentBounds(), contentBounds);
    QCOMPARE(m_controller.selectedText(), multilineText);

    m_controller.discardDrafts();
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
    m_controller.setSelectedTextRange(0, m_controller.selectedText().size());
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

void PDFEditControllerTest::changesFontSizeWithUndoRedo()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const qreal originalSize = m_controller.selectedFontSize();
    const qreal originalRatio = m_controller.selectedFontSizeRatio();
    const qreal baseline = m_controller.selectedBaselineY();
    const QRectF sourceRect = m_controller.selectedSourceRect();
    QCOMPARE(originalSize, 24.0);

    m_controller.setSelectedFontSize(36.0);
    QVERIFY(!m_controller.textEditActive());
    QCOMPARE(m_controller.selectedFontSize(), 36.0);
    QCOMPARE(m_controller.selectedFontSizeRatio(), 36.0 / m_document.pageSize(0).height());
    QCOMPARE(m_controller.selectedDisplayFontSizeRatio(), m_controller.selectedFontSizeRatio());
    QCOMPARE(m_controller.selectedText(), QString::fromLatin1(kRegularText));
    QCOMPARE(m_controller.selectedSourceRect(), sourceRect);
    QCOMPARE(m_controller.selectedBaselineY(), baseline);
    const QVariantList spans = m_controller.selectedCharacterSpans();
    QCOMPARE(spans.size(), 1);
    QCOMPARE(spans.constFirst().toMap().value(QStringLiteral("fontSizeRatio")).toReal(),
             36.0 / m_document.pageSize(0).height());
    QVERIFY(m_controller.canUndo());

    m_controller.undo();
    QCOMPARE(m_controller.selectedFontSize(), originalSize);
    QCOMPARE(m_controller.selectedFontSizeRatio(), originalRatio);
    QVERIFY(!m_controller.hasDrafts());
    QVERIFY(!m_controller.canUndo());
    QVERIFY(m_controller.canRedo());
    m_controller.redo();
    QCOMPARE(m_controller.selectedFontSize(), 36.0);
    QVERIFY(m_controller.hasDrafts());

    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(m_controller.selectedFontSize(), originalSize);
    QVERIFY(!m_controller.hasDrafts());

    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(0, m_controller.selectedText().size());
    m_controller.setSelectedFontSize(48.0);
    QCOMPARE(m_controller.selectedFontSize(), 48.0);
    m_controller.cancelSelectedTextEdit();
    QCOMPARE(m_controller.selectedFontSize(), originalSize);
    QVERIFY(!m_controller.hasDrafts());
    QVERIFY(!m_controller.canUndo());

    m_controller.beginSelectedTextEdit();
    const QRectF expandedFrame = m_controller.selectedFrameRect();
    m_controller.setSelectedTextRange(0, m_controller.selectedText().size());
    m_controller.setSelectedFontSize(48.0);
    m_controller.finishSelectedTextEdit();
    QCOMPARE(m_controller.selectedFrameRect(), expandedFrame);
    m_controller.undo();
    QVERIFY(!m_controller.hasDrafts());
}

void PDFEditControllerTest::rejectsInvalidAndNoOpFontSizes()
{
    PDFEditController emptyController;
    QCOMPARE(emptyController.selectedFontSize(), 0.0);
    emptyController.setSelectedFontSize(36.0);
    QCOMPARE(emptyController.selectedFontSize(), 0.0);
    QVERIFY(!emptyController.hasDrafts());

    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const qreal originalSize = m_controller.selectedFontSize();
    const QRectF originalFrame = m_controller.selectedFrameRect();
    const QVariantList originalSpans = m_controller.selectedCharacterSpans();
    const QList<qreal> rejectedSizes = {
        -10.0, 0.0, 0.5, 12.5, 512.5, 513.0,
        std::numeric_limits<qreal>::infinity(),
        -std::numeric_limits<qreal>::infinity(),
        std::numeric_limits<qreal>::quiet_NaN(),
        originalSize
    };
    for (qreal size : rejectedSizes) {
        m_controller.setSelectedFontSize(size);
        QCOMPARE(m_controller.selectedFontSize(), originalSize);
        QCOMPARE(m_controller.selectedFrameRect(), originalFrame);
        QCOMPARE(m_controller.selectedCharacterSpans(), originalSpans);
        QVERIFY(!m_controller.textEditActive());
        QVERIFY(!m_controller.hasDrafts());
        QVERIFY(!m_controller.canUndo());
    }

    for (qreal size : { 1.0, 512.0 }) {
        m_controller.setSelectedFontSize(size);
        QCOMPARE(m_controller.selectedFontSize(), size);
        QVERIFY(m_controller.hasDrafts());
        m_controller.undo();
        QCOMPARE(m_controller.selectedFontSize(), originalSize);
        QVERIFY(!m_controller.hasDrafts());
    }
    m_controller.discardDrafts();
}

void PDFEditControllerTest::formatsFontSizeInSelectedRange()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const qreal pageHeight = m_document.pageSize(0).height();
    const qreal originalRatio = m_controller.selectedFontSizeRatio();
    const qreal originalDisplayRatio = m_controller.selectedDisplayFontSizeRatio();
    const qreal baseline = m_controller.selectedBaselineY();
    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(8, 3);
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedFontItalic(true);
    m_controller.setSelectedFontUnderline(true);
    const QColor rangeColor(QStringLiteral("#1565c0"));
    m_controller.setSelectedColor(rangeColor);
    QSignalSpy sizeAppliedSpy(&m_controller, &PDFEditController::selectionFontSizeApplied);
    m_controller.setSelectedFontSize(36.0);
    QCOMPARE(sizeAppliedSpy.size(), 1);
    QCOMPARE(sizeAppliedSpy.constFirst().at(0).toInt(), 8);
    QCOMPARE(sizeAppliedSpy.constFirst().at(1).toInt(), 3);
    QCOMPARE(sizeAppliedSpy.constFirst().at(2).toReal(), 36.0 / pageHeight);
    QCOMPARE(m_controller.selectedFontSize(), 36.0);
    QCOMPARE(m_controller.selectedBaselineY(), baseline);

    const QVariantList spans = m_controller.selectedCharacterSpans();
    QCOMPARE(spans.size(), 3);
    const QVariantMap selectedSpan = spans.at(1).toMap();
    QCOMPARE(selectedSpan.value(QStringLiteral("start")).toInt(), 8);
    QCOMPARE(selectedSpan.value(QStringLiteral("length")).toInt(), 3);
    QCOMPARE(selectedSpan.value(QStringLiteral("fontSizeRatio")).toReal(), 36.0 / pageHeight);
    QCOMPARE(selectedSpan.value(QStringLiteral("displayFontSizeRatio")).toReal(), 36.0 / pageHeight);
    QVERIFY(selectedSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(selectedSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(selectedSpan.value(QStringLiteral("fontUnderline")).toBool());
    QCOMPARE(selectedSpan.value(QStringLiteral("color")).value<QColor>(), rangeColor);
    for (int index : { 0, 2 }) {
        const QVariantMap unchangedSpan = spans.at(index).toMap();
        QCOMPARE(unchangedSpan.value(QStringLiteral("fontSizeRatio")).toReal(), originalRatio);
        QCOMPARE(unchangedSpan.value(QStringLiteral("displayFontSizeRatio")).toReal(), originalDisplayRatio);
        QVERIFY(!unchangedSpan.value(QStringLiteral("fontBold")).toBool());
    }

    m_controller.setSelectedTextRange(0, m_controller.selectedText().size());
    QCOMPARE(m_controller.selectedFontSize(), 0.0);
    m_controller.setSelectedTextRange(1, 2);
    QCOMPARE(m_controller.selectedFontSize(), 24.0);
    m_controller.setSelectedTextRange(9, 0);
    QCOMPARE(m_controller.selectedFontSize(), 36.0);
    m_controller.cancelSelectedTextEdit();
    QVERIFY(!m_controller.hasDrafts());
    QCOMPARE(m_controller.selectedFontSize(), 24.0);
}

void PDFEditControllerTest::appliesFontSizeToTypingAtCaret()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const QString originalText = m_controller.selectedText();
    const qreal originalRatio = m_controller.selectedFontSizeRatio();
    const qreal pageHeight = m_document.pageSize(0).height();
    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(8, 0);
    m_controller.setSelectedFontSize(36.0);
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedFontItalic(true);
    m_controller.setSelectedFontUnderline(true);
    QCOMPARE(m_controller.selectedFontSize(), 36.0);
    QVERIFY(!m_controller.hasDrafts());
    QVERIFY(!m_controller.canUndo());
    m_controller.finishSelectedTextEdit();
    QVERIFY(!m_controller.hasDrafts());
    QCOMPARE(m_controller.selectedFontSize(), 24.0);

    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(8, 0);
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedFontSize(36.0);
    m_controller.setSelectedFontItalic(true);
    m_controller.setSelectedFontUnderline(true);
    const QString inserted = QStringLiteral("new");
    const QString editedText = originalText.left(8) + inserted + originalText.mid(8);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(
        "import QtQuick\n"
        "TextEdit { textFormat: TextEdit.RichText; font.pixelSize: 36 }"), QUrl());
    std::unique_ptr<QObject> textEdit(component.create());
    QVERIFY2(textEdit, qPrintable(component.errorString()));
    QVERIFY(textEdit->setProperty("text", originalText));
    auto *quickDocument = textEdit->property("textDocument").value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *textDocument = quickDocument->textDocument();
    QVERIFY(textDocument);
    m_controller.initializeEditorFontSizes(quickDocument);
    QVERIFY(QMetaObject::invokeMethod(textEdit.get(), "insert",
                                     Q_ARG(int, 8), Q_ARG(QString, inserted)));
    m_controller.updateSelectedTextPreview(editedText, 11);
    m_controller.applyTypingFormatToEditorDocument(quickDocument, 8, 3, pageHeight * 1.5);
    QTextCursor typedCursor(textDocument);
    typedCursor.setPosition(8);
    typedCursor.setPosition(11, QTextCursor::KeepAnchor);
    QCOMPARE(typedCursor.charFormat().font().pixelSize(), 54);
    QCOMPARE(typedCursor.charFormat().fontWeight(), QFont::Bold);
    QVERIFY(typedCursor.charFormat().fontItalic());
    QVERIFY(typedCursor.charFormat().fontUnderline());
    textDocument->undo();
    QCOMPARE(textDocument->toPlainText(), originalText);
    m_controller.setSelectedTextRange(11, 0);
    QCOMPARE(m_controller.selectedFontSize(), 36.0);
    const QVariantList spans = m_controller.selectedCharacterSpans();
    QCOMPARE(spans.size(), 3);
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("fontSizeRatio")).toReal(), originalRatio);
    QCOMPARE(spans.at(2).toMap().value(QStringLiteral("fontSizeRatio")).toReal(), originalRatio);
    const QVariantMap insertionSpan = spans.at(1).toMap();
    QCOMPARE(insertionSpan.value(QStringLiteral("start")).toInt(), 8);
    QCOMPARE(insertionSpan.value(QStringLiteral("length")).toInt(), 3);
    QCOMPARE(insertionSpan.value(QStringLiteral("fontSizeRatio")).toReal(), 36.0 / pageHeight);
    QVERIFY(insertionSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(insertionSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(insertionSpan.value(QStringLiteral("fontUnderline")).toBool());
    m_controller.setSelectedTextRange(2, 0);
    QCOMPARE(m_controller.selectedFontSize(), 24.0);
    QVERIFY(!m_controller.selectedFontBold());
    m_controller.finishSelectedTextEdit();
    m_controller.undo();
    QVERIFY(!m_controller.hasDrafts());
    QCOMPARE(m_controller.selectedText(), originalText);
    m_controller.redo();
    QCOMPARE(m_controller.selectedText(), editedText);
    QCOMPARE(m_controller.selectedCharacterSpans(), spans);
    m_controller.discardDrafts();
}

void PDFEditControllerTest::appliesColorToTypingAtCaret_data()
{
    QTest::addColumn<bool>("addedText");
    QTest::newRow("source-text") << false;
    QTest::newRow("added-text") << true;
}

void PDFEditControllerTest::appliesColorToTypingAtCaret()
{
    QFETCH(bool, addedText);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("caret-color-%1-source.pdf").arg(addedText));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("caret-color-%1-output.pdf").arg(addedText));
    QVERIFY(createFontStylePdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0), QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    if (addedText) {
        QVERIFY(!controller.addText(0, 0.12, 0.55).isEmpty());
    } else {
        QVERIFY(controller.selectCandidate(
            0, candidate.value(QStringLiteral("objectId")).toString()));
        controller.beginSelectedTextEdit();
    }
    const QColor originalColor = controller.selectedColor();
    const QVariantList initialSpans = controller.selectedCharacterSpans();
    const QColor typingColor(QStringLiteral("#1565c0"));
    controller.setSelectedTextRange(addedText ? 0 : 8, 0);
    controller.setSelectedColor(typingColor);
    QCOMPARE(controller.selectedColor(), typingColor);
    QCOMPARE(controller.selectedCharacterSpans(), initialSpans);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    if (addedText) {
        QVERIFY(!controller.addText(0, 0.12, 0.55).isEmpty());
        controller.updateSelectedTextPreview(QStringLiteral("Original"), 8);
    } else {
        QVERIFY(controller.selectCandidate(
            0, candidate.value(QStringLiteral("objectId")).toString()));
        controller.beginSelectedTextEdit();
    }
    const QString originalText = controller.selectedText();
    const QVariantList beforeSpans = controller.selectedCharacterSpans();
    const QVariantList beforeDrafts = controller.draftsForPage(0);
    constexpr int insertionPosition = 3;
    const QString insertedText = QStringLiteral("new");
    controller.setSelectedTextRange(insertionPosition, 0);
    controller.setSelectedColor(typingColor);
    controller.setSelectedColor(typingColor);
    QCOMPARE(controller.selectedCharacterSpans(), beforeSpans);
    QCOMPARE(controller.draftsForPage(0), beforeDrafts);
    QVERIFY(!controller.canUndo());
    const QString coloredText = originalText.left(insertionPosition)
        + insertedText + originalText.mid(insertionPosition);
    controller.updateSelectedTextPreview(coloredText, insertionPosition + insertedText.size());
    const QVariantList coloredSpans = controller.selectedCharacterSpans();
    QCOMPARE(coloredSpans.size(), 3);
    QCOMPARE(coloredSpans.at(0).toMap().value(QStringLiteral("color")).value<QColor>(), originalColor);
    QCOMPARE(coloredSpans.at(1).toMap().value(QStringLiteral("color")).value<QColor>(), typingColor);
    QCOMPARE(coloredSpans.at(1).toMap().value(QStringLiteral("start")).toInt(), insertionPosition);
    QCOMPARE(coloredSpans.at(1).toMap().value(QStringLiteral("length")).toInt(), insertedText.size());
    QCOMPARE(coloredSpans.at(2).toMap().value(QStringLiteral("color")).value<QColor>(), originalColor);

    // Moving away from the typing position inherits the destination's color.
    controller.setSelectedTextRange(1, 0);
    QCOMPARE(controller.selectedColor(), originalColor);
    const QString finalText = coloredText.left(1) + QLatin1Char('P') + coloredText.mid(1);
    controller.updateSelectedTextPreview(finalText, 2);
    const QVariantList finalSpans = controller.selectedCharacterSpans();
    QCOMPARE(finalSpans.at(0).toMap().value(QStringLiteral("color")).value<QColor>(), originalColor);
    const QString objectId = controller.selectedObjectId();
    controller.finishSelectedTextEdit();
    QVERIFY(controller.canUndo());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedText(), finalText);
    QCOMPARE(controller.selectedCharacterSpans(), finalSpans);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    const auto savedCharacters = textCharacterRangeInfo(outputPath, finalText);
    QCOMPARE(savedCharacters.size(), finalText.size());
    for (qsizetype index = 0; index < savedCharacters.size(); ++index) {
        if (finalText.at(index).isSpace())
            continue;
        const bool newlyColored = index >= insertionPosition + 1
            && index < insertionPosition + 1 + insertedText.size();
        QCOMPARE(savedCharacters.at(index).color, newlyColored ? typingColor : originalColor);
    }
}

void PDFEditControllerTest::retainsTypingColorWhenChangingFontFamily()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0, m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const QString originalText = m_controller.selectedText();
    const QColor originalColor = m_controller.selectedColor();
    const QString originalFamily = m_controller.selectedFontFamily();
    QString replacementFamily;
    for (const QString &family : m_controller.availableFontFamilies()) {
        if (family.compare(m_controller.selectedFontFamily(), Qt::CaseInsensitive) != 0
            && ApplicationFonts::isFontFaceSelectable(family, true, true)
            && ApplicationFonts::fontFaceSupportsText(family, true, true, originalText)) {
            replacementFamily = family;
            break;
        }
    }
    QVERIFY(!replacementFamily.isEmpty());
    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(8, 0);
    const QColor typingColor(QStringLiteral("#c62828"));
    m_controller.setSelectedColor(typingColor);
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedFontSize(30.0);
    m_controller.setSelectedFontItalic(true);
    m_controller.setSelectedFontUnderline(true);
    QVERIFY(!m_controller.hasDrafts());
    m_controller.setSelectedFontFamily(replacementFamily);
    QVERIFY(!m_controller.hasDrafts());
    QCOMPARE(m_controller.selectedText(), originalText);
    QCOMPARE(m_controller.selectedColor(), typingColor);
    QCOMPARE(m_controller.selectedFontSize(), 30.0);
    const QString editedText = originalText.left(8) + QLatin1Char('X') + originalText.mid(8);
    m_controller.updateSelectedTextPreview(editedText, 9);
    const QVariantList spans = m_controller.selectedCharacterSpans();
    QCOMPARE(spans.size(), 3);
    const QVariantMap inserted = spans.at(1).toMap();
    QCOMPARE(inserted.value(QStringLiteral("color")).value<QColor>(), typingColor);
    QCOMPARE(inserted.value(QStringLiteral("fontFamily")).toString(), replacementFamily);
    QCOMPARE(inserted.value(QStringLiteral("fontSizeRatio")).toReal(),
             30.0 / m_document.pageSize(0).height());
    QVERIFY(inserted.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(inserted.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(inserted.value(QStringLiteral("fontUnderline")).toBool());
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("color")).value<QColor>(), originalColor);
    QCOMPARE(spans.at(2).toMap().value(QStringLiteral("color")).value<QColor>(), originalColor);
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("fontFamily")).toString(), originalFamily);
    QCOMPARE(spans.at(2).toMap().value(QStringLiteral("fontFamily")).toString(), originalFamily);
    m_controller.setSelectedTextRange(1, 0);
    QCOMPARE(m_controller.selectedFontFamily(), originalFamily);
    m_controller.cancelSelectedTextEdit();
    QVERIFY(!m_controller.hasDrafts());
}

void PDFEditControllerTest::joinsNativeFontSizeTypingUndo_data()
{
    QTest::addColumn<bool>("inputMethod");
    QTest::addColumn<int>("caretPosition");
    QTest::addColumn<bool>("fromSelection");
    QTest::newRow("keyboard-start") << false << 0 << false;
    QTest::newRow("keyboard-middle") << false << 8 << false;
    QTest::newRow("keyboard-end") << false << -1 << false;
    QTest::newRow("vietnamese-ime-middle") << true << 8 << false;
    QTest::newRow("keyboard-open-editor") << false << -1 << true;
    QTest::newRow("vietnamese-ime-open-editor") << true << -1 << true;
}

void PDFEditControllerTest::joinsNativeFontSizeTypingUndo()
{
    QFETCH(bool, inputMethod);
    QFETCH(int, caretPosition);
    QFETCH(bool, fromSelection);
    const QString insertedText = inputMethod ? QStringLiteral("ế") : QStringLiteral("x");
    m_controller.discardDrafts();
    m_controller.setTool(PDFEditController::SelectTool);
    QVERIFY(m_controller.selectCandidate(
        0, m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const QString originalText = m_controller.selectedText();
    const QColor originalColor = m_controller.selectedColor();
    const QString originalFamily = m_controller.selectedFontFamily();
    if (caretPosition < 0)
        caretPosition = originalText.size();
    QString replacementFamily;
    for (const QString &family : m_controller.availableFontFamilies()) {
        if (family.compare(originalFamily, Qt::CaseInsensitive) != 0
            && ApplicationFonts::isFontFaceSelectable(family, false, false)
            && ApplicationFonts::fontFaceSupportsText(family, false, false, originalText + insertedText)) {
            replacementFamily = family;
            break;
        }
    }
    QVERIFY(!replacementFamily.isEmpty());
    const QString editedText = originalText.left(caretPosition) + insertedText
        + originalText.mid(caretPosition);
    const qreal displayedHeight = m_document.pageSize(0).height() * 1.5;
    const QString themePath = QFINDTESTDATA("../src/Components/AppTheme.qml");
    const QString overlayPath = editOverlayQmlPath();
    QVERIFY(registerQmlTestTypes(themePath));
    QQuickWindow window;
    window.resize(900, 600);
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(overlayPath));
    const QVariantMap properties{
        {QStringLiteral("page"), 0},
        {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), false},
        {QStringLiteral("draftTextureReady"), false},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&m_controller)},
        {QStringLiteral("pagePointScale"), 1.5},
        {QStringLiteral("width"), 900.0},
        {QStringLiteral("height"), displayedHeight}};
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties(properties));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    qobject_cast<QQuickItem *>(overlay.get())->setParentItem(window.contentItem());
    window.show();
    if (fromSelection) {
        QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginSelectedEditingWithStyle",
            Q_ARG(QVariant, QStringLiteral("fontFamily:") + replacementFamily)));
    } else {
        QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginSelectedEditing"));
    }
    QVERIFY(m_controller.textEditActive());
    QVERIFY(!m_controller.hasDrafts());
    QObject *textEdit = nullptr;
    for (QObject *child : overlay->findChildren<QObject *>()) {
        if (child->metaObject()->indexOfProperty("textDocument") >= 0) {
            textEdit = child;
            break;
        }
    }
    QVERIFY(textEdit);
    auto *item = qobject_cast<QQuickItem *>(textEdit);
    QVERIFY(item);
    auto *quickDocument = textEdit->property("textDocument").value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *document = quickDocument->textDocument();
    QTRY_VERIFY(window.isExposed());
    QVERIFY(textEdit->setProperty("cursorPosition", caretPosition));
    m_controller.setSelectedTextRange(caretPosition, 0);
    if (!fromSelection)
        m_controller.setSelectedFontFamily(replacementFamily);
    QCOMPARE(m_controller.selectedFontFamily(), replacementFamily);
    QVERIFY(!m_controller.hasDrafts());
    QCOMPARE(document->toPlainText(), originalText);
    const QColor typingColor(QStringLiteral("#1565c0"));
    m_controller.setSelectedColor(typingColor);
    m_controller.setSelectedFontSize(32);
    m_controller.setSelectedFontUnderline(true);
    item->forceActiveFocus();
    if (inputMethod) {
        QInputMethodEvent event;
        event.setCommitString(insertedText);
        QCoreApplication::sendEvent(item, &event);
    } else {
        QTest::keyClick(&window, Qt::Key_X);
    }
    QTRY_COMPARE(m_controller.selectedText(), editedText);
    QTextCursor cursor(document);
    cursor.setPosition(caretPosition);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QCOMPARE(cursor.charFormat().font().pixelSize() * item->scale(), 48.0);
    QVERIFY(cursor.charFormat().fontUnderline());
    QCOMPARE(cursor.charFormat().foreground().color(), typingColor);
    const QVariantList typedSpans = m_controller.selectedCharacterSpans();
    for (const QVariant &value : typedSpans) {
        const QVariantMap span = value.toMap();
        const bool inserted = span.value(QStringLiteral("start")).toInt() == caretPosition;
        QCOMPARE(span.value(QStringLiteral("fontFamily")).toString(),
                 inserted ? replacementFamily : originalFamily);
        QCOMPARE(span.value(QStringLiteral("color")).value<QColor>(),
                 inserted ? typingColor : originalColor);
    }
    QCOMPARE(PDFFontAliases::semanticFamily(cursor.charFormat().fontFamilies().toStringList().value(0)),
             replacementFamily);
    QVERIFY(QMetaObject::invokeMethod(textEdit, "undo"));
    QTRY_COMPARE(m_controller.selectedText(), originalText);
    QCOMPARE(document->toPlainText(), originalText);
    QVERIFY(!document->isUndoAvailable());
    for (const QVariant &span : m_controller.selectedCharacterSpans())
        QCOMPARE(span.toMap().value(QStringLiteral("color")).value<QColor>(), originalColor);
    QVERIFY(QMetaObject::invokeMethod(textEdit, "redo"));
    QTRY_COMPARE(m_controller.selectedText(), editedText);
    QCOMPARE(textEdit->property("cursorPosition").toInt(),
             caretPosition + insertedText.size());
    QCOMPARE(m_controller.selectedCharacterSpans(), typedSpans);
    cursor.setPosition(caretPosition);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QCOMPARE(cursor.charFormat().foreground().color(), typingColor);
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "cancelEditing"));
}

void PDFEditControllerTest::preservesEditorUndoWhenChangingFontSize()
{
    m_controller.discardDrafts();
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const QString originalText = m_controller.selectedText();
    const qreal originalRatio = m_controller.selectedFontSizeRatio();
    const qreal originalDisplayRatio = m_controller.selectedDisplayFontSizeRatio();
    const qreal baseline = m_controller.selectedBaselineY();
    const qreal displayedHeight = m_document.pageSize(0).height() * 1.5;
    m_controller.beginSelectedTextEdit();

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(
        "import QtQuick\n"
        "TextEdit { textFormat: TextEdit.RichText; font.pixelSize: 36 }"), QUrl());
    std::unique_ptr<QObject> textEdit(component.create());
    QVERIFY2(textEdit, qPrintable(component.errorString()));
    QVERIFY(textEdit->setProperty("text", originalText));
    auto *quickDocument = textEdit->property("textDocument").value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *textDocument = quickDocument->textDocument();
    QVERIFY(textDocument);
    m_controller.initializeEditorFontSizes(quickDocument, displayedHeight);
    QVERIFY(!textDocument->isUndoAvailable());

    QVERIFY(QMetaObject::invokeMethod(textEdit.get(), "insert",
                                     Q_ARG(int, originalText.size()),
                                     Q_ARG(QString, QStringLiteral("X"))));
    const QString editedText = originalText + QLatin1Char('X');
    QCOMPARE(textDocument->toPlainText(), editedText);
    m_controller.updateSelectedTextPreview(editedText, editedText.size());
    m_controller.setSelectedTextRange(1, 2);
    QSignalSpy sizeAppliedSpy(&m_controller, &PDFEditController::selectionFontSizeApplied);
    m_controller.setSelectedFontSize(36.0);
    QCOMPARE(sizeAppliedSpy.size(), 1);
    const QList<QVariant> sizeArguments = sizeAppliedSpy.constFirst();
    QVERIFY(m_controller.applyFontSizeToEditorDocument(
        quickDocument, sizeArguments.at(0).toInt(), sizeArguments.at(1).toInt(),
        sizeArguments.at(2).toReal(), displayedHeight));
    QTextCursor formattedCursor(textDocument);
    formattedCursor.setPosition(1);
    formattedCursor.setPosition(3, QTextCursor::KeepAnchor);
    QCOMPARE(formattedCursor.charFormat().font().pixelSize(), 54);
    formattedCursor.setPosition(0);
    formattedCursor.setPosition(1, QTextCursor::KeepAnchor);
    QCOMPARE(formattedCursor.charFormat().font().pixelSize(), 36);
    QCOMPARE(m_controller.selectedFontSize(), 36.0);
    QCOMPARE(m_controller.selectedBaselineY(), baseline);

    textDocument->undo();
    QCOMPARE(textDocument->toPlainText(), editedText);
    QVERIFY(m_controller.syncEditorFontStyles(quickDocument));
    QCOMPARE(m_controller.selectedFontSize(), 24.0);
    QCOMPARE(m_controller.selectedFontSizeRatio(), originalRatio);
    QCOMPARE(m_controller.selectedDisplayFontSizeRatio(), originalDisplayRatio);
    QCOMPARE(m_controller.selectedCharacterSpans().size(), 1);
    textDocument->redo();
    QVERIFY(m_controller.syncEditorFontStyles(quickDocument));
    QCOMPARE(m_controller.selectedFontSize(), 36.0);
    textDocument->undo();
    QVERIFY(m_controller.syncEditorFontStyles(quickDocument));
    textDocument->undo();
    QCOMPARE(textDocument->toPlainText(), originalText);
    m_controller.updateSelectedTextPreview(originalText, originalText.size());
    m_controller.syncEditorFontStyles(quickDocument);
    m_controller.finishSelectedTextEdit();
    QVERIFY(!m_controller.hasDrafts());
    QVERIFY(!m_controller.canUndo());
    QCOMPARE(m_controller.selectedFontSize(), 24.0);
}

void PDFEditControllerTest::savesFontSizeRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("font-size-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("font-size-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.setSelectedFontSize(18.0);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    candidate = candidateForText(waitForCandidates(controller, 0), QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QCOMPARE(controller.selectedFontSize(), 18.0);
    controller.beginSelectedTextEdit();
    controller.setSelectedTextRange(8, 3);
    controller.setSelectedFontSize(30.0);
    controller.finishSelectedTextEdit();
    QVERIFY(controller.save());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    const QVariantList reopenedCandidates = waitForCandidates(controller, 0);
    candidate = candidateForText(reopenedCandidates, QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    controller.setSelectedTextRange(8, 3);
    QCOMPARE(controller.selectedFontSize(), 30.0);
    controller.setSelectedTextRange(0, 7);
    QCOMPARE(controller.selectedFontSize(), 18.0);
    // The space following the resized word is encoded with that word's final
    // glyph, but must retain its own semantic format in the editable paragraph.
    controller.setSelectedTextRange(11, 1);
    QCOMPARE(controller.selectedFontSize(), 18.0);
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());

    const QString savedRange = QString::fromLatin1(kRegularText);
    const auto characters = textCharacterRangeInfo(outputPath, savedRange);
    QCOMPARE(characters.size(), savedRange.size());
    for (qsizetype index = 0; index < characters.size(); ++index) {
        if (savedRange.at(index).isSpace())
            continue;
        QCOMPARE(characters[index].pointSize, index >= 8 && index < 11 ? 30.0F : 18.0F);
    }

    // A second replacement must use the restored semantic space widths, not
    // the size of the visible glyph with which each space was encoded.
    controller.setSelectedColor(Qt::blue);
    QVERIFY(controller.save());
    const auto recoloredCharacters = textCharacterRangeInfo(outputPath, savedRange);
    QCOMPARE(recoloredCharacters.size(), characters.size());
    for (qsizetype index = 0; index < characters.size(); ++index) {
        if (savedRange.at(index).isSpace())
            continue;
        QCOMPARE(recoloredCharacters[index].pointSize, characters[index].pointSize);
        QCOMPARE(recoloredCharacters[index].color, QColor(Qt::blue));
        QVERIFY2(qAbs(recoloredCharacters[index].matrix.e - characters[index].matrix.e) < 0.15F
                     && qAbs(recoloredCharacters[index].matrix.f - characters[index].matrix.f) < 0.15F,
                 qPrintable(QStringLiteral("Visible character %1 moved after recoloring").arg(index)));
    }
    candidate = candidateForText(waitForCandidates(controller, 0), savedRange);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedTextEdit();
    for (qsizetype index = 0; index < savedRange.size(); ++index) {
        if (!savedRange.at(index).isSpace())
            continue;
        controller.setSelectedTextRange(index, 1);
        QCOMPARE(controller.selectedFontSize(), 18.0);
    }
    controller.finishSelectedTextEdit();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(normalizedPageText(sourcePath).contains(QString::fromLatin1(kRegularText)));
}

void PDFEditControllerTest::rejectsDamagedParagraphWhitespaceFormats_data()
{
    QTest::addColumn<QString>("mutation");
    QTest::newRow("visible-character") << QStringLiteral("visible-character");
    QTest::newRow("overlap") << QStringLiteral("overlap");
    QTest::newRow("invalid-advance") << QStringLiteral("invalid-advance");
}

void PDFEditControllerTest::rejectsDamagedParagraphWhitespaceFormats()
{
    QFETCH(QString, mutation);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("whitespace-metadata-%1-source.pdf").arg(mutation));
    const QString savedPath = m_temporaryDirectory.filePath(
        QStringLiteral("whitespace-metadata-%1-valid.pdf").arg(mutation));
    const QString damagedPath = m_temporaryDirectory.filePath(
        QStringLiteral("whitespace-metadata-%1-damaged.pdf").arg(mutation));
    QVERIFY(createFontStylePdf(sourcePath));
    const QString text = QString::fromLatin1(kRegularText);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), text);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    controller.setSelectedFontSize(18.0);
    controller.beginSelectedTextEdit();
    controller.setSelectedTextRange(8, 3);
    controller.setSelectedFontSize(30.0);
    controller.finishSelectedTextEdit();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(savedPath)));
    candidate = candidateForText(waitForCandidates(controller, 0), text);
    QVERIFY(candidate.value(QStringLiteral("objectId")).toString()
                .startsWith(QLatin1String("paragraph-layout:")));

    {
        PDFiumProcessLock lock;
        FPDF_DOCUMENT pdf = FPDF_LoadDocument(QFile::encodeName(savedPath).constData(), nullptr);
        QVERIFY(pdf);
        const auto closePdf = qScopeGuard([&] { FPDF_CloseDocument(pdf); });
        FPDF_PAGE page = FPDF_LoadPage(pdf, 0);
        QVERIFY(page);
        const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
        bool changed = false;
        for (int index = 0; index < FPDFPage_CountObjects(page) && !changed; ++index) {
            const auto object = FPDFPage_GetObject(page, index);
            for (int markIndex = 0; markIndex < FPDFPageObj_CountMarks(object); ++markIndex) {
                const auto mark = FPDFPageObj_GetMark(object, markIndex);
                FPDF_WCHAR name[64]{};
                unsigned long nameSize = 0;
                if (!FPDFPageObjMark_GetName(mark, name, sizeof(name), &nameSize)
                    || nameSize < 2 || nameSize > sizeof(name) || nameSize % 2 != 0
                    || QString::fromUtf16(reinterpret_cast<const char16_t *>(name), nameSize / 2 - 1)
                        != QLatin1String("TrePDFParagraphLayout")) {
                    continue;
                }
                int partIndex = -1;
                if (!FPDFPageObjMark_GetParamIntValue(mark, "Index", &partIndex) || partIndex != 0)
                    continue;
                unsigned long size = 0;
                QVERIFY(FPDFPageObjMark_GetParamBlobValue(mark, "Model", nullptr, 0, &size));
                QVERIFY(size > 0 && size <= 1024 * 1024);
                QByteArray bytes(qsizetype(size), Qt::Uninitialized);
                unsigned long actual = 0;
                QVERIFY(FPDFPageObjMark_GetParamBlobValue(mark, "Model",
                    reinterpret_cast<unsigned char *>(bytes.data()), size, &actual));
                QCOMPARE(actual, size);
                const QJsonDocument modelDocument = QJsonDocument::fromJson(bytes);
                QVERIFY(modelDocument.isObject());
                QJsonObject model = modelDocument.object();
                QJsonArray formats = model.value(QStringLiteral("whitespaceFormats")).toArray();
                QVERIFY(!formats.isEmpty());
                QJsonObject format = formats.first().toObject();
                if (mutation == QLatin1String("visible-character")) {
                    format.insert(QStringLiteral("start"), 0);
                    format.insert(QStringLiteral("length"), 1);
                    formats[0] = format;
                } else if (mutation == QLatin1String("overlap")) {
                    formats.insert(1, format);
                } else {
                    QCOMPARE(mutation, QStringLiteral("invalid-advance"));
                    format.insert(QStringLiteral("sourceAdvanceRatio"), 513.0 / FPDF_GetPageHeightF(page));
                    formats[0] = format;
                }
                model.insert(QStringLiteral("whitespaceFormats"), formats);
                const QByteArray damagedBytes = QJsonDocument(model).toJson(QJsonDocument::Compact);
                QVERIFY(FPDFPageObjMark_SetBlobParam(pdf, object, mark, "Model",
                    reinterpret_cast<const unsigned char *>(damagedBytes.constData()),
                    static_cast<unsigned long>(damagedBytes.size())));
                changed = true;
                break;
            }
        }
        QVERIFY(changed);
        QVERIFY(FPDFPage_GenerateContent(page));
        PdfFileWriter writer(damagedPath);
        QVERIFY(writer.open());
        QVERIFY(FPDF_SaveAsCopy(pdf, &writer, FPDF_NO_INCREMENTAL));
    }
    QVERIFY(document.open(QUrl::fromLocalFile(damagedPath)));
    const QVariantList candidates = waitForCandidates(controller, 0);
    QVERIFY(!candidates.isEmpty());
    for (const QVariant &value : candidates) {
        QVERIFY(!value.toMap().value(QStringLiteral("objectId")).toString()
                     .startsWith(QLatin1String("paragraph-layout:")));
    }
    QVERIFY(normalizedPageText(damagedPath).contains(text));
}

void PDFEditControllerTest::appliesFontStylesToTypingAtCaret()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));
    const QString originalText = QString::fromLatin1(kRegularText);
    const QColor originalColor = m_controller.selectedColor();
    const qsizetype caretPosition = 8;
    const QString styledInsertion = QStringLiteral("new");
    const QString regularInsertion = QStringLiteral(" plain");
    QSignalSpy typingFormatAppliedSpy(
        &m_controller,
        &PDFEditController::typingFormatApplied);
    QVERIFY(!m_controller.selectedFontBold());
    QVERIFY(!m_controller.selectedFontItalic());
    QVERIFY(!m_controller.selectedFontUnderline());

    m_controller.beginSelectedTextEdit();
    m_controller.setSelectedTextRange(caretPosition, 0);
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedFontItalic(true);
    m_controller.setSelectedFontUnderline(true);
    QVERIFY(m_controller.textEditActive());
    QVERIFY(m_controller.selectedFontBold());
    QVERIFY(m_controller.selectedFontItalic());
    QVERIFY(m_controller.selectedFontUnderline());
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
    QVERIFY(!m_controller.canUndo());

    const QColor typingColor(QStringLiteral("#1565c0"));
    m_controller.setSelectedColor(typingColor);
    QCOMPARE(m_controller.selectedColor(), typingColor);

    const QString styledText = originalText.left(caretPosition)
        + styledInsertion
        + originalText.mid(caretPosition);
    const qsizetype styledCaret = caretPosition + styledInsertion.size();
    m_controller.updateSelectedTextPreview(styledText, styledCaret);
    QCOMPARE(typingFormatAppliedSpy.size(), 1);
    m_controller.setSelectedTextRange(styledCaret, 0);

    QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap styledDraft = drafts.constFirst().toMap();
    QCOMPARE(styledDraft.value(QStringLiteral("text")).toString(), styledText);
    const QVariantList styledSpans = styledDraft.value(
        QStringLiteral("characterSpans")).toList();
    QCOMPARE(styledSpans.size(), 3);
    const QVariantMap styledSpan = styledSpans.at(1).toMap();
    QCOMPARE(styledSpan.value(QStringLiteral("start")).toLongLong(), caretPosition);
    QCOMPARE(
        styledSpan.value(QStringLiteral("length")).toLongLong(),
        styledInsertion.size());
    QVERIFY(styledSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(styledSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(styledSpan.value(QStringLiteral("fontUnderline")).toBool());
    QCOMPARE(
        styledSpan.value(QStringLiteral("color")).value<QColor>(),
        typingColor);
    QVERIFY(m_controller.selectedFontBold());
    QVERIFY(m_controller.selectedFontItalic());
    QVERIFY(m_controller.selectedFontUnderline());

    m_controller.setSelectedColor(originalColor);
    m_controller.setSelectedFontBold(false);
    m_controller.setSelectedFontItalic(false);
    m_controller.setSelectedFontUnderline(false);
    QVERIFY(!m_controller.selectedFontBold());
    QVERIFY(!m_controller.selectedFontItalic());
    QVERIFY(!m_controller.selectedFontUnderline());

    const QString finalText = styledText.left(styledCaret)
        + regularInsertion
        + styledText.mid(styledCaret);
    m_controller.updateSelectedTextPreview(
        finalText,
        styledCaret + regularInsertion.size());
    QCOMPARE(typingFormatAppliedSpy.size(), 2);
    m_controller.setSelectedTextRange(
        styledCaret + regularInsertion.size(),
        0);

    drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap finalDraft = drafts.constFirst().toMap();
    QCOMPARE(finalDraft.value(QStringLiteral("text")).toString(), finalText);
    const QVariantList finalSpans = finalDraft.value(
        QStringLiteral("characterSpans")).toList();
    QCOMPARE(finalSpans.size(), 3);
    const QVariantMap prefixSpan = finalSpans.at(0).toMap();
    const QVariantMap finalStyledSpan = finalSpans.at(1).toMap();
    const QVariantMap suffixSpan = finalSpans.at(2).toMap();
    QCOMPARE(prefixSpan.value(QStringLiteral("length")).toLongLong(), caretPosition);
    QCOMPARE(prefixSpan.value(QStringLiteral("color")).value<QColor>(), originalColor);
    QCOMPARE(suffixSpan.value(QStringLiteral("color")).value<QColor>(), originalColor);
    QVERIFY(!prefixSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!prefixSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(!prefixSpan.value(QStringLiteral("fontUnderline")).toBool());
    QVERIFY(finalStyledSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(finalStyledSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(finalStyledSpan.value(QStringLiteral("fontUnderline")).toBool());
    QCOMPARE(
        suffixSpan.value(QStringLiteral("start")).toLongLong(),
        styledCaret);
    QVERIFY(!suffixSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!suffixSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(!suffixSpan.value(QStringLiteral("fontUnderline")).toBool());

    m_controller.finishSelectedTextEdit();
    QVERIFY(m_controller.canUndo());
    m_controller.undo();
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
    m_controller.redo();
    QCOMPARE(
        m_controller.draftsForPage(0).constFirst().toMap()
            .value(QStringLiteral("characterSpans")).toList().size(),
        3);

    m_controller.discardDrafts();
}

void PDFEditControllerTest::rendersEmbeddedFontStylesBeforeSave_data()
{
    QTest::addColumn<bool>("bold");
    QTest::addColumn<bool>("italic");
    QTest::addColumn<bool>("formatSelection");
    QTest::addColumn<bool>("inputMethod");
    QTest::newRow("bold-keyboard") << true << false << false << false;
    QTest::newRow("italic-keyboard") << false << true << false << false;
    QTest::newRow("bold-italic-ime") << true << true << false << true;
    QTest::newRow("bold-selection") << true << false << true << false;
    QTest::newRow("italic-selection") << false << true << true << false;
}

void PDFEditControllerTest::rendersEmbeddedFontStylesBeforeSave()
{
    QFETCH(bool, bold);
    QFETCH(bool, italic);
    QFETCH(bool, formatSelection);
    QFETCH(bool, inputMethod);
    const QString probePath = qEnvironmentVariable("TREPDF_TEST_STYLE_PDF");
    const QString originalApplicationName = QCoreApplication::applicationName();
    const bool originalTestMode = QStandardPaths::isTestModeEnabled();
    QString probeFontStoreRoot;
    const auto restoreFontStore = qScopeGuard([&]() {
        if (!probeFontStoreRoot.isEmpty())
            QDir(probeFontStoreRoot).removeRecursively();
        if (!probePath.isEmpty()) {
            QCoreApplication::setApplicationName(originalApplicationName);
            QStandardPaths::setTestModeEnabled(originalTestMode);
        }
    });
    if (!probePath.isEmpty()) {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("TrePDF-style-probe-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString root = QFileInfo(ApplicationFonts::downloadedFontDirectory()).absolutePath();
        QCOMPARE(QFileInfo(root).fileName(), QCoreApplication::applicationName());
        QVERIFY(!QFileInfo::exists(root));
        probeFontStoreRoot = root;
        const char *const paths[] = {
            "fixtures/fonts/tex-gyre/texgyretermes-regular.otf",
            "fixtures/fonts/tex-gyre/texgyretermes-italic.otf",
            "fixtures/fonts/tex-gyre/texgyretermes-bold.otf",
            "fixtures/fonts/tex-gyre/texgyretermes-bolditalic.otf"};
        QList<ApplicationFonts::DownloadedFontData> faces;
        for (int style = 0; style < 4; ++style) {
            QFile font(QFINDTESTDATA(paths[style]));
            QVERIFY(font.open(QIODevice::ReadOnly));
            faces.append({font.readAll(), QStringLiteral("TeX Gyre Termes"),
                          bool(style & 2), bool(style & 1), uint('x')});
        }
        QString error;
        QVERIFY2(ApplicationFonts::installDownloadedFonts(faces, error), qPrintable(error));
    }
    const QString sourcePath = probePath.isEmpty() ? m_temporaryDirectory.filePath(
        QStringLiteral("embedded-live-style-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())))
        : probePath;
    if (probePath.isEmpty()) {
        PDFiumProcessLock lock;
        QVERIFY(createEmbeddedTestFontPdf(sourcePath));
    }
    PDFDocument pdf;
    PDFEditController controller;
    QVERIFY(pdf.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&pdf);
    controller.setTool(PDFEditController::SelectTool);
    const int page = probePath.isEmpty() ? 0 : qEnvironmentVariableIntValue("TREPDF_TEST_STYLE_PAGE");
    const QString originalText = probePath.isEmpty() ? QStringLiteral("Tiếng Việt gốc")
        : qEnvironmentVariable("TREPDF_TEST_STYLE_TEXT");
    QVERIFY(originalText.size() > 4);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, page), originalText);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(page, candidate.value(QStringLiteral("objectId")).toString()));
    const QString sourceFamily = controller.selectedCharacterSpans().constFirst().toMap()
        .value(QStringLiteral("fontFamily")).toString();
    QVERIFY(!PDFFontAliases::sourceFingerprint(sourceFamily).isEmpty());

    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQuickWindow window;
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(
        editOverlayQmlPath()));
    const qreal zoom = 1.5;
    const QSizeF pageSize = pdf.pageSize(page);
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties({
        {QStringLiteral("page"), page}, {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), false},
        {QStringLiteral("draftTextureReady"), false},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), zoom},
        {QStringLiteral("width"), pageSize.width() * zoom},
        {QStringLiteral("height"), pageSize.height() * zoom}}));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    auto *overlayItem = qobject_cast<QQuickItem *>(overlay.get());
    QVERIFY(overlayItem);
    overlayItem->setParentItem(window.contentItem());
    window.resize(qCeil(pageSize.width() * zoom), 600);
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
    QVERIFY(editorItem);
    auto *quickDocument = editor->property("textDocument").value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *document = quickDocument->textDocument();
    QVERIFY(document);
    QTRY_COMPARE(document->toPlainText(), originalText);

    QString fontError;
    const auto hasRenderedStyle = [&](int start, int length, bool expectedBold, bool expectedItalic) {
        document->size();
        const QTextBlock block = document->findBlock(start);
        const auto runs = block.layout()->glyphRuns(start - block.position(), length);
        if (runs.isEmpty()) {
            fontError = QStringLiteral("No glyph runs for range %1:%2").arg(start).arg(length);
            return false;
        }
        for (const QGlyphRun &run : runs) {
            const QRawFont raw = run.rawFont();
            // A font engine may report the requested synthetic weight/style in
            // QRawFont too. Inspect the actual font program backing its glyphs.
            const QByteArray os2 = raw.fontTable("OS/2");
            const int weight = os2.size() >= 64
                ? (quint8(os2[4]) << 8) | quint8(os2[5]) : -1;
            const bool italicFace = os2.size() >= 64 && (quint8(os2[63]) & 1);
            if (!raw.isValid() || weight < 0 || (weight >= 700) != expectedBold
                || italicFace != expectedItalic) {
                fontError = QStringLiteral("Range %1:%2 rendered with %3 / %4 (intrinsic weight %5, italic %6)")
                    .arg(start).arg(length).arg(raw.familyName(), raw.styleName())
                    .arg(weight).arg(italicFace);
                return false;
            }
        }
        return true;
    };
    QVERIFY2(hasRenderedStyle(0, originalText.size(), false, false), qPrintable(fontError));
    const int rangeStart = formatSelection ? originalText.size() - 4 : originalText.size();
    const QString insertion = inputMethod ? QStringLiteral("ế") : QStringLiteral("x");
    const int rangeLength = formatSelection ? 4 : insertion.size();
    QVERIFY(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, rangeStart),
                                     Q_ARG(int, rangeStart + (formatSelection ? rangeLength : 0))));
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "syncEditorSelection"));
    if (bold)
        controller.setSelectedFontBold(true);
    if (italic)
        controller.setSelectedFontItalic(true);
    if (!formatSelection) {
        QVERIFY(!controller.hasDrafts());
        editorItem->forceActiveFocus();
        if (inputMethod) {
            QInputMethodEvent event;
            event.setCommitString(insertion);
            QCoreApplication::sendEvent(editorItem, &event);
        } else {
            QTest::keyClick(&window, Qt::Key_X);
        }
    }
    const QString editedText = formatSelection ? originalText : originalText + insertion;
    QTRY_COMPARE(controller.selectedText(), editedText);
    // Semantic fontWeight alone misses an embedded Regular alias still drawing
    // Regular outlines after the editor has accepted a Bold/Italic command.
    QTRY_VERIFY2(hasRenderedStyle(rangeStart, rangeLength, bold, italic), qPrintable(fontError));
    QVERIFY2(hasRenderedStyle(0, rangeStart, false, false), qPrintable(fontError));
    QTextCursor prefix(document);
    prefix.setPosition(0);
    prefix.setPosition(1, QTextCursor::KeepAnchor);
    QCOMPARE(prefix.charFormat().fontFamilies().toStringList().constFirst(), sourceFamily);

    QVERIFY(QMetaObject::invokeMethod(editor, "undo"));
    QTRY_COMPARE(controller.selectedText(), originalText);
    QTRY_VERIFY2(hasRenderedStyle(0, originalText.size(), false, false), qPrintable(fontError));
    QVERIFY(!document->isUndoAvailable());
    QVERIFY(QMetaObject::invokeMethod(editor, "redo"));
    QTRY_COMPARE(controller.selectedText(), editedText);
    QTRY_VERIFY2(hasRenderedStyle(rangeStart, rangeLength, bold, italic), qPrintable(fontError));
    QVERIFY2(hasRenderedStyle(0, rangeStart, false, false), qPrintable(fontError));
    QVERIFY(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, rangeStart),
                                     Q_ARG(int, rangeStart + rangeLength)));
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "syncEditorSelection"));
    if (bold) {
        controller.setSelectedFontBold(false);
        QTRY_VERIFY2(hasRenderedStyle(rangeStart, rangeLength, false, italic), qPrintable(fontError));
    }
    if (italic) {
        controller.setSelectedFontItalic(false);
        QTRY_VERIFY2(hasRenderedStyle(rangeStart, rangeLength, false, false), qPrintable(fontError));
    }
    QTextCursor restored(document);
    restored.setPosition(rangeStart);
    restored.setPosition(rangeStart + rangeLength, QTextCursor::KeepAnchor);
    QCOMPARE(restored.charFormat().fontFamilies().toStringList().constFirst(), sourceFamily);
    if (!probePath.isEmpty()) {
        if (bold)
            controller.setSelectedFontBold(true);
        if (italic)
            controller.setSelectedFontItalic(true);
        QVERIFY(QMetaObject::invokeMethod(overlay.get(), "commitEditing"));
        const QString output = m_temporaryDirectory.filePath(
            QStringLiteral("live-style-probe-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
        QVERIFY(pageText(output, page).contains(editedText));
        const QVariantMap reopened = candidateForText(waitForCandidates(controller, page), editedText);
        QVERIFY(!reopened.isEmpty());
        QVERIFY(controller.selectCandidate(page, reopened.value(QStringLiteral("objectId")).toString()));
        bool inspectedStyledSpan = false;
        for (const QVariant &value : controller.selectedCharacterSpans()) {
            const QVariantMap span = value.toMap();
            const int start = span.value(QStringLiteral("start")).toInt();
            const int end = start + span.value(QStringLiteral("length")).toInt();
            if (start >= rangeStart + rangeLength || end <= rangeStart)
                continue;
            inspectedStyledSpan = true;
            QCOMPARE(span.value(QStringLiteral("fontBold")).toBool(), bold);
            QCOMPARE(span.value(QStringLiteral("fontItalic")).toBool(), italic);
        }
        QVERIFY(inspectedStyledSpan);
        QVERIFY(QMetaObject::invokeMethod(overlay.get(), "beginSelectedEditing"));
        document = quickDocument->textDocument();
        QTRY_COMPARE(document->toPlainText(), editedText);
        QTRY_VERIFY2(hasRenderedStyle(rangeStart, rangeLength, bold, italic), qPrintable(fontError));
        QVERIFY2(hasRenderedStyle(0, rangeStart, false, false), qPrintable(fontError));
        QVERIFY(QMetaObject::invokeMethod(overlay.get(), "cancelEditing"));
        QVERIFY(!controller.hasDrafts());
        return;
    }
    QVERIFY(QMetaObject::invokeMethod(overlay.get(), "cancelEditing"));
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::appliesTypingFormatBetweenRepeatedCharacters()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(QStringLiteral("aaa"));
    m_controller.setSelectedTextRange(1, 0);
    m_controller.setSelectedFontBold(true);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral(
            "import QtQuick\n"
            "TextEdit { textFormat: TextEdit.RichText; text: \"aaa\" }"),
        QUrl());
    std::unique_ptr<QObject> textEdit(component.create());
    QVERIFY2(textEdit, qPrintable(component.errorString()));
    auto *quickDocument = textEdit->property("textDocument")
                              .value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *textDocument = quickDocument->textDocument();
    QVERIFY(textDocument);
    textDocument->clearUndoRedoStacks();
    QVERIFY(QMetaObject::invokeMethod(
        textEdit.get(),
        "insert",
        Q_ARG(int, 1),
        Q_ARG(QString, QStringLiteral("a"))));

    m_controller.updateSelectedTextPreview(QStringLiteral("aaaa"), 2);
    m_controller.applyTypingFormatToEditorDocument(quickDocument, 1, 1);
    m_controller.setSelectedTextRange(2, 0);

    QTextCursor formattedCursor(textDocument);
    formattedCursor.setPosition(1);
    formattedCursor.setPosition(2, QTextCursor::KeepAnchor);
    QCOMPARE(formattedCursor.charFormat().fontWeight(), QFont::Bold);
    textDocument->undo();
    QCOMPARE(textDocument->toPlainText(), QStringLiteral("aaa"));

    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap draft = drafts.constFirst().toMap();
    QCOMPARE(draft.value(QStringLiteral("text")).toString(), QStringLiteral("aaaa"));
    const QVariantList spans = draft.value(
        QStringLiteral("characterSpans")).toList();
    QCOMPARE(spans.size(), 3);
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("start")).toLongLong(), 0);
    QCOMPARE(spans.at(0).toMap().value(QStringLiteral("length")).toLongLong(), 1);
    QVERIFY(!spans.at(0).toMap().value(QStringLiteral("fontBold")).toBool());
    QCOMPARE(spans.at(1).toMap().value(QStringLiteral("start")).toLongLong(), 1);
    QCOMPARE(spans.at(1).toMap().value(QStringLiteral("length")).toLongLong(), 1);
    QVERIFY(spans.at(1).toMap().value(QStringLiteral("fontBold")).toBool());
    QCOMPARE(spans.at(2).toMap().value(QStringLiteral("start")).toLongLong(), 2);
    QCOMPARE(spans.at(2).toMap().value(QStringLiteral("length")).toLongLong(), 2);
    QVERIFY(!spans.at(2).toMap().value(QStringLiteral("fontBold")).toBool());

    m_controller.cancelSelectedTextEdit();
    m_controller.discardDrafts();
}

void PDFEditControllerTest::preservesEditorUndoWhenFormattingSelection()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QString editedText = QStringLiteral("ABCDX");
    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(editedText);
    m_controller.setSelectedTextRange(1, 2);
    QSignalSpy selectionStyleSpy(
        &m_controller,
        &PDFEditController::selectionFontStyleApplied);
    m_controller.setSelectedFontBold(true);
    QCOMPARE(selectionStyleSpy.size(), 1);

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        QByteArrayLiteral(
            "import QtQuick\n"
            "TextEdit { textFormat: TextEdit.RichText; text: \"ABCD\" }"),
        QUrl());
    std::unique_ptr<QObject> textEdit(component.create());
    QVERIFY2(textEdit, qPrintable(component.errorString()));
    auto *quickDocument = textEdit->property("textDocument")
                              .value<QQuickTextDocument *>();
    QVERIFY(quickDocument);
    QTextDocument *textDocument = quickDocument->textDocument();
    QVERIFY(textDocument);
    textDocument->clearUndoRedoStacks();
    QSignalSpy textChangedSpy(textEdit.get(), SIGNAL(textChanged()));
    QVERIFY(QMetaObject::invokeMethod(
        textEdit.get(),
        "insert",
        Q_ARG(int, 4),
        Q_ARG(QString, QStringLiteral("X"))));
    QCOMPARE(textDocument->toPlainText(), editedText);
    const int signalsAfterInsertion = textChangedSpy.size();
    QVERIFY(signalsAfterInsertion > 0);

    const QList<QVariant> styleArguments = selectionStyleSpy.takeFirst();
    QVERIFY(m_controller.applyFontStyleToEditorDocument(
        quickDocument,
        styleArguments.at(0).toInt(),
        styleArguments.at(1).toInt(),
        styleArguments.at(2).toInt(),
        styleArguments.at(3).toBool()));
    QVERIFY(textChangedSpy.size() > signalsAfterInsertion);

    QTextCursor formattedCursor(textDocument);
    formattedCursor.setPosition(1);
    formattedCursor.setPosition(3, QTextCursor::KeepAnchor);
    QCOMPARE(formattedCursor.charFormat().fontWeight(), QFont::Bold);

    const int signalsAfterFormatting = textChangedSpy.size();
    textDocument->undo();
    QCOMPARE(textDocument->toPlainText(), editedText);
    QVERIFY(textChangedSpy.size() > signalsAfterFormatting);
    formattedCursor.setPosition(1);
    formattedCursor.setPosition(3, QTextCursor::KeepAnchor);
    QVERIFY(formattedCursor.charFormat().fontWeight() < QFont::Bold);
    QVERIFY(m_controller.syncEditorFontStyles(quickDocument));

    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantList spans = drafts.constFirst().toMap()
                                   .value(QStringLiteral("characterSpans"))
                                   .toList();
    QCOMPARE(spans.size(), 1);
    QVERIFY(!spans.constFirst().toMap()
                 .value(QStringLiteral("fontBold"))
                 .toBool());

    textDocument->undo();
    QCOMPARE(textDocument->toPlainText(), QStringLiteral("ABCD"));

    m_controller.cancelSelectedTextEdit();
    m_controller.discardDrafts();
}

void PDFEditControllerTest::formatsSelectedRangeInMultilineDraft()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QString text = QStringLiteral("Red blue\nGreen");
    const QColor rangeColor(QStringLiteral("#c62828"));
    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(text);
    m_controller.setSelectedTextRange(4, 4);
    m_controller.setSelectedColor(rangeColor);
    m_controller.setSelectedFontBold(true);
    m_controller.setSelectedFontItalic(true);
    m_controller.setSelectedFontUnderline(true);

    const QVariantList drafts = m_controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    const QVariantMap draft = drafts.constFirst().toMap();
    QCOMPARE(draft.value(QStringLiteral("text")).toString(), text);

    const QVariantList spans = draft.value(QStringLiteral("characterSpans")).toList();
    QCOMPARE(spans.size(), 3);
    const QVariantMap firstSpan = spans.at(0).toMap();
    QCOMPARE(firstSpan.value(QStringLiteral("start")).toLongLong(), 0);
    QCOMPARE(firstSpan.value(QStringLiteral("length")).toLongLong(), 4);
    QVERIFY(!firstSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!firstSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(!firstSpan.value(QStringLiteral("fontUnderline")).toBool());
    QVERIFY(firstSpan.value(QStringLiteral("color")).value<QColor>() != rangeColor);
    const QVariantMap selectedSpan = spans.at(1).toMap();
    QCOMPARE(selectedSpan.value(QStringLiteral("start")).toLongLong(), 4);
    QCOMPARE(selectedSpan.value(QStringLiteral("length")).toLongLong(), 4);
    QVERIFY(selectedSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(selectedSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(selectedSpan.value(QStringLiteral("fontUnderline")).toBool());
    QCOMPARE(selectedSpan.value(QStringLiteral("color")).value<QColor>(), rangeColor);
    const QVariantMap remainingSpan = spans.at(2).toMap();
    QCOMPARE(remainingSpan.value(QStringLiteral("start")).toLongLong(), 8);
    QCOMPARE(
        remainingSpan.value(QStringLiteral("length")).toLongLong(),
        text.size() - 8);
    QVERIFY(!remainingSpan.value(QStringLiteral("fontBold")).toBool());
    QVERIFY(!remainingSpan.value(QStringLiteral("fontItalic")).toBool());
    QVERIFY(!remainingSpan.value(QStringLiteral("fontUnderline")).toBool());
    QVERIFY(remainingSpan.value(QStringLiteral("color")).value<QColor>() != rangeColor);

    const QVariantList paragraphs = draft.value(
        QStringLiteral("paragraphFormats")).toList();
    QCOMPARE(paragraphs.size(), 2);
    QCOMPARE(paragraphs.at(0).toMap().value(QStringLiteral("start")).toLongLong(), 0);
    QCOMPARE(paragraphs.at(0).toMap().value(QStringLiteral("length")).toLongLong(), 8);
    QCOMPARE(paragraphs.at(1).toMap().value(QStringLiteral("start")).toLongLong(), 9);
    QCOMPARE(paragraphs.at(1).toMap().value(QStringLiteral("length")).toLongLong(), 5);

    m_controller.finishSelectedTextEdit();
    m_controller.undo();
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
    m_controller.redo();
    QCOMPARE(
        m_controller.draftsForPage(0).constFirst().toMap()
            .value(QStringLiteral("characterSpans")).toList().size(),
        3);

    m_controller.discardDrafts();
}

void PDFEditControllerTest::alignsSelectedParagraphsWithUndoRedo()
{
    QVERIFY(m_controller.selectCandidate(
        0,
        m_regularCandidate.value(QStringLiteral("objectId")).toString()));

    const QString text = QStringLiteral("First\nSecond\nThird");
    m_controller.beginSelectedTextEdit();
    m_controller.updateSelectedTextPreview(text);
    m_controller.setSelectedTextRange(7, 0);
    QCOMPARE(
        m_controller.selectedParagraphAlignment(),
        static_cast<int>(PDFEditController::LeftAlignment));

    m_controller.setSelectedParagraphAlignment(
        PDFEditController::CenterAlignment);
    QCOMPARE(
        m_controller.selectedParagraphAlignment(),
        static_cast<int>(PDFEditController::CenterAlignment));

    QVariantList paragraphs = m_controller.draftsForPage(0)
                                  .constFirst()
                                  .toMap()
                                  .value(QStringLiteral("paragraphFormats"))
                                  .toList();
    QCOMPARE(paragraphs.size(), 3);
    QCOMPARE(
        paragraphs.at(0).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("left"));
    QCOMPARE(
        paragraphs.at(1).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("center"));
    QCOMPARE(
        paragraphs.at(2).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("left"));

    m_controller.updateSelectedTextPreview(
        QStringLiteral("First\nSecond!\nThird"));
    paragraphs = m_controller.draftsForPage(0)
                     .constFirst()
                     .toMap()
                     .value(QStringLiteral("paragraphFormats"))
                     .toList();
    QCOMPARE(
        paragraphs.at(1).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("center"));

    m_controller.setSelectedTextRange(0, text.size());
    QCOMPARE(m_controller.selectedParagraphAlignment(), -1);
    m_controller.finishSelectedTextEdit();

    QVERIFY(m_controller.canUndo());
    m_controller.undo();
    QVERIFY(m_controller.draftsForPage(0).isEmpty());
    m_controller.redo();
    paragraphs = m_controller.draftsForPage(0)
                     .constFirst()
                     .toMap()
                     .value(QStringLiteral("paragraphFormats"))
                     .toList();
    QCOMPARE(
        paragraphs.at(1).toMap().value(QStringLiteral("alignment")).toString(),
        QStringLiteral("center"));

    m_controller.discardDrafts();
}

void PDFEditControllerTest::appliesFontStylesToDisplayFont_data()
{
    QTest::addColumn<bool>("fontBold");
    QTest::addColumn<bool>("fontItalic");
    QTest::addColumn<bool>("fontUnderline");

    QTest::newRow("regular") << false << false << false;
    QTest::newRow("bold") << true << false << false;
    QTest::newRow("italic") << false << true << false;
    QTest::newRow("underline") << false << false << true;
    QTest::newRow("combined") << true << true << true;
}

void PDFEditControllerTest::appliesFontStylesToDisplayFont()
{
    QFETCH(bool, fontBold);
    QFETCH(bool, fontItalic);
    QFETCH(bool, fontUnderline);

    const QFont displayFont = m_controller.fontForDisplay(
        QString(),
        QFont::Unstretched,
        fontBold,
        fontItalic,
        fontUnderline,
        18.0);

    QCOMPARE(displayFont.bold(), fontBold);
    QCOMPARE(displayFont.italic(), fontItalic);
    QCOMPARE(displayFont.underline(), fontUnderline);
    QCOMPARE(displayFont.pixelSize(), 18);
    QCOMPARE(displayFont.stretch(), static_cast<int>(QFont::Unstretched));
}

void PDFEditControllerTest::movesTopLevelTextWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-text-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap textCandidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kGroupedText));
    QVERIFY2(!textCandidate.isEmpty(), "Top-level text candidate was not discovered");
    const QString objectId = textCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF originalFrame = textCandidate
        .value(QStringLiteral("frameRect"))
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

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    QVERIFY(controller.textEditActive());
    controller.updateSelectedFrameRect(movedFrame);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    controller.finishSelectedFrameResize();

    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    const QVariantList drafts = controller.draftsForPage(0);
    QCOMPARE(drafts.size(), 1);
    QCOMPARE(
        drafts.constFirst().toMap()
            .value(QStringLiteral("sourceTextMoveOnly")).toBool(),
        true);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 0);
    const QVector<PDFEditController::DraftObjectMove> moves
        = controller.draftedObjectMovesForPage(0);
    QCOMPARE(moves.size(), 2);
    for (const PDFEditController::DraftObjectMove &move : moves) {
        QCOMPARE(move.kind, PDFEditController::DraftObjectMove::Kind::Text);
        QCOMPARE(move.sourceRect, originalFrame);
        QCOMPARE(move.frameRect, movedFrame);
        QCOMPARE(move.objectPath.size(), 1);
    }

    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);

    controller.redo();
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 2);

    controller.commitSelectedText(QStringLiteral("Moved and edited PDF text"));
    const QVariantList editedDrafts = controller.draftsForPage(0);
    QCOMPARE(editedDrafts.size(), 1);
    QCOMPARE(
        editedDrafts.constFirst().toMap()
            .value(QStringLiteral("sourceTextMoveOnly")).toBool(),
        false);
    QVERIFY(!controller.draftedTextObjectPathsForPage(0).isEmpty());
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);
}
