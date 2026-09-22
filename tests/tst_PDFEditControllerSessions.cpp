#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::savesAsynchronously()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-async-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-async-output.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    const QString expectedText = QStringLiteral("Lưu tiếng Việt");
    const QString editedText = expectedText.normalized(
        QString::NormalizationForm_D);
    controller.commitSelectedText(editedText);

    QSignalSpy saveFinishedSpy(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(outputPath)));
    QVERIFY(controller.saving());
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    controller.undo();
    controller.redo();
    controller.discardDrafts();
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 5000);
    QCOMPARE(saveFinishedSpy.constFirst().constFirst().toBool(), true);
    QVERIFY(!controller.saving());
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QVERIFY(normalizedPageText(outputPath).contains(expectedText));
    const TextObjectFontInfo editedFont = textObjectFontInfo(
        outputPath,
        QStringLiteral("Lưu"));
    QVERIFY(editedFont.found);
    QCOMPARE(editedFont.embedded, 1);
}

void PDFEditControllerTest::keepsDraftsWhenSaveFails()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-failure-source.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(controller.selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    controller.commitSelectedText(QStringLiteral("This draft must survive"));
    const int revisionBeforeSave = document.renderRevision();
    QVERIFY(controller.canUndo());

    const QString invalidPath = m_temporaryDirectory.filePath(
        QStringLiteral("missing-directory/output.pdf"));
    QSignalSpy saveFinishedSpy(&controller, &PDFEditController::saveFinished);
    QVERIFY(controller.requestSaveAs(QUrl::fromLocalFile(invalidPath)));
    QVERIFY(controller.saving());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 5000);
    QCOMPARE(saveFinishedSpy.constFirst().constFirst().toBool(), false);
    QVERIFY(!QFile::exists(invalidPath));
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.saving());
    QCOMPARE(document.source().toLocalFile(), sourcePath);
    QCOMPARE(document.renderRevision(), revisionBeforeSave);
    QVERIFY(pageText(sourcePath).contains(QString::fromLatin1(kRegularText)));
}

void PDFEditControllerTest::opensDocumentsInIndependentTabs()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("tabs-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("tabs-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    QVERIFY(firstSession);
    QCOMPARE(tabs.count(), 1);
    QCOMPARE(tabs.currentSession(), firstSession);

    firstSession->setZoom(1.75);
    firstSession->setScrollY(420.0);
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(secondSession);
    QVERIFY(secondSession != firstSession);
    QCOMPARE(tabs.count(), 2);
    QCOMPARE(tabs.currentIndex(), 1);
    QCOMPARE(tabs.currentSession(), secondSession);
    QCOMPARE(secondSession->zoom(), 1.0);
    QCOMPARE(secondSession->scrollY(), 0.0);

    const QString duplicatePath = QFileInfo(firstPath).dir().filePath(
        QStringLiteral("./tabs-first.pdf"));
    QCOMPARE(tabs.open(QUrl::fromLocalFile(duplicatePath)), firstSession);
    QCOMPARE(tabs.count(), 2);
    QCOMPARE(tabs.currentIndex(), 0);
    QCOMPARE(firstSession->zoom(), 1.75);
    QCOMPARE(firstSession->scrollY(), 420.0);
}

