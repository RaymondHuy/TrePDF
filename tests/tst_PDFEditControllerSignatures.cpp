#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "SignatureLibrary.h"

#include <QPointer>

using namespace PDFEditControllerTestSupport;

namespace {

QImage signaturePreview(const SignatureLibrary &library)
{
    const QByteArray encoded = library.previewUrl().toEncoded();
    if (!encoded.startsWith("data:image/png;base64,")) {
        return {};
    }
    return QImage::fromData(QByteArray::fromBase64(encoded.mid(encoded.indexOf(',') + 1)),
                            "PNG");
}

bool hasTransparentAndOpaquePixels(const QImage &image)
{
    bool transparent = false;
    bool opaque = false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int alpha = image.pixelColor(x, y).alpha();
            transparent |= alpha == 0;
            opaque |= alpha == 255;
        }
    }
    return transparent && opaque;
}

QVariantMap signaturePoint(qreal x, qreal y)
{
    return {{QStringLiteral("x"), x}, {QStringLiteral("y"), y}};
}

QVariantList separatedSignatureStrokes()
{
    return {QVariantList{signaturePoint(0.1, 0.2), signaturePoint(0.25, 0.75)},
            QVariantList{signaturePoint(0.75, 0.25), signaturePoint(0.9, 0.8)}};
}

QVariantMap firstImageCandidate(PDFEditController &controller)
{
    const QVariantList candidates = waitForCandidates(controller, 0);
    for (const QVariant &candidate : candidates) {
        if (candidate.toMap().value(QStringLiteral("kind")).toString()
            == QLatin1String("image")) {
            return candidate.toMap();
        }
    }
    return {};
}

} // namespace

void PDFEditControllerTest::preparesVisualSignatureText_data()
{
    QTest::addColumn<int>("style");
    QTest::newRow("handwriting") << 0;
    QTest::newRow("serif") << 1;
    QTest::newRow("sans") << 2;
}

void PDFEditControllerTest::preparesVisualSignatureText()
{
    QFETCH(int, style);
    SignatureLibrary library(m_temporaryDirectory.filePath(
        QStringLiteral("signature-text-%1.json").arg(style)));
    QVERIFY(!library.hasPreview());
    const QColor ink(QStringLiteral("#184fbd"));
    QVERIFY(library.prepareText(QStringLiteral("Nguyễn Văn Huy"), style, ink));
    QVERIFY(library.hasPreview());
    QVERIFY(library.error().isEmpty());
    const QImage image = signaturePreview(library);
    QVERIFY(!image.isNull());
    QVERIFY(hasTransparentAndOpaquePixels(image));
    QVERIFY(image.width() > image.height());
    bool foundInk = false;
    for (int y = 0; y < image.height() && !foundInk; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() == 255) {
                QCOMPARE(image.pixelColor(x, y), ink);
                foundInk = true;
                break;
            }
        }
    }
    QVERIFY(foundInk);
    library.clearPreview();
    QVERIFY(!library.hasPreview());
    QVERIFY(library.previewUrl().isEmpty());
}

void PDFEditControllerTest::preparesVisualSignatureDrawing()
{
    SignatureLibrary library(m_temporaryDirectory.filePath(
        QStringLiteral("signature-drawing.json")));
    QVERIFY(library.prepareDrawing(separatedSignatureStrokes(), Qt::black, 2.0));
    const QImage image = signaturePreview(library);
    QVERIFY(!image.isNull());
    QVERIFY(hasTransparentAndOpaquePixels(image));
    // Pen-up between strokes must remain blank, rather than joining two signatures.
    for (int y = 0; y < image.height(); ++y) {
        QCOMPARE(image.pixelColor(image.width() / 2, y).alpha(), 0);
    }
    QVERIFY(library.prepareDrawing(
        {QVariant(QVariantList{signaturePoint(0.2, 0.3)})}, Qt::black, 2.0));
    QVERIFY(hasTransparentAndOpaquePixels(signaturePreview(library)));
    QVariantList maximumStrokes;
    for (int stroke = 0; stroke < 8; ++stroke) {
        QVariantList points;
        for (int point = 0; point < 4096; ++point)
            points.append(signaturePoint(qreal(point) / 4095.0,
                                          point % 2 == 0 ? 0.1 : 0.9));
        maximumStrokes.append(QVariant(points));
    }
    QVERIFY(!library.prepareDrawing(maximumStrokes, Qt::black, 2.0));
    QCOMPARE(library.error(), qtTrId("ESignDrawingTooComplex"));
    QVERIFY(!library.hasPreview());
    maximumStrokes.clear();
    for (int stroke = 0; stroke < 8; ++stroke) {
        QVariantList points;
        for (int point = 0; point < 4096; ++point)
            points.append(signaturePoint(qreal(point) / 4095.0, 0.1 + stroke * 0.1));
        maximumStrokes.append(QVariant(points));
    }
    QVERIFY(library.prepareDrawing(maximumStrokes, Qt::black, 2.0));
    QVERIFY(!signaturePreview(library).isNull());
}

