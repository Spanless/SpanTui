#include "workspace/file_icon_theme.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace {

void loadStringMap(const QJsonObject &object, QHash<QString, QString> *out) {
    out->clear();
    out->reserve(object.size());
    for (auto it = object.begin(); it != object.end(); ++it) {
        out->insert(it.key().toLower(), it.value().toString());
    }
}

} // namespace

FileIconTheme::FileIconTheme(QObject *parent) : QObject(parent) {
    m_ready = load();
}

bool FileIconTheme::ready() const {
    return m_ready;
}

void FileIconTheme::setKnownNames(const QStringList &names) {
    m_knownNames.clear();
    m_knownNames.reserve(names.size());
    for (const QString &name : names) {
        if (!name.isEmpty()) {
            m_knownNames.insert(name);
        }
    }
}

bool FileIconTheme::load() {
    Q_INIT_RESOURCE(ui_icons);
    QFile file(QStringLiteral(":/icons/file-icons.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject root = doc.object();

    m_fileIcon = root.value(QStringLiteral("file")).toString(QStringLiteral("file"));
    m_folderIcon = root.value(QStringLiteral("folder")).toString(QStringLiteral("folder"));
    m_folderExpandedIcon =
        root.value(QStringLiteral("folderExpanded")).toString(QStringLiteral("folder-open"));
    m_rootFolderIcon =
        root.value(QStringLiteral("rootFolder")).toString(QStringLiteral("folder"));
    m_rootFolderExpandedIcon =
        root.value(QStringLiteral("rootFolderExpanded")).toString(QStringLiteral("folder-open"));

    loadStringMap(root.value(QStringLiteral("fileExtensions")).toObject(), &m_fileExtensions);
    loadStringMap(root.value(QStringLiteral("fileNames")).toObject(), &m_fileNames);

    return !m_fileExtensions.isEmpty() || !m_fileNames.isEmpty();
}

QStringList FileIconTheme::extensionCandidates(const QString &lowerFileName) {
    // For "foo.bar.baz" → "bar.baz", "baz" (longest compound first).
    QStringList candidates;
    int from = 0;
    while (true) {
        const int dot = lowerFileName.indexOf(QLatin1Char('.'), from);
        if (dot < 0) {
            break;
        }
        candidates.push_back(lowerFileName.mid(dot + 1));
        from = dot + 1;
    }
    return candidates;
}

QString FileIconTheme::resolveName(const QString &iconId, const QString &fallback) const {
    if (iconId.isEmpty()) {
        return fallback;
    }
    if (!m_knownNames.isEmpty() && !m_knownNames.contains(iconId)) {
        return fallback;
    }
    return iconId;
}

QString FileIconTheme::iconName(const QString &name,
                                bool isDir,
                                bool expanded,
                                bool isRoot) const {
    if (isDir) {
        if (isRoot) {
            return resolveName(expanded ? m_rootFolderExpandedIcon : m_rootFolderIcon,
                               expanded ? m_folderExpandedIcon : m_folderIcon);
        }
        return resolveName(expanded ? m_folderExpandedIcon : m_folderIcon,
                           expanded ? QStringLiteral("folder-open") : QStringLiteral("folder"));
    }

    if (!m_ready) {
        return resolveName(m_fileIcon, QStringLiteral("file"));
    }

    const QString lower = name.trimmed().toLower();
    if (lower.isEmpty()) {
        return resolveName(m_fileIcon, QStringLiteral("file"));
    }

    if (const auto it = m_fileNames.constFind(lower); it != m_fileNames.cend()) {
        return resolveName(*it, m_fileIcon);
    }

    const QStringList extensions = extensionCandidates(lower);
    for (const QString &ext : extensions) {
        if (const auto it = m_fileExtensions.constFind(ext); it != m_fileExtensions.cend()) {
            return resolveName(*it, m_fileIcon);
        }
    }

    return resolveName(m_fileIcon, QStringLiteral("file"));
}
