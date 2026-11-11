#include "application/task_progress_binding.h"

#include "application/task_progress.h"
#include "application/task_progress_titles.h"
#include "git/git_constants.h"
#include "git/git_repository_service.h"

namespace span::application {

namespace {
// Two owners (projects, editor tiles) share the same semantic source. Scoping
// every stored key by owner identity keeps independent operations from
// overwriting each other while `taskProgressSource()` stays the public name.
QString ownerScopeSuffix(const QObject *owner)
{
    if (!owner) {
        return {};
    }
    return QLatin1Char('#')
        + QString::number(reinterpret_cast<quintptr>(owner), 16);
}
} // namespace

QString taskProgressSource(ProgressOwner owner)
{
    switch (owner) {
    case ProgressOwner::Git:
        return QStringLiteral("git");
    case ProgressOwner::GitHistory:
        return QStringLiteral("git-history");
    case ProgressOwner::GitDiff:
        return QStringLiteral("git-diff");
    case ProgressOwner::Update:
        return QStringLiteral("update");
    case ProgressOwner::Files:
        return QStringLiteral("files");
    case ProgressOwner::Document:
        return QStringLiteral("document");
    case ProgressOwner::Remote:
        return QStringLiteral("remote");
    }
    return {};
}

QString taskProgressSourceForGitOperation(const QString &operation)
{
    if (operation == kGitOperationHistory || operation == kGitOperationHistoryParse
        || operation == kGitOperationBranches || operation == kGitOperationRefresh
        || operation == kGitOperationDiscover) {
        return taskProgressSource(ProgressOwner::GitHistory);
    }
    // One commit view: each step restarts the source, the last one closes it.
    if (operation == kGitOperationCommitDetails || operation == kGitOperationCommitFiles
        || operation == kGitOperationCommitFilesStats) {
        return taskProgressSource(ProgressOwner::GitDiff);
    }
    return taskProgressSource(ProgressOwner::Git);
}

TaskProgressBinding::TaskProgressBinding(TaskProgressService *service,
                                         ProgressOwner owner, QObject *parent)
    : TaskProgressBinding(service, taskProgressSource(owner), parent)
{
}

TaskProgressBinding::TaskProgressBinding(TaskProgressService *service,
                                         QString source, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_scopeSuffix(ownerScopeSuffix(parent))
    , m_source(source.trimmed().isEmpty()
                   ? QString()
                   : source.trimmed() + m_scopeSuffix)
{
}

QString TaskProgressBinding::resolveSource(const QString &source) const
{
    const QString trimmed = source.trimmed();
    return trimmed.isEmpty() ? m_source : trimmed + m_scopeSuffix;
}

TaskProgressBinding::~TaskProgressBinding()
{
    if (!m_service) {
        return;
    }
    const QSet<QString> active = m_activeSources;
    m_activeSources.clear();
    for (const QString &source : active) {
        m_service->finish(source, TaskProgressService::Cancelled, QString());
    }
}

void TaskProgressBinding::markStarted(const QString &source)
{
    if (!source.isEmpty()) {
        m_activeSources.insert(source);
    }
}

void TaskProgressBinding::markFinished(const QString &source)
{
    m_activeSources.remove(source);
}

QString TaskProgressBinding::source() const
{
    return m_source;
}

void TaskProgressBinding::progressStarted(const QString &title)
{
    if (m_service) {
        m_service->start(m_source, title);
        markStarted(m_source);
    }
}

void TaskProgressBinding::progressChanged(qreal fraction, const QString &detail)
{
    if (m_service)
        m_service->update(m_source, fraction, detail);
}

void TaskProgressBinding::progressFinished(span::core::OperationOutcome outcome,
                                           const QString &detail)
{
    progressFinishedOn(QString(), outcome, detail);
}

void TaskProgressBinding::progressStartedOn(const QString &source,
                                            const QString &title)
{
    if (!m_service) {
        return;
    }
    const QString key = resolveSource(source);
    m_service->start(key, title);
    markStarted(key);
}

void TaskProgressBinding::progressFinishedOn(const QString &source,
                                             span::core::OperationOutcome outcome,
                                             const QString &detail)
{
    if (!m_service)
        return;
    const QString key = resolveSource(source);
    const int result = outcome == span::core::OperationOutcome::Failed
        ? TaskProgressService::Failed
        : outcome == span::core::OperationOutcome::Cancelled
          ? TaskProgressService::Cancelled
          : TaskProgressService::Succeeded;
    m_service->finish(key, result, detail);
    markFinished(key);
}

void bindGitProgress(TaskProgressService *service, GitRepositoryService *git, QObject *lifetime)
{
    auto *binding = new TaskProgressBinding(service, QString(), lifetime);
    QObject::connect(git, &GitRepositoryService::taskStarted, binding, [binding](const QString &operation) {
        binding->progressStartedOn(taskProgressSourceForGitOperation(operation), gitProgressTitle(operation));
    });
    QObject::connect(git, &GitRepositoryService::taskFinished, binding,
                     [binding](const QString &operation, span::core::OperationOutcome outcome, const QString &detail) {
                         binding->progressFinishedOn(taskProgressSourceForGitOperation(operation), outcome, detail);
                     });
}

} // namespace span::application
