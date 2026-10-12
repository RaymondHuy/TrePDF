#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QClipboard>
#include <QMimeData>

using namespace PDFEditControllerTestSupport;

namespace {

QByteArray imageFixtureStream(const QByteArray &dictionary, const QByteArray &content)
{
    return QByteArray("<< ") + dictionary + " /Length "
        + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream";
}

bool createSharedNestedImagePdf(const QString &path, bool nested, int rotation,
                               bool offsetCrop, bool internalClip = false,
                               bool inheritedResources = false,
                               bool coloredBackdrop = false)
{
    const QByteArray geometry = QByteArray("/MediaBox [0 0 400 400] /Rotate ")
        + QByteArray::number(rotation * 90)
        + (offsetCrop ? " /CropBox [20 20 380 380] " : " ");
    const QByteArray firstContent = (coloredBackdrop
        ? QByteArray("q 0.7 0.1 0.8 rg 0 0 400 400 re f Q\n") : QByteArray()) + QByteArray(
        "q 1 0 0 1 50 270 cm /Fm0 Do Q\n"
        "q 1 0 0 1 230 270 cm /Fm0 Do Q\n");
    const QByteArray secondContent("q 1 0 0 1 90 150 cm /Fm0 Do Q\n");
    const QByteArray imageContent = QByteArray("q ")
        + (internalClip ? "0 0 60 45 re W n " : "")
        + "90 0 0 50 0 0 cm /Im0 Do Q\n";
    const QByteArray pageResources = QByteArray("/Resources << /XObject << /Fm0 9 0 R ")
        + (inheritedResources ? "/Im0 7 0 R " : "") + ">> >> ";
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        QByteArray("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 ")
            + (inheritedResources ? pageResources : QByteArray()) + ">>",
        QByteArray("<< /Type /Page /Parent 2 0 R ") + geometry
            + (inheritedResources ? QByteArray() : pageResources) + "/Contents 5 0 R >>",
        QByteArray("<< /Type /Page /Parent 2 0 R ") + geometry
            + (inheritedResources ? QByteArray() : pageResources) + "/Contents 6 0 R >>",
        imageFixtureStream({}, firstContent),
        imageFixtureStream({}, secondContent),
        imageFixtureStream(
            "/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            "/ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask 8 0 R",
            QByteArray::fromHex("ff000000ff000000ffffff00")),
        imageFixtureStream(
            "/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            "/ColorSpace /DeviceGray /BitsPerComponent 8",
            QByteArray::fromHex("ffb060ff")),
        imageFixtureStream(
            QByteArray("/Type /XObject /Subtype /Form /BBox [0 0 100 70] ")
                + "/Matrix [1 0.05 0.1 1 0 0] "
                + (inheritedResources ? QByteArray()
                    : QByteArray("/Resources << /XObject << ")
                        + (nested ? "/Inner 10 0 R" : "/Im0 7 0 R") + " >> >>"),
            nested ? QByteArray("q /Inner Do Q\n") : imageContent),
    };
    if (nested) {
        objects.append(imageFixtureStream(
            "/Type /XObject /Subtype /Form /BBox [0 0 100 70] "
            "/Matrix [1 0 0 1 2 3] /Resources << /XObject << /Im0 7 0 R >> >>",
            imageContent));
    }
    return writePdfObjects(path, objects);
}

QImage renderNestedImagePage(const QString &path, int pageIndex,
                            const QSize &size = QSize(800, 800), const QByteArray &password = {})
{
    PDFiumProcessLock pdfiumLock;
    const QByteArray encodedPath = QFile::encodeName(path);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encodedPath.constData(), password.constData());
    FPDF_PAGE page = document ? FPDF_LoadPage(document, pageIndex) : nullptr;
    QImage result(size, QImage::Format_ARGB32);
    result.fill(Qt::white);
    FPDF_BITMAP bitmap = page ? FPDFBitmap_CreateEx(
        size.width(), size.height(), FPDFBitmap_BGRA,
        result.bits(), result.bytesPerLine()) : nullptr;
    if (bitmap) {
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, size.width(), size.height(), 0, FPDF_ANNOT);
        FPDFBitmap_Destroy(bitmap);
    } else {
        result = {};
    }
    if (page)
        FPDF_ClosePage(page);
    if (document)
        FPDF_CloseDocument(document);
    return result;
}

QVariantList imageCandidates(const QVariantList &candidates)
{
    QVariantList images;
    for (const QVariant &value : candidates) {
        if (value.toMap().value(QStringLiteral("kind")).toString() == QLatin1String("image"))
            images.append(value);
    }
    return images;
}

QRect imagePixelRect(const QRectF &frame, const QSize &size)
{
    return QRectF(frame.x() * size.width(), frame.y() * size.height(),
                  frame.width() * size.width(), frame.height() * size.height())
        .toAlignedRect().adjusted(-2, -2, 2, 2).intersected(QRect(QPoint(), size));
}

} // namespace

void PDFEditControllerTest::editsSharedNestedImagesRoundTrip_data()
{
    QTest::addColumn<bool>("nested");
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("offsetCrop");
    QTest::addColumn<bool>("internalClip");
    QTest::addColumn<bool>("inheritedResources");
    QTest::newRow("single-form") << false << 0 << false << false << false;
    QTest::newRow("form-chain") << true << 0 << false << false << false;
    QTest::newRow("chain-cropped-90") << true << 1 << true << false << false;
    QTest::newRow("chain-cropped-270") << true << 3 << true << false << false;
    QTest::newRow("chain-internal-clip") << true << 0 << false << true << false;
    QTest::newRow("inherited-resources") << false << 0 << false << false << true;
}

void PDFEditControllerTest::editsSharedNestedImagesRoundTrip()
{
    QFETCH(bool, nested);
    QFETCH(int, rotation);
    QFETCH(bool, offsetCrop);
    QFETCH(bool, internalClip);
    QFETCH(bool, inheritedResources);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("shared-nested-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    QVERIFY(createSharedNestedImagePdf(sourcePath, nested, rotation, offsetCrop,
                                       internalClip, inheritedResources));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = sourceFile.readAll();
    sourceFile.close();
    const QImage original = renderNestedImagePage(sourcePath, 0);
    const QImage originalPeerPage = renderNestedImagePage(sourcePath, 1);
    QVERIFY(!original.isNull());
    QVERIFY(!originalPeerPage.isNull());

    for (const QString &operation : {QStringLiteral("move"), QStringLiteral("resize"),
                                     QStringLiteral("rotate"), QStringLiteral("delete")}) {
        PDFDocument document;
        PDFEditController controller;
        QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
        controller.setDocument(&document);
        const QVariantList images = imageCandidates(waitForCandidates(controller, 0));
        QCOMPARE(images.size(), 2);
        const QVariantMap candidate = images.constFirst().toMap();
        const QString objectId = candidate.value(QStringLiteral("objectId")).toString();
        QVERIFY(controller.selectCandidate(0, objectId));
        QVERIFY(controller.selectedEditable());
        QVERIFY(controller.selectedImageResizable());
        QVERIFY(controller.selectedImageRotatable());
        QVERIFY(controller.selectedDeletable());
        const QRectF sourceFrame = controller.selectedFrameRect();
        const QRectF peerFrame = images.at(1).toMap().value(QStringLiteral("sourceRect")).toRectF();
        const QRect peerPixels = imagePixelRect(peerFrame, original.size());
        const auto apply = [&]() {
            if (operation == QLatin1String("delete")) {
                controller.deleteSelectedObject();
            } else if (operation == QLatin1String("rotate")) {
                controller.rotateSelectedImage(1);
            } else {
                const QSizeF size = operation == QLatin1String("resize")
                    ? QSizeF(sourceFrame.width() * 0.7, sourceFrame.height() * 1.2)
                    : sourceFrame.size();
                controller.beginSelectedFrameResize();
                controller.updateSelectedFrameRect(QRectF(QPointF(0.4, 0.4), size));
                controller.finishSelectedFrameResize();
            }
        };
        apply();
        QVERIFY(controller.hasDrafts());
        const QRectF expectedFrame = controller.selectedFrameRect();
        if (operation != QLatin1String("delete")) {
            const auto moves = controller.draftedObjectMovesForPage(0);
            QCOMPARE(moves.size(), 1);
            QCOMPARE(moves.constFirst().objectPath.size(), 1);
            QVERIFY(!expectedFrame.intersects(peerFrame));
        }
        controller.undo();
        QVERIFY(!controller.hasDrafts());
        QVERIFY(controller.selectCandidate(0, objectId));
        QCOMPARE(controller.selectedFrameRect(), sourceFrame);
        controller.redo();
        QVERIFY(controller.hasDrafts());
        controller.discardDrafts();
        QVERIFY(!controller.hasDrafts());
        QVERIFY(controller.selectCandidate(0, objectId));
        apply();
        const QString outputPath = sourcePath + QLatin1Char('.') + operation + QStringLiteral(".pdf");
        QVERIFY2(controller.saveAs(QUrl::fromLocalFile(outputPath)), qPrintable(operation));
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
        const QImage saved = renderNestedImagePage(outputPath, 0);
        QVERIFY(!saved.isNull());
        QVERIFY(changedPixelCount(original, saved, original.rect()) > 100);
        QCOMPARE(saved.copy(peerPixels), original.copy(peerPixels));
        QCOMPARE(renderNestedImagePage(outputPath, 1), originalPeerPage);
        const QVariantList reopened = imageCandidates(waitForCandidates(controller, 0));
        QCOMPARE(reopened.size(), operation == QLatin1String("delete") ? 1 : 2);
        if (operation != QLatin1String("delete")) {
            const QVariantMap edited = reopened.constFirst().toMap();
            const QRectF reopenedFrame = edited.value(QStringLiteral("sourceRect")).toRectF();
            QVERIFY(QLineF(reopenedFrame.topLeft(), expectedFrame.topLeft()).length() < 0.003);
            QVERIFY(QLineF(reopenedFrame.bottomRight(), expectedFrame.bottomRight()).length() < 0.003);
            QVERIFY(edited.value(QStringLiteral("imageResizable")).toBool());
        }
    }
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    QCOMPARE(sourceFile.readAll(), originalBytes);
}

void PDFEditControllerTest::keepsUnsafeNestedImagesReadOnly_data()
{
    QTest::addColumn<QString>("fixture");
    for (const char *fixture : {"outer-clip", "transparency-group", "marked-wrapper",
                                "marked-image", "deep-chain", "mixed-siblings", "stencil-paint"})
        QTest::newRow(fixture) << QString::fromLatin1(fixture);
}

void PDFEditControllerTest::keepsUnsafeNestedImagesReadOnly()
{
    QFETCH(QString, fixture);
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("unsafe-nested-%1.pdf").arg(fixture));
    const int depth = fixture == QLatin1String("deep-chain") ? 17 : 1;
    const QByteArray pageContent = fixture == QLatin1String("outer-clip")
        ? QByteArray("q 110 120 80 30 re W n /Fm0 Do Q\n")
        : fixture == QLatin1String("marked-wrapper")
            ? QByteArray("/Span BMC /Fm0 Do EMC\n")
            : fixture == QLatin1String("stencil-paint")
                ? QByteArray("0 1 1 0 k /Fm0 Do\n") : QByteArray("/Fm0 Do\n");
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] "
        "/Resources << /XObject << /Fm0 6 0 R >> >> /Contents 4 0 R >>",
        imageFixtureStream({}, pageContent),
        imageFixtureStream(
            "/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            "/ColorSpace /DeviceRGB /BitsPerComponent 8",
            QByteArray::fromHex("ff000000ff000000ffffff00")),
    };
    if (fixture == QLatin1String("stencil-paint")) {
        objects[4] = imageFixtureStream(
            "/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            "/ImageMask true /BitsPerComponent 1", QByteArray::fromHex("4080"));
    }
    for (int index = 0; index < depth; ++index) {
        const bool leaf = index == depth - 1;
        QByteArray content = leaf ? QByteArray("q 120 0 0 60 100 100 cm /Im0 Do Q\n")
                                  : QByteArray("/Fm0 Do\n");
        if (leaf && fixture == QLatin1String("marked-image"))
            content = "/Span BMC " + content + " EMC\n";
        if (leaf && fixture == QLatin1String("mixed-siblings"))
            content += "0 0 20 20 re f\n";
        QByteArray dictionary("/Type /XObject /Subtype /Form /BBox [0 0 400 400] ");
        if (fixture == QLatin1String("transparency-group"))
            dictionary += "/Group << /S /Transparency /I true >> ";
        dictionary += "/Resources << /XObject << ";
        dictionary += leaf ? QByteArray("/Im0 5 0 R")
            : QByteArray("/Fm0 ") + QByteArray::number(7 + index) + " 0 R";
        dictionary += " >> >>";
        objects.append(imageFixtureStream(dictionary, content));
    }
    QVERIFY(writePdfObjects(path, objects));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!controller.selectedEditable());
    QVERIFY(!controller.selectedDeletable());
    QVERIFY(!controller.selectedImageResizable());
    QVERIFY(!controller.selectedImageRotatable());
    const QRectF original = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.2, 0.2, 0.3, 0.3));
    controller.finishSelectedFrameResize();
    controller.rotateSelectedImage(1);
    controller.deleteSelectedObject();
    QCOMPARE(controller.selectedFrameRect(), original);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::previewsSharedNestedImageChanges()
{
    if (QGuiApplication::screens().isEmpty())
        QSKIP("The nested image preview test needs a Qt Quick render screen.");
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("shared-nested-preview.pdf"));
    QVERIFY(createSharedNestedImagePdf(sourcePath, true, 0, false));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    const QString objectId = candidate.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectCandidate(0, objectId));
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
    const auto grabPage = [&]() {
        QElapsedTimer timer;
        timer.start();
        while ((!pageItem.draftTextureCurrent() || pageItem.m_renderPending)
               && timer.elapsed() < 5000)
            QTest::qWait(10);
        return pageItem.draftTextureCurrent() && !pageItem.m_renderPending
            ? window.grabWindow() : QImage();
    };
    const QImage original = grabPage();
    QVERIFY(!original.isNull());
    const QImage peerPage = renderNestedImagePage(sourcePath, 1);
    for (const QString &operation : {QStringLiteral("move"), QStringLiteral("resize"),
                                     QStringLiteral("rotate"), QStringLiteral("delete")}) {
        QVERIFY(controller.selectCandidate(0, objectId));
        if (operation == QLatin1String("delete")) {
            controller.deleteSelectedObject();
        } else if (operation == QLatin1String("rotate")) {
            controller.rotateSelectedImage(1);
        } else {
            const QRectF frame = controller.selectedFrameRect();
            controller.beginSelectedFrameResize();
            controller.updateSelectedFrameRect(QRectF(QPointF(0.4, 0.4),
                operation == QLatin1String("resize") ? frame.size() * 0.7 : frame.size()));
            controller.finishSelectedFrameResize();
        }
        const QImage preview = grabPage();
        QVERIFY(!preview.isNull());
        QVERIFY(changedPixelCount(original, preview, original.rect()) > 100);
        const QString outputPath = sourcePath + QLatin1Char('.') + operation + QStringLiteral(".pdf");
        const auto result = PDFSaveWriter::save(controller, outputPath);
        QVERIFY2(result.succeeded(), qPrintable(result.detail));
        const QImage saved = renderNestedImagePage(outputPath, 0, preview.size());
        QVERIFY(changedPixelCount(saved, preview, saved.rect()) < saved.width() * saved.height() / 200);
        QCOMPARE(renderNestedImagePage(outputPath, 1), peerPage);
        controller.undo();
        QCOMPARE(grabPage(), original);
        controller.redo();
        QCOMPARE(grabPage(), preview);
        controller.discardDrafts();
        QCOMPARE(grabPage(), original);
    }
}

