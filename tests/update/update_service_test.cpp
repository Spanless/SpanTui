#include "update/github_release.h"
#include "update/installer_source.h"
#include "update/update_service.h"

#include "core/product_paths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using span::core::OperationOutcome;

namespace {

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QString sha256Of(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString installerName(const QString &version)
{
    return QStringLiteral("SpanTui-%1-Setup.exe").arg(version);
}

// Where `dev package` puts a version inside the folder that holds them all.
QString packageDirectory(const QString &source, const QString &version)
{
    return QDir(source).filePath(version + QLatin1Char('/') + InstallerSource::platformSlot());
}

// A `dev package` output in `source`: release-manifest.json and the Setup it
// lists. `listedSha256` replaces the digest of the written bytes.
bool writeLocalPackage(const QString &source, const QString &version, const QString &builtAtUtc = {},
                       const QByteArray &installer = QByteArrayLiteral("setup-bytes"), const QString &listedSha256 = {})
{
    const QString directory = packageDirectory(source, version);
    QJsonObject manifest{{QStringLiteral("name"), QStringLiteral("SpanTui")},
                         {QStringLiteral("version"), version},
                         {QStringLiteral("artifacts"),
                          QJsonArray{QJsonObject{{QStringLiteral("file"), installerName(version)},
                                                 {QStringLiteral("size"), installer.size()},
                                                 {QStringLiteral("sha256"),
                                                  listedSha256.isEmpty() ? sha256Of(installer) : listedSha256}}}}};
    if (!builtAtUtc.isEmpty()) manifest.insert(QStringLiteral("builtAtUtc"), builtAtUtc);
    return writeFile(QDir(directory).filePath(installerName(version)), installer)
        && writeFile(QDir(directory).filePath(QStringLiteral("release-manifest.json")),
                     QJsonDocument(manifest).toJson());
}

// What the installer leaves in the install directory: its uninstaller and
// the image's build stamp.
bool writeInstalledImage(const QString &directory, const QString &version = {}, const QString &builtAtUtc = {})
{
    QJsonObject stamp;
    if (!version.isEmpty()) stamp.insert(QStringLiteral("version"), version);
    if (!builtAtUtc.isEmpty()) stamp.insert(QStringLiteral("builtAtUtc"), builtAtUtc);
    return writeFile(QDir(directory).filePath(QStringLiteral("unins000.exe")), "uninstaller")
        && writeFile(QDir(directory).filePath(QStringLiteral("spantui-build.json")), QJsonDocument(stamp).toJson());
}

OperationOutcome outcomeOf(const QSignalSpy &finished, int index = 0)
{
    return qvariant_cast<OperationOutcome>(finished.at(index).at(0));
}

QJsonObject releaseJson(const QString &tag, const QJsonArray &assets,
                        bool prerelease = false, bool draft = false)
{
    return {{QStringLiteral("tag_name"), tag},
            {QStringLiteral("html_url"),
             QStringLiteral("https://github.com/Spanless/SpanTui/releases/tag/") + tag},
            {QStringLiteral("prerelease"), prerelease},
            {QStringLiteral("draft"), draft},
            {QStringLiteral("assets"), assets}};
}

QJsonObject assetJson(const QString &name, const QUrl &url, const QString &digest = {})
{
    QJsonObject asset{{QStringLiteral("name"), name},
                      {QStringLiteral("browser_download_url"), url.toString()}};
    if (!digest.isEmpty())
        asset.insert(QStringLiteral("digest"), digest);
    return asset;
}

QString updatesDirectory()
{
    return QDir(span::core::ProductPaths::cacheDirectory()).filePath(QStringLiteral("updates"));
}

// argv_echo_child stands in for the installer: it records its command line
// in its working directory, the temporary folder.
QString recordedArgumentsPath()
{
    return QDir::temp().filePath(QStringLiteral("argv.txt"));
}

QByteArray argvEchoChild()
{
    return readFile(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("argv_echo_child.exe")));
}

// The fraction argument of each progressChanged recorded by `spy`.
QList<qreal> progressFractions(const QSignalSpy &spy)
{
    QList<qreal> fractions;
    for (const QList<QVariant> &call : spy) fractions.append(call.at(0).toReal());
    return fractions;
}

} // namespace

class UpdateServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void automaticCheckStaysSilent();
    void manualCheckReportsLifecycle();
    void folderWithoutPackagesFails();
    void manifestWithoutInstallerFails();
    void newerPackageIsAvailable();
    void newestPackageIsTheCandidate();
    void olderPackageIsUpToDate();
    void sameVersionComparesBuildTime();
    void classifiesTheRunningImage();
    void persistsSettings();
    void sourceFolderUrlKeepsNetworkShares();
    void changingSourceCancelsVisibleCheck();
    void applyWithoutUpdateDoesNothing();
    void imageThatCannotInstallOnlyReports();
    void parsesStableRelease();
    void rejectsUnstableOrIncompleteReleases();
    void comparesReleaseVersionsNumerically();
    void remoteCheckFindsNewerRelease();
    void remoteCheckReportsOlderReleaseAndUnreachableFeed();
    void localSourceReplacesRemote();
    void remoteApplyRefusesDigestMismatch();
    void localApplyRefusesDigestMismatch();
    void localApplyStartsTheInstaller();
    void remoteApplyStartsTheDownloadedInstaller();

private:
    // A service on the local channel (no GitHub request can start).
    static void useLocal(UpdateService &service);
    // Runs a visible check of `path` and waits for it.
    void check(UpdateService &service, const QString &path);
    // Points the remote channel at a release description written to `dir`.
    static QUrl writeFeed(const QTemporaryDir &dir, const QJsonObject &release);
    // Waits for the stand-in installer and returns its command line.
    static QStringList recordedArguments();
};

void UpdateServiceTest::init()
{
    QFile::remove(span::core::ProductPaths::settingsPath());
    QFile::remove(recordedArgumentsPath());
}

void UpdateServiceTest::useLocal(UpdateService &service)
{
    service.setLocalSourceEnabled(true);
    QVERIFY(!service.busy());
}

void UpdateServiceTest::check(UpdateService &service, const QString &path)
{
    // Setting the source starts its own check; wait for it, then check
    // explicitly so callers observe a complete visible lifecycle.
    service.setLocalSourcePath(path);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    service.checkNow(false, true);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
}

QUrl UpdateServiceTest::writeFeed(const QTemporaryDir &dir, const QJsonObject &release)
{
    const QString path = dir.filePath(QStringLiteral("latest.json"));
    writeFile(path, QJsonDocument(release).toJson());
    return QUrl::fromLocalFile(path);
}

QStringList UpdateServiceTest::recordedArguments()
{
    const QString path = recordedArgumentsPath();
    if (!QTest::qWaitFor([&path]() { return QFileInfo::exists(path); }, 10000)) return {};
    const QStringList arguments = QString::fromUtf8(readFile(path)).split(QLatin1Char('\n'));
    QFile::remove(path);
    return arguments;
}

void UpdateServiceTest::automaticCheckStaysSilent()
{
    UpdateService service;
    useLocal(service);
    QSignalSpy started(&service, &UpdateService::progressStarted);
    QSignalSpy changed(&service, &UpdateService::progressChanged);
    QSignalSpy finished(&service, &UpdateService::progressFinished);

    service.checkNow(false, false);
    QVERIFY(service.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 1000);
    QCOMPARE(started.count(), 0);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(finished.count(), 0);
}

void UpdateServiceTest::manualCheckReportsLifecycle()
{
    UpdateService service;
    useLocal(service);
    QVERIFY(!service.canCheck());
    QSignalSpy started(&service, &UpdateService::progressStarted);
    QSignalSpy changed(&service, &UpdateService::progressChanged);
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    QSignalSpy visibleFinished(&service, &UpdateService::visibleCheckFinished);

    service.checkNow(false, true);
    QCOMPARE(started.count(), 1);
    QCOMPARE(changed.count(), 1);
    QVERIFY(service.busy());
    // A second request while checking is ignored rather than stacked.
    service.checkNow(false, true);
    QCOMPARE(started.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 1000);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(visibleFinished.count(), 1);
    // No configured source is a failed check, not "up to date".
    QCOMPARE(outcomeOf(finished), OperationOutcome::Failed);
    QVERIFY(!finished.first().at(1).toString().isEmpty());
}

void UpdateServiceTest::folderWithoutPackagesFails()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    // An unpacked build is not a package: there is no installer to run.
    QVERIFY(writeFile(source.filePath(QStringLiteral("SpanTui.exe")), "a build"));
    UpdateService service;
    useLocal(service);
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    check(service, source.path());
    QVERIFY(!service.updateAvailable());
    QVERIFY(service.sourceLocation().isEmpty());
    QCOMPARE(outcomeOf(finished, finished.count() - 1), OperationOutcome::Failed);

    // One version's own folder is no source either: the source is the folder
    // above the versions, which stays the same when the version changes.
    QTemporaryDir packages;
    QVERIFY(packages.isValid());
    QVERIFY(writeLocalPackage(packages.path(), QStringLiteral("999.0.0")));
    check(service, packageDirectory(packages.path(), QStringLiteral("999.0.0")));
    QVERIFY(!service.updateAvailable());
    QCOMPARE(outcomeOf(finished, finished.count() - 1), OperationOutcome::Failed);
}