void PDFEditControllerTest::requiresPasswordForProtectedDocuments()
{
    const QString protectedPath = QFINDTESTDATA("fixtures/password-1.pdf");
    QVERIFY(!protectedPath.isEmpty());
    const QUrl protectedSource = QUrl::fromLocalFile(protectedPath);
    const QString password = QStringLiteral("fixture-open-1");
    PDFDocument document;

    QVERIFY(!document.open(protectedSource));
    QVERIFY(document.passwordRequired());
    QVERIFY(!document.isLoaded());
    QVERIFY(document.documentPassword().isEmpty());
    QVERIFY(!document.open(protectedSource, QStringLiteral("incorrect")));
    QVERIFY(document.passwordRequired());
    QVERIFY(!document.isLoaded());
    QVERIFY(document.documentPassword().isEmpty());

    QVERIFY(document.open(protectedSource, password));
    QVERIFY(!document.passwordRequired());
    QVERIFY(document.isLoaded());
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.pageSize(0), QSizeF(300, 400));
    QCOMPARE(document.documentPassword(), password.toUtf8());
    const int revision = document.renderRevision();

    // A failed retry must leave the existing session and its credentials usable.
    QVERIFY(!document.open(protectedSource, QStringLiteral("incorrect")));
    QVERIFY(document.passwordRequired());
    QVERIFY(document.isLoaded());
    QCOMPARE(document.source(), protectedSource);
    QCOMPARE(document.renderRevision(), revision);
    QCOMPARE(document.documentPassword(), password.toUtf8());

    const QString malformedPath = m_temporaryDirectory.filePath(
        QStringLiteral("password-malformed.pdf"));
    QFile malformed(malformedPath);
    QVERIFY(malformed.open(QIODevice::WriteOnly));
    QVERIFY(malformed.write("This is not a PDF.") > 0);
    malformed.close();
    QVERIFY(!document.open(QUrl::fromLocalFile(malformedPath)));
    QVERIFY(!document.passwordRequired());
    QCOMPARE(document.source(), protectedSource);
    QCOMPARE(document.renderRevision(), revision);
    QCOMPARE(document.documentPassword(), password.toUtf8());

    const QString plainPath = m_temporaryDirectory.filePath(
        QStringLiteral("password-plain.pdf"));
    QVERIFY(createFontStylePdf(plainPath));
    QVERIFY(document.open(QUrl::fromLocalFile(plainPath)));
    QVERIFY(!document.passwordRequired());
    QVERIFY(document.documentPassword().isEmpty());
    QVERIFY(document.open(protectedSource, password));
    document.close();
    QVERIFY(!document.isLoaded());
    QVERIFY(!document.passwordRequired());
    QVERIFY(document.documentPassword().isEmpty());
}

void PDFEditControllerTest::requestsPasswordsBeforeAddingTabs()
{
    const QString firstPath = QFINDTESTDATA("fixtures/password-1.pdf");
    const QString secondPath = QFINDTESTDATA("fixtures/password-2.pdf");
    QVERIFY(!firstPath.isEmpty());
    QVERIFY(!secondPath.isEmpty());
    const QUrl firstSource = QUrl::fromLocalFile(firstPath);
    const QUrl secondSource = QUrl::fromLocalFile(secondPath);
    PDFTabModel tabs;
    QSignalSpy passwordSpy(&tabs, &PDFTabModel::passwordRequested);
    QSignalSpy countSpy(&tabs, &PDFTabModel::countChanged);

    QVERIFY(!tabs.open(firstSource));
    QCOMPARE(tabs.count(), 0);
    QCOMPARE(tabs.currentIndex(), -1);
    QCOMPARE(passwordSpy.size(), 1);
    QCOMPARE(passwordSpy.constLast().at(0).toUrl(), firstSource);
    QCOMPARE(passwordSpy.constLast().at(1).toBool(), false);
    QCOMPARE(countSpy.size(), 0);
    QVERIFY(!tabs.open(firstSource, QStringLiteral("incorrect")));
    QCOMPARE(passwordSpy.size(), 2);
    QCOMPARE(passwordSpy.constLast().at(1).toBool(), true);
    QCOMPARE(tabs.count(), 0);

    PDFDocumentSession *first = tabs.open(
        firstSource, QStringLiteral("fixture-open-1"));
    QVERIFY(first);
    QCOMPARE(tabs.count(), 1);
    QCOMPARE(passwordSpy.size(), 2);
    first->setZoom(1.75);
    first->setScrollY(123.0);

    // Dismissing a request needs no rollback: no provisional tab is inserted.
    QVERIFY(!tabs.open(secondSource));
    QCOMPARE(passwordSpy.size(), 3);
    QCOMPARE(passwordSpy.constLast().at(0).toUrl(), secondSource);
    QCOMPARE(passwordSpy.constLast().at(1).toBool(), false);
    QCOMPARE(tabs.count(), 1);
    QCOMPARE(tabs.currentSession(), first);
    QCOMPARE(first->zoom(), 1.75);
    QCOMPARE(first->scrollY(), 123.0);

    PDFDocumentSession *second = tabs.open(
        secondSource, QStringLiteral("fixture-open-2"));
    QVERIFY(second);
    QVERIFY(second != first);
    QCOMPARE(tabs.count(), 2);
    QCOMPARE(first->document()->documentPassword(), QByteArray("fixture-open-1"));
    QCOMPARE(second->document()->documentPassword(), QByteArray("fixture-open-2"));
    QCOMPARE(tabs.currentSession(), second);
    QCOMPARE(tabs.open(firstSource), first);
    QCOMPARE(tabs.currentSession(), first);
    QCOMPARE(passwordSpy.size(), 3);
    QCOMPARE(tabs.count(), 2);
    QCOMPARE(countSpy.size(), 2);

    const QString malformedPath = m_temporaryDirectory.filePath(
        QStringLiteral("password-tabs-malformed.pdf"));
    QFile malformed(malformedPath);
    QVERIFY(malformed.open(QIODevice::WriteOnly));
    QVERIFY(malformed.write("Not a PDF.") > 0);
    malformed.close();
    QVERIFY(!tabs.open(QUrl::fromLocalFile(malformedPath)));
    QCOMPARE(passwordSpy.size(), 3);
    QCOMPARE(tabs.count(), 2);
    QCOMPARE(tabs.currentSession(), first);
}

