#include "ApplicationLanguage.h"

#include <QDir>
#include <QFile>
#include <QPointer>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

class ApplicationLanguageTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void restoresSavedLanguage_data();
    void restoresSavedLanguage();
    void persistsChangesAndRejectsUnsupportedLanguages();
    void menuRetranslatesAndRemembersSelection_data();
    void menuRetranslatesAndRemembersSelection();
    void menuScrollsInSmallWindow();

private:
    QTemporaryDir m_directory;
    QUrl m_mainUrl;
};

void ApplicationLanguageTest::initTestCase()
{
    QVERIFY(m_directory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("TrePDF-language-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("isolated-settings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_directory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_directory.path());
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QVERIFY(QDir(m_directory.path()).mkdir(QStringLiteral("i18n")));
    for (const auto &language : {"vi", "en", "hr", "th", "fr", "de", "es", "pt_BR", "ja", "zh_CN", "ko", "id"}) {
        const QString name = QStringLiteral("qml_%1.qm").arg(QLatin1String(language));
        QVERIFY2(QFile::copy(QDir(QStringLiteral(TREPDF_LANGUAGE_QM_DIR)).filePath(name),
                            m_directory.filePath(QStringLiteral("i18n/") + name)),
                 qPrintable(name));
    }

    const QString components = QFINDTESTDATA("../src/Components");
    QVERIFY(!components.isEmpty());
    const QByteArray source = QStringLiteral(R"(
        import QtQuick
        import "%1"
        Window {
            width: 640
            height: 600
            visible: true
            LanguageMenuButton {
                x: 20
                y: 20
                theme: AppTheme {}
                documentLoaded: false
            }
            Text {
                objectName: "translatedSettingsLabel"
                //% "Settings"
                text: qsTrId("SettingsTab")
            }
        }
    )").arg(QUrl::fromLocalFile(components).toString()).toUtf8();
    QFile mainFile(m_directory.filePath(QStringLiteral("Main.qml")));
    QVERIFY(mainFile.open(QIODevice::WriteOnly));
    QCOMPARE(mainFile.write(source), source.size());
    mainFile.close();
    m_mainUrl = QUrl::fromLocalFile(mainFile.fileName());
}

void ApplicationLanguageTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(settings.fileName().startsWith(m_directory.path()));
}

void ApplicationLanguageTest::restoresSavedLanguage_data()
{
    QTest::addColumn<QString>("saved");
    QTest::addColumn<QString>("expected");
    QTest::newRow("first-launch") << QString() << QStringLiteral("vi");
    QTest::newRow("invalid") << QStringLiteral("unsupported") << QStringLiteral("vi");
    for (const auto &language : {"vi", "en", "hr", "th", "fr", "de", "es", "pt_BR", "ja", "zh_CN", "ko", "id"})
        QTest::newRow(language) << QString::fromLatin1(language) << QString::fromLatin1(language);
}

void ApplicationLanguageTest::restoresSavedLanguage()
{
    QFETCH(QString, saved);
    QFETCH(QString, expected);
    if (!saved.isNull())
        QSettings().setValue(QStringLiteral("Interface/language"), saved);
    QQmlApplicationEngine engine;
    ApplicationLanguage::initialize(engine);
    QCOMPARE(engine.uiLanguage(), expected);
}

void ApplicationLanguageTest::persistsChangesAndRejectsUnsupportedLanguages()
{
    {
        QQmlApplicationEngine engine;
        ApplicationLanguage::initialize(engine);
        engine.setUiLanguage(QStringLiteral("hr"));
        QCOMPARE(QSettings().value(QStringLiteral("Interface/language")).toString(),
                 QStringLiteral("hr"));
        engine.setUiLanguage(QStringLiteral("invalid"));
        QCOMPARE(engine.uiLanguage(), QStringLiteral("vi"));
        QCOMPARE(QSettings().value(QStringLiteral("Interface/language")).toString(),
                 QStringLiteral("vi"));
        engine.setUiLanguage(QStringLiteral("th"));
    }
    QQmlApplicationEngine reopened;
    ApplicationLanguage::initialize(reopened);
    QCOMPARE(reopened.uiLanguage(), QStringLiteral("th"));
}

