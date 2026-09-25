#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFWatermark.h"

using namespace PDFEditControllerTestSupport;

namespace {

bool createWatermarkSource(const QString &path, int rotation = 0, bool cropped = false)
{
    const QByteArray paint("0.1 0.3 0.8 rg 130 0 40 200 re f\n");
    const QByteArray page = QByteArray("<< /Type /Page /Parent 2 0 R "
        "/MediaBox [0 0 300 200] /Resources << >> /Contents 5 0 R /Rotate ")
        + QByteArray::number(rotation)
        + (cropped ? " /CropBox [20 30 280 170]" : "") + " >>";
    return writePdfObjects(path, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>"),
        page,
        page,
        QByteArray("<< /Length ") + QByteArray::number(paint.size())
            + " >>\nstream\n" + paint + "endstream"
    });
}

QVariantMap textWatermarkSettings()
{
    return {
        {QStringLiteral("text"), QStringLiteral("CONFIDENTIAL")},
        {QStringLiteral("fontFamily"), QStringLiteral("Helvetica")},
        {QStringLiteral("fontSize"), 48},
        {QStringLiteral("color"), QStringLiteral("#a02030")},
        {QStringLiteral("opacity"), 0.5},
        {QStringLiteral("rotation"), -30.0},
        {QStringLiteral("scale"), 0.4},
        {QStringLiteral("x"), 0.5},
        {QStringLiteral("y"), 0.5},
        {QStringLiteral("behind"), false},
        {QStringLiteral("pages"), QStringLiteral("all")},
        {QStringLiteral("imageMode"), false}
    };
}

bool waitForWatermarks(PDFEditController &controller)
{
    controller.loadWatermarks();
    return QTest::qWaitFor([&controller]() {
        return controller.watermarksReady() && !controller.loadingWatermarks();
    }, 10000);
}

QString firstWatermarkId(const PDFEditController &controller)
{
    const QVariantList groups = controller.watermarks();
    return groups.isEmpty() ? QString()
                            : groups.constFirst().toMap().value(QStringLiteral("id")).toString();
}

QByteArray watermarkFileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

PDFWatermarks::ScanResult scanWatermarks(const QString &path, const QByteArray &password = {})
{
    return PDFWatermarks::scan(path, password,
        std::make_shared<std::atomic_bool>(false));
}

} // namespace

void PDFEditControllerTest::watermarkDraftsUseHistoryAndRemainPerSession()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("watermark-history.pdf"));
    QVERIFY(createWatermarkSource(path));
    const QByteArray original = watermarkFileBytes(path);
    PDFDocument document;
    PDFDocument otherDocument;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    QVERIFY(otherDocument.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    PDFEditController other;
    controller.setDocument(&document);
    other.setDocument(&otherDocument);
    QVERIFY(waitForWatermarks(controller));
    QVERIFY(waitForWatermarks(other));
    QVERIFY(controller.watermarks().isEmpty());
    const quint64 beforeRevision = controller.draftRevisionForPage(0);

    QVERIFY(controller.applyWatermark(textWatermarkSettings()));
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(controller.watermarks().size(), 1);
    QCOMPARE(controller.watermarkChangesForPage(0).size(), 1);
    QCOMPARE(controller.watermarkChangesForPage(1).size(), 1);
    QVERIFY(controller.draftRevisionForPage(0) > beforeRevision);
    QVERIFY(!other.hasDrafts());
    QVERIFY(other.watermarks().isEmpty());
    QVERIFY(other.watermarkChangesForPage(0).isEmpty());
    QCOMPARE(watermarkFileBytes(path), original);

    controller.undo();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.watermarks().isEmpty());
    QVERIFY(controller.watermarkChangesForPage(0).isEmpty());
    QVERIFY(controller.watermarkChangesForPage(1).isEmpty());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(controller.watermarks().size(), 1);
    QCOMPARE(controller.watermarkChangesForPage(1).size(), 1);
    controller.discardDrafts();
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(controller.watermarkChangesForPage(0).isEmpty());
    QVERIFY(waitForWatermarks(controller));
    QVERIFY(controller.watermarks().isEmpty());
    QCOMPARE(watermarkFileBytes(path), original);
}

