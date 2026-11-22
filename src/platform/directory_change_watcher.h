#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace span::platform {

// Watches a whole directory tree; notifications arrive on the owning
// thread's event loop.
//   Windows: an overlapped ReadDirectoryChangesW read, no thread of its own.
//   macOS: an FSEvents stream delivered on a private dispatch queue and
//   forwarded to the owning thread.
class DirectoryChangeWatcher final : public QObject {
    Q_OBJECT

public:
    explicit DirectoryChangeWatcher(QObject *parent = nullptr);
    ~DirectoryChangeWatcher() override;

    // Replaces the watched tree. False, and not watching, when the directory
    // cannot be watched (missing, or a file system without notifications).
    bool watch(const QString &root);
    void stop();
    bool watching() const;

signals:
    // Paths relative to the root, with '/' separators. Empty when the changes
    // overflowed the buffer and were not enumerated: anything may have changed.
    void changed(const QStringList &relativePaths);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace span::platform
