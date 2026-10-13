#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFMergeController.h"

using namespace PDFEditControllerTestSupport;

void PDFEditControllerTest::guardsMergeAgainstOpenPdfDraftsAndDestinations()
{
    const QString first = m_temporaryDirectory.filePath(QStringLiteral("merge-guard-first.pdf"));
    const QString second = m_temporaryDirectory.filePath(QStringLiteral("merge-guard-second.pdf"));
    QVERIFY(createFontStylePdf(first));
    QVERIFY(createFontStylePdf(second));
    const QUrl firstUrl = QUrl::fromLocalFile(first);
    const QUrl secondUrl = QUrl::fromLocalFile(second);
    const QUrl output = QUrl::fromLocalFile(m_temporaryDirectory.filePath(QStringLiteral("merge-guard-output.pdf")));
    PDFTabModel tabs;
    auto *firstSession = tabs.open(firstUrl);
    auto *secondSession = tabs.open(secondUrl);
    QVERIFY(firstSession && secondSession);
    QCOMPARE(tabs.mergeValidationError({firstUrl}, secondUrl), QStringLiteral("MergePdfDestinationOpen"));
    QVERIFY(tabs.mergeValidationError({firstUrl, secondUrl}, output).isEmpty());
    firstSession->editController()->setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!firstSession->editController()->addStickyNote(0, QPointF(0.2, 0.2)).isEmpty());
    QVERIFY(!tabs.mergeValidationError({firstUrl, secondUrl}, output).isEmpty());
    // Drafts in an unrelated open document do not block saved inputs.
    QVERIFY(tabs.mergeValidationError({secondUrl}, output).isEmpty());
#ifndef Q_OS_WIN
    const QString alias = m_temporaryDirectory.filePath(QStringLiteral("merge-guard-alias.pdf"));
    QVERIFY(QFile::link(first, alias));
    QVERIFY(!tabs.mergeValidationError({QUrl::fromLocalFile(alias), secondUrl}, output).isEmpty());
    QCOMPARE(tabs.mergeValidationError({secondUrl}, QUrl::fromLocalFile(alias)),
             QStringLiteral("MergePdfDestinationOpen"));
#endif
    firstSession->editController()->discardDrafts();
    QVERIFY(tabs.mergeValidationError({firstUrl, secondUrl}, output).isEmpty());
}

void PDFEditControllerTest::mergesPdfsThroughMainWindow()
{
    const QString first = m_temporaryDirectory.filePath(QStringLiteral("merge-ui-first.pdf"));
    const QString second = m_temporaryDirectory.filePath(QStringLiteral("merge-ui-second.pdf"));
    QVERIFY(createFontStylePdf(first));
    QVERIFY(createFontStylePdf(second));
    const QUrl output = QUrl::fromLocalFile(m_temporaryDirectory.filePath(QStringLiteral("merge-ui-output.pdf")));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QVERIFY(root->setProperty("allowWindowClose", true));
    auto *tabs = root->findChild<PDFTabModel *>();
    auto *controller = root->findChild<PDFMergeController *>();
    auto *ribbonTabs = root->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(tabs && controller && ribbonTabs);
    QVERIFY(ribbonTabs->setProperty("currentIndex", 1));
    const auto button = [&]() { return root->findChild<QObject *>(QStringLiteral("mergePdfButton")); };
    QTRY_VERIFY(button());
    QVERIFY(button()->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(button(), "clicked"));
    const auto dialog = [&]() { return root->findChild<QObject *>(QStringLiteral("mergePdfDialog")); };
    QTRY_VERIFY(dialog() && dialog()->property("opened").toBool());
    QVERIFY(controller->addFiles({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)}));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 30000);
    QCOMPARE(controller->files().size(), 2);
    const int expectedPages = controller->totalPages();
    QVERIFY(controller->canMerge());

    auto *source = tabs->open(QUrl::fromLocalFile(first));
    QVERIFY(source);
    source->editController()->setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!source->editController()->addStickyNote(0, QPointF(0.2, 0.2)).isEmpty());
    QVERIFY(QMetaObject::invokeMethod(root.get(), "mergePdfTo", Q_ARG(QVariant, QVariant(output))));
    QVERIFY(!controller->busy());
    QVERIFY(!dialog()->property("errorText").toString().isEmpty());
    QVERIFY(!QFileInfo::exists(output.toLocalFile()));
    source->editController()->discardDrafts();
    controller->moveFile(1, 0);
    QCOMPARE(controller->files().first().toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("merge-ui-second.pdf"));
    QSignalSpy finished(controller, &PDFMergeController::finished);
    QVERIFY(QMetaObject::invokeMethod(root.get(), "mergePdfTo", Q_ARG(QVariant, QVariant(output))));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 30000);
    QCOMPARE(finished.size(), 1);
    QVERIFY2(finished.first().first().toBool(), qPrintable(controller->statusText()));
    QTRY_COMPARE(tabs->count(), 2);
    QCOMPARE(tabs->currentSession()->document()->source(),
             QUrl::fromLocalFile(QFileInfo(output.toLocalFile()).canonicalFilePath()));
    QCOMPARE(tabs->currentSession()->document()->pageCount(), expectedPages);
    QVERIFY(!source->hasDrafts());
    QTRY_VERIFY(!dialog());
}
