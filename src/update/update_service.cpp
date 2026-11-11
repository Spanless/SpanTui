#include "update/update_service.h"
#include "core/product_paths.h"
#include "platform/windows/process_containment.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPromise>
#include <QSaveFile>
#include <QSettings>
#include <QtConcurrent>

namespace {

constexpr auto kSettingsGroup = "update";
constexpr auto kLocalEnabledKey = "localSourceEnabled";
constexpr auto kLocalPathKey = "localSourcePath";
constexpr auto kAutoApplyKey = "autoApply";
constexpr int kApiTimeoutMs = 15000;
// Inactivity limit: a stalled download fails, a slow one does not.
constexpr int kDownloadTimeoutMs = 60000;
constexpr qint64 kChunkBytes = 1024 * 1024;
// QFuture progress is an int, so worker steps count KiB.
constexpr qint64 kProgressUnitBytes = 1024;

int progressUnits(qint64 bytes)
{
    return int((bytes + kProgressUnitBytes - 1) / kProgressUnitBytes);
}

QString megabytes(qint64 bytes)
{
    return QLocale().toString(double(bytes) / (1024.0 * 1024.0), 'f', 1);
}

void writeSetting(const char *key, const QVariant &value) {
    QSettings settings(span::core::ProductPaths::settingsPath(), QSettings::IniFormat);
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    settings.setValue(QString::fromLatin1(key), value);
    settings.endGroup();
}

QByteArray userAgent() {
    return QStringLiteral("SpanTui/%1").arg(QCoreApplication::applicationVersion()).toUtf8();
}

// Staged installers: <data root>\cache\updates. The one that ran stays until
// the next update stages its successor.
QString updatesDirectory() {
    return QDir(span::core::ProductPaths::cacheDirectory()).filePath(QStringLiteral("updates"));
}

} // namespace

UpdateService::UpdateService(QObject *parent) : QObject(parent) {
    QSettings settings(span::core::ProductPaths::settingsPath(), QSettings::IniFormat);
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    m_localSourceEnabled = settings.value(QString::fromLatin1(kLocalEnabledKey), false).toBool();
    m_localSourcePath = settings.value(QString::fromLatin1(kLocalPathKey)).toString();
    // Default off: manual check + confirm dialog is safer for day-to-day use.
    m_autoApply = settings.value(QString::fromLatin1(kAutoApplyKey), false).toBool();
    settings.endGroup();
    m_releaseFeedUrl = GitHubRelease::latestReleaseUrl();
    m_imageDirectory = QCoreApplication::applicationDirPath();
    m_imageKind = InstallerSource::imageKind(m_imageDirectory);
    m_currentVersion = QCoreApplication::applicationVersion();
    m_statusMessage = idleStatus();
}

UpdateService::~UpdateService()
{
    // A step still running must not start Setup after the window is gone.
    m_applyJob.cancel();
}

bool UpdateService::localSourceEnabled() const { return m_localSourceEnabled; }

void UpdateService::setLocalSourceEnabled(bool enabled) {
    if (m_localSourceEnabled == enabled) {
        return;
    }
    m_localSourceEnabled = enabled;
    writeSetting(kLocalEnabledKey, enabled);
    emit localSourceEnabledChanged();
    resetSource(tr("Update source changed"));
    if (m_localSourceEnabled && !m_localSourcePath.isEmpty()) {
        checkNow(false, false);
    }
}

QString UpdateService::localSourcePath() const { return m_localSourcePath; }

void UpdateService::setLocalSourceFolderUrl(const QUrl &url) {
    if (url.isLocalFile()) {
        setLocalSourcePath(url.toLocalFile());
    }
}

void UpdateService::setLocalSourcePath(const QString &path) {
    const QString normalized = QDir::cleanPath(path.trimmed());
    if (m_localSourcePath == normalized) {
        return;
    }
    m_localSourcePath = normalized;
    writeSetting(kLocalPathKey, m_localSourcePath);
    emit localSourcePathChanged();
    if (!m_localSourceEnabled) {
        return;
    }
    resetSource(tr("Update source changed"));
    setStatus(m_localSourcePath.isEmpty()
                  ? tr("Update source cleared.")
                  : tr("Update source set. Checking…"),
              false);
    if (!m_localSourcePath.isEmpty()) {
        checkNow(m_autoApply);
    }
}

