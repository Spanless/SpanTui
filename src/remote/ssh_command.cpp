#include "remote/ssh_command.h"

#include "core/external_tools.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace span::remote {
namespace {

QStringList commonOptions(const SshEndpoint &endpoint)
{
    QStringList arguments{
        QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=30"),
        QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=3"),
        QStringLiteral("-o"), QStringLiteral("ConnectTimeout=15"),
    };
    if (!endpoint.identityFile.isEmpty()) {
        arguments << QStringLiteral("-i")
                  << QDir::toNativeSeparators(endpoint.identityFile)
                  << QStringLiteral("-o") << QStringLiteral("IdentitiesOnly=yes");
    }
    return arguments;
}

QStringList destination(const SshEndpoint &endpoint)
{
    QStringList arguments{QStringLiteral("-p"), QString::number(endpoint.port)};
    if (!endpoint.user.isEmpty()) {
        arguments << QStringLiteral("-l") << endpoint.user;
    }
    arguments << QStringLiteral("--") << endpoint.host;
    return arguments;
}

} // namespace

EndpointError checkEndpoint(const SshEndpoint &endpoint)
{
    static const QRegularExpression hostPattern(
        QStringLiteral(R"(^[A-Za-z0-9._:\-]+$)"));
    static const QRegularExpression userPattern(
        QStringLiteral(R"(^[A-Za-z0-9._@\\$+\-]+$)"));
    if (endpoint.host.isEmpty() || endpoint.host.size() > 253
        || endpoint.host.startsWith(QLatin1Char('-'))
        || !hostPattern.match(endpoint.host).hasMatch()) {
        return EndpointError::Host;
    }
    if (endpoint.port < 1 || endpoint.port > 65535) {
        return EndpointError::Port;
    }
    if (!endpoint.user.isEmpty()
        && (endpoint.user.size() > 128 || endpoint.user.startsWith(QLatin1Char('-'))
            || !userPattern.match(endpoint.user).hasMatch())) {
        return EndpointError::User;
    }
    if (!endpoint.identityFile.isEmpty()
        && !QFileInfo(endpoint.identityFile).isFile()) {
        return EndpointError::IdentityFile;
    }
    return EndpointError::None;
}

QString sshExecutable()
{
    return span::core::toolExecutable(span::core::ExternalTool::Ssh);
}

QString sftpExecutable()
{
    return span::core::toolExecutable(span::core::ExternalTool::Sftp);
}

QStringList sshTerminalArguments(const SshEndpoint &endpoint)
{
    return commonOptions(endpoint) + destination(endpoint);
}

QStringList sshShellArguments(const SshEndpoint &endpoint)
{
    return QStringList{QStringLiteral("-T")} + commonOptions(endpoint)
        + destination(endpoint) + QStringList{QStringLiteral("exec /bin/sh")};
}

QStringList sftpBatchArguments(const SshEndpoint &endpoint)
{
    QStringList arguments{QStringLiteral("-o"), QStringLiteral("BatchMode=no")};
    arguments << commonOptions(endpoint)
              << QStringLiteral("-b") << QStringLiteral("-")
              << QStringLiteral("-P") << QString::number(endpoint.port);
    if (!endpoint.user.isEmpty()) {
        arguments << QStringLiteral("-o") << QStringLiteral("User=") + endpoint.user;
    }
    const bool ipv6 = endpoint.host.contains(QLatin1Char(':'));
    arguments << QStringLiteral("--")
              << (ipv6 ? QLatin1Char('[') + endpoint.host + QLatin1Char(']') : endpoint.host);
    return arguments;
}

QString windowsCommandLine(const QString &program, const QStringList &arguments)
{
    const auto quote = [](const QString &argument) {
        static const QRegularExpression needsQuotes(QStringLiteral(R"([\s"])"));
        if (!argument.isEmpty() && !needsQuotes.match(argument).hasMatch()) {
            return argument;
        }
        QString quoted(QLatin1Char('"'));
        int backslashes = 0;
        for (const QChar ch : argument) {
            if (ch == QLatin1Char('\\')) {
                ++backslashes;
                continue;
            }
            if (ch == QLatin1Char('"')) {
                quoted += QString(backslashes * 2 + 1, QLatin1Char('\\'));
                quoted += ch;
            } else {
                quoted += QString(backslashes, QLatin1Char('\\'));
                quoted += ch;
            }
            backslashes = 0;
        }
        quoted += QString(backslashes * 2, QLatin1Char('\\'));
        quoted += QLatin1Char('"');
        return quoted;
    };
    QStringList parts{quote(program)};
    for (const QString &argument : arguments) {
        parts << quote(argument);
    }
    return parts.join(QLatin1Char(' '));
}

QString shellQuote(const QString &value)
{
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString sftpQuote(const QString &value)
{
    QString quoted(QLatin1Char('"'));
    for (const QChar ch : value) {
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\\')) {
            quoted += QLatin1Char('\\');
        }
        quoted += ch;
    }
    quoted += QLatin1Char('"');
    return quoted;
}

} // namespace span::remote
