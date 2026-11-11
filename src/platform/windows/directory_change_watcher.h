#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace span::platform {

// Watches a whole directory tree (ReadDirectoryChangesW). The overlapped read
// completes on the owning thread's event loop; there is no thread of its own.
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
    void readCompleted();
    bool startRead();

    std::unique_ptr<Impl> m_impl;
};

} // namespace span::platform
