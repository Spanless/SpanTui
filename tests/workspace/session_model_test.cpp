#include "workspace/session_model.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

QString canonical(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

QVariant role(const SessionModel &model, int row, SessionModel::Role role)
{
    return model.data(model.index(row), role);
}

} // namespace

class SessionModelTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void addAssignsDefaultsAndActivates();
    void localizesStoredDefaultNames();
    void normalizesProjectPaths();
    void activationIsSelectionOnly();
    void removingActiveSessionStaysInsideProject();
    void removingEarlierRowKeepsActiveSession();
    void remembersFocusedSessionPerProject();
    void serializeLoadRoundTrip();
    void loadFillsMissingFields();
    void renameTrimsAndRejectsBlankNames();
    void metadataChangesDoNotPretendToChangeActiveSession();
    void commandLineIsTransient();
    void listsSessionsForProject();
    void remoteScopesStayOpaque();

private:
    QTemporaryDir m_projectA;
    QTemporaryDir m_projectB;
};

void SessionModelTest::init()
{
    QVERIFY(m_projectA.isValid());
    QVERIFY(m_projectB.isValid());
}

void SessionModelTest::addAssignsDefaultsAndActivates()
{
    SessionModel model;
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy sessionsChanged(&model, &SessionModel::sessionsChanged);
    QSignalSpy activeChanged(&model, &SessionModel::activeSessionIdChanged);

    const QString first = model.addSession({}, m_projectA.path(), {});
    const QString second = model.addSession(QStringLiteral("build"), m_projectA.path(), {});
    QVERIFY(!first.isEmpty());
    QVERIFY(first != second);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(inserted.count(), 2);
    QCOMPARE(sessionsChanged.count(), 2);
    QCOMPARE(activeChanged.count(), 2);
    QCOMPARE(model.activeSessionId(), second);
    QCOMPARE(model.activeIndex(), 1);
    QCOMPARE(model.sessionIdAt(0), first);
    QVERIFY(model.sessionIdAt(2).isEmpty());

    QCOMPARE(role(model, 0, SessionModel::NameRole).toString(), QStringLiteral("Terminal 1"));
    QCOMPARE(role(model, 1, SessionModel::NameRole).toString(), QStringLiteral("build"));
    QCOMPARE(role(model, 0, SessionModel::CwdRole).toString(), m_projectA.path());
    // Without an explicit project the working directory is the project.
    QCOMPARE(role(model, 0, SessionModel::ProjectPathRole).toString(), canonical(m_projectA.path()));
    QCOMPARE(role(model, 0, SessionModel::ActiveRole).toBool(), false);
    QCOMPARE(role(model, 1, SessionModel::ActiveRole).toBool(), true);
    QVERIFY(!model.data(model.index(2), SessionModel::IdRole).isValid());

    // No directory at all falls back to the home directory.
    const QString home = model.addSession();
    const int row = model.indexForSessionId(home);
    QCOMPARE(role(model, row, SessionModel::CwdRole).toString(), QDir::homePath());
    QCOMPARE(role(model, row, SessionModel::ProjectPathRole).toString(), canonical(QDir::homePath()));
}

void SessionModelTest::localizesStoredDefaultNames()
{
    // Default names are stored as literal text in whichever language created
    // them; the model shows them in the current one and keeps custom names.
    SessionModel model;
    const auto stored = [this](const QString &id, const QString &name) {
        SessionRecord record;
        record.id = id;
        record.name = name;
        record.cwd = m_projectA.path();
        return record;
    };
    model.load({
        stored(QStringLiteral("zh"), QStringLiteral("  终端 3 ")),
        stored(QStringLiteral("en"), QStringLiteral("Terminal7")),
        stored(QStringLiteral("custom"), QStringLiteral("My Terminal 2")),
    });
    QCOMPARE(model.nameForSessionId(QStringLiteral("zh")), QStringLiteral("Terminal 3"));
    QCOMPARE(model.nameForSessionId(QStringLiteral("en")), QStringLiteral("Terminal 7"));
    QCOMPARE(model.nameForSessionId(QStringLiteral("custom")), QStringLiteral("My Terminal 2"));
    QVERIFY(model.nameForSessionId(QStringLiteral("missing")).isEmpty());
    // Persistence keeps what was stored.
    QCOMPARE(model.records().at(0).name, QStringLiteral("  终端 3 "));
}