void PDFEditControllerTest::preservesNestedImageResourcesWithPageChangesAndPasswords()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("nested-resources-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("nested-resources-protected.pdf"));
    QVERIFY(createSharedNestedImagePdf(sourcePath, false, 0, false, false, true));
    const QImage original = renderNestedImagePage(sourcePath, 1);
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(waitForCandidates(controller, 1), QStringLiteral("image"));
    const QRectF originalFrame = candidate.value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(controller.selectCandidate(1, candidate.value(QStringLiteral("objectId")).toString()));
    controller.rotateSelectedImage(1);
    QVERIFY(controller.hasDrafts());
    // Source page 1 becomes output page 1 via an insertion followed by deletion
    // of a different source page. The blank output page must stay blank.
    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);
    document.setCurrentPage(0);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    const QString password = QStringLiteral("image-form-test-password");
    QVERIFY(controller.setOpenPassword(password));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.documentPassword(), password.toUtf8());
    QVERIFY(renderNestedImagePage(outputPath, 1).isNull());
    const QImage rotated = renderNestedImagePage(outputPath, 1, QSize(800, 800), password.toUtf8());
    QVERIFY(!rotated.isNull());
    QVERIFY(changedPixelCount(original, rotated, original.rect()) > 100);
    QImage white(800, 800, QImage::Format_ARGB32);
    white.fill(Qt::white);
    QCOMPARE(renderNestedImagePage(outputPath, 0, white.size(), password.toUtf8()), white);
    QVERIFY(waitForCandidates(controller, 0).isEmpty());
    const QVariantMap reopened = candidateForKind(waitForCandidates(controller, 1), QStringLiteral("image"));
    QVERIFY(controller.selectCandidate(1, reopened.value(QStringLiteral("objectId")).toString()));
    controller.rotateSelectedImage(-1);
    // Ordinary encrypted Save must also preserve the resource fallback.
    QVERIFY(controller.save());
    const QImage restored = renderNestedImagePage(outputPath, 1, original.size(), password.toUtf8());
    QVERIFY(!restored.isNull());
    QCOMPARE(restored.size(), original.size());
    // Two inverse matrix round-trips can change subpixel antialiasing slightly.
    // Require both tight geometry and bounded edge-only raster differences.
    int maxDifference = 0;
    int differentPixels = 0;
    for (int y = 0; y < original.height(); ++y) {
        for (int x = 0; x < original.width(); ++x) {
            const QColor a = original.pixelColor(x, y);
            const QColor b = restored.pixelColor(x, y);
            const int difference = qMax(qMax(qAbs(a.red() - b.red()), qAbs(a.green() - b.green())),
                                       qMax(qAbs(a.blue() - b.blue()), qAbs(a.alpha() - b.alpha())));
            maxDifference = qMax(maxDifference, difference);
            differentPixels += difference != 0;
        }
    }
    QVERIFY(maxDifference <= 4);
    QVERIFY(differentPixels < original.width() * original.height() / 1000);
    QVERIFY(renderNestedImagePage(outputPath, 1).isNull());

    const QVariantMap saved = candidateForKind(waitForCandidates(controller, 1), QStringLiteral("image"));
    const QRectF savedFrame = saved.value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(QLineF(savedFrame.topLeft(), originalFrame.topLeft()).length() < 0.00001);
    QVERIFY(QLineF(savedFrame.bottomRight(), originalFrame.bottomRight()).length() < 0.00001);
    QVERIFY(controller.selectCandidate(1, saved.value(QStringLiteral("objectId")).toString()));
    controller.rotateSelectedImage(1);
    const QString failurePath = m_temporaryDirectory.filePath(QStringLiteral("nested-resources-target-sentinel.pdf"));
    QFile sentinel(failurePath);
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    QCOMPARE(sentinel.write("destination unchanged"), qint64(21));
    sentinel.close();
    const PDFSaveWriter::Request request = PDFSaveWriter::prepare(controller, failurePath,
        m_temporaryDirectory.filePath(QStringLiteral("missing-source.pdf")));
    QVERIFY(!PDFSaveWriter::save(request).succeeded());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(sentinel.open(QIODevice::ReadOnly));
    QCOMPARE(sentinel.readAll(), QByteArray("destination unchanged"));
}

void PDFEditControllerTest::preservesNestedImageResourcesWhenAddingImage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("nested-neighbor-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("nested-neighbor-saved.pdf"));
    QVERIFY(createSharedNestedImagePdf(sourcePath, false, 0, false, false, true));
    const QImage original = renderNestedImagePage(sourcePath, 0);
    const QImage peerPage = renderNestedImagePage(sourcePath, 1);
    QVERIFY(!original.isNull());
    QVERIFY(!peerPage.isNull());
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantList originals = imageCandidates(waitForCandidates(controller, 0));
    QCOMPARE(originals.size(), 2);
    QImage image(40, 20, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    const QVariantMap added = controller.addImageData(0, 0.5, 0.75, image, 60);
    QVERIFY(!added.isEmpty());
    const QRectF addedFrame = added.value(QStringLiteral("frameRect")).toRectF();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const QImage withNeighbor = renderNestedImagePage(outputPath, 0);
    QVERIFY(!withNeighbor.isNull());
    for (const auto &entry : originals) {
        const QRectF frame = entry.toMap().value(QStringLiteral("sourceRect")).toRectF();
        QVERIFY(!frame.intersects(addedFrame));
        const QRect pixels = imagePixelRect(frame, original.size());
        QCOMPARE(withNeighbor.copy(pixels), original.copy(pixels));
    }
    QCOMPARE(renderNestedImagePage(outputPath, 1), peerPage);
    const QPoint center(qRound(addedFrame.center().x() * withNeighbor.width()),
                        qRound(addedFrame.center().y() * withNeighbor.height()));
    QCOMPARE(withNeighbor.pixelColor(center), QColor(Qt::blue));
    const QVariantList reopened = imageCandidates(waitForCandidates(controller, 0));
    QCOMPARE(reopened.size(), 3);
    const auto neighbor = std::find_if(reopened.cbegin(), reopened.cend(), [&addedFrame](const auto &entry) {
        return entry.toMap().value(QStringLiteral("sourceRect")).toRectF().intersects(addedFrame);
    });
    QVERIFY(neighbor != reopened.cend());
    QVERIFY(controller.selectCandidate(0, neighbor->toMap().value(QStringLiteral("objectId")).toString()));
    controller.deleteSelectedObject();
    QVERIFY(controller.save());
    QCOMPARE(renderNestedImagePage(outputPath, 0), original);
    QCOMPARE(renderNestedImagePage(outputPath, 1), peerPage);
}

void PDFEditControllerTest::editsExternalNestedImageWithoutChangingSharedPage()
{
    const QString sourcePath = qEnvironmentVariable("TREPDF_TEST_NESTED_IMAGE_PDF");
    if (sourcePath.isEmpty())
        QSKIP("Set TREPDF_TEST_NESTED_IMAGE_PDF to the 500 Lines or Less PDF fixture.");
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray sourceBytes = sourceFile.readAll();
    sourceFile.close();
    const QImage original = renderNestedImagePage(sourcePath, 0);
    const QImage thirdPage = renderNestedImagePage(sourcePath, 2);
    QVERIFY(!original.isNull());
    QVERIFY(!thirdPage.isNull());
    const QString outputDirectory = qEnvironmentVariable("TREPDF_TEST_NESTED_IMAGE_OUTPUT_DIR");
    if (!outputDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(outputDirectory));
        QVERIFY(original.save(QDir(outputDirectory).filePath(QStringLiteral("original.png"))));
    }
    for (const QString &operation : {QStringLiteral("move"), QStringLiteral("resize"),
                                     QStringLiteral("rotate"), QStringLiteral("delete")}) {
        PDFDocument document;
        PDFEditController controller;
        QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
        const int sourcePageCount = document.pageCount();
        controller.setDocument(&document);
        const QVariantList candidates = imageCandidates(waitForCandidates(controller, 0));
        QCOMPARE(candidates.size(), 1);
        const QVariantMap candidate = candidates.constFirst().toMap();
        QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
        QVERIFY(controller.selectedImageResizable());
        QVERIFY(controller.selectedImageRotatable());
        if (operation == QLatin1String("delete")) {
            controller.deleteSelectedObject();
        } else if (operation == QLatin1String("rotate")) {
            controller.rotateSelectedImage(1);
        } else {
            const QRectF frame = controller.selectedFrameRect();
            controller.beginSelectedFrameResize();
            controller.updateSelectedFrameRect(QRectF(QPointF(frame.x(), 0.65),
                operation == QLatin1String("resize") ? frame.size() * 0.7 : frame.size()));
            controller.finishSelectedFrameResize();
        }
        QVERIFY(controller.hasDrafts());
        const QString outputPath = m_temporaryDirectory.filePath(
            QStringLiteral("external-nested-%1.pdf").arg(operation));
        QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
        QCOMPARE(document.pageCount(), sourcePageCount);
        QCOMPARE(renderNestedImagePage(outputPath, 2), thirdPage);
        const QImage saved = renderNestedImagePage(outputPath, 0);
        if (!outputDirectory.isEmpty()) {
            QVERIFY(saved.save(QDir(outputDirectory).filePath(operation + QStringLiteral(".png"))));
        }
        QVERIFY(changedPixelCount(original, saved, original.rect()) > 100);
        QCOMPARE(imageCandidates(waitForCandidates(controller, 0)).size(),
                 operation == QLatin1String("delete") ? 0 : 1);
    }
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    QCOMPARE(sourceFile.readAll(), sourceBytes);
}

void PDFEditControllerTest::rendersAddedImageSnapshot()
{
    QImage source(8, 8, QImage::Format_ARGB32);
    source.fill(QColor(QStringLiteral("#2b5fd8")));
    for (int y = 0; y < source.height() / 2; ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor(QStringLiteral("#d86f4b")));
        }
    }

    DraftImageItem item;
    item.setWidth(32);
    item.setHeight(32);
    item.setSourceImage(QVariant::fromValue(source));
    source.fill(Qt::green);

    QImage rendered(32, 32, QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    item.paint(&painter);
    painter.end();

    const QColor topPixel = rendered.pixelColor(16, 4);
    const QColor bottomPixel = rendered.pixelColor(16, 28);
    QVERIFY(topPixel.red() > topPixel.blue());
    QVERIFY(bottomPixel.blue() > bottomPixel.red());
}

