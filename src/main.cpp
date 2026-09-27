#include "ApplicationFonts.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("TrePDF"));
    QCoreApplication::setApplicationVersion(QStringLiteral(TREPDF_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("TreOffice"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/trepdf/branding/TrePDF-logo.png")));

    ApplicationFonts::registerBundledFonts();
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlApplicationEngine engine;
    engine.setUiLanguage(QStringLiteral("vi"));
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("TrePDF"), QStringLiteral("Main"));

    return app.exec();
}