void SessionModelTest::normalizesProjectPaths()
{
    SessionModel model;
    const QString withSlash = m_projectA.path() + QStringLiteral("/");
    const QString id = model.addSession({}, m_projectA.path(), withSlash);
    QCOMPARE(model.projectPathForSession(id), canonical(m_projectA.path()));
    // Lookups normalize the query the same way.
    QCOMPARE(model.countForProject(withSlash), 1);
    QCOMPARE(model.countForProject(m_projectA.path()), 1);
    QCOMPARE(model.countForProject(m_projectB.path()), 0);
    QCOMPARE(model.countForProject(QString()), 0);

    // A project that does not exist (yet) is cleaned lexically.
    const QString missing = QStringLiteral("C:/span-missing-project/a/../b");
    const QString missingId = model.addSession({}, m_projectA.path(), missing);
    QCOMPARE(model.projectPathForSession(missingId), QDir::cleanPath(missing));
    QCOMPARE(model.countForProject(QStringLiteral("C:/span-missing-project/b")), 1);
    QVERIFY(model.projectPathForSession(QStringLiteral("missing")).isEmpty());
}

void SessionModelTest::activationIsSelectionOnly()
{
    SessionModel model;
    const QString first = model.addSession({}, m_projectA.path());
    const QString second = model.addSession({}, m_projectA.path());
    model.addSession({}, m_projectA.path());
    QCOMPARE(model.activeIndex(), 2);

    QSignalSpy sessionsChanged(&model, &SessionModel::sessionsChanged);
    QSignalSpy indexChanged(&model, &SessionModel::activeIndexChanged);
    QSignalSpy idChanged(&model, &SessionModel::activeSessionIdChanged);
    QSignalSpy dataChanged(&model, &QAbstractItemModel::dataChanged);

    model.activate(0);
    QCOMPARE(model.activeSessionId(), first);
    QCOMPARE(indexChanged.count(), 1);
    QCOMPARE(idChanged.count(), 1);
    // Switching tabs must not rewrite the catalog or resynchronize terminals.
    QCOMPARE(sessionsChanged.count(), 0);
    // The old and the new row repaint their active flag, nothing else.
    QCOMPARE(dataChanged.count(), 2);
    QCOMPARE(dataChanged.at(0).at(2).value<QList<int>>(), QList<int>{SessionModel::ActiveRole});
    QCOMPARE(dataChanged.at(1).at(2).value<QList<int>>(), QList<int>{SessionModel::ActiveRole});
    QCOMPARE(dataChanged.at(0).at(0).toModelIndex().row(), 2);
    QCOMPARE(dataChanged.at(1).at(0).toModelIndex().row(), 0);
    QCOMPARE(role(model, 0, SessionModel::ActiveRole).toBool(), true);
    QCOMPARE(role(model, 2, SessionModel::ActiveRole).toBool(), false);

    // Re-activating the active row or an invalid one is silent.
    model.activate(0);
    model.activate(-1);
    model.activate(3);
    model.activateById(QStringLiteral("missing"));
    QCOMPARE(indexChanged.count(), 1);
    QCOMPARE(dataChanged.count(), 2);

    model.activateById(second);
    QCOMPARE(model.activeIndex(), 1);
    QCOMPARE(sessionsChanged.count(), 0);
}

void SessionModelTest::removingActiveSessionStaysInsideProject()
{
    SessionModel model;
    const QString a1 = model.addSession({}, m_projectA.path());
    const QString b1 = model.addSession({}, m_projectB.path());
    const QString a2 = model.addSession({}, m_projectA.path());
    QCOMPARE(model.activeSessionId(), a2);

    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy idChanged(&model, &SessionModel::activeSessionIdChanged);
    model.removeSessionById(a2);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(model.count(), 2);
    // The neighbour row belongs to another project; focus stays in A.
    QCOMPARE(model.activeSessionId(), a1);
    QCOMPARE(idChanged.count(), 1);
    QCOMPARE(role(model, model.activeIndex(), SessionModel::ActiveRole).toBool(), true);

    // Closing a project's last session leaves no active session rather than
    // routing another project's terminal into it.
    model.activateById(b1);
    model.removeSessionById(b1);
    QCOMPARE(model.activeIndex(), -1);
    QVERIFY(model.activeSessionId().isEmpty());
    QCOMPARE(model.count(), 1);

    model.removeSession(5);
    model.removeSessionById(QStringLiteral("missing"));
    QCOMPARE(model.count(), 1);
}

void SessionModelTest::removingEarlierRowKeepsActiveSession()
{
    SessionModel model;
    model.addSession({}, m_projectA.path());
    model.addSession({}, m_projectA.path());
    const QString active = model.addSession({}, m_projectA.path());

    QSignalSpy indexChanged(&model, &SessionModel::activeIndexChanged);
    QSignalSpy idChanged(&model, &SessionModel::activeSessionIdChanged);
    model.removeSession(0);
    QCOMPARE(model.activeIndex(), 1);
    QCOMPARE(model.activeSessionId(), active);
    QCOMPARE(indexChanged.count(), 1);
    QCOMPARE(idChanged.count(), 0);

    // A later row leaves the index alone.
    model.addSession({}, m_projectA.path());
    model.activateById(active);
    indexChanged.clear();
    model.removeSession(2);
    QCOMPARE(model.activeSessionId(), active);
    QCOMPARE(indexChanged.count(), 0);
}

