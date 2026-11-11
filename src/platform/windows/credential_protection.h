#pragma once

#include <QByteArray>
#include <QString>

namespace span::platform::credentials {

// Windows DPAPI (CryptProtectData) bound to the current user account: the
// blob is useless on another account or machine. Saved SSH passwords and key
// passphrases are stored only in this form.
QByteArray protectSecret(const QString &secret);
// Empty result and `ok == false` when the blob cannot be decrypted (other
// user, corrupted row).
QString unprotectSecret(const QByteArray &blob, bool *ok = nullptr);

// Writes `data` to `path` as a file only the current user can open: it is
// created fresh with a protected DACL holding a single ACE for the user's
// SID, so nothing inherited from the folder applies (what OpenSSH demands of
// a private key file). Replaces an existing file atomically.
bool writeOwnerOnlyFile(const QString &path, const QByteArray &data, QString *error);
// Renames `from` over `to` atomically. Within one folder the moved file keeps
// its own DACL, so a staged owner-only file stays owner-only.
bool replaceFile(const QString &from, const QString &to, QString *error);
// Creates the folder (and missing parents); a newly created last folder gets
// the same owner-only DACL.
bool makeOwnerOnlyDirectory(const QString &path, QString *error);

} // namespace span::platform::credentials
