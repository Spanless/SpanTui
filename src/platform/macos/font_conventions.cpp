#include "platform/font_conventions.h"

#include <QFontDatabase>
#include <QStringList>

namespace span::platform {

namespace {

QString installedFamily(const QStringList &installed, const QString &family)
{
    for (const QString &candidate : installed) {
        if (candidate.compare(family, Qt::CaseInsensitive) == 0) {
            return candidate;
        }
    }
    return {};
}

} // namespace

SystemFontFamilies systemFontFamilies()
{
    const QStringList installed = QFontDatabase::families();
    QString mono = installedFamily(installed, QStringLiteral("SF Mono"));
    if (mono.isEmpty()) {
        mono = installedFamily(installed, QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    }
    return {installedFamily(installed, QFontDatabase::systemFont(QFontDatabase::GeneralFont).family()), mono,
            installedFamily(installed, QStringLiteral("PingFang SC"))};
}

bool prefersSystemFonts()
{
    return true;
}

} // namespace span::platform