void PDFEditControllerTest::opensPasswordProtectedWorkerSessions()
{
    PDFTabModel tabs;
    PDFDocumentSession *first = tabs.open(
        QUrl::fromLocalFile(QFINDTESTDATA("fixtures/password-1.pdf")),
        QStringLiteral("fixture-open-1"));
    PDFDocumentSession *second = tabs.open(
        QUrl::fromLocalFile(QFINDTESTDATA("fixtures/password-2.pdf")),
        QStringLiteral("fixture-open-2"));
    QVERIFY(first);
    QVERIFY(second);
    for (PDFDocumentSession *session : {first, second}) {
        session->searchController()->search(QStringLiteral("Protected fixture"));
    }
    for (PDFDocumentSession *session : {first, second}) {
        QTRY_COMPARE_WITH_TIMEOUT(session->searchController()->state(),
                                  PDFSearchController::Ready, 5000);
        QCOMPARE(session->searchController()->resultCount(), 1);
        const QString text = session == first
            ? QStringLiteral("Protected fixture 1")
            : QStringLiteral("Protected fixture 2");
        QVERIFY(!candidateForText(waitForCandidates(*session->editController(), 0),
                                  text).isEmpty());
        const auto cancellation = std::make_shared<std::atomic_bool>(false);
        const QString path = session->document()->renderFilePath();
        const QByteArray password = session->document()->documentPassword();
        QVERIFY(!PDFWebLinks::scan(path, cancellation, password).failed);
        QVERIFY(!PDFWebLinks::scanSourceLinks(path, cancellation, password).failed);
        QVERIFY(!PDFWebLinks::scanEditableLinks(path, cancellation, password).failed);
        QVERIFY(PDFWebLinks::scan(path, cancellation).failed);
    }

    QQuickWindow window;
    window.setColor(Qt::white);
    window.resize(300, 400);
    TestablePDFPageItem item(window.contentItem());
    item.setSize(QSizeF(300, 400));
    item.setDocument(first->document());
    item.completeForTest();
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.isExposed() && item.draftTextureReady()
                                && item.draftTextureCurrent(), 5000);
    const auto grab = item.grabToImage(QSize(300, 400));
    QVERIFY(grab);
    QTRY_VERIFY_WITH_TIMEOUT(!grab->image().isNull(), 5000);
    const QColor marker = grab->image().pixelColor(50, 100);
    QVERIFY(marker.red() > 220 && marker.green() < 40 && marker.blue() < 40);

    PDFFormController *forms = first->formController();
    QTRY_VERIFY_WITH_TIMEOUT(!forms->javascriptTransitionPending(), 5000);
    item.setFormController(forms);
    QTRY_VERIFY_WITH_TIMEOUT(item.draftTextureReady()
                                && item.draftTextureCurrent(), 5000);
    QSignalSpy inputSpy(forms, &PDFFormController::inputStateChanged);
    forms->pointerEvent(0, PDFFormWorker::PointerEventType::Press, 0,
                        QPointF(40, 190), QSize(300, 400));
    forms->pointerEvent(0, PDFFormWorker::PointerEventType::Release, 0,
                        QPointF(40, 190), QSize(300, 400));
    QTRY_COMPARE_WITH_TIMEOUT(inputSpy.size(), 2, 5000);
    QVERIFY(forms->hasDrafts());
    QVERIFY(!second->formController()->hasDrafts());
    forms->discardChanges();
    QTRY_VERIFY_WITH_TIMEOUT(!forms->hasDrafts(), 5000);
}

