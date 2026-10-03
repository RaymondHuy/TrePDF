#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFFileOpenHandler.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileOpenEvent>
#include <QKeySequence>
#include <QMimeData>

using namespace PDFEditControllerTestSupport;

namespace {
QUrl canonicalUrl(const QString &path)
{
    return QUrl::fromLocalFile(QFileInfo(path).canonicalFilePath());
}
} // namespace

void PDFEditControllerTest::filtersAndQueuesLocalPdfOpenRequests()
{
    const QString firstPath = m_temporaryDirectory.filePath(
        QStringLiteral("Tài liệu #1.PDF"));
    const QString secondPath = m_temporaryDirectory.filePath(
        QStringLiteral("file-open-second.pdf"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));
    const QUrl first = canonicalUrl(firstPath);
    const QUrl second = canonicalUrl(secondPath);
    const QString directoryPath = m_temporaryDirectory.filePath(
        QStringLiteral("not-a-file.pdf"));
    QVERIFY(QDir().mkpath(directoryPath));
    const QString textPath = m_temporaryDirectory.filePath(QStringLiteral("not-pdf.txt"));
    QFile textFile(textPath);
    QVERIFY(textFile.open(QIODevice::WriteOnly));
    QVERIFY(textFile.write("ordinary text") > 0);
    textFile.close();
    QUrl withQuery(first);
    withQuery.setQuery(QStringLiteral("download=true"));
    QUrl withFragment(first);
    withFragment.setFragment(QStringLiteral("page=2"));

    PDFFileOpenHandler handler(nullptr);
    const QList<QUrl> mixed = {
        QUrl(QStringLiteral("https://example.invalid/remote.pdf")),
        QUrl::fromLocalFile(textPath),
        QUrl::fromLocalFile(directoryPath),
        QUrl::fromLocalFile(m_temporaryDirectory.filePath(QStringLiteral("missing.pdf"))),
        withQuery, withFragment, first,
        QUrl::fromLocalFile(QFileInfo(firstPath).dir().filePath(
            QStringLiteral("./Tài liệu #1.PDF"))),
        second
    };
    QCOMPARE(handler.localPdfFiles(mixed), (QList<QUrl>{first, second}));

#ifndef Q_OS_WIN
    const QString aliasPath = m_temporaryDirectory.filePath(QStringLiteral("file-open-alias.pdf"));
    QVERIFY(QFile::link(firstPath, aliasPath));
    QCOMPARE(handler.localPdfFiles({first, QUrl::fromLocalFile(aliasPath)}),
             (QList<QUrl>{first}));
#endif

    QSignalSpy pending(&handler, &PDFFileOpenHandler::filesPending);
    QCoreApplication::instance()->installEventFilter(&handler);
    QFileOpenEvent nativeEvent(firstPath);
    QVERIFY(QCoreApplication::sendEvent(QCoreApplication::instance(), &nativeEvent));
    handler.enqueueArguments({second.toString(QUrl::FullyEncoded), firstPath});
    // Finder can send the same file as launch arguments and as an event.
    QCOMPARE(pending.count(), 0);
    QTRY_COMPARE(pending.count(), 1);
    QCOMPARE(handler.takePendingFiles(), (QList<QUrl>{first, second}));
    QVERIFY(handler.takePendingFiles().isEmpty());

    QFileOpenEvent laterEvent(first);
    QVERIFY(QCoreApplication::sendEvent(QCoreApplication::instance(), &laterEvent));
    QTRY_COMPARE(pending.count(), 2);
    QCOMPARE(handler.takePendingFiles(), (QList<QUrl>{first}));
}