void ApplicationLanguageTest::menuRetranslatesAndRemembersSelection_data()
{
    QTest::addColumn<QString>("finalLanguage");
    QTest::addColumn<QString>("finalMenuText");
    QTest::newRow("French") << QStringLiteral("fr") << QStringLiteral("Langue");
    QTest::newRow("German") << QStringLiteral("de") << QStringLiteral("Sprache");
    QTest::newRow("Spanish") << QStringLiteral("es") << QStringLiteral("Idioma");
    QTest::newRow("PortugueseBrazil") << QStringLiteral("pt_BR") << QStringLiteral("Idioma");
    QTest::newRow("Japanese") << QStringLiteral("ja") << QStringLiteral("言語");
    QTest::newRow("ChineseSimplified") << QStringLiteral("zh_CN") << QStringLiteral("语言");
    QTest::newRow("Korean") << QStringLiteral("ko") << QStringLiteral("언어");
    QTest::newRow("Indonesian") << QStringLiteral("id") << QStringLiteral("Bahasa");
}

void ApplicationLanguageTest::menuRetranslatesAndRemembersSelection()
{
    QFETCH(QString, finalLanguage);
    QFETCH(QString, finalMenuText);
    const QStringList itemNames{QStringLiteral("languageViItem"), QStringLiteral("languageEnItem"),
                                QStringLiteral("languageHrItem"), QStringLiteral("languageThItem"),
                                QStringLiteral("languageFrItem"), QStringLiteral("languageDeItem"),
                                QStringLiteral("languageEsItem"), QStringLiteral("languagePtBrItem"),
                                QStringLiteral("languageJaItem"), QStringLiteral("languageZhCnItem"),
                                QStringLiteral("languageKoItem"), QStringLiteral("languageIdItem")};
    const QStringList languages{QStringLiteral("vi"), QStringLiteral("en"),
                               QStringLiteral("hr"), QStringLiteral("th"),
                               QStringLiteral("fr"), QStringLiteral("de"),
                               QStringLiteral("es"), QStringLiteral("pt_BR"),
                               QStringLiteral("ja"), QStringLiteral("zh_CN"),
                               QStringLiteral("ko"), QStringLiteral("id")};
    const QStringList autonyms{QStringLiteral("Tiếng Việt"), QStringLiteral("English"),
                              QStringLiteral("Hrvatski"), QStringLiteral("ไทย"),
                              QStringLiteral("Français"), QStringLiteral("Deutsch"),
                              QStringLiteral("Español"), QStringLiteral("Português (Brasil)"),
                              QStringLiteral("日本語"), QStringLiteral("简体中文"),
                              QStringLiteral("한국어"), QStringLiteral("Bahasa Indonesia")};
    const int finalIndex = static_cast<int>(languages.indexOf(finalLanguage));
    QVERIFY(finalIndex >= 0);
    {
        QQmlApplicationEngine engine;
        ApplicationLanguage::initialize(engine);
        engine.load(m_mainUrl);
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *button = window->findChild<QQuickItem *>(QStringLiteral("settingsLanguageButton"));
        QVERIFY(button);
        QVERIFY(button->isEnabled()); // Works before a PDF is opened.
        auto *settingsLabel = window->findChild<QObject *>(QStringLiteral("translatedSettingsLabel"));
        QVERIFY(settingsLabel);
        QVERIFY(!button->findChild<QObject *>(QStringLiteral("settingsLanguageMenu")));

        for (int languageIndex : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, finalIndex, finalIndex}) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
            QTRY_VERIFY(button->findChild<QObject *>(QStringLiteral("settingsLanguageMenu")));
            QPointer<QObject> menu = button->findChild<QObject *>(QStringLiteral("settingsLanguageMenu"));
            QTRY_VERIFY(menu->property("opened").toBool());
            for (int index = 0; index < languages.size(); ++index) {
                auto *item = menu->findChild<QQuickItem *>(itemNames.at(index));
                QVERIFY(item);
                QCOMPARE(item->property("text").toString(), autonyms.at(index));
                QCOMPARE(item->property("checked").toBool(), engine.uiLanguage() == languages.at(index));
            }
            const QString screenshotDirectory = qEnvironmentVariable("TREPDF_LANGUAGE_SCREENSHOT_DIR");
            if (!screenshotDirectory.isEmpty()) {
                QVERIFY(QDir().mkpath(screenshotDirectory));
                QTRY_VERIFY(!window->grabWindow().isNull());
                QVERIFY(window->grabWindow().save(QDir(screenshotDirectory).filePath(
                    QStringLiteral("language-menu-%1.png").arg(engine.uiLanguage()))));
            }
            auto *item = menu->findChild<QQuickItem *>(itemNames.at(languageIndex));
            QVERIFY(item);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
            QTRY_COMPARE(engine.uiLanguage(), languages.at(languageIndex));
            QTRY_VERIFY(menu.isNull()); // The lazy menu is released after closing.
            QCOMPARE(QSettings().value(QStringLiteral("Interface/language")).toString(),
                     languages.at(languageIndex));

            QTranslator expected;
            QVERIFY(expected.load(QStringLiteral("qml_") + languages.at(languageIndex),
                                  m_directory.filePath(QStringLiteral("i18n"))));
            const QString title = expected.translate(nullptr, "UiLanguageMenu");
            QVERIFY(!title.isEmpty());
            QTRY_COMPARE(button->property("text").toString(), title);
            QTRY_COMPARE(settingsLabel->property("text").toString(),
                         expected.translate(nullptr, "SettingsTab"));
        }
        button->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(button->findChild<QObject *>(QStringLiteral("settingsLanguageMenu")));
        QPointer<QObject> menu = button->findChild<QObject *>(QStringLiteral("settingsLanguageMenu"));
        QTRY_VERIFY(menu->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(menu.isNull());
        QCOMPARE(engine.uiLanguage(), finalLanguage);
    }
    QQmlApplicationEngine reopened;
    ApplicationLanguage::initialize(reopened);
    reopened.load(m_mainUrl);
    QCOMPARE(reopened.rootObjects().size(), 1);
    QCOMPARE(reopened.uiLanguage(), finalLanguage);
    auto *button = reopened.rootObjects().constFirst()->findChild<QObject *>(
        QStringLiteral("settingsLanguageButton"));
    QVERIFY(button);
    QCOMPARE(button->property("text").toString(), finalMenuText);
}

