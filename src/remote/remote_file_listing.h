#pragma once

#include "remote/remote_constants.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace span::remote {

struct RemoteFileEntry {
    QString name;
    QString path;
    bool isDir = false;
    bool isLink = false;
    qint64 size = 0;
    qint64 modifiedSecs = 0;
    QString permissions;
    QString owner;
    QString group;
};

struct RemoteListing {
    // The shell's $PWD after entering the requested directory.
    QString directory;
    QVector<RemoteFileEntry> entries;
};

// Remote shell scripts. Paths are POSIX absolute paths, quoted as single
// shell words; every script reports failures through its exit status and
// prints the remote error text on stdout.
QString remoteHomeScript();
// GNU find listing: "$PWD\0" followed by one NUL-terminated record per entry.
QString remoteListScript(const QString &directory);
QString remoteMakeDirectoryScript(const QString &path);
QString remoteCreateFileScript(const QString &path);
QString remoteRenameScript(const QString &from, const QString &to);
QString remoteRemoveScript(const QStringList &paths);

bool parseRemoteListing(const QByteArray &payload, RemoteListing *listing);

QString remoteParentPath(const QString &path);
QString remoteChildPath(const QString &directory, const QString &name);
// A single path segment: not empty, not "." or "..", no '/' or NUL.
bool isValidRemoteName(const QString &name);
// Absolute, normalized and never the filesystem root — the only paths the
// browser will hand to rm -rf.
bool isRemovableRemotePath(const QString &path);
// Collapses "//", "/./" and "/../" in an absolute path the user typed.
QString normalizeRemotePath(const QString &path);

} // namespace span::remote