void PDFEditControllerTest::addsImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-image-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-image-preview.png"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#4b8fd8")));
    QVERIFY(image.save(imagePath, "PNG"));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::AddImageTool);

    const QPointF insertionPoint(0.2, 0.65);
    const QVariantMap added = controller.addImage(
        0,
        insertionPoint.x(),
        insertionPoint.y(),
        QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    QCOMPARE(added.value(QStringLiteral("kind")).toString(), QStringLiteral("addedImage"));
    QCOMPARE(added.value(QStringLiteral("added")).toBool(), true);
    QCOMPARE(added.value(QStringLiteral("imagePixelWidth")).toInt(), 80);
    QCOMPARE(added.value(QStringLiteral("imagePixelHeight")).toInt(), 40);
    QCOMPARE(controller.tool(), PDFEditController::SelectTool);
    const QString objectId = added.value(QStringLiteral("objectId")).toString();
    const QRectF frame = added.value(QStringLiteral("frameRect")).toRectF();
    QVERIFY(frame.contains(insertionPoint));
    QVERIFY(frame.left() >= 0.0);
    QVERIFY(frame.top() >= 0.0);
    QVERIFY(frame.right() <= 1.0);
    QVERIFY(frame.bottom() <= 1.0);
    const QSizeF pageSize = document.pageSize(0);
    const qreal displayedAspect = frame.width() * pageSize.width()
        / (frame.height() * pageSize.height());
    QVERIFY(qAbs(displayedAspect - 2.0) < 0.001);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.textEditActive());
    QCOMPARE(
        controller.candidateAt(0, frame.center().x(), frame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);

    controller.undo();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());
    const QVariantList afterUndo = waitForCandidates(controller, 0);
    QVERIFY(std::none_of(
        afterUndo.cbegin(),
        afterUndo.cend(),
        [&objectId](const QVariant &value) {
            return value.toMap().value(QStringLiteral("objectId")).toString()
                == objectId;
        }));

    controller.redo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    const QVariantList afterRedo = waitForCandidates(controller, 0);
    const auto restored = std::find_if(
        afterRedo.cbegin(),
        afterRedo.cend(),
        [&objectId](const QVariant &value) {
            return value.toMap().value(QStringLiteral("objectId")).toString()
                == objectId;
        });
    QVERIFY(restored != afterRedo.cend());
    QCOMPARE(
        restored->toMap().value(QStringLiteral("frameRect")).toRectF(),
        frame);

    QVERIFY(controller.selectCandidate(0, objectId));
    const QRectF resizedFrame(
        frame.x() + 0.01,
        frame.y() + 0.01,
        frame.width() * 0.8,
        frame.height() * 0.8);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
    controller.finishSelectedFrameResize();
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
}

void PDFEditControllerTest::copiesAndPastesAddedImagesWithHistoryAndSave()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard);
    clipboard->clear();
    const auto clearClipboard = qScopeGuard([clipboard]() { clipboard->clear(); });
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("clipboard-added-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("clipboard-added-saved.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    QPainter imagePainter(&image);
    imagePainter.fillRect(QRect(0, 0, 40, 20), Qt::red);
    imagePainter.fillRect(QRect(40, 0, 40, 20), Qt::green);
    imagePainter.fillRect(QRect(0, 20, 40, 20), Qt::blue);
    imagePainter.fillRect(QRect(40, 20, 40, 20), Qt::yellow);
    imagePainter.end();

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(!controller.addImageData(0, 0.35, 0.45, image).isEmpty());
    const QString originalId = controller.selectedObjectId();
    controller.setImageAspectRatioLocked(false);
    const QRectF initialFrame = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(initialFrame.topLeft(),
        QSizeF(initialFrame.width() * 0.7, initialFrame.height() * 1.3)));
    controller.finishSelectedFrameResize();
    controller.rotateSelectedImage(1);
    const QRectF copiedFrame = controller.selectedFrameRect();
    const QVariantList beforeCopy = controller.draftsForPage(0);
    QVERIFY(controller.selectedImageCopyable());
    QVERIFY(controller.copySelectedImage());
    QTRY_VERIFY_WITH_TIMEOUT(controller.clipboardHasImage(), 5000);
    QCOMPARE(controller.draftsForPage(0), beforeCopy);
    QCOMPARE(controller.selectedObjectId(), originalId);
    const QImage copied = clipboard->image();
    QVERIFY(!copied.isNull());
    const QImage rotated = image.transformed(QTransform().rotate(90));
    const QSizeF pageSize = document.pageSize(0);
    const qreal copiedAspect = copiedFrame.width() * pageSize.width()
        / (copiedFrame.height() * pageSize.height());
    QVERIFY(qAbs(qreal(copied.width()) / copied.height() - copiedAspect) < 0.03);
    for (const QPointF point : {QPointF(0.25, 0.25), QPointF(0.75, 0.25),
                               QPointF(0.25, 0.75), QPointF(0.75, 0.75)}) {
        QCOMPARE(copied.pixelColor(int(point.x() * copied.width()), int(point.y() * copied.height())),
                 rotated.pixelColor(int(point.x() * rotated.width()), int(point.y() * rotated.height())));
    }

    const QVariantMap pasted = controller.pasteImageFromClipboard(0);
    QVERIFY(!pasted.isEmpty());
    const QString pastedId = pasted.value(QStringLiteral("objectId")).toString();
    QVERIFY(pastedId != originalId);
    const QRectF pastedFrame = controller.selectedFrameRect();
    QVERIFY(qAbs(pastedFrame.width() - copiedFrame.width()) < 0.000001);
    QVERIFY(qAbs(pastedFrame.height() - copiedFrame.height()) < 0.000001);
    QVERIFY(qAbs((pastedFrame.x() - copiedFrame.x()) * pageSize.width() - 12.0) < 0.001);
    QVERIFY(qAbs((pastedFrame.y() - copiedFrame.y()) * pageSize.height() - 12.0) < 0.001);
    QCOMPARE(controller.draftsForPage(0).size(), 2);
    controller.undo();
    QCOMPARE(controller.draftsForPage(0), beforeCopy);
    controller.redo();
    QCOMPARE(controller.draftsForPage(0).size(), 2);
    QVERIFY(controller.selectCandidate(0, pastedId));
    QCOMPARE(controller.selectedFrameRect(), pastedFrame);

    // Removing the original also proves the clipboard owns its pixels.
    QVERIFY(controller.selectCandidate(0, originalId));
    controller.deleteSelectedObject();
    clipboard->clear();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(inspectImages(sourcePath).count, 0);
    QCOMPARE(inspectImages(outputPath).count, 1);
    const QVariantList reopened = imageCandidates(waitForCandidates(controller, 0));
    QCOMPARE(reopened.size(), 1);
    const QRectF savedFrame = reopened.constFirst().toMap().value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(QLineF(savedFrame.topLeft(), pastedFrame.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(savedFrame.bottomRight(), pastedFrame.bottomRight()).length() < 0.0001);
    const QImage saved = renderFirstPage(outputPath, QSize(1190, 1684));
    QVERIFY(!saved.isNull());
    for (const QPointF point : {QPointF(0.25, 0.25), QPointF(0.75, 0.25),
                               QPointF(0.25, 0.75), QPointF(0.75, 0.75)}) {
        QCOMPARE(saved.pixelColor(qRound((pastedFrame.x() + point.x() * pastedFrame.width()) * saved.width()),
                                  qRound((pastedFrame.y() + point.y() * pastedFrame.height()) * saved.height())),
                 rotated.pixelColor(int(point.x() * rotated.width()), int(point.y() * rotated.height())));
    }
}

void PDFEditControllerTest::copiesSourceImageAppearanceWithoutChangingPdf_data()
{
    QTest::addColumn<bool>("topLevel");
    QTest::addColumn<bool>("nested");
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("clipped");
    QTest::newRow("top-level") << true << false << 0 << false;
    QTest::newRow("top-level-cropped-90") << true << false << 1 << false;
    QTest::newRow("masked-form") << false << false << 0 << false;
    QTest::newRow("masked-form-chain") << false << true << 0 << false;
    QTest::newRow("masked-clipped-chain-cropped-90") << false << true << 1 << true;
}

void PDFEditControllerTest::copiesSourceImageAppearanceWithoutChangingPdf()
{
    QFETCH(bool, topLevel);
    QFETCH(bool, nested);
    QFETCH(int, rotation);
    QFETCH(bool, clipped);
    QClipboard *clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard);
    clipboard->clear();
    const auto clearClipboard = qScopeGuard([clipboard]() { clipboard->clear(); });
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("clipboard-source-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString referencePath = m_temporaryDirectory.filePath(
        QStringLiteral("clipboard-reference-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    if (topLevel) {
        QVERIFY(createTopLevelImagePdf(sourcePath, {-120, 20, 15, 60, 420, 500}, rotation, rotation != 0));
        QVERIFY(QFile::copy(sourcePath, referencePath));
    } else {
        // The colored page artwork must not leak into the copied image's mask.
        QVERIFY(createSharedNestedImagePdf(sourcePath, nested, rotation, rotation != 0, clipped, false, true));
        QVERIFY(createSharedNestedImagePdf(referencePath, nested, rotation, rotation != 0, clipped));
    }
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = sourceFile.readAll();
    sourceFile.close();
    const QImage originalPage = renderNestedImagePage(sourcePath, 0, QSize(1600, 1600));
    const QImage referencePage = renderNestedImagePage(referencePath, 0, QSize(1600, 1600));
    QVERIFY(!originalPage.isNull());
    QVERIFY(!referencePage.isNull());

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantList candidates = imageCandidates(waitForCandidates(controller, 0));
    QCOMPARE(candidates.size(), topLevel ? 1 : 2);
    QVERIFY(controller.selectCandidate(0, candidates.constFirst().toMap().value(QStringLiteral("objectId")).toString()));
    const QRectF frame = controller.selectedFrameRect();
    QVERIFY(controller.selectedImageCopyable());
    QVERIFY(controller.copySelectedImage());
    QTRY_VERIFY_WITH_TIMEOUT(controller.clipboardHasImage(), 10000);
    const QImage copied = clipboard->image();
    QVERIFY(!copied.isNull());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QCOMPARE(controller.selectedFrameRect(), frame);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    QCOMPARE(sourceFile.readAll(), originalBytes);
    sourceFile.close();
    QCOMPARE(renderNestedImagePage(sourcePath, 0, originalPage.size()), originalPage);

    QImage overWhite(copied.size(), QImage::Format_ARGB32);
    overWhite.fill(Qt::white);
    QPainter painter(&overWhite);
    painter.drawImage(QPoint(), copied);
    painter.end();
    bool hasPartialAlpha = false;
    for (int y = 0; y < copied.height() && !hasPartialAlpha; ++y) {
        for (int x = 0; x < copied.width(); ++x) {
            const int alpha = copied.pixelColor(x, y).alpha();
            if (alpha > 0 && alpha < 250) {
                hasPartialAlpha = true;
                break;
            }
        }
    }
    if (!topLevel)
        QVERIFY(hasPartialAlpha);
    for (qreal y : {0.15, 0.35, 0.65, 0.85}) {
        for (qreal x : {0.15, 0.35, 0.65, 0.85}) {
            const int pixelX = int(x * overWhite.width());
            const int pixelY = int(y * overWhite.height());
            const QColor actual = overWhite.pixelColor(pixelX, pixelY);
            // Compare the same pixel centers at both raster resolutions.
            // Sampling the nominal fraction instead straddles sheared edges.
            const QColor expected = referencePage.pixelColor(
                int((frame.x() + (pixelX + 0.5) / overWhite.width() * frame.width())
                    * referencePage.width()),
                int((frame.y() + (pixelY + 0.5) / overWhite.height() * frame.height())
                    * referencePage.height()));
            // PDFium's antialiasing can put a sheared edge on either side of
            // a pixel at different resolutions. Permit one copied pixel of
            // coverage variation, without relaxing interior color/alpha checks.
            const QRect neighborhood = QRectF(
                (frame.x() + qMax(0, pixelX - 1) / qreal(overWhite.width()) * frame.width())
                    * referencePage.width(),
                (frame.y() + qMax(0, pixelY - 1) / qreal(overWhite.height()) * frame.height())
                    * referencePage.height(),
                3.0 * frame.width() / overWhite.width() * referencePage.width(),
                3.0 * frame.height() / overWhite.height() * referencePage.height())
                .toAlignedRect().intersected(referencePage.rect());
            int minimum[3] = {255, 255, 255};
            int maximum[3] = {0, 0, 0};
            for (int ry = neighborhood.top(); ry <= neighborhood.bottom(); ++ry) {
                for (int rx = neighborhood.left(); rx <= neighborhood.right(); ++rx) {
                    const QColor color = referencePage.pixelColor(rx, ry);
                    const int channels[] = {color.red(), color.green(), color.blue()};
                    for (int channel = 0; channel < 3; ++channel) {
                        minimum[channel] = qMin(minimum[channel], channels[channel]);
                        maximum[channel] = qMax(maximum[channel], channels[channel]);
                    }
                }
            }
            const int channels[] = {actual.red(), actual.green(), actual.blue()};
            for (int channel = 0; channel < 3; ++channel) {
                QVERIFY2(channels[channel] >= minimum[channel] - 24
                             && channels[channel] <= maximum[channel] + 24,
                         qPrintable(QStringLiteral("Copied appearance at (%1, %2): %3, expected near %4")
                             .arg(x).arg(y).arg(actual.name(), expected.name())));
            }
        }
    }
}

void PDFEditControllerTest::pastesHeaderCopiesAtOriginalCoordinates_data()
{
    QTest::addColumn<bool>("sourceImage");
    QTest::addColumn<bool>("differentPageSize");
    QTest::addColumn<bool>("pasteInHeader");
    QTest::newRow("added-same-size-header") << false << false << true;
    QTest::newRow("source-same-size-header") << true << false << true;
    QTest::newRow("added-different-size-header") << false << true << true;
    QTest::newRow("source-different-size-header") << true << true << true;
    QTest::newRow("added-same-size-edit") << false << false << false;
    QTest::newRow("source-same-size-edit") << true << false << false;
    QTest::newRow("added-different-size-edit") << false << true << false;
    QTest::newRow("source-different-size-edit") << true << true << false;
}

