#pragma once

#include "core/operation_outcome.h"
#include "update/github_release.h"
#include "update/installer_source.h"

#include <QFuture>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
template <typename T> class QPromise;

// See docs/services/update/updater.md.
class UpdateService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool localSourceEnabled READ localSourceEnabled WRITE setLocalSourceEnabled NOTIFY
                   localSourceEnabledChanged FINAL)
    Q_PROPERTY(
        QString localSourcePath READ localSourcePath WRITE setLocalSourcePath NOTIFY localSourcePathChanged FINAL)
    Q_PROPERTY(bool autoApply READ autoApply WRITE setAutoApply NOTIFY autoApplyChanged FINAL)
    Q_PROPERTY(QString releaseRepository READ releaseRepository CONSTANT FINAL)
    Q_PROPERTY(bool canCheck READ canCheck NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool portable READ portable NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY statusChanged FINAL)
    // Local: the installer found in the folder. Remote: the release page.
    Q_PROPERTY(QString sourceLocation READ sourceLocation NOTIFY statusChanged FINAL)
    // For images that cannot install: the release page or the local folder.
    Q_PROPERTY(QUrl downloadPageUrl READ downloadPageUrl NOTIFY statusChanged FINAL)
    // applyStepProgress is -1 while unknown; applyError lasts until the next
    // install starts.
    Q_PROPERTY(bool applying READ applying NOTIFY applyProgressChanged FINAL)
    Q_PROPERTY(QStringList applySteps READ applySteps NOTIFY applyProgressChanged FINAL)
    Q_PROPERTY(int applyStep READ applyStep NOTIFY applyProgressChanged FINAL)
    Q_PROPERTY(qreal applyStepProgress READ applyStepProgress NOTIFY applyProgressChanged FINAL)
    Q_PROPERTY(QString applyDetail READ applyDetail NOTIFY applyProgressChanged FINAL)
    Q_PROPERTY(QString applyError READ applyError NOTIFY applyProgressChanged FINAL)

public:
    explicit UpdateService(QObject *parent = nullptr);
    ~UpdateService() override;

    bool localSourceEnabled() const;
    void setLocalSourceEnabled(bool enabled);
    QString localSourcePath() const;
    void setLocalSourcePath(const QString &path);
    // Shares keep their server (file://server/share -> //server/share);
    // non-file URLs are ignored.
    Q_INVOKABLE void setLocalSourceFolderUrl(const QUrl &url);
    bool autoApply() const;
    void setAutoApply(bool value);
    QString releaseRepository() const;

    // Test seams.
    QUrl releaseFeedUrl() const;
    void setReleaseFeedUrl(const QUrl &url);
    QString imageDirectory() const;
    void setImageDirectory(const QString &directory);

    bool canCheck() const;
    bool canInstall() const;
    bool portable() const;
    bool updateAvailable() const;
    bool busy() const;
    QString statusMessage() const;
    QString currentVersion() const;
    QString availableVersion() const;
    QString sourceLocation() const;
    QUrl downloadPageUrl() const;
    bool applying() const;
    QStringList applySteps() const;
    int applyStep() const;
    qreal applyStepProgress() const;
    QString applyDetail() const;
    QString applyError() const;

    // Manual UI passes false, so the user confirms before applyAndRestart().
    Q_INVOKABLE void checkNow(bool applyIfAvailable = false,
                              bool reportProgress = true);
    Q_INVOKABLE bool applyAndRestart();
    void retranslate();

signals:
    void localSourceEnabledChanged();
    void localSourcePathChanged();
    void autoApplyChanged();
    void statusChanged();
    void visibleCheckFinished(bool available, const QString &message);
    void applyStarted();
    void applyProgressChanged();
    void updateFailed(const QString &message);
    void progressStarted(const QString &title);
    void progressChanged(qreal fraction, const QString &detail);
    void progressFinished(span::core::OperationOutcome outcome,
                          const QString &detail);

private:
    enum class Source { None, Local, Remote };
    enum class ApplyStep { Download, Copy, Verify, Launch };

    struct CheckResult {
        bool available = false;
        bool failed = false;
        QString reason;
        QString availableVersion;
        QString location;
    };

    void checkLocal(quint64 generation, bool applyIfAvailable, bool reportProgress);
    void checkRemote(quint64 generation, bool applyIfAvailable, bool reportProgress);
    void finishCheck(const CheckResult &result, bool applyIfAvailable, bool reportProgress);
    void downloadRelease(quint64 generation);
    // After the last step (Setup started) the application quits.
    void beginStep(quint64 generation, int index);
    void finishStep(quint64 generation);
    // Worker progress counts KiB processed.
    void watchStep(quint64 generation, const QFuture<QString> &job);
    void reportStepBytes(qint64 done, qint64 total);
    void publishApplyProgress();
    qreal applyFraction() const;
    QStringList installerArguments() const;
    // Worker threads: each adds a translated error, empty on success, and
    // stops early when cancelled on destruction.
    static void copyInstaller(QPromise<QString> &promise, const QString &source, const QString &target);
    // Removes `installer` on a digest mismatch.
    static void verifyInstaller(QPromise<QString> &promise, const QString &installer, const QString &sha256);
    // Removes everything else in the updates folder first.
    static void startInstaller(QPromise<QString> &promise, const QString &installer, const QStringList &arguments,
                               const QString &logDirectory);
    void failApply(const QString &message);
    // Drops an in-flight check, never an apply.
    void resetSource(const QString &cancelReason);
    QString idleStatus() const;
    QString cannotInstallReason() const;
    QNetworkAccessManager *network();
    void setStatus(const QString &message, bool available);

    bool m_localSourceEnabled = false;
    QString m_localSourcePath;
    bool m_autoApply = false;
    QUrl m_releaseFeedUrl;
    QString m_imageDirectory;
    InstallerSource::ImageKind m_imageKind = InstallerSource::ImageKind::Development;
    bool m_busy = false;
    bool m_applying = false;
    bool m_updateAvailable = false;
    QString m_statusMessage;
    QString m_lastReason;
    QString m_currentVersion;
    QString m_availableVersion;
    QString m_sourceLocation;
    Source m_pendingSource = Source::None;
    InstallerSource::Result m_localProbe;
    GitHubRelease m_release;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QSaveFile> m_download;
    QList<ApplyStep> m_applySteps;
    int m_applyStep = -1;
    qreal m_applyStepProgress = -1;
    QString m_applyDetail;
    QString m_applyError;
    QString m_stagedInstaller;
    QString m_stagedSha256;
    QFuture<QString> m_applyJob;
    quint64 m_generation = 0;
    bool m_reportsProgress = false;
};