void PDFEditControllerTest::opensNativePdfRequestsThroughMainWindow()
{
    const QString plainPath = m_temporaryDirectory.filePath(QStringLiteral("native-open.pdf"));
    QVERIFY(createFontStylePdf(plainPath));
    const QUrl plain = canonicalUrl(plainPath);
    const QUrl firstProtected = canonicalUrl(QFINDTESTDATA("fixtures/password-1.pdf"));
    const QUrl secondProtected = canonicalUrl(QFINDTESTDATA("fixtures/password-2.pdf"));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    auto *handler = PDFFileOpenHandler::create(&engine, &engine);
    QVERIFY(handler);
    QVERIFY(handler->takePendingFiles().isEmpty());
    QFileOpenEvent startupEvent(plain);
    QVERIFY(QCoreApplication::sendEvent(QCoreApplication::instance(), &startupEvent));
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QVERIFY(root->setProperty("allowWindowClose", true));
    auto *tabs = root->findChild<PDFTabModel *>();
    QVERIFY(tabs);
    QTRY_COMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentSession()->document()->source(), plain);

    auto *original = tabs->currentSession();
    original->editController()->setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!original->editController()->addStickyNote(0, QPointF(0.2, 0.2)).isEmpty());
    QFileOpenEvent duplicate(plainPath);
    QVERIFY(QCoreApplication::sendEvent(QCoreApplication::instance(), &duplicate));
    QFileOpenEvent protectedEvent(firstProtected);
    QVERIFY(QCoreApplication::sendEvent(QCoreApplication::instance(), &protectedEvent));
    const auto popup = [&]() {
        return root->findChild<QObject *>(QStringLiteral("pdfPasswordPopup"));
    };
    QTRY_VERIFY(popup() && popup()->property("opened").toBool());
    QCOMPARE(popup()->property("fileName").toString(), QStringLiteral("password-1.pdf"));
    QCOMPARE(tabs->count(), 1);
    QVERIFY(original->editController()->hasDrafts());

    QFileOpenEvent anotherProtectedEvent(secondProtected);
    QVERIFY(QCoreApplication::sendEvent(QCoreApplication::instance(), &anotherProtectedEvent));
    QCoreApplication::processEvents();
    QCOMPARE(popup()->property("fileName").toString(), QStringLiteral("password-1.pdf"));
    auto *cancel = root->findChild<QObject *>(QStringLiteral("cancelPdfPasswordButton"));
    QVERIFY(cancel);
    QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
    QTRY_VERIFY(popup() && popup()->property("opened").toBool()
        && popup()->property("fileName").toString() == QStringLiteral("password-2.pdf"));
    auto *input = root->findChild<QObject *>(QStringLiteral("pdfPasswordInput"));
    auto *open = root->findChild<QObject *>(QStringLiteral("openPasswordPdfButton"));
    QVERIFY(input && open);
    QVERIFY(input->setProperty("text", QStringLiteral("fixture-open-2")));
    QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
    QTRY_COMPARE(tabs->count(), 2);
    QCOMPARE(tabs->currentSession()->document()->source(), secondProtected);
    QVERIFY(original->editController()->hasDrafts());
    QTRY_VERIFY(!popup());
    QVERIFY(handler->takePendingFiles().isEmpty());
}