void PDFEditControllerTest::pastesHeaderCopiesAtOriginalCoordinates()
{
    QFETCH(bool, sourceImage);
    QFETCH(bool, differentPageSize);
    QFETCH(bool, pasteInHeader);
    QClipboard *clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard);
    clipboard->clear();
    const auto clearClipboard = qScopeGuard([clipboard]() { clipboard->clear(); });
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("clipboard-header-position-%1.pdf")
            .arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = sourcePath + QStringLiteral(".saved.pdf");
    const QByteArray destinationBox = differentPageSize ? "600 800" : "400 500";
    QVERIFY(writePdfObjects(sourcePath, {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] "
        "/Resources << /XObject << /Im0 7 0 R >> >> /Contents 5 0 R >>",
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ")
            + destinationBox + "] /Resources << >> /Contents 6 0 R >>",
        imageFixtureStream({}, sourceImage
            ? QByteArray("q 80 0 0 40 67 437 cm /Im0 Do Q") : QByteArray()),
        imageFixtureStream({}, {}),
        imageFixtureStream(
            "/Type /XObject /Subtype /Image /Width 2 /Height 2 "
            "/ColorSpace /DeviceRGB /BitsPerComponent 8",
            QByteArray::fromHex("ff000000ff000000ffffff00")),
    }));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setHeaderSelectionOnly(true);
    controller.setTool(PDFEditController::SelectTool);
    if (sourceImage) {
        const QVariantList candidates = imageCandidates(waitForCandidates(controller, 0));
        QCOMPARE(candidates.size(), 1);
        QVERIFY(controller.selectCandidate(0,
            candidates.constFirst().toMap().value(QStringLiteral("objectId")).toString()));
    } else {
        QImage image(80, 40, QImage::Format_ARGB32);
        image.fill(Qt::cyan);
        QVERIFY(!controller.addImageData(0, 107.0 / 400.0, 43.0 / 500.0,
                                         image, 80.0, 40.0).isEmpty());
    }
    const QRectF original = controller.selectedFrameRect();
    const QSizeF sourceSize = document.pageSize(0);
    const QRectF originalPoints(original.x() * sourceSize.width(),
                                original.y() * sourceSize.height(),
                                original.width() * sourceSize.width(),
                                original.height() * sourceSize.height());
    QVERIFY(QLineF(originalPoints.topLeft(), QPointF(67, 23)).length() < 0.001);
    QVERIFY(controller.selectedImageCopyable());
    QVERIFY(controller.copySelectedImage());
    QTRY_VERIFY_WITH_TIMEOUT(controller.clipboardHasImage(), 10000);
    // Placement follows the copy's origin even if the destination uses Edit.
    controller.setHeaderSelectionOnly(pasteInHeader);
    QVERIFY(!controller.pasteImageFromClipboard(1).isEmpty());
    const QString pastedId = controller.selectedObjectId();
    const QRectF pasted = controller.selectedFrameRect();
    const QSizeF destinationSize = document.pageSize(1);
    const QRectF pastedPoints(pasted.x() * destinationSize.width(),
                              pasted.y() * destinationSize.height(),
                              pasted.width() * destinationSize.width(),
                              pasted.height() * destinationSize.height());
    QVERIFY(QLineF(pastedPoints.topLeft(), originalPoints.topLeft()).length() < 0.001);
    QVERIFY(QLineF(pastedPoints.bottomRight(), originalPoints.bottomRight()).length() < 0.001);
    QCOMPARE(controller.draftsForPage(1).size(), 1);
    controller.undo();
    QVERIFY(controller.draftsForPage(1).isEmpty());
    controller.redo();
    QVERIFY(controller.selectCandidate(1, pastedId));
    QCOMPARE(controller.selectedFrameRect(), pasted);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    for (int page = 0; page < 2; ++page) {
        const QVariantList savedImages = imageCandidates(waitForCandidates(controller, page));
        QCOMPARE(savedImages.size(), 1);
        const QRectF saved = savedImages.constFirst().toMap()
                                 .value(QStringLiteral("sourceRect")).toRectF();
        const QSizeF size = document.pageSize(page);
        const QRectF savedPoints(saved.x() * size.width(), saved.y() * size.height(),
                                saved.width() * size.width(), saved.height() * size.height());
        QVERIFY(QLineF(savedPoints.topLeft(), originalPoints.topLeft()).length() < 0.01);
        QVERIFY(QLineF(savedPoints.bottomRight(), originalPoints.bottomRight()).length() < 0.01);
    }
}

void PDFEditControllerTest::matchesPastedHeaderImagesBeforeAndAfterSave_data()
{
    QTest::addColumn<bool>("sourceImage");
    QTest::addColumn<bool>("softMask");
    QTest::newRow("added") << false << false;
    QTest::newRow("source") << true << false;
    QTest::newRow("source-with-alpha") << true << true;
}

void PDFEditControllerTest::matchesPastedHeaderImagesBeforeAndAfterSave()
{
    QFETCH(bool, sourceImage);
    QFETCH(bool, softMask);
    QClipboard *clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard);
    clipboard->clear();
    const auto clearClipboard = qScopeGuard([clipboard]() { clipboard->clear(); });
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("clipboard-header-match-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = sourcePath + QStringLiteral(".saved.pdf");
    QImage image(366, 162, QImage::Format_ARGB32);
    QByteArray pixels;
    QByteArray alpha;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color((x * 7 + y) % 256, (x + y * 3) % 256, (x * 5 + y * 11) % 256,
                               softMask ? (x + y) % 256 : 255);
            image.setPixelColor(x, y, color);
            pixels.append(char(color.red()));
            pixels.append(char(color.green()));
            pixels.append(char(color.blue()));
            alpha.append(char(color.alpha()));
        }
    }
    const QByteArray placement("q 132 0 0 58.399 269.0221 426.4131 cm /Im0 Do Q");
    const QByteArray pagePrefix("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 500 500] ");
    QVERIFY(writePdfObjects(sourcePath, {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R] /Count 4 >>",
        pagePrefix + "/Resources << /XObject << /Im0 12 0 R >> >> /Contents 7 0 R >>",
        pagePrefix + "/Resources << >> /Contents 8 0 R >>",
        pagePrefix + "/Resources << /XObject << /Im0 11 0 R >> >> /Contents 9 0 R >>",
        pagePrefix + "/Resources << >> /Contents 10 0 R >>",
        imageFixtureStream({}, placement),
        imageFixtureStream({}, {}),
        imageFixtureStream({}, sourceImage ? placement : QByteArray()),
        imageFixtureStream({}, {}),
        imageFixtureStream(QByteArray("/Type /XObject /Subtype /Image /Width 366 /Height 162 ")
            + "/ColorSpace /DeviceRGB /BitsPerComponent 8"
            + (softMask ? " /SMask 13 0 R" : ""), pixels),
        imageFixtureStream("/Type /XObject /Subtype /Image /Width 366 /Height 162 "
            "/ColorSpace /DeviceRGB /BitsPerComponent 8", QByteArray(pixels.size(), char(127))),
        imageFixtureStream("/Type /XObject /Subtype /Image /Width 366 /Height 162 "
            "/ColorSpace /DeviceGray /BitsPerComponent 8", alpha),
    }));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setHeaderSelectionOnly(true);
    controller.setTool(PDFEditController::SelectTool);
    if (sourceImage) {
        const QVariantList images = imageCandidates(waitForCandidates(controller, 2));
        QCOMPARE(images.size(), 1);
        QVERIFY(controller.selectCandidate(2, images.constFirst().toMap().value(QStringLiteral("objectId")).toString()));
    } else {
        QVERIFY(!controller.addImageData(2, (269.0221 + 66) / 500, (15.1879 + 58.399 / 2) / 500,
                                         image, 132, 58.399).isEmpty());
    }
    const QRectF sourceFrame = controller.selectedFrameRect();
    QVERIFY(controller.copySelectedImage());
    QTRY_VERIFY_WITH_TIMEOUT(controller.clipboardHasImage(), 10000);
    QVERIFY(!controller.pasteImageFromClipboard(3).isEmpty());
    QVERIFY(QLineF(controller.selectedFrameRect().topLeft(), sourceFrame.topLeft()).length() < 0.00001);
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 2);
    QCOMPARE(controller.headerSyncMatchPages(), QStringLiteral("3-4"));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    document.close();
    QVERIFY(document.open(QUrl::fromLocalFile(outputPath)));
    controller.setHeaderSelectionOnly(true);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantList reopened = imageCandidates(waitForCandidates(controller, 3));
    QCOMPARE(reopened.size(), 1);
    QVERIFY(controller.selectCandidate(3, reopened.constFirst().toMap().value(QStringLiteral("objectId")).toString()));
    controller.findSimilarHeaders();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.headerSyncLoading(), 15000);
    QCOMPARE(controller.headerSyncMatchCount(), 2);
    QCOMPARE(controller.headerSyncMatchPages(), QStringLiteral("3-4"));
    const QRectF beforeMove = controller.selectedFrameRect();
    const QRectF moved = beforeMove.translated(0.02, 0.01);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(moved);
    controller.finishSelectedFrameResize();
    QVERIFY2(controller.applyHeaderChanges(), qPrintable(controller.headerSyncError()));
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.draftsForPage(1).isEmpty());
    for (int page : {2, 3}) {
        QVERIFY(!waitForCandidates(controller, page).isEmpty());
        QCOMPARE(controller.draftsForPage(page).size(), 1);
        const QRectF actual = controller.draftsForPage(page).constFirst().toMap().value(QStringLiteral("frameRect")).toRectF();
        QVERIFY(QLineF(actual.topLeft(), moved.topLeft()).length() < 0.00001);
        // Added-image Save quantizes frame edges to 1/64 pt; the untouched
        // source retains its original fractional dimensions when only moved.
        QVERIFY(QLineF(actual.bottomRight(), moved.bottomRight()).length() < 0.0001);
    }
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.redo();
    QCOMPARE(controller.draftsForPage(2).size(), 1);
    QCOMPARE(controller.draftsForPage(3).size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    for (int page : {2, 3}) {
        const QVariantList images = imageCandidates(waitForCandidates(controller, page));
        QCOMPARE(images.size(), 1);
        const QRectF actual = images.constFirst().toMap().value(QStringLiteral("sourceRect")).toRectF();
        QVERIFY(QLineF(actual.topLeft(), moved.topLeft()).length() < 0.0001);
        QVERIFY(QLineF(actual.bottomRight(), moved.bottomRight()).length() < 0.0001);
    }
}

void PDFEditControllerTest::pastesClipboardImagesWithinHeaderAndRejectsInvalidInput()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard);
    clipboard->clear();
    const auto clearClipboard = qScopeGuard([clipboard]() { clipboard->clear(); });
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("clipboard-header-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setHeaderSelectionOnly(true);
    controller.setTool(PDFEditController::SelectTool);
    QVERIFY(!controller.clipboardHasImage());
    QVERIFY(controller.pasteImageFromClipboard(0).isEmpty());
    clipboard->setText(QStringLiteral("Clipboard text is not an image"));
    QVERIFY(!controller.clipboardHasImage());
    QVERIFY(controller.pasteImageFromClipboard(0).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    QImage tallImage(40, 400, QImage::Format_ARGB32);
    tallImage.fill(QColor(QStringLiteral("#259fd0")));
    clipboard->setImage(tallImage);
    QVERIFY(controller.clipboardHasImage());
    QVERIFY(controller.pasteImageFromClipboard(-1).isEmpty());
    QVERIFY(controller.pasteImageFromClipboard(document.pageCount()).isEmpty());
    QVERIFY(!controller.hasDrafts());
    const QVariantMap pasted = controller.pasteImageFromClipboard(0);
    QVERIFY(!pasted.isEmpty());
    const QRectF frame = controller.selectedFrameRect();
    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(frame.left() >= 0.0);
    QVERIFY(frame.right() <= 1.0);
    QVERIFY(frame.top() >= 0.0);
    QVERIFY(frame.bottom() * pageSize.height() <= 100.0001);
    QVERIFY(qAbs(frame.width() * pageSize.width() / (frame.height() * pageSize.height()) - 0.1) < 0.001);
    QCOMPARE(controller.selectedKind(), QStringLiteral("addedImage"));
    QVERIFY(controller.selectedImageCopyable());
    QVERIFY(controller.copySelectedImage());
    QTRY_VERIFY_WITH_TIMEOUT(controller.clipboardHasImage(), 5000);
    const QVariantMap duplicate = controller.pasteImageFromClipboard(0);
    QVERIFY(!duplicate.isEmpty());
    const QRectF duplicateFrame = controller.selectedFrameRect();
    QCOMPARE(duplicateFrame.size(), frame.size());
    QVERIFY(duplicateFrame.bottom() * pageSize.height() <= 100.0001);
    QCOMPARE(controller.draftsForPage(0).size(), 2);
    controller.undo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::cancelsSourceImageCopyWithoutPastingStaleClipboard()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QVERIFY(clipboard);
    clipboard->clear();
    const auto clearClipboard = qScopeGuard([clipboard]() { clipboard->clear(); });
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("clipboard-cancel-source.pdf"));
    const QString nextPath = m_temporaryDirectory.filePath(QStringLiteral("clipboard-cancel-next.pdf"));
    QVERIFY(createSharedNestedImagePdf(sourcePath, true, 0, false, true));
    QVERIFY(createTopLevelImagePdf(nextPath));
    PDFDocument document;
    PDFDocument nextDocument;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QVERIFY(nextDocument.open(QUrl::fromLocalFile(nextPath)));
    PDFEditController nextController;
    nextController.setDocument(&nextDocument);
    nextController.setTool(PDFEditController::SelectTool);
    const QVariantList nextCandidates = imageCandidates(waitForCandidates(nextController, 0));
    QCOMPARE(nextCandidates.size(), 1);
    QVERIFY(nextController.selectCandidate(0,
        nextCandidates.constFirst().toMap().value(QStringLiteral("objectId")).toString()));
    int nextCopySuccesses = 0;
    connect(&nextController, &PDFEditController::statusMessage, this,
            [&nextCopySuccesses](const QString &message) {
        if (message == qtTrId("ImageCopied"))
            ++nextCopySuccesses;
    });

    for (const QString &change : {QStringLiteral("selection"), QStringLiteral("clipboard"),
                                  QStringLiteral("document")}) {
        PDFEditController controller;
        controller.setDocument(&document);
        controller.setTool(PDFEditController::SelectTool);
        const QVariantList candidates = imageCandidates(waitForCandidates(controller, 0));
        QCOMPARE(candidates.size(), 2);
        QVERIFY(controller.selectCandidate(0,
            candidates.constFirst().toMap().value(QStringLiteral("objectId")).toString()));
        QImage sentinel(16, 12, QImage::Format_ARGB32);
        sentinel.fill(Qt::magenta);
        clipboard->setImage(sentinel);
        QVERIFY(controller.clipboardHasImage());
        QVERIFY(controller.copySelectedImage());
        // Source rendering completes through a queued GUI callback. Before that
        // callback, Paste must not insert the image left on the clipboard.
        QVERIFY(!controller.clipboardHasImage());
        QVERIFY(!nextController.clipboardHasImage());
        QVERIFY(nextController.pasteImageFromClipboard(0).isEmpty());
        QVERIFY(!nextController.hasDrafts());
        QVERIFY(controller.pasteImageFromClipboard(0).isEmpty());
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
        if (change == QLatin1String("selection")) {
            controller.clearSelection();
        } else if (change == QLatin1String("clipboard")) {
            sentinel.fill(Qt::cyan);
            clipboard->setImage(sentinel);
        } else {
            controller.setDocument(&nextDocument);
        }
        QCOMPARE(clipboard->image(), sentinel);
        QVERIFY(controller.clipboardHasImage());
        QSignalSpy clipboardChanges(clipboard, &QClipboard::dataChanged);

        // A successful subsequent source copy drains the single copy worker.
        // This checks delayed completion without relying on an arbitrary sleep.
        const int previousSuccesses = nextCopySuccesses;
        bool nextStarted = false;
        QTRY_VERIFY_WITH_TIMEOUT(nextStarted || (nextStarted = nextController.copySelectedImage()), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(nextCopySuccesses, previousSuccesses + 1, 5000);
        QCOMPARE(clipboardChanges.size(), 1);
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
        QVERIFY(!nextController.hasDrafts());
    }
}

void PDFEditControllerTest::movesAddedImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-added-image-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-added-image.png"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#4b8fd8")));
    QVERIFY(image.save(imagePath, "PNG"));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap added = controller.addImage(
        0,
        0.2,
        0.55,
        QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    const QString objectId = added.value(QStringLiteral("objectId")).toString();
    const QRectF originalFrame = added.value(QStringLiteral("frameRect")).toRectF();
    const qreal movedX = originalFrame.right() + 0.03;
    QVERIFY(movedX + originalFrame.width() < 1.0);
    const QRectF movedFrame(
        movedX,
        originalFrame.y() + 0.08,
        originalFrame.width(),
        originalFrame.height());

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();

    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.selectedContentBounds(), movedFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);
    QCOMPARE(
        controller.candidateAt(0, movedFrame.center().x(), movedFrame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);
    QVERIFY(controller.candidateAt(
        0,
        originalFrame.center().x(),
        originalFrame.center().y()).value(QStringLiteral("objectId")).toString()
        != objectId);

    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
}

