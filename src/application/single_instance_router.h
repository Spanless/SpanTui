#pragma once

#include "platform/instance_lock.h"

#include <QObject>
#include <QString>

#include <atomic>

class QThread;

namespace span::application {

class SingleInstanceAcceptWorker;

class SingleInstanceRouter final : public QObject {
    Q_OBJECT

public:
    enum class StartResult {
        Primary,
        Forwarded,
        Rejected,
        Failed,
    };

    explicit SingleInstanceRouter(QObject *parent = nullptr,
                                  QString endpointOverride = {});
    ~SingleInstanceRouter() override;

    StartResult startOrForward(const QString &projectPath, QString *errorMessage = nullptr);
    QString endpointName() const;

signals:
    void activationRequested(const QString &projectPath);

private:
    enum class ForwardOutcome {
        Accepted,
        Rejected,
        Unreachable,
        Unacknowledged,
    };

    ForwardOutcome forward(const QString &projectPath, const QString &requestId,
                           int retryWindowMs, QString *errorMessage);
    bool becomePrimary(QString *errorMessage);
    void stopPrimary();

    SingleInstanceAcceptWorker *m_acceptWorker = nullptr;
    QThread *m_acceptThread = nullptr;
    // Held while this router is the primary.
    span::platform::InstanceLock m_primaryLock;
    QString m_endpointName;
    std::atomic_bool m_primaryListening{false};
};

} // namespace span::application
