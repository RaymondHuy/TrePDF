#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::addsBlankPageWithUndoRedoAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("add-blank-page-source.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.sourcePageCount(), 2);
    const QSizeF currentPageSize = document.pageSize(0);
    const QSizeF followingPageSize = document.pageSize(1);
    QCOMPARE(currentPageSize, QSizeF(595.0, 842.0));
    QCOMPARE(followingPageSize, QSizeF(320.0, 480.0));
    controller.setDocument(&document);

    QSignalSpy pageCountSpy(&document, &PDFDocument::pageCountChanged);
    QSignalSpy pageStructureSpy(
        &document,
        &PDFDocument::pageStructureChanged);
    const int addedPage = controller.addBlankPage();

    QCOMPARE(addedPage, 1);
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), addedPage);
    QCOMPARE(document.pageSize(addedPage), currentPageSize);
    QVERIFY(document.isTransientBlankPage(addedPage));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), -1);
    QCOMPARE(document.sourcePageIndex(2), 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(pageCountSpy.count(), 1);
    QCOMPARE(pageStructureSpy.count(), 1);
    QCOMPARE(pageStructureSpy.constLast().constFirst().toBool(), false);

    QCOMPARE(controller.undo(), 0);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(addedPage), followingPageSize);
    QVERIFY(!document.isTransientBlankPage(addedPage));
    QCOMPARE(document.sourcePageIndex(addedPage), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.canRedo());
    QCOMPARE(pageCountSpy.count(), 2);
    QCOMPARE(pageStructureSpy.count(), 2);
    QCOMPARE(pageStructureSpy.constLast().constFirst().toBool(), false);

    QCOMPARE(controller.redo(), addedPage);
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), addedPage);
    QCOMPARE(document.pageSize(addedPage), currentPageSize);
    QVERIFY(document.isTransientBlankPage(addedPage));
    QCOMPARE(document.sourcePageIndex(2), 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(pageCountSpy.count(), 3);
    QCOMPARE(pageStructureSpy.count(), 3);
    QCOMPARE(pageStructureSpy.constLast().constFirst().toBool(), false);

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.pageSize(addedPage), followingPageSize);
    QVERIFY(!document.isTransientBlankPage(addedPage));
    QCOMPARE(document.sourcePageIndex(addedPage), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QCOMPARE(pageCountSpy.count(), 4);
    QCOMPARE(pageStructureSpy.count(), 4);
    QCOMPARE(pageStructureSpy.constLast().constFirst().toBool(), true);
}

void PDFEditControllerTest::savesBlankPageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-blank-page-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-blank-page-output.pdf"));
    QVERIFY(createDifferentPageSizesPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    const QSizeF currentPageSize = document.pageSize(0);
    const QSizeF followingPageSize = document.pageSize(1);
    controller.setDocument(&document);

    const int addedPage = controller.addBlankPage();
    QCOMPARE(addedPage, 1);
    QCOMPARE(document.pageSize(addedPage), currentPageSize);
    QCOMPARE(document.sourcePageIndex(addedPage), -1);
    QCOMPARE(document.sourcePageIndex(2), 1);
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.sourcePageCount(), 3);
    QCOMPARE(document.pageSize(addedPage), currentPageSize);
    QVERIFY(!document.isTransientBlankPage(addedPage));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());

    PDFDocument savedDocument;
    QVERIFY(savedDocument.open(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(savedDocument.pageCount(), 3);
    QCOMPARE(savedDocument.pageSize(0), currentPageSize);
    QCOMPARE(savedDocument.pageSize(addedPage), currentPageSize);
    QCOMPARE(savedDocument.pageSize(2), followingPageSize);
    QVERIFY(normalizedPageText(outputPath, addedPage).isEmpty());

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 2);
    QCOMPARE(sourceDocument.pageSize(0), currentPageSize);
    QCOMPARE(sourceDocument.pageSize(1), followingPageSize);
}

