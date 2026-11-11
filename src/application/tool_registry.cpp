#include "application/tool_registry.h"

#include "application/workspace_settings.h"
#include "core/core_constants.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QVersionNumber>

namespace span::application {
namespace {

using span::core::ExternalTool;
using span::core::toolId;

constexpr int kProbeTimeoutMs = 5000;
constexpr ExternalTool kTools[] = {ExternalTool::Git, ExternalTool::Ssh, ExternalTool::Sftp, ExternalTool::Tailscale};

bool toolFromId(const QString &id, ExternalTool *tool)
{
    for (const ExternalTool candidate : kTools) {
        if (toolId(candidate) == id) {
            *tool = candidate;
            return true;
        }
    }
    return false;
}

// Porcelain v2 status (2.11), --absolute-git-dir (2.13) and
// --ignored=matching (2.16) are all required by the Git pages.
QVersionNumber minimumVersion(ExternalTool tool)
{
    return tool == ExternalTool::Git ? QVersionNumber(2, 16) : QVersionNumber();
}

QStringList probeArguments(ExternalTool tool)
{
    switch (tool) {
    case ExternalTool::Git:
        return {QStringLiteral("--version")};
    case ExternalTool::Ssh:
        return {QStringLiteral("-V")};
    case ExternalTool::Sftp:
        break;
    case ExternalTool::Tailscale: return {QStringLiteral("version")};
    }
    return {};
}

QString statusKey(ToolRegistry::Status status)
{
    switch (status) {
    case ToolRegistry::Status::Checking:
        return QStringLiteral("checking");
    case ToolRegistry::Status::Available:
        return QStringLiteral("available");
    case ToolRegistry::Status::Missing:
        return QStringLiteral("missing");
    case ToolRegistry::Status::Outdated:
        return QStringLiteral("outdated");
    case ToolRegistry::Status::Broken:
        return QStringLiteral("broken");
    case ToolRegistry::Status::InvalidOverride:
        return QStringLiteral("invalid");
    }
    return {};
}

} // namespace

ToolRegistry::ToolRegistry(WorkspaceSettings *settings, QObject *parent)
    : QAbstractListModel(parent),
      m_settings(settings)
{
    for (const ExternalTool tool : kTools) {
        Entry entry;
        entry.tool = tool;
        m_entries.push_back(entry);
    }
}

ToolRegistry::~ToolRegistry()
{
    for (Entry &item : m_entries) {
        if (item.probe) {
            item.probe->disconnect(this);
            item.probe->kill();
            item.probe->waitForFinished(span::core::kProcessKillWaitMs);
        }
    }
}

int ToolRegistry::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant ToolRegistry::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
        return {};
    }
    const Entry &item = m_entries.at(index.row());
    switch (role) {
    case ToolIdRole:
        return toolId(item.tool);
    case StatusRole:
        return statusKey(item.status);
    case VersionRole:
        return item.version;
    case PathRole:
        return item.path;
    case OverrideRole:
        return m_settings ? m_settings->toolPathOverrides().value(toolId(item.tool)).toString() : QString();
    case MinimumVersionRole: {
        const QVersionNumber minimum = minimumVersion(item.tool);
        return minimum.isNull() ? QString() : minimum.toString();
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> ToolRegistry::roleNames() const
{
    return {
        {ToolIdRole, "toolId"},
        {StatusRole, "status"},
        {VersionRole, "version"},
        {PathRole, "path"},
        {OverrideRole, "overridePath"},
        {MinimumVersionRole, "minimumVersion"},
    };
}

bool ToolRegistry::gitAvailable() const { return available(ExternalTool::Git); }
bool ToolRegistry::sshAvailable() const { return available(ExternalTool::Ssh); }
bool ToolRegistry::sftpAvailable() const { return available(ExternalTool::Sftp); }
bool ToolRegistry::tailscaleAvailable() const
{
    return available(ExternalTool::Tailscale);
}

quint32 ToolRegistry::availabilityBits() const
{
    quint32 bits = 0;
    for (const ExternalTool tool : kTools) {
        if (available(tool)) {
            bits |= 1u << int(tool);
        }
    }
    return bits;
}

bool ToolRegistry::checking() const
{
    for (const Entry &item : m_entries) {
        if (item.status == Status::Checking) {
            return true;
        }
    }
    return false;
}

// A found tool counts as available while its probe runs, so pages do not
// flash away at every launch; a failed probe withdraws it.
bool ToolRegistry::available(ExternalTool tool) const
{
    const Status current = entry(tool).status;
    return current == Status::Available || current == Status::Checking;
}

ToolRegistry::Status ToolRegistry::status(ExternalTool tool) const
{
    return entry(tool).status;
}

void ToolRegistry::refresh()
{
    const bool wasChecking = checking();
    const quint32 availableWas = availabilityBits();
    for (Entry &item : m_entries) {
        if (item.probe) {
            item.probe->disconnect(this);
            item.probe->kill();
            item.probe->deleteLater();
            item.probe = nullptr;
        }
        resolve(item);
        publish(item);
        if (item.status == Status::Checking) {
            startProbe(item);
        }
    }
    if (!m_entries.isEmpty()) {
        emit dataChanged(index(0), index(int(m_entries.size()) - 1));
    }
    settle(wasChecking, availableWas);
}

void ToolRegistry::setOverridePath(const QString &toolId, const QString &path)
{
    ExternalTool tool;
    if (!m_settings || !toolFromId(toolId, &tool)) {
        return;
    }
    QVariantMap overrides = m_settings->toolPathOverrides();
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        overrides.remove(toolId);
    } else {
        overrides.insert(toolId, QDir::toNativeSeparators(trimmed));
    }
    m_settings->setToolPathOverrides(overrides);
    refresh();
}

void ToolRegistry::setOverrideUrl(const QString &toolId, const QUrl &url)
{
    if (url.isLocalFile()) {
        setOverridePath(toolId, url.toLocalFile());
    }
}

QString ToolRegistry::parseVersion(ExternalTool tool, const QByteArray &output)
{
    const QString text = QString::fromUtf8(output);
    QRegularExpression pattern;
    switch (tool) {
    case ExternalTool::Git:
        // "git version 2.45.1.windows.1"
        pattern.setPattern(QStringLiteral(R"(git version (\d+\.\d+(?:\.\d+)?\S*))"));
        break;
    case ExternalTool::Ssh:
    case ExternalTool::Sftp:
        // "OpenSSH_for_Windows_9.5p1, LibreSSL 3.8.2"
        pattern.setPattern(QStringLiteral(R"(OpenSSH\w*?_(\d+\.\d+\w*))"));
        break;
    case ExternalTool::Tailscale:
        // "1.104.2\n  tailscale commit: …" — the version is the first line.
        pattern.setPattern(QStringLiteral(R"(\A\s*(\d+\.\d+(?:\.\d+)?)\s*$)"));
        pattern.setPatternOptions(QRegularExpression::MultilineOption);
        break;
    }
    const QRegularExpressionMatch match = pattern.match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}

bool ToolRegistry::meetsMinimum(ExternalTool tool, const QString &version)
{
    const QVersionNumber minimum = minimumVersion(tool);
    if (minimum.isNull()) {
        return true;
    }
    const QVersionNumber parsed = QVersionNumber::fromString(version);
    return !parsed.isNull() && parsed >= minimum;
}

void ToolRegistry::resolve(Entry &item)
{
    item.version.clear();
    const QString configured =
        m_settings ? m_settings->toolPathOverrides().value(toolId(item.tool)).toString().trimmed() : QString();
    if (!configured.isEmpty()) {
        // An explicit choice is honoured or reported, never silently
        // replaced by whatever auto-detection finds.
        const QFileInfo info(configured);
        item.path = QDir::toNativeSeparators(configured);
        item.status = QDir::isAbsolutePath(configured) && info.isFile() && info.isExecutable()
            ? Status::Checking : Status::InvalidOverride;
    } else {
        item.path = span::core::defaultToolExecutable(item.tool);
        item.status = item.path.isEmpty() ? Status::Missing : Status::Checking;
    }
    // sftp has no version switch; it ships with ssh and is ready once found.
    if (item.status == Status::Checking && probeArguments(item.tool).isEmpty()) {
        item.status = Status::Available;
    }
}

void ToolRegistry::startProbe(Entry &item)
{
    auto *probe = new QProcess(this);
    item.probe = probe;
    const ExternalTool tool = item.tool;
    probe->setProgram(item.path);
    probe->setArguments(probeArguments(tool));
    probe->setWorkingDirectory(QDir::tempPath());
    probe->setProcessChannelMode(QProcess::MergedChannels);
    connect(probe, &QProcess::finished, this,
            [this, tool, probe]() { finishProbe(tool, probe, true); });
    connect(probe, &QProcess::errorOccurred, this,
            [this, tool, probe](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finishProbe(tool, probe, false);
        }
    });
    // A hung probe (network-mounted binary, broken install) gets killed and
    // then reports as finished with a crash exit.
    QTimer::singleShot(kProbeTimeoutMs, probe, [probe]() { probe->kill(); });
    probe->start(QIODevice::ReadOnly);
}

