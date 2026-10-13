#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"
#include "PDFTextSelectionController.h"
#include "PDFTextSelectionItem.h"

#include <QClipboard>
#include <QKeySequence>
#include <qpdf/qpdf-c.h>

using namespace PDFEditControllerTestSupport;

namespace {

bool createViewSelectionPdf(const QString &path, int rotation = 0,
                            bool cropped = false, bool interactiveRegions = false)
{
    const QByteArray contents = "BT /F1 24 Tf 40 250 Td (Alpha beta) Tj "
                                "0 -40 Td (Second line) Tj ET";
    QByteArray page = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] "
                      "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R ";
    page += "/Rotate " + QByteArray::number(rotation) + " ";
    if (cropped)
        page += "/CropBox [20 20 380 280] ";
    if (interactiveRegions)
        page += "/Annots [6 0 R 7 0 R] ";
    page += ">>";
    QVector<QByteArray> objects = {
        QByteArray("<< /Type /Catalog /Pages 2 0 R ")
            + (interactiveRegions ? "/AcroForm << /Fields [6 0 R] >> " : "") + ">>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>", page,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Length " + QByteArray::number(contents.size()) + " >>\nstream\n"
            + contents + "\nendstream"
    };
    if (interactiveRegions) {
        objects.append("<< /Type /Annot /Subtype /Widget /FT /Tx /T (Input) "
                       "/Rect [38 243 104 272] /F 4 >>");
        objects.append("<< /Type /Annot /Subtype /Link /Rect [106 243 160 272] "
                       "/Border [0 0 0] /A << /S /URI /URI (https://example.com/) >> >>");
    }
    return writePdfObjects(path, objects);
}

// Derive pointer coordinates from the fixture's source glyphs, independently
// of the selection item's hit-test/cache implementation.
QVector<QPointF> viewSelectionGlyphCenters(const QString &path)
{
    PDFiumProcessLock lock;
    const QByteArray encoded = QFile::encodeName(path);
    FPDF_DOCUMENT document = FPDF_LoadDocument(encoded.constData(), nullptr);
    if (!document)
        return {};
    const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(document); });
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    if (!page)
        return {};
    const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
    FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
    if (!text)
        return {};
    const auto closeText = qScopeGuard([&] { FPDFText_ClosePage(text); });
    QVector<QPointF> result;
    for (int index = 0; index < FPDFText_CountChars(text); ++index) {
        const auto character = FPDFText_GetUnicode(text, index);
        if (character == '\r' || character == '\n')
            continue;
        double left = 0, right = 0, bottom = 0, top = 0;
        if (!FPDFText_GetCharBox(text, index, &left, &right, &bottom, &top))
            return {};
        int x = 0, y = 0;
        if (!FPDF_PageToDevice(page, 0, 0, 10000, 10000, 0,
                              (left + right) / 2, (bottom + top) / 2, &x, &y))
            return {};
        result.append(QPointF(x / 10000.0, y / 10000.0));
    }
    return result;
}

QPoint selectionScenePoint(QQuickItem *item, const QPointF &normalized)
{
    return item->mapToScene(QPointF(normalized.x() * item->width(),
                                    normalized.y() * item->height())).toPoint();
}

void dragViewSelection(QQuickWindow *window, QQuickItem *item,
                       const QPointF &start, const QPointF &end)
{
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                      selectionScenePoint(item, start));
    QTest::mouseMove(window, selectionScenePoint(item, end), 20);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                        selectionScenePoint(item, end));
}

void standardSelectionKey(QQuickWindow *window, QKeySequence::StandardKey key)
{
    const auto sequence = QKeySequence(key);
    QTest::keyClick(window, sequence[0].key(), sequence[0].keyboardModifiers());
}

class SelectionPointerSink final : public QQuickItem
{
public:
    explicit SelectionPointerSink(QQuickItem *parent) : QQuickItem(parent)
    {
        setAcceptedMouseButtons(Qt::LeftButton);
    }
    int presses = 0;
protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        ++presses;
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override { event->accept(); }
};

PDFTextSelectionItem *selectionItemIn(QQuickItem *parent)
{
    if (auto *item = qobject_cast<PDFTextSelectionItem *>(parent);
        item && item->page() == 0 && item->isVisible())
        return item;
    for (auto *child : parent->childItems()) {
        if (auto *found = selectionItemIn(child))
            return found;
    }
    return nullptr;
}