void UpdateServiceTest::manifestWithoutInstallerFails()
{
    QTemporaryDir quick;
    QVERIFY(quick.isValid());
    // dev package --quick lists the portable archive only.
    const QJsonObject manifest{
        {QStringLiteral("version"), QStringLiteral("999.0.0")},
        {QStringLiteral("artifacts"),
         QJsonArray{QJsonObject{{QStringLiteral("file"), QStringLiteral("SpanTui-999.0.0-portable.zip")},
                                {QStringLiteral("sha256"), QString(64, QLatin1Char('0'))}}}}};
    QVERIFY(writeFile(QDir(packageDirectory(quick.path(), QStringLiteral("999.0.0")))
                          .filePath(QStringLiteral("release-manifest.json")),
                      QJsonDocument(manifest).toJson()));
    UpdateService service;
    useLocal(service);
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    check(service, quick.path());
    QVERIFY(!service.updateAvailable());
    QCOMPARE(outcomeOf(finished, finished.count() - 1), OperationOutcome::Failed);

    // A listed installer that is not on disk fails the same way.
    QTemporaryDir missing;
    QVERIFY(missing.isValid());
    QVERIFY(writeLocalPackage(missing.path(), QStringLiteral("999.0.0")));
    QVERIFY(QFile::remove(QDir(packageDirectory(missing.path(), QStringLiteral("999.0.0")))
                              .filePath(installerName(QStringLiteral("999.0.0")))));
    check(service, missing.path());
    QVERIFY(!service.updateAvailable());
    QCOMPARE(outcomeOf(finished, finished.count() - 1), OperationOutcome::Failed);
}

void UpdateServiceTest::newerPackageIsAvailable()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    QVERIFY(writeLocalPackage(source.path(), QStringLiteral("999.0.0")));
    UpdateService service;
    useLocal(service);
    QSignalSpy visible(&service, &UpdateService::visibleCheckFinished);
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    check(service, source.path());

    QVERIFY(service.updateAvailable());
    QCOMPARE(service.availableVersion(), QStringLiteral("999.0.0"));
    QCOMPARE(QFileInfo(service.sourceLocation()).fileName(), installerName(QStringLiteral("999.0.0")));
    QVERIFY(service.statusMessage().contains(QStringLiteral("999.0.0")));
    QCOMPARE(outcomeOf(finished, finished.count() - 1), OperationOutcome::Succeeded);
    QCOMPARE(visible.constLast().at(0).toBool(), true);
}

void UpdateServiceTest::newestPackageIsTheCandidate()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    // Versions compare numerically, not by folder name; a same-version
    // rebuild replaces its own folder, so only one 999.0.0 can exist.
    QVERIFY(writeLocalPackage(source.path(), QStringLiteral("1.5.0")));
    QVERIFY(writeLocalPackage(source.path(), QStringLiteral("999.0.0")));
    QVERIFY(writeLocalPackage(source.path(), QStringLiteral("10.0.0")));
    // Folders without a package for this platform are not candidates.
    QVERIFY(writeFile(QDir(source.filePath(QStringLiteral("2000.0.0"))).filePath(QStringLiteral("notes.txt")), "x"));
    UpdateService service;
    useLocal(service);
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    check(service, source.path());
    QVERIFY(service.updateAvailable());
    QCOMPARE(service.availableVersion(), QStringLiteral("999.0.0"));
    QCOMPARE(QFileInfo(service.sourceLocation()).absolutePath(),
             QFileInfo(packageDirectory(source.path(), QStringLiteral("999.0.0"))).absoluteFilePath());

    // A newest package that cannot be installed is reported; an older one
    // never stands in for it.
    const QJsonObject quick{
        {QStringLiteral("version"), QStringLiteral("1000.0.0")},
        {QStringLiteral("artifacts"),
         QJsonArray{QJsonObject{{QStringLiteral("file"), QStringLiteral("SpanTui-1000.0.0-portable.zip")},
                                {QStringLiteral("sha256"), QString(64, QLatin1Char('0'))}}}}};
    QVERIFY(writeFile(QDir(packageDirectory(source.path(), QStringLiteral("1000.0.0")))
                          .filePath(QStringLiteral("release-manifest.json")),
                      QJsonDocument(quick).toJson()));
    service.checkNow(false, true);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(!service.updateAvailable());
    QCOMPARE(service.availableVersion(), QStringLiteral("1000.0.0"));
    QCOMPARE(outcomeOf(finished, finished.count() - 1), OperationOutcome::Failed);
}

