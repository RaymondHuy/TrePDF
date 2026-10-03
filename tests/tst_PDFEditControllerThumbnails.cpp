#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

using namespace PDFEditControllerTestSupport;

namespace {
QQuickItem *visualItem(QQuickItem *parent, const QString &name)
{
    if (parent->objectName() == name)
        return parent;
    for (auto *child : parent->childItems()) {
        if (auto *found = visualItem(child, name))
            return found;
    }
    return nullptr;
}

QList<PDFPageItem *> previews(QQuickItem *parent)
{
    QList<PDFPageItem *> result;
    if (auto *page = qobject_cast<PDFPageItem *>(parent);
        page && page->objectName() == QStringLiteral("thumbnailPreview")) {
        result.append(page);
    }
    for (auto *child : parent->childItems())
        result.append(previews(child));
    return result;
}

bool createManyThumbnailPages(const QString &path)
{
    PDFiumProcessLock lock;
    const auto document = FPDF_CreateNewDocument();
    if (!document)
        return false;
    const auto cleanup = qScopeGuard([&]() { FPDF_CloseDocument(document); });
    for (int index = 0; index < 240; ++index) {
        const auto page = FPDFPage_New(document, index,
                                      index % 2 ? 700 : 240,
                                      index % 2 ? 500 : 360);
        if (!page)
            return false;
        const bool ok = addTextObject(document, page, "Helvetica",
                                      QStringLiteral("Thumbnail page %1").arg(index + 1),
                                      24, 280) && FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
        if (!ok)
            return false;
    }
    PdfFileWriter writer(path);
    return writer.open() && FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL);
}
} // namespace

void PDFEditControllerTest::navigatesWithPageThumbnailsAcrossTabsAndSplitPanes()
{
    const auto firstPath = m_temporaryDirectory.filePath(QStringLiteral("thumbnail-first.pdf"));
    const auto secondPath = m_temporaryDirectory.filePath(QStringLiteral("thumbnail-second.pdf"));
    QVERIFY(createThreePageTextPdf(firstPath));
    QVERIFY(createDifferentPageSizesPdf(secondPath));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window);
    window->setProperty("allowWindowClose", true);
    auto *model = window->findChild<PDFTabModel *>();
    QVERIFY(model);
    auto *first = model->open(QUrl::fromLocalFile(firstPath));
    QVERIFY(first);
    QObject *workspace = nullptr;
    QTRY_VERIFY((workspace = window->findChild<QObject *>(QStringLiteral("documentWorkspace"))));
    QTRY_VERIFY(workspace->property("initialViewReady").toBool());
    auto *list = visualItem(window->contentItem(), QStringLiteral("thumbnailList"));
    QVERIFY(list);
    QCOMPARE(list->property("count").toInt(), 3);
    auto *thirdPage = visualItem(list, QStringLiteral("pageThumbnail_2"));
    QVERIFY(thirdPage);
    QVERIFY(QMetaObject::invokeMethod(thirdPage, "clicked"));
    QTRY_COMPARE(first->document()->currentPage(), 2);
    QTRY_COMPARE(list->property("currentIndex").toInt(), 2);

    // The sidebar follows the focused split pane without moving the other one.
    QVERIFY(QMetaObject::invokeMethod(workspace, "setSplitViewMode",
                                      Q_ARG(QVariant, int(PDFDocumentSession::VerticalSplit))));
    QTRY_VERIFY(window->findChild<QObject *>(QStringLiteral("secondaryDocumentView")));
    QVERIFY(workspace->setProperty("activePaneIndex", 1));
    auto *firstPage = visualItem(list, QStringLiteral("pageThumbnail_0"));
    QVERIFY(firstPage);
    QVERIFY(QMetaObject::invokeMethod(firstPage, "clicked"));
    QTRY_COMPARE(first->secondaryViewState()->currentPage(), 0);
    QCOMPARE(first->document()->currentPage(), 2);
    QTRY_COMPARE(list->property("currentIndex").toInt(), 0);

    QVERIFY(workspace->setProperty("activePaneIndex", 0));
    auto *edit = first->editController();
    QCOMPARE(edit->addBlankPage(), 3);
    QTRY_COMPARE(list->property("count").toInt(), 4);
    edit->undo();
    QTRY_COMPARE(list->property("count").toInt(), 3);
    edit->redo();
    QTRY_COMPARE(list->property("count").toInt(), 4);
    edit->discardDrafts();
    QTRY_COMPARE(list->property("count").toInt(), 3);
    first->document()->setCurrentPage(1);
    QVERIFY(edit->deleteCurrentPage() >= 0);
    QTRY_COMPARE(list->property("count").toInt(), 2);
    edit->discardDrafts();
    QTRY_COMPARE(list->property("count").toInt(), 3);

    QVERIFY(QMetaObject::invokeMethod(workspace, "navigateActivePaneToPage", Q_ARG(QVariant, 0)));
    QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
    firstPage = visualItem(list, QStringLiteral("pageThumbnail_0"));
    QVERIFY(firstPage);
    QTRY_VERIFY(firstPage->mapToItem(list, QPointF()).y() >= 0);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      firstPage->mapToScene(QPointF(firstPage->width() / 2, 80)).toPoint());
    QTRY_VERIFY(firstPage->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(first->document()->currentPage(), 1);
    QTRY_COMPARE(list->property("currentIndex").toInt(), 1);
    QTest::keyClick(window, Qt::Key_Up);
    QTRY_COMPARE(first->document()->currentPage(), 0);

    // A same-path Save must replace cached source pixels even with the same page count.
    firstPage = visualItem(list, QStringLiteral("pageThumbnail_0"));
    QVERIFY(firstPage);
    const auto preview = previews(firstPage).first();
    QTRY_VERIFY(preview->draftTextureReady());
    const auto oldRevision = preview->m_cachedRevision;
    QVERIFY(!edit->addText(0, 0.1, 0.5).isEmpty());
    edit->updateSelectedTextPreview(QStringLiteral("Saved thumbnail content"));
    edit->finishSelectedTextEdit();
    QVERIFY(first->hasDrafts());
    QSignalSpy saved(edit, &PDFEditController::saveFinished);
    QVERIFY(edit->requestSave());
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 10000);
    QVERIFY(saved.first().first().toBool());
    QVERIFY(!first->hasDrafts());
    QTRY_VERIFY(preview->draftTextureReady());
    QTRY_VERIFY(preview->m_cachedRevision != oldRevision);
    QCOMPARE(preview->m_cachedRevision, first->document()->renderRevision());

    auto *toggle = window->findChild<QQuickItem *>(QStringLiteral("toggleThumbnailsButton"));
    QVERIFY(toggle);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint());
    QVERIFY(!first->thumbnailsVisible());
    QTRY_VERIFY(!visualItem(window->contentItem(), QStringLiteral("thumbnailList")));
    auto *accessible = QAccessible::queryAccessibleInterface(toggle);
    QVERIFY(accessible && accessible->actionInterface());
    accessible->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
    QTRY_VERIFY(first->thumbnailsVisible());
    QTRY_VERIFY(visualItem(window->contentItem(), QStringLiteral("thumbnailList")));
    accessible->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
    QTRY_VERIFY(!first->thumbnailsVisible());
    auto *second = model->open(QUrl::fromLocalFile(secondPath));
    QVERIFY(second);
    QVERIFY(second->thumbnailsVisible());
    QTRY_VERIFY(visualItem(window->contentItem(), QStringLiteral("thumbnailList")));
    model->setCurrentIndex(0);
    QTRY_VERIFY(!visualItem(window->contentItem(), QStringLiteral("thumbnailList")));
    QCOMPARE(model->currentSession(), first);
    window->close();
}

