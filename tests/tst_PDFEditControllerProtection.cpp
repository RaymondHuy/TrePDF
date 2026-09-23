#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QProcess>
#include <QProcessEnvironment>

using namespace PDFEditControllerTestSupport;

namespace {

QByteArray protectionFileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString saveFailureMessage(const QSignalSpy &status)
{
    return status.isEmpty() ? QStringLiteral("Save failed without a status message")
                            : status.constLast().constFirst().toString();
}

} // namespace

void PDFEditControllerTest::stagesOpenPasswordUntilSave()
{
    PDFEditController controller;
    QVERIFY(!controller.canSetOpenPassword());
    QVERIFY(!controller.setOpenPassword(QStringLiteral("unused")));
    QVERIFY(!controller.passwordChangePending());
    QVERIFY(!controller.hasDrafts());

    const QString path = m_temporaryDirectory.filePath(QStringLiteral("protection-staging.pdf"));
    QVERIFY(createFontStylePdf(path));
    const QByteArray originalBytes = protectionFileBytes(path);
    QVERIFY(!originalBytes.isEmpty());
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    controller.setDocument(&document);
    QVERIFY(!document.passwordProtected());
    QVERIFY(controller.canSetOpenPassword());
    QVERIFY(!controller.setOpenPassword(QString()));
    QVERIFY(!controller.setOpenPassword(QString(128, QLatin1Char('x'))));
    QVERIFY(!controller.setOpenPassword(QString(64, QChar(0x00e9))));
    QVERIFY(!controller.setOpenPassword(QStringLiteral("before") + QChar(0) + QStringLiteral("after")));
    QVERIFY(!controller.hasDrafts());

    QSignalSpy draftsChanged(&controller, &PDFEditController::historyStateChanged);
    QVERIFY(controller.setOpenPassword(QStringLiteral("draft-password")));
    QVERIFY(controller.passwordChangePending());
    QVERIFY(controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!document.passwordProtected());
    QVERIFY(document.documentPassword().isEmpty());
    QCOMPARE(protectionFileBytes(path), originalBytes);

    // Content history must never silently remove a pending security change.
    QCOMPARE(controller.addBlankPage(), 1);
    QVERIFY(controller.canUndo());
    controller.undo();
    QCOMPARE(document.pageCount(), 1);
    QVERIFY(controller.passwordChangePending());
    QVERIFY(controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    controller.redo();
    QCOMPARE(document.pageCount(), 2);
    QVERIFY(controller.passwordChangePending());
    controller.discardDrafts();
    QVERIFY(!controller.passwordChangePending());
    QVERIFY(!controller.hasDrafts());
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(protectionFileBytes(path), originalBytes);
    QVERIFY(!draftsChanged.isEmpty());

    QVERIFY(controller.setOpenPassword(QStringLiteral("discard-on-document-change")));
    controller.setDocument(nullptr);
    QVERIFY(!controller.passwordChangePending());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canSetOpenPassword());

    PDFDocument protectedDocument;
    QVERIFY(protectedDocument.open(
        QUrl::fromLocalFile(QFINDTESTDATA("fixtures/password-1.pdf")),
        QStringLiteral("fixture-open-1")));
    controller.setDocument(&protectedDocument);
    QVERIFY(protectedDocument.passwordProtected());
    QVERIFY(!controller.canSetOpenPassword());
    QVERIFY(!controller.setOpenPassword(QStringLiteral("must-not-replace-existing-security")));
    QVERIFY(!controller.passwordChangePending());
    QVERIFY(!controller.hasDrafts());
}

void PDFEditControllerTest::savesNewOpenPasswordAsynchronously_data()
{
    QTest::addColumn<QString>("password");
    QTest::newRow("ascii") << QStringLiteral("new-open-password");
    QTest::newRow("unicode") << QStringLiteral("Mật khẩu 日本語 🔐");
    QTest::newRow("preserve-spaces") << QStringLiteral("  exact spaces  ");
    QTest::newRow("127-utf8-bytes") << QString(63, QChar(0x00e9)) + QLatin1Char('a');
}

