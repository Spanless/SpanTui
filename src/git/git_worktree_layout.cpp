#include "git/git_worktree_layout.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace span::git {
namespace {

// Small files only: `.git`, `commondir`, `gitdir` and `HEAD` hold one line.
constexpr qint64 kPointerFileLimit = 4096;
// A detached HEAD shows as this many leading digits of its commit.
constexpr qsizetype kShortCommitDigits = 7;

QString firstLine(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.read(kPointerFileLimit);
    const qsizetype end = bytes.indexOf('\n');
    return QString::fromUtf8(end < 0 ? bytes : bytes.left(end)).trimmed();
}

// `relative` resolved against `base`; absolute paths stay as they are.
QString resolved(const QString &base, const QString &relative)
{
    return QDir::cleanPath(QDir(base).absoluteFilePath(relative));
}

QString canonicalDirectory(const QString &path)
{
    const QFileInfo info(path);
    return info.isDir() ? QDir::cleanPath(info.canonicalFilePath()) : QString();
}

} // namespace

QString mainWorktreeOf(const QString &folder)
{
    const QString root = canonicalDirectory(folder);
    if (root.isEmpty()) {
        return {};
    }
    const QFileInfo dotGit(root + QStringLiteral("/.git"));
    if (!dotGit.isFile()) {
        return {};
    }
    const QString pointer = firstLine(dotGit.filePath());
    if (!pointer.startsWith(QLatin1String("gitdir:"))) {
        return {};
    }
    const QString gitDir = resolved(root, pointer.mid(7).trimmed());
    const QString commonPointer = firstLine(gitDir + QStringLiteral("/commondir"));
    if (commonPointer.isEmpty()) {
        return {};
    }
    const QString commonDir = resolved(gitDir, commonPointer);
    // A bare repository's linked worktrees have no main worktree.
    if (QFileInfo(commonDir).fileName() != QLatin1String(".git")) {
        return {};
    }
    const QString main = canonicalDirectory(QFileInfo(commonDir).path());
    return main.compare(root, Qt::CaseInsensitive) == 0 ? QString() : main;
}

QList<LinkedWorktree> linkedWorktreesOf(const QString &mainWorktree)
{
    const QString root = canonicalDirectory(mainWorktree);
    if (root.isEmpty() || !QFileInfo(root + QStringLiteral("/.git")).isDir()) {
        return {};
    }
    QList<LinkedWorktree> worktrees;
    const QDir admin(root + QStringLiteral("/.git/worktrees"));
    const QStringList ids = admin.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &id : ids) {
        const QString entry = admin.filePath(id);
        // `gitdir` names the worktree's `.git` file.
        const QString gitFile = firstLine(entry + QStringLiteral("/gitdir"));
        if (gitFile.isEmpty()) {
            continue;
        }
        const QString path = canonicalDirectory(QFileInfo(resolved(entry, gitFile)).path());
        if (path.isEmpty() || path.compare(root, Qt::CaseInsensitive) == 0) {
            continue;
        }
        LinkedWorktree worktree;
        worktree.path = path;
        const QString head = firstLine(entry + QStringLiteral("/HEAD"));
        const QLatin1String prefix("ref: refs/heads/");
        if (head.startsWith(prefix)) {
            worktree.branch = head.mid(prefix.size());
        }
        worktrees.append(worktree);
    }
    std::sort(worktrees.begin(), worktrees.end(), [](const LinkedWorktree &a, const LinkedWorktree &b) {
        return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
    });
    return worktrees;
}

QString headLabelOf(const QString &folder)
{
    const QString dotGit = QDir(folder).filePath(QStringLiteral(".git"));
    const QFileInfo info(dotGit);
    QString gitDir;
    if (info.isDir()) {
        gitDir = dotGit;
    } else if (info.isFile()) {
        const QString pointer = firstLine(dotGit);
        if (!pointer.startsWith(QLatin1String("gitdir:"))) {
            return {};
        }
        gitDir = resolved(folder, pointer.mid(7).trimmed());
    } else {
        return {};
    }
    const QString head = firstLine(gitDir + QStringLiteral("/HEAD"));
    const QLatin1String branchPrefix("ref: refs/heads/");
    if (head.startsWith(branchPrefix)) {
        return head.mid(branchPrefix.size());
    }
    static const QRegularExpression commit(QStringLiteral("^[0-9a-f]{40,64}$"));
    return commit.match(head).hasMatch() ? head.left(kShortCommitDigits) : QString();
}

bool isWorktreeRoot(const QString &folder)
{
    return QFileInfo::exists(QDir(folder).filePath(QStringLiteral(".git")));
}

bool isValidBranchName(const QString &name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('-')) || name == QLatin1String("@")
        || name.startsWith(QLatin1Char('/')) || name.endsWith(QLatin1Char('/')) || name.endsWith(QLatin1Char('.'))
        || name.contains(QLatin1String("..")) || name.contains(QLatin1String("//"))
        || name.contains(QLatin1String("@{"))) {
        return false;
    }
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || c == QLatin1Char(' ')
            || QStringLiteral("~^:?*[\\").contains(c)) {
            return false;
        }
    }
    const QStringList components = name.split(QLatin1Char('/'));
    for (const QString &component : components) {
        if (component.startsWith(QLatin1Char('.')) || component.endsWith(QLatin1String(".lock"))) {
            return false;
        }
    }
    return true;
}

}
