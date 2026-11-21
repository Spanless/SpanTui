#include "platform/instance_lock.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <utility>

namespace span::platform {

struct InstanceLock::Impl {
    HANDLE mutex = nullptr;
};

InstanceLock::InstanceLock() : m_impl(std::make_unique<Impl>()) {}

InstanceLock::~InstanceLock()
{
    release();
}

bool InstanceLock::tryAcquire(const QString &name)
{
    if (m_impl->mutex) return true;
    const std::wstring lockName = (QStringLiteral("Local\\") + name).toStdWString();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, lockName.c_str());
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return false;
    }
    m_impl->mutex = mutex;
    return true;
}

void InstanceLock::release()
{
    if (m_impl->mutex) CloseHandle(std::exchange(m_impl->mutex, nullptr));
}

} // namespace span::platform
