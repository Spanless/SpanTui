#include "workspace/file_tree_model.h"

#include "core/product_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPromise>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QtConcurrent>

namespace {

constexpr qint64 kCopyChunkBytes = 1024 * 1024;

struct CopyPlan {
    QVector<QPair<QString, QString>> files;
    QVector<QPair<QString, QString>> directories;
    qint64 totalBytes = 0;
    bool valid = true;
    QString failure;
};

struct MoveResult {
    bool ok = false;
    QString error;
};

void scanCopyTree(const QString &source, const QString &target, CopyPlan *plan) {
    const QFileInfo info(source);
    if (info.isSymLink()) {
        // Recreating links portably is not possible (QFile::link writes a
        // shortcut, not a reparse point), so refuse the whole move instead of
        // silently dropping the entry and deleting the source afterwards.
        plan->valid = false;
        plan->failure = QStringLiteral("Cannot move across volumes: %1 is a symbolic link or reparse point.")
                            .arg(QDir::toNativeSeparators(source));
        return;
    }
    if (!info.exists()) {
        plan->valid = false;
        plan->failure = QStringLiteral("Cannot move: %1 no longer exists.")
                            .arg(QDir::toNativeSeparators(source));
        return;
    }
    if (info.isFile()) {
        plan->files.push_back({source, target});
        plan->totalBytes += qMax<qint64>(0, info.size());
        return;
    }
    plan->directories.push_back({source, target});
    // Include hidden/system entries: a move must never drop desktop.ini and
    // similar files just because Explorer hides them.
    const QFileInfoList entries = QDir(source).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        scanCopyTree(entry.absoluteFilePath(),
                     QDir(target).filePath(entry.fileName()), plan);
        if (!plan->valid)
            return;
    }
}

bool executeCopyPlan(const CopyPlan &plan, QPromise<MoveResult> *promise) {
    for (const auto &directory : plan.directories) {
        if (!QDir().mkpath(directory.second))
            return false;
    }
    qint64 copied = 0;
    for (const auto &pair : plan.files) {
        QDir().mkpath(QFileInfo(pair.second).dir().absolutePath());
        QFile source(pair.first);
        QSaveFile target(pair.second);
        if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly))
            return false;
        while (!source.atEnd()) {
            const QByteArray chunk = source.read(kCopyChunkBytes);
            if (chunk.isEmpty() && source.error() != QFile::NoError)
                return false;
            if (target.write(chunk) != chunk.size())
                return false;
            copied += chunk.size();
            const int progress = plan.totalBytes <= 0
                ? 1000
                : int(qBound<qint64>(qint64(0),
                                     copied * 1000 / plan.totalBytes,
                                     qint64(1000)));
            promise->setProgressValue(progress);
        }
        if (!target.commit())
            return false;
    }
    promise->setProgressValue(1000);
    return true;
}

// removeRecursively() keeps going past an entry it cannot delete, so a failed
// source removal leaves the source partially gone. Put every missing entry
// back from the completed copy before the copy itself may be discarded.
bool restoreSourceFromCopy(const CopyPlan &plan) {
    bool restored = true;
    for (const auto &directory : plan.directories) {
        restored = QDir().mkpath(directory.first) && restored;
    }
    for (const auto &pair : plan.files) {
        if (QFileInfo::exists(pair.first))
            continue;
        QDir().mkpath(QFileInfo(pair.first).dir().absolutePath());
        restored = QFile::copy(pair.second, pair.first) && restored;
    }
    return restored;
}

bool sameVolume(const QString &first, const QString &second) {
    const QStorageInfo a(first);
    const QStorageInfo b(second);
    return a.isValid() && b.isValid()
        && QString::compare(a.rootPath(), b.rootPath(), Qt::CaseInsensitive) == 0;
}

} // namespace

bool FileTreeModel::validateName(const QString &name) const {
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        const_cast<FileTreeModel *>(this)->setError(tr("Invalid file name."));
        return false;
    }
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == QLatin1String(".") || trimmed == QLatin1String("..")) {
        const_cast<FileTreeModel *>(this)->setError(tr("Invalid file name."));
        return false;
    }
    static const QRegularExpression illegal(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])"));
    if (illegal.match(trimmed).hasMatch()) {
        const_cast<FileTreeModel *>(this)->setError(tr("Invalid file name."));
        return false;
    }
    // Win32 reserved device names (with or without extension).
    const QString base = QFileInfo(trimmed).completeBaseName().isEmpty()
                             ? trimmed
                             : QFileInfo(trimmed).completeBaseName();
    const QString upper = base.toUpper();
    static const QStringList reserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),  QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9"),
    };
    if (reserved.contains(upper) || reserved.contains(trimmed.toUpper())) {
        const_cast<FileTreeModel *>(this)->setError(tr("Invalid file name."));
        return false;
    }
    return true;
}

