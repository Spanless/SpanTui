#include "git/git_worktree_commands.h"

#include "core/external_tools.h"
#include "git/git_lane_scheduler.h"
#include "git/git_repository_service.h"
#include "git/git_worktree_layout.h"

#include <QDir>
#include <QFileInfo>

#include <memory>

namespace {

QStringList commonArguments(const QString &repository)
{
    return {QStringLiteral("-c"), QStringLiteral("core.quotepath=false"), QStringLiteral("-C"), repository};
}

QProcess *startGit(QObject *parent, const QString &repository, const QStringList &arguments)
{
    auto *process = new QProcess(parent);
    process->setProcessEnvironment(GitLaneScheduler::environment());
    process->setProcessChannelMode(QProcess::SeparateChannels);
    // Always an absolute path (never a bare "git"); empty while Git is
    // unavailable, which fails the start.
    process->start(span::core::toolExecutable(span::core::ExternalTool::Git), commonArguments(repository) + arguments,
                   QIODevice::ReadOnly);
    return process;
}

QString errorText(QProcess *process)
{
    const QString message = QString::fromUtf8(process->readAllStandardError()).trimmed();
    if (!message.isEmpty()) {
        return message;
    }
    return process->error() == QProcess::FailedToStart
        ? process->errorString()
        : QStringLiteral("git exited with code %1").arg(process->exitCode());
}

bool usableRepository(const QString &repository)
{
    return !repository.isEmpty() && QFileInfo(repository).isDir();
}

} // namespace

GitWorktreeCommands::GitWorktreeCommands(QObject *parent) : QObject(parent) {}

GitWorktreeCommands::~GitWorktreeCommands() = default;

bool GitWorktreeCommands::busy() const
{
    return m_write != nullptr;
}

bool GitWorktreeCommands::add(const QString &repository, const QString &branch, const QString &base,
                              const QString &path, Completion completion)
{
    if (!usableRepository(repository) || !span::git::isValidBranchName(branch)
        || !GitRepositoryService::isSafeGitRef(base) || !QDir::isAbsolutePath(path)) {
        return false;
    }
    return startWrite(repository,
                      {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"), branch,
                       QStringLiteral("--"), QDir::toNativeSeparators(QDir::cleanPath(path)), base},
                      tr("Creating worktree"), std::move(completion));
}

bool GitWorktreeCommands::remove(const QString &repository, const QString &path, bool force, Completion completion)
{
    if (!usableRepository(repository) || !QDir::isAbsolutePath(path)) {
        return false;
    }
    QStringList arguments{QStringLiteral("worktree"), QStringLiteral("remove")};
    if (force) {
        arguments << QStringLiteral("--force");
    }
    arguments << QStringLiteral("--") << QDir::toNativeSeparators(QDir::cleanPath(path));
    return startWrite(repository, arguments, tr("Deleting worktree"), std::move(completion));
}

bool GitWorktreeCommands::startWrite(const QString &repository, const QStringList &arguments, const QString &title,
                                     Completion completion)
{
    if (m_write) {
        return false;
    }
    emit progressStarted(title);
    QProcess *process = startGit(this, repository, arguments);
    m_write = process;
    emit busyChanged();
    const auto finish = [this, process, completion = std::move(completion)]() {
        if (m_write != process) {
            return;
        }
        const bool ok = process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0
            && process->error() != QProcess::FailedToStart;
        const QString error = ok ? QString() : errorText(process);
        m_write = nullptr;
        process->deleteLater();
        emit progressFinished(ok ? span::core::OperationOutcome::Succeeded : span::core::OperationOutcome::Failed,
                              error);
        emit busyChanged();
        if (completion) {
            completion(ok, error);
        }
    };
    connect(process, &QProcess::finished, this, finish);
    connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
        // finished() follows every other error.
        if (error == QProcess::FailedToStart && process->state() == QProcess::NotRunning) {
            finish();
        }
    });
    return true;
}

void GitWorktreeCommands::listBranches(const QString &repository, BranchCompletion completion)
{
    if (!usableRepository(repository)) {
        if (completion) {
            completion({}, {});
        }
        return;
    }
    QProcess *process =
        startGit(this, repository,
                 {QStringLiteral("for-each-ref"), QStringLiteral("--format=%(HEAD)%09%(refname)"),
                  QStringLiteral("--sort=refname"), QStringLiteral("refs/heads"), QStringLiteral("refs/remotes")});
    auto done = std::make_shared<bool>(false);
    const auto finish = [process, done, completion = std::move(completion)](bool ok) {
        if (*done) {
            return;
        }
        *done = true;
        QString current;
        const QStringList branches = ok ? parseBranches(process->readAllStandardOutput(), &current) : QStringList();
        process->deleteLater();
        if (completion) {
            completion(branches, current);
        }
    };
    connect(process, &QProcess::finished, this,
            [process, finish]() { finish(process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0); });
    connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && process->state() == QProcess::NotRunning) {
            finish(false);
        }
    });
}

QStringList GitWorktreeCommands::parseBranches(const QByteArray &output, QString *current)
{
    QStringList local;
    QStringList remote;
    const QLatin1String heads("refs/heads/");
    const QLatin1String remotes("refs/remotes/");
    for (const QByteArray &rawLine : output.split('\n')) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        const qsizetype tab = line.indexOf(QLatin1Char('\t'));
        const QString ref = tab < 0 ? line : line.mid(tab + 1);
        if (ref.startsWith(heads)) {
            const QString name = ref.mid(heads.size());
            local.append(name);
            if (current && line.startsWith(QLatin1Char('*'))) {
                *current = name;
            }
        } else if (ref.startsWith(remotes) && !ref.endsWith(QLatin1String("/HEAD"))) {
            remote.append(ref.mid(remotes.size()));
        }
    }
    return local + remote;
}