void PDFEditControllerTest::movesTopLevelImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("move-image-source.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    const QString objectId = imageCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QRectF originalFrame = imageCandidate
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const qreal movedX = qMax<qreal>(0.0, originalFrame.x() - 0.3);
    const qreal movedY = qMin<qreal>(
        1.0 - originalFrame.height(),
        originalFrame.y() + 0.12);
    const QRectF requestedFrame(
        movedX,
        movedY,
        originalFrame.width(),
        originalFrame.height());
    const QRectF movedFrame(
        movedX,
        movedY,
        originalFrame.width(),
        originalFrame.height());
    QVERIFY(movedFrame != originalFrame);

    QVERIFY(controller.selectCandidate(0, objectId));
    controller.beginSelectedFrameResize();
    QVERIFY(controller.textEditActive());
    controller.updateSelectedFrameRect(requestedFrame);
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.selectedContentBounds(), movedFrame);
    controller.finishSelectedFrameResize();

    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 0);
    const QVector<PDFEditController::DraftObjectMove> moves
        = controller.draftedObjectMovesForPage(0);
    QCOMPARE(moves.size(), 1);
    QCOMPARE(moves.constFirst().kind, PDFEditController::DraftObjectMove::Kind::Image);
    QCOMPARE(moves.constFirst().sourceRect, originalFrame);
    QCOMPARE(moves.constFirst().frameRect, movedFrame);
    QCOMPARE(moves.constFirst().objectPath.size(), 1);
    QCOMPARE(
        controller.candidateAt(0, movedFrame.center().x(), movedFrame.center().y())
            .value(QStringLiteral("objectId")).toString(),
        objectId);
    QVERIFY(controller.candidateAt(
        0,
        originalFrame.center().x(),
        originalFrame.center().y()).value(QStringLiteral("objectId")).toString()
        != objectId);

    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 0);

    controller.redo();
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).size(), 1);
}

void PDFEditControllerTest::resizesTopLevelImageWithHistoryAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("resize-image-history.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    const QString objectId = candidate.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectCandidate(0, objectId));
    QVERIFY(controller.selectedImageResizable());
    const QRectF originalFrame = controller.selectedFrameRect();
    const QRectF resizedFrame(0.2, 0.3,
                             originalFrame.width() * 1.5,
                             originalFrame.height() * 0.7);

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedSourceRect(), originalFrame);
    controller.cancelSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    // Returning to the starting frame is a no-op even after live previews.
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resizedFrame);
    controller.updateSelectedFrameRect(originalFrame);
    controller.finishSelectedFrameResize();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.2, 0.3, 0.2, 0.2));
    controller.updateSelectedFrameRect(resizedFrame);
    controller.finishSelectedFrameResize();
    QVERIFY(!controller.textEditActive());
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QCOMPARE(controller.selectedContentBounds(), resizedFrame);
    const auto transforms = controller.draftedObjectMovesForPage(0);
    QCOMPARE(transforms.size(), 1);
    QCOMPARE(transforms.constFirst().kind, PDFEditController::DraftObjectMove::Kind::Image);
    QCOMPARE(transforms.constFirst().sourceRect, originalFrame);
    QCOMPARE(transforms.constFirst().frameRect, resizedFrame);
    QCOMPARE(controller.candidateAt(0, resizedFrame.center().x(), resizedFrame.center().y())
                 .value(QStringLiteral("objectId")).toString(), objectId);

    // Cancel a second gesture without deleting the first committed resize.
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.1, 0.1, 0.3, 0.3));
    controller.cancelSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), resizedFrame);
    QVERIFY(controller.hasDrafts());

    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedFrameRect(), originalFrame);
    QVERIFY(controller.draftedObjectMovesForPage(0).isEmpty());
}

void PDFEditControllerTest::keepsClippedSourceImagesMoveOnly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("clipped-image-resize.pdf"));
    const QByteArray content("q 110 120 90 40 re W n 120 0 0 60 100 100 cm /Im0 Do Q\n");
    const QByteArray pixels = QByteArray::fromHex("ff000000ff000000ffffffff");
    QVERIFY(writePdfObjects(sourcePath, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] "
                   "/Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>"),
        QByteArray("<< /Length ") + QByteArray::number(content.size())
            + QByteArray(" >>\nstream\n") + content + QByteArray("endstream"),
        QByteArray("<< /Type /XObject /Subtype /Image /Width 2 /Height 2 "
                   "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 12 >>\nstream\n")
            + pixels + QByteArray("\nendstream"),
    }));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(candidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(!candidate.value(QStringLiteral("imageResizable")).toBool());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!controller.selectedImageResizable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(
        sourceFrame.topLeft(), sourceFrame.size() * 0.5));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());

    const QRectF movedFrame = sourceFrame.translated(0.1, 0.1);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QVERIFY(controller.hasDrafts());
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());
    controller.deleteSelectedObject();
    QVERIFY(controller.hasDrafts());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::keepsOffPageSourceImagesMoveOnly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("off-page-image-resize.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath, {120, 0, 0, 60, -20, 620}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(candidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(!candidate.value(QStringLiteral("imageResizable")).toBool());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!controller.selectedImageResizable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(sourceFrame.topLeft(), sourceFrame.size() * 0.5));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());
    const QRectF movedFrame = sourceFrame.translated(0.1, 0.1);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    controller.deleteSelectedObject();
    QVERIFY(controller.hasDrafts());
    controller.undo();
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::keepsSingularSourceImagesMoveOnly()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("singular-image-move.pdf"));
    const QString outputPath = sourcePath + QStringLiteral(".copy.pdf");
    // A rank-one image still has a nonempty axis-aligned selection frame.
    // Translation must remain available even though resizing is unsupported.
    QVERIFY(createTopLevelImagePdf(sourcePath, {120, 120, 60, 60, 100, 100}));
    const ImageSaveInspection original = inspectImages(sourcePath);
    QVERIFY(original.matrixReadable);
    QVERIFY(original.pixelsReadable);
    QCOMPARE(original.count, 1);
    QCOMPARE(original.matrix.a * original.matrix.d
        - original.matrix.b * original.matrix.c, 0.0F);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedEditable());
    QVERIFY(!controller.selectedImageResizable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    QVERIFY(!sourceFrame.isEmpty());
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(
        sourceFrame.topLeft(), sourceFrame.size() * 0.5));
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());

    const QPointF delta(0.1, 0.1);
    const QRectF movedFrame = sourceFrame.translated(delta);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), movedFrame);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());

    const ImageSaveInspection saved = inspectImages(outputPath);
    QVERIFY(saved.matrixReadable);
    QVERIFY(saved.pixelsReadable);
    QCOMPARE(saved.count, original.count);
    QCOMPARE(saved.firstTopLevelIndex, original.firstTopLevelIndex);
    QCOMPARE(saved.pixelWidth, original.pixelWidth);
    QCOMPARE(saved.pixelHeight, original.pixelHeight);
    QCOMPARE(saved.pixels, original.pixels);
    QCOMPARE(saved.matrix.a, original.matrix.a);
    QCOMPARE(saved.matrix.b, original.matrix.b);
    QCOMPARE(saved.matrix.c, original.matrix.c);
    QCOMPARE(saved.matrix.d, original.matrix.d);
    QVERIFY(qAbs(saved.matrix.e - original.matrix.e - delta.x() * 595.0) < 0.001);
    QVERIFY(qAbs(saved.matrix.f - original.matrix.f + delta.y() * 842.0) < 0.001);
}

