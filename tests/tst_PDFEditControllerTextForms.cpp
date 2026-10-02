#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

namespace {

const QString formText = QStringLiteral("Nested sample");
const QString changedFormText = QStringLiteral("Edited sample");

QByteArray textFormStream(const QByteArray &dictionary, const QByteArray &content)
{
    return "<< " + dictionary + " /Length " + QByteArray::number(content.size())
        + " >>\nstream\n" + content + "\nendstream";
}

bool createSharedTextFormPdf(const QString &path, bool nested, int rotation, bool crop,
                             bool inherited = false)
{
    // Counter-rotate page content so the visible text stays horizontal, including
    // the page-space normalization used by the inline editor.
    const QByteArray orientation = rotation == 1 ? "0 1 -1 0 400 0 cm\n"
        : rotation == 2 ? "-1 0 0 -1 400 400 cm\n"
        : rotation == 3 ? "0 -1 1 0 0 400 cm\n" : "";
    const QByteArray geometry = "/MediaBox [0 0 400 400] /Rotate "
        + QByteArray::number(rotation * 90)
        + (crop ? " /CropBox [20 20 380 380] " : " ");
    const QByteArray resources("/Resources << /Font << /F0 7 0 R /F1 9 0 R >> "
                               "/XObject << /Fm0 8 0 R >> >> ");
    const QByteArray text("BT /F0 12 Tf 1 0 0 1 4 10 Tm (Nested sample) Tj ET\n");
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R " + geometry + resources + "/Contents 5 0 R >>",
        "<< /Type /Page /Parent 2 0 R " + geometry + resources + "/Contents 6 0 R >>",
        textFormStream({}, "q " + orientation
            + "q 1 0 0 1 45 270 cm /Fm0 Do Q\n"
              "q 1 0 0 1 230 270 cm /Fm0 Do Q\n"
              "BT /F1 12 Tf 1 0 0 1 45 100 Tm (Unchanged neighbor) Tj ET\nQ\n"),
        textFormStream({}, "q " + orientation
            + "1 0 0 1 90 150 cm /Fm0 Do Q\n"),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        textFormStream(QByteArray("/Type /XObject /Subtype /Form /BBox [0 0 150 40] ")
            + "/Matrix [1.1 0 0 0.9 2 3] "
            + (inherited ? QByteArray() : QByteArray("/Resources << ")
                + (nested ? "/XObject << /Inner 10 0 R >>" : "/Font << /F0 7 0 R >>")
                + " >>"), nested ? QByteArray("/Inner Do\n") : text),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman >>",
    };
    if (nested) {
        objects.append(textFormStream(
            "/Type /XObject /Subtype /Form /BBox [0 0 130 35] "
            "/Matrix [0.95 0 0 1.1 1 2] /Resources << /Font << /F0 7 0 R >> >>", text));
    }
    return writePdfObjects(path, objects);
}

QImage renderTextFormPage(const QString &path, int index, QSize size = QSize(800, 800))
{
    PDFiumProcessLock lock;
    const QByteArray encoded = QFile::encodeName(path);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encoded.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, index) : nullptr;
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::white);
    FPDF_BITMAP bitmap = page ? FPDFBitmap_CreateEx(size.width(), size.height(),
        FPDFBitmap_BGRA, image.bits(), image.bytesPerLine()) : nullptr;
    if (bitmap) {
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, size.width(), size.height(), 0, FPDF_ANNOT);
        FPDFBitmap_Destroy(bitmap);
    } else {
        image = {};
    }
    if (page)
        FPDF_ClosePage(page);
    if (document)
        FPDF_CloseDocument(document);
    return image;
}

QVariantList matchingTextForms(const QVariantList &candidates, const QString &text)
{
    QVariantList result;
    for (const auto &value : candidates) {
        const QVariantMap candidate = value.toMap();
        if (candidate.value(QStringLiteral("kind")).toString() == QLatin1String("text")
            && candidate.value(QStringLiteral("text")).toString() == text)
            result.append(value);
    }
    std::sort(result.begin(), result.end(), [](const QVariant &left, const QVariant &right) {
        return left.toMap().value(QStringLiteral("sourceRect")).toRectF().left()
            < right.toMap().value(QStringLiteral("sourceRect")).toRectF().left();
    });
    return result;
}

