#include "platform/script_launch.h"

#include <QFileInfo>

namespace span::platform {

QString scriptCommandLine(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QLatin1String("bat") && suffix != QLatin1String("cmd") && suffix != QLatin1String("ps1")) return {};
    if (path.contains(QLatin1Char('"')) || path.contains(QLatin1Char('\r')) || path.contains(QLatin1Char('\n')))
        return {};
    // A single-quoted PowerShell literal keeps spaces and shell metacharacters
    // in the file name inert. Doubling apostrophes is PowerShell's literal
    // escape, and -NoExit leaves the terminal open for inspection.
    QString literal = path;
    literal.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral("powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass "
                          "-NoExit -Command \"& '%1'\"")
        .arg(literal);
}

} // namespace span::platform