void PDFEditControllerTest::savesNewOpenPasswordAsynchronously()
{
    QFETCH(QString, password);
    const QString suffix = QString::fromLatin1(QTest::currentDataTag());
    const QString sourcePath = m_temporaryDirectory.filePath(QStringLiteral("protect-source-%1.pdf").arg(suffix));
    const QString outputPath = m_temporaryDirectory.filePath(QStringLiteral("protect-output-%1.pdf").arg(suffix));
    QVERIFY(createFontStylePdf(sourcePath));
    const QByteArray originalBytes = protectionFileBytes(sourcePath);
    QVERIFY(!originalBytes.isEmpty());
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.setOpenPassword(password));
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QSignalSpy status(&controller, &PDFEditController::statusMessage);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(controller.saving());
    QVERIFY(controller.passwordChangePending());
    QVERIFY(!controller.setOpenPassword(QStringLiteral("cannot-change-in-flight")));
    controller.discardDrafts();
    QVERIFY(controller.passwordChangePending());
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY2(saved.constFirst().constFirst().toBool(), qPrintable(saveFailureMessage(status)));
    QVERIFY(!controller.saving());
    QVERIFY(!controller.passwordChangePending());
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(document.passwordProtected());
    QVERIFY(!controller.canSetOpenPassword());
    QCOMPARE(document.source(), QUrl::fromLocalFile(outputPath));
    QCOMPARE(document.documentPassword(), password.toUtf8());
    QCOMPARE(protectionFileBytes(sourcePath), originalBytes);

    PDFDocument reopened;
    QVERIFY(!reopened.open(QUrl::fromLocalFile(outputPath)));
    QVERIFY(reopened.passwordRequired());
    QVERIFY(!reopened.open(QUrl::fromLocalFile(outputPath), QStringLiteral("wrong-password")));
    QVERIFY(reopened.passwordRequired());
    QVERIFY(reopened.open(QUrl::fromLocalFile(outputPath), password));
    QCOMPARE(reopened.pageCount(), 1);
    QCOMPARE(reopened.pageSize(0), document.pageSize(0));
    PDFEditController reopenedController;
    reopenedController.setDocument(&reopened);
    QVERIFY(!candidateForText(waitForCandidates(reopenedController, 0),
                             QString::fromLatin1(kRegularText)).isEmpty());
    QVERIFY(document.allowsFormFilling());
    {
        PDFiumProcessLock lock;
        FPDF_DOCUMENT raw = FPDF_LoadDocument(QFile::encodeName(outputPath).constData(),
                                               password.toUtf8().constData());
        QVERIFY(raw);
        const int securityRevision = FPDF_GetSecurityHandlerRevision(raw);
        const unsigned long permissions = FPDF_GetDocPermissions(raw);
        FPDF_CloseDocument(raw);
        QCOMPARE(permissions & 0xF3CUL, 0xF3CUL);
        QCOMPARE(securityRevision, 6); // AES-256 with the current standard security handler.
    }
}