void PDFEditControllerTest::promptsForProtectedPdfPasswords()
{
    const QString plainPath = m_temporaryDirectory.filePath(
        QStringLiteral("password-popup-original.pdf"));
    QVERIFY(createFontStylePdf(plainPath));
    const QUrl protectedSource = QUrl::fromLocalFile(
        QFINDTESTDATA("fixtures/password-1.pdf"));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window);
    window->show();
    QTRY_VERIFY_WITH_TIMEOUT(window->isExposed(), 5000);
    auto *tabs = window->findChild<PDFTabModel *>();
    QVERIFY(tabs);
    PDFDocumentSession *original = tabs->open(QUrl::fromLocalFile(plainPath));
    QVERIFY(original);

    QVERIFY(!tabs->open(protectedSource));
    QObject *popup = nullptr;
    QTRY_VERIFY((popup = window->findChild<QObject *>(QStringLiteral("pdfPasswordPopup"))));
    QTRY_VERIFY(popup->property("opened").toBool());
    auto *input = window->findChild<QQuickItem *>(QStringLiteral("pdfPasswordInput"));
    auto *error = window->findChild<QQuickItem *>(QStringLiteral("pdfPasswordError"));
    QObject *openButton = window->findChild<QObject *>(QStringLiteral("openPasswordPdfButton"));
    QVERIFY(input);
    QVERIFY(error);
    QVERIFY(openButton);
    QCOMPARE(input->property("echoMode").toInt(), 2); // TextInput.Password
    QVERIFY(!error->isVisible());
    QVERIFY(!openButton->property("enabled").toBool());
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentSession(), original);

    QVERIFY(input->setProperty("text", QStringLiteral("incorrect")));
    QVERIFY(openButton->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
    QTRY_VERIFY(error->isVisible());
    QVERIFY(popup->property("opened").toBool());
    QVERIFY(input->property("text").toString().isEmpty());
    QVERIFY(!openButton->property("enabled").toBool());
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentSession(), original);

    QVERIFY(input->setProperty("text", QStringLiteral("fixture-open-1")));
    input->forceActiveFocus();
    QTRY_VERIFY(input->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(tabs->count(), 2);
    QCOMPARE(tabs->currentSession()->document()->source(), protectedSource);
    QTRY_VERIFY(!window->findChild<QObject *>(QStringLiteral("pdfPasswordPopup")));
    QVERIFY(window->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::preservesQueuedPdfPasswordPrompts()
{
    const QString plainPath = m_temporaryDirectory.filePath(
        QStringLiteral("password-queue-original.pdf"));
    QVERIFY(createFontStylePdf(plainPath));
    const QUrl firstSource = QUrl::fromLocalFile(QFINDTESTDATA("fixtures/password-1.pdf"));
    const QUrl secondSource = QUrl::fromLocalFile(QFINDTESTDATA("fixtures/password-2.pdf"));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window);
    window->show();
    QTRY_VERIFY_WITH_TIMEOUT(window->isExposed(), 5000);
    auto *tabs = window->findChild<PDFTabModel *>();
    QVERIFY(tabs);
    PDFDocumentSession *original = tabs->open(QUrl::fromLocalFile(plainPath));
    QVERIFY(original);
    const QVariant files = QVariantList{firstSource, secondSource};
    const auto findPopup = [&]() {
        return window->findChild<QObject *>(QStringLiteral("pdfPasswordPopup"));
    };

    QVERIFY(QMetaObject::invokeMethod(window, "openFiles", Q_ARG(QVariant, files)));
    QTRY_VERIFY(findPopup());
    QTRY_VERIFY(findPopup()->property("opened").toBool());
    QCOMPARE(findPopup()->property("fileName").toString(), QStringLiteral("password-1.pdf"));
    QObject *cancel = window->findChild<QObject *>(QStringLiteral("cancelPdfPasswordButton"));
    QVERIFY(cancel);
    QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
    QTRY_VERIFY(findPopup() && findPopup()->property("opened").toBool()
                && findPopup()->property("fileName").toString() == QStringLiteral("password-2.pdf"));
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentSession(), original);

    auto *input = window->findChild<QQuickItem *>(QStringLiteral("pdfPasswordInput"));
    QVERIFY(input);
    QVERIFY(input->property("text").toString().isEmpty());
    input->forceActiveFocus();
    QTRY_VERIFY(input->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!findPopup());
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentSession(), original);

    // Completing one request must preserve the next file from the same selection.
    QVERIFY(QMetaObject::invokeMethod(window, "openFiles", Q_ARG(QVariant, files)));
    QTRY_VERIFY(findPopup());
    for (int index = 1; index <= 2; ++index) {
        QTRY_VERIFY(findPopup() && findPopup()->property("opened").toBool());
        QCOMPARE(findPopup()->property("fileName").toString(),
                 QStringLiteral("password-%1.pdf").arg(index));
        input = window->findChild<QQuickItem *>(QStringLiteral("pdfPasswordInput"));
        QVERIFY(input);
        QVERIFY(input->property("text").toString().isEmpty());
        QVERIFY(input->setProperty("text", QStringLiteral("fixture-open-%1").arg(index)));
        QObject *open = window->findChild<QObject *>(QStringLiteral("openPasswordPdfButton"));
        QVERIFY(open);
        QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
        QTRY_COMPARE(tabs->count(), index + 1);
    }
    QTRY_VERIFY(!findPopup());
    QCOMPARE(tabs->currentSession()->document()->source(), secondSource);
    QVERIFY(window->setProperty("allowWindowClose", true));
}

