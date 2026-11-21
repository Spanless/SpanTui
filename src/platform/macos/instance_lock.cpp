#include "platform/instance_lock.h"

#include "platform/local_endpoint.h"

#include <QFile>

#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace span::platform {

struct InstanceLock::Impl {
    int fd = -1;
};

InstanceLock::InstanceLock() : m_impl(std::make_unique<Impl>()) {}

InstanceLock::~InstanceLock()
{
    release();
}

bool InstanceLock::tryAcquire(const QString &name)
{
    if (m_impl->fd >= 0) return true;
    // The file stays: removing it would let a second process lock a new one
    // while the first still holds the old.
    const QByteArray path = QFile::encodeName(localEndpointAddress(name) + QStringLiteral(".lock"));
    const int fd = ::open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    m_impl->fd = fd;
    return true;
}

void InstanceLock::release()
{
    if (m_impl->fd >= 0) ::close(std::exchange(m_impl->fd, -1));
}

} // namespace span::platform
