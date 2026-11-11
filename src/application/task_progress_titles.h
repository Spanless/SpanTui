#pragma once

#include "git/git_constants.h"

#include <QCoreApplication>
#include <QString>

namespace span::application {

inline QString gitProgressTitle(const QString &operation)
{
    if (operation == kGitOperationRefresh || operation == kGitOperationDiscover)
        return QCoreApplication::translate("TaskProgress", "Refreshing repository");
    if (operation == kGitOperationHistory || operation == kGitOperationHistoryParse)
        return QCoreApplication::translate("TaskProgress", "Loading history");
    if (operation == kGitOperationCommitFiles) return QCoreApplication::translate("TaskProgress", "Preparing diff");
    return QCoreApplication::translate("TaskProgress", "Working");
}

} // namespace span::application
