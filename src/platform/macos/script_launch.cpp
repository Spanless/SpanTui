#include "platform/script_launch.h"

#include <QFileInfo>
#include <QHash>

namespace span::platform {
namespace {

const QString kFallbackShell = QStringLiteral("/bin/zsh");

// The user's login shell. The command line is split on whitespace, so a
// shell path holding any falls back as well.
QString loginShell()
{
    const QString shell = qEnvironmentVariable("SHELL");
    const bool usable = QFileInfo(shell).isAbsolute() && QFileInfo(shell).isExecutable()
        && !shell.contains(QLatin1Char(' ')) && !shell.contains(QLatin1Char('\t'));
    return usable ? shell : kFallbackShell;
}

} // namespace

QString scriptCommandLine(const QString &path)
{
    static const QHash<QString, QString> interpreters{
        {QStringLiteral("sh"), QStringLiteral("/bin/sh")},
        {QStringLiteral("command"), QStringLiteral("/bin/sh")},
        {QStringLiteral("bash"), QStringLiteral("/bin/bash")},
        {QStringLiteral("zsh"), QStringLiteral("/bin/zsh")},
    };
    const QString interpreter = interpreters.value(QFileInfo(path).suffix().toLower());
    if (interpreter.isEmpty()) return {};
    if (path.contains(QLatin1Char('"')) || path.contains(QLatin1Char('\r')) || path.contains(QLatin1Char('\n')))
        return {};
    // Single quotes keep spaces and shell metacharacters in the file name
    // inert; an apostrophe closes the quote, adds an escaped one and reopens.
    QString literal = path;
    literal.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    // The login shell runs the script with the user's PATH, then replaces
    // itself with an interactive one.
    const QString shell = loginShell();
    return QStringLiteral("%1 -l -c \"%2 '%3'; exec %1 -l\"").arg(shell, interpreter, literal);
}

} // namespace span::platform