void PDFEditControllerTest::rejectsInvalidVisualSignatureInput()
{
    SignatureLibrary library(m_temporaryDirectory.filePath(
        QStringLiteral("signature-invalid.json")));
    const auto checkFailure = [&library]() {
        return !library.hasPreview() && library.previewUrl().isEmpty()
            && !library.error().isEmpty();
    };
    QVERIFY(library.prepareCheck(false));
    QVERIFY(!library.prepareText(QStringLiteral(" \n\t "), 0, Qt::black));
    QVERIFY(checkFailure());
    QVERIFY(!library.prepareText(QString(10000, QLatin1Char('a')), 0, Qt::black));
    QVERIFY(checkFailure());
    QVERIFY(!library.prepareText(QStringLiteral("Name"), 100, Qt::black));
    QVERIFY(checkFailure());
    QVERIFY(!library.prepareText(QStringLiteral("Name"), 0, Qt::transparent));
    QVERIFY(checkFailure());
    const QList<QVariantList> invalidStrokes = {
        {},
        {QVariant(QVariantList{})},
        {QVariant(QVariantList{signaturePoint(0.2, 0.3), signaturePoint(2.0, 0.4)})},
        {QVariant(QVariantList{signaturePoint(0.2, 0.3),
                      signaturePoint(std::numeric_limits<qreal>::quiet_NaN(), 0.4)})},
        {QVariant(QVariantList{signaturePoint(0.2, 0.3), QVariantMap{{QStringLiteral("y"), 0.4}}})}
    };
    for (const QVariantList &strokes : invalidStrokes) {
        QVERIFY(library.prepareCheck(false));
        QVERIFY(!library.prepareDrawing(strokes, Qt::black, 2.0));
        QVERIFY(checkFailure());
    }
    QVERIFY(!library.prepareDrawing(separatedSignatureStrokes(), Qt::black, -1.0));
    QVERIFY(checkFailure());
    QVariantList oversizedStroke;
    for (int index = 0; index < 4097; ++index) {
        oversizedStroke.append(signaturePoint(qreal(index) / 4097.0, 0.5));
    }
    QVERIFY(!library.prepareDrawing({QVariant(oversizedStroke)}, Qt::black, 2.0));
    QVERIFY(checkFailure());
    QVariantList tooManyStrokes;
    for (int index = 0; index < 65; ++index) {
        tooManyStrokes.append(QVariant(QVariantList{signaturePoint(0.2, 0.3)}));
    }
    QVERIFY(!library.prepareDrawing(tooManyStrokes, Qt::black, 2.0));
    QVERIFY(checkFailure());
    QVERIFY(!library.prepareImage(QUrl(QStringLiteral("https://example.invalid/signature.png")), true));
    QVERIFY(checkFailure());
    QVERIFY(!library.prepareImage(QUrl::fromLocalFile(
        m_temporaryDirectory.filePath(QStringLiteral("missing-signature.png"))), true));
    QVERIFY(checkFailure());
    const QString blankPath = m_temporaryDirectory.filePath(QStringLiteral("blank-signature.png"));
    QImage blank(80, 40, QImage::Format_ARGB32);
    blank.fill(Qt::white);
    QVERIFY(blank.save(blankPath));
    QVERIFY(!library.prepareImage(QUrl::fromLocalFile(blankPath), true));
    QVERIFY(checkFailure());
}