bool createCopyRestrictedPdf(const QString &source, const QString &output)
{
    qpdf_data handle = qpdf_init();
    if (!handle)
        return false;
    const auto cleanup = qScopeGuard([&] {
        qpdf_get_error(handle);
        qpdf_cleanup(&handle);
    });
    qpdf_silence_errors(handle);
    qpdf_set_suppress_warnings(handle, QPDF_TRUE);
    if (qpdf_read(handle, QFile::encodeName(source).constData(), "") != QPDF_SUCCESS
        || qpdf_init_write(handle, QFile::encodeName(output).constData()) != QPDF_SUCCESS)
        return false;
    qpdf_set_r6_encryption_parameters2(handle, "selection-reader", "selection-owner",
        QPDF_TRUE, QPDF_FALSE, QPDF_TRUE, QPDF_TRUE, QPDF_TRUE, QPDF_TRUE,
        qpdf_r3p_full, QPDF_TRUE);
    return !qpdf_has_error(handle) && qpdf_write(handle) == QPDF_SUCCESS;
}

} // namespace

void PDFEditControllerTest::selectsAndCopiesViewText()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("view-selection.pdf"));
    QVERIFY(createViewSelectionPdf(path));
    const auto centers = viewSelectionGlyphCenters(path);
    QCOMPARE(centers.size(), 21);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController edits;
    edits.setDocument(&document);
    PDFTextSelectionController selection;
    QQuickWindow window;
    window.resize(600, 450);
    PDFTextSelectionItem item(window.contentItem());
    item.setSize(QSizeF(600, 450));
    item.setController(&selection);
    item.setDocument(&document);
    item.setActive(true);
    window.show();
    QTRY_VERIFY(window.isExposed() && item.ready());
    const QString oldClipboard = QGuiApplication::clipboard()->text();
    const auto restoreClipboard = qScopeGuard([&] { QGuiApplication::clipboard()->setText(oldClipboard); });
    QSignalSpy began(&item, &PDFTextSelectionItem::selectionStarted);
    dragViewSelection(&window, &item, centers.first(), centers.last());
    QCOMPARE(selection.selectedText(), QStringLiteral("Alpha beta\nSecond line"));
    QCOMPARE(selection.selectedPage(), 0);
    QVERIFY(selection.hasSelection());
    QVERIFY(!began.isEmpty());
    standardSelectionKey(&window, QKeySequence::Copy);
    QCOMPARE(QGuiApplication::clipboard()->text(), selection.selectedText());

    dragViewSelection(&window, &item, centers.last(), centers.first());
    QCOMPARE(selection.selectedText(), QStringLiteral("Alpha beta\nSecond line"));
    QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier,
                       selectionScenePoint(&item, centers[7]));
    QCOMPARE(selection.selectedText(), QStringLiteral("beta"));
    QSignalSpy context(&item, &PDFTextSelectionItem::contextMenuRequested);
    QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier,
                      selectionScenePoint(&item, centers[7]));
    QCOMPARE(context.size(), 1);
    QCOMPARE(selection.selectedText(), QStringLiteral("beta"));
    QVERIFY(selection.copy());
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("beta"));
    standardSelectionKey(&window, QKeySequence::SelectAll);
    QCOMPARE(selection.selectedText(), QStringLiteral("Alpha beta\nSecond line"));
    QTest::keyClick(&window, Qt::Key_Escape);
    QVERIFY(!selection.hasSelection());
    QVERIFY(!selection.copy());
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("beta"));
    dragViewSelection(&window, &item, centers.first(), centers[4]);
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(550, 420));
    QVERIFY(!selection.hasSelection());
    QVERIFY(!edits.hasDrafts());
    QVERIFY(!edits.canUndo());
}

void PDFEditControllerTest::selectsViewTextOnRotatedPages_data()
{
    QTest::addColumn<int>("intrinsicRotation");
    QTest::addColumn<int>("viewRotation");
    QTest::addColumn<QString>("expectedText");
    QTest::addColumn<int>("betaCharacter");
    // PDFium orders the cropped 90-degree fixture's second line first. View
    // rotation alone must leave source extraction order unchanged.
    QTest::newRow("intrinsic-90") << 90 << 0
        << QStringLiteral("Second line\nAlpha beta") << 18;
    QTest::newRow("view-90") << 0 << 90
        << QStringLiteral("Alpha beta\nSecond line") << 7;
    QTest::newRow("combined-180-270") << 180 << 270
        << QStringLiteral("Alpha beta\nSecond line") << 7;
    QTest::newRow("intrinsic-270") << 270 << 0
        << QStringLiteral("Alpha beta\nSecond line") << 7;
}