void PDFEditControllerTest::savesOpenPasswordOnSamePathWithTextChanges()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("protect-same-path.pdf"));
    QVERIFY(createFontStylePdf(path));
    const QByteArray originalBytes = protectionFileBytes(path);
    const QString password = QStringLiteral("same-path-password");
    PDFTabModel tabs;
    PDFDocumentSession *session = tabs.open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    PDFEditController *controller = session->editController();
    const QVariantMap candidate = candidateForText(waitForCandidates(*controller, 0),
                                                  QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller->selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QString editedText = QStringLiteral("Protected text");
    controller->commitSelectedText(editedText);
    QVERIFY(controller->setOpenPassword(password));
    QSignalSpy saved(controller, &PDFEditController::saveFinished);
    QSignalSpy status(controller, &PDFEditController::statusMessage);
    QVERIFY(tabs.requestSave(session));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY2(saved.constFirst().constFirst().toBool(), qPrintable(saveFailureMessage(status)));
    QVERIFY(!controller->hasDrafts());
    QVERIFY(session->document()->passwordProtected());
    QCOMPARE(session->document()->source(), QUrl::fromLocalFile(path));
    QVERIFY(protectionFileBytes(path) != originalBytes);
    PDFDocument reopened;
    QVERIFY(!reopened.open(QUrl::fromLocalFile(path)));
    QVERIFY(reopened.passwordRequired());
    QVERIFY(reopened.open(QUrl::fromLocalFile(path), password));
    session->searchController()->search(editedText);
    QTRY_COMPARE_WITH_TIMEOUT(session->searchController()->state(), PDFSearchController::Ready, 5000);
    QCOMPARE(session->searchController()->resultCount(), 1);
    QVERIFY(!candidateForText(waitForCandidates(*controller, 0), editedText).isEmpty());

    // A later normal Save must keep the newly applied AES-256 protection.
    const QVariantMap protectedCandidate = candidateForText(waitForCandidates(*controller, 0), editedText);
    QVERIFY(controller->selectCandidate(0, protectedCandidate.value(QStringLiteral("objectId")).toString()));
    controller->commitSelectedText(QStringLiteral("Changed"));
    QVERIFY(tabs.requestSave(session));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 15000);
    QVERIFY2(saved.constLast().constFirst().toBool(), qPrintable(saveFailureMessage(status)));
    QVERIFY(!reopened.open(QUrl::fromLocalFile(path)));
    QVERIFY(reopened.passwordRequired());
    QVERIFY(reopened.open(QUrl::fromLocalFile(path), password));
    QVERIFY(!candidateForText(waitForCandidates(*controller, 0), QStringLiteral("Changed")).isEmpty());
}

void PDFEditControllerTest::keepsPendingOpenPasswordWhenSaveFails()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("protect-failed-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("protect-unwritable-target.pdf"));
    QVERIFY(createFontStylePdf(path));
    const QByteArray originalBytes = protectionFileBytes(path);
    // A directory destination fails deterministically, even when tests run with elevated rights.
    QVERIFY(QDir().mkpath(target));
    const QString sentinelPath = QDir(target).filePath(QStringLiteral("existing-data"));
    QFile sentinel(sentinelPath);
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    QCOMPARE(sentinel.write("keep-me"), qint64(7));
    sentinel.close();
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    const QString password = QStringLiteral("retry-the-same-password");
    QVERIFY(controller.setOpenPassword(password));
    const int originalRevision = document.renderRevision();
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(target)));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(!saved.constFirst().constFirst().toBool());
    QVERIFY(!controller.saving());
    QVERIFY(controller.passwordChangePending());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canSetOpenPassword());
    QVERIFY(!document.passwordProtected());
    QCOMPARE(document.renderRevision(), originalRevision);
    QCOMPARE(document.source(), QUrl::fromLocalFile(path));
    QCOMPARE(protectionFileBytes(path), originalBytes);
    QCOMPARE(protectionFileBytes(sentinelPath), QByteArray("keep-me"));

    const QString retryPath = m_temporaryDirectory.filePath(QStringLiteral("protect-retry.pdf"));
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(retryPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 15000);
    QVERIFY(saved.constLast().constFirst().toBool());
    QVERIFY(!controller.passwordChangePending());
    PDFDocument reopened;
    QVERIFY(!reopened.open(QUrl::fromLocalFile(retryPath)));
    QVERIFY(reopened.open(QUrl::fromLocalFile(retryPath), password));
}

