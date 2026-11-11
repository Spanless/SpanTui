#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QUrl>

// The remote update channel: the latest stable release of the public GitHub
// repository and its installer asset (SpanTui-<version>-Setup.exe, produced
// by `dev package`).
class GitHubRelease final {
    Q_DECLARE_TR_FUNCTIONS(GitHubRelease)

public:
    static QString repository();
    // GET returns the latest release that is neither a draft nor a prerelease.
    static QUrl latestReleaseUrl();

    // Reads one release object of the GitHub REST API. `ok` is false with a
    // translated `error` when the release is not a stable vX.Y.Z release or
    // carries no installer.
    static GitHubRelease fromJson(const QByteArray &json);
    // Numeric comparison of two X.Y.Z versions.
    static bool isNewer(const QString &version, const QString &than);

    bool ok = false;
    QString error;
    QString tag;
    QString version;
    QString pageUrl;
    QString assetName;
    QUrl assetUrl;
    // Lower-case hex SHA-256 of the asset when GitHub reports one.
    QString assetSha256;
};
