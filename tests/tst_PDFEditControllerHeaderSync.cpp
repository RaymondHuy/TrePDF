#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

namespace {
bool createSyncPages(const QString &path, int count, bool existing = false)
{
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>", {},
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    QByteArray kids;
    for (int page = 0; page < count; ++page) {
        const int id = objects.size() + 1;
        kids += QByteArray::number(id) + " 0 R ";
        QByteArray content;
        if (existing) {
            const QByteArray text = (page == 2 || page == 4) ? "Different header" : "Shared header";
            const QByteArray x = page == 3 ? "120" : "24";
            content = "BT /F1 12 Tf " + x + " 370 Td (" + text + ") Tj ET\n"
                "BT /F1 12 Tf 24 190 Td (Shared header) Tj ET\n";
        }
        objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] "
            "/Resources << /Font << /F1 3 0 R >> >> /Contents "
            + QByteArray::number(id + 1) + " 0 R >>");
        objects.append("<< /Length " + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream");
    }
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count "
        + QByteArray::number(count) + " >>";
    return writePdfObjects(path, objects);
}
bool createImageSyncPages(const QString &path)
{
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>", {},
        "<< /Type /XObject /Subtype /Image /Width 1 /Height 1 "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /ASCIIHexDecode "
        "/Length 7 >>\nstream\nff0000>\nendstream",
        "<< /Type /XObject /Subtype /Image /Width 1 /Height 1 "
        "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /ASCIIHexDecode "
        "/Length 7 >>\nstream\n0000ff>\nendstream"};
    QByteArray kids;
    for (int page = 0; page < 4; ++page) {
        const int id = objects.size() + 1;
        kids += QByteArray::number(id) + " 0 R ";
        QByteArray content("q 40 0 0 20 24 350 cm /I1 Do Q\n"
                                 "q 40 0 0 20 24 180 cm /I1 Do Q\n");
        if (page == 3)
            content += "q 40 0 0 20 25 349 cm /I1 Do Q\n";
        objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] "
            "/Resources << /XObject << /I1 " + QByteArray::number(page == 2 ? 4 : 3)
            + " 0 R >> >> /Contents " + QByteArray::number(id + 1) + " 0 R >>");
        objects.append("<< /Length " + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream");
    }
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count 4 >>";
    return writePdfObjects(path, objects);
}
QString syncId(const QVariantMap &object)
{
    return object.value(QStringLiteral("objectId")).toString();
}
QVariantMap headerCandidate(PDFEditController &controller, int page, const QString &text)
{
    for (const QVariant &value : waitForCandidates(controller, page)) {
        const QVariantMap candidate = value.toMap();
        if (candidate.value(QStringLiteral("text")).toString() == text
            && candidate.value(QStringLiteral("frameRect")).toRectF().bottom() <= 0.25)
            return candidate;
    }
    return {};
}
QVariantMap onlyDraft(const PDFEditController &controller, int page)
{
    const QVariantList drafts = controller.draftsForPage(page);
    return drafts.isEmpty() ? QVariantMap() : drafts.constFirst().toMap();
}
QString draftText(const PDFEditController &controller, int page)
{
    return onlyDraft(controller, page).value(QStringLiteral("text")).toString();
}
void activateHeaderSelection(PDFEditController &controller)
{
    controller.setTool(PDFEditController::SelectTool);
    controller.setHeaderSelectionOnly(true);
}
}