void PDFEditControllerTest::savesOpenPasswordWithCommentsAndForms()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("protect-comment-form-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("protect-comment-form-saved.pdf"));
    QVERIFY(createFontStylePdf(path));
    PDFTabModel tabs;
    PDFDocumentSession *session = tabs.open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    PDFEditController *controller = session->editController();
    controller->setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!controller->addStickyNote(0, QPointF(0.2, 0.7)).isEmpty());
    const QString commentText = QStringLiteral("Protected comment survives saving");
    controller->setSelectedCommentText(commentText);
    PDFFormDesignerController *forms = session->formDesignerController();
    const QString field = forms->addTextField(0, 0.2, 0.4, 0.4, 0.08);
    QVERIFY(!field.isEmpty());
    QVERIFY(forms->setFieldName(field, QStringLiteral("ProtectedField")));
    const QString fieldValue = QStringLiteral("Nguyễn Văn A");
    QVERIFY(forms->setFieldValue(field, fieldValue));
    const QString password = QStringLiteral("comments-and-forms-password");
    QVERIFY(controller->setOpenPassword(password));
    QSignalSpy saved(controller, &PDFEditController::saveFinished);
    QSignalSpy status(controller, &PDFEditController::statusMessage);
    QVERIFY(tabs.requestSaveAs(session, QUrl::fromLocalFile(target)));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY2(saved.constFirst().constFirst().toBool(), qPrintable(saveFailureMessage(status)));
    QVERIFY(!controller->hasDrafts());
    QVERIFY(session->document()->passwordProtected());

    PDFTabModel reopenedTabs;
    PDFDocumentSession *reopened = reopenedTabs.open(QUrl::fromLocalFile(target), password);
    QVERIFY(reopened);
    PDFEditController *reopenedController = reopened->editController();
    reopenedController->requestCommentIndex();
    QTRY_VERIFY_WITH_TIMEOUT(!reopenedController->commentItems().isEmpty(), 5000);
    QCOMPARE(reopenedController->commentItems().constFirst().toMap().value(QStringLiteral("text")).toString(),
             commentText);
    PDFFormDesignerController *reopenedForms = reopened->formDesignerController();
    QTRY_COMPARE_WITH_TIMEOUT(reopenedForms->textFieldsForPage(0).size(), 1, 5000);
    QCOMPARE(reopenedForms->textFieldsForPage(0).constFirst().toMap().value(QStringLiteral("text")).toString(),
             fieldValue);
}

void PDFEditControllerTest::keepsPendingOpenPasswordWhenEncryptionFails()
{
    // qpdf reads its provider once per process. Isolate failure injection from
    // the other tests and verify that a backend exception never escapes a worker.
    if (!qEnvironmentVariableIsSet("TREPDF_TEST_CRYPTO_FAILURE")) {
        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("TREPDF_TEST_CRYPTO_FAILURE"), QStringLiteral("1"));
        environment.insert(QStringLiteral("QPDF_CRYPTO_PROVIDER"), QStringLiteral("missing-test-provider"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("keepsPendingOpenPasswordWhenEncryptionFails")});
        QVERIFY(child.waitForFinished(30000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, child.readAllStandardOutput().constData());
        return;
    }
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("crypto-failure.pdf"));
    QVERIFY(createFontStylePdf(path));
    const QByteArray originalBytes = protectionFileBytes(path);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.setOpenPassword(QStringLiteral("failure-test-password")));
    QSignalSpy saved(&controller, &PDFEditController::saveFinished);
    QSignalSpy status(&controller, &PDFEditController::statusMessage);
    QVERIFY(controller.requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(!saved.constFirst().constFirst().toBool());
    QVERIFY(!status.isEmpty());
    //% "Password protection could not be applied. The PDF was not saved."
    QCOMPARE(status.constLast().constFirst().toString(), qtTrId("PdfSavePasswordProtectionFailed"));
    QVERIFY(controller.passwordChangePending());
    QVERIFY(controller.hasDrafts());
    QVERIFY(!controller.saving());
    QVERIFY(!document.passwordProtected());
    QCOMPARE(protectionFileBytes(path), originalBytes);
    const QStringList temporaryFiles = QDir(m_temporaryDirectory.path()).entryList(
        {QStringLiteral(".trepdf-*")}, QDir::Files | QDir::Hidden);
    QVERIFY(temporaryFiles.isEmpty());
}

