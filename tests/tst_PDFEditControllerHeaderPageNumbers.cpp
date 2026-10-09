#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QPointer>
#include <limits>

using namespace PDFEditControllerTestSupport;

namespace {
bool createNumberPages(const QString &path, int count, bool shortLastPage = false)
{
    QByteArray kids;
    QVector<QByteArray> pages;
    for (int page = 0; page < count; ++page) {
        kids += QByteArray::number(page + 3) + " 0 R ";
        pages.append(QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 ")
            + (shortLastPage && page == count - 1 ? "20" : "400")
            + "] /Resources << >> >>");
    }
    QVector<QByteArray> objects{
        QByteArray("<< /Type /Catalog /Pages 2 0 R >>"),
        QByteArray("<< /Type /Pages /Kids [") + kids + "] /Count "
            + QByteArray::number(count) + " >>"};
    objects += pages;
    return writePdfObjects(path, objects);
}

QQuickItem *numberItem(QQuickItem *parent, const QString &name)
{
    if (parent->objectName() == name && parent->isVisible())
        return parent;
    for (auto *child : parent->childItems()) {
        if (auto *item = numberItem(child, name))
            return item;
    }
    return nullptr;
}
}

void PDFEditControllerTest::headerPageNumbersRestartAndPersist()
{
    const QString source = m_temporaryDirectory.filePath(QStringLiteral("page-number-source.pdf"));
    const QString target = m_temporaryDirectory.filePath(QStringLiteral("page-number-target.pdf"));
    QVERIFY(createNumberPages(source, 12));
    PDFDocumentSession session;
    QVERIFY(session.open(QUrl::fromLocalFile(source)));
    auto &controller = *session.editController();
    const QStringList expected{QString(), QStringLiteral("i"), QStringLiteral("ii"),
        QStringLiteral("iii"), QStringLiteral("iv"), QStringLiteral("1"),
        QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"),
        QStringLiteral("5"), QString(), QString()};
    // Keep a pre-existing command to prove the numbering is one separate batch.
    QVERIFY(controller.addHeaderText(QStringLiteral("Keep"), QStringLiteral("12")));
    QVERIFY(controller.addHeaderPageNumbers(2, 10, 2, 6, 24, 16, 80));
    for (int page = 0; page < 11; ++page) {
        QCOMPARE(controller.headerPageNumberText(page, 2, 10, 2, 6), expected.at(page));
        const QVariantList drafts = controller.draftsForPage(page);
        QCOMPARE(drafts.size(), expected.at(page).isEmpty() ? 0 : 1);
        if (!drafts.isEmpty()) {
            const QVariantMap draft = drafts.constFirst().toMap();
            QCOMPARE(draft.value(QStringLiteral("text")).toString(), expected.at(page));
            const QRectF frame = draft.value(QStringLiteral("frameRect")).toRectF();
            QVERIFY(qAbs(frame.left() * 300 - 24) < 0.01);
            QVERIFY(qAbs(frame.top() * 400 - 16) < 0.01);
            QVERIFY(frame.bottom() * 400 <= 100.01);
        }
    }
    controller.undo();
    for (int page = 0; page < 11; ++page)
        QVERIFY(controller.draftsForPage(page).isEmpty());
    QCOMPARE(controller.draftsForPage(11).size(), 1);
    QVERIFY(controller.canUndo());
    controller.redo();
    QVERIFY(controller.saveAs(QUrl::fromLocalFile(target)));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(!controller.canUndo());
    for (int page = 0; page < 12; ++page) {
        QCOMPARE(normalizedPageText(target, page).trimmed(),
            page == 11 ? QStringLiteral("Keep") : expected.at(page));
        QVERIFY(normalizedPageText(source, page).trimmed().isEmpty());
    }
    PDFDocument reopened;
    QVERIFY(reopened.open(QUrl::fromLocalFile(target)));
    PDFEditController reopenedController;
    reopenedController.setDocument(&reopened);
    QCOMPARE(reopened.pageCount(), 12);
    QTRY_VERIFY(!candidateForText(reopenedController.candidatesForPage(5), QStringLiteral("1")).isEmpty());
}

