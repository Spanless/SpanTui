#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <memory>

class QProcess;

namespace span::remote {

// Markers carry a per-channel random nonce, so rc-file noise and payload
// bytes cannot pass for a frame boundary.
class RemoteShellFrameParser {
public:
    struct Frame {
        quint64 id = 0;
        int exitCode = -1;
        QByteArray payload;
    };

    explicit RemoteShellFrameParser(QByteArray nonce);

    // Shell text that runs `script` in a subshell with stdin closed and
    // brackets its output with the markers feed() looks for.
    QByteArray wrap(quint64 id, const QString &script) const;
    QVector<Frame> feed(const QByteArray &bytes);
    qsizetype bufferedBytes() const { return m_buffer.size(); }

private:
    QByteArray m_nonce;
    QByteArray m_buffer;
    bool m_inFrame = false;
    quint64 m_frameId = 0;
    qsizetype m_scanFrom = 0;
};

// One long-lived `ssh -T host exec /bin/sh`; requests run strictly in order.
// A dead process fails every outstanding request; the next one reconnects.
class RemoteShellChannel final : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,
        Connecting,
        Ready,
        Failed,
    };
    Q_ENUM(State)

    explicit RemoteShellChannel(QObject *parent = nullptr);
    ~RemoteShellChannel() override;

    void setLaunch(const QString &program, const QStringList &arguments,
                   const QHash<QString, QString> &environment);
    State state() const { return m_state; }
    QString errorMessage() const { return m_errorMessage; }

    quint64 submit(const QString &script);
    void stop();

signals:
    void stateChanged();
    void replied(quint64 id, int exitCode, const QByteArray &payload);
    void requestFailed(quint64 id, const QString &message);

private:
    void start();
    void terminate(const QString &message, State state);
    void readOutput();
    void handleFinished();
    void setState(State state);

    QString m_program;
    QStringList m_arguments;
    QHash<QString, QString> m_environment;
    QProcess *m_process = nullptr;
    std::unique_ptr<RemoteShellFrameParser> m_parser;
    QByteArray m_stderr;
    QSet<quint64> m_outstanding;
    quint64 m_nextId = 1;
    State m_state = State::Idle;
    QString m_errorMessage;
};

} // namespace span::remote