void PDFEditControllerTest::keepsPendingPasswordsIndependentAcrossTabs()
{
    const QString firstPath = m_temporaryDirectory.filePath(QStringLiteral("protect-first-tab.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(QStringLiteral("protect-second-tab.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));
    PDFTabModel tabs;
    PDFDocumentSession *first = tabs.open(QUrl::fromLocalFile(firstPath));
    PDFDocumentSession *second = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(first->editController()->setOpenPassword(QStringLiteral("first-tab-password")));
    QVERIFY(first->hasDrafts());
    QVERIFY(tabs.hasDrafts());
    QVERIFY(!second->hasDrafts());
    QVERIFY(!tabs.closeSession(first));
    QCOMPARE(tabs.count(), 2);
    QVERIFY(second->editController()->setOpenPassword(QStringLiteral("second-tab-password")));
    first->editController()->discardDrafts();
    QVERIFY(!first->hasDrafts());
    QVERIFY(second->editController()->passwordChangePending());
    QVERIFY(tabs.hasDrafts());
    QVERIFY(tabs.closeSession(first));
    QCOMPARE(tabs.count(), 1);
    QCOMPARE(tabs.currentSession(), second);
    QSignalSpy saved(second->editController(), &PDFEditController::saveFinished);
    QVERIFY(tabs.requestSave(second));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.constFirst().constFirst().toBool());
    QVERIFY(!tabs.hasDrafts());
    PDFDocument firstDocument;
    QVERIFY(firstDocument.open(QUrl::fromLocalFile(firstPath)));
    QVERIFY(!firstDocument.passwordProtected());
    PDFDocument secondDocument;
    QVERIFY(!secondDocument.open(QUrl::fromLocalFile(secondPath), QStringLiteral("first-tab-password")));
    QVERIFY(secondDocument.open(QUrl::fromLocalFile(secondPath), QStringLiteral("second-tab-password")));
}