bool UpdateService::autoApply() const { return m_autoApply; }

void UpdateService::setAutoApply(bool value) {
    if (m_autoApply == value) {
        return;
    }
    m_autoApply = value;
    writeSetting(kAutoApplyKey, m_autoApply);
    emit autoApplyChanged();
}

QString UpdateService::releaseRepository() const { return GitHubRelease::repository(); }
QUrl UpdateService::releaseFeedUrl() const { return m_releaseFeedUrl; }
void UpdateService::setReleaseFeedUrl(const QUrl &url) { m_releaseFeedUrl = url; }

QString UpdateService::imageDirectory() const
{
    return m_imageDirectory;
}

void UpdateService::setImageDirectory(const QString &directory)
{
    m_imageDirectory = directory;
    m_imageKind = InstallerSource::imageKind(directory);
    emit statusChanged();
}

bool UpdateService::canCheck() const {
    return !m_localSourceEnabled || !m_localSourcePath.isEmpty();
}
bool UpdateService::canInstall() const
{
    return m_imageKind == InstallerSource::ImageKind::Installed;
}
bool UpdateService::portable() const
{
    return m_imageKind == InstallerSource::ImageKind::Portable;
}
bool UpdateService::updateAvailable() const { return m_updateAvailable; }
bool UpdateService::busy() const { return m_busy; }
QString UpdateService::statusMessage() const { return m_statusMessage; }
QString UpdateService::currentVersion() const { return m_currentVersion; }
QString UpdateService::availableVersion() const { return m_availableVersion; }
QString UpdateService::sourceLocation() const { return m_sourceLocation; }

QUrl UpdateService::downloadPageUrl() const
{
    if (m_localSourceEnabled) {
        // The newest package's folder, which also holds its portable zip.
        return QUrl::fromLocalFile(m_localProbe.installer.isEmpty() ? m_localSourcePath
                                                                    : QFileInfo(m_localProbe.installer).absolutePath());
    }
    if (!m_release.pageUrl.isEmpty()) {
        return QUrl(m_release.pageUrl);
    }
    return QUrl(QStringLiteral("https://github.com/%1/releases/latest").arg(releaseRepository()));
}

bool UpdateService::applying() const
{
    return m_applying;
}

QStringList UpdateService::applySteps() const
{
    QStringList titles;
    for (const ApplyStep step : m_applySteps) {
        switch (step) {
        case ApplyStep::Download: titles.append(tr("Download the installer")); break;
        case ApplyStep::Copy: titles.append(tr("Copy the installer")); break;
        case ApplyStep::Verify: titles.append(tr("Verify the installer")); break;
        case ApplyStep::Launch: titles.append(tr("Start the installer")); break;
        }
    }
    return titles;
}

int UpdateService::applyStep() const
{
    return m_applyStep;
}
qreal UpdateService::applyStepProgress() const
{
    return m_applyStepProgress;
}
QString UpdateService::applyDetail() const
{
    return m_applyDetail;
}
QString UpdateService::applyError() const
{
    return m_applyError;
}

QString UpdateService::idleStatus() const {
    if (!m_localSourceEnabled) {
        return tr("Ready to check GitHub for a new stable release.");
    }
    return m_localSourcePath.isEmpty()
               ? tr("Choose a local build folder in Developer settings to enable local updates.")
               : tr("Ready to check for a newer local build.");
}

QString UpdateService::cannotInstallReason() const
{
    return portable() ? tr("Portable copies update by hand: unpack the new package and move the data folder into it.")
                      : tr("A development build does not install updates.");
}

void UpdateService::retranslate() {
    if (m_busy)
        return;
    if (m_updateAvailable) {
        setStatus(tr("Update available: %1 → %2").arg(m_currentVersion, m_availableVersion), true);
    } else {
        setStatus(m_lastReason.isEmpty() ? idleStatus() : m_lastReason, false);
    }
}

void UpdateService::setStatus(const QString &message, bool available) {
    m_statusMessage = message;
    m_updateAvailable = available;
    emit statusChanged();
}

