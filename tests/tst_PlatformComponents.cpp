#include "PDFEditControllerTest.h"
#include "PDFEditControllerTestSupport.h"

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

using namespace PDFEditControllerTestSupport;

namespace {
void selectorRows()
{
    QTest::addColumn<bool>("ios");
    QTest::newRow("default") << false;
    QTest::newRow("ios") << true;
    QTest::newRow("default-after-ios") << false;
}

QUrl platformUrl(const QString &name)
{
    const QString directory = QFINDTESTDATA("../src/Components/ios");
    return QUrl::fromLocalFile(QDir(directory).filePath(name + QStringLiteral(".qml")));
}

void configureSelector(QQmlApplicationEngine &engine, bool ios)
{
    engine.setExtraFileSelectors(ios ? QStringList{QStringLiteral("ios")} : QStringList{});
}
}

void PDFEditControllerTest::selectsPlatformInputPerEngine_data() { selectorRows(); }

void PDFEditControllerTest::selectsPlatformInputPerEngine()
{
    QFETCH(bool, ios);
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlApplicationEngine engine;
    configureSelector(engine, ios);
    QQmlComponent themeComponent(&engine);
    themeComponent.setData("import TrePDF\nAppTheme {}", QUrl());
    std::unique_ptr<QObject> theme(themeComponent.create());
    QVERIFY2(theme, qPrintable(themeComponent.errorString()));
    PDFTextSelectionController selection;
    engine.setInitialProperties({
        {QStringLiteral("theme"), QVariant::fromValue(theme.get())},
        {QStringLiteral("documentLoaded"), true},
        {QStringLiteral("allowsTextCopy"), true},
        {QStringLiteral("hasDrafts"), false},
        {QStringLiteral("saving"), false},
        {QStringLiteral("textEditing"), false},
        {QStringLiteral("formTextFocused"), false},
        {QStringLiteral("viewTabActive"), true},
        {QStringLiteral("zoom"), 1.0},
        {QStringLiteral("selectionController"), QVariant::fromValue(&selection)}
    });
    engine.load(platformUrl(QStringLiteral("DocumentInput")));
    QCOMPARE(engine.rootObjects().size(), 1);
    auto *root = qobject_cast<QQuickItem *>(engine.rootObjects().constFirst());
    QVERIFY(root);
    QVERIFY(root->property("theme").value<QObject *>());
    QCOMPARE(root->property("textSelectionEnabled").toBool(), !ios);
    auto *toggle = root->findChild<QQuickItem *>(QStringLiteral("mobileTextSelectionButton"));
    QCOMPARE(toggle != nullptr, ios);
    QVERIFY(root->metaObject()->indexOfSignal("zoomRequested(double)") >= 0);
    QVERIFY(root->metaObject()->indexOfSignal("activated()") >= 0);
    if (!ios) {
        delete root;
        return;
    }
    QVERIFY(toggle->isEnabled());
    QVERIFY(toggle->setProperty("checked", true));
    QCOMPARE(root->property("textSelectionEnabled").toBool(), true);
    QVERIFY(root->setProperty("allowsTextCopy", false));
    QVERIFY(!toggle->isEnabled());
    QVERIFY(root->setProperty("viewTabActive", false));
    QCOMPARE(root->property("textSelectionEnabled").toBool(), false);
    QVERIFY(!toggle->isVisible());
    delete root;
}

void PDFEditControllerTest::selectsPlatformViewportPerEngine_data() { selectorRows(); }

void PDFEditControllerTest::selectsPlatformViewportPerEngine()
{
    QFETCH(bool, ios);
    QQmlApplicationEngine engine;
    configureSelector(engine, ios);
    engine.setInitialProperties({
        {QStringLiteral("width"), 120.0}, {QStringLiteral("height"), 40.0},
        {QStringLiteral("contentImplicitWidth"), 300.0},
        {QStringLiteral("contentImplicitHeight"), 28.0}
    });
    engine.load(platformUrl(QStringLiteral("HorizontalControlViewport")));
    QCOMPARE(engine.rootObjects().size(), 1);
    auto *root = qobject_cast<QQuickItem *>(engine.rootObjects().constFirst());
    QVERIFY(root);
    QCOMPARE(root->implicitWidth(), 300.0);
    QCOMPARE(root->implicitHeight(), 28.0);
    QCOMPARE(root->property("contentWidth").toReal(), ios ? 300.0 : 120.0);
    QCOMPARE(root->clip(), ios);
    if (ios) {
        QVERIFY(root->property("interactive").toBool());
        root->setWidth(400);
        QCOMPARE(root->property("contentWidth").toReal(), 400.0);
        QVERIFY(!root->property("interactive").toBool());
    }
}

void PDFEditControllerTest::selectsPlatformWindowMetrics_data() { selectorRows(); }

