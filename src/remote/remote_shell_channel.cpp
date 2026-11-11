#include "remote/remote_shell_channel.h"

#include "core/core_constants.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>

#include <utility>

namespace span::remote {
namespace {

constexpr char kMarkStart = '\x01';
constexpr char kMarkEnd = '\x02';
constexpr qsizetype kMaxStderr = 4096;
// A listing of a few hundred thousand entries stays well below this; past
// it the channel is treated as broken rather than buffering without bound.
constexpr qsizetype kMaxFrame = 256 * 1024 * 1024;

QByteArray randomNonce()
{
    const quint64 value = QRandomGenerator::system()->generate64();
    return QByteArray::number(value, 16).rightJustified(16, '0');
}

} // namespace

RemoteShellFrameParser::RemoteShellFrameParser(QByteArray nonce)
    : m_nonce(std::move(nonce))
{
}

QByteArray RemoteShellFrameParser::wrap(quint64 id, const QString &script) const
{
    const QByteArray tag = m_nonce + ':' + QByteArray::number(id);
    return "printf '\\001%s\\002' '" + tag + ":B'\n(\n" + script.toUtf8()
        + "\n) </dev/null\nprintf '\\001%s:%d\\002' '" + tag + ":E' \"$?\"\n";
}

QVector<RemoteShellFrameParser::Frame> RemoteShellFrameParser::feed(const QByteArray &bytes)
{
    QVector<Frame> frames;
    m_buffer.append(bytes);
    const QByteArray prefix = QByteArray(1, kMarkStart) + m_nonce + ':';
    while (true) {
        if (!m_inFrame) {
            const qsizetype begin = m_buffer.indexOf(prefix);
            if (begin < 0) {
                // Login noise before the first marker: keep only a possible
                // partial prefix at the tail.
                m_buffer = m_buffer.right(prefix.size() - 1);
                return frames;
            }
            const qsizetype close = m_buffer.indexOf(kMarkEnd, begin);
            if (close < 0) {
                m_buffer.remove(0, begin);
                return frames;
            }
            const QByteArray header = m_buffer.mid(begin + prefix.size(),
                                                   close - begin - prefix.size());
            m_buffer.remove(0, close + 1);
            const QList<QByteArray> fields = header.split(':');
            bool ok = false;
            const quint64 id = fields.value(0).toULongLong(&ok);
            if (!ok || fields.size() != 2 || fields.at(1) != "B") {
                continue;
            }
            m_inFrame = true;
            m_frameId = id;
            m_scanFrom = 0;
        }

        const QByteArray endPrefix = prefix + QByteArray::number(m_frameId) + ":E:";
        const qsizetype end = m_buffer.indexOf(endPrefix, m_scanFrom);
        if (end < 0) {
            m_scanFrom = qMax<qsizetype>(0, m_buffer.size() - endPrefix.size());
            return frames;
        }
        const qsizetype close = m_buffer.indexOf(kMarkEnd, end + endPrefix.size());
        if (close < 0) {
            m_scanFrom = end;
            return frames;
        }
        Frame frame;
        frame.id = m_frameId;
        bool ok = false;
        frame.exitCode = m_buffer.mid(end + endPrefix.size(),
                                      close - end - endPrefix.size()).toInt(&ok);
        if (!ok) {
            frame.exitCode = -1;
        }
        frame.payload = m_buffer.left(end);
        m_buffer.remove(0, close + 1);
        m_inFrame = false;
        m_scanFrom = 0;
        frames.push_back(std::move(frame));
    }
}

RemoteShellChannel::RemoteShellChannel(QObject *parent) : QObject(parent) {}

RemoteShellChannel::~RemoteShellChannel()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(span::core::kProcessKillWaitMs);
    }
}

void RemoteShellChannel::setLaunch(const QString &program, const QStringList &arguments,
                                   const QHash<QString, QString> &environment)
{
    m_program = program;
    m_arguments = arguments;
    m_environment = environment;
}

quint64 RemoteShellChannel::submit(const QString &script)
{
    if (!m_process) {
        start();
    }
    const quint64 id = m_nextId++;
    if (!m_process) {
        QMetaObject::invokeMethod(this, [this, id, message = m_errorMessage]() {
            emit requestFailed(id, message);
        }, Qt::QueuedConnection);
        return id;
    }
    m_outstanding.insert(id);
    m_process->write(m_parser->wrap(id, script));
    return id;
}

void RemoteShellChannel::stop()
{
    terminate(QString(), State::Idle);
}

void RemoteShellChannel::terminate(const QString &message, State state)
{
    if (!m_process) {
        return;
    }
    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    process->kill();
    process->waitForFinished(span::core::kProcessKillWaitMs);
    process->deleteLater();
    m_errorMessage = message;
    const QSet<quint64> outstanding = std::exchange(m_outstanding, {});
    setState(state);
    for (const quint64 id : outstanding) {
        emit requestFailed(id, message);
    }
}

void RemoteShellChannel::start()
{
    if (m_program.isEmpty()) {
        m_errorMessage = tr("The OpenSSH client (ssh.exe) was not found.");
        setState(State::Failed);
        return;
    }
    m_parser = std::make_unique<RemoteShellFrameParser>(randomNonce());
    m_stderr.clear();
    m_errorMessage.clear();
    auto *process = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (auto it = m_environment.cbegin(); it != m_environment.cend(); ++it) {
        environment.insert(it.key(), it.value());
    }
    process->setProcessEnvironment(environment);
    process->setProgram(m_program);
    process->setArguments(m_arguments);
    connect(process, &QProcess::readyReadStandardOutput, this,
            &RemoteShellChannel::readOutput);
    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        m_stderr.append(process->readAllStandardError());
        if (m_stderr.size() > kMaxStderr) {
            m_stderr = m_stderr.right(kMaxStderr);
        }
    });
    connect(process, &QProcess::finished, this, &RemoteShellChannel::handleFinished);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            handleFinished();
        }
    });
    m_process = process;
    setState(State::Connecting);
    process->start();
}

void RemoteShellChannel::readOutput()
{
    if (!m_process || !m_parser) {
        return;
    }
    const QByteArray bytes = m_process->readAllStandardOutput();
    const QVector<RemoteShellFrameParser::Frame> frames = m_parser->feed(bytes);
    if (m_parser->bufferedBytes() > kMaxFrame) {
        terminate(tr("The remote reply is too large."), State::Failed);
        return;
    }
    for (const RemoteShellFrameParser::Frame &frame : frames) {
        if (!m_outstanding.remove(frame.id)) {
            continue;
        }
        if (m_state != State::Ready) {
            setState(State::Ready);
        }
        emit replied(frame.id, frame.exitCode, frame.payload);
    }
}

void RemoteShellChannel::handleFinished()
{
    if (!m_process) {
        return;
    }
    QProcess *process = m_process;
    m_process = nullptr;
    process->disconnect(this);
    process->deleteLater();
    const QString detail = QString::fromLocal8Bit(m_stderr).trimmed();
    m_errorMessage = detail.isEmpty()
        ? (process->error() == QProcess::FailedToStart
               ? tr("Could not start the OpenSSH client.")
               : tr("The SSH connection closed."))
        : detail;
    const QSet<quint64> outstanding = std::exchange(m_outstanding, {});
    setState(State::Failed);
    for (const quint64 id : outstanding) {
        emit requestFailed(id, m_errorMessage);
    }
}

void RemoteShellChannel::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

} // namespace span::remote
