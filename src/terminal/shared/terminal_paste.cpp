#include "terminal/shared/terminal_paste.h"

#include "core/product_paths.h"
#include "terminal/terminal_constants.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

namespace span::terminal {

namespace {

QString firstWord(const QString &commandLine)
{
    const QString trimmed = commandLine.trimmed();
    if (trimmed.startsWith(QLatin1Char('"'))) {
        const qsizetype end = trimmed.indexOf(QLatin1Char('"'), 1);
        return end < 0 ? trimmed.mid(1) : trimmed.mid(1, end - 1);
    }
    const qsizetype end = trimmed.indexOf(QRegularExpression(QStringLiteral("\\s")));
    return end < 0 ? trimmed : trimmed.left(end);
}

QString programName(const QString &commandLine)
{
    QString word = firstWord(commandLine);
    word.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QString name = word.mid(word.lastIndexOf(QLatin1Char('/')) + 1).toLower();
    if (name.endsWith(QLatin1String(".exe"))) name.chop(4);
    return name;
}

bool posixSafe(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || QStringView(u"_@%+=:,./-").contains(c);
}

QString quoteSingle(const QString &text, const QString &escapedQuote)
{
    QString quoted = text;
    quoted.replace(QLatin1Char('\''), escapedQuote);
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// Bare in PowerShell and cmd: what neither splits, expands nor treats as an
// operator (',' builds an array and a leading '@' splats in PowerShell; '%'
// expands in cmd).
bool windowsShellSafe(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || QStringView(u"_.:/\\-").contains(c);
}

// PowerShell also ends a single-quoted string at the typographic single
// quotes, so each of them is doubled as well.
QString powerShellQuote(const QString &path)
{
    QString quoted;
    quoted.reserve(path.size() + 2);
    quoted += QLatin1Char('\'');
    for (const QChar c : path) {
        quoted += c;
        if (QStringView(u"'\u2018\u2019\u201A\u201B").contains(c)) quoted += c;
    }
    return quoted + QLatin1Char('\'');
}

// Windows paths cannot hold '"'. '%' still expands between double quotes,
// so it leaves them as ^%, which escapes it.
QString cmdQuote(const QString &path)
{
    QString quoted = path;
    quoted.replace(QLatin1Char('%'), QStringLiteral("\"^%\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

QString posixQuote(const QString &path)
{
    if (!path.isEmpty() && std::all_of(path.cbegin(), path.cend(), posixSafe)) return path;
    return quoteSingle(path, QStringLiteral("'\\''"));
}

// C:\Users\a -> /mnt/c/Users/a; \\wsl$\Ubuntu\home\a -> /home/a.
QString wslPath(const QString &path)
{
    QString unix = path;
    unix.replace(QLatin1Char('\\'), QLatin1Char('/'));
    for (const QLatin1StringView prefix : {QLatin1StringView("//wsl$/"), QLatin1StringView("//wsl.localhost/")}) {
        if (unix.startsWith(prefix, Qt::CaseInsensitive)) {
            const qsizetype rest = unix.indexOf(QLatin1Char('/'), prefix.size());
            return rest < 0 ? QStringLiteral("/") : unix.mid(rest);
        }
    }
    if (unix.size() >= 2 && unix.at(1) == QLatin1Char(':') && unix.at(0).isLetter())
        return QStringLiteral("/mnt/") + unix.at(0).toLower() + unix.mid(2);
    return unix;
}

QString canonicalOrClean(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(QDir::fromNativeSeparators(path)) : canonical;
}

void pruneStagingDirectory(const QString &directory, const QDateTime &now)
{
    QVector<StagedFile> files;
    QDirIterator it(directory, QDir::Files | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        it.next();
        files.push_back({it.filePath(), it.fileInfo().lastModified()});
    }
    for (const QString &path :
         expiredStagedFiles(files, now, kPasteStagingMaxAgeSeconds, kPasteStagingMaxFiles)) {
        QFile::remove(path);
    }
}

// paste-20261117-093012-345.png; a suffix keeps two pastes in one
// millisecond apart.
QString uniquePath(const QDir &directory, const QDateTime &now, const QString &name)
{
    const QString stamp = now.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    QString candidate = directory.filePath(stamp + QLatin1Char('-') + name);
    for (int n = 2; QFileInfo::exists(candidate); ++n)
        candidate = directory.filePath(stamp + QLatin1Char('-') + QString::number(n) + QLatin1Char('-') + name);
    return candidate;
}

} // namespace

PathQuoting pathQuotingForCommandLine(const QString &commandLine)
{
    const QString name = programName(commandLine);
    if (name == QLatin1String("ssh") || name == QLatin1String("mosh")) return PathQuoting::Remote;
    if (name == QLatin1String("pwsh") || name == QLatin1String("powershell")) return PathQuoting::PowerShell;
    if (name == QLatin1String("cmd")) return PathQuoting::Cmd;
    if (name == QLatin1String("wsl")) return PathQuoting::Wsl;
    return PathQuoting::Posix;
}

QString quotePath(const QString &path, PathQuoting quoting)
{
    switch (quoting) {
    case PathQuoting::Posix:
        return posixQuote(path);
    case PathQuoting::Wsl:
        return posixQuote(wslPath(path));
    case PathQuoting::PowerShell:
    case PathQuoting::Cmd: {
        const QString native = QDir::toNativeSeparators(path);
        if (!native.isEmpty() && std::all_of(native.cbegin(), native.cend(), windowsShellSafe)) return native;
        return quoting == PathQuoting::PowerShell ? powerShellQuote(native) : cmdQuote(native);
    }
    case PathQuoting::Remote:
        break;
    }
    return {};
}

QString pastePaths(const QStringList &paths, PathQuoting quoting)
{
    if (quoting == PathQuoting::Remote) return {};
    QStringList quoted;
    for (const QString &path : paths) {
        if (!path.isEmpty()) quoted.append(quotePath(path, quoting));
    }
    return quoted.isEmpty() ? QString() : quoted.join(QLatin1Char(' ')) + QLatin1Char(' ');
}

ClipboardPasteKind classifyClipboard(bool hasLocalFiles, bool hasText, bool hasImage)
{
    if (hasLocalFiles) return ClipboardPasteKind::Files;
    if (hasText) return ClipboardPasteKind::Text;
    if (hasImage) return ClipboardPasteKind::Image;
    return ClipboardPasteKind::Nothing;
}

QStringList expiredStagedFiles(QVector<StagedFile> files, const QDateTime &now, qint64 maxAgeSeconds, int maxFiles)
{
    std::sort(files.begin(), files.end(),
              [](const StagedFile &a, const StagedFile &b) { return a.modified > b.modified; });
    QStringList expired;
    for (qsizetype index = 0; index < files.size(); ++index) {
        const StagedFile &file = files.at(index);
        if (index >= maxFiles || file.modified.secsTo(now) > maxAgeSeconds) expired.append(file.path);
    }
    return expired;
}

bool isTransientFile(const QString &path)
{
    const QString file = canonicalOrClean(path);
    const QString temp = canonicalOrClean(QDir::tempPath());
    return !temp.isEmpty() && file.startsWith(temp + QLatin1Char('/'));
}

QString pasteStagingDirectory()
{
    return QDir(core::ProductPaths::cacheDirectory()).filePath(kPasteStagingDirectoryName);
}

QString stagePastedImage(const QByteArray &png, const QString &directory, const QDateTime &now, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return QString();
    };
    QDir dir(directory);
    if (!dir.mkpath(QStringLiteral("."))) return fail(QStringLiteral("cannot create %1").arg(directory));
    pruneStagingDirectory(directory, now);
    QSaveFile file(uniquePath(dir, now, QStringLiteral("paste.png")));
    if (!file.open(QIODevice::WriteOnly) || file.write(png) != png.size() || !file.commit())
        return fail(file.errorString());
    return file.fileName();
}

QString stageDroppedFile(const QString &path, const QString &directory, const QDateTime &now)
{
    const QFileInfo source(path);
    if (!source.isFile() || !isTransientFile(path)) return path;
    QDir dir(directory);
    if (!dir.mkpath(QStringLiteral("."))) return path;
    pruneStagingDirectory(directory, now);
    const QString target = uniquePath(dir, now, source.fileName());
    return QFile::copy(path, target) ? target : path;
}

} // namespace span::terminal
