#pragma once

#include "core/operation_outcome.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

class GitRepositoryService;

namespace span::application {

class TaskProgressService;

enum class ProgressOwner {
    Git,
    GitHistory,
    GitDiff,
    Update,
    Files,
    Document,
    Remote,
};

QString taskProgressSource(ProgressOwner owner);
QString taskProgressSourceForGitOperation(const QString &operation);

// Adapts the shared owner progress contract to one stable task source. Keeping
// this mapping outside workspace startup makes every source/result route
// independently testable.
class TaskProgressBinding final : public QObject {
    Q_OBJECT

public:
    TaskProgressBinding(TaskProgressService *service, ProgressOwner owner,
                        QObject *parent = nullptr);
    TaskProgressBinding(TaskProgressService *service, QString source,
                        QObject *parent = nullptr);
    // Cancels every still-running source this binding started, so an owner
    // destroyed mid-operation can never leave a permanently spinning task.
    ~TaskProgressBinding() override;

    QString source() const;

public slots:
    void progressStarted(const QString &title);
    void progressChanged(qreal fraction, const QString &detail);
    void progressFinished(span::core::OperationOutcome outcome,
                          const QString &detail);
    void progressStartedOn(const QString &source, const QString &title);
    void progressFinishedOn(const QString &source,
                            span::core::OperationOutcome outcome,
                            const QString &detail);

private:
    void markStarted(const QString &source);
    void markFinished(const QString &source);
    QString resolveSource(const QString &source) const;

    QPointer<TaskProgressService> m_service;
    QString m_scopeSuffix;
    QString m_source;
    QSet<QString> m_activeSources;
};

// Routes an owner's progressStarted/progressChanged/progressFinished to one
// task source. The binding is parented to the owner: a destroyed owner leaves
// no running entry.
template <typename Owner> void bindProgress(TaskProgressService *service, Owner *owner, ProgressOwner source)
{
    auto *binding = new TaskProgressBinding(service, source, owner);
    QObject::connect(owner, &Owner::progressStarted, binding, &TaskProgressBinding::progressStarted);
    QObject::connect(owner, &Owner::progressChanged, binding, &TaskProgressBinding::progressChanged);
    QObject::connect(owner, &Owner::progressFinished, binding, &TaskProgressBinding::progressFinished);
}

// Git reports raw operation names; start and finish derive the task source
// from the same name, so they always pair. `lifetime` scopes the entries and
// ends the binding.
void bindGitProgress(TaskProgressService *service, GitRepositoryService *git, QObject *lifetime);

} // namespace span::application

Q_DECLARE_METATYPE(span::application::ProgressOwner)
