#pragma once

#include <QObject>

namespace span::application {

// System clipboard writes for QML (singleton `Clipboard`).
class Clipboard final : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Empty text leaves the clipboard unchanged.
    Q_INVOKABLE void copyText(const QString &text) const;
};

} // namespace span::application