void UpdateService::resetSource(const QString &cancelReason) {
    // An apply the user confirmed runs to completion against its own source.
    if (m_applying) {
        return;
    }
    if (m_busy) {
        ++m_generation;
        if (QNetworkReply *reply = m_reply.data()) {
            m_reply = nullptr;
            reply->abort();
        }
        m_busy = false;
        if (m_reportsProgress) {
            emit progressFinished(span::core::OperationOutcome::Cancelled, cancelReason);
        }
        m_reportsProgress = false;
    }
    m_pendingSource = Source::None;
    m_localProbe = {};
    m_release = {};
    m_lastReason.clear();
    m_availableVersion.clear();
    m_sourceLocation.clear();
    setStatus(idleStatus(), false);
}

QNetworkAccessManager *UpdateService::network() {
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    return m_network;
}

void UpdateService::checkNow(bool applyIfAvailable, bool reportProgress) {
    if (m_busy) {
        return;
    }
    m_applying = false;
    m_busy = true;
    m_reportsProgress = reportProgress;
    if (reportProgress) {
        emit progressStarted(tr("Checking for updates"));
        emit progressChanged(-1, m_localSourceEnabled
                                     ? tr("Inspecting the configured update source")
                                     : tr("Asking GitHub for the latest stable release"));
    }
    emit statusChanged();
    const quint64 generation = ++m_generation;
    if (m_localSourceEnabled) {
        checkLocal(generation, applyIfAvailable, reportProgress);
    } else {
        checkRemote(generation, applyIfAvailable, reportProgress);
    }
}

void UpdateService::checkLocal(quint64 generation, bool applyIfAvailable, bool reportProgress) {
    const QString sourcePath = m_localSourcePath;
    const QString imageDirectory = m_imageDirectory;
    const QString currentVersion = m_currentVersion;
    auto *watcher = new QFutureWatcher<InstallerSource::Result>(this);
    connect(watcher, &QFutureWatcher<InstallerSource::Result>::finished, this,
            [this, watcher, generation, applyIfAvailable, reportProgress]() {
                const InstallerSource::Result probe = watcher->result();
                watcher->deleteLater();
                if (generation != m_generation) return;
                m_localProbe = probe;
                m_release = {};
                m_pendingSource = probe.available ? Source::Local : Source::None;
                CheckResult result;
                result.available = probe.available;
                result.failed = probe.failed;
                result.reason = probe.reason;
                result.availableVersion = probe.availableVersion;
                result.location = probe.installer;
                finishCheck(result, applyIfAvailable, reportProgress);
            });
    watcher->setFuture(QtConcurrent::run([sourcePath, imageDirectory, currentVersion]() {
        return InstallerSource::probe(sourcePath, imageDirectory, currentVersion);
    }));
}

void UpdateService::checkRemote(quint64 generation, bool applyIfAvailable, bool reportProgress) {
    QNetworkRequest request(m_releaseFeedUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setTransferTimeout(kApiTimeoutMs);
    QNetworkReply *reply = network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation, applyIfAvailable, reportProgress]() {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        m_reply = nullptr;
        CheckResult result;
        GitHubRelease release;
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            // /releases/latest answers 404 until a stable release exists.
            if (httpStatus == 404) {
                result.reason = tr("No stable release has been published yet.");
            } else {
                result.failed = true;
                result.reason = tr("Could not query GitHub releases: %1")
                                    .arg(reply->errorString());
            }
        } else {
            release = GitHubRelease::fromJson(reply->readAll());
            if (!release.ok) {
                result.failed = true;
                result.reason = release.error;
            } else {
                result.availableVersion = release.version;
                result.location = release.pageUrl;
                result.available = GitHubRelease::isNewer(release.version, m_currentVersion);
                result.reason = result.available
                    ? tr("Stable release %1 is available.").arg(release.version)
                    : tr("Already on the latest stable release (%1).").arg(release.version);
            }
        }
        m_localProbe = {};
        m_release = result.available ? release : GitHubRelease{};
        m_pendingSource = result.available ? Source::Remote : Source::None;
        finishCheck(result, applyIfAvailable, reportProgress);
    });
}

