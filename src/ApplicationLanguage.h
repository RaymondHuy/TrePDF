#pragma once

#include <QQmlApplicationEngine>
#include <QSettings>
#include <QStringList>

namespace ApplicationLanguage {

inline QString supportedLanguage(const QString &language)
{
    static const QStringList languages{QStringLiteral("vi"), QStringLiteral("en"),
                                       QStringLiteral("hr"), QStringLiteral("th"),
                                       QStringLiteral("fr"), QStringLiteral("de"),
                                       QStringLiteral("es"), QStringLiteral("pt_BR"),
                                       QStringLiteral("ja"), QStringLiteral("zh_CN"),
                                       QStringLiteral("ko"), QStringLiteral("id")};
    return languages.contains(language) ? language : QStringLiteral("vi");
}

inline void initialize(QQmlApplicationEngine &engine)
{
    const QString key = QStringLiteral("Interface/language");
    engine.setUiLanguage(supportedLanguage(QSettings().value(key).toString()));
    QObject::connect(&engine, &QJSEngine::uiLanguageChanged, &engine, [&engine, key] {
        const QString language = supportedLanguage(engine.uiLanguage());
        if (engine.uiLanguage() != language) {
            engine.setUiLanguage(language);
            return;
        }
        QSettings settings;
        settings.setValue(key, language);
        settings.sync();
    });
}

} // namespace ApplicationLanguage