QRect textFormPixels(const QRectF &rect, const QSize &size)
{
    return QRectF(rect.x() * size.width(), rect.y() * size.height(),
                  rect.width() * size.width(), rect.height() * size.height())
        .toAlignedRect().adjusted(-4, -4, 4, 4).intersected(QRect(QPoint(), size));
}

QVector<FS_MATRIX> topLevelFormMatrices(const QString &path, int pageIndex = 0)
{
    PDFiumProcessLock lock;
    const QByteArray encoded = QFile::encodeName(path);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encoded.constData(), nullptr);
    FPDF_PAGE page = document ? FPDF_LoadPage(document, pageIndex) : nullptr;
    QVector<FS_MATRIX> result;
    if (page) {
        for (int index = 0; index < FPDFPage_CountObjects(page); ++index) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, index);
            FS_MATRIX matrix{};
            if (FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_FORM
                && FPDFPageObj_GetMatrix(object, &matrix))
                result.append(matrix);
        }
        FPDF_ClosePage(page);
    }
    if (document)
        FPDF_CloseDocument(document);
    return result;
}

} // namespace

void PDFEditControllerTest::editsSharedTextFormsRoundTrip_data()
{
    QTest::addColumn<bool>("nested");
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("crop");
    QTest::addColumn<bool>("inherited");
    QTest::newRow("single-form") << false << 0 << false << false;
    QTest::newRow("nested-scaled-form") << true << 0 << false << false;
    QTest::newRow("nested-crop-90") << true << 1 << true << false;
    QTest::newRow("nested-crop-180") << true << 2 << true << false;
    QTest::newRow("nested-crop-270") << true << 3 << true << false;
    QTest::newRow("inherited-font") << false << 0 << false << true;
}