void PDFEditControllerTest::savesEditAfterInsertingEarlierPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-inserted-page-edit-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-inserted-page-edit-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    const QVariantMap sourceCandidate = candidateForText(
        waitForCandidates(controller, 1),
        QStringLiteral("Second fixture page"));
    QVERIFY(!sourceCandidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        1,
        sourceCandidate.value(QStringLiteral("objectId")).toString()));
    const QString editedText = QStringLiteral("Edited page after insertion");
    controller.commitSelectedText(editedText);
    QCOMPARE(controller.draftsForPage(1).size(), 1);

    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);
    QCOMPARE(document.pageCount(), 4);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), -1);
    QCOMPARE(document.sourcePageIndex(2), 1);
    QCOMPARE(document.sourcePageIndex(3), 2);
    QVERIFY(document.isTransientBlankPage(1));
    QVERIFY(controller.draftsForPage(1).isEmpty());
    const QVariantList shiftedCandidates = waitForCandidates(controller, 2);
    QVERIFY(!candidateForText(shiftedCandidates, editedText).isEmpty());
    QCOMPARE(controller.draftsForPage(2).size(), 1);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.pageCount(), 4);
    QCOMPARE(document.sourcePageCount(), 4);
    QCOMPARE(document.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(1), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(2), QSizeF(320.0, 480.0));
    QCOMPARE(document.pageSize(3), QSizeF(500.0, 700.0));
    QVERIFY(normalizedPageText(outputPath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(outputPath, 1).isEmpty());
    QVERIFY(normalizedPageText(outputPath, 2).contains(editedText));
    QVERIFY(normalizedPageText(outputPath, 3).contains(
        QStringLiteral("Third fixture page")));
    QVERIFY(!controller.hasDrafts());

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 3);
    QVERIFY(normalizedPageText(sourcePath, 1).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(!normalizedPageText(sourcePath, 1).contains(editedText));
}

void PDFEditControllerTest::savesMultipleInsertedPagesAfterUndoRedo()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-multiple-inserted-pages-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-multiple-inserted-pages-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    QCOMPARE(controller.addBlankPage(), 1);
    document.setCurrentPage(2);
    QCOMPARE(controller.addBlankPage(), 3);
    document.setCurrentPage(0);
    QCOMPARE(controller.addBlankPage(), 1);

    QCOMPARE(document.pageCount(), 6);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), -1);
    QCOMPARE(document.sourcePageIndex(2), -1);
    QCOMPARE(document.sourcePageIndex(3), 1);
    QCOMPARE(document.sourcePageIndex(4), -1);
    QCOMPARE(document.sourcePageIndex(5), 2);

    document.setCurrentPage(3);
    QCOMPARE(controller.deleteCurrentPage(), 3);
    QCOMPARE(document.pageCount(), 5);
    QCOMPARE(document.sourcePageIndex(3), -1);
    QCOMPARE(document.sourcePageIndex(4), 2);

    document.setCurrentPage(2);
    QCOMPARE(controller.deleteCurrentPage(), 2);
    QCOMPARE(document.pageCount(), 4);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), -1);
    QCOMPARE(document.sourcePageIndex(2), -1);
    QCOMPARE(document.sourcePageIndex(3), 2);

    QCOMPARE(controller.undo(), 2);
    QCOMPARE(document.pageCount(), 5);
    QCOMPARE(controller.undo(), 3);
    QCOMPARE(document.pageCount(), 6);
    QCOMPARE(document.sourcePageIndex(3), 1);
    QCOMPARE(controller.redo(), 3);
    QCOMPARE(document.pageCount(), 5);
    QCOMPARE(controller.redo(), 2);
    QCOMPARE(document.pageCount(), 4);

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.pageCount(), 4);
    QCOMPARE(document.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(1), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(2), QSizeF(320.0, 480.0));
    QCOMPARE(document.pageSize(3), QSizeF(500.0, 700.0));
    QVERIFY(normalizedPageText(outputPath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(outputPath, 1).isEmpty());
    QVERIFY(normalizedPageText(outputPath, 2).isEmpty());
    QVERIFY(normalizedPageText(outputPath, 3).contains(
        QStringLiteral("Third fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 0).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 3).contains(
        QStringLiteral("Second fixture page")));

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 3);
    QVERIFY(normalizedPageText(sourcePath, 1).contains(
        QStringLiteral("Second fixture page")));
}

