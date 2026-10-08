#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

namespace {

QImage headerImagePixels(const QSize &size = QSize(80, 40))
{
    QImage image(size, QImage::Format_ARGB32);
    QPainter painter(&image);
    painter.fillRect(QRect(0, 0, size.width() / 2, size.height() / 2), Qt::red);
    painter.fillRect(QRect(size.width() / 2, 0, size.width() / 2, size.height() / 2), Qt::green);
    painter.fillRect(QRect(0, size.height() / 2, size.width() / 2, size.height() / 2), Qt::blue);
    painter.fillRect(QRect(size.width() / 2, size.height() / 2,
                          size.width() / 2, size.height() / 2), Qt::yellow);
    return image;
}

QVariantList imageHeaderSnapshot(const PDFEditController &controller, int pageCount)
{
    QVariantList snapshot;
    for (int page = 0; page < pageCount; ++page)
        snapshot.append(QVariant(controller.draftsForPage(page)));
    return snapshot;
}

bool createImageHeaderPages(const QString &path, const QList<QSizeF> &sizes)
{
    QByteArray kids;
    QVector<QByteArray> pages;
    for (int index = 0; index < sizes.size(); ++index) {
        kids += QByteArray::number(index + 3) + " 0 R ";
        const QSizeF size = sizes.at(index);
        pages.append(QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ")
            + QByteArray::number(size.width()) + ' ' + QByteArray::number(size.height())
            + "] /Resources << >> >>");
    }
    QVector<QByteArray> objects{
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [") + kids + "] /Count "
            + QByteArray::number(sizes.size()) + " >>"};
    objects += pages;
    return writePdfObjects(path, objects);
}

} // namespace

void PDFEditControllerTest::preparesHeaderImageWithoutDraftsAndSnapshotsPixels()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-image-prepare.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("header-image-prepare.png"));
    QVERIFY(createThreePageTextPdf(source));
    const QImage pixels = headerImagePixels(QSize(2048, 1024));
    QVERIFY(pixels.save(imagePath, "PNG"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QSignalSpy history(&controller, &PDFEditController::historyStateChanged);
    const QVariantMap prepared = controller.prepareHeaderImage(QUrl::fromLocalFile(imagePath));
    QCOMPARE(prepared.value(QStringLiteral("pixelWidth")).toInt(), 2048);
    QCOMPARE(prepared.value(QStringLiteral("pixelHeight")).toInt(), 1024);
    const QString previewUrl = prepared.value(QStringLiteral("previewSource")).toString();
    const QString prefix = QStringLiteral("data:image/png;base64,");
    QVERIFY(previewUrl.startsWith(prefix));
    const QImage preview = QImage::fromData(QByteArray::fromBase64(previewUrl.mid(prefix.size()).toLatin1()));
    QCOMPARE(preview.size(), QSize(1024, 512));
    QCOMPARE(preview.pixelColor(100, 100), QColor(Qt::red));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QCOMPARE(history.count(), 0);

    // Replacement failures retain the already-decoded temporary pixels.
    QVERIFY(controller.prepareHeaderImage(QUrl(QStringLiteral("https://example.com/image.png"))).isEmpty());
    QVERIFY(controller.prepareHeaderImage(QUrl::fromLocalFile(source)).isEmpty());
    QVERIFY(QFile::remove(imagePath));
    QVERIFY(controller.addHeaderImage(QStringLiteral("1"), 20, 15, 120, 60));
    QCOMPARE(controller.draftsForPage(0).constFirst().toMap().value(QStringLiteral("image")).value<QImage>(), pixels);
    controller.clearHeaderImage();
    QVERIFY(!controller.addHeaderImage(QStringLiteral("2"), 20, 15, 120, 60));
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.draftsForPage(1).isEmpty());

    QVERIFY(pixels.save(imagePath, "PNG"));
    QVERIFY(!controller.prepareHeaderImage(QUrl::fromLocalFile(imagePath)).isEmpty());
    controller.setDocument(nullptr);
    controller.setDocument(&document);
    QVERIFY(!controller.addHeaderImage(QStringLiteral("1"), 20, 15, 120, 60));
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::headerImageUsesAtomicHistoryAndCustomPages()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-image-history.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("header-image-history.png"));
    QVERIFY(createImageHeaderPages(source, {QSizeF(300, 400), QSizeF(400, 600), QSizeF(500, 800)}));
    const QImage pixels = headerImagePixels();
    QVERIFY(pixels.save(imagePath, "PNG"));
    PDFDocument document;
    PDFEditController controller;
    PDFEditController otherController;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    otherController.setDocument(&document);
    QVERIFY(!controller.prepareHeaderImage(QUrl::fromLocalFile(imagePath)).isEmpty());
    QVERIFY(!otherController.addHeaderImage(QStringLiteral("1"), 20, 10, 80, 40));
    QVERIFY(controller.addHeaderImage(QStringLiteral("2"), 30, 20, 80, 40));
    const QVariantList before = imageHeaderSnapshot(controller, 3);
    QVERIFY(controller.addHeaderImage(QStringLiteral("all"), 40, 15, 80, 40, 2));
    const QVariantList applied = imageHeaderSnapshot(controller, 3);
    for (int page = 0; page < 3; ++page) {
        const QVariantList drafts = controller.draftsForPage(page);
        QCOMPARE(drafts.size(), page == 1 ? 2 : 1);
        const QVariantMap added = drafts.constLast().toMap();
        QCOMPARE(added.value(QStringLiteral("kind")).toString(), QStringLiteral("addedImage"));
        QCOMPARE(added.value(QStringLiteral("imageQuarterTurns")).toInt(), 2);
        const QRectF frame = added.value(QStringLiteral("frameRect")).toRectF();
        const QSizeF size = document.pageSize(page);
        QVERIFY(qAbs(frame.x() * size.width() - 40) < 0.001);
        QVERIFY(qAbs(frame.y() * size.height() - 15) < 0.001);
        QVERIFY(qAbs(frame.width() * size.width() - 80) < 0.001);
        QVERIFY(qAbs(frame.height() * size.height() - 40) < 0.001);
        QCOMPARE(added.value(QStringLiteral("image")).value<QImage>().cacheKey(),
                 drafts.constFirst().toMap().value(QStringLiteral("image")).value<QImage>().cacheKey());
    }
    controller.undo();
    QCOMPARE(imageHeaderSnapshot(controller, 3), before);
    QVERIFY(controller.canUndo());
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(imageHeaderSnapshot(controller, 3), applied);
    controller.undo();
    QVERIFY(controller.addHeaderImage(QStringLiteral("3, 1, 3-3, 1"), 10, 10, 80, 40));
    for (int page = 0; page < 3; ++page)
        QCOMPARE(controller.draftsForPage(page).size(), 1);
    QVERIFY(!controller.canRedo());
    controller.undo();
    QCOMPARE(imageHeaderSnapshot(controller, 3), before);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
}

