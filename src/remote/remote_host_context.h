#pragma once

#include "remote/ssh_command.h"

#include <QHash>
#include <QObject>

namespace span::remote {

class RemoteFileModel;
class RemoteSystemMonitor;

// Per-host runtime container, the remote counterpart of ProjectContext:
// created on first use by RemoteHostRegistry and owning the host's file
// browser (and through it the shell channel and transfer queue) and the
// server resource monitor that shares that channel.
class RemoteHostContext final : public QObject {
    Q_OBJECT
    Q_MOC_INCLUDE("remote/remote_file_model.h")
    Q_MOC_INCLUDE("remote/remote_system_monitor.h")
    Q_PROPERTY(QString hostId READ hostId CONSTANT FINAL)
    Q_PROPERTY(span::remote::RemoteFileModel *fileModel READ fileModel CONSTANT FINAL)
    Q_PROPERTY(span::remote::RemoteSystemMonitor *systemMonitor READ systemMonitor CONSTANT FINAL)

public:
    RemoteHostContext(QString hostId, const SshEndpoint &endpoint, const QHash<QString, QString> &environment,
                      QObject *parent = nullptr);
    ~RemoteHostContext() override;

    QString hostId() const { return m_hostId; }
    RemoteFileModel *fileModel() const { return m_fileModel; }
    RemoteSystemMonitor *systemMonitor() const { return m_systemMonitor; }

    // New endpoint after the host was edited (see RemoteFileModel::configure);
    // `keyChanged`: the key copy now holds another key.
    void reconfigure(const SshEndpoint &endpoint, const QHash<QString, QString> &environment, bool keyChanged);

private:
    QString m_hostId;
    RemoteFileModel *m_fileModel = nullptr;
    RemoteSystemMonitor *m_systemMonitor = nullptr;
};

} // namespace span::remote