void PDFEditControllerTest::savesPasswordProtectedContentAsynchronously()
{
    const QString sourcePath = QFINDTESTDATA("fixtures/password-1.pdf");
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("password-content-saved.pdf"));
    const QString password = QStringLiteral("fixture-open-1");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = source.readAll();
    source.close();
    PDFTabModel tabs;
    PDFDocumentSession *session = tabs.open(QUrl::fromLocalFile(sourcePath), password);
    QVERIFY(session);
    PDFEditController *controller = session->editController();
    const QVariantMap candidate = candidateForText(
        waitForCandidates(*controller, 0), QStringLiteral("Protected fixture 1"));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(controller->selectCandidate(0, candidate.value(QStringLiteral("objectId")).toString()));
    const QString editedText = QStringLiteral("Changed fixture 1");
    controller->commitSelectedText(editedText);
    QVERIFY(controller->hasDrafts());
    QSignalSpy saved(controller, &PDFEditController::saveFinished);
    QSignalSpy status(controller, &PDFEditController::statusMessage);
    QVERIFY(controller->requestSaveAs(QUrl::fromLocalFile(outputPath)));
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    QVERIFY2(saved.constFirst().constFirst().toBool(),
             status.isEmpty() ? "Save failed without a status message"
                              : qPrintable(status.constLast().constFirst().toString()));
    QVERIFY(!controller->hasDrafts());
    QVERIFY(!controller->canUndo());
    QCOMPARE(session->document()->source(), QUrl::fromLocalFile(outputPath));
    QCOMPARE(session->document()->documentPassword(), password.toUtf8());
    session->searchController()->search(editedText);
    QTRY_COMPARE_WITH_TIMEOUT(session->searchController()->state(), PDFSearchController::Ready, 5000);
    QCOMPARE(session->searchController()->resultCount(), 1);

    PDFDocument reopened;
    QVERIFY(!reopened.open(QUrl::fromLocalFile(outputPath)));
    QVERIFY(reopened.passwordRequired());
    QVERIFY(reopened.open(QUrl::fromLocalFile(outputPath), password));
    PDFEditController reopenedController;
    reopenedController.setDocument(&reopened);
    QVERIFY(!candidateForText(waitForCandidates(reopenedController, 0), editedText).isEmpty());
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), originalBytes);
}