void PDFEditControllerTest::syncsSimilarSourceHeadersFromOriginalText()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("sync-source.pdf"));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("sync-source-saved.pdf"));
    QVERIFY(createSyncPages(source, 5, true));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    activateHeaderSelection(controller);
    const auto anchor = headerCandidate(controller, 0, QStringLiteral("Shared header"));
    QVERIFY(!anchor.isEmpty());
    QVERIFY(controller.selectCandidate(0, syncId(anchor)));
    QVERIFY(controller.headerSyncAvailable());
    controller.commitSelectedText(QStringLiteral("Changed header"));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 2);
    waitForCandidates(controller, 1);
    QVERIFY(controller.headerSyncCanApply());
    QVERIFY2(controller.applyHeaderChanges(), qPrintable(controller.headerSyncError()));
    QCOMPARE(draftText(controller, 0), QStringLiteral("Changed header"));
    QCOMPARE(draftText(controller, 1), QStringLiteral("Changed header"));
    for (int page = 2; page < 5; ++page)
        QVERIFY(controller.draftsForPage(page).isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(draftText(controller, 1), QStringLiteral("Changed header"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    QCOMPARE(normalizedPageText(saved, 0).count(QStringLiteral("Changed header")), 1);
    QCOMPARE(normalizedPageText(saved, 1).count(QStringLiteral("Changed header")), 1);
    QVERIFY(normalizedPageText(saved, 0).contains(QStringLiteral("Shared header")));
    QVERIFY(!normalizedPageText(saved, 2).contains(QStringLiteral("Changed header")));
}

void PDFEditControllerTest::syncsGroupedHeaderDraftsToCustomPagesWithUndo()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("sync-draft.pdf"));
    QVERIFY(createSyncPages(path, 3));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Original"), QStringLiteral("all"), 24, 20, 180));
    const QString anchor = syncId(onlyDraft(controller, 0));
    activateHeaderSelection(controller);
    QVERIFY(controller.selectCandidate(0, anchor));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 3);
    controller.commitSelectedText(QStringLiteral("Updated"));
    QVERIFY(!controller.applyHeaderChanges(QStringLiteral("4")));
    QCOMPARE(draftText(controller, 1), QStringLiteral("Original"));
    QVERIFY(controller.applyHeaderChanges(QStringLiteral("2")));
    QCOMPARE(draftText(controller, 0), QStringLiteral("Updated"));
    QCOMPARE(draftText(controller, 1), QStringLiteral("Updated"));
    QCOMPARE(draftText(controller, 2), QStringLiteral("Original"));
    controller.undo();
    for (int page = 0; page < 3; ++page)
        QCOMPARE(draftText(controller, page), QStringLiteral("Original"));
    controller.redo();
    QCOMPARE(draftText(controller, 1), QStringLiteral("Updated"));
    QCOMPARE(draftText(controller, 2), QStringLiteral("Original"));
}

void PDFEditControllerTest::syncsLegacyFragmentedHeaderText()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("sync-legacy-header.pdf"));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("sync-legacy-header-saved.pdf"));
    const QByteArray mark = "/TrePDFHeader << /Version 1 /Group (b8717a08-dfa3-4f80-b73c-7cfe62900bf0) /Role (text) >> BDC\n";
    const QByteArray fragments =
        "BT /F1 12 Tf 1.6 0 0 1 24 370 Tm (i) Tj ET\n"
        "BT /F1 12 Tf 1 0 0 1 25.6 370 Tm (s) Tj ET\n"
        "BT /F1 12 Tf 1.6 0 0 1 38 370 Tm (i) Tj ET\n"
        "BT /F1 12 Tf 1 0 0 1 39.6 370 Tm (s) Tj ET\n"
        "BT /F1 12 Tf 1 0 0 1 24 355 Tm (222) Tj ET\n";
    const QVector<QByteArray> streams{
        mark + "BT /F1 12 Tf 24 370 Td (is is 222) Tj ET\nEMC\n",
        mark + fragments + "EMC\n",
        // Duplicate origins and widely separated blocks remain ambiguous even
        // when a malformed producer reuses the same Header group identifier.
        mark + "BT /F1 12 Tf 24 370 Td (is is 222) Tj ET\n"
            "BT /F1 12 Tf 24 370 Td (Other text) Tj ET\nEMC\n",
        mark + "BT /F1 12 Tf 24 370 Td (is is 222) Tj ET\n"
            "BT /F1 12 Tf 170 370 Td (is is 222) Tj ET\nEMC\n",
        fragments};
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>", {},
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    QByteArray kids;
    for (const QByteArray &content : streams) {
        const int id = objects.size() + 1;
        kids += QByteArray::number(id) + " 0 R ";
        objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] "
            "/Resources << /Font << /F1 3 0 R >> >> /Contents "
            + QByteArray::number(id + 1) + " 0 R >>");
        objects.append("<< /Length " + QByteArray::number(content.size())
            + " >>\nstream\n" + content + "endstream");
    }
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count 5 >>";
    QVERIFY(writePdfObjects(path, objects));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    activateHeaderSelection(controller);
    const auto original = waitForCandidates(controller, 1);
    QCOMPARE(original.size(), 1);
    QCOMPARE(original.first().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("is is\n222"));
    QCOMPARE(waitForCandidates(controller, 2).size(), 2);
    QCOMPARE(waitForCandidates(controller, 3).size(), 2);
    QVERIFY(waitForCandidates(controller, 4).size() > 1);
    const auto anchor = headerCandidate(controller, 0, QStringLiteral("is is 222"));
    QVERIFY(controller.selectCandidate(0, syncId(anchor)));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 2);
    controller.commitSelectedText(QStringLiteral("is is 333"));
    QVERIFY(controller.applyHeaderChanges());
    QCOMPARE(draftText(controller, 1), QStringLiteral("is is 333"));
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(waitForCandidates(controller, 1), original);
    controller.redo();
    QCOMPARE(draftText(controller, 1), QStringLiteral("is is 333"));
    for (int page = 2; page < 5; ++page)
        QVERIFY(controller.draftsForPage(page).isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    for (int page = 0; page < 2; ++page) {
        const auto reopened = waitForCandidates(controller, page);
        QCOMPARE(reopened.size(), 1);
        QCOMPARE(reopened.first().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("is is 333"));
        QCOMPARE(normalizedPageText(saved, page), QStringLiteral("is is 333"));
    }
}

