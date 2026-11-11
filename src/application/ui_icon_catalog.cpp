#include "application/ui_icon_catalog.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSvgRenderer>
#include <QXmlStreamReader>
#include <QtDebug>

UiIconCatalog::UiIconCatalog(QObject *parent) : QObject(parent) {
    Q_INIT_RESOURCE(ui_icons);
    m_ready = load();
}

bool UiIconCatalog::ready() const {
    return m_ready;
}

QStringList UiIconCatalog::names() const {
    QStringList keys = m_paths.keys();
    keys.sort();
    return keys;
}

QString UiIconCatalog::path(const QString &name) const {
    if (!m_ready || name.isEmpty()) {
        return {};
    }
    const auto it = m_paths.constFind(name);
    if (it == m_paths.cend()) {
        qWarning("UiIconCatalog: unknown icon name: %s", qPrintable(name));
        return {};
    }
    return *it;
}

bool UiIconCatalog::filled(const QString &name) const {
    if (!m_ready || name.isEmpty()) {
        return false;
    }
    return m_filled.value(name, false);
}

bool UiIconCatalog::animated(const QString &name) const {
    return motion(name) != nullptr;
}

const IconMotionSpec *UiIconCatalog::motion(const QString &name) const {
    if (!m_ready || name.isEmpty()) {
        return nullptr;
    }
    const auto it = m_motions.constFind(name);
    return it == m_motions.cend() ? nullptr : &*it;
}

namespace {

bool looksFilled(const QStringView fill, const QStringView stroke) {
    const bool hasFill = fill == QLatin1String("currentColor");
    const bool noStroke = stroke.isEmpty() || stroke == QLatin1String("none");
    return hasFill && noStroke;
}

} // namespace

UiIconCatalog::ParsedIcon UiIconCatalog::parseSvg(const QString &filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QXmlStreamReader xml(&file);
    QStringList parts;
    QHash<QString, QString> ids;
    bool filled = false;
    int defsDepth = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::EndElement && xml.name() == QLatin1String("defs")) {
            --defsDepth;
            continue;
        }
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }
        if (xml.name() == QLatin1String("defs")) {
            ++defsDepth;
            continue;
        }
        if (xml.name() == QLatin1String("svg")) {
            filled = looksFilled(xml.attributes().value(QLatin1String("fill")),
                                 xml.attributes().value(QLatin1String("stroke")));
            continue;
        }
        if (xml.name() != QLatin1String("path")) {
            continue;
        }
        const auto attrs = xml.attributes();
        const QString data = attrs.value(QLatin1String("d")).toString().trimmed();
        if (data.isEmpty()) {
            continue;
        }
        const QString id = attrs.value(QLatin1String("id")).toString();
        if (!id.isEmpty()) {
            ids.insert(id, data);
        }
        // <defs> holds geometry only motion uses; the static glyph skips it.
        if (defsDepth > 0) {
            continue;
        }
        parts.append(data);
        if (attrs.hasAttribute(QLatin1String("fill"))
            || attrs.hasAttribute(QLatin1String("stroke"))) {
            filled = looksFilled(attrs.value(QLatin1String("fill")),
                                 attrs.value(QLatin1String("stroke")));
        }
    }
    if (xml.hasError() || parts.isEmpty()) {
        return {};
    }
    return {parts.join(QLatin1Char(' ')), filled, ids};
}

bool UiIconCatalog::load() {
    m_paths.clear();
    m_filled.clear();
    m_motions.clear();
    const auto failClosed = [this] {
        m_paths.clear();
        m_filled.clear();
        m_motions.clear();
        return false;
    };

    QDirIterator iterator(QStringLiteral(":/icons"),
                          QStringList{QStringLiteral("*.svg")},
                          QDir::Files,
                          QDirIterator::Subdirectories);
    if (!iterator.hasNext()) {
        qWarning("UiIconCatalog: no SVG files under :/icons");
        return false;
    }

    while (iterator.hasNext()) {
        const QString filePath = iterator.next();
        const QString name = QFileInfo(filePath).completeBaseName();
        if (name.isEmpty()) {
            qWarning("UiIconCatalog: empty icon name for %s", qPrintable(filePath));
            return failClosed();
        }
        if (m_paths.contains(name)) {
            qWarning("UiIconCatalog: duplicate icon name: %s", qPrintable(name));
            return failClosed();
        }

        QSvgRenderer renderer(filePath);
        if (!renderer.isValid()) {
            qWarning("UiIconCatalog: invalid SVG: %s", qPrintable(filePath));
            return failClosed();
        }

        const ParsedIcon parsed = parseSvg(filePath);
        if (parsed.path.isEmpty()) {
            qWarning("UiIconCatalog: missing path data: %s", qPrintable(filePath));
            return failClosed();
        }
        m_paths.insert(name, parsed.path);
        m_filled.insert(name, parsed.filled);

        const QString motionPath = filePath.chopped(4) + QStringLiteral(".motion.json");
        QFile motionFile(motionPath);
        if (!motionFile.exists()) {
            continue;
        }
        QJsonParseError jsonError;
        const QJsonDocument document = motionFile.open(QIODevice::ReadOnly)
                                           ? QJsonDocument::fromJson(motionFile.readAll(), &jsonError)
                                           : QJsonDocument();
        IconMotionSpec spec;
        QString reason = jsonError.error != QJsonParseError::NoError ? jsonError.errorString()
                                                                     : QString();
        if (!document.isObject()
            || !IconMotionSpec::parse(document.object(), parsed.ids, &spec, &reason)) {
            qWarning("UiIconCatalog: invalid motion %s: %s", qPrintable(motionPath),
                     qPrintable(reason));
            return failClosed();
        }
        m_motions.insert(name, spec);
    }

    return !m_paths.isEmpty();
}
