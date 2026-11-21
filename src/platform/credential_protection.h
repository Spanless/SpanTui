#pragma once

#include <QByteArray>
#include <QString>

namespace span::platform::credentials {

// Saved SSH passwords and key passphrases are stored only in this form.
//   Windows: DPAPI (CryptProtectData) bound to the current user account; the
//   blob is useless on another account or machine.
//   macOS: AES-256 + HMAC-SHA256 under a random key in the data root's
//   owner-only ProductPaths::credentialKeyPath(); the blob is useless without
//   that file.
QByteArray protectSecret(const QString &secret);
// Empty result and `ok == false` when the blob cannot be decrypted (other
// user, corrupted row).
QString unprotectSecret(const QByteArray &blob, bool *ok = nullptr);

// Writes `data` to `path` as a file only the current user can open (what
// OpenSSH demands of a private key file): Windows creates it fresh with a
// protected DACL holding a single ACE for the user's SID, so nothing inherited
// from the folder applies; POSIX creates it with mode 0600. Replaces an
// existing file atomically.
bool writeOwnerOnlyFile(const QString &path, const QByteArray &data, QString *error);
// Renames `from` over `to` atomically. Within one folder the moved file keeps
// its own permissions, so a staged owner-only file stays owner-only.
bool replaceFile(const QString &from, const QString &to, QString *error);
// Creates the folder (and missing parents); a newly created last folder gets
// owner-only permissions (the DACL above; mode 0700 on POSIX).
bool makeOwnerOnlyDirectory(const QString &path, QString *error);

} // namespace span::platform::credentials
