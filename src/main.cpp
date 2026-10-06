#include "ApplicationFonts.h"
#include "PDFFileOpenHandler.h"
#include "PDFMemoryTelemetry.h"
#include "platform/PlatformService.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
#if TREPDF_ENABLE_MEMORY_TELEMETRY
    PDFMemoryTelemetry::startLogging(&app);
#endif
    auto *fileOpenHandler = new PDFFileOpenHandler(&app);
    app.installEventFilter(fileOpenHandler);
    auto *platformService = new PlatformService(fileOpenHandler, &app);
    platformService->enqueueArguments(QCoreApplication::arguments().mid(1));

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
