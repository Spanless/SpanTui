#include "application/translator.h"

#include "application/appearance_settings.h"
#include "application/application_constants.h"

#include <QCoreApplication>
#include <QQmlEngine>

using span::application::AppearanceSettings;
using span::application::kLanguageChinese;
using span::application::kLanguageEnglish;

Translator::Translator(AppearanceSettings *appearance, QQmlEngine *engine, QObject *parent)
    : QObject(parent),
      m_appearance(appearance),
      m_engine(engine)
{
    installLanguage(m_appearance ? m_appearance->language() : AppearanceSettings::defaultLanguage(), false);
}

bool Translator::setLanguage(const QString &language) {
    return installLanguage(language, true);
}

bool Translator::installLanguage(const QString &language, bool userRequested) {
    const QString normalized = language == kLanguageChinese ? QString(kLanguageChinese) : QString(kLanguageEnglish);

    QCoreApplication::removeTranslator(&m_translator);

    bool installed = true;
    if (normalized != kLanguageEnglish) {
        installed = m_translator.load(QStringLiteral(":/i18n/SpanTui_%1.qm").arg(normalized));
        if (installed) {
            QCoreApplication::installTranslator(&m_translator);
        }
    }

    const QString appliedLanguage = installed ? normalized : QString(kLanguageEnglish);
    if (m_appearance) {
        m_appearance->setLanguage(appliedLanguage);
    }

    const bool changed = m_currentLanguage != appliedLanguage;
    m_currentLanguage = appliedLanguage;
    if (m_engine) {
        m_engine->retranslate();
    }
    if (changed) {
        emit languageChanged();
    }

    if (!installed && userRequested) {
        emit errorOccurred(tr("Translation file is missing. Falling back to English."));
    }
    return installed;
}