void PDFEditControllerTest::rejectsHeaderImageWithoutChangingHistory_data()
{
    QTest::addColumn<QString>("pages");
    QTest::addColumn<QRectF>("frame");
    QTest::addColumn<int>("turns");
    const QRectF valid(20, 10, 80, 40);
    for (const char *range : {"", "0", "4", "3-2", "1,", "1-4"})
        QTest::newRow(range[0] ? range : "empty") << QString::fromLatin1(range) << valid << 0;
    QTest::newRow("negative-x") << QStringLiteral("all") << QRectF(-1, 10, 80, 40) << 0;
    QTest::newRow("negative-y") << QStringLiteral("all") << QRectF(20, -1, 80, 40) << 0;
    QTest::newRow("zero-width") << QStringLiteral("all") << QRectF(20, 10, 0, 40) << 0;
    QTest::newRow("zero-height") << QStringLiteral("all") << QRectF(20, 10, 80, 0) << 0;
    QTest::newRow("below-header") << QStringLiteral("all") << QRectF(20, 100, 80, 40) << 0;
    QTest::newRow("crosses-header") << QStringLiteral("all") << QRectF(20, 80, 80, 40) << 0;
    QTest::newRow("crosses-right") << QStringLiteral("all") << QRectF(290, 10, 80, 40) << 0;
    QTest::newRow("nan") << QStringLiteral("all")
        << QRectF(std::numeric_limits<qreal>::quiet_NaN(), 10, 80, 40) << 0;
    QTest::newRow("infinite") << QStringLiteral("all")
        << QRectF(20, 10, std::numeric_limits<qreal>::infinity(), 40) << 0;
    QTest::newRow("negative-turn") << QStringLiteral("all") << valid << -1;
    QTest::newRow("too-many-turns") << QStringLiteral("all") << valid << 4;
}

