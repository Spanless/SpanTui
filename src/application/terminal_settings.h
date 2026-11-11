#pragma once

#include "terminal/core/terminal_color.h"
#include "terminal/terminal_constants.h"

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <optional>

namespace span::application {

class AppearanceSettings;
class SettingsStore;

// Terminal preferences (QML singleton `TerminalSettings`). Color schemes:
// one active scheme per surface (dark/light) plus the user-managed
// collection; each scheme owns the terminal surfaces and the UI paper.
// surfaceColors resolves the slot for the current effective theme.
class TerminalSettings final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString colorSchemeDark READ colorSchemeDark WRITE setColorSchemeDark NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString colorSchemeLight READ colorSchemeLight WRITE setColorSchemeLight NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QVariantMap surfaceColors READ surfaceColors NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString cursorStyle READ cursorStyle WRITE setCursorStyle NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(int refreshRate READ refreshRate WRITE setRefreshRate NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(bool terminateOnTileClose READ terminateOnTileClose WRITE setTerminateOnTileClose NOTIFY
                   closePreferenceChanged FINAL)
    // Shell profile choices as stored: {"default": id, "arguments": {id: text},
    // "custom": [{id, name, executable, arguments}]}. ShellProfileRegistry
    // owns their meaning and validation.
    Q_PROPERTY(QVariantMap shellProfiles READ shellProfiles NOTIFY shellProfilesChanged FINAL)

public:
    TerminalSettings(SettingsStore *store, const AppearanceSettings *appearance, QObject *parent = nullptr);

    // Reads every key from the store; call once after it opened.
    void load();

    QString colorSchemeDark() const { return m_colorSchemeDark; }
    QString colorSchemeLight() const { return m_colorSchemeLight; }
    QVariantMap surfaceColors() const;
    // Fully resolved scheme for the currently active surface.
    span::terminal::TerminalColorScheme activeColorScheme() const;
    // Built-in (builtIn = true) plus user schemes, in display order.
    Q_INVOKABLE QVariantList colorSchemes() const;
    Q_INVOKABLE QVariantMap colorScheme(const QString &name) const;
    Q_INVOKABLE bool saveColorScheme(const QString &name, const QVariantMap &scheme);
    Q_INVOKABLE bool removeColorScheme(const QString &name);
    QString cursorStyle() const { return m_cursorStyle; }
    int refreshRate() const { return m_refreshRate; }
    bool terminateOnTileClose() const { return m_terminateOnTileClose; }
    QVariantMap shellProfiles() const { return m_shellProfiles; }

    void setColorSchemeDark(const QString &value);
    void setColorSchemeLight(const QString &value);
    void setCursorStyle(const QString &value);
    void setRefreshRate(int value);
    void setTerminateOnTileClose(bool value);
    void setShellProfiles(const QVariantMap &value);

signals:
    // Colors (including an effective theme flip), cursor style, refresh rate.
    void appearanceChanged();
    void colorSchemesChanged();
    void closePreferenceChanged();
    void shellProfilesChanged();
    void errorOccurred(const QString &message);

private:
    std::optional<span::terminal::TerminalColorScheme> findColorScheme(const QString &name) const;
    void assignColorScheme(QString &slot, const QString &value, const QString &key);
    void storeUserColorSchemes();
    void store(const QString &key, const QString &value);

    QPointer<SettingsStore> m_store;
    const AppearanceSettings *m_appearance = nullptr;
    QString m_colorSchemeDark = span::terminal::kDefaultDarkColorScheme;
    QString m_colorSchemeLight = span::terminal::kDefaultLightColorScheme;
    QVector<span::terminal::TerminalColorScheme> m_userColorSchemes;
    QString m_cursorStyle = span::terminal::kCursorStyleBlock;
    int m_refreshRate = span::terminal::kDefaultFrameRate;
    bool m_terminateOnTileClose = true;
    QVariantMap m_shellProfiles;
};

} // namespace span::application
