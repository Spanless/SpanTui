#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace span::terminal {

// How a local path is written so the session's shell reads it back as one
// argument.
enum class PathQuoting {
    Posix, // sh, bash, zsh, fish, Git Bash: single quotes
    PowerShell, // pwsh, Windows PowerShell: single quotes, ' doubled
    Cmd, // cmd.exe: double quotes
    Wsl, // wsl.exe: the Linux path of a Windows drive, then Posix
    Remote, // ssh: a local path means nothing on the other host
};

// The quoting for the program a session's command line starts: its first
// word's file name decides (ssh -> Remote, pwsh -> PowerShell, ...).
PathQuoting pathQuotingForCommandLine(const QString &commandLine);

// One path, quoted only when the shell would otherwise split or expand it.
// Empty for Remote.
QString quotePath(const QString &path, PathQuoting quoting);

// The paths, quoted and separated by spaces, with a trailing space so the
// next argument can be typed right away (as Terminal.app and Ghostty drop
// files). Empty for Remote or no paths.
QString pastePaths(const QStringList &paths, PathQuoting quoting);

// What a clipboard paste sends. Files win over their text form (Finder puts
// the file name next to the URL); text wins over an image (a spreadsheet
// copies both).
enum class ClipboardPasteKind { Nothing, Files, Text, Image };
ClipboardPasteKind classifyClipboard(bool hasLocalFiles, bool hasText, bool hasImage);

// Staged copies of pasted images and of dropped files the system is about
// to delete (screenshot thumbnails), under the cache.
struct StagedFile {
    QString path;
    QDateTime modified;
};

// Files to delete: older than `maxAgeSeconds`, or beyond the newest
// `maxFiles`.
QStringList expiredStagedFiles(QVector<StagedFile> files, const QDateTime &now, qint64 maxAgeSeconds, int maxFiles);

// True when `path` lives in the system temporary directory, where the
// system removes it soon (the macOS screenshot thumbnail, a file dragged out
// of a browser).
bool isTransientFile(const QString &path);

// The staging directory: ProductPaths::cacheDirectory()/paste-images.
QString pasteStagingDirectory();

// Writes `png` to a new file in `directory` and prunes expired files there.
// Empty with `error` set on failure.
QString stagePastedImage(const QByteArray &png, const QString &directory, const QDateTime &now,
                         QString *error = nullptr);

// Copies a transient file into `directory` (keeping its name) and prunes.
// Returns `path` itself when it is not transient or the copy fails.
QString stageDroppedFile(const QString &path, const QString &directory, const QDateTime &now);

} // namespace span::terminal