void UpdateService::finishCheck(const CheckResult &result, bool applyIfAvailable,
                                bool reportProgress) {
    m_busy = false;
    m_reportsProgress = false;
    m_availableVersion = result.availableVersion;
    m_sourceLocation = result.location;
    m_lastReason = result.reason;

    if (!result.available) {
        setStatus(result.reason, false);
        if (reportProgress) {
            emit progressFinished(
                result.failed ? span::core::OperationOutcome::Failed
                              : span::core::OperationOutcome::Succeeded,
                result.failed ? result.reason : QString());
            emit visibleCheckFinished(false, result.reason);
        }
        return;
    }

    const QString message = tr("Update available: %1 → %2")
                                .arg(m_currentVersion, result.availableVersion);
    setStatus(message, true);
    if (reportProgress) {
        emit progressFinished(span::core::OperationOutcome::Succeeded, {});
        emit visibleCheckFinished(true, message);
    }

    // Startup/path-change callers explicitly opt into apply; an image that
    // cannot install only reports the update.
    if (applyIfAvailable && canInstall()) applyAndRestart();
}

bool UpdateService::applyAndRestart() {
    if (m_busy) {
        return false;
    }
    if (m_pendingSource == Source::None) {
        setStatus(m_lastReason.isEmpty() ? tr("No update to apply.") : m_lastReason, false);
        return false;
    }
    if (!canInstall()) {
        setStatus(cannotInstallReason(), true);
        return false;
    }

    m_applying = true;
    m_busy = true;
    m_reportsProgress = true;
    const quint64 generation = ++m_generation;
    const QString directory = updatesDirectory();
    const bool remote = m_pendingSource == Source::Remote;
    // The staged copy is what runs; a remote download is written there directly.
    m_stagedInstaller =
        QDir(directory).filePath(remote ? m_release.assetName : QFileInfo(m_localProbe.installer).fileName());
    m_stagedSha256 = remote ? m_release.assetSha256 : m_localProbe.sha256;
    m_applySteps = {remote ? ApplyStep::Download : ApplyStep::Copy};
    // GitHub lists no digest for assets uploaded before it computed them.
    if (!m_stagedSha256.isEmpty()) {
        m_applySteps.append(ApplyStep::Verify);
    }
    m_applySteps.append(ApplyStep::Launch);
    m_applyStep = 0;
    m_applyStepProgress = -1;
    m_applyDetail = QFileInfo(m_stagedInstaller).fileName();
    m_applyError.clear();
    emit progressStarted(tr("Updating to %1").arg(m_availableVersion));
    emit statusChanged();
    emit applyStarted();
    if (!QDir().mkpath(directory)) {
        failApply(tr("Cannot create the update download folder %1.")
                      .arg(QDir::toNativeSeparators(directory)));
        return false;
    }
    beginStep(generation, 0);
    return true;
}

void UpdateService::beginStep(quint64 generation, int index)
{
    const ApplyStep step = m_applySteps.at(index);
    m_applyStep = index;
    m_applyStepProgress = -1;
    // Windows scans a freshly written executable before it runs; for an
    // unsigned Setup that holds up its start for seconds.
    m_applyDetail = step == ApplyStep::Launch
        ? tr("Windows checks a new program before it first runs; this can take a few seconds.")
        : QFileInfo(m_stagedInstaller).fileName();
    publishApplyProgress();
    switch (step) {
    case ApplyStep::Download: downloadRelease(generation); break;
    case ApplyStep::Copy:
        watchStep(generation,
                  QtConcurrent::run(&UpdateService::copyInstaller, m_localProbe.installer, m_stagedInstaller));
        break;
    case ApplyStep::Verify:
        watchStep(generation, QtConcurrent::run(&UpdateService::verifyInstaller, m_stagedInstaller, m_stagedSha256));
        break;
    case ApplyStep::Launch:
        watchStep(generation,
                  QtConcurrent::run(&UpdateService::startInstaller, m_stagedInstaller, installerArguments(),
                                    span::core::ProductPaths::logDirectory()));
        break;
    }
}

void UpdateService::finishStep(quint64 generation)
{
    if (m_applyStep + 1 < m_applySteps.size()) {
        beginStep(generation, m_applyStep + 1);
        return;
    }
    // Setup is running and waits for this process to exit (/WAITPID).
    emit progressFinished(span::core::OperationOutcome::Succeeded, {});
    QCoreApplication::quit();
}

