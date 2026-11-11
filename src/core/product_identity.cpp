#include "core/product_identity.h"

#include "core/core_constants.h"

#include <QCoreApplication>
#include <QFileInfo>

namespace span::core {

QString productImageName()
{
    const QString path = QCoreApplication::applicationFilePath();
    QString base = QFileInfo(path).completeBaseName();
    if (base.isEmpty()) {
        return kProductName;
    }
    return base;
}

} // namespace span::core
