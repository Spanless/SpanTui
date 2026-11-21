#include "platform/credential_protection.h"

#include "core/product_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <mutex>

#include <CommonCrypto/CommonCrypto.h>
#include <Security/SecRandom.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Saved secrets are AES-256-CBC encrypted and HMAC-SHA256 authenticated
// (encrypt-then-MAC) under a random key kept in an owner-only file of the data
// root. The login Keychain would bind the key to the account more tightly, but
// without a signing identity every rebuilt binary has a new code signature and
// the Keychain asks the user again, which a headless test cannot answer.
namespace span::platform::credentials {
namespace {

constexpr char kMagic[] = "SPKF1";
constexpr int kMagicSize = sizeof(kMagic) - 1;
constexpr int kCipherKeyBytes = kCCKeySizeAES256;
constexpr int kMacKeyBytes = CC_SHA256_DIGEST_LENGTH;
constexpr int kKeyFileBytes = kCipherKeyBytes + kMacKeyBytes;
constexpr int kIvBytes = kCCBlockSizeAES128;
constexpr int kMacBytes = CC_SHA256_DIGEST_LENGTH;

QString errnoMessage(int code)
{
    return QString::fromLocal8Bit(std::strerror(code));
}

void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}

bool randomBytes(void *out, size_t size)
{
    return SecRandomCopyBytes(kSecRandomDefault, size, out) == errSecSuccess;
}

bool writeAll(int fd, const QByteArray &data)
{
    const char *cursor = data.constData();
    qsizetype left = data.size();
    while (left > 0) {
        const ssize_t written = ::write(fd, cursor, size_t(left));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        cursor += written;
        left -= written;
    }
    return true;
}

// The key of the current data root, created on first use. Cached per root:
// --data-dir and tests switch roots within one process.
QByteArray masterKey()
{
    static std::mutex mutex;
    static QString cachedPath;
    static QByteArray cachedKey;
    const std::lock_guard<std::mutex> lock(mutex);
    const QString path = span::core::ProductPaths::credentialKeyPath();
    if (path == cachedPath && cachedKey.size() == kKeyFileBytes) return cachedKey;

    // Only a missing key is created. One that exists but cannot be read
    // (permissions) or is damaged still encrypts what is saved; replacing it
    // would lose every saved secret, so it stays for inspection.
    if (QFileInfo::exists(path)) {
        QFile existing(path);
        const QByteArray key = existing.open(QIODevice::ReadOnly) ? existing.readAll() : QByteArray();
        if (key.size() != kKeyFileBytes) return {};
        cachedPath = path;
        cachedKey = key;
        return key;
    }
    QByteArray key(kKeyFileBytes, Qt::Uninitialized);
    QString ignored;
    if (!randomBytes(key.data(), size_t(key.size()))
        || !makeOwnerOnlyDirectory(QFileInfo(path).absolutePath(), &ignored)
        || !writeOwnerOnlyFile(path, key, &ignored)) {
        return {};
    }
    cachedPath = path;
    cachedKey = key;
    return key;
}

QByteArray hmac(const QByteArray &key, const QByteArray &data)
{
    QByteArray mac(kMacBytes, Qt::Uninitialized);
    CCHmac(kCCHmacAlgSHA256, key.constData() + kCipherKeyBytes, kMacKeyBytes, data.constData(), size_t(data.size()),
           mac.data());
    return mac;
}

} // namespace

QByteArray protectSecret(const QString &secret)
{
    if (secret.isEmpty()) return {};
    const QByteArray key = masterKey();
    if (key.isEmpty()) return {};
    const QByteArray plain = secret.toUtf8();
    QByteArray iv(kIvBytes, Qt::Uninitialized);
    if (!randomBytes(iv.data(), size_t(iv.size()))) return {};
    QByteArray cipher(plain.size() + kCCBlockSizeAES128, Qt::Uninitialized);
    size_t cipherSize = 0;
    if (CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key.constData(), kCipherKeyBytes, iv.constData(),
                plain.constData(), size_t(plain.size()), cipher.data(), size_t(cipher.size()), &cipherSize)
        != kCCSuccess) {
        return {};
    }
    cipher.truncate(qsizetype(cipherSize));
    QByteArray blob = QByteArray(kMagic, kMagicSize) + iv + cipher;
    blob += hmac(key, blob);
    return blob;
}

QString unprotectSecret(const QByteArray &blob, bool *ok)
{
    if (ok) *ok = false;
    if (blob.size() < kMagicSize + kIvBytes + kCCBlockSizeAES128 + kMacBytes
        || !blob.startsWith(QByteArray::fromRawData(kMagic, kMagicSize))) {
        return {};
    }
    const QByteArray key = masterKey();
    if (key.isEmpty()) return {};
    const QByteArray signedPart = blob.left(blob.size() - kMacBytes);
    const QByteArray mac = blob.right(kMacBytes);
    const QByteArray expected = hmac(key, signedPart);
    if (timingsafe_bcmp(mac.constData(), expected.constData(), size_t(kMacBytes)) != 0) return {};
    const QByteArray iv = signedPart.mid(kMagicSize, kIvBytes);
    const QByteArray cipher = signedPart.mid(kMagicSize + kIvBytes);
    QByteArray plain(cipher.size(), Qt::Uninitialized);
    size_t plainSize = 0;
    if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key.constData(), kCipherKeyBytes, iv.constData(),
                cipher.constData(), size_t(cipher.size()), plain.data(), size_t(plain.size()), &plainSize)
        != kCCSuccess) {
        return {};
    }
    plain.truncate(qsizetype(plainSize));
    if (ok) *ok = true;
    return QString::fromUtf8(plain);
}

bool writeOwnerOnlyFile(const QString &path, const QByteArray &data, QString *error)
{
    const QString staging =
        path + QStringLiteral(".") + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".tmp");
    const QByteArray stagingName = QFile::encodeName(staging);
    // Created 0600 regardless of the umask's group/other bits.
    const int fd = ::open(stagingName.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        setError(error, errnoMessage(errno));
        return false;
    }
    const bool written = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0 && writeAll(fd, data) && ::fsync(fd) == 0;
    const int writeErrno = errno;
    ::close(fd);
    if (!written) {
        ::unlink(stagingName.constData());
        setError(error, errnoMessage(writeErrno));
        return false;
    }
    if (!replaceFile(staging, path, error)) {
        ::unlink(stagingName.constData());
        return false;
    }
    return true;
}

bool replaceFile(const QString &from, const QString &to, QString *error)
{
    if (::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) != 0) {
        setError(error, errnoMessage(errno));
        return false;
    }
    return true;
}

bool makeOwnerOnlyDirectory(const QString &path, QString *error)
{
    const QFileInfo info(path);
    if (info.isDir()) return true;
    if (!QDir().mkpath(info.absolutePath())) {
        setError(error, QStringLiteral("Cannot create %1").arg(QDir::toNativeSeparators(info.absolutePath())));
        return false;
    }
    if (::mkdir(QFile::encodeName(path).constData(), S_IRWXU) != 0 && errno != EEXIST) {
        setError(error, errnoMessage(errno));
        return false;
    }
    return true;
}

} // namespace span::platform::credentials
