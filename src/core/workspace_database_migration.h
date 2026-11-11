#pragma once

#include <QString>

class QSqlDatabase;

namespace span::core {

// Sole owner of the workspace database schema. Every connection that reads
// or writes span_tui.db relies on this having run first (SettingsStore does it
// at startup).
bool migrateWorkspaceDatabase(QSqlDatabase &database, QString *errorMessage = nullptr);

} // namespace span::core