void SessionModelTest::remembersFocusedSessionPerProject()
{
    SessionModel model;
    const QString a1 = model.addSession({}, m_projectA.path());
    const QString a2 = model.addSession({}, m_projectA.path());
    const QString b1 = model.addSession({}, m_projectB.path());
    Q_UNUSED(a1);

    model.activateById(a2);
    model.activateById(b1);
    // Returning to A restores its last focused session, not its first row.
    QCOMPARE(model.activeIndexForProject(m_projectA.path()), model.indexForSessionId(a2));
    QCOMPARE(model.activeIndexForProject(m_projectB.path()), model.indexForSessionId(b1));
    QCOMPARE(model.activeIndexForProject(QStringLiteral("C:/span-no-such-project")), -1);
    QCOMPARE(model.activeIndexForProject(QString()), -1);

    // Once the remembered session is gone, A falls back to its first row.
    model.removeSessionById(a2);
    QCOMPARE(model.activeSessionId(), b1);
    QCOMPARE(model.activeIndexForProject(m_projectA.path()), 0);
}

void SessionModelTest::serializeLoadRoundTrip()
{
    SessionModel model;
    const QString a1 = model.addSession(QStringLiteral("server"), m_projectA.path());
    const QString a2 = model.addSession({}, m_projectA.path());
    const QString b1 = model.addSession({}, m_projectB.path());
    model.activateById(a2);
    model.activateById(b1);

    const QVector<SessionRecord> stored = model.records();
    QCOMPARE(stored.size(), 3);
    QCOMPARE(stored.at(0).active, false);
    QCOMPARE(stored.at(2).active, true);
    QCOMPARE(stored.at(0).projectActive, false);
    QCOMPARE(stored.at(1).projectActive, true);
    QCOMPARE(stored.at(2).projectActive, true);

    SessionModel restored;
    QSignalSpy reset(&restored, &QAbstractItemModel::modelReset);
    QSignalSpy sessionsChanged(&restored, &SessionModel::sessionsChanged);
    restored.load(stored);
    QCOMPARE(reset.count(), 1);
    QCOMPARE(sessionsChanged.count(), 1);
    QCOMPARE(restored.count(), 3);
    QCOMPARE(restored.sessionIdAt(0), a1);
    QCOMPARE(restored.sessionIdAt(1), a2);
    QCOMPARE(restored.sessionIdAt(2), b1);
    QCOMPARE(restored.nameForSessionId(a1), QStringLiteral("server"));
    QCOMPARE(restored.activeSessionId(), b1);
    QCOMPARE(restored.activeIndexForProject(m_projectA.path()), 1);
    QVERIFY(restored.records() == stored);
}

void SessionModelTest::loadFillsMissingFields()
{
    SessionModel model;
    SessionRecord projectOnly;
    projectOnly.projectPath = m_projectA.path();
    SessionRecord cwdOnly;
    cwdOnly.id = QStringLiteral("fixed");
    cwdOnly.cwd = m_projectB.path();
    model.load({projectOnly, cwdOnly, SessionRecord{}});
    QCOMPARE(model.count(), 3);
    // No stored active flag: the first session is active.
    QCOMPARE(model.activeIndex(), 0);

    QVERIFY(!model.sessionIdAt(0).isEmpty());
    QVERIFY(model.sessionIdAt(0) != model.sessionIdAt(2));
    QCOMPARE(role(model, 0, SessionModel::NameRole).toString(), QStringLiteral("Terminal"));
    // A project without a cwd starts in the project...
    QCOMPARE(role(model, 0, SessionModel::CwdRole).toString(), canonical(m_projectA.path()));
    // ...and a cwd without a project becomes the project.
    QCOMPARE(model.sessionIdAt(1), QStringLiteral("fixed"));
    QCOMPARE(model.projectPathForSession(QStringLiteral("fixed")), canonical(m_projectB.path()));
    QCOMPARE(role(model, 2, SessionModel::CwdRole).toString(), QDir::homePath());

    // Loading replaces, it does not append.
    model.load({});
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.activeIndex(), -1);
}

void SessionModelTest::renameTrimsAndRejectsBlankNames()
{
    SessionModel model;
    model.addSession({}, m_projectA.path());
    QSignalSpy sessionsChanged(&model, &SessionModel::sessionsChanged);
    QSignalSpy dataChanged(&model, &QAbstractItemModel::dataChanged);

    model.renameSession(0, QStringLiteral("  logs  "));
    QCOMPARE(role(model, 0, SessionModel::NameRole).toString(), QStringLiteral("logs"));
    QCOMPARE(sessionsChanged.count(), 1);
    QCOMPARE(dataChanged.count(), 1);
    QCOMPARE(dataChanged.at(0).at(2).value<QList<int>>(), QList<int>{SessionModel::NameRole});

    model.renameSession(0, QStringLiteral("   "));
    model.renameSession(4, QStringLiteral("other"));
    QCOMPARE(role(model, 0, SessionModel::NameRole).toString(), QStringLiteral("logs"));
    QCOMPARE(sessionsChanged.count(), 1);
}