void UpdateServiceTest::olderPackageIsUpToDate()
{
    QTemporaryDir source;
    QVERIFY(source.isValid());
    QVERIFY(writeLocalPackage(source.path(), QStringLiteral("0.0.1")));
    UpdateService service;
    useLocal(service);
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    check(service, source.path());
    QVERIFY(!service.updateAvailable());
    QCOMPARE(service.availableVersion(), QStringLiteral("0.0.1"));
    // A valid source with nothing newer is a successful check.
    QCOMPARE(outcomeOf(finished, finished.count() - 1), OperationOutcome::Succeeded);
}

void UpdateServiceTest::sameVersionComparesBuildTime()
{
    QTemporaryDir image;
    QTemporaryDir later;
    QTemporaryDir earlier;
    QVERIFY(image.isValid() && later.isValid() && earlier.isValid());
    QVERIFY(writeInstalledImage(image.path(), QStringLiteral("2.0.0"), QStringLiteral("2026-05-01T10:00:00Z")));
    QVERIFY(writeLocalPackage(later.path(), QStringLiteral("2.0.0"), QStringLiteral("2026-05-01T12:00:00Z")));
    QVERIFY(writeLocalPackage(earlier.path(), QStringLiteral("2.0.0"), QStringLiteral("2026-05-01T09:00:00Z")));
    UpdateService service;
    // The image stamp, not the application version, is the installed build.
    service.setImageDirectory(image.path());
    useLocal(service);
    check(service, later.path());
    QVERIFY(service.updateAvailable());
    check(service, earlier.path());
    QVERIFY(!service.updateAvailable());

    QVERIFY(InstallerSource::isNewer(QStringLiteral("0.10.0"), {}, QStringLiteral("0.9.0"), {}));
    // Same version without both build times is not an update.
    QVERIFY(!InstallerSource::isNewer(QStringLiteral("1.0.0"), QStringLiteral("2026-01-01T00:00:00Z"),
                                      QStringLiteral("1.0.0"), {}));
}

void UpdateServiceTest::classifiesTheRunningImage()
{
    QTemporaryDir installed;
    QTemporaryDir portable;
    QTemporaryDir build;
    QVERIFY(installed.isValid() && portable.isValid() && build.isValid());
    QVERIFY(writeInstalledImage(installed.path()));
    QVERIFY(writeFile(portable.filePath(QStringLiteral("portable.txt")), "portable"));
    // A portable copy that also carries an uninstaller stays portable.
    QVERIFY(writeFile(portable.filePath(QStringLiteral("unins000.exe")), "uninstaller"));
    QCOMPARE(InstallerSource::imageKind(installed.path()), InstallerSource::ImageKind::Installed);
    QCOMPARE(InstallerSource::imageKind(portable.path()), InstallerSource::ImageKind::Portable);
    QCOMPARE(InstallerSource::imageKind(build.path()), InstallerSource::ImageKind::Development);

    UpdateService service;
    // The test runs from a build tree.
    QVERIFY(!service.canInstall());
    QVERIFY(!service.portable());
    QSignalSpy status(&service, &UpdateService::statusChanged);
    service.setImageDirectory(installed.path());
    QVERIFY(service.canInstall());
    service.setImageDirectory(portable.path());
    QVERIFY(!service.canInstall());
    QVERIFY(service.portable());
    QCOMPARE(status.count(), 2);
}

void UpdateServiceTest::persistsSettings()
{
    {
        UpdateService service;
        QVERIFY(!service.localSourceEnabled());
        QSignalSpy enabledChanged(&service, &UpdateService::localSourceEnabledChanged);
        QSignalSpy sourceChanged(&service, &UpdateService::localSourcePathChanged);
        service.setLocalSourceEnabled(true);
        QCOMPARE(enabledChanged.count(), 1);
        service.setLocalSourcePath(QStringLiteral("  C:/span-missing-source/a/../b  "));
        QCOMPARE(service.localSourcePath(), QStringLiteral("C:/span-missing-source/b"));
        QCOMPARE(sourceChanged.count(), 1);
        // The same path again is not a change.
        service.setLocalSourcePath(QStringLiteral("C:/span-missing-source/b"));
        QCOMPARE(sourceChanged.count(), 1);
        service.setAutoApply(true);
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    }
    UpdateService reloaded;
    QVERIFY(reloaded.localSourceEnabled());
    QCOMPARE(reloaded.localSourcePath(), QStringLiteral("C:/span-missing-source/b"));
    QVERIFY(reloaded.autoApply());
}