void PDFEditControllerTest::headerPageNumberSequencesAndLimits()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("page-number-limits.pdf"));
    QVERIFY(createNumberPages(path, 4001));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QCOMPARE(controller.headerPageNumberText(8, 1, 3999, 1, 0), QStringLiteral("ix"));
    QCOMPARE(controller.headerPageNumberText(43, 1, 3999, 1, 0), QStringLiteral("xliv"));
    QCOMPARE(controller.headerPageNumberText(943, 1, 3999, 1, 0), QStringLiteral("cmxliv"));
    QCOMPARE(controller.headerPageNumberText(1993, 1, 3999, 1, 0), QStringLiteral("mcmxciv"));
    QCOMPARE(controller.headerPageNumberText(3998, 1, 3999, 1, 0), QStringLiteral("mmmcmxcix"));
    QCOMPARE(controller.headerPageNumberText(3999, 1, 4001, 1, 4000), QStringLiteral("1"));
    QCOMPARE(controller.headerPageNumberText(4000, 1, 4001, 1, 4000), QStringLiteral("2"));
    QVERIFY(controller.headerPageNumberText(0, 1, 4000, 1, 0).isEmpty());
    QVERIFY(!controller.addHeaderPageNumbers(1, 4000, 1, 0, 24, 16, 100));
    QVERIFY(!controller.hasDrafts());
    QVERIFY(controller.headerPageNumberText(1, 1, 5, 0, 3).isEmpty());
    QCOMPARE(controller.headerPageNumberText(2, 1, 5, 0, 3), QStringLiteral("1"));
    QCOMPARE(controller.headerPageNumberText(4, 1, 5, 0, 3), QStringLiteral("3"));
    QVERIFY(controller.headerPageNumberText(5, 1, 5, 0, 3).isEmpty());
    QVERIFY(controller.addHeaderPageNumbers(1, 5, 0, 3, 24, 16, 100));
    QVERIFY(controller.draftsForPage(0).isEmpty());
    QVERIFY(controller.draftsForPage(1).isEmpty());
    QCOMPARE(controller.draftsForPage(2).constFirst().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("1"));
    controller.discardDrafts();
    QVERIFY(controller.addHeaderPageNumbers(2, 5, 2, 0, 24, 16, 100));
    QCOMPARE(controller.draftsForPage(4).constFirst().toMap().value(QStringLiteral("text")).toString(), QStringLiteral("iv"));
}

void PDFEditControllerTest::rejectsHeaderPageNumbersAtomically()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("page-number-invalid.pdf"));
    QVERIFY(createNumberPages(path, 3, true));
    PDFDocument document;
    QVERIFY(document.open(QUrl::fromLocalFile(path)));
    PDFEditController controller;
    controller.setDocument(&document);
    QVERIFY(controller.addHeaderText(QStringLiteral("Keep"), QStringLiteral("1")));
    QVERIFY(controller.addHeaderText(QStringLiteral("Redo"), QStringLiteral("2")));
    controller.undo();
    const QVariantList before = controller.draftsForPage(0);
    QSignalSpy history(&controller, &PDFEditController::historyStateChanged);
    const QList<QList<int>> invalid{{0, 2, 0, 1}, {2, 1, 0, 2}, {1, 4, 0, 1},
        {1, 2, 0, 0}, {1, 2, -1, 1}, {2, 3, 1, 2}, {1, 2, 1, 1}, {1, 2, 2, 1},
        {1, 2, 0, 3}, {1, 2, 3, 0}, {std::numeric_limits<int>::min(), 2, 0, 1}};
    for (const auto &range : invalid)
        QVERIFY(!controller.addHeaderPageNumbers(range[0], range[1], range[2], range[3], 24, 16, 100));
    // Last page fails after the first two fit; no partial batch is allowed.
    QVERIFY(!controller.addHeaderPageNumbers(1, 3, 0, 1, 24, 16, 100));
    QVERIFY(!controller.addHeaderPageNumbers(1, 2, 0, 1,
        std::numeric_limits<qreal>::quiet_NaN(), 16, 100));
    QCOMPARE(controller.draftsForPage(0), before);
    QVERIFY(controller.draftsForPage(1).isEmpty());
    QVERIFY(controller.draftsForPage(2).isEmpty());
    QCOMPARE(history.size(), 0);
    QVERIFY(controller.canRedo());
    controller.redo();
    QCOMPARE(controller.draftsForPage(1).size(), 1);
}

