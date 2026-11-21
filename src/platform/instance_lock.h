#pragma once

#include <QString>

#include <memory>

namespace span::platform {

// An exclusive lock named `name` that at most one process of this user holds
// at a time. The kernel drops it with the owning process, so a crashed owner
// never blocks the next start.
//   Windows: a named mutex in the session namespace (Local\<name>).
//   macOS: flock() on <endpoint address>.lock (platform/local_endpoint.h).
class InstanceLock final {
public:
    InstanceLock();
    ~InstanceLock();

    InstanceLock(const InstanceLock &) = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;

    // False while another process holds it.
    bool tryAcquire(const QString &name);
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace span::platform
