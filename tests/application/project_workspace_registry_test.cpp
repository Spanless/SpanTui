#include "application/project_workspace_registry.h"
#include "application/settings_store.h"

#include <QDir>
#include <QFile>
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
    void missingFolderStaysRemovable();
    void worktreeNestsUnderMainProject();
    void mainProjectAdoptsListedWorktrees();
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

bool writeText(const QString &path, const QByteArray &text)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(text) == text.size();
}

// The files `git worktree add` leaves behind, written by hand: `main` holds a
// `.git` directory, `worktree` a `.git` file pointing into
// `main/.git/worktrees/<id>`, checked out on `branch`.
bool linkWorktree(const QString &main, const QString &worktree, const QString &id, const QByteArray &branch)
{
    const QString admin = main + QStringLiteral("/.git/worktrees/") + id;
    return writeText(main + QStringLiteral("/.git/HEAD"), "ref: refs/heads/main\n")
        && writeText(admin + QStringLiteral("/gitdir"), (worktree + QStringLiteral("/.git\n")).toUtf8())
        && writeText(admin + QStringLiteral("/commondir"), "../..\n")
        && writeText(admin + QStringLiteral("/HEAD"), "ref: refs/heads/" + branch + '\n')
        && writeText(worktree + QStringLiteral("/.git"), ("gitdir: " + admin + '\n').toUtf8());
}

QVariantList worktreesOf(const ProjectWorkspaceRegistry &registry, int row)
{
    return registry.data(registry.index(row), ProjectWorkspaceRegistry::WorktreesRole).toList();
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

void ProjectWorkspaceRegistryTest::missingFolderStaysRemovable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString project = makeProject(dir, QStringLiteral("alpha"));
    prepareSchema(databasePath);

    {
        ProjectWorkspaceRegistry registry(databasePath);
        QVERIFY(registry.addProject(project));
        QVERIFY(registry.isAvailable(project));
        QVERIFY(QDir(project).removeRecursively());

        QSignalSpy changed(&registry, &QAbstractItemModel::dataChanged);
        registry.refreshFolders();
        QCOMPARE(changed.count(), 1);
        QVERIFY(!registry.isAvailable(project));
        QCOMPARE(registry.data(registry.index(0), ProjectWorkspaceRegistry::AvailableRole).toBool(), false);
        // Nothing changed since: no repaint.
        registry.refreshFolders();
        QCOMPARE(changed.count(), 1);
        // The folder is gone, yet the entry is still found and removable.
        QCOMPARE(registry.canonicalProjectPath(project), QString());
        QCOMPARE(registry.registeredPath(project), project);
    }

    // A restart keeps the entry, unavailable, until it is removed.
    ProjectWorkspaceRegistry reloaded(databasePath);
    QCOMPARE(reloaded.count(), 1);
    QVERIFY(!reloaded.isAvailable(project));
    QVERIFY(reloaded.removeProject(project));
    QCOMPARE(reloaded.count(), 0);
}

void ProjectWorkspaceRegistryTest::worktreeNestsUnderMainProject()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString main = makeProject(dir, QStringLiteral("app"));
    const QString worktree = makeProject(dir, QStringLiteral("worktrees/app/feature"));
    const QString other = makeProject(dir, QStringLiteral("worktrees/app/other"));
    QVERIFY(linkWorktree(main, worktree, QStringLiteral("feature"), "feature/login"));
    QVERIFY(linkWorktree(main, other, QStringLiteral("other"), "other"));
    prepareSchema(databasePath);

    {
        ProjectWorkspaceRegistry registry(databasePath);
        QVERIFY(registry.addProject(main));
        QVERIFY(registry.isGitRepository(main));
        // Existing worktrees are offered, never listed by themselves.
        QCOMPARE(registry.rowCount(), 1);
        QVERIFY(worktreesOf(registry, 0).isEmpty());
        QVariantList unlisted = registry.unlistedWorktrees(main);
        QCOMPARE(unlisted.size(), 2);
        QCOMPARE(unlisted.first().toMap().value(QStringLiteral("branch")).toString(), QStringLiteral("feature/login"));

        QVERIFY(registry.setExpanded(main, false));
        QVERIFY(registry.addProject(worktree));
        QCOMPARE(registry.rowCount(), 1);
        QCOMPARE(registry.count(), 2);
        QCOMPARE(registry.parentProject(worktree), main);
        QCOMPARE(registry.worktreePaths(main), QStringList{worktree});
        // The new child shows: its main project opens.
        QCOMPARE(registry.data(registry.index(0), ProjectWorkspaceRegistry::ExpandedRole).toBool(), true);
        const QVariantList children = worktreesOf(registry, 0);
        QCOMPARE(children.size(), 1);
        QCOMPARE(children.first().toMap().value(QStringLiteral("path")).toString(), worktree);
        unlisted = registry.unlistedWorktrees(main);
        QCOMPARE(unlisted.size(), 1);
        QCOMPARE(unlisted.first().toMap().value(QStringLiteral("path")).toString(), other);

        // Removing the child keeps its main project.
        QVERIFY(registry.removeProject(worktree));
        QCOMPARE(registry.count(), 1);
        QVERIFY(registry.addProject(worktree));
        QVERIFY(registry.addProject(other));
        QCOMPARE(registry.count(), 3);
    }

    {
        // Nesting survives a restart.
        ProjectWorkspaceRegistry reloaded(databasePath);
        QCOMPARE(reloaded.rowCount(), 1);
        QCOMPARE(reloaded.count(), 3);
        QCOMPARE(reloaded.parentProject(other), main);
        // Removing the main project drops the entries listed under it.
        QVERIFY(reloaded.removeProject(main));
        QCOMPARE(reloaded.count(), 0);
    }

    ProjectWorkspaceRegistry empty(databasePath);
    QCOMPARE(empty.count(), 0);
    // No folder was touched.
    QVERIFY(QFileInfo(worktree).isDir());
    QVERIFY(QFileInfo(main).isDir());
}

void ProjectWorkspaceRegistryTest::mainProjectAdoptsListedWorktrees()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("state.db"));
    const QString main = makeProject(dir, QStringLiteral("app"));
    const QString worktree = makeProject(dir, QStringLiteral("worktrees/app/feature"));
    QVERIFY(linkWorktree(main, worktree, QStringLiteral("feature"), "feature"));
    prepareSchema(databasePath);

    ProjectWorkspaceRegistry registry(databasePath);
    // Without its main project a worktree is a top-level project.
    QVERIFY(registry.addProject(worktree));
    QCOMPARE(registry.rowCount(), 1);
    QCOMPARE(registry.parentProject(worktree), QString());

    QVERIFY(registry.addProject(main));
    QCOMPARE(registry.rowCount(), 1);
    QCOMPARE(registry.count(), 2);
    QCOMPARE(registry.parentProject(worktree), main);
    QCOMPARE(registry.data(registry.index(0), ProjectWorkspaceRegistry::PathRole).toString(), main);
}

QTEST_MAIN(ProjectWorkspaceRegistryTest)
#include "project_workspace_registry_test.moc"
