#include "platform/font_conventions.h"

#include <QFontDatabase>
#include <QStringList>

namespace span::platform {

namespace {

// First of `families` that is installed, in the database's spelling.
QString firstInstalled(const QStringList &installed, const QStringList &families)
{
    for (const QString &family : families) {
        for (const QString &candidate : installed) {
            if (candidate.compare(family, Qt::CaseInsensitive) == 0) {
                return candidate;
            }
        }
    }
    return {};
}

} // namespace

SystemFontFamilies systemFontFamilies()
{
    const QStringList installed = QFontDatabase::families();
    return {firstInstalled(installed,
                           {QStringLiteral("Segoe UI Variable Text"),
                            QFontDatabase::systemFont(QFontDatabase::GeneralFont).family()}),
            firstInstalled(installed, {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas")}),
            firstInstalled(installed, {QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei")})};
}

bool prefersSystemFonts()
{
    return false;
}

} // namespace span::platform