void PDFEditControllerTest::syncsGroupedHeaderTextLayoutAcrossDifferentFrames()
{
    // Reopened members of one Header batch can already have different layout
    // frames. Content or font edits must use the anchor's physical frame size,
    // including on a different-sized page, rather than preserve that mismatch.
    for (bool changeFontOnly : {false, true}) {
        const QString suffix = changeFontOnly ? QStringLiteral("font") : QStringLiteral("text");
        const QString path = m_temporaryDirectory.filePath(QStringLiteral("sync-layout-%1.pdf").arg(suffix));
        const QString seed = m_temporaryDirectory.filePath(QStringLiteral("sync-layout-seed-%1.pdf").arg(suffix));
        const QString saved = m_temporaryDirectory.filePath(QStringLiteral("sync-layout-saved-%1.pdf").arg(suffix));
        QVERIFY(writePdfObjects(path, {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << >> >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>"}));
        PDFDocument document;
        QVERIFY(document.open(QUrl::fromLocalFile(path)));
        PDFEditController controller;
        controller.setDocument(&document);
        const QString original = QStringLiteral("this is Huy");
        QVERIFY(controller.addHeaderText(original, QStringLiteral("all"), 24, 20, 65));
        controller.setTool(PDFEditController::SelectTool);
        QVERIFY(controller.selectCandidate(0, syncId(onlyDraft(controller, 0))));
        controller.beginSelectedFrameResize();
        controller.updateSelectedFrameRect(QRectF(24.0 / 300, 20.0 / 400, 180.0 / 300, 24.0 / 400));
        controller.finishSelectedFrameResize();
        QVERIFY(controller.selectCandidate(2, syncId(onlyDraft(controller, 2))));
        controller.beginSelectedFrameResize();
        controller.updateSelectedFrameRect(controller.selectedFrameRect().translated(12.0 / 300, 5.0 / 400));
        controller.finishSelectedFrameResize();
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(seed)));
        activateHeaderSelection(controller);
        QVector<QRectF> originalFrames;
        for (int page = 0; page < 3; ++page) {
            const auto candidate = headerCandidate(controller, page, original);
            QVERIFY(!candidate.isEmpty());
            originalFrames.append(candidate.value(QStringLiteral("frameRect")).toRectF());
        }
        QVERIFY(qAbs(originalFrames.at(0).width() * 300 - 180) < 0.01);
        QVERIFY(qAbs(originalFrames.at(1).width() * 600 - 65) < 0.01);
        QVERIFY(controller.selectCandidate(0, syncId(headerCandidate(controller, 0, original))));
        controller.findSimilarHeaders();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
        QCOMPARE(controller.headerSyncMatchCount(), 3);
        const QString expected = changeFontOnly ? original : QStringLiteral("this is Huy 222");
        if (changeFontOnly)
            controller.setSelectedFontSize(18);
        else
            controller.commitSelectedText(expected);
        const QRectF anchorFrame = controller.selectedFrameRect();
        QVERIFY2(controller.applyHeaderChanges(), qPrintable(controller.headerSyncError()));
        const qreal anchorContentHeight = onlyDraft(controller, 0)
            .value(QStringLiteral("contentBounds")).toRectF().height() * 400;
        for (int page = 0; page < 3; ++page) {
            const auto draft = onlyDraft(controller, page);
            const auto frame = draft.value(QStringLiteral("frameRect")).toRectF();
            const QSizeF pageSize = document.pageSize(page);
            QCOMPARE(draft.value(QStringLiteral("text")).toString(), expected);
            QVERIFY(qAbs(frame.width() * pageSize.width() - anchorFrame.width() * 300) < 0.01);
            QVERIFY(qAbs(frame.height() * pageSize.height() - anchorFrame.height() * 400) < 0.01);
            QVERIFY(qAbs(frame.x() - originalFrames.at(page).x()) < 0.00001);
            QVERIFY(qAbs(frame.y() - originalFrames.at(page).y()) < 0.00001);
            QVERIFY(qAbs(draft.value(QStringLiteral("contentBounds")).toRectF().height()
                * pageSize.height() - anchorContentHeight) < 0.1);
        }
        controller.undo();
        QVERIFY(!controller.hasDrafts());
        for (int page = 0; page < 3; ++page) {
            const auto candidate = headerCandidate(controller, page, original);
            QCOMPARE(candidate.value(QStringLiteral("frameRect")).toRectF(), originalFrames.at(page));
        }
        controller.redo();
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
        for (int page = 0; page < 3; ++page) {
            // Do not simplify whitespace: that would conceal an unintended wrap.
            QString extracted = pageText(saved, page);
            extracted.replace(QChar(0xfffe), QLatin1Char(' '));
            extracted.replace(QChar(0xffff), QLatin1Char(' '));
            QCOMPARE(extracted.trimmed(), expected);
            const auto candidate = headerCandidate(controller, page, expected);
            QVERIFY(!candidate.isEmpty());
            const auto frame = candidate.value(QStringLiteral("frameRect")).toRectF();
            QVERIFY(qAbs(frame.width() * document.pageSize(page).width() - anchorFrame.width() * 300) < 0.01);
        }
    }
}

void PDFEditControllerTest::retainsHeaderGroupsAndPageNumberValuesAfterSave()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("sync-numbers.pdf"));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("sync-numbers-saved.pdf"));
    const QString styled = m_temporaryDirectory.filePath(QStringLiteral("sync-numbers-styled.pdf"));
    QVERIFY(createSyncPages(path, 4));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderPageNumbers(1, 4, 1, 3, 24, 20, 80));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    activateHeaderSelection(controller);
    const auto anchor = headerCandidate(controller, 0, QStringLiteral("i"));
    QVERIFY(!anchor.isEmpty());
    QVERIFY(controller.selectCandidate(0, syncId(anchor)));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 4);
    for (int page = 1; page < 4; ++page)
        waitForCandidates(controller, page);
    controller.setSelectedFontSize(18);
    QVERIFY2(controller.applyHeaderChanges(), qPrintable(controller.headerSyncError()));
    const QStringList expected{QStringLiteral("i"), QStringLiteral("ii"), QStringLiteral("1"), QStringLiteral("2")};
    for (int page = 0; page < 4; ++page) {
        QCOMPARE(draftText(controller, page), expected.at(page));
        QCOMPARE(onlyDraft(controller, page).value(QStringLiteral("fontSizeRatio")).toReal() * 400, 18.0);
    }
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(styled)));
    for (int page = 0; page < 4; ++page)
        QCOMPARE(normalizedPageText(styled, page).trimmed(), expected.at(page));
    const auto restored = headerCandidate(controller, 2, QStringLiteral("1"));
    QVERIFY(!restored.isEmpty());
    QVERIFY(controller.selectCandidate(2, syncId(restored)));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 4);
    controller.commitSelectedText(QStringLiteral("9"));
    QVERIFY(!controller.applyHeaderChanges());
    QCOMPARE(controller.headerSyncError(), QStringLiteral("HeaderSyncNumberContent"));
    for (int page : {0, 1, 3})
        QVERIFY(controller.draftsForPage(page).isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::syncsHeaderImagesAndDeletionWithHistory()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("sync-images.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("sync-image.png"));
    const QString saved = m_temporaryDirectory.filePath(QStringLiteral("sync-images-saved.pdf"));
    QVERIFY(createSyncPages(path, 3));
    QImage image(40, 20, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(imagePath));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(!controller.prepareHeaderImage(QUrl::fromLocalFile(imagePath)).isEmpty());
    QVERIFY(controller.addHeaderImage(QStringLiteral("all"), 24, 20, 60, 30));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(saved)));
    activateHeaderSelection(controller);
    const auto candidates = waitForCandidates(controller, 0);
    QCOMPARE(candidates.size(), 1);
    const auto anchor = candidates.constFirst().toMap();
    QVERIFY(controller.selectCandidate(0, syncId(anchor)));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 3);
    for (int page = 1; page < 3; ++page)
        waitForCandidates(controller, page);
    const QRectF original = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(original.translated(0.05, 0.025));
    controller.finishSelectedFrameResize();
    QVERIFY(controller.applyHeaderChanges());
    for (int page = 0; page < 3; ++page)
        QCOMPARE(onlyDraft(controller, page).value(QStringLiteral("frameRect")).toRectF(), original.translated(0.05, 0.025));
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.selectCandidate(0, syncId(anchor)));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    controller.deleteSelectedObject();
    QVERIFY(controller.headerSyncAvailable());
    QVERIFY(controller.applyHeaderChanges());
    for (int page = 0; page < 3; ++page)
        QVERIFY(onlyDraft(controller, page).value(QStringLiteral("deleted")).toBool());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::invalidatesHeaderSyncAcrossHistoryAndSessionChanges()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("sync-lifecycle.pdf"));
    QVERIFY(createSyncPages(path, 3));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Original"), QStringLiteral("all"), 24, 20, 180));
    const QString anchor = syncId(onlyDraft(controller, 0));
    activateHeaderSelection(controller);
    QVERIFY(controller.selectCandidate(0, anchor));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    controller.commitSelectedText(QStringLiteral("Changed"));
    controller.undo();
    QVERIFY(!controller.headerSyncCanApply());
    QVERIFY(!controller.applyHeaderChanges());
    QVERIFY(controller.selectCandidate(0, anchor));
    controller.findSimilarHeaders();
    controller.setHeaderSelectionOnly(false);
    QTRY_VERIFY(!controller.headerSyncLoading());
    QVERIFY(!controller.headerSyncAvailable());
    controller.setHeaderSelectionOnly(true);
    QVERIFY(controller.selectCandidate(0, anchor));
    controller.findSimilarHeaders();
    document.close();
    QTRY_VERIFY(!controller.headerSyncLoading());
    QVERIFY(!controller.headerSyncAvailable());
}