void PDFEditControllerTest::promptsToSetOpenPassword()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("set-password-popup.pdf"));
    const QString otherPath = m_temporaryDirectory.filePath(QStringLiteral("set-password-other-tab.pdf"));
    QVERIFY(createFontStylePdf(path));
    QVERIFY(createFontStylePdf(otherPath));
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
    QVERIFY(tabs);
    QVERIFY(ribbonTabs);
    PDFDocumentSession *session = tabs->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    QVERIFY(ribbonTabs->setProperty("currentIndex", 5));
    QObject *setButton = nullptr;
    QTRY_VERIFY((setButton = window->findChild<QObject *>(QStringLiteral("protectSetPasswordButton"))));
    QVERIFY(setButton->property("enabled").toBool());
    const auto popup = [&]() {
        return window->findChild<QObject *>(QStringLiteral("setPasswordDialog"));
    };
    const auto control = [&](const char *name) {
        return window->findChild<QQuickItem *>(QString::fromLatin1(name));
    };
    QVERIFY(QMetaObject::invokeMethod(setButton, "clicked"));
    QTRY_VERIFY(popup() && popup()->property("opened").toBool());
    auto *input = control("setPdfPasswordInput");
    auto *confirmation = control("confirmPdfPasswordInput");
    auto *show = control("showPdfPasswordCheckBox");
    auto *apply = control("applyPdfPasswordButton");
    QVERIFY(input && confirmation && show && apply);
    QCOMPARE(input->property("echoMode").toInt(), 2);
    QCOMPARE(confirmation->property("echoMode").toInt(), 2);
    QVERIFY(!apply->isEnabled());
    QVERIFY(input->setProperty("text", QStringLiteral("popup-password")));
    QVERIFY(confirmation->setProperty("text", QStringLiteral("different")));
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(control("setPdfPasswordError")->isVisible());
    QVERIFY(!session->hasDrafts());

    // Never accept an oversized password by silently truncating it to 127.
    const QString overlongPassword(128, QLatin1Char('x'));
    QVERIFY(input->setProperty("text", overlongPassword));
    QVERIFY(confirmation->setProperty("text", overlongPassword));
    QCOMPARE(input->property("text").toString(), overlongPassword);
    QCOMPARE(confirmation->property("text").toString(), overlongPassword);
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QVERIFY(popup()->property("opened").toBool());
    QVERIFY(control("setPdfPasswordError")->isVisible());
    QVERIFY(!session->hasDrafts());

    QVERIFY(show->setProperty("checked", true));
    QCOMPARE(input->property("echoMode").toInt(), 0);
    QCOMPARE(confirmation->property("echoMode").toInt(), 0);
    QVERIFY(QMetaObject::invokeMethod(control("cancelSetPdfPasswordButton"), "clicked"));
    QTRY_VERIFY(!popup());
    QVERIFY(!session->hasDrafts());

    QVERIFY(QMetaObject::invokeMethod(setButton, "clicked"));
    QTRY_VERIFY(popup() && popup()->property("opened").toBool());
    input = control("setPdfPasswordInput");
    confirmation = control("confirmPdfPasswordInput");
    QVERIFY(input->property("text").toString().isEmpty());
    QVERIFY(confirmation->property("text").toString().isEmpty());
    QCOMPARE(input->property("echoMode").toInt(), 2);
    QVERIFY(input->setProperty("text", QStringLiteral("popup-password")));
    QVERIFY(confirmation->setProperty("text", QStringLiteral("popup-password")));
    confirmation->forceActiveFocus();
    QTRY_VERIFY(confirmation->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!popup());
    QVERIFY(session->editController()->passwordChangePending());
    QVERIFY(session->hasDrafts());
    QVERIFY(!session->document()->passwordProtected());
    QVERIFY(control("protectSaveButton")->isEnabled());

    // Opening and cancelling a replacement must retain the pending password.
    QVERIFY(QMetaObject::invokeMethod(setButton, "clicked"));
    QTRY_VERIFY(popup() && popup()->property("opened").toBool());
    input = control("setPdfPasswordInput");
    QVERIFY(input->property("text").toString().isEmpty());
    input->forceActiveFocus();
    QTRY_VERIFY(input->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!popup());
    QVERIFY(session->editController()->passwordChangePending());

    QVERIFY(QMetaObject::invokeMethod(setButton, "clicked"));
    QTRY_VERIFY(popup() && popup()->property("opened").toBool());
    PDFDocumentSession *other = tabs->open(QUrl::fromLocalFile(otherPath));
    QVERIFY(other);
    QTRY_VERIFY(!popup());
    QVERIFY(!other->hasDrafts());
    QVERIFY(session->editController()->passwordChangePending());

    // Leaving Protect invalidates a dialog even if changed programmatically.
    QVERIFY(QMetaObject::invokeMethod(setButton, "clicked"));
    QTRY_VERIFY(popup() && popup()->property("opened").toBool());
    QVERIFY(ribbonTabs->setProperty("currentIndex", 0));
    QTRY_VERIFY(!popup());
    QVERIFY(!other->hasDrafts());

    QSignalSpy saved(session->editController(), &PDFEditController::saveFinished);
    QVERIFY(tabs->requestSave(session));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 15000);
    QVERIFY(saved.constFirst().constFirst().toBool());
    PDFDocument reopened;
    QVERIFY(!reopened.open(QUrl::fromLocalFile(path)));
    QVERIFY(reopened.open(QUrl::fromLocalFile(path), QStringLiteral("popup-password")));
}
