#include "ApplicationFonts.h"
#include "PDFFileOpenHandler.h"
#include "PDFMemoryTelemetry.h"
#include "platform/PlatformService.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>

namespace {
bool hasNativeBundleIcon()
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return false;

    const auto bundle = CFBundleGetMainBundle();
    if (!bundle)
        return false;
    const auto packageType = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CFBundlePackageType"));
    const auto iconName = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CFBundleIconFile"));
    if (!packageType || !CFEqual(packageType, CFSTR("APPL"))
        || !iconName || CFGetTypeID(iconName) != CFStringGetTypeID()
        || CFStringGetLength(static_cast<CFStringRef>(iconName)) == 0) {
        return false;
    }

    const auto iconUrl = CFBundleCopyResourceURL(bundle, static_cast<CFStringRef>(iconName),
                                                nullptr, nullptr);
    if (!iconUrl)
        return false;
    CFRelease(iconUrl);
    return true;
}
} // namespace
#endif

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
    // Cocoa uses the bundle's ICNS, including when its executable is launched
    // directly by CLion/terminal. Avoid a second decoded application icon.
#ifdef Q_OS_MACOS
    if (!hasNativeBundleIcon())
#endif
        QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/trepdf/branding/TrePDF-icon-512.png")));

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
