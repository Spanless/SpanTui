#include "remote/ssh_askpass_platform.h"

#include <QFile>

#include <cerrno>
#include <cstring>

#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace span::remote::askpass_platform {

namespace {

constexpr qsizetype kMaxReply = 64 * 1024;

bool writeAll(int fd, const QByteArray &bytes)
{
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.constData() + offset, size_t(bytes.size() - offset));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += written;
    }
    return true;
}

} // namespace

quint32 parentProcessId()
{
    return quint32(getppid());
}

bool processParent(quint32 processId, quint32 *parent, QString *executableName)
{
    proc_bsdinfo info{};
    if (proc_pidinfo(pid_t(processId), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != int(sizeof(info))) {
        return false;
    }
    if (parent) *parent = info.pbi_ppid;
    if (executableName) {
        *executableName = QString::fromUtf8(info.pbi_name[0] ? info.pbi_name : info.pbi_comm);
    }
    return true;
}

QString promptArgument(int argc, char **argv)
{
    return argc >= 2 && argv ? QString::fromUtf8(argv[1]) : QString();
}

bool exchange(const QString &endpoint, const QByteArray &request, QByteArray *reply)
{
    const QByteArray path = QFile::encodeName(endpoint);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.isEmpty() || size_t(path.size()) >= sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, path.constData(), size_t(path.size()));
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    bool ok =
        ::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 && writeAll(fd, request);
    while (ok && !reply->contains('\n') && reply->size() < kMaxReply) {
        char buffer[4096];
        const ssize_t count = ::read(fd, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            break;
        }
        reply->append(buffer, qsizetype(count));
    }
    ::close(fd);
    return ok;
}

bool writeStandardOutput(const QByteArray &bytes)
{
    return writeAll(STDOUT_FILENO, bytes);
}

} // namespace span::remote::askpass_platform