void UpdateServiceTest::sourceFolderUrlKeepsNetworkShares()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = root.filePath(QStringLiteral("build out"));
    QVERIFY(QDir().mkpath(folder));
    UpdateService service;
    useLocal(service);

    // What the folder dialog returns: a percent-encoded file URL.
    const QUrl local(QStringLiteral("file:///") + QString(folder).replace(QStringLiteral(" "),
                                                                        QStringLiteral("%20")));
    service.setLocalSourceFolderUrl(local);
    QCOMPARE(service.localSourcePath(), QDir::cleanPath(folder));
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);

    // A share keeps its server: file://server/share/dir is //server/share/dir.
    // The administrative share of this machine stands in for a real one.
    const QString unc = QStringLiteral("//localhost/") + folder.left(1) + QStringLiteral("$")
        + folder.mid(2);
    if (!QFileInfo(unc).isDir()) QSKIP("Administrative share is not reachable");
    const QUrl share = QUrl::fromLocalFile(unc);
    QCOMPARE(share.host(), QStringLiteral("localhost"));
    service.setLocalSourceFolderUrl(share);
    QCOMPARE(service.localSourcePath(), unc);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);

    // Anything but a local file leaves the source alone.
    service.setLocalSourceFolderUrl(QUrl(QStringLiteral("https://example.com/build")));
    QCOMPARE(service.localSourcePath(), unc);
}

void UpdateServiceTest::changingSourceCancelsVisibleCheck()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    UpdateService service;
    useLocal(service);
    service.setLocalSourcePath(first.path());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);

    QSignalSpy finished(&service, &UpdateService::progressFinished);
    service.checkNow(false, true);
    QVERIFY(service.busy());
    service.setLocalSourcePath(second.path());
    // The stale check closes its progress as cancelled; its result is dropped.
    QCOMPARE(finished.count(), 1);
    QCOMPARE(outcomeOf(finished), OperationOutcome::Cancelled);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QCOMPARE(finished.count(), 2);
}

void UpdateServiceTest::applyWithoutUpdateDoesNothing()
{
    UpdateService service;
    QSignalSpy started(&service, &UpdateService::progressStarted);
    QSignalSpy failed(&service, &UpdateService::updateFailed);
    QVERIFY(!service.applyAndRestart());
    QCOMPARE(started.count(), 0);
    QCOMPARE(failed.count(), 0);
    QVERIFY(!service.updateAvailable());
    QVERIFY(!service.statusMessage().isEmpty());
}

void UpdateServiceTest::imageThatCannotInstallOnlyReports()
{
    QTemporaryDir portable;
    QTemporaryDir source;
    QVERIFY(portable.isValid() && source.isValid());
    QVERIFY(writeFile(portable.filePath(QStringLiteral("portable.txt")), "portable"));
    QVERIFY(writeLocalPackage(source.path(), QStringLiteral("999.0.0")));
    UpdateService service;
    service.setImageDirectory(portable.path());
    service.setAutoApply(true);
    useLocal(service);
    QSignalSpy started(&service, &UpdateService::progressStarted);
    QSignalSpy failed(&service, &UpdateService::updateFailed);

    // The source change runs a visible check and asks for an automatic
    // install; a portable copy only reports the update.
    service.setLocalSourcePath(source.path());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(service.updateAvailable());
    QCOMPARE(started.count(), 1);
    QVERIFY(!service.applyAndRestart());
    QCOMPARE(started.count(), 1);
    QCOMPARE(failed.count(), 0);
    QVERIFY(service.updateAvailable());
    QVERIFY(!QFileInfo::exists(QDir(updatesDirectory()).filePath(installerName(QStringLiteral("999.0.0")))));
    // The newest package's folder holds the portable zip to download.
    QCOMPARE(
        service.downloadPageUrl(),
        QUrl::fromLocalFile(QFileInfo(packageDirectory(source.path(), QStringLiteral("999.0.0"))).absoluteFilePath()));

    // On the remote channel the download page is the release page.
    QTemporaryDir feed;
    QVERIFY(feed.isValid());
    service.setLocalSourceEnabled(false);
    QCOMPARE(service.downloadPageUrl(), QUrl(QStringLiteral("https://github.com/Spanless/SpanTui/releases/latest")));
    service.setReleaseFeedUrl(
        writeFeed(feed,
                  releaseJson(QStringLiteral("v999.0.0"),
                              {assetJson(installerName(QStringLiteral("999.0.0")),
                                         QUrl::fromLocalFile(feed.filePath(QStringLiteral("s.exe"))))})));
    service.checkNow(false, false);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QCOMPARE(service.downloadPageUrl(),
             QUrl(QStringLiteral("https://github.com/Spanless/SpanTui/releases/tag/v999.0.0")));
}