void PDFEditControllerTest::preparesSignatureDateAndCheck()
{
    SignatureLibrary library(m_temporaryDirectory.filePath(
        QStringLiteral("signature-date-check.json")));
    QList<QImage> formats;
    for (int format = 0; format < 3; ++format) {
        QVERIFY(library.prepareDate(QStringLiteral("2026-09-21"), format));
        const QImage image = signaturePreview(library);
        QVERIFY(hasTransparentAndOpaquePixels(image));
        formats.append(image);
    }
    QVERIFY(formats.at(0) != formats.at(1));
    QVERIFY(formats.at(1) != formats.at(2));
    QVERIFY(!library.prepareDate(QStringLiteral("2026-02-31"), 0));
    QVERIFY(!library.hasPreview());
    QVERIFY(!library.prepareDate(QStringLiteral("2026-09-21"), -1));
    QVERIFY(!library.hasPreview());
    QVERIFY(library.prepareCheck(false));
    const QImage check = signaturePreview(library);
    QVERIFY(hasTransparentAndOpaquePixels(check));
    QVERIFY(library.prepareCheck(true));
    const QImage cross = signaturePreview(library);
    QVERIFY(hasTransparentAndOpaquePixels(cross));
    QVERIFY(check != cross);
}

void PDFEditControllerTest::persistsLocalSignatureLibraryAtomically()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("signature-library.json"));
    SignatureLibrary library(path);
    QVERIFY(library.prepareText(QStringLiteral("Nguyễn Văn Huy"), 0, Qt::black));
    QVERIFY(library.savePreview(QStringLiteral("Chữ ký chính"), QStringLiteral("signature")));
    const QVariantMap signature = library.entries().constFirst().toMap();
    const QString signatureId = signature.value(QStringLiteral("id")).toString();
    QVERIFY(!signatureId.isEmpty());
    QVERIFY(library.prepareText(QStringLiteral("NVH"), 2, Qt::blue));
    QVERIFY(library.savePreview(QStringLiteral("Ký tắt"), QStringLiteral("initials")));
    QCOMPARE(library.entries().size(), 2);
    const QVariantList entries = library.entries();
    {
        SignatureLibrary reopened(path);
        QCOMPARE(reopened.entries(), entries);
        QVERIFY(reopened.selectEntry(signatureId));
        QCOMPARE(reopened.previewUrl(), signature.value(QStringLiteral("previewUrl")).toUrl());
    }
    // A directory occupying the destination deterministically rejects an atomic
    // replacement even when the tests are run by a privileged user.
    const QString backup = path + QStringLiteral(".backup");
    QVERIFY(QFile::rename(path, backup));
    QVERIFY(QDir().mkdir(path));
    QVERIFY(library.prepareCheck(false));
    QVERIFY(!library.savePreview(QStringLiteral("New signature"), QStringLiteral("signature")));
    QCOMPARE(library.entries(), entries);
    QVERIFY(!library.error().isEmpty());
    QVERIFY(!library.removeEntry(signatureId));
    QCOMPARE(library.entries(), entries);
    QVERIFY(QDir().rmdir(path));
    QVERIFY(QFile::rename(backup, path));
    QVERIFY(library.removeEntry(signatureId));
    QCOMPARE(library.entries().size(), 1);
    SignatureLibrary reopened(path);
    QCOMPARE(reopened.entries(), library.entries());
    QVERIFY(!reopened.selectEntry(signatureId));
    QVERIFY(!reopened.hasPreview());
}

void PDFEditControllerTest::preservesUnreadableSignatureLibrary()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("corrupt-signature-library.json"));
    const QByteArray original("{ broken library, preserve it for recovery");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();
    SignatureLibrary library(path);
    QVERIFY(!library.error().isEmpty());
    QVERIFY(library.entries().isEmpty());
    QVERIFY(library.prepareCheck(false));
    QVERIFY(!library.savePreview(QStringLiteral("Signature"), QStringLiteral("signature")));
    QVERIFY(library.entries().isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
}