void UpdateService::watchStep(quint64 generation, const QFuture<QString> &job)
{
    m_applyJob = job;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::progressValueChanged, this, [this, watcher, generation](int value) {
        if (generation != m_generation || watcher->progressMaximum() <= 0) return;
        reportStepBytes(qint64(value) * kProgressUnitBytes, qint64(watcher->progressMaximum()) * kProgressUnitBytes);
    });
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, generation]() {
        watcher->deleteLater();
        if (watcher->isCanceled() || generation != m_generation) return;
        const QString error = watcher->result();
        if (!error.isEmpty()) {
            failApply(error);
            return;
        }
        finishStep(generation);
    });
    watcher->setFuture(job);
}

void UpdateService::reportStepBytes(qint64 done, qint64 total)
{
    // Reports at 0.1 % or 0.1 MB steps, which also paces a fast download.
    const QString detail = total > 0 ? tr("%1 of %2 MB").arg(megabytes(done), megabytes(total))
                                     : tr("%1 MB received").arg(megabytes(done));
    const qreal progress = total > 0 ? qBound<qreal>(0, qreal(done) / qreal(total), 1) : -1;
    if (detail == m_applyDetail && qAbs(progress - m_applyStepProgress) < 0.001) return;
    m_applyDetail = detail;
    m_applyStepProgress = progress;
    publishApplyProgress();
}

void UpdateService::publishApplyProgress()
{
    emit applyProgressChanged();
    const QString step = applySteps().value(m_applyStep);
    emit progressChanged(applyFraction(), m_applyDetail.isEmpty() ? step : tr("%1: %2").arg(step, m_applyDetail));
}

qreal UpdateService::applyFraction() const
{
    // A remote update is mostly its download; starting Setup is a short tail.
    const auto weight = [](ApplyStep step) {
        switch (step) {
        case ApplyStep::Download: return 8;
        case ApplyStep::Copy:
        case ApplyStep::Verify: return 2;
        case ApplyStep::Launch: return 1;
        }
        return 1;
    };
    int total = 0;
    qreal done = 0;
    for (int i = 0; i < m_applySteps.size(); ++i) {
        const int share = weight(m_applySteps.at(i));
        total += share;
        if (i < m_applyStep) {
            done += share;
        } else if (i == m_applyStep && m_applyStepProgress > 0) {
            done += share * m_applyStepProgress;
        }
    }
    return total > 0 ? done / total : 0;
}

void UpdateService::downloadRelease(quint64 generation)
{
    m_download = std::make_unique<QSaveFile>(m_stagedInstaller);
    if (!m_download->open(QIODevice::WriteOnly)) {
        const QString error = m_download->errorString();
        m_download.reset();
        failApply(tr("Could not download the update: %1").arg(error));
        return;
    }

    QNetworkRequest request(m_release.assetUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader("Accept", "application/octet-stream");
    request.setTransferTimeout(kDownloadTimeoutMs);
    QNetworkReply *reply = network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, generation]() {
        if (generation != m_generation || !m_download)
            return;
        if (m_download->write(reply->readAll()) < 0)
            reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, generation](qint64 received, qint64 total) {
        if (generation == m_generation) reportStepBytes(received, total);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
        reply->deleteLater();
        if (generation != m_generation || !m_download)
            return;
        m_reply = nullptr;
        if (reply->error() == QNetworkReply::NoError && m_download->error() == QFileDevice::NoError)
            m_download->write(reply->readAll());
        if (m_download->error() != QFileDevice::NoError
            || reply->error() != QNetworkReply::NoError || !m_download->commit()) {
            const QString error = m_download->error() != QFileDevice::NoError
                ? m_download->errorString()
                : reply->errorString();
            m_download->cancelWriting();
            m_download.reset();
            failApply(tr("Could not download the update: %1").arg(error));
            return;
        }
        m_download.reset();
        finishStep(generation);
    });
}

QStringList UpdateService::installerArguments() const
{
    return {
        // Setup shows only its progress window; message boxes take their
        // default answer.
        QStringLiteral("/SILENT"),
        QStringLiteral("/SUPPRESSMSGBOXES"),
        QStringLiteral("/NORESTART"),
        QStringLiteral("/CLOSEAPPLICATIONS"),
        // installer/SpanTui.iss waits for this process before it looks for
        // files in use, and starts the new version afterwards.
        QStringLiteral("/WAITPID=%1").arg(QCoreApplication::applicationPid()),
        QStringLiteral("/RELAUNCH=1"),
        QStringLiteral("/LOG=%1").arg(QDir::toNativeSeparators(
            QDir(span::core::ProductPaths::logDirectory()).filePath(QStringLiteral("update-setup.log")))),
    };
}

