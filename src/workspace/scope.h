#pragma once

#include <QLatin1StringView>
#include <QString>

namespace span::workspace {

// Sessions and tiles belong to a scope: a local project or a saved SSH host.
// The stored key is the project's canonical path, or "remote:<host id>". The
// prefix can never start a Windows absolute path (drive letters are one
// character), so the two spaces cannot collide.
class Scope final {
public:
    enum class Kind { None, Project, RemoteHost };

    Scope() = default;

    static Scope project(const QString &path) { return path.isEmpty() ? Scope() : Scope(Kind::Project, path); }
    static Scope remoteHost(const QString &hostId)
    {
        return hostId.isEmpty() ? Scope() : Scope(Kind::RemoteHost, hostId);
    }
    static Scope fromKey(const QString &key)
    {
        return key.startsWith(kRemotePrefix) ? remoteHost(key.mid(kRemotePrefix.size())) : project(key);
    }

    bool isEmpty() const { return m_kind == Kind::None; }
    bool isProject() const { return m_kind == Kind::Project; }
    bool isRemoteHost() const { return m_kind == Kind::RemoteHost; }
    // "" unless the scope is a project.
    QString projectPath() const { return isProject() ? m_id : QString(); }
    // "" unless the scope is a remote host.
    QString hostId() const { return isRemoteHost() ? m_id : QString(); }
    QString key() const { return isRemoteHost() ? kRemotePrefix + m_id : m_id; }

    friend bool operator==(const Scope &a, const Scope &b) { return a.m_kind == b.m_kind && a.m_id == b.m_id; }
    friend bool operator!=(const Scope &a, const Scope &b) { return !(a == b); }

private:
    static constexpr QLatin1StringView kRemotePrefix{"remote:"};

    Scope(Kind kind, const QString &id) : m_kind(kind), m_id(id) {}

    Kind m_kind = Kind::None;
    QString m_id;
};

} // namespace span::workspace
