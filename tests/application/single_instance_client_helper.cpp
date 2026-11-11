#include "application/single_instance_router.h"

#include <QCoreApplication>
#include <QDebug>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() != 3) {
        return 64;
    }
    span::application::SingleInstanceRouter router(nullptr, arguments.at(1));
    QString error;
    const auto result = router.startOrForward(arguments.at(2), &error);
    if (result == span::application::SingleInstanceRouter::StartResult::Forwarded) {
        return 0;
    }
    if (result == span::application::SingleInstanceRouter::StartResult::Rejected) {
        qCritical().noquote() << "Rejected:" << error;
        return 2;
    }
    qCritical().noquote() << "route result" << static_cast<int>(result) << error;
    return 1;
}
