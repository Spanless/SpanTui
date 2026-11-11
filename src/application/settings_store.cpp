#include "application/settings_store.h"

#include "application/application_constants.h"
#include "core/core_constants.h"
#include "core/product_paths.h"
#include "core/workspace_database_migration.h"
#include "workspace/session_model.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>

namespace span::application {

namespace {

constexpr int kSaveDelayMs = 250;

// Persists the per-project focused session independently of the single
// `sessions.active` column, so restarting restores each project's last
// terminal instead of collapsing every project to one global selection.
QString activeByProjectJson(const QVector<SessionRecord> &sessions)
{
    QJsonObject activeByProject;
    for (const SessionRecord &session : sessions) {
        if (session.projectActive && !session.projectPath.isEmpty() && !session.id.isEmpty()) {
            activeByProject.insert(session.projectPath, session.id);
        }
    }
    return QString::fromUtf8(QJsonDocument(activeByProject).toJson(QJsonDocument::Compact));
}

} // namespace

// Rolls back unless commit() succeeds.
class SettingsStore::Transaction {
public:
    explicit Transaction(SettingsStore &store) : m_store(store)
    {
        auto db = QSqlDatabase::database(m_store.m_connectionName);
        if (!db.transaction()) {
            emit m_store.errorOccurred(db.lastError().text());
            return;
        }
        m_active = true;
    }
    ~Transaction()
    {
        if (m_active) {
            QSqlDatabase::database(m_store.m_connectionName).rollback();
        }
    }
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    bool active() const { return m_active; }
    bool commit()
    {
        if (!m_active) {
            return false;
        }
        m_active = false;
        auto db = QSqlDatabase::database(m_store.m_connectionName);
        if (!db.commit()) {
            const QString error = db.lastError().text();
            db.rollback();
            emit m_store.errorOccurred(error);
            return false;
        }
        return true;
    }

private:
    SettingsStore &m_store;
    bool m_active = false;
};

SettingsStore::SettingsStore(QObject *parent) : QObject(parent)
{
    m_connectionName = QStringLiteral("span_settings_%1").arg(reinterpret_cast<quintptr>(this));
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &SettingsStore::flush);
}