void PDFEditControllerTest::selectsViewTextOnRotatedPages()
{
    QFETCH(int, intrinsicRotation);
    QFETCH(int, viewRotation);
    QFETCH(QString, expectedText);
    QFETCH(int, betaCharacter);
    const QString path = m_temporaryDirectory.filePath(
        QStringLiteral("view-selection-rotation-%1-%2.pdf").arg(intrinsicRotation).arg(viewRotation));
    QVERIFY(createViewSelectionPdf(path, intrinsicRotation, true));
    const auto centers = viewSelectionGlyphCenters(path);
    QCOMPARE(centers.size(), 21);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFTextSelectionController selection;
    QQuickWindow window;
    window.resize(640, 640);
    PDFTextSelectionItem item(window.contentItem());
    const QSizeF size = document.pageSize(0);
    item.setSize(size);
    item.setPosition(QPointF((640 - size.width()) / 2, (640 - size.height()) / 2));
    item.setRotation(viewRotation);
    item.setController(&selection);
    item.setDocument(&document);
    item.setActive(true);
    window.show();
    QTRY_VERIFY(window.isExposed() && item.ready());
    dragViewSelection(&window, &item, centers.first(), centers.last());
    QCOMPARE(selection.selectedText(), expectedText);
    QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier,
                       selectionScenePoint(&item, centers[betaCharacter]));
    QCOMPARE(selection.selectedText(), QStringLiteral("beta"));
}

void PDFEditControllerTest::clearsViewTextSelectionAcrossSourceChanges()
{
    const QString first = m_temporaryDirectory.filePath(QStringLiteral("view-selection-before.pdf"));
    const QString second = m_temporaryDirectory.filePath(QStringLiteral("view-selection-unicode.pdf"));
    QVERIFY(createViewSelectionPdf(first));
    QVERIFY(createEmbeddedTestFontPdf(second));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(first)));
    const auto centers = viewSelectionGlyphCenters(first);
    QCOMPARE(centers.size(), 21);
    PDFTextSelectionController selection;
    QQuickWindow window;
    window.resize(600, 450);
    PDFTextSelectionItem item(window.contentItem());
    item.setSize(QSizeF(600, 450));
    item.setController(&selection);
    item.setDocument(&document);
    item.setActive(true);
    window.show();
    QTRY_VERIFY(window.isExposed() && item.ready());
    dragViewSelection(&window, &item, centers.first(), centers[4]);
    QCOMPARE(selection.selectedText(), QStringLiteral("Alpha"));
    item.setActive(false);
    QVERIFY(!selection.hasSelection());
    QVERIFY(!item.ready());
    item.setActive(true);
    QTRY_VERIFY(item.ready());
    dragViewSelection(&window, &item, centers.first(), centers[4]);
    QVERIFY(document.open(QUrl::fromLocalFile(second)));
    QVERIFY(!selection.hasSelection());
    QTRY_VERIFY(item.ready());
    item.forceActiveFocus();
    standardSelectionKey(&window, QKeySequence::SelectAll);
    QCOMPARE(selection.selectedText(), QStringLiteral("Tiếng Việt gốc\nKhông đổi"));
    item.setPage(1);
    QVERIFY(!selection.hasSelection());
    QVERIFY(!item.ready());
    item.setPage(0);
    // Invalidate immediately while a new asynchronous request is pending.
    item.setDocument(nullptr);
    QCoreApplication::processEvents();
    QVERIFY(!item.ready());
    QVERIFY(!selection.hasSelection());
}

void PDFEditControllerTest::preservesFormsAndLinksDuringViewTextSelection()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("view-selection-interactive.pdf"));
    QVERIFY(createViewSelectionPdf(path, 0, false, true));
    const auto centers = viewSelectionGlyphCenters(path);
    QCOMPARE(centers.size(), 21);
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFTextSelectionController selection;
    QQuickWindow window;
    window.resize(600, 450);
    SelectionPointerSink underlying(window.contentItem());
    underlying.setSize(QSizeF(600, 450));
    PDFTextSelectionItem item(window.contentItem());
    item.setSize(QSizeF(600, 450));
    item.setZ(1);
    item.setController(&selection);
    item.setDocument(&document);
    item.setActive(true);
    window.show();
    QTRY_VERIFY(window.isExposed() && item.ready());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                      selectionScenePoint(&item, centers[1]));
    QCOMPARE(underlying.presses, 1);
    QVERIFY(!selection.hasSelection());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                      selectionScenePoint(&item, centers[7]));
    QCOMPARE(underlying.presses, 2);
    QVERIFY(!selection.hasSelection());
    dragViewSelection(&window, &item, centers[10], centers.last());
    QCOMPARE(selection.selectedText(), QStringLiteral("Second line"));
    QCOMPARE(underlying.presses, 2);
}