void PDFEditControllerTest::savesVisualSignatureSnapshotThroughHistory()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("signature-source.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("signature-import.png"));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("signature-saved.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));
    QImage input(120, 60, QImage::Format_ARGB32);
    input.fill(Qt::white);
    {
        QPainter painter(&input);
        painter.setPen(QPen(Qt::blue, 5));
        painter.drawLine(15, 40, 95, 15);
        painter.drawLine(65, 45, 110, 20);
    }
    QVERIFY(input.save(imagePath));
    SignatureLibrary library(m_temporaryDirectory.filePath(QStringLiteral("signature-snapshot.json")));
    QVERIFY(library.prepareImage(QUrl::fromLocalFile(imagePath), true));
    const QImage prepared = signaturePreview(library);
    QVERIFY(hasTransparentAndOpaquePixels(prepared));
    QVERIFY(library.savePreview(QStringLiteral("Imported"), QStringLiteral("signature")));
    const QString id = library.entries().constFirst().toMap().value(QStringLiteral("id")).toString();
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap placed = library.place(&controller, 0, 0.3, 0.55, 144.0);
    QVERIFY(!placed.isEmpty());
    const QString objectId = placed.value(QStringLiteral("objectId")).toString();
    const QRectF frame = placed.value(QStringLiteral("frameRect")).toRectF();
    QCOMPARE(placed.value(QStringLiteral("kind")).toString(), QStringLiteral("addedImage"));
    QVERIFY(qAbs(frame.width() * document.pageSize(0).width() - 144.0) < 0.01);
    QVERIFY(QFile::remove(imagePath));
    QVERIFY(library.removeEntry(id));
    library.clearPreview();
    QVERIFY(controller.selectCandidate(0, objectId));
    const QRectF moved = frame.translated(0.1, 0.08);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(moved);
    controller.finishSelectedFrameResize();
    const QRectF resized(moved.topLeft(), moved.size() * 0.75);
    controller.beginSelectedFrameResize();
    controller.updateSelectedFrameRect(resized);
    controller.finishSelectedFrameResize();
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), moved);
    controller.undo();
    QCOMPARE(controller.selectedFrameRect(), frame);
    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    controller.redo();
    controller.redo();
    QVERIFY(controller.selectCandidate(0, objectId));
    QCOMPARE(controller.selectedFrameRect(), resized);
    controller.deleteSelectedObject();
    QVERIFY(controller.draftsForPage(0).isEmpty());
    controller.undo();
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    const ImageSaveInspection inspection = inspectImages(outputPath);
    QVERIFY(inspection.readable);
    QVERIFY(inspection.pixelsReadable);
    QCOMPARE(inspection.count, 1);
    QCOMPARE(QSize(inspection.pixelWidth, inspection.pixelHeight), prepared.size());
    const QImage rendered = renderFirstPage(outputPath, QSize(1190, 1684));
    QVERIFY(!rendered.isNull());
    const QRect imageRect(qRound(resized.x() * rendered.width()),
                         qRound(resized.y() * rendered.height()),
                         qRound(resized.width() * rendered.width()),
                         qRound(resized.height() * rendered.height()));
    int bluePixels = 0;
    int whitePixels = 0;
    for (int y = imageRect.top(); y <= imageRect.bottom(); ++y) {
        for (int x = imageRect.left(); x <= imageRect.right(); ++x) {
            const QColor pixel = rendered.pixelColor(x, y);
            bluePixels += pixel.blue() > 180 && pixel.red() < 90;
            whitePixels += pixel.red() > 245 && pixel.green() > 245 && pixel.blue() > 245;
        }
    }
    QVERIFY(bluePixels > 100);
    QVERIFY(whitePixels > 100);
    QCOMPARE(inspectImages(sourcePath).count, 0);
    PDFDocument reopenedDocument;
    PDFEditController reopenedController;
    QVERIFY(reopenedDocument.open(QUrl::fromLocalFile(outputPath)));
    reopenedController.setDocument(&reopenedDocument);
    const QVariantMap saved = firstImageCandidate(reopenedController);
    QVERIFY(!saved.isEmpty());
    const QRectF savedRect = saved.value(QStringLiteral("sourceRect")).toRectF();
    QVERIFY(QLineF(savedRect.topLeft(), resized.topLeft()).length() < 0.001);
    QVERIFY(QLineF(savedRect.bottomRight(), resized.bottomRight()).length() < 0.001);
}