void PDFEditControllerTest::resizesSourceImageThroughQmlHandles()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The source image handle test needs a Qt Quick render screen.");
    }
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("image-qml-resize.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.setTool(PDFEditController::SelectTool);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.imageAspectRatioLocked());
    const QRectF original = controller.selectedFrameRect();
    const qreal normalizedAspect = original.width() / original.height();

    const QString themePath = QFINDTESTDATA("../src/Components/AppTheme.qml");
    const QString overlayPath = editOverlayQmlPath();
    QVERIFY(registerQmlTestTypes(themePath));
    QQuickWindow window;
    window.resize(595, 842);
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(overlayPath));
    const QVariantMap properties{
        {QStringLiteral("page"), 0},
        {QStringLiteral("pooled"), false},
        {QStringLiteral("draftTextureCurrent"), true},
        {QStringLiteral("draftTextureReady"), true},
        {QStringLiteral("displayedSuppressedCommentObjectIds"), QStringList()},
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("controller"), QVariant::fromValue(&controller)},
        {QStringLiteral("pagePointScale"), 1.0},
        {QStringLiteral("width"), 595.0},
        {QStringLiteral("height"), 842.0}};
    std::unique_ptr<QObject> overlay(component.createWithInitialProperties(properties));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    auto *overlayItem = qobject_cast<QQuickItem *>(overlay.get());
    QVERIFY(overlayItem);
    overlayItem->setParentItem(window.contentItem());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);

    // Locate real delegates from their directional data; do not duplicate the
    // QML resize algorithm or replace the controller with a mock.
    const auto findHandle = [&](auto &&self, QQuickItem *item,
                                int horizontal, int vertical) -> QQuickItem * {
        const QVariant raw = item->property("modelData");
        const QVariantMap data = raw.metaType() == QMetaType::fromType<QJSValue>()
            ? raw.value<QJSValue>().toVariant().toMap() : raw.toMap();
        if (data.contains(QStringLiteral("horizontal"))
            && data.value(QStringLiteral("horizontal")).toInt() == horizontal
            && data.value(QStringLiteral("vertical")).toInt() == vertical) {
            for (QQuickItem *child : item->childItems()) {
                if (child->metaObject()->indexOfProperty("acceptedButtons") >= 0) {
                    return child;
                }
            }
        }
        for (QQuickItem *child : item->childItems()) {
            if (QQuickItem *found = self(self, child, horizontal, vertical)) {
                return found;
            }
        }
        return nullptr;
    };
    const auto handle = [&](int horizontal, int vertical) {
        return findHandle(findHandle, overlayItem, horizontal, vertical);
    };
    const auto centerInWindow = [](QQuickItem *item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    };
    for (const QPoint direction : {
             QPoint(-1, -1), QPoint(0, -1), QPoint(1, -1), QPoint(1, 0),
             QPoint(1, 1), QPoint(0, 1), QPoint(-1, 1), QPoint(-1, 0)}) {
        QQuickItem *mouse = handle(direction.x(), direction.y());
        QVERIFY(mouse);
        QVERIFY(mouse->isVisible());
        const QPoint start = centerInWindow(mouse);
        const QPoint end = start + QPoint(direction.x() * 30, direction.y() * 20);
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&window, end);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
        QVERIFY(!controller.textEditActive());
        const QRectF resized = controller.selectedFrameRect();
        QVERIFY(resized.width() > original.width());
        QVERIFY(qAbs(resized.width() / resized.height() - normalizedAspect) < 0.000001);
        if (direction.x() < 0)
            QVERIFY(qAbs(resized.right() - original.right()) < 0.000001);
        else if (direction.x() > 0)
            QVERIFY(qAbs(resized.left() - original.left()) < 0.000001);
        else
            QVERIFY(qAbs(resized.center().x() - original.center().x()) < 0.000001);
        if (direction.y() < 0)
            QVERIFY(qAbs(resized.bottom() - original.bottom()) < 0.000001);
        else if (direction.y() > 0)
            QVERIFY(qAbs(resized.top() - original.top()) < 0.000001);
        else
            QVERIFY(qAbs(resized.center().y() - original.center().y()) < 0.000001);
        controller.undo();
        QCOMPARE(controller.selectedFrameRect(), original);
        QVERIFY(!controller.hasDrafts());
        QVERIFY(!controller.canUndo());
    }

    QQuickItem *rightHandle = handle(1, 0);
    QVERIFY(rightHandle);
    rightHandle->forceActiveFocus();
    QTest::keyClick(&window, Qt::Key_Right, Qt::ShiftModifier);
    const QRectF keyboardLocked = controller.selectedFrameRect();
    QVERIFY(qAbs(keyboardLocked.width() - original.width() - 10.0 / window.width()) < 0.000001);
    QVERIFY(qAbs(keyboardLocked.width() / keyboardLocked.height() - normalizedAspect) < 0.000001);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), original);

    // The dedicated image panel changes the session preference without
    // creating a PDF draft. Its unlocked handles change one dimension only.
    const QString panelPath = QFINDTESTDATA("../src/Components/ImagePropertiesPanel.qml");
    QVERIFY(!panelPath.isEmpty());
    QQuickWindow panelWindow;
    panelWindow.resize(260, 842);
    QQmlComponent panelComponent(&engine, QUrl::fromLocalFile(panelPath));
    const QVariantMap panelProperties{
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("editController"), QVariant::fromValue(&controller)},
        {QStringLiteral("collapsed"), false},
        {QStringLiteral("width"), 260.0},
        {QStringLiteral("height"), 842.0}};
    std::unique_ptr<QObject> panel(
        panelComponent.createWithInitialProperties(panelProperties));
    QVERIFY2(panel, qPrintable(panelComponent.errorString()));
    auto *panelItem = qobject_cast<QQuickItem *>(panel.get());
    QVERIFY(panelItem);
    panelItem->setParentItem(panelWindow.contentItem());
    panelWindow.show();
    QTRY_VERIFY_WITH_TIMEOUT(panelWindow.isExposed(), 5000);
    QPointer<QQuickItem> checkbox = panel->findChild<QQuickItem *>(
        QStringLiteral("imageAspectRatioLock"));
    QVERIFY(checkbox);
    QTRY_VERIFY(checkbox->isVisible());
    QVERIFY(checkbox->isEnabled());
    QVERIFY(checkbox->property("checked").toBool());
    for (QObject *child : overlay->findChildren<QObject *>()) {
        QVERIFY(child->property("text").toString() != QLatin1String("ImageAspectRatioLocked")
                || child->metaObject()->indexOfProperty("checked") < 0);
    }
    QTest::mouseClick(&panelWindow, Qt::LeftButton, Qt::NoModifier, centerInWindow(checkbox));
    QVERIFY(!controller.imageAspectRatioLocked());
    QVERIFY(!controller.hasDrafts());

    controller.setImageAspectRatioLocked(true);
    QAccessibleInterface *accessibleCheckbox = QAccessible::queryAccessibleInterface(checkbox);
    QVERIFY(accessibleCheckbox);
    QAccessibleActionInterface *checkboxActions = accessibleCheckbox->actionInterface();
    QVERIFY(checkboxActions);
    QVERIFY(checkboxActions->actionNames().contains(QAccessibleActionInterface::toggleAction()));
    checkboxActions->doAction(QAccessibleActionInterface::toggleAction());
    QVERIFY(!checkbox->property("checked").toBool());
    QVERIFY(!controller.imageAspectRatioLocked());
    QVERIFY(!controller.hasDrafts());
    panel->setProperty("collapsed", true);
    QTRY_VERIFY(checkbox.isNull());
    panel->setProperty("collapsed", false);
    checkbox = panel->findChild<QQuickItem *>(QStringLiteral("imageAspectRatioLock"));
    QVERIFY(checkbox);
    QTRY_VERIFY(checkbox->isVisible());
    QVERIFY(!checkbox->property("checked").toBool());
    QVERIFY(!controller.hasDrafts());
    panelWindow.hide();
    window.requestActivate();
    rightHandle = handle(1, 0);
    QVERIFY(rightHandle);
    rightHandle->forceActiveFocus();
    QTest::keyClick(&window, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(controller.selectedFrameRect().height(), original.height());
    QVERIFY(controller.selectedFrameRect().width() > original.width());
    controller.undo();

    rightHandle = handle(1, 0);
    QVERIFY(rightHandle);
    QPoint start = centerInWindow(rightHandle);
    QPoint end = start + QPoint(35, 50);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&window, end);
    QCOMPARE(controller.selectedFrameRect().height(), original.height());
    QVERIFY(controller.selectedFrameRect().width() > original.width());
    QTest::keyClick(&window, Qt::Key_Escape);
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
    QCOMPARE(controller.selectedFrameRect(), original);
    QVERIFY(!controller.hasDrafts());

    controller.setImageAspectRatioLocked(true);
    for (const QPoint delta : {QPoint(1000, 1000), QPoint(-1000, -1000)}) {
        QQuickItem *cornerHandle = handle(1, 1);
        QVERIFY(cornerHandle);
        start = centerInWindow(cornerHandle);
        end = start + delta;
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&window, end);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, end);
        const QRectF resized = controller.selectedFrameRect();
        QVERIFY(resized != original);
        QVERIFY(resized.right() <= 1.0 + 0.000001);
        QVERIFY(resized.bottom() <= 1.0 + 0.000001);
        QVERIFY(resized.width() * window.width() >= 24.0 - 0.000001);
        QVERIFY(resized.height() * window.height() >= 24.0 - 0.000001);
        QVERIFY(qAbs(resized.width() / resized.height() - normalizedAspect) < 0.000001);
        QCOMPARE(resized.topLeft(), original.topLeft());
        controller.undo();
        QCOMPARE(controller.selectedFrameRect(), original);
    }
}

void PDFEditControllerTest::savesResizedTopLevelImageRoundTrip_data()
{
    QTest::addColumn<QList<qreal>>("imageMatrix");
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("offsetCrop");
    QTest::addColumn<bool>("overwrite");
    const QList<QPair<QByteArray, QList<qreal>>> matrices{
        {"upright", {120, 0, 0, 60, 300, 500}},
        {"rotated-image", {0, 120, -60, 0, 360, 500}},
        {"skewed", {120, 25, 35, 60, 300, 500}},
        {"reflected", {-120, 25, 35, 60, 420, 500}},
    };
    for (const auto &entry : matrices) {
        for (int rotation = 0; rotation < 4; ++rotation) {
            for (const bool overwrite : {false, true}) {
                const QByteArray name = entry.first + "-crop-rotate-"
                    + QByteArray::number(rotation * 90)
                    + (overwrite ? "-save" : "-save-as");
                QTest::newRow(name.constData())
                    << entry.second << rotation << true << overwrite;
            }
        }
    }
    QTest::newRow("uncropped-save-as") << matrices.constFirst().second << 0 << false << false;
    QTest::newRow("uncropped-save") << matrices.constFirst().second << 0 << false << true;
}

void PDFEditControllerTest::savesResizedTopLevelImageRoundTrip()
{
    QFETCH(QList<qreal>, imageMatrix);
    QFETCH(int, rotation);
    QFETCH(bool, offsetCrop);
    QFETCH(bool, overwrite);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("resize-image-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = overwrite ? sourcePath : sourcePath + QStringLiteral(".copy.pdf");
    const FS_MATRIX matrix{
        float(imageMatrix[0]), float(imageMatrix[1]), float(imageMatrix[2]),
        float(imageMatrix[3]), float(imageMatrix[4]), float(imageMatrix[5])};
    QVERIFY(createTopLevelImagePdf(sourcePath, matrix, rotation, offsetCrop));
    const ImageSaveInspection original = inspectImages(sourcePath);
    QVERIFY(original.matrixReadable);
    QVERIFY(original.pixelsReadable);
    QCOMPARE(original.normalizedCorners.size(), 4);
    QVERIFY(original.topLeftPixel != original.bottomLeftPixel);
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray sourceBytes = sourceFile.readAll();
    sourceFile.close();

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageResizable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    const QRectF targetFrame(0.18, 0.23,
                            sourceFrame.width() * 1.35,
                            sourceFrame.height() * 0.65);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(targetFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), targetFrame);
    QVERIFY(overwrite ? controller.save()
                      : controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(document.source().toLocalFile(), outputPath);

    const ImageSaveInspection saved = inspectImages(outputPath);
    QVERIFY(saved.matrixReadable);
    QCOMPARE(saved.count, 1);
    QCOMPARE(saved.firstTopLevelIndex, original.firstTopLevelIndex);
    QCOMPARE(saved.pixelWidth, original.pixelWidth);
    QCOMPARE(saved.pixelHeight, original.pixelHeight);
    QCOMPARE(saved.pixels, original.pixels);
    QCOMPARE(saved.normalizedCorners.size(), 4);
    for (int index = 0; index < 4; ++index) {
        const QPointF corner = original.normalizedCorners[index];
        const QPointF expected(
            targetFrame.x() + (corner.x() - sourceFrame.x()) * 1.35,
            targetFrame.y() + (corner.y() - sourceFrame.y()) * 0.65);
        const QPointF actual = saved.normalizedCorners[index];
        QVERIFY2(QLineF(expected, actual).length() < 0.0001,
                 qPrintable(QStringLiteral("corner %1: expected (%2,%3), got (%4,%5)")
                     .arg(index).arg(expected.x()).arg(expected.y()).arg(actual.x()).arg(actual.y())));
    }
    const qreal originalDeterminant = original.matrix.a * original.matrix.d
        - original.matrix.b * original.matrix.c;
    const qreal savedDeterminant = saved.matrix.a * saved.matrix.d
        - saved.matrix.b * saved.matrix.c;
    QVERIFY(originalDeterminant * savedDeterminant > 0.0);
    QVERIFY(normalizedPageText(outputPath).contains(QString::fromLatin1(kImageFixtureText)));

    const QVariantMap reopenedCandidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!reopenedCandidate.isEmpty());
    const QRectF reopenedFrame = reopenedCandidate.value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(QLineF(reopenedFrame.topLeft(), targetFrame.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(reopenedFrame.bottomRight(), targetFrame.bottomRight()).length() < 0.0001);
    QVERIFY(reopenedCandidate.value(QStringLiteral("imageResizable")).toBool());
    if (!overwrite) {
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        QCOMPARE(sourceFile.readAll(), sourceBytes);
    }
}

void PDFEditControllerTest::restoresSourceImagePreviewAfterResize()
{
    if (QGuiApplication::screens().isEmpty()) {
        QSKIP("The source image preview test needs a Qt Quick render screen.");
    }
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("resize-preview-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("resize-preview-reference.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath, {-120, 25, 35, 60, 420, 500}, 1, true));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QRectF sourceFrame = controller.selectedFrameRect();
    const QRectF targetFrame(0.18, 0.23, sourceFrame.width() * 1.5, sourceFrame.height() * 0.75);

    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(650, 500);
    TestablePDFPageItem pageItem(window.contentItem());
    pageItem.setSize(window.size());
    pageItem.setEditController(&controller);
    pageItem.setDocument(&document);
    pageItem.setPage(0);
    pageItem.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    const auto grabPage = [&]() {
        QElapsedTimer timer;
        timer.start();
        while ((!pageItem.draftTextureCurrent() || pageItem.m_renderPending)
               && timer.elapsed() < 5000) {
            QTest::qWait(10);
        }
        return pageItem.draftTextureCurrent() && !pageItem.m_renderPending
            ? window.grabWindow() : QImage();
    };
    const QImage originalPreview = grabPage();
    QVERIFY(!originalPreview.isNull());

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(targetFrame);
    const QImage livePreview = grabPage();
    QVERIFY(!livePreview.isNull());
    QVERIFY(changedPixelCount(originalPreview, livePreview, livePreview.rect()) > 100);
    controller.finishSelectedFrameResize();
    const PDFSaveWriter::Result saved = PDFSaveWriter::save(controller, outputPath);
    QVERIFY2(saved.succeeded(), qPrintable(saved.detail));
    const QImage reference = renderFirstPage(outputPath, livePreview.size());
    QVERIFY(!reference.isNull());
    QVERIFY(changedPixelCount(reference, livePreview, reference.rect()) < reference.width() * reference.height() / 200);

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.4, 0.3, 0.3, 0.3));
    const QImage canceledPreview = grabPage();
    QVERIFY(!canceledPreview.isNull());
    QVERIFY(changedPixelCount(canceledPreview, livePreview, livePreview.rect()) > 100);
    controller.cancelSelectedFrameResize();
    QCOMPARE(grabPage(), livePreview);

    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(QRectF(0.4, 0.3, 0.3, 0.3));
    QVERIFY(!grabPage().isNull());
    controller.updateSelectedFrameRect(targetFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(grabPage(), livePreview);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(grabPage(), originalPreview);
    controller.redo();
    QCOMPARE(grabPage(), livePreview);
    controller.discardDrafts();
    QCOMPARE(grabPage(), originalPreview);
}