void ToolRegistry::finishProbe(ExternalTool tool, QProcess *probe, bool ran)
{
    Entry &item = entry(tool);
    if (item.probe != probe) {
        return;
    }
    const bool wasChecking = checking();
    const quint32 availableWas = availabilityBits();
    item.probe = nullptr;
    probe->disconnect(this);
    probe->deleteLater();

    const bool succeeded = ran && probe->exitStatus() == QProcess::NormalExit
        && probe->exitCode() == 0;
    item.version = succeeded ? parseVersion(tool, probe->readAll()) : QString();
    if (!succeeded) {
        item.status = Status::Broken;
    } else if (!minimumVersion(tool).isNull() && item.version.isEmpty()) {
        item.status = Status::Broken;
    } else if (!meetsMinimum(tool, item.version)) {
        item.status = Status::Outdated;
    } else {
        item.status = Status::Available;
    }
    publish(item);
    updateRow(tool);
    settle(wasChecking, availableWas);
}

void ToolRegistry::publish(Entry &item)
{
    const QString usable = available(item.tool) ? item.path : QString();
    // The first publication always lands: until then core falls back to the
    // default location, which may differ from an invalid user override.
    if (item.publishedOnce && usable == item.published) {
        return;
    }
    item.publishedOnce = true;
    item.published = usable;
    span::core::setToolExecutable(item.tool, usable);
    emit executableChanged(item.tool);
}

void ToolRegistry::updateRow(ExternalTool tool)
{
    for (int row = 0; row < m_entries.size(); ++row) {
        if (m_entries.at(row).tool == tool) {
            emit dataChanged(index(row), index(row));
            return;
        }
    }
}

ToolRegistry::Entry &ToolRegistry::entry(ExternalTool tool)
{
    return m_entries[int(tool)];
}

const ToolRegistry::Entry &ToolRegistry::entry(ExternalTool tool) const
{
    return m_entries.at(int(tool));
}

void ToolRegistry::settle(bool wasChecking, quint32 availableWas)
{
    if (availableWas != availabilityBits()) {
        emit availabilityChanged();
    }
    if (wasChecking != checking()) {
        emit checkingChanged();
    }
}

} // namespace span::application