void UpdateService::copyInstaller(QPromise<QString> &promise, const QString &source, const QString &target)
{
    const QString failure =
        tr("Could not copy the installer %1 into the update folder.").arg(QDir::toNativeSeparators(source));
    QFile input(source);
    // An unfinished copy never replaces the target.
    QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        promise.addResult(failure);
        return;
    }
    promise.setProgressRange(0, progressUnits(input.size()));
    QByteArray buffer(kChunkBytes, Qt::Uninitialized);
    qint64 copied = 0;
    while (!promise.isCanceled()) {
        const qint64 read = input.read(buffer.data(), buffer.size());
        if (read < 0 || output.write(buffer.constData(), read) != read) {
            promise.addResult(failure);
            return;
        }
        if (read == 0) {
            promise.addResult(output.commit() ? QString() : failure);
            return;
        }
        copied += read;
        promise.setProgressValue(progressUnits(copied));
    }
}

void UpdateService::verifyInstaller(QPromise<QString> &promise, const QString &installer, const QString &sha256)
{
    const QString failure = tr("Could not read the installer %1.").arg(QDir::toNativeSeparators(installer));
    QFile file(installer);
    if (!file.open(QIODevice::ReadOnly)) {
        promise.addResult(failure);
        return;
    }
    promise.setProgressRange(0, progressUnits(file.size()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(kChunkBytes, Qt::Uninitialized);
    qint64 hashed = 0;
    for (;;) {
        if (promise.isCanceled()) return;
        const qint64 read = file.read(buffer.data(), buffer.size());
        if (read < 0) {
            promise.addResult(failure);
            return;
        }
        if (read == 0) break;
        hash.addData(QByteArrayView(buffer.constData(), read));
        hashed += read;
        promise.setProgressValue(progressUnits(hashed));
    }
    file.close();
    // The staged copy is what runs, so it is the one verified. The digest
    // shares a source with the installer: it catches damage, not tampering.
    if (QString::fromLatin1(hash.result().toHex()).compare(sha256, Qt::CaseInsensitive) != 0) {
        QFile::remove(installer);
        promise.addResult(tr("The installer does not match the digest its source lists."));
        return;
    }
    promise.addResult(QString());
}

void UpdateService::startInstaller(QPromise<QString> &promise, const QString &installer, const QStringList &arguments,
                                   const QString &logDirectory)
{
    // Earlier installers, and the package tree the former updater extracted
    // into a data root other than the default one Setup cleans.
    const QFileInfo staged(installer);
    for (const QFileInfo &entry :
         staged.absoluteDir().entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (entry.fileName().compare(staged.fileName(), Qt::CaseInsensitive) == 0) continue;
        if (entry.isDir() && !entry.isSymLink())
            QDir(entry.absoluteFilePath()).removeRecursively();
        else
            QFile::remove(entry.absoluteFilePath());
    }
    // Inno Setup aborts when it cannot create its log file.
    QDir().mkpath(logDirectory);
    if (promise.isCanceled()) return;
    // Setup must outlive this process and its kill-on-close job. Its working
    // directory is never the install directory, which Setup replaces.
    QString error;
    const quint32 pid =
        span::platform::process::startUnaffiliatedProcess(installer, arguments, QDir::tempPath(), &error);
    if (pid == 0) {
        promise.addResult(error.isEmpty() ? tr("Could not start the installer.")
                                          : tr("Could not start the installer: %1").arg(error));
        return;
    }
    promise.addResult(QString());
}

void UpdateService::failApply(const QString &message) {
    m_busy = false;
    m_applying = false;
    m_reportsProgress = false;
    m_applyError = message;
    m_applyJob = {};
    // The update stays on offer: the user may retry after fixing the cause.
    setStatus(message, true);
    emit applyProgressChanged();
    emit updateFailed(message);
    emit progressFinished(span::core::OperationOutcome::Failed, message);
}