void UpdateServiceTest::parsesStableRelease()
{
    const QString digest = QString(64, QLatin1Char('A'));
    const QJsonArray assets{assetJson(QStringLiteral("SpanTui-1.2.3-portable.zip"),
                                      QUrl(QStringLiteral("https://example.com/portable.zip"))),
                            assetJson(QStringLiteral("SpanTui-1.2.3-Setup.exe"),
                                      QUrl(QStringLiteral("https://example.com/setup.exe")),
                                      QStringLiteral("sha256:") + digest)};
    const GitHubRelease release = GitHubRelease::fromJson(
        QJsonDocument(releaseJson(QStringLiteral("v1.2.3"), assets)).toJson());
    QVERIFY2(release.ok, qPrintable(release.error));
    QCOMPARE(release.tag, QStringLiteral("v1.2.3"));
    QCOMPARE(release.version, QStringLiteral("1.2.3"));
    QCOMPARE(release.assetName, QStringLiteral("SpanTui-1.2.3-Setup.exe"));
    QCOMPARE(release.assetUrl, QUrl(QStringLiteral("https://example.com/setup.exe")));
    QCOMPARE(release.assetSha256, digest.toLower());
    QVERIFY(release.pageUrl.endsWith(QStringLiteral("/v1.2.3")));
}

void UpdateServiceTest::rejectsUnstableOrIncompleteReleases()
{
    const QJsonArray assets{
        assetJson(QStringLiteral("SpanTui-1.2.3-Setup.exe"), QUrl(QStringLiteral("https://example.com/s.exe")))};
    const auto parse = [](const QJsonObject &object) {
        return GitHubRelease::fromJson(QJsonDocument(object).toJson());
    };
    QVERIFY(!parse(releaseJson(QStringLiteral("v1.2.3"), assets, true)).ok);
    QVERIFY(!parse(releaseJson(QStringLiteral("v1.2.3"), assets, false, true)).ok);
    QVERIFY(!parse(releaseJson(QStringLiteral("v1.3.0-beta.1"), assets)).ok);
    QVERIFY(!parse(releaseJson(QStringLiteral("v1.2.3"), {})).ok);
    QVERIFY(!GitHubRelease::fromJson("not json").ok);
    // A release with only the portable archive has nothing to install.
    const GitHubRelease noInstaller = parse(releaseJson(
        QStringLiteral("v1.2.3"),
        {assetJson(QStringLiteral("SpanTui-1.2.3-portable.zip"), QUrl(QStringLiteral("https://example.com/p.zip")))}));
    QVERIFY(!noInstaller.ok);
    QVERIFY(!noInstaller.error.isEmpty());
}

void UpdateServiceTest::comparesReleaseVersionsNumerically()
{
    QVERIFY(GitHubRelease::isNewer(QStringLiteral("0.10.0"), QStringLiteral("0.9.0")));
    QVERIFY(!GitHubRelease::isNewer(QStringLiteral("1.0.0"), QStringLiteral("1.0.0")));
    QVERIFY(!GitHubRelease::isNewer(QStringLiteral("0.9.9"), QStringLiteral("1.0.0")));
}

