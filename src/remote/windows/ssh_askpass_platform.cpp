#include "remote/ssh_askpass_platform.h"

#include <qt_windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <string>

namespace span::remote::askpass_platform {

namespace {

constexpr qsizetype kMaxReply = 64 * 1024;
constexpr int kBusyAttempts = 20;
constexpr DWORD kBusyWaitMs = 500;

bool writeAll(HANDLE handle, const QByteArray &bytes)
{
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes.constData() + offset, DWORD(bytes.size() - offset), &written, nullptr)
            || written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

} // namespace

quint32 parentProcessId()
{
    quint32 parent = 0;
    return processParent(GetCurrentProcessId(), &parent, nullptr) ? parent : 0;
}

bool processParent(quint32 processId, quint32 *parent, QString *executableName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == processId) {
            if (parent) *parent = entry.th32ParentProcessID;
            if (executableName) *executableName = QString::fromWCharArray(entry.szExeFile);
            found = true;
            break;
        }
    }
    CloseHandle(snapshot);
    return found;
}

QString promptArgument(int, char **)
{
    int count = 0;
    LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) {
        return {};
    }
    const QString prompt = count >= 2 ? QString::fromWCharArray(arguments[1]) : QString();
    LocalFree(arguments);
    return prompt;
}

bool exchange(const QString &endpoint, const QByteArray &request, QByteArray *reply)
{
    const std::wstring name = endpoint.toStdWString();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < kBusyAttempts; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
            break;
        }
        WaitNamedPipeW(name.c_str(), kBusyWaitMs);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = writeAll(pipe, request);
    while (ok && !reply->contains('\n') && reply->size() < kMaxReply) {
        char buffer[4096];
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            ok = false;
            break;
        }
        reply->append(buffer, qsizetype(read));
    }
    CloseHandle(pipe);
    return ok;
}

bool writeStandardOutput(const QByteArray &bytes)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    return output != nullptr && output != INVALID_HANDLE_VALUE && writeAll(output, bytes);
}

} // namespace span::remote::askpass_platform
