#include "application/project_workspace_registry.h"
#include "application/settings_store.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using span::application::ProjectWorkspaceRegistry;

class ProjectWorkspaceRegistryTest final : public QObject {
    Q_OBJECT

private slots:
    void startsWithoutProjects();
    void addProjectPersistsCanonicalPath();
    void addProjectRefusesMissingDirectory();
    void addProjectIsIdempotent();
    void removeProjectDropsPersistedRow();
    void persistsExpansion();
};

namespace {

QString makeProject(const QTemporaryDir &dir, const QString &name)
{
    const QString path = dir.filePath(name);
    QDir().mkpath(path);
    return QFileInfo(path).canonicalFilePath();
}

void prepareSchema(const QString &databasePath)
{
    span::application::SettingsStore state;
    state.setDatabasePathForTesting(databasePath);
    QVERIFY(state.open());
}

} // namespace

void ProjectWorkspaceRegistryTest::startsWithoutProjects()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry registry(databasePath);
    QCOMPARE(registry.count(), 0);
    QVERIFY(registry.needsInitialProject());
    QVERIFY(registry.projectPaths().isEmpty());
}

void ProjectWorkspaceRegistryTest::addProjectPersistsCanonicalPath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    {
        ProjectWorkspaceRegistry registry(databasePath);
        QVERIFY(registry.addProject(project));
        QCOMPARE(registry.count(), 1);
        QVERIFY(registry.contains(project));
        QCOMPARE(registry.canonicalProjectPath(project), project);
        QVERIFY(!registry.needsInitialProject());
    }

    ProjectWorkspaceRegistry reloaded(databasePath);
    QCOMPARE(reloaded.count(), 1);
    QCOMPARE(reloaded.projectPaths(), QStringList{project});
}

void ProjectWorkspaceRegistryTest::addProjectRefusesMissingDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry registry(databasePath);
    QSignalSpy errors(&registry, &ProjectWorkspaceRegistry::errorOccurred);
    QVERIFY(!registry.addProject(dir.filePath(QStringLiteral("missing"))));
    QCOMPARE(registry.count(), 0);
    QCOMPARE(errors.size(), 1);
}

void ProjectWorkspaceRegistryTest::addProjectIsIdempotent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry registry(databasePath);
    QVERIFY(registry.addProject(project));
    QVERIFY(registry.addProject(project));
    QCOMPARE(registry.count(), 1);
}

void ProjectWorkspaceRegistryTest::removeProjectDropsPersistedRow()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    {
        ProjectWorkspaceRegistry registry(databasePath);
        QVERIFY(registry.addProject(project));
        QVERIFY(registry.removeProject(project));
        QCOMPARE(registry.count(), 0);
        QVERIFY(!registry.removeProject(project));
    }

    ProjectWorkspaceRegistry reloaded(databasePath);
    QCOMPARE(reloaded.count(), 0);
}

void ProjectWorkspaceRegistryTest::persistsExpansion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    {
        ProjectWorkspaceRegistry registry(databasePath);
        QVERIFY(registry.addProject(project));
        QVERIFY(registry.setExpanded(project, false));
        QVERIFY(!registry.setExpanded(QStringLiteral("missing"), false));
    }

    ProjectWorkspaceRegistry reloaded(databasePath);
    QCOMPARE(reloaded.count(), 1);
    QCOMPARE(reloaded.data(reloaded.index(0), ProjectWorkspaceRegistry::ExpandedRole).toBool(),
             false);
}

QTEST_MAIN(ProjectWorkspaceRegistryTest)
#include "project_workspace_registry_test.moc"