void PDFEditControllerTest::deletesTopLevelImageWithUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-image-source.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    QCOMPARE(imageCandidate.value(QStringLiteral("editable")).toBool(), true);
    const QString objectId = imageCandidate
        .value(QStringLiteral("objectId"))
        .toString();
    const QPointF hitPoint = imageCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF()
        .center();
    const auto containsImage = [&objectId](const QVariantList &candidates) {
        return std::any_of(
            candidates.cbegin(),
            candidates.cend(),
            [&objectId](const QVariant &candidateValue) {
                const QVariantMap candidate = candidateValue.toMap();
                return candidate.value(QStringLiteral("kind")).toString()
                        == QLatin1String("image")
                    && candidate.value(QStringLiteral("objectId")).toString()
                        == objectId;
            });
    };

    QCOMPARE(
        controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedKind(), QStringLiteral("image"));
    QVERIFY(controller.selectedEditable());

    controller.deleteSelectedObject();

    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(controller.selectedObjectId(), QString());
    const QVariantList deletionDrafts = controller.draftsForPage(0);
    QCOMPARE(deletionDrafts.size(), 1);
    QVERIFY(deletionDrafts.constFirst().toMap()
        .value(QStringLiteral("deleted"))
        .toBool());
    QCOMPARE(
        deletionDrafts.constFirst().toMap()
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
    QVERIFY(controller.allDraftedTextObjectsSuppressibleForPage(0));
    QVERIFY(!containsImage(waitForCandidates(controller, 0)));
    QVERIFY(controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
    QVERIFY(!controller.selectCandidate(0, objectId));

    controller.undo();

    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.canRedo());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 0);
    QVERIFY(containsImage(waitForCandidates(controller, 0)));
    QCOMPARE(
        controller.candidateAt(0, hitPoint.x(), hitPoint.y())
            .value(QStringLiteral("objectId"))
            .toString(),
        objectId);
    QVERIFY(controller.selectCandidate(0, objectId));

    controller.redo();

    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.draftedTextObjectPathsForPage(0).size(), 1);
    QCOMPARE(controller.selectedObjectId(), QString());
    QVERIFY(!containsImage(waitForCandidates(controller, 0)));
    QVERIFY(controller.candidateAt(0, hitPoint.x(), hitPoint.y())
        .value(QStringLiteral("objectId"))
        .toString() != objectId);
}

void PDFEditControllerTest::savesAddedImageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-image-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-image-input.png"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-image-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#2b5fd8")));
    for (int y = 0; y < image.height() / 2; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor(QStringLiteral("#d86f4b")));
        }
    }
    QVERIFY(image.save(imagePath, "PNG"));
    QCOMPARE(inspectImages(sourcePath).count, 0);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap added = controller.addImage(
        0,
        0.45,
        0.35,
        QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    const QRectF frame = added.value(QStringLiteral("frameRect")).toRectF();
    QVERIFY(QFile::remove(imagePath));

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const ImageSaveInspection inspection = inspectImages(outputPath);
    QVERIFY(inspection.readable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(inspection.pixelWidth, 80U);
    QCOMPARE(inspection.pixelHeight, 40U);
    QVERIFY(inspection.pixelsReadable);
    QVERIFY(inspection.topLeftPixel.red() > inspection.topLeftPixel.blue());
    QVERIFY(inspection.bottomLeftPixel.blue() > inspection.bottomLeftPixel.red());
    QCOMPARE(inspectImages(sourcePath).count, 0);

    const QVariantList savedCandidates = waitForCandidates(controller, 0);
    const auto savedImage = std::find_if(
        savedCandidates.cbegin(),
        savedCandidates.cend(),
        [](const QVariant &value) {
            return value.toMap().value(QStringLiteral("kind")).toString()
                == QLatin1String("image");
        });
    QVERIFY(savedImage != savedCandidates.cend());
    const QRectF savedRect = savedImage->toMap()
        .value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(qAbs(savedRect.x() - frame.x()) < 0.001);
    QVERIFY(qAbs(savedRect.y() - frame.y()) < 0.001);
    QVERIFY(qAbs(savedRect.width() - frame.width()) < 0.001);
    QVERIFY(qAbs(savedRect.height() - frame.height()) < 0.001);
}

void PDFEditControllerTest::savesMovedTopLevelImageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-move-image-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-move-image-output.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    const ImageSaveInspection sourceInspection = inspectImages(sourcePath);
    QVERIFY(sourceInspection.readable);
    QVERIFY(sourceInspection.matrixReadable);
    QCOMPARE(sourceInspection.count, 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    const QRectF originalFrame = imageCandidate
        .value(QStringLiteral("frameRect"))
        .toRectF();
    const QRectF movedFrame(
        qMax<qreal>(0.0, originalFrame.x() - 0.2),
        qMin<qreal>(1.0 - originalFrame.height(), originalFrame.y() + 0.1),
        originalFrame.width(),
        originalFrame.height());
    QVERIFY(movedFrame != originalFrame);
    QVERIFY(controller.selectCandidate(
        0,
        imageCandidate.value(QStringLiteral("objectId")).toString()));
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(movedFrame);
    controller.finishSelectedFrameResize();

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());

    const ImageSaveInspection savedInspection = inspectImages(outputPath);
    QVERIFY(savedInspection.readable);
    QVERIFY(savedInspection.matrixReadable);
    QCOMPARE(savedInspection.count, 1);
    QCOMPARE(savedInspection.firstTopLevelIndex, sourceInspection.firstTopLevelIndex);
    QCOMPARE(savedInspection.pixelWidth, sourceInspection.pixelWidth);
    QCOMPARE(savedInspection.pixelHeight, sourceInspection.pixelHeight);
    QCOMPARE(savedInspection.pixelsReadable, sourceInspection.pixelsReadable);
    QCOMPARE(savedInspection.topLeftPixel, sourceInspection.topLeftPixel);
    QCOMPARE(savedInspection.bottomLeftPixel, sourceInspection.bottomLeftPixel);
    QVERIFY(qAbs(savedInspection.matrix.a - sourceInspection.matrix.a) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.b - sourceInspection.matrix.b) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.c - sourceInspection.matrix.c) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.d - sourceInspection.matrix.d) < 0.001F);
    QVERIFY(qAbs(savedInspection.matrix.e - sourceInspection.matrix.e) > 1.0F);
    QVERIFY(qAbs(savedInspection.matrix.f - sourceInspection.matrix.f) > 1.0F);
    QVERIFY(normalizedPageText(outputPath).contains(
        QString::fromLatin1(kImageFixtureText)));

    const QVariantMap savedCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY(!savedCandidate.isEmpty());
    const QRectF savedRect = savedCandidate
        .value(QStringLiteral("sourceRect"))
        .toRectF();
    QVERIFY(qAbs(savedRect.x() - movedFrame.x()) < 0.001);
    QVERIFY(qAbs(savedRect.y() - movedFrame.y()) < 0.001);
    QVERIFY(qAbs(savedRect.width() - movedFrame.width()) < 0.001);
    QVERIFY(qAbs(savedRect.height() - movedFrame.height()) < 0.001);

    const ImageSaveInspection unchangedSourceInspection = inspectImages(sourcePath);
    QVERIFY(unchangedSourceInspection.matrixReadable);
    QCOMPARE(unchangedSourceInspection.matrix.e, sourceInspection.matrix.e);
    QCOMPARE(unchangedSourceInspection.matrix.f, sourceInspection.matrix.f);
}

void PDFEditControllerTest::savesDeletedTopLevelImageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-delete-image-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-delete-image-output.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));

    const ImageSaveInspection sourceBeforeSave = inspectImages(sourcePath);
    QVERIFY(sourceBeforeSave.readable);
    QCOMPARE(sourceBeforeSave.count, 1);

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap imageCandidate = candidateForKind(
        waitForCandidates(controller, 0),
        QStringLiteral("image"));
    QVERIFY2(!imageCandidate.isEmpty(), "Top-level image candidate was not discovered");
    QVERIFY(imageCandidate.value(QStringLiteral("editable")).toBool());
    QVERIFY(controller.selectCandidate(
        0,
        imageCandidate.value(QStringLiteral("objectId")).toString()));
    controller.deleteSelectedObject();
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    const ImageSaveInspection savedInspection = inspectImages(outputPath);
    QVERIFY(savedInspection.readable);
    QCOMPARE(savedInspection.count, 0);
    QVERIFY(normalizedPageText(outputPath).contains(
        QString::fromLatin1(kImageFixtureText)));

    const ImageSaveInspection unchangedSourceInspection = inspectImages(sourcePath);
    QVERIFY(unchangedSourceInspection.readable);
    QCOMPARE(unchangedSourceInspection.count, 1);
    QVERIFY(normalizedPageText(sourcePath).contains(
        QString::fromLatin1(kImageFixtureText)));
}

void PDFEditControllerTest::rotatesImagesWithHistory_data()
{
    QTest::addColumn<bool>("square");
    QTest::newRow("rectangular") << false;
    QTest::newRow("square") << true;
}

void PDFEditControllerTest::rotatesImagesWithHistory()
{
    QFETCH(bool, square);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("rotate-history-%1.pdf").arg(square));
    QVERIFY(createTopLevelImagePdf(sourcePath,
        {120, 0, 0, square ? 120.0F : 60.0F, 200, 400}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    const QString objectId = candidate.value(QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectCandidate(0, objectId));
    QVERIFY(controller.selectedImageRotatable());
    const QRectF original = controller.selectedFrameRect();
    const QSizeF pageSize = document.pageSize(0);
    controller.rotateSelectedImage(1);
    const QRectF rotated = controller.selectedFrameRect();
    QVERIFY(QLineF(original.center(), rotated.center()).length() < 0.000001);
    QVERIFY(qAbs(rotated.width() * pageSize.width()
                 - original.height() * pageSize.height()) < 0.000001);
    QVERIFY(qAbs(rotated.height() * pageSize.height()
                 - original.width() * pageSize.width()) < 0.000001);
    QCOMPARE(controller.selectedContentBounds(), rotated);
    QCOMPARE(controller.selectedSourceRect(), original);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 1);
    const auto moves = controller.draftedObjectMovesForPage(0);
    QCOMPARE(moves.size(), 1);
    QCOMPARE(moves.constFirst().imageQuarterTurns, 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.textEditActive());

    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), original);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.draftedObjectMovesForPage(0).isEmpty());
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), rotated);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 1);

    // A full turn and an inverse turn both restore the source without leaving
    // an orientation-only draft, even when the frame never changes (square).
    for (int turn = 0; turn < 3; ++turn)
        controller.rotateSelectedImage(1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(QLineF(controller.selectedFrameRect().topLeft(), original.topLeft()).length() < 0.000001);
    controller.rotateSelectedImage(-1);
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 3);
    controller.rotateSelectedImage(1);
    QVERIFY(!controller.hasDrafts());

    // A half turn is a real edit even though its selection frame is identical.
    controller.rotateSelectedImage(2);
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 2);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedFrameRect(), original);
}

void PDFEditControllerTest::savesRotatedSourceImageRoundTrip_data()
{
    QTest::addColumn<QList<qreal>>("imageMatrix");
    QTest::addColumn<int>("pageRotation");
    QTest::addColumn<int>("quarterTurns");
    QTest::addColumn<bool>("overwrite");
    const QList<QPair<QByteArray, QList<qreal>>> matrices{
        {"upright", {120, 0, 0, 60, 300, 500}},
        {"reflected-skew", {-120, 25, 35, 60, 420, 500}},
    };
    for (const auto &entry : matrices) {
        for (int pageRotation = 0; pageRotation < 4; ++pageRotation) {
            for (int turn : {1, -1, 2}) {
                const QByteArray name = entry.first + "-page-"
                    + QByteArray::number(pageRotation * 90) + "-turn-"
                    + QByteArray::number(turn);
                QTest::newRow(name.constData())
                    << entry.second << pageRotation << turn << (pageRotation % 2 == 0);
            }
        }
    }
}

