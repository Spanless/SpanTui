#include "application/shell_profile_registry.h"

#include "application/terminal_settings.h"
#include "core/external_tools.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace span::application {
namespace {

const QString kDefaultKey = QStringLiteral("default");
const QString kArgumentsKey = QStringLiteral("arguments");
const QString kCustomKey = QStringLiteral("custom");
const QString kCustomPrefix = QStringLiteral("custom:");

bool startable(const QString &executable)
{
    return QDir::isAbsolutePath(executable) && QFileInfo(executable).isFile();
}

} // namespace

ShellProfileRegistry::ShellProfileRegistry(TerminalSettings *settings, QObject *parent)
    : QAbstractListModel(parent),
      m_settings(settings)
{
    if (m_settings) {
        connect(m_settings, &TerminalSettings::shellProfilesChanged, this, &ShellProfileRegistry::rebuild);
    }
    refresh();
}

int ShellProfileRegistry::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_profiles.size());
}

QVariant ShellProfileRegistry::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_profiles.size()) {
        return {};
    }
    const Profile &profile = m_profiles.at(index.row());
    switch (role) {
    case ProfileIdRole:
        return profile.id;
    case NameRole:
        return profile.name;
    case ExecutableRole:
        return profile.executable;
    case ArgumentsRole:
        return profile.arguments;
    case DefaultArgumentsRole:
        return profile.defaultArguments;
    case CustomRole:
        return profile.custom;
    case DefaultRole:
        return profile.id == m_defaultProfileId;
    case AvailableRole:
        return profile.available;
    case IconRole:
        return profile.icon;
    default:
        return {};
    }
}

QHash<int, QByteArray> ShellProfileRegistry::roleNames() const
{
    return {
        {ProfileIdRole, "profileId"},
        {NameRole, "name"},
        {ExecutableRole, "executable"},
        {ArgumentsRole, "arguments"},
        {DefaultArgumentsRole, "defaultArguments"},
        {CustomRole, "custom"},
        {DefaultRole, "isDefault"},
        {AvailableRole, "available"},
        {IconRole, "icon"},
    };
}

template <typename Edit>
void ShellProfileRegistry::editCustom(const QString &profileId, Edit edit)
{
    QVariantMap value = stored();
    QVariantList custom = value.value(kCustomKey).toList();
    for (QVariant &item : custom) {
        QVariantMap entry = item.toMap();
        if (entry.value(QStringLiteral("id")).toString() == profileId) {
            edit(entry);
            item = entry;
            value.insert(kCustomKey, custom);
            store(value);
            return;
        }
    }
}

QString ShellProfileRegistry::defaultProfileId() const
{
    return m_defaultProfileId;
}

QVariantList ShellProfileRegistry::launchProfiles() const
{
    QVariantList result;
    for (const Profile &profile : m_profiles) {
        if (profile.available) {
            result.push_back(QVariantMap{{QStringLiteral("profileId"), profile.id},
                                         {QStringLiteral("name"), profile.name},
                                         {QStringLiteral("icon"), profile.icon}});
        }
    }
    return result;
}

QString ShellProfileRegistry::commandLine(const QString &profileId) const
{
    const Profile *profile = find(profileId);
    if (!profile || !profile->available) {
        profile = find(m_defaultProfileId);
    }
    return profile ? terminal::shellCommandLine(profile->executable, profile->arguments)
                   : QString();
}

void ShellProfileRegistry::refresh()
{
    m_detected = terminal::detectShells(core::toolExecutable(core::ExternalTool::Git));
    rebuild();
}

void ShellProfileRegistry::retranslate()
{
    rebuild();
}

void ShellProfileRegistry::setDefaultProfile(const QString &profileId)
{
    const Profile *profile = find(profileId);
    if (!profile || !profile->available) {
        return;
    }
    QVariantMap value = stored();
    value.insert(kDefaultKey, profileId);
    store(value);
}

void ShellProfileRegistry::setArguments(const QString &profileId, const QString &arguments)
{
    const Profile *profile = find(profileId);
    if (!profile) {
        return;
    }
    const QString trimmed = arguments.trimmed();
    if (profile->custom) {
        editCustom(profileId, [&trimmed](QVariantMap &entry) {
            entry.insert(kArgumentsKey, trimmed);
        });
        return;
    }
    QVariantMap value = stored();
    QVariantMap replaced = value.value(kArgumentsKey).toMap();
    if (trimmed == profile->defaultArguments) {
        replaced.remove(profileId);
    } else {
        replaced.insert(profileId, trimmed);
    }
    if (replaced.isEmpty()) {
        value.remove(kArgumentsKey);
    } else {
        value.insert(kArgumentsKey, replaced);
    }
    store(value);
}