void FileTreeModel::movePathAsync(const QString &from, const QString &to,
                                  std::function<void(bool)> completion) {
    const QString targetParent = QFileInfo(to).dir().absolutePath();
    if (!QDir().mkpath(targetParent)) {
        if (completion) completion(false);
        return;
    }
    if (!m_copyMovesForTesting && QFile::rename(from, to)) {
        if (completion) completion(true);
        return;
    }
    if (QFileInfo::exists(to)) {
        // Never let the copy fallback overwrite a target that appeared after
        // the caller's pre-check.
        setError(tr("Destination already exists: %1").arg(to));
        if (completion) completion(false);
        return;
    }
    // On one volume a failed rename means something holds the item (an open
    // file, a process's current directory); copying and deleting would only
    // strip the source down to its locked entries.
    if (!m_copyMovesForTesting && sameVolume(from, targetParent)) {
        if (completion) completion(false);
        setError(tr("Could not move %1: it or an item inside it is in use.")
                     .arg(QDir::toNativeSeparators(from)));
        return;
    }
    // Cross-volume rename falls back to an asynchronous recursive copy. The
    // source is removed only after the destination completed, preserving the
    // previous rollback/undo semantics.
    m_progressTitle = tr("Moving files");
    beginAsyncLoad();
    emit progressChanged(-1, tr("Scanning files"));
    auto *watcher = new QFutureWatcher<MoveResult>(this);
    connect(watcher, &QFutureWatcher<MoveResult>::finished, this,
            [this, watcher, completion = std::move(completion)]() mutable {
        const MoveResult result = watcher->result();
        watcher->deleteLater();
        if (!result.ok) {
            m_progressFailed = true;
            completion(false);
            if (!result.error.isEmpty())
                setError(result.error);
        } else {
            completion(true);
        }
        endAsyncLoad();
    });
    connect(watcher, &QFutureWatcher<MoveResult>::progressValueChanged, this,
            [this](int value) {
        emit progressChanged(value / 1000.0,
                             tr("Copying files %1%").arg(value / 10));
    });
    watcher->setFuture(QtConcurrent::run(
        [](QPromise<MoveResult> &promise, const QString &source,
           const QString &target) {
            promise.setProgressRange(0, 1000);
            CopyPlan plan;
            scanCopyTree(source, target, &plan);
            MoveResult result;
            result.ok = plan.valid && executeCopyPlan(plan, &promise);
            bool discardTarget = !result.ok;
            if (!result.ok) {
                result.error = plan.failure.isEmpty()
                    ? FileTreeModel::tr("Could not copy the item to its new location.")
                    : plan.failure;
            } else {
                const QFileInfo info(source);
                result.ok = info.isDir() ? QDir(source).removeRecursively()
                                         : QFile::remove(source);
                if (!result.ok) {
                    // The copy is now the only complete image; drop it only
                    // once the source is whole again.
                    discardTarget = restoreSourceFromCopy(plan);
                    result.error = discardTarget
                        ? FileTreeModel::tr("Could not move %1: an item inside it is in use. "
                                            "The original was left in place.")
                              .arg(QDir::toNativeSeparators(source))
                        : FileTreeModel::tr("Could not move %1 completely. "
                                            "A full copy remains at %2.")
                              .arg(QDir::toNativeSeparators(source),
                                   QDir::toNativeSeparators(target));
                }
            }
            if (discardTarget) {
                const QFileInfo targetInfo(target);
                if (targetInfo.isDir())
                    QDir(target).removeRecursively();
                else
                    QFile::remove(target);
            }
            promise.addResult(result);
        }, from, to));
}

QString FileTreeModel::defaultTrashRoot() {
    return QDir(span::core::ProductPaths::root())
        .filePath(QStringLiteral("trash"));
}

bool FileTreeModel::purgeTrash(const QString &trashRoot) {
    const QString clean = QDir::cleanPath(trashRoot);
    // Refuse to wipe anything that is not unmistakably a leaf directory.
    if (clean.isEmpty() || QDir(clean).isRoot()) {
        return false;
    }
    return QDir(clean).removeRecursively();
}