void PDFEditControllerTest::savesUpdatesAndRemovesWatermarkGroups()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("watermark-persist-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("watermark-persist-target.pdf"));
    QVERIFY(createWatermarkSource(source));
    const QByteArray original = watermarkFileBytes(source);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(waitForWatermarks(controller));
    QVERIFY(controller.applyWatermark(textWatermarkSettings()));
    const QString groupId = firstWatermarkId(controller);
    QVERIFY(!groupId.isEmpty());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(watermarkFileBytes(source), original);

    auto saved = scanWatermarks(target);
    QVERIFY2(saved.succeeded, qPrintable(saved.error));
    QCOMPARE(saved.groups.size(), 1);
    QCOMPARE(saved.groups.constFirst().spec.id, groupId);
    QCOMPARE(saved.groups.constFirst().pages, QSet<int>({0, 1}));
    QCOMPARE(saved.groups.constFirst().spec.text, QStringLiteral("CONFIDENTIAL"));

    // Rehydrate into an independent controller before editing, so the update
    // cannot depend on the original controller's in-memory artwork or history.
    PDFDocument reopened;
    QVERIFY(reopened.open(QUrl::fromLocalFile(target)));
    PDFEditController reopenedController;
    reopenedController.setDocument(&reopened);
    QVERIFY(waitForWatermarks(reopenedController));
    QCOMPARE(firstWatermarkId(reopenedController), groupId);
    QVariantMap replacement = textWatermarkSettings();
    replacement.insert(QStringLiteral("text"), QStringLiteral("REVIEWED"));
    replacement.insert(QStringLiteral("pages"), QStringLiteral("2"));
    replacement.insert(QStringLiteral("behind"), true);
    QVERIFY(reopenedController.applyWatermark(replacement, groupId));
    QCOMPARE(reopenedController.watermarkChangesForPage(0).size(), 1);
    QVERIFY(!reopenedController.watermarkChangesForPage(0).constFirst().add);
    QVERIFY(reopenedController.save());
    saved = scanWatermarks(target);
    QVERIFY2(saved.succeeded, qPrintable(saved.error));
    QCOMPARE(saved.groups.size(), 1);
    QCOMPARE(saved.groups.constFirst().spec.id, groupId);
    QCOMPARE(saved.groups.constFirst().spec.text, QStringLiteral("REVIEWED"));
    QCOMPARE(saved.groups.constFirst().pages, QSet<int>({1}));
    QVERIFY(saved.groups.constFirst().spec.behind);

    QVERIFY(waitForWatermarks(reopenedController));
    QVERIFY(reopenedController.removeWatermark(groupId));
    QVERIFY(reopenedController.hasDrafts());
    reopenedController.undo();
    QVERIFY(!reopenedController.hasDrafts());
    QCOMPARE(reopenedController.watermarks().size(), 1);
    reopenedController.redo();
    QVERIFY(reopenedController.save());
    saved = scanWatermarks(target);
    QVERIFY2(saved.succeeded, qPrintable(saved.error));
    QVERIFY(saved.groups.isEmpty());
    const QSize pixelSize(600, 400);
    const QImage before = renderFirstPage(source, pixelSize);
    const QImage after = renderFirstPage(target, pixelSize);
    QVERIFY(!before.isNull());
    QVERIFY(!after.isNull());
    QCOMPARE(changedPixelCount(before, after, before.rect()), 0);
}

void PDFEditControllerTest::snapshotsImageWatermarksWithTransparency()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("watermark-image-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("watermark-image-target.pdf"));
    const QString imagePath = m_temporaryDirectory.filePath(QStringLiteral("watermark-image.png"));
    QVERIFY(createWatermarkSource(source));
    QImage image(40, 40, QImage::Format_ARGB32);
    image.fill(Qt::red);
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 30; ++x)
            image.setPixelColor(x, y, Qt::transparent);
    QVERIFY(image.save(imagePath));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(waitForWatermarks(controller));
    const QVariantMap snapshot = controller.loadWatermarkImage(QUrl::fromLocalFile(imagePath));
    QVERIFY(!snapshot.value(QStringLiteral("token")).toString().isEmpty());
    QVariantMap settings = textWatermarkSettings();
    settings.insert(QStringLiteral("imageMode"), true);
    settings.insert(QStringLiteral("imageToken"), snapshot.value(QStringLiteral("token")));
    settings.insert(QStringLiteral("opacity"), 1.0);
    settings.insert(QStringLiteral("rotation"), 0.0);
    QVERIFY(controller.applyWatermark(settings));
    QVERIFY(QFile::remove(imagePath));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    const auto saved = scanWatermarks(target);
    QVERIFY2(saved.succeeded, qPrintable(saved.error));
    QCOMPARE(saved.groups.size(), 1);
    QVERIFY(saved.groups.constFirst().spec.imageMode);
    const QImage restored = saved.groups.constFirst().spec.image;
    QVERIFY(!restored.isNull());
    QCOMPARE(restored.size(), image.size());
    QCOMPARE(restored.pixelColor(20, 20).alpha(), 0);
    QCOMPARE(restored.pixelColor(2, 2).alpha(), 255);
    const QImage rendered = renderFirstPage(target, QSize(600, 400));
    const QImage original = renderFirstPage(source, QSize(600, 400));
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.pixelColor(300, 200), original.pixelColor(300, 200));
    QVERIFY(rendered.pixelColor(200, 100).red() > 240);
    QVERIFY(rendered.pixelColor(200, 100).green() < 20);
    QVERIFY(changedPixelCount(original, rendered, rendered.rect()) > 1000);

    // Watermark image objects are managed through Protect, not the ordinary
    // image editing path after reopening the PDF.
    const QVariantList candidates = waitForCandidates(controller, 0);
    for (const QVariant &candidate : candidates)
        QVERIFY(candidate.toMap().value(QStringLiteral("kind")).toString()
            != QStringLiteral("image"));
}