void PDFEditControllerTest::deletesSourcePageWithUndoRedoAndDiscard()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-source-page.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    QSignalSpy pageStructureSpy(
        &document,
        &PDFDocument::pageStructureChanged);
    document.setCurrentPage(1);

    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 1);
    QCOMPARE(document.sourcePageIndex(2), 2);
    QCOMPARE(document.displayPageIndexForSourcePage(0), 0);
    QCOMPARE(document.displayPageIndexForSourcePage(1), 1);
    QCOMPARE(document.displayPageIndexForSourcePage(2), 2);

    QCOMPARE(controller.deleteCurrentPage(), 1);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(500.0, 700.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 2);
    QCOMPARE(document.displayPageIndexForSourcePage(0), 0);
    QCOMPARE(document.displayPageIndexForSourcePage(1), -1);
    QCOMPARE(document.displayPageIndexForSourcePage(2), 1);
    QCOMPARE(pageStructureSpy.count(), 1);
    QCOMPARE(
        pageStructureSpy.constFirst().constFirst().toBool(),
        true);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());

    controller.undo();
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 1);
    QCOMPARE(document.sourcePageIndex(2), 2);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(controller.canRedo());

    controller.redo();
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(500.0, 700.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 2);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(!controller.canRedo());

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(document.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 1);
    QCOMPARE(document.sourcePageIndex(2), 2);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::refusesToDeleteOnlyPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-only-page.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF pageSize = document.pageSize(0);

    QCOMPARE(controller.deleteCurrentPage(), -1);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), pageSize);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::deletesTransientBlankPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-transient-blank-page.pdf"));
    QVERIFY(createFontStylePdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QSizeF sourcePageSize = document.pageSize(0);

    const int blankPage = controller.addBlankPage();
    QCOMPARE(blankPage, 1);
    document.setCurrentPage(blankPage);
    QCOMPARE(document.sourcePageIndex(blankPage), -1);

    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), sourcePageSize);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QVERIFY(controller.canUndo());

    QCOMPARE(controller.undo(), blankPage);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), blankPage);
    QCOMPARE(document.pageSize(blankPage), sourcePageSize);
    QCOMPARE(document.sourcePageIndex(blankPage), -1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.canUndo());
    QVERIFY(controller.canRedo());

    controller.redo();
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.sourcePageIndex(0), 0);

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), sourcePageSize);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
}

void PDFEditControllerTest::restoresLastPageSelectionWhenUndoingDeletion()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-last-page-selection.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    document.setCurrentPage(2);
    const QVariantMap candidate = candidateForText(
        waitForCandidates(controller, 2),
        QStringLiteral("Third fixture page"));
    QVERIFY(!candidate.isEmpty());
    const QString objectId = candidate.value(
        QStringLiteral("objectId")).toString();
    QVERIFY(controller.selectCandidate(2, objectId));

    QCOMPARE(controller.deleteCurrentPage(), 1);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(controller.selectedPage(), -1);

    QCOMPARE(controller.undo(), 2);
    QCOMPARE(document.currentPage(), 2);
    QCOMPARE(controller.selectedPage(), 2);
    QCOMPARE(controller.selectedObjectId(), objectId);

    QCOMPARE(controller.redo(), 1);
    QCOMPARE(document.currentPage(), 1);
    QCOMPARE(controller.selectedPage(), -1);
}

void PDFEditControllerTest::mapsCandidatesAfterDeletingEarlierPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("delete-page-candidate-mapping.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    document.setCurrentPage(0);

    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), QSizeF(320.0, 480.0));
    QCOMPARE(document.sourcePageIndex(0), 1);

    const QVariantList shiftedCandidates = waitForCandidates(controller, 0);
    QVERIFY(!candidateForText(
                 shiftedCandidates,
                 QStringLiteral("Second fixture page")).isEmpty());
    QVERIFY(candidateForText(
                shiftedCandidates,
                QStringLiteral("First fixture page")).isEmpty());

    controller.discardDrafts();
    QCOMPARE(document.pageCount(), 3);
    QCOMPARE(document.sourcePageIndex(0), 0);
}