void PDFEditControllerTest::savesRotatedSourceImageRoundTrip()
{
    QFETCH(QList<qreal>, imageMatrix);
    QFETCH(int, pageRotation);
    QFETCH(int, quarterTurns);
    QFETCH(bool, overwrite);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("rotate-source-%1.pdf").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QString outputPath = overwrite ? sourcePath : sourcePath + QStringLiteral(".copy.pdf");
    QVERIFY(createTopLevelImagePdf(sourcePath,
        {float(imageMatrix[0]), float(imageMatrix[1]), float(imageMatrix[2]),
         float(imageMatrix[3]), float(imageMatrix[4]), float(imageMatrix[5])},
        pageRotation, true));
    const ImageSaveInspection original = inspectImages(sourcePath);
    QVERIFY(original.matrixReadable);
    QVERIFY(original.pixelsReadable);
    QCOMPARE(original.normalizedCorners.size(), 4);
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = sourceFile.readAll();
    sourceFile.close();
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageRotatable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    const QSizeF pageSize = document.pageSize(0);
    controller.rotateSelectedImage(quarterTurns);
    const QRectF rotatedFrame = controller.selectedFrameRect();
    // Subsequent movement and nonuniform resizing retain the chosen orientation.
    const QRectF targetFrame(0.18, 0.23,
                            rotatedFrame.width() * 1.2,
                            rotatedFrame.height() * 0.75);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(targetFrame);
    controller.finishSelectedFrameResize();
    QCOMPARE(controller.selectedFrameRect(), targetFrame);
    QCOMPARE(controller.draftedObjectMovesForPage(0).constFirst().imageQuarterTurns,
             (quarterTurns + 4) % 4);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), rotatedFrame);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), targetFrame);
    QVERIFY(overwrite ? controller.save() : controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    const ImageSaveInspection saved = inspectImages(outputPath);
    QVERIFY(saved.matrixReadable);
    QCOMPARE(saved.count, original.count);
    QCOMPARE(saved.firstTopLevelIndex, original.firstTopLevelIndex);
    QCOMPARE(saved.pixelWidth, original.pixelWidth);
    QCOMPARE(saved.pixelHeight, original.pixelHeight);
    QCOMPARE(saved.pixels, original.pixels);
    QCOMPARE(saved.normalizedCorners.size(), 4);
    // Use a physical page-space rotation as the oracle; normalized x/y have
    // different scales on these nonsquare cropped and intrinsically rotated pages.
    const QTransform rotation = QTransform().rotate(quarterTurns * 90);
    for (int index = 0; index < original.normalizedCorners.size(); ++index) {
        const QPointF delta = original.normalizedCorners[index] - sourceFrame.center();
        const QPointF rotated = rotation.map(QPointF(
            delta.x() * pageSize.width(), delta.y() * pageSize.height()));
        const QPointF expected = targetFrame.center() + QPointF(
            rotated.x() / pageSize.width() * 1.2,
            rotated.y() / pageSize.height() * 0.75);
        QVERIFY2(QLineF(expected, saved.normalizedCorners[index]).length() < 0.0001,
                 qPrintable(QStringLiteral("corner %1: expected (%2,%3), got (%4,%5)")
                     .arg(index).arg(expected.x()).arg(expected.y())
                     .arg(saved.normalizedCorners[index].x()).arg(saved.normalizedCorners[index].y())));
    }
    QVERIFY(normalizedPageText(outputPath).contains(QString::fromLatin1(kImageFixtureText)));
    const QVariantMap reopened = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!reopened.isEmpty());
    const QRectF savedFrame = reopened.value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(QLineF(savedFrame.topLeft(), targetFrame.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(savedFrame.bottomRight(), targetFrame.bottomRight()).length() < 0.0001);
    QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageRotatable());
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 0);
    if (!overwrite) {
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        QCOMPARE(sourceFile.readAll(), originalBytes);
    }
}

void PDFEditControllerTest::savesRotatedAddedImageRoundTrip_data()
{
    QTest::addColumn<int>("quarterTurns");
    QTest::newRow("clockwise") << 1;
    QTest::newRow("counterclockwise") << -1;
    QTest::newRow("half-turn") << 2;
}

void PDFEditControllerTest::savesRotatedAddedImageRoundTrip()
{
    QFETCH(int, quarterTurns);
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-added-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-added-input.png"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("rotate-added-%1.pdf").arg(quarterTurns));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage image(80, 40, QImage::Format_ARGB32);
    QPainter imagePainter(&image);
    imagePainter.fillRect(QRect(0, 0, 40, 20), Qt::red);
    imagePainter.fillRect(QRect(40, 0, 40, 20), Qt::green);
    imagePainter.fillRect(QRect(0, 20, 40, 20), Qt::blue);
    imagePainter.fillRect(QRect(40, 20, 40, 20), Qt::yellow);
    imagePainter.end();
    QVERIFY(image.save(imagePath, "PNG"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap added = controller.addImage(0, 0.45, 0.5, QUrl::fromLocalFile(imagePath));
    QVERIFY(!added.isEmpty());
    QVERIFY(controller.selectedImageRotatable());
    const QRectF initialFrame = controller.selectedFrameRect();
    controller.rotateSelectedImage(quarterTurns);
    const QRectF frame = controller.selectedFrameRect();
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(),
             (quarterTurns + 4) % 4);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), initialFrame);
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), frame);
    QVERIFY(QFile::remove(imagePath));

    const QImage expected = image.transformed(QTransform().rotate(quarterTurns * 90));
    DraftImageItem item;
    item.setWidth(expected.width());
    item.setHeight(expected.height());
    item.setSourceImage(QVariant::fromValue(image));
    item.setImageQuarterTurns((quarterTurns + 4) % 4);
    QImage preview(expected.size(), QImage::Format_ARGB32);
    preview.fill(Qt::transparent);
    QPainter painter(&preview);
    item.paint(&painter);
    painter.end();
    QCOMPARE(preview, expected);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    const ImageSaveInspection saved = inspectImages(outputPath);
    QCOMPARE(saved.count, 1);
    QCOMPARE(saved.pixelWidth, 80U);
    QCOMPARE(saved.pixelHeight, 40U);
    QCOMPARE(saved.pixels, image);
    QCOMPARE(inspectImages(sourcePath).count, 0);
    const QImage rendered = renderFirstPage(outputPath, QSize(1190, 1684));
    QVERIFY(!rendered.isNull());
    for (const QPointF point : {QPointF(0.25, 0.25), QPointF(0.75, 0.25),
                                QPointF(0.25, 0.75), QPointF(0.75, 0.75)}) {
        const QColor expectedColor = expected.pixelColor(
            int(point.x() * expected.width()), int(point.y() * expected.height()));
        const QColor actualColor = rendered.pixelColor(
            qRound((frame.x() + point.x() * frame.width()) * rendered.width()),
            qRound((frame.y() + point.y() * frame.height()) * rendered.height()));
        QCOMPARE(actualColor, expectedColor);
    }
    const QVariantMap reopened = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!reopened.isEmpty());
    QVERIFY(controller.selectCandidate(0, reopened.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageRotatable());
    const QRectF reopenedFrame = controller.selectedFrameRect();
    QVERIFY(QLineF(reopenedFrame.topLeft(), frame.topLeft()).length() < 0.0001);
    QVERIFY(QLineF(reopenedFrame.bottomRight(), frame.bottomRight()).length() < 0.0001);
}

void PDFEditControllerTest::rejectsUnsupportedImageRotation_data()
{
    QTest::addColumn<QString>("fixture");
    for (const char *fixture : {"clipped", "mixed-form", "singular", "off-page", "too-tall"})
        QTest::newRow(fixture) << QString::fromLatin1(fixture);
}

void PDFEditControllerTest::rejectsUnsupportedImageRotation()
{
    QFETCH(QString, fixture);
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("rotate-unsupported-%1.pdf").arg(fixture));
    if (fixture == QLatin1String("clipped") || fixture == QLatin1String("mixed-form")) {
        const bool nested = fixture == QLatin1String("mixed-form");
        const QByteArray content = nested ? QByteArray("/Fm0 Do\n")
            : QByteArray("q 110 120 90 40 re W n 120 0 0 60 100 100 cm /Im0 Do Q\n");
        const QByteArray formContent(
            "q 120 0 0 60 100 100 cm /Im0 Do Q\n0 0 20 20 re f\n");
        QList<QByteArray> objects{
            QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
            QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
            QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] ")
                + (nested ? QByteArray("/Resources << /XObject << /Fm0 6 0 R >> >> ")
                          : QByteArray("/Resources << /XObject << /Im0 5 0 R >> >> "))
                + QByteArray("/Contents 4 0 R >>"),
            QByteArray("<< /Length ") + QByteArray::number(content.size())
                + QByteArray(" >>\nstream\n") + content + QByteArray("endstream"),
            QByteArray("<< /Type /XObject /Subtype /Image /Width 2 /Height 2 ")
                + QByteArray("/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 12 >>\nstream\n")
                + QByteArray::fromHex("ff000000ff000000ffffffff") + QByteArray("\nendstream"),
        };
        if (nested) {
            objects.append(QByteArray("<< /Type /XObject /Subtype /Form /BBox [0 0 400 400] ")
                + QByteArray("/Resources << /XObject << /Im0 5 0 R >> >> /Length ")
                + QByteArray::number(formContent.size()) + QByteArray(" >>\nstream\n")
                + formContent + QByteArray("endstream"));
        }
        QVERIFY(writePdfObjects(sourcePath, objects));
    } else {
        const FS_MATRIX matrix = fixture == QLatin1String("singular")
            ? FS_MATRIX{120, 120, 60, 60, 100, 100}
            : fixture == QLatin1String("off-page")
                ? FS_MATRIX{120, 0, 0, 60, -20, 620}
                : FS_MATRIX{60, 0, 0, 700, 100, 100};
        QVERIFY(createTopLevelImagePdf(sourcePath, matrix));
    }
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    controller.rotateSelectedImage(1);
    QVERIFY(!controller.hasDrafts());
    const QVariantMap candidate = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(!controller.selectedImageRotatable());
    const QRectF original = controller.selectedFrameRect();
    controller.rotateSelectedImage(1);
    controller.rotateSelectedImage(-1);
    QCOMPARE(controller.selectedFrameRect(), original);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.draftedObjectMovesForPage(0).isEmpty());
}

void PDFEditControllerTest::keepsRotatedImageInsidePage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-near-edge.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath, {160, 0, 0, 60, 400, 0}));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QVERIFY(controller.selectedImageRotatable());
    const QRectF sourceFrame = controller.selectedFrameRect();
    controller.rotateSelectedImage(1);
    const QRectF rotated = controller.selectedFrameRect();
    QVERIFY(rotated.left() >= 0.0);
    QVERIFY(rotated.top() >= 0.0);
    QVERIFY(rotated.right() <= 1.0 + 0.000001);
    QVERIFY(rotated.bottom() <= 1.0 + 0.000001);
    // Candidate bounds are quantized through PDFium's integer page projection.
    // Rotation must preserve those measured physical dimensions exactly.
    const QSizeF pageSize = document.pageSize(0);
    QVERIFY(qAbs(rotated.width() * pageSize.width()
                 - sourceFrame.height() * pageSize.height()) < 0.000001);
    QVERIFY(qAbs(rotated.height() * pageSize.height()
                 - sourceFrame.width() * pageSize.width()) < 0.000001);
    QVERIFY(qAbs(rotated.bottom() - 1.0) < 0.000001);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), sourceFrame);
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::restoresSourceImagePreviewAfterRotation()
{
    if (QGuiApplication::screens().isEmpty())
        QSKIP("The source image rotation preview test needs a Qt Quick render screen.");
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-preview-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("rotate-preview-reference.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath, {-120, 25, 35, 60, 420, 500}, 1, true));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(650, 500);
    TestablePDFPageItem pageItem(window.contentItem());
    pageItem.setSize(window.size());
    pageItem.setEditController(&controller);
    pageItem.setDocument(&document);
    pageItem.setPage(0);
    pageItem.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    const auto grabPage = [&]() {
        QElapsedTimer timer;
        timer.start();
        while ((!pageItem.draftTextureCurrent() || pageItem.m_renderPending)
               && timer.elapsed() < 5000)
            QTest::qWait(10);
        return pageItem.draftTextureCurrent() && !pageItem.m_renderPending
            ? window.grabWindow() : QImage();
    };
    const QImage original = grabPage();
    QVERIFY(!original.isNull());
    controller.rotateSelectedImage(1);
    const QImage preview = grabPage();
    QVERIFY(!preview.isNull());
    QVERIFY(changedPixelCount(original, preview, preview.rect()) > 100);
    const PDFSaveWriter::Result result = PDFSaveWriter::save(controller, outputPath);
    QVERIFY2(result.succeeded(), qPrintable(result.detail));
    const QImage saved = renderFirstPage(outputPath, preview.size());
    QVERIFY(!saved.isNull());
    QVERIFY(changedPixelCount(saved, preview, saved.rect()) < saved.width() * saved.height() / 200);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(grabPage(), original);
    controller.redo();
    QCOMPARE(grabPage(), preview);
    controller.discardDrafts();
    QCOMPARE(grabPage(), original);
}

void PDFEditControllerTest::rotatesImageThroughPropertiesPanel()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("rotate-panel.pdf"));
    QVERIFY(createTopLevelImagePdf(sourcePath));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForKind(
        waitForCandidates(controller, 0), QStringLiteral("image"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller.selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QRectF original = controller.selectedFrameRect();

    const QString themePath = QFINDTESTDATA("../src/Components/AppTheme.qml");
    const QString panelPath = QFINDTESTDATA("../src/Components/ImagePropertiesPanel.qml");
    QVERIFY(registerQmlTestTypes(themePath));
    QQuickWindow window;
    window.resize(264, 500);
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    QQmlComponent component(&engine, QUrl::fromLocalFile(panelPath));
    const QVariantMap properties{
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("editController"), QVariant::fromValue(&controller)},
        {QStringLiteral("collapsed"), false},
        {QStringLiteral("width"), 264.0},
        {QStringLiteral("height"), 500.0}};
    std::unique_ptr<QObject> panel(component.createWithInitialProperties(properties));
    QVERIFY2(panel, qPrintable(component.errorString()));
    auto *panelItem = qobject_cast<QQuickItem *>(panel.get());
    QVERIFY(panelItem);
    panelItem->setParentItem(window.contentItem());
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed(), 5000);
    auto *left = panel->findChild<QQuickItem *>(QStringLiteral("rotateImageLeftButton"));
    auto *right = panel->findChild<QQuickItem *>(QStringLiteral("rotateImageRightButton"));
    QVERIFY(left);
    QVERIFY(right);
    QTRY_VERIFY(left->isVisible() && right->isVisible());
    QVERIFY(left->isEnabled() && right->isEnabled());
    const auto center = [](QQuickItem *item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    };
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center(right));
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 1);
    QVERIFY(controller.hasDrafts());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center(left));
    QCOMPARE(controller.selectedFrameRect(), original);
    QVERIFY(!controller.hasDrafts());
    controller.undo();
    QCOMPARE(controller.selectedDraftPreview().value(QStringLiteral("imageQuarterTurns")).toInt(), 1);
    controller.redo();
    QCOMPARE(controller.selectedFrameRect(), original);
    controller.clearSelection();
    QTRY_VERIFY(!left->isEnabled() && !right->isEnabled());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center(right));
    QVERIFY(!controller.hasDrafts());
}
