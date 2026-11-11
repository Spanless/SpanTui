#pragma once

#include "terminal/api/shell_catalog.h"

#include <QAbstractListModel>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVector>

namespace span::application {

class TerminalSettings;

// Sole owner of shell profiles: what a local terminal session starts.
// Detected profiles come from the installed shells (terminal::detectShells);
// custom profiles are an executable the user picked. The user's choices —
// default profile, replaced arguments, custom profiles — persist through
// TerminalSettings::shellProfiles. One row per profile for Settings → Terminal.
class ShellProfileRegistry final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString defaultProfileId READ defaultProfileId NOTIFY defaultProfileChanged FINAL)
    // Startable profiles as [{profileId, name, icon}], for pickers and menus.
    Q_PROPERTY(QVariantList launchProfiles READ launchProfiles NOTIFY profilesChanged FINAL)

public:
    enum Role {
        ProfileIdRole = Qt::UserRole + 1,
        NameRole,
        ExecutableRole,
        ArgumentsRole,
        DefaultArgumentsRole,
        CustomRole,
        DefaultRole,
        AvailableRole,
        // File whose embedded icon represents the profile.
        IconRole,
    };

    explicit ShellProfileRegistry(TerminalSettings *settings, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The stored default while it names a startable profile, otherwise the
    // first detected shell. Empty when nothing can be started.
    QString defaultProfileId() const;
    QVariantList launchProfiles() const;
    // Command line for a session of `profileId`; an empty, removed or
    // unstartable id uses the default profile. Empty when none exists.
    QString commandLine(const QString &profileId) const;

    // Detects the installed shells again; Git Bash follows the published
    // Git executable.
    Q_INVOKABLE void refresh();
    // Display names follow the UI language.
    void retranslate();
    Q_INVOKABLE void setDefaultProfile(const QString &profileId);
    // Detected profiles store only arguments that differ from their default.
    Q_INVOKABLE void setArguments(const QString &profileId, const QString &arguments);
    Q_INVOKABLE QString addCustomProfile(const QString &executable);
    Q_INVOKABLE QString addCustomProfileUrl(const QUrl &url);
    Q_INVOKABLE void removeCustomProfile(const QString &profileId);

signals:
    void defaultProfileChanged();
    void profilesChanged();

private:
    struct Profile {
        QString id;
        QString name;
        QString executable;
        QString icon;
        QString arguments;
        QString defaultArguments;
        bool custom = false;
        bool available = true;
    };

    void rebuild();
    QVariantMap stored() const;
    void store(const QVariantMap &value);
    // Applies `edit` to the stored custom profile `profileId`.
    template <typename Edit>
    void editCustom(const QString &profileId, Edit edit);
    const Profile *find(const QString &profileId) const;

    TerminalSettings *m_settings = nullptr;
    QVector<terminal::ShellCandidate> m_detected;
    QVector<Profile> m_profiles;
    QString m_defaultProfileId;
};

} // namespace span::application
