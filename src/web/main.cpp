#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("TrePDF Web"));
    QCoreApplication::setApplicationVersion(QStringLiteral(TREPDF_VERSION));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("TrePDFWeb", "Viewer");
    return app.exec();
}