void PDFEditControllerTest::editsSharedTextFormsRoundTrip()
{
    QFETCH(bool, nested);
    QFETCH(int, rotation);
    QFETCH(bool, crop);
    QFETCH(bool, inherited);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("text-form-%1.pdf")
        .arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createSharedTextFormPdf(source, nested, rotation, crop, inherited));
    QFile sourceFile(source);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray sourceBytes = sourceFile.readAll();
    sourceFile.close();
    const QImage original = renderTextFormPage(source, 0);
    const QImage peerPage = renderTextFormPage(source, 1);
    QVERIFY(!original.isNull());
    QVERIFY(!peerPage.isNull());
    const QVector<FS_MATRIX> originalMatrices = topLevelFormMatrices(source);
    QCOMPARE(originalMatrices.size(), 2);

    for (const QString &operation : {QStringLiteral("content"), QStringLiteral("color"),
             QStringLiteral("resize"), QStringLiteral("move"), QStringLiteral("delete")}) {
        PDFDocument document;
        PDFEditController controller;
        QVERIFY(document.open(QUrl::fromLocalFile(source)));
        controller.setDocument(&document);
        const QVariantList candidates = waitForCandidates(controller, 0);
        const QVariantList forms = matchingTextForms(candidates, formText);
        QCOMPARE(forms.size(), 2);
        const QVariantMap candidate = forms.constFirst().toMap();
        const QString id = candidate.value(QStringLiteral("objectId")).toString();
        QVERIFY(controller.selectCandidate(0, id));
        QVERIFY2(controller.selectedEditable(), qPrintable(controller.selectedReason()));
        QVERIFY(controller.selectedDeletable());
        const QRectF frame = controller.selectedFrameRect();
        const QRectF sourceRect = controller.selectedSourceRect();
        const QRectF peerRect = forms.at(1).toMap().value(QStringLiteral("sourceRect")).toRectF();
        const QRectF neighborRect = candidateForText(candidates, QStringLiteral("Unchanged neighbor"))
            .value(QStringLiteral("sourceRect")).toRectF();
        QVERIFY(!neighborRect.isEmpty());
        const auto apply = [&]() {
            if (operation == QLatin1String("delete")) {
                controller.deleteSelectedObject();
            } else if (operation == QLatin1String("content")) {
                controller.commitSelectedText(changedFormText);
            } else if (operation == QLatin1String("color")) {
                controller.beginSelectedTextEdit();
                controller.setSelectedTextRange(0, controller.selectedText().size());
                controller.setSelectedColor(Qt::blue);
                controller.finishSelectedTextEdit();
            } else {
                controller.beginSelectedFrameResize();
                controller.updateSelectedFrameRect(operation == QLatin1String("move")
                    ? frame.translated(0.03, 0.15)
                    : QRectF(frame.topLeft(), QSizeF(frame.width() * 0.6, frame.height() * 2.4)));
                controller.finishSelectedFrameResize();
            }
        };
        apply();
        QVERIFY2(controller.hasDrafts(), qPrintable(operation));
        if (operation == QLatin1String("move")) {
            QVERIFY(controller.draftedTextObjectPathsForPage(0).isEmpty());
            const auto moves = controller.draftedObjectMovesForPage(0);
            QCOMPARE(moves.size(), 1);
            QCOMPARE(moves.constFirst().objectPath.size(), 1);
            QCOMPARE(moves.constFirst().kind, PDFEditController::DraftObjectMove::Kind::Text);
        } else {
            const auto paths = controller.draftedTextObjectPathsForPage(0);
            QCOMPARE(paths.size(), 1);
            QCOMPARE(paths.constFirst().size(), 1);
            QVERIFY(controller.allDraftedTextObjectsSuppressibleForPage(0));
        }
        controller.undo();
        QVERIFY(!controller.hasDrafts());
        QVERIFY(controller.selectCandidate(0, id));
        QCOMPARE(controller.selectedFrameRect(), frame);
        controller.redo();
        QVERIFY(controller.hasDrafts());
        controller.discardDrafts();
        QVERIFY(!controller.hasDrafts());
        QVERIFY(controller.selectCandidate(0, id));
        QCOMPARE(controller.selectedText(), formText);
        apply();

        const QString output = source + QLatin1Char('.') + operation + QStringLiteral(".pdf");
        QVERIFY2(controller.saveAs(QUrl::fromLocalFile(output)), qPrintable(operation));
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
        const QImage saved = renderTextFormPage(output, 0);
        QVERIFY(!saved.isNull());
        QVERIFY(changedPixelCount(original, saved, original.rect()) > 20);
        QCOMPARE(saved.copy(textFormPixels(peerRect, original.size())),
                 original.copy(textFormPixels(peerRect, original.size())));
        QCOMPARE(saved.copy(textFormPixels(neighborRect, original.size())),
                 original.copy(textFormPixels(neighborRect, original.size())));
        QCOMPARE(renderTextFormPage(output, 1), peerPage);
        QCOMPARE(normalizedPageText(output, 1), normalizedPageText(source, 1));
        const QString savedText = normalizedPageText(output);
        QVERIFY(savedText.contains(QStringLiteral("Unchanged neighbor")));
        QCOMPARE(savedText.count(formText), operation == QLatin1String("content")
            || operation == QLatin1String("delete") ? 1 : 2);
        if (operation == QLatin1String("content"))
            QVERIFY(savedText.contains(changedFormText));
        const QVector<FS_MATRIX> savedMatrices = topLevelFormMatrices(output);
        QCOMPARE(savedMatrices.size(), operation == QLatin1String("move") ? 2 : 1);
        if (operation == QLatin1String("move")) {
            const FS_MATRIX before = originalMatrices.constFirst();
            const FS_MATRIX after = savedMatrices.constFirst();
            QCOMPARE(after.a, before.a);
            QCOMPARE(after.b, before.b);
            QCOMPARE(after.c, before.c);
            QCOMPARE(after.d, before.d);
            QVERIFY(qAbs(after.e - before.e) > 1 || qAbs(after.f - before.f) > 1);
            const auto reopened = matchingTextForms(waitForCandidates(controller, 0), formText);
            QCOMPARE(reopened.size(), 2);
            const QRectF moved = reopened.constFirst().toMap().value(QStringLiteral("sourceRect")).toRectF();
            QVERIFY(QLineF(moved.topLeft(), sourceRect.translated(0.03, 0.15).topLeft()).length() < 0.002);
        } else if (operation == QLatin1String("content")) {
            const auto reopened = candidateForText(waitForCandidates(controller, 0), changedFormText);
            QVERIFY(reopened.value(QStringLiteral("editable")).toBool());
            QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
            controller.commitSelectedText(QStringLiteral("Edited again"));
            QVERIFY(controller.save());
            QVERIFY(normalizedPageText(output).contains(QStringLiteral("Edited again")));
            QCOMPARE(renderTextFormPage(output, 1), peerPage);
        }
    }
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    QCOMPARE(sourceFile.readAll(), sourceBytes);
}

