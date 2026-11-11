#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Resolves file/folder names to first-party catalog stems (type-cpp, file, …).
// Associations live in :/icons/file-icons.json. Drawing stays in UiIconCatalog.
class FileIconTheme final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready CONSTANT FINAL)

public:
    explicit FileIconTheme(QObject *parent = nullptr);

    bool ready() const;

    // Restrict resolved stems to this set. Empty set accepts any mapped name.
    // Workspace startup passes UiIconCatalog::names() so unknown stems fall back.
    void setKnownNames(const QStringList &names);

    // Catalog stem such as "type-cpp" or "folder-open".
    Q_INVOKABLE QString iconName(const QString &name,
                                 bool isDir,
                                 bool expanded = false,
                                 bool isRoot = false) const;

private:
    bool load();
    QString resolveName(const QString &iconId, const QString &fallback) const;
    static QStringList extensionCandidates(const QString &lowerFileName);

    bool m_ready = false;
    QString m_fileIcon = QStringLiteral("file");
    QString m_folderIcon = QStringLiteral("folder");
    QString m_folderExpandedIcon = QStringLiteral("folder-open");
    QString m_rootFolderIcon = QStringLiteral("folder");
    QString m_rootFolderExpandedIcon = QStringLiteral("folder-open");

    QHash<QString, QString> m_fileExtensions;
    QHash<QString, QString> m_fileNames;
    QSet<QString> m_knownNames;
};
