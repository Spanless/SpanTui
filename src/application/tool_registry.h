#pragma once

#include "core/external_tools.h"

#include <QAbstractListModel>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVector>

class QProcess;

namespace span::application {

class WorkspaceSettings;

// Sole owner of external tool availability (Git, the OpenSSH ssh and sftp
// clients, the Tailscale CLI). For each tool it resolves the executable — the user's override
// from WorkspaceSettings, otherwise the default location — runs a version probe in
// the background and publishes the usable path to core::setToolExecutable.
// Features hang off the availability flags: an unavailable tool's pages are
// hidden and its commands are never started. One row per tool for the
// Settings → Environment page.
class ToolRegistry final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool gitAvailable READ gitAvailable NOTIFY availabilityChanged FINAL)
    Q_PROPERTY(bool sshAvailable READ sshAvailable NOTIFY availabilityChanged FINAL)
    Q_PROPERTY(bool sftpAvailable READ sftpAvailable NOTIFY availabilityChanged FINAL)
    Q_PROPERTY(bool tailscaleAvailable READ tailscaleAvailable NOTIFY availabilityChanged FINAL)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged FINAL)

public:
    enum class Status {
        Checking,        // found, version probe still running
        Available,
        Missing,         // nothing found
        Outdated,        // older than the minimum version
        Broken,          // found but did not run or answer the probe
        InvalidOverride, // the configured path is not an executable file
    };

    enum Role {
        ToolIdRole = Qt::UserRole + 1,
        StatusRole,
        VersionRole,
        PathRole,
        OverrideRole,
        MinimumVersionRole,
    };

    explicit ToolRegistry(WorkspaceSettings *settings, QObject *parent = nullptr);
    ~ToolRegistry() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool gitAvailable() const;
    bool sshAvailable() const;
    bool sftpAvailable() const;
    bool tailscaleAvailable() const;
    bool checking() const;
    bool available(span::core::ExternalTool tool) const;
    Status status(span::core::ExternalTool tool) const;

    // Resolves every tool again (picks up installs and PATH edits made
    // since launch) and restarts the version probes.
    Q_INVOKABLE void refresh();
    // "" clears the override and returns the tool to auto-detection.
    Q_INVOKABLE void setOverridePath(const QString &toolId, const QString &path);
    Q_INVOKABLE void setOverrideUrl(const QString &toolId, const QUrl &url);

    // Version text from the probe output ("" when unrecognised).
    static QString parseVersion(span::core::ExternalTool tool, const QByteArray &output);
    // Whether `version` satisfies the tool's minimum (tools without one: true).
    static bool meetsMinimum(span::core::ExternalTool tool, const QString &version);

signals:
    void availabilityChanged();
    void checkingChanged();
    // The published executable of `tool` changed (path, or usable/unusable).
    void executableChanged(span::core::ExternalTool tool);

private:
    struct Entry {
        span::core::ExternalTool tool;
        Status status = Status::Missing;
        QString path;
        QString version;
        QPointer<QProcess> probe;
        QString published;
        bool publishedOnce = false;
    };

    void resolve(Entry &entry);
    void startProbe(Entry &entry);
    void finishProbe(span::core::ExternalTool tool, QProcess *probe, bool ran);
    void publish(Entry &entry);
    void updateRow(span::core::ExternalTool tool);
    Entry &entry(span::core::ExternalTool tool);
    const Entry &entry(span::core::ExternalTool tool) const;
    // One bit per tool (bit = int(tool)) that is available.
    quint32 availabilityBits() const;
    void settle(bool wasChecking, quint32 availableWas);

    WorkspaceSettings *m_settings = nullptr;
    QVector<Entry> m_entries;
};

} // namespace span::application