QString ShellProfileRegistry::addCustomProfile(const QString &executable)
{
    const QString path = QDir::toNativeSeparators(executable.trimmed());
    if (path.isEmpty()) {
        return {};
    }
    const QString id = kCustomPrefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap value = stored();
    QVariantList custom = value.value(kCustomKey).toList();
    custom.push_back(QVariantMap{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), QFileInfo(path).completeBaseName()},
        {QStringLiteral("executable"), path},
        {kArgumentsKey, QString()},
    });
    value.insert(kCustomKey, custom);
    store(value);
    return id;
}

QString ShellProfileRegistry::addCustomProfileUrl(const QUrl &url)
{
    return url.isLocalFile() ? addCustomProfile(url.toLocalFile()) : QString();
}

void ShellProfileRegistry::removeCustomProfile(const QString &profileId)
{
    QVariantMap value = stored();
    QVariantList custom = value.value(kCustomKey).toList();
    const auto removed = custom.removeIf([&profileId](const QVariant &entry) {
        return entry.toMap().value(QStringLiteral("id")).toString() == profileId;
    });
    if (removed == 0) {
        return;
    }
    value.insert(kCustomKey, custom);
    if (value.value(kDefaultKey).toString() == profileId) {
        value.remove(kDefaultKey);
    }
    store(value);
}

void ShellProfileRegistry::rebuild()
{
    const QVariantMap value = stored();
    const QVariantMap replaced = value.value(kArgumentsKey).toMap();

    QVector<Profile> profiles;
    for (const terminal::ShellCandidate &shell : std::as_const(m_detected)) {
        Profile profile;
        profile.id = shell.id;
        // Product names stay as published; only the generic one is localized.
        profile.name = shell.id == QStringLiteral("cmd") ? tr("Command Prompt") : shell.name;
        profile.executable = shell.executable;
        profile.icon = shell.icon;
        profile.defaultArguments = shell.arguments;
        profile.arguments = replaced.contains(shell.id)
            ? replaced.value(shell.id).toString()
            : shell.arguments;
        profiles.push_back(profile);
    }
    for (const QVariant &item : value.value(kCustomKey).toList()) {
        const QVariantMap entry = item.toMap();
        Profile profile;
        profile.id = entry.value(QStringLiteral("id")).toString();
        if (!profile.id.startsWith(kCustomPrefix)) {
            continue;
        }
        profile.custom = true;
        profile.executable = entry.value(QStringLiteral("executable")).toString();
        profile.name = entry.value(QStringLiteral("name")).toString().trimmed();
        if (profile.name.isEmpty()) {
            profile.name = QFileInfo(profile.executable).completeBaseName();
        }
        profile.arguments = entry.value(kArgumentsKey).toString();
        profile.available = startable(profile.executable);
        profile.icon = profile.available ? profile.executable : QString();
        profiles.push_back(profile);
    }

    QString defaultId;
    const QString chosen = value.value(kDefaultKey).toString();
    for (const Profile &profile : std::as_const(profiles)) {
        if (profile.available && (profile.id == chosen || defaultId.isEmpty())) {
            defaultId = profile.id;
            if (profile.id == chosen) {
                break;
            }
        }
    }

    const bool sameRows = profiles.size() == m_profiles.size()
        && std::equal(profiles.cbegin(), profiles.cend(), m_profiles.cbegin(),
                      [](const Profile &a, const Profile &b) { return a.id == b.id; });
    const bool defaultChanged = defaultId != m_defaultProfileId;
    if (sameRows) {
        m_profiles = profiles;
        m_defaultProfileId = defaultId;
        if (!m_profiles.isEmpty()) {
            emit dataChanged(index(0), index(int(m_profiles.size()) - 1));
        }
    } else {
        beginResetModel();
        m_profiles = profiles;
        m_defaultProfileId = defaultId;
        endResetModel();
    }
    emit profilesChanged();
    if (defaultChanged) {
        emit defaultProfileChanged();
    }
}

QVariantMap ShellProfileRegistry::stored() const
{
    return m_settings ? m_settings->shellProfiles() : QVariantMap();
}

void ShellProfileRegistry::store(const QVariantMap &value)
{
    if (m_settings) {
        m_settings->setShellProfiles(value);
    }
}

const ShellProfileRegistry::Profile *ShellProfileRegistry::find(const QString &profileId) const
{
    if (profileId.isEmpty()) {
        return nullptr;
    }
    for (const Profile &profile : m_profiles) {
        if (profile.id == profileId) {
            return &profile;
        }
    }
    return nullptr;
}

} // namespace span::application