void UpdateServiceTest::remoteCheckFindsNewerRelease()
{
    QTemporaryDir feed;
    QVERIFY(feed.isValid());
    UpdateService service;
    QVERIFY(service.canCheck());
    service.setReleaseFeedUrl(
        writeFeed(feed,
                  releaseJson(QStringLiteral("v999.0.0"),
                              {assetJson(installerName(QStringLiteral("999.0.0")),
                                         QUrl::fromLocalFile(feed.filePath(QStringLiteral("s.exe"))))})));
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    QSignalSpy visible(&service, &UpdateService::visibleCheckFinished);

    service.checkNow(false, true);
    QVERIFY(service.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(service.updateAvailable());
    QCOMPARE(service.availableVersion(), QStringLiteral("999.0.0"));
    QVERIFY(service.sourceLocation().endsWith(QStringLiteral("/v999.0.0")));
    QCOMPARE(outcomeOf(finished), OperationOutcome::Succeeded);
    QCOMPARE(visible.constLast().at(0).toBool(), true);
}

void UpdateServiceTest::remoteCheckReportsOlderReleaseAndUnreachableFeed()
{
    QTemporaryDir feed;
    QVERIFY(feed.isValid());
    UpdateService service;
    service.setReleaseFeedUrl(
        writeFeed(feed,
                  releaseJson(QStringLiteral("v0.0.1"),
                              {assetJson(installerName(QStringLiteral("0.0.1")),
                                         QUrl::fromLocalFile(feed.filePath(QStringLiteral("s.exe"))))})));
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    service.checkNow(false, true);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(!service.updateAvailable());
    QCOMPARE(outcomeOf(finished), OperationOutcome::Succeeded);
    QVERIFY(!service.applyAndRestart());

    service.setReleaseFeedUrl(QUrl::fromLocalFile(feed.filePath(QStringLiteral("missing.json"))));
    service.checkNow(false, true);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(!service.updateAvailable());
    QCOMPARE(outcomeOf(finished, 1), OperationOutcome::Failed);
    QVERIFY(!finished.at(1).at(1).toString().isEmpty());
}

void UpdateServiceTest::localSourceReplacesRemote()
{
    QTemporaryDir feed;
    QTemporaryDir source;
    QVERIFY(feed.isValid());
    QVERIFY(source.isValid());
    QVERIFY(writeLocalPackage(source.path(), QStringLiteral("500.0.0")));
    UpdateService service;
    service.setReleaseFeedUrl(
        writeFeed(feed,
                  releaseJson(QStringLiteral("v999.0.0"),
                              {assetJson(installerName(QStringLiteral("999.0.0")),
                                         QUrl::fromLocalFile(feed.filePath(QStringLiteral("s.exe"))))})));
    service.setLocalSourcePath(source.path());
    // The folder alone does nothing while local updates are off.
    QVERIFY(!service.busy());

    service.setLocalSourceEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(service.updateAvailable());
    QCOMPARE(service.availableVersion(), QStringLiteral("500.0.0"));
    service.checkNow(false, true);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    // Only the local folder is consulted, never the newer release.
    QCOMPARE(service.availableVersion(), QStringLiteral("500.0.0"));

    // Switching back forgets the local result until GitHub is checked.
    service.setLocalSourceEnabled(false);
    QVERIFY(!service.busy());
    QVERIFY(!service.updateAvailable());
    QVERIFY(service.availableVersion().isEmpty());
    QVERIFY(!service.applyAndRestart());
    service.checkNow(false, true);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QCOMPARE(service.availableVersion(), QStringLiteral("999.0.0"));
}

void UpdateServiceTest::remoteApplyRefusesDigestMismatch()
{
    QTemporaryDir image;
    QTemporaryDir feed;
    QVERIFY(image.isValid() && feed.isValid());
    QVERIFY(writeInstalledImage(image.path()));
    const QString installer = feed.filePath(installerName(QStringLiteral("999.0.0")));
    QVERIFY(writeFile(installer, QByteArrayLiteral("not the published bytes")));
    UpdateService service;
    service.setImageDirectory(image.path());
    service.setReleaseFeedUrl(
        writeFeed(feed,
                  releaseJson(QStringLiteral("v999.0.0"),
                              {assetJson(installerName(QStringLiteral("999.0.0")), QUrl::fromLocalFile(installer),
                                         QStringLiteral("sha256:") + QString(64, QLatin1Char('0')))})));
    service.checkNow(false, false);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(service.updateAvailable());

    QSignalSpy started(&service, &UpdateService::progressStarted);
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    QSignalSpy failed(&service, &UpdateService::updateFailed);
    QVERIFY(service.applyAndRestart());
    QCOMPARE(started.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(outcomeOf(finished), OperationOutcome::Failed);
    // The download finished; the digest step failed and says why.
    QVERIFY(!service.applying());
    QCOMPARE(service.applySteps().size(), 3);
    QCOMPARE(service.applyStep(), 1);
    QCOMPARE(service.applyError(), failed.at(0).at(0).toString());
    // The rejected download is gone; the update stays on offer for a retry.
    QVERIFY(!QFileInfo::exists(QDir(updatesDirectory()).filePath(installerName(QStringLiteral("999.0.0")))));
    QVERIFY(service.updateAvailable());
}

void UpdateServiceTest::localApplyRefusesDigestMismatch()
{
    QTemporaryDir image;
    QTemporaryDir source;
    QVERIFY(image.isValid() && source.isValid());
    QVERIFY(writeInstalledImage(image.path()));
    QVERIFY(writeLocalPackage(source.path(), QStringLiteral("999.0.0"), {}, QByteArrayLiteral("damaged"),
                              QString(64, QLatin1Char('0'))));
    UpdateService service;
    service.setImageDirectory(image.path());
    useLocal(service);
    check(service, source.path());
    QVERIFY(service.updateAvailable());

    QSignalSpy failed(&service, &UpdateService::updateFailed);
    QVERIFY(service.applyAndRestart());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(service.applyStep(), 1);
    QCOMPARE(service.applyError(), failed.at(0).at(0).toString());
    QVERIFY(service.updateAvailable());
    QVERIFY(!QFileInfo::exists(recordedArgumentsPath()));
}

void UpdateServiceTest::localApplyStartsTheInstaller()
{
    QTemporaryDir image;
    QTemporaryDir source;
    QVERIFY(image.isValid() && source.isValid());
    QVERIFY(writeInstalledImage(image.path(), QStringLiteral("1.0.0")));
    const QByteArray installer = argvEchoChild();
    QVERIFY(!installer.isEmpty());
    QVERIFY(writeLocalPackage(source.path(), QStringLiteral("999.0.0"), {}, installer));
    // A previous update left its installer behind.
    const QString stale = QDir(updatesDirectory()).filePath(installerName(QStringLiteral("1.0.0")));
    QVERIFY(writeFile(stale, "old installer"));

    UpdateService service;
    service.setImageDirectory(image.path());
    useLocal(service);
    check(service, source.path());
    QVERIFY(service.updateAvailable());
    QSignalSpy applyStarted(&service, &UpdateService::applyStarted);
    QSignalSpy changed(&service, &UpdateService::progressChanged);
    QSignalSpy finished(&service, &UpdateService::progressFinished);
    QVERIFY(service.applyAndRestart());
    QCOMPARE(applyStarted.count(), 1);
    QVERIFY(service.applying());
    QCOMPARE(service.applySteps().size(), 3);
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
    QCOMPARE(outcomeOf(finished, finished.count() - 1), OperationOutcome::Succeeded);
    // Copy, verify, start: the last step ran and the overall fraction only grew.
    QCOMPARE(service.applyStep(), 2);
    QVERIFY(service.applyError().isEmpty());
    const QList<qreal> fractions = progressFractions(changed);
    QVERIFY(!fractions.isEmpty());
    QVERIFY(std::is_sorted(fractions.cbegin(), fractions.cend()));
    QVERIFY(fractions.constFirst() >= 0);
    QVERIFY(fractions.constLast() > 0);

    const QStringList arguments = recordedArguments();
    const QString log = QDir::toNativeSeparators(
        QDir(span::core::ProductPaths::logDirectory()).filePath(QStringLiteral("update-setup.log")));
    QCOMPARE(arguments,
             (QStringList{
                 QStringLiteral("/SILENT"),
                 QStringLiteral("/SUPPRESSMSGBOXES"),
                 QStringLiteral("/NORESTART"),
                 QStringLiteral("/CLOSEAPPLICATIONS"),
                 QStringLiteral("/WAITPID=%1").arg(QCoreApplication::applicationPid()),
                 QStringLiteral("/RELAUNCH=1"),
                 QStringLiteral("/LOG=%1").arg(log),
             }));
    // The staged copy is what ran; the earlier installer is gone.
    const QString staged = QDir(updatesDirectory()).filePath(installerName(QStringLiteral("999.0.0")));
    QCOMPARE(readFile(staged), installer);
    QVERIFY(!QFileInfo::exists(stale));
    QVERIFY(QFileInfo(QDir(span::core::ProductPaths::logDirectory()).path()).isDir());
}

void UpdateServiceTest::remoteApplyStartsTheDownloadedInstaller()
{
    QTemporaryDir image;
    QTemporaryDir feed;
    QVERIFY(image.isValid() && feed.isValid());
    QVERIFY(writeInstalledImage(image.path()));
    const QByteArray bytes = argvEchoChild();
    QVERIFY(!bytes.isEmpty());
    const QString published = feed.filePath(QStringLiteral("published.exe"));
    QVERIFY(writeFile(published, bytes));
    UpdateService service;
    service.setImageDirectory(image.path());
    service.setReleaseFeedUrl(
        writeFeed(feed,
                  releaseJson(QStringLiteral("v999.0.0"),
                              {assetJson(installerName(QStringLiteral("999.0.0")), QUrl::fromLocalFile(published),
                                         QStringLiteral("sha256:") + sha256Of(bytes))})));
    service.checkNow(false, false);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(service.updateAvailable());

    QSignalSpy finished(&service, &UpdateService::progressFinished);
    QVERIFY(service.applyAndRestart());
    // Download, verify, start.
    QCOMPARE(service.applySteps().size(), 3);
    QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
    QCOMPARE(outcomeOf(finished, finished.count() - 1), OperationOutcome::Succeeded);
    QCOMPARE(service.applyStep(), 2);
    const QStringList arguments = recordedArguments();
    QVERIFY2(arguments.contains(QStringLiteral("/SILENT")), qPrintable(arguments.join(' ')));
    QCOMPARE(readFile(QDir(updatesDirectory()).filePath(installerName(QStringLiteral("999.0.0")))), bytes);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Settings persist in <root>\settings.ini: keep them in a scratch root.
    QTemporaryDir dataRoot;
    if (!dataRoot.isValid()) {
        return 1;
    }
    span::core::ProductPaths::setRootOverride(dataRoot.path());
    QCoreApplication::setOrganizationName(QStringLiteral("SpanTuiTest"));
    QCoreApplication::setApplicationName(QStringLiteral("UpdateServiceTest"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    UpdateServiceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "update_service_test.moc"