void PDFEditControllerTest::keepsWatermarkDraftsOnSaveFailure()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("watermark-failure-source.pdf"));
    const QString blocked = m_temporaryDirectory.filePath(QStringLiteral("watermark-failure-target.pdf"));
    const QString retry = m_temporaryDirectory.filePath(QStringLiteral("watermark-failure-retry.pdf"));
    QVERIFY(createWatermarkSource(source));
    QVERIFY(QDir().mkpath(blocked));
    const QByteArray original = watermarkFileBytes(source);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(waitForWatermarks(controller));
    QVERIFY(controller.applyWatermark(textWatermarkSettings()));
    const QString id = firstWatermarkId(controller);
    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(blocked)));
    QVERIFY(QFileInfo(blocked).isDir());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(firstWatermarkId(controller), id);
    QCOMPARE(document.source(), QUrl::fromLocalFile(source));
    QCOMPARE(watermarkFileBytes(source), original);
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(retry)));
    QVERIFY(!controller.hasDrafts());
    const auto saved = scanWatermarks(retry);
    QVERIFY2(saved.succeeded, qPrintable(saved.error));
    QCOMPARE(saved.groups.size(), 1);
    QCOMPARE(saved.groups.constFirst().spec.id, id);
}

void PDFEditControllerTest::previewsWatermarksOnCroppedRotatedPages_data()
{
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("behind");
    for (int rotation : {0, 90, 180, 270}) {
        const QByteArray name = QByteArray::number(rotation);
        QTest::newRow((name + "-foreground").constData()) << rotation << false;
        QTest::newRow((name + "-background").constData()) << rotation << true;
    }
}

void PDFEditControllerTest::previewsWatermarksOnCroppedRotatedPages()
{
    QFETCH(int, rotation);
    QFETCH(bool, behind);
    const QString tag = QString::fromLatin1(QTest::currentDataTag());
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("watermark-render-%1.pdf").arg(tag));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("watermark-render-saved-%1.pdf").arg(tag));
    QVERIFY(createWatermarkSource(source, rotation, true));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(waitForWatermarks(controller));
    QVariantMap settings = textWatermarkSettings();
    settings.insert(QStringLiteral("behind"), behind);
    settings.insert(QStringLiteral("fontSize"), 24);
    QVERIFY(controller.applyWatermark(settings));

    const QSize pixelSize = (rotation % 180 == 0) ? QSize(520, 280) : QSize(280, 520);
    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(pixelSize);
    TestablePDFPageItem item(window.contentItem());
    item.setSize(pixelSize);
    item.setEditController(&controller);
    item.setDocument(&document);
    item.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed() && item.draftTextureCurrent()
                                && item.draftTextureReady(), 10000);
    const auto grab = item.grabToImage(pixelSize);
    QVERIFY(grab);
    QTRY_VERIFY_WITH_TIMEOUT(!grab->image().isNull(), 10000);
    const QImage preview = grab->image();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    const QImage saved = renderFirstPage(target, pixelSize);
    const QImage original = renderFirstPage(source, pixelSize);
    QVERIFY(!saved.isNull());
    QVERIFY(!original.isNull());
    QCOMPARE(preview.size(), saved.size());
    QVERIFY(changedPixelCount(original, saved, saved.rect()) > 100);
    const qsizetype differences = changedPixelCount(preview, saved, saved.rect());
    QVERIFY2(differences < saved.width() * saved.height() / 200,
        qPrintable(QStringLiteral("Preview and saved watermark differ at %1 pixels").arg(differences)));
}