void PDFEditControllerTest::discardsVisualSignatureWithoutChangingLibrary()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("signature-discard.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));
    SignatureLibrary library(m_temporaryDirectory.filePath(QStringLiteral("signature-discard.json")));
    QVERIFY(library.prepareText(QStringLiteral("NVH"), 2, Qt::black));
    QVERIFY(library.savePreview(QStringLiteral("Initials"), QStringLiteral("initials")));
    const QVariantList entries = library.entries();
    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QVERIFY(!library.place(&controller, 0, 0.2, 0.6, 90.0).isEmpty());
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(library.entries(), entries);
    QCOMPARE(inspectImages(sourcePath).count, 0);
}

void PDFEditControllerTest::routesSignRibbonActionsAndGuardsEmptyDocuments()
{
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    PDFDocument document;
    PDFEditController controller;
    controller.setDocument(&document);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Components/SignRibbon.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> ribbon(component.createWithInitialProperties({
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("document"), QVariant::fromValue(&document)},
        {QStringLiteral("editController"), QVariant::fromValue(&controller)}}));
    QVERIFY2(ribbon, qPrintable(component.errorString()));
    const QList<QPair<QString, QByteArray>> actions = {
        {QStringLiteral("signSelectButton"), SIGNAL(selectRequested())},
        {QStringLiteral("signSignatureButton"), SIGNAL(signatureRequested())},
        {QStringLiteral("signInitialsButton"), SIGNAL(initialsRequested())},
        {QStringLiteral("signDateButton"), SIGNAL(dateRequested())},
        {QStringLiteral("signCheckButton"), SIGNAL(checkRequested())}
    };
    for (const auto &action : actions) {
        QObject *button = ribbon->findChild<QObject *>(action.first);
        QVERIFY(button);
        QVERIFY(!button->property("enabled").toBool());
    }
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("signature-ribbon.pdf"));
    QVERIFY(createFontStylePdf(path));
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    for (const auto &action : actions) {
        QObject *button = ribbon->findChild<QObject *>(action.first);
        QTRY_VERIFY(button->property("enabled").toBool());
        QSignalSpy requested(ribbon.get(), action.second.constData());
        QVERIFY(requested.isValid());
        QVERIFY(QMetaObject::invokeMethod(button, "clicked", Qt::DirectConnection));
        QCOMPARE(requested.count(), 1);
    }
}

void PDFEditControllerTest::preparesTypedAndDrawnSignaturesThroughDialog()
{
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent themeComponent(&engine);
    themeComponent.setData(QByteArrayLiteral("import TrePDF\nAppTheme {}"), QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    SignatureLibrary library(m_temporaryDirectory.filePath(QStringLiteral("signature-dialog.json")));
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(R"qml(
        import QtQuick
        import QtQuick.Controls.Basic
        import TrePDF
        ApplicationWindow {
            id: window
            required property AppTheme theme
            required property SignatureLibrary library
            width: 900
            height: 650
            visible: true
            SignatureDialog { theme: window.theme; library: window.library }
        }
    )qml"), QUrl());
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("library"), QVariant::fromValue(&library)}}));
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window);
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("signatureDialog"));
    QVERIFY(dialog);
    QObject *tabs = dialog->findChild<QObject *>(QStringLiteral("signatureModeTabs"));
    QObject *textField = dialog->findChild<QObject *>(QStringLiteral("signatureTextField"));
    QObject *saveCheck = dialog->findChild<QObject *>(QStringLiteral("signatureSaveCheck"));
    QObject *savedName = dialog->findChild<QObject *>(QStringLiteral("signatureSavedName"));
    QVERIFY(tabs && textField && saveCheck && savedName);
    QSignalSpy placement(dialog, SIGNAL(placementRequested(double)));
    QVERIFY(placement.isValid());
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("initialized").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "submit"));
    QCOMPARE(placement.count(), 0);
    QVERIFY(tabs->setProperty("currentIndex", 1));
    QVERIFY(textField->setProperty("text", QStringLiteral("Nguyễn Văn Huy")));
    QVERIFY(saveCheck->setProperty("checked", true));
    QVERIFY(savedName->setProperty("text", QStringLiteral("Test signature")));
    QVERIFY(QMetaObject::invokeMethod(dialog, "submit"));
    QCOMPARE(placement.count(), 1);
    QCOMPARE(placement.constFirst().constFirst().toDouble(), 140.0);
    QVERIFY(library.hasPreview());
    QCOMPARE(library.entries().size(), 1);
    QVERIFY(hasTransparentAndOpaquePixels(signaturePreview(library)));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(tabs->setProperty("currentIndex", 0));
    QVERIFY(saveCheck->setProperty("checked", false));
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("initialized").toBool());
    auto *drawArea = dialog->findChild<QQuickItem *>(QStringLiteral("signatureDrawArea"));
    QVERIFY(drawArea);
    QTRY_VERIFY(drawArea->isVisible() && drawArea->width() > 100 && drawArea->height() > 30);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QSignalSpy frameRendered(window, &QQuickWindow::frameSwapped);
    window->requestUpdate();
    QVERIFY(frameRendered.wait(2000));
    const QPoint start = drawArea->mapToScene(QPointF(drawArea->width() * 0.2,
                                                     drawArea->height() * 0.7)).toPoint();
    const QPoint middle = drawArea->mapToScene(QPointF(drawArea->width() * 0.4,
                                                      drawArea->height() * 0.3)).toPoint();
    const QPoint end = drawArea->mapToScene(QPointF(drawArea->width() * 0.7,
                                                   drawArea->height() * 0.65)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(window, middle, 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end, 20);
    QVERIFY(dialog->property("pointCount").toInt() >= 3);
    QTRY_VERIFY(library.hasPreview());
    QVERIFY(hasTransparentAndOpaquePixels(signaturePreview(library)));
    QVERIFY(QMetaObject::invokeMethod(dialog, "submit"));
    QCOMPARE(placement.count(), 2);
    QCOMPARE(library.entries().size(), 1);
}

