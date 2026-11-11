#include "application/gui_application.h"

#include "application/message_center.h"

#include <QString>

#include <exception>

namespace span::application {

bool GuiApplication::notify(QObject *receiver, QEvent *event)
{
    try {
        return QGuiApplication::notify(receiver, event);
    } catch (const std::exception &ex) {
        if (auto *center = MessageCenter::instance())
            center->postUnhandledException(QString::fromUtf8(ex.what()));
        return true;
    } catch (...) {
        if (auto *center = MessageCenter::instance())
            center->postUnhandledException(QString());
        return true;
    }
}

} // namespace span::application
