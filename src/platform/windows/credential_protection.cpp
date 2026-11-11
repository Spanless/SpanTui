#include "platform/windows/credential_protection.h"

#include <QDir>
#include <QFileInfo>

#include <qt_windows.h>
#include <wincrypt.h>

#include <string>

namespace span::platform::credentials {
namespace {

// Separates SpanTui blobs from other DPAPI users of the same account.
constexpr char kEntropy[] = "SpanTui.remote-host-secret.v1";

DATA_BLOB entropyBlob()
{
    DATA_BLOB blob{};
    blob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(kEntropy));
    blob.cbData = DWORD(sizeof(kEntropy) - 1);
    return blob;
}

QString systemMessage(DWORD code)
{
    return qt_error_string(int(code));
}

// Security attributes granting full access to the current user only, with a
// protected DACL so inheritable ACEs of the parent folder are not merged in.
class OwnerOnlySecurity final {
public:
    OwnerOnlySecurity()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            m_error = GetLastError();
            return;
        }
        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        m_tokenUser.resize(needed);
        const bool haveUser = needed > 0
            && GetTokenInformation(token, TokenUser, m_tokenUser.data(), needed, &needed);
        m_error = haveUser ? ERROR_SUCCESS : GetLastError();
        CloseHandle(token);
        if (!haveUser) {
            return;
        }
        const auto *user = reinterpret_cast<const TOKEN_USER *>(m_tokenUser.data());
        const DWORD aclSize = DWORD(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE)
                                    + GetLengthSid(user->User.Sid));
        m_acl.resize(aclSize);
        auto *acl = reinterpret_cast<PACL>(m_acl.data());
        if (!InitializeAcl(acl, aclSize, ACL_REVISION)
            || !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, user->User.Sid)
            || !InitializeSecurityDescriptor(&m_descriptor, SECURITY_DESCRIPTOR_REVISION)
            || !SetSecurityDescriptorDacl(&m_descriptor, TRUE, acl, FALSE)
            || !SetSecurityDescriptorControl(&m_descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
            m_error = GetLastError();
            return;
        }
        m_attributes.nLength = sizeof(m_attributes);
        m_attributes.lpSecurityDescriptor = &m_descriptor;
        m_attributes.bInheritHandle = FALSE;
        m_ready = true;
    }

    OwnerOnlySecurity(const OwnerOnlySecurity &) = delete;
    OwnerOnlySecurity &operator=(const OwnerOnlySecurity &) = delete;

    SECURITY_ATTRIBUTES *attributes() { return m_ready ? &m_attributes : nullptr; }
    DWORD error() const { return m_error; }

private:
    QByteArray m_tokenUser;
    QByteArray m_acl;
    SECURITY_DESCRIPTOR m_descriptor{};
    SECURITY_ATTRIBUTES m_attributes{};
    DWORD m_error = ERROR_SUCCESS;
    bool m_ready = false;
};

} // namespace

bool writeOwnerOnlyFile(const QString &path, const QByteArray &data, QString *error)
{
    OwnerOnlySecurity security;
    if (!security.attributes()) {
        *error = systemMessage(security.error());
        return false;
    }
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const QString stagingPath = native + QStringLiteral(".new");
    const std::wstring staging = stagingPath.toStdWString();
    DeleteFileW(staging.c_str());
    // CREATE_NEW: the security descriptor only applies to a file this call
    // creates, never to one it would reuse.
    HANDLE file = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, security.attributes(),
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = systemMessage(GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(file, data.constData(), DWORD(data.size()), &written, nullptr)
        && written == DWORD(data.size()) && FlushFileBuffers(file);
    const DWORD writeError = GetLastError();
    CloseHandle(file);
    if (!wrote) {
        *error = systemMessage(writeError);
        DeleteFileW(staging.c_str());
        return false;
    }
    // A rename within the folder keeps the staged file's own DACL.
    if (!replaceFile(stagingPath, native, error)) {
        DeleteFileW(staging.c_str());
        return false;
    }
    return true;
}

bool replaceFile(const QString &from, const QString &to, QString *error)
{
    const std::wstring source = QDir::toNativeSeparators(QFileInfo(from).absoluteFilePath()).toStdWString();
    const std::wstring target = QDir::toNativeSeparators(QFileInfo(to).absoluteFilePath()).toStdWString();
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = systemMessage(GetLastError());
        return false;
    }
    return true;
}

bool makeOwnerOnlyDirectory(const QString &path, QString *error)
{
    const QFileInfo info(path);
    if (info.isDir()) {
        return true;
    }
    if (!QDir().mkpath(info.absolutePath())) {
        *error = QStringLiteral("Cannot create %1").arg(QDir::toNativeSeparators(info.absolutePath()));
        return false;
    }
    OwnerOnlySecurity security;
    if (!security.attributes()) {
        *error = systemMessage(security.error());
        return false;
    }
    const std::wstring native = QDir::toNativeSeparators(info.absoluteFilePath()).toStdWString();
    if (!CreateDirectoryW(native.c_str(), security.attributes())
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        *error = systemMessage(GetLastError());
        return false;
    }
    return true;
}

QByteArray protectSecret(const QString &secret)
{
    if (secret.isEmpty()) {
        return {};
    }
    QByteArray plain = secret.toUtf8();
    DATA_BLOB input{DWORD(plain.size()), reinterpret_cast<BYTE *>(plain.data())};
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB output{};
    const BOOL ok = CryptProtectData(&input, nullptr, &entropy, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &output);
    SecureZeroMemory(plain.data(), size_t(plain.size()));
    if (!ok) {
        return {};
    }
    const QByteArray blob(reinterpret_cast<const char *>(output.pbData),
                          qsizetype(output.cbData));
    LocalFree(output.pbData);
    return blob;
}

QString unprotectSecret(const QByteArray &blob, bool *ok)
{
    if (ok) {
        *ok = false;
    }
    if (blob.isEmpty()) {
        return {};
    }
    QByteArray copy = blob;
    DATA_BLOB input{DWORD(copy.size()), reinterpret_cast<BYTE *>(copy.data())};
    DATA_BLOB entropy = entropyBlob();
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return {};
    }
    const QString secret = QString::fromUtf8(reinterpret_cast<const char *>(output.pbData),
                                             qsizetype(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    if (ok) {
        *ok = true;
    }
    return secret;
}

} // namespace span::platform::credentials