void SessionModelTest::metadataChangesDoNotPretendToChangeActiveSession()
{
    SessionModel model;
    model.addSession(QStringLiteral("shell"), m_projectA.path(), m_projectA.path());
    QSignalSpy idChanged(&model, &SessionModel::activeSessionIdChanged);
    QSignalSpy rowsChanged(&model, &SessionModel::dataChanged);

    model.renameSession(model.activeIndex(), QStringLiteral("renamed"));
    QCOMPARE(idChanged.count(), 0);
    QCOMPARE(rowsChanged.count(), 1);

    model.retranslate();
    QCOMPARE(idChanged.count(), 0);
    QCOMPARE(rowsChanged.count(), 2);
}

void SessionModelTest::commandLineIsTransient()
{
    SessionModel model;
    const QString command = QStringLiteral("powershell.exe -NoExit -Command \"& 'C:\\work\\test.ps1'\"");
    const QString id = model.addCommandSession(QStringLiteral("Run test.ps1"), m_projectA.path(), m_projectA.path(),
                                               QStringLiteral("  ") + command + QStringLiteral("\n"));

    QVERIFY(!id.isEmpty());
    QCOMPARE(model.rowCount(), 1);
    // SessionRecord has no command field, so the command never reaches
    // persistence: a reload starts a plain shell. It is handed out once,
    // trimmed.
    QCOMPARE(model.takeInitialCommandLine(id), command);
    QVERIFY(model.takeInitialCommandLine(id).isEmpty());
    QVERIFY(model.takeInitialCommandLine(QStringLiteral("missing")).isEmpty());
}

void SessionModelTest::listsSessionsForProject()
{
    SessionModel model;
    const QString a1 = model.addSession(QStringLiteral("one"), m_projectA.path());
    model.addSession({}, m_projectB.path());
    const QString a2 = model.addSession({}, m_projectA.path());

    const QVariantList sessions = model.sessionsForProject(m_projectA.path());
    QCOMPARE(sessions.size(), 2);
    const QVariantMap first = sessions.at(0).toMap();
    const QVariantMap second = sessions.at(1).toMap();
    QCOMPARE(first.value(QStringLiteral("sessionId")).toString(), a1);
    QCOMPARE(first.value(QStringLiteral("index")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("name")).toString(), QStringLiteral("one"));
    QCOMPARE(first.value(QStringLiteral("active")).toBool(), false);
    QCOMPARE(second.value(QStringLiteral("sessionId")).toString(), a2);
    QCOMPARE(second.value(QStringLiteral("index")).toInt(), 2);
    QCOMPARE(second.value(QStringLiteral("name")).toString(), QStringLiteral("Terminal 3"));
    QCOMPARE(second.value(QStringLiteral("active")).toBool(), true);
    QVERIFY(model.sessionsForProject(QString()).isEmpty());
}

void SessionModelTest::remoteScopesStayOpaque()
{
    SessionModel model;
    const QString remote = model.addSession(QStringLiteral("web"), {}, QStringLiteral("remote:h1"));
    model.addSession({}, m_projectA.path());

    // A host key is a scope, never a path: no canonicalization, and the
    // local shell side starts in the home directory.
    QCOMPARE(model.projectPathForSession(remote), QStringLiteral("remote:h1"));
    QCOMPARE(role(model, 0, SessionModel::CwdRole).toString(), QDir::homePath());
    const QVariantList sessions = model.sessionsForRemoteHost(QStringLiteral("h1"));
    QCOMPARE(sessions.size(), 1);
    QCOMPARE(sessions.at(0).toMap().value(QStringLiteral("sessionId")).toString(), remote);
    QVERIFY(model.sessionsForRemoteHost(QStringLiteral("h2")).isEmpty());
    QVERIFY(model.sessionsForRemoteHost(QString()).isEmpty());

    SessionModel restored;
    SessionRecord remoteOnly;
    remoteOnly.projectPath = QStringLiteral("remote:h1");
    restored.load({remoteOnly});
    QCOMPARE(restored.projectPathForSession(restored.sessionIdAt(0)), QStringLiteral("remote:h1"));
    QCOMPARE(role(restored, 0, SessionModel::CwdRole).toString(), QDir::homePath());
}

QTEST_GUILESS_MAIN(SessionModelTest)
#include "session_model_test.moc"
