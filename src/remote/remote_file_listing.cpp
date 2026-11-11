#include "remote/remote_file_listing.h"

#include "remote/ssh_command.h"

namespace span::remote {

QString remoteHomeScript()
{
    return QStringLiteral("printf '%s' \"${HOME:-/}\"");
}

QString remoteListScript(const QString &directory)
{
    // A find without -printf (not GNU) fails the probe on "." alone, which
    // needs no read permission; the listing itself can then still fail, for
    // a directory that may be entered but not read (mode 711), with find's
    // own error after the directory record.
    return QStringLiteral(
               "cd %1 2>&1 || exit %2\n"
               "printf '%s\\0' \"$PWD\"\n"
               "LC_ALL=C find . -maxdepth 0 -printf '' 2>/dev/null || exit %3\n"
               "LC_ALL=C find . -mindepth 1 -maxdepth 1 "
               "-printf '%y\\t%Y\\t%s\\t%T@\\t%M\\t%u\\t%g\\t%P\\0' 2>&1 || exit %4")
        .arg(shellQuote(directory), QString::number(kListEnterFailedExitCode),
             QString::number(kListMissingFindExitCode), QString::number(kListReadFailedExitCode));
}

QString remoteMakeDirectoryScript(const QString &path)
{
    return QStringLiteral("mkdir %1 2>&1").arg(shellQuote(path));
}

QString remoteCreateFileScript(const QString &path)
{
    // Multi-argument arg(): a name containing "%2" is never substituted again.
    return QStringLiteral("if [ -e %1 ] || [ -L %1 ]; then exit %2; fi\n: > %1 2>&1")
        .arg(shellQuote(path), QString::number(kRemoteExistsExitCode));
}

QString remoteRenameScript(const QString &from, const QString &to)
{
    return QStringLiteral("if [ -e %1 ] || [ -L %1 ]; then exit %2; fi\nmv -- %3 %1 2>&1")
        .arg(shellQuote(to), QString::number(kRemoteExistsExitCode), shellQuote(from));
}

QString remoteRemoveScript(const QStringList &paths)
{
    QStringList quoted;
    for (const QString &path : paths) {
        quoted << shellQuote(path);
    }
    return QStringLiteral("rm -rf -- %1 2>&1").arg(quoted.join(QLatin1Char(' ')));
}

bool parseRemoteListing(const QByteArray &payload, RemoteListing *listing)
{
    // The directory record is NUL-terminated: shell error text such as dash's
    // "/bin/sh: 1: cd: can't cd to …" also starts with '/', but has no NUL.
    const QList<QByteArray> records = payload.split('\0');
    if (records.size() < 2 || records.constFirst().isEmpty()
        || !records.constFirst().startsWith('/')) {
        return false;
    }
    RemoteListing result;
    result.directory = QString::fromUtf8(records.constFirst());
    for (qsizetype i = 1; i < records.size(); ++i) {
        const QByteArray &record = records.at(i);
        if (record.isEmpty()) {
            continue;
        }
        // Seven tab-separated fields, then the name (which may itself hold
        // a tab, so it is everything after the seventh separator).
        qsizetype from = 0;
        QList<QByteArray> fields;
        for (int field = 0; field < 7; ++field) {
            const qsizetype tab = record.indexOf('\t', from);
            if (tab < 0) {
                break;
            }
            fields << record.mid(from, tab - from);
            from = tab + 1;
        }
        if (fields.size() != 7 || from >= record.size()) {
            continue;
        }
        RemoteFileEntry entry;
        entry.name = QString::fromUtf8(record.mid(from));
        entry.path = remoteChildPath(result.directory, entry.name);
        entry.isLink = fields.at(0) == "l";
        entry.isDir = fields.at(0) == "d" || (entry.isLink && fields.at(1) == "d");
        entry.size = fields.at(2).toLongLong();
        entry.modifiedSecs = qint64(fields.at(3).toDouble());
        entry.permissions = QString::fromLatin1(fields.at(4));
        entry.owner = QString::fromUtf8(fields.at(5));
        entry.group = QString::fromUtf8(fields.at(6));
        result.entries.push_back(std::move(entry));
    }
    if (listing) {
        *listing = std::move(result);
    }
    return true;
}

QString remoteParentPath(const QString &path)
{
    const QString normalized = normalizeRemotePath(path);
    const qsizetype slash = normalized.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : normalized.left(slash);
}

QString remoteChildPath(const QString &directory, const QString &name)
{
    return directory.endsWith(QLatin1Char('/')) ? directory + name
                                                : directory + QLatin1Char('/') + name;
}

bool isValidRemoteName(const QString &name)
{
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..")
        && !name.contains(QLatin1Char('/')) && !name.contains(QChar(0));
}

bool isRemovableRemotePath(const QString &path)
{
    return path.startsWith(QLatin1Char('/')) && !path.contains(QChar(0))
        && normalizeRemotePath(path) == path && path != QLatin1String("/");
}

QString normalizeRemotePath(const QString &path)
{
    QStringList parts;
    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        if (segment == QLatin1String(".")) {
            continue;
        }
        if (segment == QLatin1String("..")) {
            if (!parts.isEmpty()) {
                parts.removeLast();
            }
            continue;
        }
        parts << segment;
    }
    return QLatin1Char('/') + parts.join(QLatin1Char('/'));
}

} // namespace span::remote