void ApplicationLanguageTest::menuScrollsInSmallWindow()
{
    QQmlApplicationEngine engine;
    ApplicationLanguage::initialize(engine);
    engine.load(m_mainUrl);
    QCOMPARE(engine.rootObjects().size(), 1);
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    QVERIFY(window);
    window->resize(360, 280);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *button = window->findChild<QQuickItem *>(QStringLiteral("settingsLanguageButton"));
    QVERIFY(button);
    button->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(button->findChild<QObject *>(QStringLiteral("settingsLanguageMenu")));
    QPointer<QObject> menu = button->findChild<QObject *>(QStringLiteral("settingsLanguageMenu"));
    QTRY_VERIFY(menu->property("opened").toBool());
    QVERIFY(menu->property("height").toReal() <= window->height() - 16);
    QCOMPARE(menu->property("count").toInt(), 12);
    for (int index = 0; index < 11; ++index)
        QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(menu->property("currentIndex").toInt(), 11);
    auto *lastItem = menu->findChild<QQuickItem *>(QStringLiteral("languageIdItem"));
    QVERIFY(lastItem);
    auto *content = menu->property("contentItem").value<QQuickItem *>();
    QVERIFY(content);
    QTRY_VERIFY(content->contains(lastItem->mapToItem(content,
        QPointF(lastItem->width() / 2, lastItem->height() / 2))));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(engine.uiLanguage(), QStringLiteral("id"));
    QTRY_VERIFY(menu.isNull());

    button->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(button->findChild<QObject *>(QStringLiteral("settingsLanguageMenu")));
    menu = button->findChild<QObject *>(QStringLiteral("settingsLanguageMenu"));
    QTRY_VERIFY(menu->property("opened").toBool());
    QTRY_COMPARE(menu->property("currentIndex").toInt(), 11);
    lastItem = menu->findChild<QQuickItem *>(QStringLiteral("languageIdItem"));
    content = menu->property("contentItem").value<QQuickItem *>();
    QVERIFY(lastItem && content);
    QTRY_VERIFY(content->contains(lastItem->mapToItem(content,
        QPointF(lastItem->width() / 2, lastItem->height() / 2))));
    const QString screenshotDirectory = qEnvironmentVariable("TREPDF_LANGUAGE_SCREENSHOT_DIR");
    if (!screenshotDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(screenshotDirectory));
        QTRY_VERIFY(!window->grabWindow().isNull());
        QVERIFY(window->grabWindow().save(QDir(screenshotDirectory).filePath(
            QStringLiteral("language-menu-small-window.png"))));
    }
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(menu.isNull());
}

QTEST_MAIN(ApplicationLanguageTest)
#include "tst_ApplicationLanguage.moc"