void PDFEditControllerTest::virtualizesAndReleasesPageThumbnails()
{
    const auto path = m_temporaryDirectory.filePath(QStringLiteral("thumbnail-240-pages.pdf"));
    QVERIFY(createManyThumbnailPages(path));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window);
    window->resize(1000, 800);
    window->setProperty("allowWindowClose", true);
    auto *model = window->findChild<PDFTabModel *>();
    QVERIFY(model);
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    QQuickItem *list = nullptr;
    QTRY_VERIFY((list = visualItem(window->contentItem(), QStringLiteral("thumbnailList"))));
    QTRY_VERIFY(!previews(list).isEmpty());
    QTRY_VERIFY(previews(list).first()->draftTextureReady());
    const auto initial = previews(list).first();
    QVERIFY(initial->m_cachedPixelSize.width() <= 144 * window->devicePixelRatio() + 1);
    QVERIFY(initial->m_cachedPixelSize.height() <= 168 * window->devicePixelRatio() + 1);
    QCOMPARE(initial->editController(), nullptr);

    // Rapid jumps in both directions must retain only viewport-sized work.
    for (int page : {80, 180, 20, 239, 0, 120, 1}) {
        QVERIFY(list->setProperty("contentY", page * 204.0));
        QTest::qWait(20);
        const auto items = previews(list);
        QVERIFY(items.size() < 20);
        int attached = 0;
        for (const auto *item : items) {
            if (item->document())
                ++attached;
        }
        QVERIFY(attached < 12);
    }
    QObject *workspace = window->findChild<QObject *>(QStringLiteral("documentWorkspace"));
    QVERIFY(workspace);
    QVERIFY(QMetaObject::invokeMethod(workspace, "navigateActivePaneToPage", Q_ARG(QVariant, 200)));
    QTRY_COMPARE(list->property("currentIndex").toInt(), 200);
    QTRY_VERIFY(list->property("contentY").toReal() > 39000);

    session->setViewRotation(90);
    auto *card = visualItem(list, QStringLiteral("pageThumbnail_200"));
    QTRY_VERIFY((card = visualItem(list, QStringLiteral("pageThumbnail_200"))));
    const auto preview = previews(card).first();
    QTRY_COMPARE(preview->rotation(), 90.0);
    QVERIFY(preview->width() <= 168);
    QVERIFY(preview->height() <= 144);

    // Collapsing destroys the list and its textures; reopening resumes at the current page.
    QPointer<PDFPageItem> released = preview;
    session->setThumbnailsVisible(false);
    QTRY_VERIFY(released.isNull());
    QVERIFY(previews(window->contentItem()).isEmpty());
    session->setThumbnailsVisible(true);
    QTRY_VERIFY((list = visualItem(window->contentItem(), QStringLiteral("thumbnailList"))));
    QTRY_COMPARE(list->property("currentIndex").toInt(), 200);
    QTRY_VERIFY(list->property("contentY").toReal() > 39000);

    // Closing during queued renders must not deliver images into the next session.
    QVERIFY(list->setProperty("contentY", 2000.0));
    QVERIFY(model->closeSession(session));
    QTRY_VERIFY(previews(window->contentItem()).isEmpty());
    QVERIFY(model->open(QUrl::fromLocalFile(path)));
    QTRY_VERIFY((list = visualItem(window->contentItem(), QStringLiteral("thumbnailList"))));
    QTRY_VERIFY(!previews(list).isEmpty());
    QTRY_VERIFY(previews(list).first()->draftTextureReady());
    QCOMPARE(list->property("currentIndex").toInt(), 0);
    window->close();
}
