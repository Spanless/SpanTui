#pragma once

#include "application/application_constants.h"

#include <QObject>
#include <QTranslator>

class QQmlEngine;

namespace span::application {
class AppearanceSettings;
}

class Translator final : public QObject {
    Q_OBJECT

public:
    Translator(span::application::AppearanceSettings *appearance, QQmlEngine *engine, QObject *parent = nullptr);

    Q_INVOKABLE bool setLanguage(const QString &language);

signals:
    void languageChanged();
    void errorOccurred(const QString &message);

private:
    bool installLanguage(const QString &language, bool userRequested);

    span::application::AppearanceSettings *m_appearance = nullptr;
    QQmlEngine *m_engine = nullptr;
    QTranslator m_translator;
    QString m_currentLanguage = span::application::kLanguageEnglish;
};