void PDFEditControllerTest::keepsUnsafeTextFormsReadOnly_data()
{
    QTest::addColumn<QString>("fixture");
    for (const char *name : {"mixed-siblings", "marked-form", "marked-text", "outer-clip",
             "inner-clip", "bbox-clip", "transparency", "inherited-color", "stroke-text",
             "skewed-text", "deep-chain"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void PDFEditControllerTest::keepsUnsafeTextFormsReadOnly()
{
    QFETCH(QString, fixture);
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("unsafe-text-form-%1.pdf").arg(fixture));
    QByteArray pageContent("q 1 0 0 1 60 220 cm /Fm0 Do Q\n");
    if (fixture == QLatin1String("marked-form"))
        pageContent = "/Span BMC " + pageContent + " EMC\n";
    if (fixture == QLatin1String("outer-clip"))
        pageContent = "q 65 225 30 20 re W n " + pageContent + " Q\n";
    if (fixture == QLatin1String("inherited-color"))
        pageContent = "0 0 1 rg " + pageContent;
    const int depth = fixture == QLatin1String("deep-chain") ? 17 : 1;
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] "
        "/Resources << /XObject << /Fm0 6 0 R >> >> /Contents 4 0 R >>",
        textFormStream({}, pageContent),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    for (int index = 0; index < depth; ++index) {
        const bool leaf = index == depth - 1;
        QByteArray content = leaf ? QByteArray("BT /F0 12 Tf 1 0 0 1 4 10 Tm (Nested sample) Tj ET\n")
                                 : QByteArray("/Fm0 Do\n");
        if (leaf && fixture == QLatin1String("mixed-siblings"))
            content += "0 0 15 5 re f\n";
        if (leaf && fixture == QLatin1String("marked-text"))
            content = "/Span BMC " + content + " EMC\n";
        if (leaf && fixture == QLatin1String("inner-clip"))
            content = "q 0 0 35 30 re W n " + content + " Q\n";
        if (leaf && fixture == QLatin1String("stroke-text"))
            content.replace("12 Tf", "12 Tf 1 Tr");
        QByteArray dictionary = "/Type /XObject /Subtype /Form /BBox "
            + QByteArray(fixture == QLatin1String("bbox-clip") ? "[0 0 30 35] " : "[0 0 150 40] ");
        if (fixture == QLatin1String("transparency"))
            dictionary += "/Group << /S /Transparency /I true >> ";
        if (fixture == QLatin1String("skewed-text"))
            dictionary += "/Matrix [1 0.2 0 1 0 0] ";
        dictionary += leaf ? QByteArray("/Resources << /Font << /F0 5 0 R >> >>")
            : QByteArray("/Resources << /XObject << /Fm0 ") + QByteArray::number(index + 7) + " 0 R >> >>";
        objects.append(textFormStream(dictionary, content));
    }
    QVERIFY(writePdfObjects(path, objects));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(waitForCandidates(controller, 0), formText);
    QVERIFY2(!candidate.isEmpty(), qPrintable(fixture));
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!controller.selectedEditable());
    QVERIFY(!controller.selectedDeletable());
    const QRectF frame = controller.selectedFrameRect();
    controller.commitSelectedText(changedFormText);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(frame.translated(0.1, 0.1));
    controller.finishSelectedFrameResize();
    controller.deleteSelectedObject();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QCOMPARE(controller.selectedFrameRect(), frame);
}