void PDFEditControllerTest::opensDroppedPdfsWithoutLosingExistingDrafts()
{
    const QString firstPath = m_temporaryDirectory.filePath(QStringLiteral("drop-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(QStringLiteral("thả tài liệu.PDF"));
    QVERIFY(createFontStylePdf(firstPath));
    QVERIFY(createFontStylePdf(secondPath));
    const QUrl first = canonicalUrl(firstPath);
    const QUrl second = canonicalUrl(secondPath);
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QVERIFY(root->setProperty("allowWindowClose", true));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    auto *tabs = root->findChild<PDFTabModel *>();
    QVERIFY(window && tabs);
    window->show();
    QTRY_VERIFY(window->isExposed());
    auto *original = tabs->open(first);
    QVERIFY(original);
    original->editController()->setTool(PDFEditController::StickyNoteTool);
    QVERIFY(!original->editController()->addStickyNote(0, QPointF(0.2, 0.2)).isEmpty());

    const QPoint dropPoint(window->width() / 2, window->height() / 2);
    QMimeData rejectedData;
    rejectedData.setUrls({QUrl(QStringLiteral("https://example.invalid/not-local.pdf"))});
    QDragEnterEvent rejected(dropPoint, Qt::CopyAction, &rejectedData,
                             Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &rejected);
    QVERIFY(!rejected.isAccepted());
    QCOMPARE(tabs->count(), 1);

    QDragLeaveEvent leave;
    QCoreApplication::sendEvent(window, &leave);
    QMimeData pdfData;
    pdfData.setUrls({first, QUrl(QStringLiteral("https://example.invalid/remote.pdf")), second});
    QDragEnterEvent enter(dropPoint, Qt::CopyAction, &pdfData, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(dropPoint), Qt::CopyAction, &pdfData, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &drop);
    QVERIFY(drop.isAccepted());
    QTRY_COMPARE(tabs->count(), 2);
    QCOMPARE(tabs->currentSession()->document()->source(), second);
    QVERIFY(original->editController()->hasDrafts());
}

void PDFEditControllerTest::focusesDocumentSearchWithStandardShortcut()
{
    const QString firstPath = m_temporaryDirectory.filePath(QStringLiteral("shortcut-find-first.pdf"));
    const QString secondPath = m_temporaryDirectory.filePath(QStringLiteral("shortcut-find-second.pdf"));
    QVERIFY(createThreePageTextPdf(firstPath));
    QVERIFY(createThreePageTextPdf(secondPath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QVERIFY(root->setProperty("allowWindowClose", true));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    auto *tabs = root->findChild<PDFTabModel *>();
    auto *ribbonTabs = root->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(window && tabs && ribbonTabs);
    window->resize(800, 700);
    window->show();
    window->requestActivate();
    QTRY_VERIFY(window->isExposed());
    auto *first = tabs->open(QUrl::fromLocalFile(firstPath));
    auto *second = tabs->open(QUrl::fromLocalFile(secondPath));
    QVERIFY(first && second);
    second->searchController()->search(QStringLiteral("Third fixture page"));
    QTRY_COMPARE(second->searchController()->state(), PDFSearchController::Ready);
    QVERIFY(ribbonTabs->setProperty("currentIndex", 1));
    QTRY_VERIFY(!root->findChild<QObject *>(QStringLiteral("documentSearchField")));
    window->contentItem()->forceActiveFocus();
    QTest::keySequence(window, QKeySequence(QKeySequence::Find));
    QTRY_COMPARE(ribbonTabs->property("currentIndex").toInt(), 0);
    QQuickItem *field = nullptr;
    QTRY_VERIFY((field = root->findChild<QQuickItem *>(QStringLiteral("documentSearchField")))
        && field->hasActiveFocus());
    QCOMPARE(tabs->currentSession(), second);
    QCOMPARE(field->property("selectedText").toString(), QStringLiteral("Third fixture page"));
    const QRectF frame = field->mapRectToScene(QRectF(0, 0, field->width(), field->height()));
    QVERIFY2(frame.left() >= 0 && frame.right() <= window->width(),
             "Find must reveal its field when the ribbon is horizontally clipped");
    for (char character : QByteArrayLiteral("First"))
        QTest::keyClick(window, character);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(second->searchController()->query(), QStringLiteral("First"));
    QTRY_COMPARE(second->searchController()->state(), PDFSearchController::Ready);
    QCOMPARE(second->searchController()->resultCount(), 1);
    QVERIFY(first->searchController()->query().isEmpty());

    QTest::keySequence(window, QKeySequence(QKeySequence::Find));
    QTRY_COMPARE(field->property("selectedText").toString(), QStringLiteral("First"));
}

void PDFEditControllerTest::opensPdfDialogWithStandardShortcut()
{
    const bool nativeDialogsDisabled = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    const auto restoreDialogs = qScopeGuard([nativeDialogsDisabled]() {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
    });
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QVERIFY(root->setProperty("allowWindowClose", true));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    auto *tabs = root->findChild<PDFTabModel *>();
    auto *dialog = root->findChild<QObject *>(QStringLiteral("openPdfDialog"));
    QVERIFY(window && tabs && dialog);
    window->show();
    window->requestActivate();
    QTRY_VERIFY(window->isExposed());
    window->contentItem()->forceActiveFocus();
    QVERIFY(!dialog->property("visible").toBool());
    QTest::keySequence(window, QKeySequence(QKeySequence::Open));
    QTRY_VERIFY(dialog->property("visible").toBool());

    const QString path = m_temporaryDirectory.filePath(QStringLiteral("native-during-open-dialog.pdf"));
    QVERIFY(createFontStylePdf(path));
    QFileOpenEvent event(path);
    QVERIFY(QCoreApplication::sendEvent(QCoreApplication::instance(), &event));
    QCoreApplication::processEvents();
    QCOMPARE(tabs->count(), 0);
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QTRY_COMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentSession()->document()->source(), canonicalUrl(path));
}