void PDFEditControllerTest::placesAndSavesSignatureFromMainWindow()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("signature-main.pdf"));
    QVERIFY(createFontStylePdf(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window);
    QVERIFY(root->setProperty("allowWindowClose", true));
    auto *model = root->findChild<PDFTabModel *>();
    auto *tabs = root->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    auto *library = root->findChild<SignatureLibrary *>(QStringLiteral("signatureLibrary"));
    QVERIFY(model && tabs && library);
    const QVariantList savedSignatures = library->entries();
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    PDFEditController *controller = session->editController();
    QVERIFY(tabs->setProperty("currentIndex", 4));
    QTRY_COMPARE(controller->tool(), PDFEditController::SelectTool);
    QObject *button = nullptr;
    QTRY_VERIFY((button = root->findChild<QObject *>(QStringLiteral("signSignatureButton"))));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QPointer<QObject> dialog;
    QTRY_VERIFY((dialog = root->findChild<QObject *>(QStringLiteral("signatureDialog")))
                && dialog->property("initialized").toBool());
    auto *modes = dialog->findChild<QObject *>(QStringLiteral("signatureModeTabs"));
    auto *text = dialog->findChild<QObject *>(QStringLiteral("signatureTextField"));
    QVERIFY(modes && text);
    QVERIFY(modes->setProperty("currentIndex", 1));
    QVERIFY(text->setProperty("text", QStringLiteral("Nguyễn Văn Huy")));
    QVERIFY(QMetaObject::invokeMethod(dialog, "submit"));
    QTRY_VERIFY(root->property("signaturePlacementPending").toBool());
    QCOMPARE(controller->tool(), PDFEditController::AddImageTool);
    QVERIFY(!controller->hasDrafts());
    QVERIFY(library->hasPreview());

    QTRY_VERIFY(dialog.isNull());

    // Exercise the same placement signal emitted by EditOverlay on a page click.
    controller->imageFileRequested(0, 0.45, 0.65);
    QTRY_VERIFY(!root->property("signaturePlacementPending").toBool());
    QCOMPARE(controller->tool(), PDFEditController::SelectTool);
    QVERIFY(!library->hasPreview());
    QTRY_COMPARE(controller->draftsForPage(0).size(), 1);
    QCOMPARE(controller->draftsForPage(0).constFirst().toMap()
                 .value(QStringLiteral("kind")).toString(), QStringLiteral("addedImage"));
    controller->undo();
    QVERIFY(!controller->hasDrafts());
    controller->redo();
    QVERIFY(controller->hasDrafts());
    QVERIFY(QMetaObject::invokeMethod(root.get(), "requestSignature",
                                       Q_ARG(QVariant, QStringLiteral("check"))));
    dialog = nullptr;
    QTRY_VERIFY((dialog = root->findChild<QObject *>(QStringLiteral("signatureDialog")))
                && dialog->property("initialized").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "submit"));
    QTRY_VERIFY(root->property("signaturePlacementPending").toBool());
    QVERIFY(QMetaObject::invokeMethod(root.get(), "requestSave"));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->saving() && !controller->hasDrafts(), 10000);
    QVERIFY(!root->property("signaturePlacementPending").toBool());
    QVERIFY(!library->hasPreview());
    QCOMPARE(inspectImages(path).count, 1);
    QVERIFY(!firstImageCandidate(*controller).isEmpty());
    QCOMPARE(library->entries(), savedSignatures);
}

