#include "remote/remote_transfer_queue.h"

#include "core/core_constants.h"
#include "remote/ssh_command.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

#include <algorithm>

namespace span::remote {
namespace {

constexpr qsizetype kMaxOutput = 4096;

QString lastLines(const QByteArray &output)
{
    QStringList lines = QString::fromLocal8Bit(output).split(QLatin1Char('\n'),
                                                             Qt::SkipEmptyParts);
    for (QString &line : lines) {
        line = line.trimmed();
    }
    lines.removeAll(QString());
    // The batch echo ("sftp> put ...") is noise next to the error itself.
    lines.erase(std::remove_if(lines.begin(), lines.end(), [](const QString &line) {
                    return line.startsWith(QStringLiteral("sftp>"));
                }),
                lines.end());
    return lines.mid(qMax<qsizetype>(0, lines.size() - 3)).join(QLatin1Char('\n'));
}

} // namespace

RemoteTransferQueue::RemoteTransferQueue(QObject *parent) : QObject(parent) {}

RemoteTransferQueue::~RemoteTransferQueue()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(span::core::kProcessKillWaitMs);
    }
}

void RemoteTransferQueue::setLaunch(const QString &program, const QStringList &arguments,
                                    const QHash<QString, QString> &environment)
{
    m_launch = Launch{program, arguments, environment};
}

void RemoteTransferQueue::enqueue(Direction direction, const QStringList &sources,
                                  const QString &destination)
{
    if (sources.isEmpty() || destination.isEmpty()) {
        return;
    }
    m_jobs.enqueue(Job{direction, sources, destination, m_launch});
    if (!m_process) {
        startNext();
    }
}

QByteArray RemoteTransferQueue::batchScript(Direction direction, const QStringList &sources,
                                            const QString &destination)
{
    QByteArray script;
    const bool upload = direction == Direction::Upload;
    const QString target = upload ? destination : QDir::fromNativeSeparators(destination);
    for (const QString &source : sources) {
        const QString from = upload ? QDir::fromNativeSeparators(source) : source;
        script += (upload ? "put -r " : "get -r ") + sftpQuote(from).toUtf8() + ' '
            + sftpQuote(target).toUtf8() + '\n';
    }
    return script;
}

void RemoteTransferQueue::startNext()
{
    if (m_jobs.isEmpty()) {
        return;
    }
    m_current = m_jobs.dequeue();
    const bool upload = m_current.direction == Direction::Upload;
    const QString firstName = QFileInfo(m_current.sources.constFirst()).fileName();
    const QString title = m_current.sources.size() == 1
        ? (upload ? tr("Uploading %1").arg(firstName) : tr("Downloading %1").arg(firstName))
        : (upload ? tr("Uploading %n items", nullptr, int(m_current.sources.size()))
                  : tr("Downloading %n items", nullptr, int(m_current.sources.size())));
    emit progressStarted(title);
    const Launch &launch = m_current.launch;
    if (launch.program.isEmpty()) {
        finishCurrent(false, tr("The OpenSSH client (sftp.exe) was not found."));
        return;
    }

    m_output.clear();
    auto *process = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (auto it = launch.environment.cbegin(); it != launch.environment.cend(); ++it) {
        environment.insert(it.key(), it.value());
    }
    process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setProgram(launch.program);
    process->setArguments(launch.arguments);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        m_output.append(process->readAllStandardOutput());
        if (m_output.size() > kMaxOutput) {
            m_output = m_output.right(kMaxOutput);
        }
    });
    connect(process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const bool succeeded = status == QProcess::NormalExit && exitCode == 0;
        finishCurrent(succeeded, succeeded ? QString() : lastLines(m_output));
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finishCurrent(false, tr("Could not start the OpenSSH client."));
        }
    });
    m_process = process;
    process->start();
    // A failed start is reported synchronously and already moved the queue on.
    if (m_process != process) {
        return;
    }
    process->write(batchScript(m_current.direction, m_current.sources,
                               m_current.destination));
    process->closeWriteChannel();
}

void RemoteTransferQueue::finishCurrent(bool succeeded, const QString &message)
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    const QString detail = succeeded || !message.isEmpty()
        ? message : tr("The transfer failed.");
    emit progressFinished(succeeded ? span::core::OperationOutcome::Succeeded
                                    : span::core::OperationOutcome::Failed,
                          detail);
    emit transferFinished(m_current.direction, m_current.destination, succeeded, detail);
    startNext();
}

} // namespace span::remote