void PDFEditControllerTest::addsHeaderPageNumbersThroughMainWindow()
{
    const QString path = m_temporaryDirectory.filePath(QStringLiteral("page-number-ui.pdf"));
    QVERIFY(createNumberPages(path, 12));
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QFileInfo(
        QFINDTESTDATA("../src/Main.qml")).canonicalFilePath()));
    std::unique_ptr<QObject> window(component.create());
    QVERIFY2(window, qPrintable(component.errorString()));
    QVERIFY(window->setProperty("visible", true));
    QVERIFY(window->setProperty("allowWindowClose", true));
    auto *quickWindow = qobject_cast<QQuickWindow *>(window.get());
    auto *model = window->findChild<PDFTabModel *>();
    auto *tabs = window->findChild<QObject *>(QStringLiteral("ribbonTabs"));
    QVERIFY(quickWindow && model && tabs);
    QVERIFY(tabs->setProperty("currentIndex", 7));
    QObject *button = nullptr;
    QTRY_VERIFY((button = window->findChild<QObject *>(QStringLiteral("headerPageNumbersButton"))));
    QVERIFY(!button->property("enabled").toBool());
    auto *session = model->open(QUrl::fromLocalFile(path));
    QVERIFY(session);
    QObject *placement = nullptr;
    QTRY_VERIFY((placement = window->findChild<QObject *>(QStringLiteral("headerPageNumberPlacement"))));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QVERIFY(placement->property("armed").toBool());
    QVERIFY(!window->findChild<QObject *>(QStringLiteral("headerPageNumberPanel")));
    QQuickItem *area = nullptr;
    QTRY_VERIFY((area = numberItem(quickWindow->contentItem(), QStringLiteral("headerPageNumberPlacementArea"))));
    QSignalSpy frame(quickWindow, &QQuickWindow::frameSwapped);
    quickWindow->requestUpdate();
    QTRY_VERIFY(!frame.isEmpty());
    QTest::mouseClick(quickWindow, Qt::LeftButton, Qt::NoModifier,
        area->mapToScene(QPointF(24, 16)).toPoint());
    QPointer<QObject> panel;
    QTRY_VERIFY((panel = window->findChild<QObject *>(QStringLiteral("headerPageNumberPanel"))));
    QVERIFY(placement->property("active").toBool());
    QVERIFY(!session->editController()->hasDrafts());
    auto *roman = panel->findChild<QObject *>(QStringLiteral("headerPageNumberRomanEnabled"));
    auto *decimal = panel->findChild<QQuickItem *>(QStringLiteral("headerPageNumberDecimalStart"));
    auto *lastPage = panel->findChild<QObject *>(QStringLiteral("headerPageNumberLastPage"));
    QVERIFY(roman && decimal && lastPage);
    // Accessibility changes checked/value directly, while keyboard edits use
    // the SpinBox's content item. Both must update the shared preview state.
    QVERIFY(roman->setProperty("checked", true));
    QVERIFY(placement->property("romanEnabled").toBool());
    QVERIFY(!placement->property("canSubmit").toBool());
    auto *decimalEditor = decimal->property("contentItem").value<QQuickItem *>();
    QVERIFY(decimalEditor);
    decimalEditor->forceActiveFocus();
    QTest::keyClick(quickWindow, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(quickWindow, Qt::Key_3);
    QTest::keyClick(quickWindow, Qt::Key_Return);
    QTRY_COMPARE(placement->property("decimalStartPage").toInt(), 3);
    QVERIFY(lastPage->setProperty("value", 5));
    QCOMPARE(placement->property("lastPage").toInt(), 5);
    QVERIFY(placement->property("canSubmit").toBool());
    QVERIFY(numberItem(quickWindow->contentItem(), QStringLiteral("headerPageNumberPreview")));
    auto *apply = panel->findChild<QObject *>(QStringLiteral("headerPageNumberApplyButton"));
    QVERIFY(apply);
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QTRY_VERIFY(panel.isNull());
    const QStringList expected{QStringLiteral("i"), QStringLiteral("ii"), QStringLiteral("1"),
        QStringLiteral("2"), QStringLiteral("3")};
    for (int page = 0; page < 12; ++page) {
        const auto drafts = session->editController()->draftsForPage(page);
        QCOMPARE(drafts.size(), page < 5 ? 1 : 0);
        if (page < 5)
            QCOMPARE(drafts.constFirst().toMap().value(QStringLiteral("text")).toString(), expected.at(page));
    }
    session->editController()->undo();
    QVERIFY(!session->editController()->hasDrafts());
    // Starting another Header tool and leaving the tab abandon temporary input.
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QVERIFY(placement->property("armed").toBool());
    QVERIFY(QMetaObject::invokeMethod(window.get(), "requestHeaderText"));
    QVERIFY(!placement->property("armed").toBool());
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QVERIFY(tabs->setProperty("currentIndex", 0));
    QVERIFY(!placement->property("armed").toBool());
    QVERIFY(!session->editController()->hasDrafts());
}
