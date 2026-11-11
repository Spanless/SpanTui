#include "remote/remote_host_context.h"

#include "remote/remote_file_model.h"
#include "remote/remote_system_monitor.h"

namespace span::remote {

RemoteHostContext::RemoteHostContext(QString hostId, const SshEndpoint &endpoint,
                                     const QHash<QString, QString> &environment, QObject *parent)
    : QObject(parent),
      m_hostId(std::move(hostId)),
      m_fileModel(new RemoteFileModel(this))
{
    m_fileModel->configure(endpoint, environment);
    m_systemMonitor = new RemoteSystemMonitor(m_fileModel->channel(), this);
}

RemoteHostContext::~RemoteHostContext() = default;

void RemoteHostContext::reconfigure(const SshEndpoint &endpoint, const QHash<QString, QString> &environment,
                                    bool keyChanged)
{
    m_fileModel->configure(endpoint, environment, keyChanged);
}

} // namespace span::remote