void PDFEditControllerTest::respectsViewTextCopyPermissions()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("selection-permissions-source.pdf"));
    const QString restricted = m_temporaryDirectory.filePath(QStringLiteral("selection-permissions.pdf"));
    QVERIFY(createViewSelectionPdf(source));
    QVERIFY(createCopyRestrictedPdf(source, restricted));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(restricted), QStringLiteral("selection-reader")));
    QVERIFY(!document.allowsTextCopy());
    PDFTextSelectionController selection;
    QQuickWindow window;
    window.resize(600, 450);
    PDFTextSelectionItem item(window.contentItem());
    item.setSize(QSizeF(600, 450));
    item.setController(&selection);
    item.setDocument(&document);
    item.setActive(true);
    window.show();
    QTRY_VERIFY(window.isExposed());
    QVERIFY(!item.ready());
    item.forceActiveFocus();
    standardSelectionKey(&window, QKeySequence::SelectAll);
    QVERIFY(!selection.hasSelection());
    QVERIFY(!selection.copy());
    QVERIFY(document.open(QUrl::fromLocalFile(restricted), QStringLiteral("selection-owner")));
    QVERIFY(document.allowsTextCopy());
    QTRY_VERIFY(item.ready());
    item.forceActiveFocus();
    standardSelectionKey(&window, QKeySequence::SelectAll);
    QCOMPARE(selection.selectedText(), QStringLiteral("Alpha beta\nSecond line"));
    // Reopening with reader credentials clears the formerly authorized selection.
    QVERIFY(document.open(QUrl::fromLocalFile(restricted), QStringLiteral("selection-reader")));
    QVERIFY(!document.allowsTextCopy());
    QVERIFY(!selection.hasSelection());
    QVERIFY(!selection.copy());
}

void PDFEditControllerTest::routesViewTextSelectionAcrossPanesAndModes()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("view-selection-workspace.pdf"));
    QVERIFY(createViewSelectionPdf(path));
    const auto centers = viewSelectionGlyphCenters(path);
    QCOMPARE(centers.size(), 21);
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    QVERIFY(window);
    window->setProperty("allowWindowClose", true);
    window->resize(1400, 900);
    auto *tabs = window->findChild<PDFTabModel *>();
    QVERIFY(tabs);
    auto *session = tabs->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    session->setThumbnailsVisible(false);
    QObject *workspace = nullptr;
    QTRY_VERIFY((workspace = window->findChild<QObject *>(QStringLiteral("documentWorkspace"))));
    QTRY_VERIFY(workspace->property("initialViewReady").toBool());
    QVERIFY(QMetaObject::invokeMethod(workspace, "setSplitViewMode",
        Q_ARG(QVariant, int(PDFDocumentSession::VerticalSplit))));
    QQuickItem *primary = nullptr;
    QQuickItem *secondary = nullptr;
    QTRY_VERIFY((primary = window->findChild<QQuickItem *>(QStringLiteral("primaryDocumentView"))));
    QTRY_VERIFY((secondary = window->findChild<QQuickItem *>(QStringLiteral("secondaryDocumentView"))));
    PDFTextSelectionItem *primaryItem = nullptr;
    PDFTextSelectionItem *secondaryItem = nullptr;
    QTRY_VERIFY((primaryItem = selectionItemIn(primary)) && primaryItem->ready());
    QTRY_VERIFY((secondaryItem = selectionItemIn(secondary)) && secondaryItem->ready());
    QVERIFY(primaryItem->controller() != secondaryItem->controller());
    dragViewSelection(window, primaryItem, centers.first(), centers[4]);
    QCOMPARE(primaryItem->controller()->selectedText(), QStringLiteral("Alpha"));
    QCOMPARE(workspace->property("activePaneIndex").toInt(), 0);
    dragViewSelection(window, secondaryItem, centers[6], centers[9]);
    QCOMPARE(secondaryItem->controller()->selectedText(), QStringLiteral("beta"));
    QCOMPARE(workspace->property("activePaneIndex").toInt(), 1);
    const QString oldClipboard = QGuiApplication::clipboard()->text();
    const auto restoreClipboard = qScopeGuard([&] { QGuiApplication::clipboard()->setText(oldClipboard); });
    standardSelectionKey(window, QKeySequence::Copy);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("beta"));
    auto *searchField = window->findChild<QQuickItem *>(QStringLiteral("documentSearchField"));
    QVERIFY(searchField);
    QVERIFY(searchField->setProperty("text", QStringLiteral("Search query")));
    searchField->forceActiveFocus();
    QVERIFY(QMetaObject::invokeMethod(searchField, "selectAll"));
    standardSelectionKey(window, QKeySequence::Copy);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Search query"));
    QVERIFY(!session->hasDrafts());
    auto *ribbon = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(ribbon);
    QVERIFY(ribbon->setProperty("currentIndex", 1));
    QTRY_VERIFY(!primaryItem->active() && !secondaryItem->active());
    QVERIFY(!primaryItem->controller()->hasSelection());
    QVERIFY(!secondaryItem->controller()->hasSelection());
    QVERIFY(ribbon->setProperty("currentIndex", 0));
    QTRY_VERIFY(primaryItem->ready() && secondaryItem->ready());
    QVERIFY(!session->editController()->addText(0, 0.1, 0.6).isEmpty());
    session->editController()->updateSelectedTextPreview(QStringLiteral("Pending text"));
    session->editController()->finishSelectedTextEdit();
    QTRY_VERIFY(!primaryItem->active() && !secondaryItem->active());
    session->editController()->discardDrafts();
    QTRY_VERIFY(primaryItem->ready() && secondaryItem->ready());
}