void PDFEditControllerTest::rejectsHeaderImageWithoutChangingHistory()
{
    QFETCH(QString, pages);
    QFETCH(QRectF, frame);
    QFETCH(int, turns);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-image-invalid.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("header-image-invalid.png"));
    QVERIFY(createImageHeaderPages(source, {QSizeF(300, 400), QSizeF(300, 400), QSizeF(300, 400)}));
    QVERIFY(headerImagePixels().save(imagePath, "PNG"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(!controller.prepareHeaderImage(QUrl::fromLocalFile(imagePath)).isEmpty());
    QVERIFY(controller.addHeaderImage(QStringLiteral("1"), 20, 10, 80, 40));
    QVERIFY(controller.addHeaderImage(QStringLiteral("2"), 20, 10, 80, 40));
    controller.undo();
    const QVariantList before = imageHeaderSnapshot(controller, 3);
    QSignalSpy history(&controller, &PDFEditController::historyStateChanged);
    QSignalSpy changed(&controller, &PDFEditController::pageDataChanged);
    QVERIFY(!controller.addHeaderImage(pages, frame.x(), frame.y(), frame.width(), frame.height(), turns));
    QCOMPARE(imageHeaderSnapshot(controller, 3), before);
    QCOMPARE(history.count(), 0);
    QCOMPARE(changed.count(), 0);
    QVERIFY(controller.canUndo());
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(controller.draftsForPage(1).size(), 1);
}

void PDFEditControllerTest::rejectsHeaderImageWhenAnyPageCannotFit()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-image-small-page.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("header-image-small-page.png"));
    QVERIFY(createImageHeaderPages(source, {QSizeF(300, 400), QSizeF(50, 400), QSizeF(300, 30)}));
    QVERIFY(headerImagePixels().save(imagePath, "PNG"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(!controller.prepareHeaderImage(QUrl::fromLocalFile(imagePath)).isEmpty());
    QVERIFY(!controller.addHeaderImage(QStringLiteral("1-2"), 20, 10, 80, 40));
    QVERIFY(!controller.addHeaderImage(QStringLiteral("1,3"), 20, 10, 80, 40));
    for (int page = 0; page < 3; ++page)
        QVERIFY(controller.draftsForPage(page).isEmpty());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.addHeaderImage(QStringLiteral("1"), 20, 10, 80, 40));
    QCOMPARE(controller.draftsForPage(0).constFirst().toMap().value(QStringLiteral("objectId")).toString(),
             QStringLiteral("added-image:1"));
}

void PDFEditControllerTest::savesRotatedHeaderImageOnSelectedPages_data()
{
    QTest::addColumn<int>("turns");
    for (int turns = 0; turns < 4; ++turns)
        QTest::newRow(qPrintable(QString::number(turns))) << turns;
}

void PDFEditControllerTest::savesRotatedHeaderImageOnSelectedPages()
{
    QFETCH(int, turns);
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("header-image-save-source-%1.pdf").arg(turns));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("header-image-save-target-%1.pdf").arg(turns));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("header-image-save.png"));
    QVERIFY(createImageHeaderPages(source, {QSizeF(300, 400), QSizeF(400, 600), QSizeF(500, 800)}));
    const QImage pixels = headerImagePixels();
    QVERIFY(pixels.save(imagePath, "PNG"));
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    controller.setDocument(&document);
    QVERIFY(!controller.prepareHeaderImage(QUrl::fromLocalFile(imagePath)).isEmpty());
    const QSizeF frameSize = turns % 2 ? QSizeF(40, 80) : QSizeF(80, 40);
    QVERIFY(controller.addHeaderImage(QStringLiteral("1,3"), 20, 10,
                                      frameSize.width(), frameSize.height(), turns));
    const QRectF frame = controller.draftsForPage(0).constFirst().toMap().value(QStringLiteral("frameRect")).toRectF();
    controller.clearHeaderImage();
    QVERIFY(QFile::remove(imagePath));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QCOMPARE(inspectImages(source).count, 0);
    const ImageSaveInspection saved = inspectImages(target);
    QCOMPARE(saved.count, 1);
    QCOMPARE(saved.pixels, pixels);
    const QImage expected = pixels.transformed(QTransform().rotate(turns * 90));
    const QImage rendered = renderFirstPage(target, QSize(600, 800));
    QVERIFY(!rendered.isNull());
    for (const QPointF point : {QPointF(0.25, 0.25), QPointF(0.75, 0.25),
                                QPointF(0.25, 0.75), QPointF(0.75, 0.75)}) {
        const QColor expectedColor = expected.pixelColor(
            int(point.x() * expected.width()), int(point.y() * expected.height()));
        QCOMPARE(rendered.pixelColor(
                     qRound((frame.x() + point.x() * frame.width()) * rendered.width()),
                     qRound((frame.y() + point.y() * frame.height()) * rendered.height())), expectedColor);
    }
    for (int page = 0; page < 3; ++page) {
        const QVariantMap reopened = candidateForKind(waitForCandidates(controller, page), QStringLiteral("image"));
        if (page == 1) {
            QVERIFY(reopened.isEmpty());
            continue;
        }
        QVERIFY(!reopened.isEmpty());
        QVERIFY(controller.selectCandidate(page, reopened.value(QStringLiteral("objectId")).toString()));
        QVERIFY(controller.selectedImageResizable());
        QVERIFY(controller.selectedImageRotatable());
        const QRectF reopenedFrame = controller.selectedFrameRect();
        const QSizeF pageSize = document.pageSize(page);
        QVERIFY(qAbs(reopenedFrame.x() * pageSize.width() - 20) < 0.01);
        QVERIFY(qAbs(reopenedFrame.y() * pageSize.height() - 10) < 0.01);
        QVERIFY(qAbs(reopenedFrame.width() * pageSize.width() - frameSize.width()) < 0.01);
        QVERIFY(qAbs(reopenedFrame.height() * pageSize.height() - frameSize.height()) < 0.01);
    }
}
