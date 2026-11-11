#include "workspace/session_model.h"

#include "workspace/scope.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>

#include <utility>

using span::workspace::Scope;

SessionModel::SessionModel(QObject *parent) : QAbstractListModel(parent) {}

QString SessionModel::displayName(const QString &storedName)
{
    // Default names are persisted as literal text; re-localize so language
    // switches (and mixed en/zh history) stay consistent in the UI.
    static const QRegularExpression defaultNameRe(QStringLiteral(R"(^(?:Terminal|终端)\s*(\d+)$)"));
    const auto match = defaultNameRe.match(storedName.trimmed());
    if (match.hasMatch()) {
        return tr("Terminal %1").arg(match.captured(1));
    }
    return storedName;
}

QString SessionModel::normalizePath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    // Remote host scopes are opaque keys, not filesystem paths.
    if (Scope::fromKey(path).isRemoteHost()) {
        return path;
    }
    const QFileInfo info(path);
    if (info.exists()) {
        return info.canonicalFilePath();
    }
    return QDir::cleanPath(path);
}

int SessionModel::indexForId(const QString &id) const
{
    if (id.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

void SessionModel::rememberActiveForProject(int row)
{
    if (row < 0 || row >= m_sessions.size()) {
        return;
    }
    const Session &session = m_sessions.at(row);
    if (session.projectPath.isEmpty() || session.id.isEmpty()) {
        return;
    }
    m_lastActiveIdByProject.insert(session.projectPath, session.id);
}

void SessionModel::setActiveIndex(int row)
{
    if (row < -1 || row >= m_sessions.size()) {
        return;
    }
    if (row == m_activeIndex) {
        if (row >= 0) {
            rememberActiveForProject(row);
        }
        return;
    }
    const int old = m_activeIndex;
    m_activeIndex = row;
    if (old >= 0 && old < m_sessions.size()) {
        emit dataChanged(index(old), index(old), {ActiveRole});
    }
    if (m_activeIndex >= 0) {
        rememberActiveForProject(m_activeIndex);
        emit dataChanged(index(m_activeIndex), index(m_activeIndex), {ActiveRole});
    }
    emit activeIndexChanged();
    emit activeSessionIdChanged();
}

int SessionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_sessions.size();
}

QVariant SessionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_sessions.size()) {
        return {};
    }

    const auto &session = m_sessions.at(index.row());
    switch (role) {
    case IdRole: return session.id;
    case NameRole: return displayName(session.name);
    case CwdRole: return session.cwd;
    case ProjectPathRole: return session.projectPath;
    case ActiveRole: return index.row() == m_activeIndex;
    default: return {};
    }
}

QHash<int, QByteArray> SessionModel::roleNames() const
{
    return {
        {IdRole, "sessionId"},  {NameRole, "name"}, {CwdRole, "cwd"}, {ProjectPathRole, "projectPath"},
        {ActiveRole, "active"},
    };
}

int SessionModel::activeIndex() const
{
    return m_activeIndex;
}

QString SessionModel::activeSessionId() const
{
    if (m_activeIndex < 0 || m_activeIndex >= m_sessions.size()) {
        return {};
    }
    return m_sessions.at(m_activeIndex).id;
}

QString SessionModel::sessionIdAt(int row) const
{
    if (row < 0 || row >= m_sessions.size()) {
        return {};
    }
    return m_sessions.at(row).id;
}

QString SessionModel::projectPathForSession(const QString &id) const
{
    const int row = indexForId(id);
    return row < 0 ? QString() : m_sessions.at(row).projectPath;
}

QString SessionModel::nameForSessionId(const QString &id) const
{
    const int row = indexForId(id);
    return row < 0 ? QString() : displayName(m_sessions.at(row).name);
}

int SessionModel::indexForSessionId(const QString &id) const
{
    return indexForId(id);
}

int SessionModel::count() const
{
    return m_sessions.size();
}

int SessionModel::countForProject(const QString &projectPath) const
{
    const QString path = normalizePath(projectPath);
    if (path.isEmpty()) {
        return 0;
    }
    int total = 0;
    for (const auto &session : m_sessions) {
        if (session.projectPath == path) {
            ++total;
        }
    }
    return total;
}

