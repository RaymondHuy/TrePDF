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
