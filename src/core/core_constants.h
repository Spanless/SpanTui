#pragma once

#include <QLatin1StringView>

namespace span::core {

// Application name, data folder name, TERM_PROGRAM and the XTVERSION reply.
inline constexpr QLatin1StringView kProductName{"SpanTui"};

inline constexpr QLatin1StringView kSqliteDriver{"QSQLITE"};
// A connection that shares its database file with other connections or
// processes waits for a lock instead of failing.
inline constexpr QLatin1StringView kSqliteBusyTimeoutPragma{"pragma busy_timeout=3000"};
// PRAGMA user_version of a fully migrated workspace database.
inline constexpr int kWorkspaceDatabaseVersion = 11;

// Wait after QProcess::kill() so the process is reaped before the QProcess
// is disconnected or destroyed.
inline constexpr int kProcessKillWaitMs = 1000;

} // namespace span::core