void PDFEditControllerTest::previewsSharedTextFormMovesAndDeletion()
{
    if (QGuiApplication::screens().isEmpty())
        QSKIP("The text Form preview test needs a Qt Quick render screen.");
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("text-form-preview.pdf"));
    QVERIFY(createSharedTextFormPdf(source, true, 0, false));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    const QVariantMap candidate = matchingTextForms(waitForCandidates(controller, 0), formText).value(0).toMap();
    const QString id = candidate.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectCandidate(0, id));
    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(800, 800);
    TestablePDFPageItem pageItem(window.contentItem());
    pageItem.setSize(window.size());
    pageItem.setEditController(&controller);
    pageItem.setDocument(&document);
    pageItem.setPage(0);
    pageItem.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    const auto grab = [&]() {
        QElapsedTimer timer;
        timer.start();
        while ((!pageItem.draftTextureCurrent() || pageItem.m_renderPending) && timer.elapsed() < 5000)
            QTest::qWait(10);
        return pageItem.draftTextureCurrent() && !pageItem.m_renderPending ? window.grabWindow() : QImage();
    };
    const QImage original = grab();
    QVERIFY(!original.isNull());
    for (bool remove : {false, true}) {
        QVERIFY(controller.selectCandidate(0, id));
        if (remove) {
            controller.deleteSelectedObject();
        } else {
            controller.beginSelectedFrameResize();
            controller.updateSelectedFrameRect(controller.selectedFrameRect().translated(0.03, 0.15));
            controller.finishSelectedFrameResize();
        }
        const QImage preview = grab();
        QVERIFY(!preview.isNull());
        QVERIFY(changedPixelCount(original, preview, original.rect()) > 20);
        const QString output = source + (remove ? QStringLiteral(".deleted.pdf") : QStringLiteral(".moved.pdf"));
        const auto result = PDFSaveWriter::save(controller, output);
        QVERIFY2(result.succeeded(), qPrintable(result.detail));
        const QImage saved = renderTextFormPage(output, 0, preview.size());
        QVERIFY(changedPixelCount(saved, preview, saved.rect()) < saved.width() * saved.height() / 200);
        controller.undo();
        QCOMPARE(grab(), original);
        controller.redo();
        QCOMPARE(grab(), preview);
        controller.discardDrafts();
        QCOMPARE(grab(), original);
    }
}