void PDFEditControllerTest::cancelsPendingSignaturesAcrossUiChanges()
{
    const QString firstPath = m_temporaryDirectory.filePath(QStringLiteral("signature-cancel-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(QStringLiteral("signature-cancel-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window);
    QVERIFY(root->setProperty("allowWindowClose", true));
    window->show();
    window->requestActivate();
    auto *model = root->findChild<PDFTabModel *>();
    auto *tabs = root->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    auto *library = root->findChild<SignatureLibrary *>(QStringLiteral("signatureLibrary"));
    QVERIFY(model && tabs && library);
    auto *first = model->open(QUrl::fromLocalFile(firstPath));
    auto *second = model->open(QUrl::fromLocalFile(secondPath));
    QVERIFY(first && second);
    model->setCurrentIndex(0);
    auto *controller = first->editController();

    for (const QString &action : {QStringLiteral("escape"), QStringLiteral("ribbon"),
                                  QStringLiteral("session"), QStringLiteral("history")}) {
        model->setCurrentIndex(0);
        QVERIFY(tabs->setProperty("currentIndex", 4));
        QTRY_COMPARE(controller->tool(), PDFEditController::SelectTool);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "requestSignature",
                                           Q_ARG(QVariant, QStringLiteral("check"))));
        QPointer<QObject> dialog;
        QTRY_VERIFY((dialog = root->findChild<QObject *>(QStringLiteral("signatureDialog")))
                    && dialog->property("initialized").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "submit"));
        QTRY_VERIFY(root->property("signaturePlacementPending").toBool());
        QTRY_VERIFY(!root->findChild<QObject *>(QStringLiteral("signatureDialog")));
        QVERIFY(library->hasPreview());
        if (action == QLatin1String("escape")) {
            QTest::keyClick(window, Qt::Key_Escape);
        } else if (action == QLatin1String("ribbon")) {
            QVERIFY(tabs->setProperty("currentIndex", 0));
        } else if (action == QLatin1String("session")) {
            model->setCurrentIndex(1);
        } else {
            QVERIFY(controller->addBlankPage());
        }
        QTRY_VERIFY2(!root->property("signaturePlacementPending").toBool(), qPrintable(action));
        QVERIFY(!library->hasPreview());
        QVERIFY(controller->tool() != PDFEditController::AddImageTool);
        // A delayed click from the old page/session cannot insert the discarded image.
        if (action != QLatin1String("ribbon"))
            controller->imageFileRequested(0, 0.4, 0.6);
        QVERIFY(controller->draftsForPage(0).isEmpty());
        QVERIFY(second->editController()->draftsForPage(0).isEmpty());
        if (action == QLatin1String("history"))
            controller->discardDrafts();
        QVERIFY(!controller->hasDrafts());
    }
    QVERIFY(QMetaObject::invokeMethod(root.get(), "requestSignature",
                                       Q_ARG(QVariant, QStringLiteral("date"))));
    QPointer<QObject> dialog;
    QTRY_VERIFY((dialog = root->findChild<QObject *>(QStringLiteral("signatureDialog")))
                && dialog->property("initialized").toBool());
    QVERIFY(library->hasPreview());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(!root->findChild<QObject *>(QStringLiteral("signatureDialog")));
    QVERIFY(!root->property("signaturePlacementPending").toBool());
    QVERIFY(!library->hasPreview());
    QVERIFY(!controller->hasDrafts());
    QCOMPARE(inspectImages(firstPath).count, 0);
    QCOMPARE(inspectImages(secondPath).count, 0);
}