void PDFEditControllerTest::matchesHeaderImagesByPixelsNotOnlyGeometry()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("sync-image-match.pdf"));
    QVERIFY(createImageSyncPages(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    activateHeaderSelection(controller);
    const auto candidates = waitForCandidates(controller, 0);
    QCOMPARE(candidates.size(), 1);
    QVERIFY(controller.selectCandidate(0, syncId(candidates.constFirst().toMap())));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 2);
    waitForCandidates(controller, 1);
    controller.rotateSelectedImage(1);
    QVERIFY2(controller.applyHeaderChanges(), qPrintable(controller.headerSyncError()));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QCOMPARE(controller.draftsForPage(1).size(), 1);
    QVERIFY(controller.draftsForPage(2).isEmpty());
    QVERIFY(controller.draftsForPage(3).isEmpty());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::rejectsHeaderSyncOverflowAtomically()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("sync-overflow.pdf"));
    QVERIFY(writePdfObjects(path, {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Resources << >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 70 400] /Resources << >> >>"}));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("A"), QStringLiteral("all"), 20, 20, 180));
    activateHeaderSelection(controller);
    QVERIFY(controller.selectCandidate(0, syncId(onlyDraft(controller, 0))));
    controller.findSimilarHeaders();
    QTRY_COMPARE_WITH_TIMEOUT(controller.headerSyncMatchCount(), 3, 15000);
    const QString replacement = QStringLiteral("Header content with several words to wrap across the small page");
    controller.commitSelectedText(replacement);
    QVERIFY(!controller.applyHeaderChanges());
    QVERIFY(!controller.headerSyncError().isEmpty());
    QCOMPARE(draftText(controller, 0), replacement);
    QCOMPARE(draftText(controller, 1), QStringLiteral("A"));
    QCOMPARE(draftText(controller, 2), QStringLiteral("A"));
    controller.undo();
    for (int page = 0; page < 3; ++page)
        QCOMPARE(draftText(controller, page), QStringLiteral("A"));
}