SettingsStore::~SettingsStore()
{
    saveNow();
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            auto db = QSqlDatabase::database(m_connectionName, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool SettingsStore::open(Mode mode)
{
    m_readOnly = mode == Mode::ReadOnly;
    m_sessionsLoaded = false;
    if (!openDatabase()) {
        return false;
    }
    m_open = true;
    if (m_readOnly) {
        return true;
    }
    QString migrationError;
    auto db = QSqlDatabase::database(m_connectionName);
    if (!span::core::migrateWorkspaceDatabase(db, &migrationError)) {
        emit errorOccurred(migrationError);
        return false;
    }
    return loadSessions();
}

bool SettingsStore::openDatabase()
{
    if (m_databasePath.isEmpty()) {
        m_databasePath = span::core::ProductPaths::databasePath();
    }
    if (!m_readOnly) {
        QDir().mkpath(QFileInfo(m_databasePath).absolutePath());
    }

    auto db = QSqlDatabase::addDatabase(span::core::kSqliteDriver, m_connectionName);
    if (m_readOnly && !QFileInfo::exists(m_databasePath)) {
        db.setDatabaseName(QStringLiteral(":memory:"));
    } else {
        db.setDatabaseName(m_databasePath);
        if (m_readOnly) {
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        }
    }
    if (!db.open()) {
        emit errorOccurred(db.lastError().text());
        return false;
    }
    QSqlQuery pragma(db);
    if (!m_readOnly) {
        pragma.exec(QStringLiteral("pragma journal_mode=WAL"));
    }
    pragma.exec(span::core::kSqliteBusyTimeoutPragma);
    if (m_readOnly) {
        pragma.exec(QStringLiteral("pragma query_only=ON"));
    }
    return true;
}

QString SettingsStore::value(const QString &key, const QString &fallback) const
{
    for (auto it = m_pending.crbegin(); it != m_pending.crend(); ++it) {
        if (it->first == key) {
            return it->second;
        }
    }
    if (!m_open) {
        return fallback;
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    query.prepare(QStringLiteral("select value from settings where key = ?"));
    query.addBindValue(key);
    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return fallback;
}

void SettingsStore::setValue(const QString &key, const QString &value)
{
    if (m_readOnly || !m_open) {
        return;
    }
    const auto existing = std::find_if(m_pending.begin(), m_pending.end(),
                                       [&key](const QPair<QString, QString> &entry) { return entry.first == key; });
    if (existing != m_pending.end()) {
        existing->second = value;
    } else {
        m_pending.append({key, value});
    }
    m_saveTimer.start();
}

bool SettingsStore::writeValue(const QString &key, const QString &value)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    // Qt SQLite binds empty QString as NULL; settings.value is NOT NULL.
    query.prepare(QStringLiteral("insert into settings(key, value) values(?, ifnull(?, '')) "
                                 "on conflict(key) do update set value = excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    if (query.exec()) {
        return true;
    }
    emit errorOccurred(tr("Could not save setting “%1”: %2").arg(key, query.lastError().text()));
    return false;
}

bool SettingsStore::flush()
{
    m_saveTimer.stop();
    if (m_pending.isEmpty()) {
        return true;
    }
    Transaction transaction(*this);
    if (!transaction.active()) {
        return false;
    }
    for (const auto &[key, value] : std::as_const(m_pending)) {
        if (!writeValue(key, value)) {
            return false;
        }
    }
    if (!transaction.commit()) {
        return false;
    }
    m_pending.clear();
    return true;
}

void SettingsStore::saveNow()
{
    if (m_readOnly || !m_open) {
        return;
    }
    flush();
    saveSessions();
}

QString SettingsStore::workspaceLayoutJson() const
{
    return value(kSettingWorkspaceLayoutJson);
}

void SettingsStore::setWorkspaceLayoutJson(const QString &json)
{
    setValue(kSettingWorkspaceLayoutJson, json);
}

QRect SettingsStore::windowGeometry() const
{
    return QRect(value(kSettingWindowX, QStringLiteral("0")).toInt(),
                 value(kSettingWindowY, QStringLiteral("0")).toInt(),
                 value(kSettingWindowWidth, QStringLiteral("0")).toInt(),
                 value(kSettingWindowHeight, QStringLiteral("0")).toInt());
}

bool SettingsStore::windowMaximized() const
{
    return value(kSettingWindowMaximized, QStringLiteral("0")) == QStringLiteral("1");
}

void SettingsStore::saveWindowGeometry(int x, int y, int width, int height, bool maximized)
{
    if (QRect(x, y, width, height).isValid()) {
        setValue(kSettingWindowX, QString::number(x));
        setValue(kSettingWindowY, QString::number(y));
        setValue(kSettingWindowWidth, QString::number(width));
        setValue(kSettingWindowHeight, QString::number(height));
    }
    setValue(kSettingWindowMaximized, maximized ? QStringLiteral("1") : QStringLiteral("0"));
    flush();
}

void SettingsStore::setSessionModel(SessionModel *model)
{
    if (m_sessionModel == model) {
        return;
    }
    if (m_sessionModel) {
        disconnect(m_sessionModel, nullptr, this, nullptr);
    }
    m_sessionModel = model;
    if (m_sessionModel) {
        connect(m_sessionModel, &SessionModel::sessionsChanged, this, &SettingsStore::saveSessions);
        // Selection-only: update active flags, not a full catalog rewrite.
        connect(m_sessionModel, &SessionModel::activeSessionIdChanged, this, &SettingsStore::saveActiveSessionFlags);
    }
}

bool SettingsStore::saveSessionsNow()
{
    return m_readOnly || saveSessions();
}

bool SettingsStore::loadSessions()
{
    m_sessionsLoaded = false;
    if (!m_sessionModel) {
        return true;
    }
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    if (!query.exec(QStringLiteral("select id, project_path, name, cwd, active, shell_profile "
                                   "from sessions order by sort_order asc, id asc"))) {
        emit errorOccurred(query.lastError().text());
        return false;
    }
    QJsonObject activeByProject;
    const QJsonDocument stored =
        QJsonDocument::fromJson(value(kSettingActiveSessionByProject, QStringLiteral("{}")).toUtf8());
    if (stored.isObject()) {
        activeByProject = stored.object();
    }
    QVector<SessionRecord> sessions;
    while (query.next()) {
        SessionRecord session;
        session.id = query.value(0).toString();
        session.projectPath = query.value(1).toString();
        session.name = query.value(2).toString();
        session.cwd = query.value(3).toString();
        session.active = query.value(4).toInt() == 1;
        session.shellProfile = query.value(5).toString();
        const QString remembered = activeByProject.value(session.projectPath).toString();
        session.projectActive = !remembered.isEmpty() && remembered == session.id;
        sessions.push_back(session);
    }
    m_sessionModel->load(sessions);
    m_sessionsLoaded = true;
    return true;
}

bool SettingsStore::saveSessions()
{
    if (m_readOnly) {
        return true;
    }
    if (!m_sessionModel || !m_sessionsLoaded) {
        return false;
    }
    Transaction transaction(*this);
    if (!transaction.active()) {
        return false;
    }
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery clear(db);
    if (!clear.exec(QStringLiteral("delete from sessions"))) {
        emit errorOccurred(clear.lastError().text());
        return false;
    }
    QSqlQuery insert(db);
    insert.prepare(
        QStringLiteral("insert into sessions(id, project_path, name, cwd, active, sort_order, shell_profile) "
                       "values(?, ?, ?, ?, ?, ?, ?)"));
    const QVector<SessionRecord> sessions = m_sessionModel->records();
    for (int i = 0; i < sessions.size(); ++i) {
        const SessionRecord &session = sessions.at(i);
        if (session.projectPath.isEmpty()) {
            continue;
        }
        insert.addBindValue(session.id);
        insert.addBindValue(session.projectPath);
        insert.addBindValue(session.name);
        insert.addBindValue(session.cwd);
        insert.addBindValue(session.active ? 1 : 0);
        insert.addBindValue(i);
        // A null QString binds as SQL NULL; the column is "not null".
        insert.addBindValue(session.shellProfile.isNull() ? QStringLiteral("") : session.shellProfile);
        if (!insert.exec()) {
            emit errorOccurred(insert.lastError().text());
            return false;
        }
    }
    return writeValue(kSettingActiveSessionByProject, activeByProjectJson(sessions)) && transaction.commit();
}

bool SettingsStore::saveActiveSessionFlags()
{
    if (m_readOnly) {
        return true;
    }
    if (!m_sessionModel || !m_sessionsLoaded) {
        return false;
    }
    Transaction transaction(*this);
    if (!transaction.active()) {
        return false;
    }
    auto db = QSqlDatabase::database(m_connectionName);
    QSqlQuery clearActive(db);
    if (!clearActive.exec(QStringLiteral("update sessions set active = 0"))) {
        emit errorOccurred(clearActive.lastError().text());
        return false;
    }
    QSqlQuery setActive(db);
    if (!setActive.prepare(QStringLiteral("update sessions set active = 1 where id = ?"))) {
        emit errorOccurred(setActive.lastError().text());
        return false;
    }
    const QVector<SessionRecord> sessions = m_sessionModel->records();
    for (const SessionRecord &session : sessions) {
        if (!session.active || session.id.isEmpty()) {
            continue;
        }
        setActive.addBindValue(session.id);
        if (!setActive.exec()) {
            emit errorOccurred(setActive.lastError().text());
            return false;
        }
    }
    return writeValue(kSettingActiveSessionByProject, activeByProjectJson(sessions)) && transaction.commit();
}

} // namespace span::application