void PDFEditControllerTest::keepsViewRotationIndependentAcrossTabs()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("rotation-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("rotation-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(firstSession);
    QVERIFY(secondSession);

    QSignalSpy rotationSpy(firstSession, &PDFDocumentSession::viewRotationChanged);
    firstSession->rotateView(1);
    QCOMPARE(firstSession->viewRotation(), 90);
    QCOMPARE(secondSession->viewRotation(), 0);
    QCOMPARE(rotationSpy.count(), 1);

    firstSession->rotateView(-2);
    QCOMPARE(firstSession->viewRotation(), 270);
    QCOMPARE(rotationSpy.count(), 2);

    firstSession->setViewRotation(450);
    QCOMPARE(firstSession->viewRotation(), 90);
    QCOMPARE(rotationSpy.count(), 3);

    firstSession->setViewRotation(90);
    QCOMPARE(rotationSpy.count(), 3);
}

void PDFEditControllerTest::keepsPageDisplayStateIndependentAcrossTabs()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("page-display-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("page-display-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    QVERIFY(firstSession);
    QCOMPARE(firstSession->pageDisplayMode(), PDFDocumentSession::ContinuousMode);
    QVERIFY(!firstSession->separateCoverPage());
    QCOMPARE(firstSession->splitViewMode(), PDFDocumentSession::NoSplit);
    QVERIFY(!firstSession->splitViewEnabled());
    QVERIFY(firstSession->secondaryViewState());
    QCOMPARE(firstSession->splitScrollX(), 0.0);
    QCOMPARE(firstSession->splitScrollY(), 0.0);
    QVERIFY(!firstSession->splitScrollPositionInitialized());

    QSignalSpy modeSpy(firstSession, &PDFDocumentSession::pageDisplayModeChanged);
    QSignalSpy coverSpy(firstSession, &PDFDocumentSession::separateCoverPageChanged);
    QSignalSpy splitSpy(firstSession, &PDFDocumentSession::splitViewEnabledChanged);
    QSignalSpy splitModeSpy(firstSession, &PDFDocumentSession::splitViewModeChanged);
    QSignalSpy splitScrollSpy(
        firstSession,
        &PDFDocumentSession::splitScrollPositionChanged);

    firstSession->setPageDisplayMode(PDFDocumentSession::FacingMode);
    QCOMPARE(firstSession->pageDisplayMode(), PDFDocumentSession::FacingMode);
    QCOMPARE(modeSpy.count(), 1);
    firstSession->setPageDisplayMode(PDFDocumentSession::FacingMode);
    firstSession->setPageDisplayMode(
        static_cast<PDFDocumentSession::PageDisplayMode>(-1));
    QCOMPARE(firstSession->pageDisplayMode(), PDFDocumentSession::FacingMode);
    QCOMPARE(modeSpy.count(), 1);

    firstSession->setSeparateCoverPage(true);
    QVERIFY(firstSession->separateCoverPage());
    QCOMPARE(coverSpy.count(), 1);
    firstSession->setSeparateCoverPage(true);
    QCOMPARE(coverSpy.count(), 1);

    firstSession->setSplitViewEnabled(true);
    QVERIFY(firstSession->splitViewEnabled());
    QCOMPARE(firstSession->splitViewMode(), PDFDocumentSession::VerticalSplit);
    QCOMPARE(splitSpy.count(), 1);
    QCOMPARE(splitModeSpy.count(), 1);
    firstSession->setSplitViewEnabled(true);
    QCOMPARE(splitSpy.count(), 1);
    QCOMPARE(splitModeSpy.count(), 1);

    firstSession->setSplitViewMode(PDFDocumentSession::HorizontalSplit);
    QCOMPARE(firstSession->splitViewMode(), PDFDocumentSession::HorizontalSplit);
    QVERIFY(firstSession->splitViewEnabled());
    QCOMPARE(splitSpy.count(), 1);
    QCOMPARE(splitModeSpy.count(), 2);
    firstSession->setSplitViewMode(
        static_cast<PDFDocumentSession::SplitViewMode>(-1));
    QCOMPARE(firstSession->splitViewMode(), PDFDocumentSession::HorizontalSplit);
    QCOMPARE(splitModeSpy.count(), 2);
    firstSession->setSplitViewMode(
        static_cast<PDFDocumentSession::SplitViewMode>(3));
    QCOMPARE(firstSession->splitViewMode(), PDFDocumentSession::HorizontalSplit);
    QCOMPARE(splitModeSpy.count(), 2);

    firstSession->setSplitScrollX(144.5);
    QVERIFY(firstSession->splitScrollPositionInitialized());
    firstSession->setSplitScrollY(320.25);
    QCOMPARE(firstSession->splitScrollX(), 144.5);
    QCOMPARE(firstSession->splitScrollY(), 320.25);
    QCOMPARE(splitScrollSpy.count(), 2);

    firstSession->secondaryViewState()->setZoom(1.75);
    firstSession->secondaryViewState()->setCurrentPage(3);
    QCOMPARE(firstSession->secondaryViewState()->zoom(), 1.75);
    QCOMPARE(firstSession->secondaryViewState()->currentPage(), 3);
    firstSession->setSplitScrollX(144.5);
    firstSession->setSplitScrollY(320.25);
    QCOMPARE(splitScrollSpy.count(), 2);

    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(secondSession);
    QVERIFY(secondSession != firstSession);
    QCOMPARE(secondSession->pageDisplayMode(), PDFDocumentSession::ContinuousMode);
    QVERIFY(!secondSession->separateCoverPage());
    QCOMPARE(secondSession->splitViewMode(), PDFDocumentSession::NoSplit);
    QVERIFY(!secondSession->splitViewEnabled());
    QCOMPARE(secondSession->splitScrollX(), 0.0);
    QCOMPARE(secondSession->splitScrollY(), 0.0);
    QVERIFY(!secondSession->splitScrollPositionInitialized());

    secondSession->setPageDisplayMode(PDFDocumentSession::SinglePageMode);
    secondSession->setSplitViewMode(PDFDocumentSession::VerticalSplit);
    secondSession->setSplitScrollX(20.0);
    secondSession->setSplitScrollY(40.0);
    QVERIFY(secondSession->splitScrollPositionInitialized());
    QCOMPARE(secondSession->splitViewMode(), PDFDocumentSession::VerticalSplit);
    QCOMPARE(firstSession->pageDisplayMode(), PDFDocumentSession::FacingMode);
    QVERIFY(firstSession->separateCoverPage());
    QVERIFY(firstSession->splitViewEnabled());
    QCOMPARE(firstSession->splitViewMode(), PDFDocumentSession::HorizontalSplit);
    QCOMPARE(firstSession->splitScrollX(), 144.5);
    QCOMPARE(firstSession->splitScrollY(), 320.25);

    const QString duplicatePath = QFileInfo(firstPath).dir().filePath(
        QStringLiteral("./page-display-first.pdf"));
    QCOMPARE(tabs.open(QUrl::fromLocalFile(duplicatePath)), firstSession);
    QCOMPARE(tabs.count(), 2);
    QCOMPARE(tabs.currentSession(), firstSession);
    QCOMPARE(firstSession->pageDisplayMode(), PDFDocumentSession::FacingMode);
    QVERIFY(firstSession->separateCoverPage());
    QVERIFY(firstSession->splitViewEnabled());
    QCOMPARE(firstSession->splitViewMode(), PDFDocumentSession::HorizontalSplit);
    QCOMPARE(firstSession->splitScrollX(), 144.5);
    QCOMPARE(firstSession->splitScrollY(), 320.25);
}