int SessionModel::firstIndexForProject(const QString &projectPath) const
{
    const QString path = normalizePath(projectPath);
    if (path.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions.at(i).projectPath == path) {
            return i;
        }
    }
    return -1;
}

int SessionModel::activeIndexForProject(const QString &projectPath) const
{
    const QString path = normalizePath(projectPath);
    if (path.isEmpty()) {
        return -1;
    }
    // Prefer the last session the user focused inside this project — including
    // after visiting another project. Falling back to "first catalog row" made
    // every project switch jump to Terminal 1 and look broken.
    const QString rememberedId = m_lastActiveIdByProject.value(path);
    const int remembered = indexForId(rememberedId);
    if (remembered >= 0 && m_sessions.at(remembered).projectPath == path) {
        return remembered;
    }
    if (m_activeIndex >= 0 && m_activeIndex < m_sessions.size() && m_sessions.at(m_activeIndex).projectPath == path) {
        return m_activeIndex;
    }
    return firstIndexForProject(path);
}

QVariantList SessionModel::sessionsForProject(const QString &projectPath) const
{
    const QString path = normalizePath(projectPath);
    QVariantList result;
    if (path.isEmpty()) {
        return result;
    }
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions.at(i).projectPath != path) {
            continue;
        }
        QVariantMap item;
        item[QStringLiteral("index")] = i;
        item[QStringLiteral("sessionId")] = m_sessions.at(i).id;
        item[QStringLiteral("name")] = displayName(m_sessions.at(i).name);
        item[QStringLiteral("cwd")] = m_sessions.at(i).cwd;
        item[QStringLiteral("projectPath")] = m_sessions.at(i).projectPath;
        item[QStringLiteral("active")] = (i == m_activeIndex);
        result.push_back(item);
    }
    return result;
}

QVariantList SessionModel::sessionsForRemoteHost(const QString &hostId) const
{
    return sessionsForProject(Scope::remoteHost(hostId).key());
}

QVector<SessionRecord> SessionModel::records() const
{
    QVector<SessionRecord> result;
    result.reserve(m_sessions.size());
    for (int i = 0; i < m_sessions.size(); ++i) {
        const Session &session = m_sessions.at(i);
        SessionRecord record;
        record.id = session.id;
        record.name = session.name;
        record.cwd = session.cwd;
        record.projectPath = session.projectPath;
        record.shellProfile = session.shellProfile;
        record.active = i == m_activeIndex;
        // Persist one "last active" flag per project so reload restores each
        // project's focused terminal next to the single global active flag.
        const QString remembered = m_lastActiveIdByProject.value(session.projectPath);
        record.projectActive = remembered.isEmpty() ? record.active : remembered == session.id;
        result.push_back(record);
    }
    return result;
}

void SessionModel::load(const QVector<SessionRecord> &records)
{
    beginResetModel();
    m_sessions.clear();
    m_lastActiveIdByProject.clear();
    m_activeIndex = -1;
    for (const SessionRecord &record : records) {
        Session session;
        session.id = record.id;
        session.name = record.name;
        session.cwd = record.cwd;
        session.projectPath = normalizePath(record.projectPath);
        session.shellProfile = record.shellProfile;
        if (session.id.isEmpty()) {
            session.id = QString::number(QDateTime::currentMSecsSinceEpoch()) + QStringLiteral("-")
                + QString::number(m_sessions.size());
        }
        if (session.name.isEmpty()) {
            session.name = QStringLiteral("Terminal");
        }
        if (session.cwd.isEmpty()) {
            const QString projectPath = Scope::fromKey(session.projectPath).projectPath();
            session.cwd = projectPath.isEmpty() ? QDir::homePath() : projectPath;
        }
        if (session.projectPath.isEmpty()) {
            session.projectPath = normalizePath(session.cwd);
        }
        if (record.projectActive && !session.projectPath.isEmpty()) {
            m_lastActiveIdByProject.insert(session.projectPath, session.id);
        }
        if (record.active) {
            m_activeIndex = m_sessions.size();
        }
        m_sessions.push_back(session);
    }
    if (!m_sessions.isEmpty() && m_activeIndex < 0) {
        m_activeIndex = 0;
        rememberActiveForProject(m_activeIndex);
    } else if (m_activeIndex >= 0) {
        rememberActiveForProject(m_activeIndex);
    }
    endResetModel();
    emit activeIndexChanged();
    emit activeSessionIdChanged();
    emit sessionsChanged();
}