void PDFEditControllerTest::savesDeletedPageRoundTrip()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-deleted-page-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-deleted-page-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    document.setCurrentPage(1);

    QCOMPARE(controller.deleteCurrentPage(), 1);
    QVERIFY(controller.hasDrafts());
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));

    QCOMPARE(document.source().toLocalFile(), outputPath);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.sourcePageCount(), 2);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(document.pageSize(1), QSizeF(500.0, 700.0));
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 1);
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(normalizedPageText(outputPath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(outputPath, 1).contains(
        QStringLiteral("Third fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 0).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(!normalizedPageText(outputPath, 1).contains(
        QStringLiteral("Second fixture page")));

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 3);
    QCOMPARE(sourceDocument.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(sourceDocument.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(sourceDocument.pageSize(2), QSizeF(500.0, 700.0));
    QVERIFY(normalizedPageText(sourcePath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(sourcePath, 1).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(normalizedPageText(sourcePath, 2).contains(
        QStringLiteral("Third fixture page")));
}

void PDFEditControllerTest::savesEditAfterDeletingEarlierPage()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-shifted-edit-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-shifted-edit-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);
    const QVariantMap sourceCandidate = candidateForText(
        waitForCandidates(controller, 1),
        QStringLiteral("Second fixture page"));
    QVERIFY(!sourceCandidate.isEmpty());
    QVERIFY(controller.selectCandidate(
        1,
        sourceCandidate.value(QStringLiteral("objectId")).toString()));
    const QString editedText = QStringLiteral("Edited shifted second page");
    controller.commitSelectedText(editedText);
    QVERIFY(controller.hasDrafts());
    QCOMPARE(controller.draftsForPage(1).size(), 1);

    document.setCurrentPage(0);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.sourcePageIndex(0), 1);
    QCOMPARE(document.sourcePageIndex(1), 2);
    const QVariantList shiftedCandidates = waitForCandidates(controller, 0);
    QVERIFY(!candidateForText(shiftedCandidates, editedText).isEmpty());
    QCOMPARE(controller.draftsForPage(0).size(), 1);
    QVERIFY(controller.draftsForPage(1).isEmpty());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.pageSize(0), QSizeF(320.0, 480.0));
    QCOMPARE(document.pageSize(1), QSizeF(500.0, 700.0));
    const QString firstOutputPageText = normalizedPageText(outputPath, 0);
    const QString secondOutputPageText = normalizedPageText(outputPath, 1);
    QVERIFY(firstOutputPageText.contains(editedText));
    QVERIFY(!firstOutputPageText.contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(secondOutputPageText.contains(
        QStringLiteral("Third fixture page")));
    QVERIFY(!secondOutputPageText.contains(editedText));
    QVERIFY(!secondOutputPageText.contains(
        QStringLiteral("Second fixture page")));

    QCOMPARE(normalizedPageText(sourcePath, 0), QStringLiteral("First fixture page"));
    QCOMPARE(normalizedPageText(sourcePath, 1), QStringLiteral("Second fixture page"));
    QCOMPARE(normalizedPageText(sourcePath, 2), QStringLiteral("Third fixture page"));
}

void PDFEditControllerTest::savesBlankAfterDeletingAllSourcePages()
{
    const QString sourcePath = m_temporaryDirectory.filePath(
        QStringLiteral("save-blank-without-source-pages-source.pdf"));
    const QString outputPath = m_temporaryDirectory.filePath(
        QStringLiteral("save-blank-without-source-pages-output.pdf"));
    QVERIFY(createThreePageTextPdf(sourcePath));

    PDFDocument document;
    PDFEditController controller;
    QVERIFY(document.open(QUrl::fromLocalFile(sourcePath)));
    controller.setDocument(&document);

    document.setCurrentPage(1);
    QCOMPARE(controller.deleteCurrentPage(), 1);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.sourcePageIndex(1), 2);

    document.setCurrentPage(1);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.sourcePageIndex(0), 0);
    const QSizeF blankPageSize = document.pageSize(0);
    QCOMPARE(blankPageSize, QSizeF(240.0, 360.0));

    const int blankPage = controller.addBlankPage();
    QCOMPARE(blankPage, 1);
    QCOMPARE(document.sourcePageIndex(blankPage), -1);
    QCOMPARE(document.pageSize(blankPage), blankPageSize);

    document.setCurrentPage(0);
    QCOMPARE(controller.deleteCurrentPage(), 0);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.currentPage(), 0);
    QCOMPARE(document.sourcePageIndex(0), -1);
    QCOMPARE(document.pageSize(0), blankPageSize);
    QVERIFY(document.isTransientBlankPage(0));
    QVERIFY(controller.hasDrafts());

    QVERIFY(controller.saveAs(QUrl::fromLocalFile(outputPath)));
    QCOMPARE(document.source().toLocalFile(), outputPath);
    QCOMPARE(document.pageCount(), 1);
    QCOMPARE(document.sourcePageCount(), 1);
    QCOMPARE(document.sourcePageIndex(0), 0);
    QCOMPARE(document.pageSize(0), blankPageSize);
    QVERIFY(!document.isTransientBlankPage(0));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    QVERIFY(!controller.canRedo());
    QVERIFY(normalizedPageText(outputPath).isEmpty());

    PDFDocument sourceDocument;
    QVERIFY(sourceDocument.open(QUrl::fromLocalFile(sourcePath)));
    QCOMPARE(sourceDocument.pageCount(), 3);
    QCOMPARE(sourceDocument.pageSize(0), QSizeF(240.0, 360.0));
    QCOMPARE(sourceDocument.pageSize(1), QSizeF(320.0, 480.0));
    QCOMPARE(sourceDocument.pageSize(2), QSizeF(500.0, 700.0));
    QVERIFY(normalizedPageText(sourcePath, 0).contains(
        QStringLiteral("First fixture page")));
    QVERIFY(normalizedPageText(sourcePath, 1).contains(
        QStringLiteral("Second fixture page")));
    QVERIFY(normalizedPageText(sourcePath, 2).contains(
        QStringLiteral("Third fixture page")));
}
