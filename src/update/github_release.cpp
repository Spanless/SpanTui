#include "update/github_release.h"

#include "update/installer_source.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>

QString GitHubRelease::repository() { return QStringLiteral("Spanless/SpanTui"); }

QUrl GitHubRelease::latestReleaseUrl() {
    return QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest")
                    .arg(repository()));
}

GitHubRelease GitHubRelease::fromJson(const QByteArray &json) {
    GitHubRelease release;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        release.error = tr("GitHub returned an unexpected release description.");
        return release;
    }
    const QJsonObject object = doc.object();
    release.tag = object.value(QStringLiteral("tag_name")).toString().trimmed();
    release.pageUrl = object.value(QStringLiteral("html_url")).toString();
    if (object.value(QStringLiteral("draft")).toBool()
        || object.value(QStringLiteral("prerelease")).toBool()) {
        release.error = tr("Release %1 is not a stable release.").arg(release.tag);
        return release;
    }
    static const QRegularExpression stableTag(QStringLiteral("^[vV]?(\\d+\\.\\d+\\.\\d+)$"));
    const QRegularExpressionMatch tagMatch = stableTag.match(release.tag);
    if (!tagMatch.hasMatch()) {
        release.error = tr("Release tag %1 is not a stable version.").arg(release.tag);
        return release;
    }
    release.version = tagMatch.captured(1);

    const QJsonArray assets = object.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString());
        if (!InstallerSource::isInstallerName(name) || !url.isValid() || url.isRelative()) {
            continue;
        }
        release.assetName = name;
        release.assetUrl = url;
        const QString digest = asset.value(QStringLiteral("digest")).toString().trimmed();
        if (digest.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive)) {
            release.assetSha256 = digest.mid(7).toLower();
        }
        break;
    }
    if (release.assetName.isEmpty()) {
        release.error = tr("Release %1 has no installer.").arg(release.tag);
        return release;
    }
    release.ok = true;
    return release;
}

bool GitHubRelease::isNewer(const QString &version, const QString &than) {
    const QVersionNumber candidate = QVersionNumber::fromString(version);
    const QVersionNumber current = QVersionNumber::fromString(than);
    return !candidate.isNull() && (current.isNull() || candidate > current);
}