void PDFEditControllerTest::clearsTabIdentityWhenDocumentCloses()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-identity.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFTabModel tabs;
    PDFDocumentSession *session = tabs.open(QUrl::fromLocalFile(sourcePath));
    QVERIFY(session);
    QCOMPARE(session->title(), QStringLiteral("tab-identity.pdf"));
    QCOMPARE(session->filePath(), sourcePath);

    session->document()->close();

    QVERIFY(session->title().isEmpty());
    QVERIFY(session->filePath().isEmpty());
}

void PDFEditControllerTest::keepsTabDraftsIndependent()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-draft-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-draft-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(firstSession);
    QVERIFY(secondSession);

    PDFEditController *firstController = firstSession->editController();
    const QVariantMap candidate = candidateForText(
        waitForCandidates(*firstController, 0),
        QString::fromLatin1(kRegularText));
    QVERIFY(!candidate.isEmpty());
    QVERIFY(firstController->selectCandidate(
        0,
        candidate.value(QStringLiteral("objectId")).toString()));
    firstController->commitSelectedText(QStringLiteral("Draft in first tab"));

    QVERIFY(firstSession->hasDrafts());
    QVERIFY(!secondSession->hasDrafts());
    QVERIFY(tabs.hasDrafts());
    QVERIFY(!tabs.closeSession(firstSession));
    QCOMPARE(tabs.count(), 2);

    firstController->discardDrafts();
    QVERIFY(!firstSession->hasDrafts());
    QVERIFY(tabs.closeSession(firstSession));
    QCOMPARE(tabs.count(), 1);
    QCOMPARE(tabs.currentIndex(), 0);
    QCOMPARE(tabs.currentSession(), secondSession);
    QVERIFY(!tabs.hasDrafts());
}