void PDFEditControllerTest::savesWatermarksWithPendingPassword()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("watermark-password-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("watermark-password-target.pdf"));
    QVERIFY(createWatermarkSource(source));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(waitForWatermarks(controller));
    QVERIFY(controller.applyWatermark(textWatermarkSettings()));
    QVERIFY(controller.setOpenPassword(QStringLiteral("watermark-opening-password")));
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(target)));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.constFirst().constFirst().toBool());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.passwordChangePending());
    PDFDocument reopened;
    QVERIFY(!reopened.open(QUrl::fromLocalFile(target)));
    QVERIFY(reopened.open(QUrl::fromLocalFile(target), QStringLiteral("watermark-opening-password")));
    const auto result = scanWatermarks(target, QByteArray("watermark-opening-password"));
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.groups.size(), 1);
    QCOMPARE(result.groups.constFirst().pages, QSet<int>({0, 1}));

    PDFEditController encryptedController;
    encryptedController.setDocument(&reopened);
    QVERIFY(waitForWatermarks(encryptedController));
    QVERIFY(encryptedController.removeWatermark(firstWatermarkId(encryptedController)));
    QVERIFY(!encryptedController.passwordChangePending());
    QVERIFY(encryptedController.save());
    PDFDocument stillEncrypted;
    QVERIFY(!stillEncrypted.open(QUrl::fromLocalFile(target)));
    QVERIFY(stillEncrypted.open(QUrl::fromLocalFile(target), QStringLiteral("watermark-opening-password")));
    const auto removed = scanWatermarks(target, QByteArray("watermark-opening-password"));
    QVERIFY2(removed.succeeded, qPrintable(removed.error));
    QVERIFY(removed.groups.isEmpty());
}

void PDFEditControllerTest::guardsWatermarksAndPageContentDrafts()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("watermark-guards.pdf"));
    QVERIFY(createWatermarkSource(path));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(waitForWatermarks(controller));
    QVariantMap invalid = textWatermarkSettings();
    invalid.insert(QStringLiteral("text"), QString());
    QVERIFY(!controller.applyWatermark(invalid));
    invalid = textWatermarkSettings();
    invalid.insert(QStringLiteral("pages"), QStringLiteral("3"));
    QVERIFY(!controller.applyWatermark(invalid));
    QVERIFY(!controller.hasDrafts());

    QCOMPARE(controller.addBlankPage(), 1);
    QVERIFY(!controller.applyWatermark(textWatermarkSettings()));
    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 2);
    QVERIFY(waitForWatermarks(controller));
    QVERIFY(controller.applyWatermark(textWatermarkSettings()));
    QVERIFY(controller.addText(0, 0.1, 0.1).isEmpty());
    QCOMPARE(controller.addBlankPage(), -1);
    QCOMPARE(document.pageCount(), 2);
    controller.discardDrafts();
    const int expectedPage = document.currentPage() + 1;
    QCOMPARE(controller.addBlankPage(), expectedPage);
    controller.discardDrafts();
}