void PDFEditControllerTest::selectsPlatformWindowMetrics()
{
    QFETCH(bool, ios);
    QQmlApplicationEngine engine;
    configureSelector(engine, ios);
    QQmlComponent windowComponent(&engine);
    windowComponent.setData("import QtQuick\nWindow { width: 900; height: 700 }", QUrl());
    std::unique_ptr<QObject> windowObject(windowComponent.create());
    auto *window = qobject_cast<QQuickWindow *>(windowObject.get());
    QVERIFY2(window, qPrintable(windowComponent.errorString()));
    window->contentItem()->setSize(QSizeF(900, 700));
    engine.setInitialProperties({{QStringLiteral("window"), QVariant::fromValue(windowObject.get())}});
    engine.load(platformUrl(QStringLiteral("WindowMetrics")));
    QCOMPARE(engine.rootObjects().size(), 1);
    QObject *root = engine.rootObjects().constFirst();
    QVERIFY(root->property("window").value<QObject *>());
    QCOMPARE(root->property("minimumWidth").toInt(), ios ? 320 : 760);
    QCOMPARE(root->property("minimumHeight").toInt(), ios ? 320 : 520);
    QCOMPARE(root->property("keyboardInset").toReal(), 0.0);
    for (const char *property : {"topInset", "bottomInset", "leftInset", "rightInset"}) {
        QVERIFY(root->property(property).isValid());
        QCOMPARE(root->property(property).toReal(), 0.0);
    }

    QQmlApplicationEngine dialogEngine;
    configureSelector(dialogEngine, ios);
    dialogEngine.setInitialProperties({
        {QStringLiteral("overlay"), QVariant::fromValue(window->contentItem())}
    });
    dialogEngine.load(platformUrl(QStringLiteral("DialogMetrics")));
    QCOMPARE(dialogEngine.rootObjects().size(), 1);
    QObject *dialog = dialogEngine.rootObjects().constFirst();
    QCOMPARE(dialog->property("active").toBool(), ios);
    QCOMPARE(dialog->property("availableWidth").toReal(), 900.0);
    QCOMPARE(dialog->property("availableHeight").toReal(), 700.0);
    delete dialog;
    delete root;
}

void PDFEditControllerTest::guardsPlatformFileDialogWrites()
{
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlApplicationEngine engine;
    configureSelector(engine, true);
    engine.load(platformUrl(QStringLiteral("PlatformFileDialog")));
    QCOMPARE(engine.rootObjects().size(), 1);
    QObject *dialog = engine.rootObjects().constFirst();
    QSignalSpy rejected(dialog, SIGNAL(rejected()));
    QVERIFY(rejected.isValid());
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QCOMPARE(rejected.count(), 1); // Desktop PlatformService cannot present native Files.
    QVERIFY(!dialog->property("visible").toBool());

    PDFDocumentSession session;
    PDFDocumentSession otherSession;
    const QUrl target = QUrl::fromLocalFile(m_temporaryDirectory.filePath("pending-export.pdf"));
    const QUrl differentTarget = QUrl::fromLocalFile(m_temporaryDirectory.filePath("other-export.pdf"));
    const auto track = [&]() {
        return QMetaObject::invokeMethod(dialog, "trackWrite",
            Q_ARG(PDFDocumentSession *, &session), Q_ARG(QUrl, target));
    };
    const auto finish = [&](PDFDocumentSession *owner, bool succeeded, const QUrl &url) {
        return QMetaObject::invokeMethod(dialog, "finishWrite",
            Q_ARG(PDFDocumentSession *, owner), Q_ARG(bool, succeeded), Q_ARG(QUrl, url));
    };
    QVERIFY(track());
    QVERIFY(dialog->property("writePending").toBool());
    QVERIFY(finish(&otherSession, true, target));
    QVERIFY(dialog->property("writePending").toBool());
    QVERIFY(finish(&session, false, target));
    QVERIFY(!dialog->property("writePending").toBool());
    QVERIFY(track());
    QVERIFY(finish(&session, true, differentTarget));
    QVERIFY(!dialog->property("writePending").toBool());
    QVERIFY(dialog->property("pendingTarget").toUrl().isEmpty());
    QVERIFY(track());
    QVERIFY(finish(&session, true, target));
    QVERIFY(!dialog->property("writePending").toBool());
}

void PDFEditControllerTest::createsSharedMainWithPlatformSelectors_data() { selectorRows(); }

void PDFEditControllerTest::createsSharedMainWithPlatformSelectors()
{
    QFETCH(bool, ios);
    QVERIFY(registerQmlTestTypes(QFINDTESTDATA("../src/Components/AppTheme.qml")));
    QQmlApplicationEngine engine;
    configureSelector(engine, ios);
    QStringList errors;
    connect(&engine, &QQmlEngine::warnings, &engine, [&errors](const QList<QQmlError> &warnings) {
        for (const auto &warning : warnings) {
            const QString text = warning.toString();
            if (text.contains(QStringLiteral("TypeError"))
                || text.contains(QStringLiteral("ReferenceError")))
                errors.append(text);
        }
    });
    engine.load(QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
    QCOMPARE(engine.rootObjects().size(), 1);
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    QVERIFY(window);
    QVERIFY(window->setProperty("allowWindowClose", true));
    QCOMPARE(window->minimumWidth(), ios ? 320 : 760);
    QCOMPARE(window->minimumHeight(), ios ? 320 : 520);
    QVERIFY(window->findChild<QObject *>(QStringLiteral("readerControlBar")));
    QCoreApplication::processEvents();
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
}
