#pragma once

#include <QGuiApplication>

namespace span::application {

class GuiApplication final : public QGuiApplication {
public:
    using QGuiApplication::QGuiApplication;

    bool notify(QObject *receiver, QEvent *event) override;
};

} // namespace span::application