void PDFEditControllerTest::initializesWatermarkDialogAfterAsyncReopen()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("watermark-dialog.pdf"));
    QVERIFY(createWatermarkSource(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window);
    QVERIFY(window->setProperty("allowWindowClose", true));
    window->show();
    QTRY_VERIFY_WITH_TIMEOUT(window->isExposed(), 5000);
    auto *tabs = window->findChild<PDFTabModel *>();
    QObject *ribbonTabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(tabs && ribbonTabs);
    QVERIFY(ribbonTabs->setProperty("currentIndex", 5));
    QObject *button = nullptr;
    QTRY_VERIFY((button = window->findChild<QObject *>(QStringLiteral("protectWatermarkButton"))));
    QVERIFY(!button->property("enabled").toBool());
    PDFDocumentSession *session = tabs->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    QTRY_VERIFY(button->property("enabled").toBool());
    const auto dialog = [&]() {
        return window->findChild<QObject *>(QStringLiteral("watermarkDialog"));
    };
    const auto control = [&](const char *name) {
        return window->findChild<QQuickItem *>(QString::fromLatin1(name));
    };
    QVERIFY(QMetaObject::invokeMethod(window, "requestWatermark", Q_ARG(QVariant, QVariant("add"))));
    QTRY_VERIFY(dialog() && dialog()->property("opened").toBool()
        && dialog()->property("canEdit").toBool());
    QVERIFY(control("watermarkTextInput"));
    QVERIFY(control("watermarkTextInput")->setProperty("text", QStringLiteral("TEAM ONLY")));
    QTRY_VERIFY(control("applyWatermarkButton")->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(control("applyWatermarkButton"), "clicked"));
    QTRY_VERIFY(!dialog());
    QVERIFY(session->hasDrafts());
    QSignalSpy saved(session->editController(), &PDFEditController::saveFinished);
    QVERIFY(tabs->requestSave(session));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.constFirst().constFirst().toBool());
    QVERIFY(!session->editController()->watermarksReady());

    // Save invalidates discovery. The dialog must wait for both the ready flag
    // and its list binding before choosing the first returned watermark.
    QVERIFY(QMetaObject::invokeMethod(window, "requestWatermark", Q_ARG(QVariant, QVariant("update"))));
    QTRY_VERIFY(dialog() && dialog()->property("opened").toBool()
        && dialog()->property("initialized").toBool());
    QVERIFY(!dialog()->property("selectedWatermarkId").toString().isEmpty());
    QVERIFY(control("watermarkTextInput")->isVisible());
    QCOMPARE(control("watermarkTextInput")->property("text").toString(), QStringLiteral("TEAM ONLY"));
    QVERIFY(control("applyWatermarkButton")->isEnabled());
    QVERIFY(control("watermarkTextInput")->setProperty("text", QStringLiteral("UPDATED TEAM")));
    QVERIFY(QMetaObject::invokeMethod(control("applyWatermarkButton"), "clicked"));
    QTRY_VERIFY(!dialog());
    QVERIFY(session->hasDrafts());
    QVERIFY(tabs->requestSave(session));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 15000);
    QVERIFY(saved.constLast().constFirst().toBool());
    QVERIFY(!session->editController()->watermarksReady());

    QVERIFY(QMetaObject::invokeMethod(window, "requestWatermark", Q_ARG(QVariant, QVariant("remove"))));
    QTRY_VERIFY(dialog() && dialog()->property("opened").toBool()
        && dialog()->property("initialized").toBool());
    QVERIFY(!dialog()->property("selectedWatermarkId").toString().isEmpty());
    QVERIFY(control("applyWatermarkButton")->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(control("cancelWatermarkButton"), "clicked"));
    QTRY_VERIFY(!dialog());
    QVERIFY(!session->hasDrafts());
}

void PDFEditControllerTest::preservesWatermarksThroughContentAndPageSaves()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("watermark-content-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("watermark-content-target.pdf"));
    QVERIFY(createThreePageTextPdf(source));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    const QVariantList originalCandidates = waitForCandidates(controller, 1);
    QVERIFY(!originalCandidates.isEmpty());
    QVERIFY(waitForWatermarks(controller));
    QVariantMap settings = textWatermarkSettings();
    settings.insert(QStringLiteral("pages"), QStringLiteral("2"));
    QVERIFY(controller.applyWatermark(settings));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));

    PDFDocument reopened;
    QVERIFY(reopened.open(QUrl::fromLocalFile(target)));
    PDFEditController editor;
    editor.setDocument(&reopened);
    const QVariantList candidates = waitForCandidates(editor, 1);
    // Vector watermark artwork must not become ordinary text or shape
    // candidates; the page retains exactly its original editable objects.
    QCOMPARE(candidates.size(), originalCandidates.size());
    const QVariantMap text = candidateForText(candidates, QStringLiteral("Second fixture page"));
    QVERIFY(!text.isEmpty());
    QVERIFY(editor.selectCandidate(1, text.value(QStringLiteral("objectId")).toString()));
    editor.commitSelectedText(QStringLiteral("Edited page"));
    reopened.setCurrentPage(0);
    QCOMPARE(editor.addBlankPage(), 1);
    QVERIFY(editor.save());
    QCOMPARE(reopened.pageCount(), 4);
    QVERIFY(waitForWatermarks(editor));
    const auto persisted = scanWatermarks(target);
    QVERIFY2(persisted.succeeded, qPrintable(persisted.error));
    QCOMPARE(persisted.groups.size(), 1);
    QCOMPARE(persisted.groups.constFirst().pages, QSet<int>({2}));
    QVERIFY(!candidateForText(waitForCandidates(editor, 2),
        QStringLiteral("Edited page")).isEmpty());

    settings.insert(QStringLiteral("text"), QStringLiteral("AFTER EDIT"));
    settings.insert(QStringLiteral("pages"), QStringLiteral("3"));
    QVERIFY(editor.applyWatermark(settings, firstWatermarkId(editor)));
    QVERIFY(editor.save());
    QVERIFY(waitForWatermarks(editor));
    QVERIFY(editor.removeWatermark(firstWatermarkId(editor)));
    QVERIFY(editor.save());
    const auto removed = scanWatermarks(target);
    QVERIFY2(removed.succeeded, qPrintable(removed.error));
    QVERIFY(removed.groups.isEmpty());
    QVERIFY(!candidateForText(waitForCandidates(editor, 2),
        QStringLiteral("Edited page")).isEmpty());
}