void PDFEditControllerTest::boundsViewTextSelection()
{
    const QString oversized = m_temporaryDirectory.filePath(QStringLiteral("view-selection-oversized.pdf"));
    const QString ordinary = m_temporaryDirectory.filePath(QStringLiteral("view-selection-after-limit.pdf"));
    // Keep each PDF string below PDFium's 32,767-character string limit and
    // both lines inside the crop box, so the page itself exceeds our scan cap.
    const QByteArray contents = QByteArray("BT /F1 0.01 Tf 1 990 Td (")
        + QByteArray(25001, 'A') + ") Tj 0 -10 Td ("
        + QByteArray(25000, 'B') + ") Tj ET";
    QVERIFY(writePdfObjects(oversized, {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 1000 1000] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Length " + QByteArray::number(contents.size()) + " >>\nstream\n"
            + contents + "\nendstream"
    }));
    {
        PDFiumProcessLock lock;
        const QByteArray encoded = QFile::encodeName(oversized);
        FPDF_DOCUMENT raw = FPDF_LoadDocument(encoded.constData(), nullptr);
        QVERIFY(raw);
        const auto closeDocument = qScopeGuard([&] { FPDF_CloseDocument(raw); });
        FPDF_PAGE page = FPDF_LoadPage(raw, 0);
        QVERIFY(page);
        const auto closePage = qScopeGuard([&] { FPDF_ClosePage(page); });
        FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
        QVERIFY(text);
        const auto closeText = qScopeGuard([&] { FPDFText_ClosePage(text); });
        const int characterCount = FPDFText_CountChars(text);
        QVERIFY2(characterCount > 50000,
                 qPrintable(QStringLiteral("Oversized fixture has only %1 characters").arg(characterCount)));
    }
    QVERIFY(createViewSelectionPdf(ordinary));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(oversized)));
    PDFTextSelectionController selection;
    QQuickWindow window;
    window.resize(600, 450);
    PDFTextSelectionItem item(window.contentItem());
    item.setSize(QSizeF(600, 450));
    item.setController(&selection);
    item.setDocument(&document);
    item.setActive(true);
    window.show();
    QTRY_VERIFY(window.isExposed() && item.limited());
    QVERIFY(!item.ready());
    item.forceActiveFocus();
    standardSelectionKey(&window, QKeySequence::SelectAll);
    QVERIFY(!selection.hasSelection());
    QVERIFY(!selection.copy());

    // A rejected page must not poison a later scan or leave partial text cached.
    QVERIFY(document.open(QUrl::fromLocalFile(ordinary)));
    QTRY_VERIFY(item.ready());
    QVERIFY(!item.limited());
    item.forceActiveFocus();
    standardSelectionKey(&window, QKeySequence::SelectAll);
    QCOMPARE(selection.selectedText(), QStringLiteral("Alpha beta\nSecond line"));
}
