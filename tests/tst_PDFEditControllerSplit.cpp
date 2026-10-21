#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFSplitController.h"

#include <filesystem>

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::routesSplitPdfDialogAndGuardsDirtySources()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("split-ui.pdf"));
    const QString otherPath = m_temporaryDirectory.filePath(QStringLiteral("split-ui-other.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));
    QVERIFY(createFontStylePdf(otherPath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    window->setProperty("visible", false);
    window->setProperty("allowWindowClose", true);
    auto *model = window->findChild<PDFTabModel *>();
    auto *controller = window->findChild<PDFSplitController *>();
    auto *loader = window->findChild<QObject *>(QStringLiteral("splitPdfDialogLoader"));
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(model);
    QVERIFY(controller);
    QVERIFY(loader);
    QVERIFY(tabs);
    QVERIFY(!loader->property("active").toBool());
    QVERIFY(!window->findChild<QObject *>(QStringLiteral("splitPdfDialog")));

    QVERIFY(tabs->setProperty("currentIndex", 1));
    QQuickItem *splitButton = nullptr;
    QQuickItem *mergeButton = nullptr;
    QTRY_VERIFY((splitButton = window->findChild<QQuickItem *>(QStringLiteral("splitPdfButton"))));
    QTRY_VERIFY((mergeButton = window->findChild<QQuickItem *>(QStringLiteral("mergePdfButton"))));
    QVERIFY(splitButton->isEnabled());
    QVERIFY(mergeButton->isEnabled());
    QCOMPARE(splitButton->parentItem(), mergeButton->parentItem());
    const auto pageTools = splitButton->parentItem()->childItems();
    QCOMPARE(pageTools.indexOf(splitButton), pageTools.indexOf(mergeButton) + 1);

    // Split is available without a PDF and creates its modal only on demand.
    QVERIFY(QMetaObject::invokeMethod(splitButton, "clicked"));
    QPointer<QObject> dialog;
    QTRY_VERIFY((dialog = window->findChild<QObject *>(QStringLiteral("splitPdfDialog"))));
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(window->property("fileOpenBlocked").toBool());
    QVERIFY(!controller->canSplit());
    auto *saveButton = dialog->findChild<QObject *>(QStringLiteral("saveSplitPdfButton"));
    QVERIFY(saveButton);
    QVERIFY(!saveButton->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(dialog.isNull());
    QVERIFY(!window->property("fileOpenBlocked").toBool());

    auto *sourceSession = model->open(QUrl::fromLocalFile(sourcePath));
    QVERIFY(sourceSession);
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSplitPdf"));
    QTRY_VERIFY((dialog = window->findChild<QObject *>(QStringLiteral("splitPdfDialog"))));
    QTRY_VERIFY(!controller->busy());
    QCOMPARE(controller->source(), QUrl::fromLocalFile(QFileInfo(sourcePath).canonicalFilePath()));
    QCOMPARE(controller->pageCount(), 3);
    QCOMPARE(controller->outputCount(), 3);
    QVERIFY(controller->canSplit());

    auto *everyN = dialog->findChild<QObject *>(QStringLiteral("splitPdfEveryNOption"));
    auto *pagesPerFile = dialog->findChild<QObject *>(QStringLiteral("splitPdfPagesPerFileInput"));
    auto *custom = dialog->findChild<QObject *>(QStringLiteral("splitPdfCustomOption"));
    auto *ranges = dialog->findChild<QObject *>(QStringLiteral("splitPdfRangesInput"));
    QVERIFY(everyN);
    QVERIFY(pagesPerFile);
    QVERIFY(custom);
    QVERIFY(ranges);
    QVERIFY(QMetaObject::invokeMethod(everyN, "clicked"));
    QCOMPARE(controller->mode(), 1);
    QVERIFY(pagesPerFile->setProperty("value", 2));
    QVERIFY(QMetaObject::invokeMethod(pagesPerFile, "valueModified"));
    QCOMPARE(controller->pagesPerFile(), 2);
    QCOMPARE(controller->outputCount(), 2);
    QVERIFY(QMetaObject::invokeMethod(custom, "clicked"));
    QCOMPARE(controller->mode(), 2);
    QVERIFY(ranges->setProperty("text", QStringLiteral("1-2; 3")));
    QVERIFY(QMetaObject::invokeMethod(ranges, "textEdited"));
    QCOMPARE(controller->pageRanges(), QStringLiteral("1-2; 3"));
    QCOMPARE(controller->outputCount(), 2);
    QVERIFY(ranges->setProperty("text", QStringLiteral("1-5")));
    QVERIFY(QMetaObject::invokeMethod(ranges, "textEdited"));
    QVERIFY(!controller->canSplit());
    QVERIFY(ranges->setProperty("text", QStringLiteral("1-2; 3")));
    QVERIFY(QMetaObject::invokeMethod(ranges, "textEdited"));
    QVERIFY(controller->canSplit());

    // Dismissing the destination picker must not create an export job.
    saveButton = dialog->findChild<QObject *>(QStringLiteral("saveSplitPdfButton"));
    QVERIFY(saveButton);
    QVERIFY(QMetaObject::invokeMethod(saveButton, "clicked"));
    auto *folderLoader = dialog->findChild<QObject *>(QStringLiteral("splitPdfFolderPickerLoader"));
    QVERIFY(folderLoader);
    QTRY_VERIFY(folderLoader->property("active").toBool());
    QPointer<QObject> folderPicker;
    QTRY_VERIFY((folderPicker = dialog->findChild<QObject *>(QStringLiteral("splitPdfFolderPicker"))));
    QVERIFY(QMetaObject::invokeMethod(folderPicker, "reject"));
    QTRY_VERIFY(!folderLoader->property("active").toBool());
    QVERIFY(!controller->busy());
    QVERIFY(!dialog->property("completed").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(dialog.isNull());

    // An unrelated dirty tab cannot block splitting this saved source.
    auto *otherSession = model->open(QUrl::fromLocalFile(otherPath));
    QVERIFY(otherSession);
    QVERIFY(otherSession->editController()->addBlankPage() >= 0);
    QVERIFY(otherSession->hasDrafts());
    QVERIFY(model->splitValidationError(QUrl::fromLocalFile(sourcePath)).isEmpty());
    QVERIFY(model->setProperty("currentIndex", model->indexOf(sourceSession)));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSplitPdf"));
    QTRY_VERIFY((dialog = window->findChild<QObject *>(QStringLiteral("splitPdfDialog"))));
    QTRY_VERIFY(!controller->busy());
    QVERIFY(controller->canSplit());

    // Recheck before writing, even if a draft appeared after source inspection.
    QVERIFY(sourceSession->editController()->addBlankPage() >= 0);
    QVERIFY(sourceSession->hasDrafts());
    const QUrl destination = QUrl::fromLocalFile(m_temporaryDirectory.path());
    QVERIFY(QMetaObject::invokeMethod(window.get(), "splitPdfTo", Q_ARG(QUrl, destination)));
    QVERIFY(!controller->busy());
    QVERIFY(!dialog->property("errorText").toString().isEmpty());
    QVERIFY(!dialog->property("completed").toBool());

    const QString aliasPath = m_temporaryDirectory.filePath(QStringLiteral("split-ui-alias.pdf"));
#ifndef Q_OS_WIN
    QVERIFY(QFile::link(sourcePath, aliasPath));
    QVERIFY(!model->splitValidationError(QUrl::fromLocalFile(aliasPath)).isEmpty());
#endif
    const QString hardlinkPath = m_temporaryDirectory.filePath(QStringLiteral("split-ui-hardlink.pdf"));
    std::error_code linkError;
    std::filesystem::create_hard_link(QFileInfo(sourcePath).filesystemFilePath(),
                                     QFileInfo(hardlinkPath).filesystemFilePath(), linkError);
    QVERIFY2(!linkError, linkError.message().c_str());
    QVERIFY(!model->splitValidationError(QUrl::fromLocalFile(hardlinkPath)).isEmpty());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(dialog.isNull());

    // Entering with dirty source shows recovery text without inspecting stale bytes.
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSplitPdf"));
    QTRY_VERIFY((dialog = window->findChild<QObject *>(QStringLiteral("splitPdfDialog"))));
    QVERIFY(!controller->busy());
    QVERIFY(!controller->canSplit());
    QVERIFY(!dialog->property("errorText").toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(dialog.isNull());
    sourceSession->editController()->discardDrafts();
    otherSession->editController()->discardDrafts();
}

void PDFEditControllerTest::completesSplitPdfWithoutOpeningAllOutputs()
{
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("split-ui-completion.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    window->setProperty("visible", false);
    window->setProperty("allowWindowClose", true);
    auto *model = window->findChild<PDFTabModel *>();
    auto *controller = window->findChild<PDFSplitController *>();
    QVERIFY(model);
    QVERIFY(controller);
    auto *session = model->open(QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);
    QCOMPARE(model->count(), 1);
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSplitPdf"));
    QPointer<QObject> dialog;
    QTRY_VERIFY((dialog = window->findChild<QObject *>(QStringLiteral("splitPdfDialog"))));
    QTRY_VERIFY(!controller->busy());
    QVERIFY(controller->canSplit());
    QSignalSpy finished(controller, &PDFSplitController::finished);
    const QUrl destination = QUrl::fromLocalFile(m_temporaryDirectory.path());
    QVERIFY(QMetaObject::invokeMethod(window.get(), "splitPdfTo", Q_ARG(QUrl, destination)));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
    QVERIFY(finished.front().front().toBool());
    QTRY_VERIFY(dialog->property("completed").toBool());
    QCOMPARE(dialog->property("completedCount").toInt(), 3);
    QCOMPARE(model->count(), 1);
    QCOMPARE(model->currentSession(), session);
    QVERIFY(!session->hasDrafts());
    const auto resultFolder = dialog->property("completedFolder").toUrl();
    QVERIFY(resultFolder.isLocalFile());
    QCOMPARE(QDir(resultFolder.toLocalFile()).entryList({QStringLiteral("*.pdf")}, QDir::Files).size(), 3);
    auto *openFolder = dialog->findChild<QObject *>(QStringLiteral("openSplitPdfFolderButton"));
    auto *done = dialog->findChild<QObject *>(QStringLiteral("cancelSplitPdfButton"));
    QVERIFY(openFolder);
    QVERIFY(done);
    QVERIFY(openFolder->property("visible").toBool());
    QVERIFY(openFolder->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(done, "clicked"));
    QTRY_VERIFY(dialog.isNull());
    QVERIFY(!window->property("modalOperationActive").toBool());
}

void PDFEditControllerTest::reusesSplitPasswordFromOpenTabs()
{
    const QString protectedPath = m_temporaryDirectory.filePath(QStringLiteral("split-session-password.pdf"));
    QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/password-1.pdf"), protectedPath));
    const QUrl source = QUrl::fromLocalFile(protectedPath);
    const QString password = QStringLiteral("fixture-open-1");
    const QString otherPath = m_temporaryDirectory.filePath(QStringLiteral("split-session-unrelated.pdf"));
    QVERIFY(createThreePageTextPdf(otherPath));
    PDFTabModel tabs;
    PDFSplitController controller;
    QVERIFY(!tabs.prepareSplitSource(nullptr, source));

    QVERIFY(tabs.prepareSplitSource(&controller, source));
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.passwordRequired());
    QVERIFY(!controller.canSplit());
    auto *session = tabs.open(source, password);
    QVERIFY(session);
    QVERIFY(tabs.open(QUrl::fromLocalFile(otherPath)));
    // Reuse the matching session, even when a different tab is active.
    QVERIFY(tabs.prepareSplitSource(&controller, source));
    QTRY_VERIFY(!controller.busy());
    QVERIFY(!controller.passwordRequired());
    QVERIFY(controller.encryptedSource());
    QVERIFY2(controller.canSplit(), qPrintable(controller.statusText()));

    const QString aliasPath = m_temporaryDirectory.filePath(QStringLiteral("split-session-hardlink.pdf"));
    std::error_code linkError;
    std::filesystem::create_hard_link(QFileInfo(protectedPath).filesystemFilePath(),
                                     QFileInfo(aliasPath).filesystemFilePath(), linkError);
    QVERIFY2(!linkError, linkError.message().c_str());
    QVERIFY(tabs.prepareSplitSource(&controller, QUrl::fromLocalFile(aliasPath)));
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.canSplit());
    QVERIFY(session->editController()->addBlankPage() >= 0);
    QVERIFY(!tabs.prepareSplitSource(&controller, QUrl::fromLocalFile(aliasPath)));
    QVERIFY(!controller.busy());
    session->editController()->discardDrafts();

    // Direct source selection starts fresh: no accidental credential reuse
    // between files, and no getter exposes the authenticated secret to QML.
    controller.clear();
    QVERIFY(controller.source().isEmpty());
    QVERIFY(controller.setSource(source));
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.passwordRequired());
    QCOMPARE(controller.metaObject()->indexOfProperty("password"), -1);
    QCOMPARE(controller.metaObject()->indexOfMethod("documentPassword()"), -1);
}

void PDFEditControllerTest::promptsForSplitPasswordsAndClearsOnClose()
{
    const QUrl source = QUrl::fromLocalFile(QFINDTESTDATA("fixtures/password-1.pdf"));
    const QString password = QStringLiteral("fixture-open-1");
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    window->setProperty("visible", false);
    window->setProperty("allowWindowClose", true);
    auto *controller = window->findChild<PDFSplitController *>();
    QVERIFY(controller);
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSplitPdf"));
    QPointer<QObject> dialog;
    QTRY_VERIFY((dialog = window->findChild<QObject *>(QStringLiteral("splitPdfDialog"))));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "selectSplitPdfSource", Q_ARG(QUrl, source)));
    QTRY_VERIFY(!controller->busy());
    QVERIFY(controller->passwordRequired());

    auto *input = dialog->findChild<QObject *>(QStringLiteral("splitPdfPasswordInput"));
    auto *unlock = dialog->findChild<QObject *>(QStringLiteral("splitPdfUnlockButton"));
    auto *show = dialog->findChild<QObject *>(QStringLiteral("splitPdfShowPasswordCheckBox"));
    auto *save = dialog->findChild<QObject *>(QStringLiteral("saveSplitPdfButton"));
    QVERIFY(input);
    QVERIFY(unlock);
    QVERIFY(show);
    QVERIFY(save);
    QCOMPARE(input->property("echoMode").toInt(), 2); // TextInput.Password
    QVERIFY(!save->property("enabled").toBool());
    QVERIFY(input->setProperty("text", QStringLiteral("incorrect")));
    QVERIFY(show->setProperty("checked", true));
    QCOMPARE(input->property("echoMode").toInt(), 0); // TextInput.Normal
    QVERIFY(QMetaObject::invokeMethod(unlock, "clicked"));
    QTRY_VERIFY(!controller->busy());
    QVERIFY(controller->passwordRequired());
    QVERIFY(input->property("text").toString().isEmpty());
    QVERIFY(!show->property("checked").toBool());

    QVERIFY(input->setProperty("text", password));
    // Enter and the button must share the same validation/clearing path.
    QVERIFY(QMetaObject::invokeMethod(input, "accepted"));
    QTRY_VERIFY(!controller->busy());
    QVERIFY2(controller->canSplit(), qPrintable(controller->statusText()));
    QVERIFY(!controller->passwordRequired());
    QVERIFY(input->property("text").toString().isEmpty());
    QVERIFY(save->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(dialog.isNull());
    QVERIFY(controller->source().isEmpty());

    // Reopening the modal for the same unopened file asks again.
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestSplitPdf"));
    QTRY_VERIFY((dialog = window->findChild<QObject *>(QStringLiteral("splitPdfDialog"))));
    QVERIFY(QMetaObject::invokeMethod(window.get(), "selectSplitPdfSource", Q_ARG(QUrl, source)));
    QTRY_VERIFY(!controller->busy());
    QVERIFY(controller->passwordRequired());
    input = dialog->findChild<QObject *>(QStringLiteral("splitPdfPasswordInput"));
    QVERIFY(input);
    QVERIFY(input->property("text").toString().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(dialog.isNull());
}