void PDFEditControllerTest::rejectsStaleWatermarkFingerprintAtomically()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("watermark-stale-source.pdf"));
    const QString modified = m_temporaryDirectory.filePath(QStringLiteral("watermark-stale-external.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("watermark-stale-target.pdf"));
    QVERIFY(createWatermarkSource(source));
    QVERIFY(createWatermarkSource(target));
    const QByteArray originalTarget = watermarkFileBytes(target);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(waitForWatermarks(controller));
    QVERIFY(controller.applyWatermark(textWatermarkSettings()));
    QVERIFY(controller.save());
    QVERIFY(waitForWatermarks(controller));
    QVERIFY(controller.removeWatermark(firstWatermarkId(controller)));

    {
        PDFiumProcessLock lock;
        FPDF_DOCUMENT raw = FPDF_LoadDocument(QFile::encodeName(source).constData(), nullptr);
        QVERIFY(raw);
        const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(raw); });
        FPDF_PAGE page = FPDF_LoadPage(raw, 0);
        QVERIFY(page);
        const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
        FPDF_PAGEOBJECT watermark = nullptr;
        for (int i = 0; i < FPDFPage_CountObjects(page); ++i) {
            FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
            if (PDFWatermarks::hasMark(object)) {
                watermark = object;
                break;
            }
        }
        QVERIFY(watermark);
        FS_MATRIX matrix{};
        QVERIFY(FPDFPageObj_GetMatrix(watermark, &matrix));
        matrix.e += 7;
        QVERIFY(FPDFPageObj_SetMatrix(watermark, &matrix));
        QVERIFY(FPDFPage_GenerateContent(page));
        PdfFileWriter writer(modified);
        QVERIFY(writer.open());
        QVERIFY(FPDF_SaveAsCopy(raw, &writer, FPDF_NO_INCREMENTAL));
    }
    QVERIFY(QFile::remove(source));
    QVERIFY(QFile::rename(modified, source));
    const QByteArray externalBytes = watermarkFileBytes(source);
    QVERIFY(!controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QCOMPARE(watermarkFileBytes(target), originalTarget);
    QCOMPARE(watermarkFileBytes(source), externalBytes);
}

void PDFEditControllerTest::preservesForeignAndMalformedWatermarkObjects()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("watermark-foreign-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("watermark-foreign-target.pdf"));
    const QByteArray paint("/Watermark BMC 1 0 0 rg 20 20 50 60 re f EMC\n"
        "/TrePDFWatermark << /Data (malformed) >> BDC 0 0 1 rg 140 40 70 80 re f EMC\n");
    QVERIFY(writePdfObjects(source, {
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] "
            "/Resources << >> /Contents 4 0 R >>"),
        QByteArray("<< /Length ") + QByteArray::number(paint.size())
            + " >>\nstream\n" + paint + "endstream"
    }));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(source)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(waitForWatermarks(controller));
    QVERIFY(controller.watermarks().isEmpty());
    QVERIFY(controller.applyWatermark(textWatermarkSettings()));
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(waitForWatermarks(controller));
    QCOMPARE(controller.watermarks().size(), 1);
    QVERIFY(controller.removeWatermark(firstWatermarkId(controller)));
    QVERIFY(controller.save());
    const auto remaining = scanWatermarks(target);
    QVERIFY2(remaining.succeeded, qPrintable(remaining.error));
    QVERIFY(remaining.groups.isEmpty());
    const QImage before = renderFirstPage(source, QSize(600, 400));
    const QImage after = renderFirstPage(target, QSize(600, 400));
    QVERIFY(!before.isNull());
    QVERIFY(!after.isNull());
    QCOMPARE(changedPixelCount(before, after, before.rect()), 0);
}