void PDFEditControllerTest::savesOnlyRequestedTab()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-second.pdf"));
    const QString firstOutputPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-first-output.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(firstSession);
    QVERIFY(secondSession);

    const auto addDraft = [](PDFDocumentSession *session, const QString &text) {
        PDFEditController *controller = session->editController();
        const QVariantMap candidate = candidateForText(
            waitForCandidates(*controller, 0),
            QString::fromLatin1(kRegularText));
        if (candidate.isEmpty()
            || !controller->selectCandidate(
                0,
                candidate.value(QStringLiteral("objectId")).toString())) {
            return false;
        }
        controller->commitSelectedText(text);
        return controller->hasDrafts();
    };
    QVERIFY(addDraft(firstSession, QStringLiteral("First tab draft")));
    QVERIFY(addDraft(secondSession, QStringLiteral("Second tab draft")));

    QSignalSpy saveFinishedSpy(&tabs, &PDFTabModel::sessionSaveFinished);
    QVERIFY(tabs.requestSaveAs(
        firstSession,
        QUrl::fromLocalFile(firstOutputPath)));
    QVERIFY(tabs.anySaving());
    QTRY_COMPARE_WITH_TIMEOUT(saveFinishedSpy.count(), 1, 5000);
    QCOMPARE(saveFinishedSpy.constFirst().at(0).value<PDFDocumentSession *>(),
             firstSession);
    QCOMPARE(saveFinishedSpy.constFirst().at(1).toBool(), true);
    QVERIFY(!tabs.anySaving());
    QVERIFY(!firstSession->hasDrafts());
    QVERIFY(secondSession->hasDrafts());
    QCOMPARE(firstSession->document()->source().toLocalFile(), firstOutputPath);
    QCOMPARE(secondSession->document()->source().toLocalFile(), secondPath);
    QVERIFY(normalizedPageText(firstOutputPath).contains(
        QStringLiteral("First tab draft")));
    QVERIFY(normalizedPageText(secondPath).contains(
        QString::fromLatin1(kRegularText)));
}

void PDFEditControllerTest::rejectsSaveAsOverAnotherOpenTab()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-as-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("tab-save-as-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));

    PDFTabModel tabs;
    PDFDocumentSession *firstSession = tabs.open(QUrl::fromLocalFile(firstPath));
    PDFDocumentSession *secondSession = tabs.open(QUrl::fromLocalFile(secondPath));
    QVERIFY(firstSession);
    QVERIFY(secondSession);

    QSignalSpy statusSpy(&tabs, &PDFTabModel::statusMessage);
    QVERIFY(!tabs.requestSaveAs(firstSession, secondSession->document()->source()));
    QCOMPARE(statusSpy.count(), 1);
    QVERIFY(!firstSession->saving());
    QCOMPARE(firstSession->document()->source().toLocalFile(), firstPath);
    QCOMPARE(secondSession->document()->source().toLocalFile(), secondPath);
}
