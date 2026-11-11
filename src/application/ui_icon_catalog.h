#pragma once

#include "application/icon_motion.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

// Resolves chrome and file-type icon names to SVG path `d` data compiled
// under :/icons, plus the optional <name>.motion.json of animated icons.
// FileIconTheme only maps names onto these stems. Unknown names fail closed.
class UiIconCatalog final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready CONSTANT FINAL)

public:
    explicit UiIconCatalog(QObject *parent = nullptr);

    bool ready() const;
    QStringList names() const;

    // SVG path data for Shape PathSvg (every <path> outside <defs>). Empty
    // when the name is unknown.
    Q_INVOKABLE QString path(const QString &name) const;
    Q_INVOKABLE bool filled(const QString &name) const;
    Q_INVOKABLE bool animated(const QString &name) const;
    // Null when the icon has no motion.
    const IconMotionSpec *motion(const QString &name) const;

private:
    struct ParsedIcon {
        QString path;
        bool filled = false;
        QHash<QString, QString> ids;  // id'd <path> elements, <defs> included
    };

    bool load();
    static ParsedIcon parseSvg(const QString &filePath);

    bool m_ready = false;
    QHash<QString, QString> m_paths;
    QHash<QString, bool> m_filled;
    QHash<QString, IconMotionSpec> m_motions;
};
