#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("TrePDF Web"));
    QCoreApplication::setApplicationVersion(QStringLiteral(TREPDF_VERSION));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QTranslator translator;
    if (translator.load(QStringLiteral(":/trepdf/web/i18n/qml_vi.qm")))
        app.installTranslator(&translator);
    QQmlApplicationEngine engine;
    engine.setExtraFileSelectors({QStringLiteral("web")});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("TrePDFWeb", "Viewer");
    return app.exec();
}