void PDFEditControllerTest::editsExternalFormTextWithoutChangingNeighbors()
{
    const QString source = qEnvironmentVariable("TREPDF_TEST_FORM_TEXT_PDF");
    if (source.isEmpty())
        QSKIP("Set TREPDF_TEST_FORM_TEXT_PDF to the 500 Lines or Less PDF fixture.");
    QFile sourceFile(source);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray sourceBytes = sourceFile.readAll();
    sourceFile.close();
    const QString subtitle = QStringLiteral("Experienced programmers solve interesting problems");
    const QImage original = renderTextFormPage(source, 2, QSize(1000, 1300));
    const QImage firstPage = renderTextFormPage(source, 0);
    QVERIFY(!original.isNull());
    QVERIFY(!firstPage.isNull());
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    const int pages = document.pageCount();
    controller.setDocument(&document);
    const QVariantList candidates = waitForCandidates(controller, 2);
    const QVariantMap candidate = candidateForText(candidates, subtitle);
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(2, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY2(controller.selectedEditable(), qPrintable(controller.selectedReason()));
    const QRectF frame = controller.selectedFrameRect();
    // Pure movement needs no substitute font and must retain the original Form
    // and its embedded Museo subset, even though Qt cannot load that raw CFF.
    const auto originalMatrices = topLevelFormMatrices(source, 2);
    const QString originalFamily = controller.selectedFontFamily();
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(frame.translated(0, 0.03));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.draftedObjectMovesForPage(2).size(), 1);
    const QString movedOutput = m_temporaryDirectory.filePath(QStringLiteral("external-form-text-moved.pdf"));
    const auto moveResult = PDFSaveWriter::save(controller, movedOutput);
    QVERIFY2(moveResult.succeeded(), qPrintable(moveResult.detail));
    const auto movedMatrices = topLevelFormMatrices(movedOutput, 2);
    QCOMPARE(movedMatrices.size(), originalMatrices.size());
    QVERIFY(movedMatrices.size() > 1);
    QCOMPARE(movedMatrices.at(1).a, originalMatrices.at(1).a);
    QCOMPARE(movedMatrices.at(1).d, originalMatrices.at(1).d);
    QVERIFY(qAbs(movedMatrices.at(1).f - originalMatrices.at(1).f) > 1);
    QCOMPARE(normalizedPageText(movedOutput, 2), normalizedPageText(source, 2));
    QCOMPARE(renderTextFormPage(movedOutput, 0), firstPage);
    {
        PDFDocument movedDocument;
        PDFEditController movedController;
        QVERIFY(movedDocument.open(QUrl::fromLocalFile(movedOutput)));
        movedController.setDocument(&movedDocument);
        const auto movedCandidate = candidateForText(waitForCandidates(movedController, 2), subtitle);
        QVERIFY(movedController.selectCandidate(2, movedCandidate.value(QStringLiteral("objectId")).toString()));
        QCOMPARE(movedController.selectedFontFamily(), originalFamily);
    }
    controller.discardDrafts();
    QVERIFY(controller.selectCandidate(2, candidate.value(QStringLiteral("objectId")).toString()));
    // Exercise the real UI path with an explicitly selected available face.
    // This test must not bypass preflight to depend on missing Museo glyphs.
    const QString replacementFamily = QStringLiteral("Arial");
    QVERIFY(ApplicationFonts::fontFaceSupportsText(replacementFamily, false, false, subtitle));
    controller.setSelectedFontFamily(replacementFamily);
    QCOMPARE(controller.selectedFontFamily(), replacementFamily);
    QVERIFY(controller.requestSelectedTextEdit());
    const QString replacement = QStringLiteral("Experienced programmers solve practical problems");
    controller.commitSelectedText(replacement);
    const auto paths = controller.draftedTextObjectPathsForPage(2);
    QCOMPARE(paths.size(), 1);
    QCOMPARE(paths.constFirst().size(), 1);
    const QString output = m_temporaryDirectory.filePath(QStringLiteral("external-form-text-edited.pdf"));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(output)));
    QCOMPARE(document.pageCount(), pages);
    QVERIFY(normalizedPageText(output, 2).contains(replacement));
    QCOMPARE(renderTextFormPage(output, 0), firstPage);
    const QImage edited = renderTextFormPage(output, 2, original.size());
    QVERIFY(changedPixelCount(original, edited, original.rect()) > 20);
    for (const QVariant &value : candidates) {
        const QVariantMap neighbor = value.toMap();
        if (neighbor.value(QStringLiteral("objectId")) == candidate.value(QStringLiteral("objectId")))
            continue;
        const QRectF rect = neighbor.value(QStringLiteral("sourceRect")).toRectF();
        if (!rect.isEmpty() && !rect.intersects(frame.adjusted(-0.02, -0.02, 0.02, 0.02))) {
            const QRect pixels = textFormPixels(rect, original.size());
            QCOMPARE(edited.copy(pixels), original.copy(pixels));
        }
    }
    const QVariantMap reopened = candidateForText(waitForCandidates(controller, 2), replacement);
    QVERIFY(controller.selectCandidate(2, reopened.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.requestSelectedTextEdit());
    controller.commitSelectedText(QStringLiteral("Experienced programmers solve useful problems"));
    QVERIFY(controller.save());
    QVERIFY(normalizedPageText(output, 2).contains(QStringLiteral("Experienced programmers solve useful problems")));
    QCOMPARE(renderTextFormPage(output, 0), firstPage);
    const QString images = qEnvironmentVariable("TREPDF_TEST_FORM_TEXT_OUTPUT_DIR");
    if (!images.isEmpty()) {
        QVERIFY(QDir().mkpath(images));
        QVERIFY(original.save(QDir(images).filePath(QStringLiteral("form-text-original.png"))));
        QVERIFY(edited.save(QDir(images).filePath(QStringLiteral("form-text-edited.png"))));
        QVERIFY(renderTextFormPage(output, 2, original.size()).save(
            QDir(images).filePath(QStringLiteral("form-text-edited-again.png"))));
    }
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    QCOMPARE(sourceFile.readAll(), sourceBytes);
}
