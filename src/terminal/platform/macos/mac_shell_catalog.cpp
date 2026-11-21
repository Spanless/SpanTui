#include "terminal/api/shell_catalog.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace span::terminal {

namespace {

// The shells the system lists as login shells, one absolute path per line.
constexpr QLatin1StringView kShellsFile{"/etc/shells"};
// /bin/sh is bash in POSIX mode, not a shell of its own.
constexpr QLatin1StringView kPosixShell{"/bin/sh"};

QStringList listedShells()
{
    QFile file{QString(kShellsFile)};
    QStringList shells;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return shells;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('/')) && line != kPosixShell) shells << line;
    }
    return shells;
}

} // namespace

QVector<ShellCandidate> detectShells(const QString &)
{
    // The user's login shell first: it is the fallback default.
    QStringList paths;
    const QString login = qEnvironmentVariable("SHELL");
    if (!login.isEmpty()) paths << login;
    paths << listedShells();

    QVector<ShellCandidate> shells;
    QSet<QString> seenPaths;
    QSet<QString> seenIds;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (seenPaths.contains(info.absoluteFilePath()) || !info.isFile() || !info.isExecutable()) continue;
        seenPaths.insert(info.absoluteFilePath());
        // "zsh"; a second install of the same shell (Homebrew's bash) is
        // told apart by its path.
        const QString base = info.fileName();
        const bool duplicate = seenIds.contains(base);
        const QString id = duplicate ? info.absoluteFilePath() : base;
        seenIds.insert(id);
        QString name = base;
        name[0] = name.at(0).toUpper();
        if (duplicate) name += QStringLiteral(" (%1)").arg(info.absolutePath());
        // A login shell, as Terminal starts it.
        shells.push_back({id, name, info.absoluteFilePath(), info.absoluteFilePath(), QStringLiteral("-l")});
    }
    return shells;
}

QString shellCommandLine(const QString &executable, const QString &arguments)
{
    const QString command = QLatin1Char('"') + executable + QLatin1Char('"');
    const QString rest = arguments.trimmed();
    return rest.isEmpty() ? command : command + QLatin1Char(' ') + rest;
}

} // namespace span::terminal