QString SessionModel::addSession(const QString &name, const QString &cwd, const QString &projectPath,
                                 const QString &shellProfile)
{
    return insertSession(name, cwd, projectPath, {}, shellProfile);
}

QString SessionModel::addCommandSession(const QString &name, const QString &cwd, const QString &projectPath,
                                        const QString &commandLine)
{
    return insertSession(name, cwd, projectPath, commandLine);
}

QString SessionModel::insertSession(const QString &name, const QString &cwd, const QString &projectPath,
                                    const QString &initialCommandLine, const QString &shellProfile)
{
    const int row = m_sessions.size();
    beginInsertRows({}, row, row);
    Session session;
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Persist the English default; displayName() localizes for the UI.
    session.name = name.isEmpty() ? QStringLiteral("Terminal %1").arg(row + 1) : name;
    session.cwd = cwd.isEmpty() ? QDir::homePath() : cwd;
    session.projectPath = normalizePath(projectPath.isEmpty() ? session.cwd : projectPath);
    if (session.projectPath.isEmpty()) {
        session.projectPath = normalizePath(session.cwd);
    }
    session.initialCommandLine = initialCommandLine.trimmed();
    session.shellProfile = shellProfile;
    const QString id = session.id;
    m_sessions.push_back(session);
    endInsertRows();
    setActiveIndex(row);
    emit sessionsChanged();
    return id;
}

QString SessionModel::takeInitialCommandLine(const QString &id)
{
    const int row = indexForId(id);
    if (row < 0) {
        return {};
    }
    return std::exchange(m_sessions[row].initialCommandLine, QString());
}

QString SessionModel::shellProfileForSession(const QString &id) const
{
    const int row = indexForId(id);
    return row < 0 ? QString() : m_sessions.at(row).shellProfile;
}

void SessionModel::removeSession(int row)
{
    if (row < 0 || row >= m_sessions.size()) {
        return;
    }

    const QString removedId = m_sessions.at(row).id;
    const QString projectPath = m_sessions.at(row).projectPath;
    const bool removedActive = row == m_activeIndex;

    beginRemoveRows({}, row, row);
    m_sessions.removeAt(row);
    endRemoveRows();

    if (m_lastActiveIdByProject.value(projectPath) == removedId) {
        m_lastActiveIdByProject.remove(projectPath);
    }

    if (removedActive) {
        // Stay inside the same project when possible. If its final session was
        // closed, show the explicit no-terminal state; never route another
        // project's PTY through the still-selected project.
        m_activeIndex = firstIndexForProject(projectPath);
        if (m_activeIndex >= 0) {
            rememberActiveForProject(m_activeIndex);
            emit dataChanged(index(m_activeIndex), index(m_activeIndex), {ActiveRole});
        }
        emit activeIndexChanged();
        emit activeSessionIdChanged();
    } else if (row < m_activeIndex) {
        --m_activeIndex;
        emit activeIndexChanged();
    }

    emit sessionsChanged();
}

void SessionModel::activateById(const QString &id)
{
    activate(indexForId(id));
}

void SessionModel::removeSessionById(const QString &id)
{
    removeSession(indexForId(id));
}

void SessionModel::activate(int row)
{
    if (row < 0 || row >= m_sessions.size()) {
        return;
    }
    // Selection-only: do not emit sessionsChanged (avoids full DB rewrite +
    // TerminalManager::synchronize on every tab switch).
    setActiveIndex(row);
}

void SessionModel::renameSession(int row, const QString &name)
{
    if (row < 0 || row >= m_sessions.size() || name.trimmed().isEmpty()) {
        return;
    }
    m_sessions[row].name = name.trimmed();
    emit dataChanged(index(row), index(row), {NameRole});
    emit sessionsChanged();
}

void SessionModel::retranslate()
{
    if (m_sessions.isEmpty()) {
        return;
    }
    emit dataChanged(index(0), index(m_sessions.size() - 1), {NameRole});
}
