#pragma once

#include <QObject>
#include <QVariantList>

namespace span::application {

// Sole owner of the "Open in" targets of a project folder: File Explorer,
// then the desktop programs Windows recommends for source files
// (platform::recommendedOpenWithHandlers over kOpenWithProbeExtensions) —
// no list of editors is kept here. Detection runs on a worker thread at
// startup and on refresh(); the menu shows the last result.
class OpenTargetRegistry final : public QObject {
    Q_OBJECT
    // [{name, executable}], ordered by name. `executable` doubles as the
    // icon path (ProgramIcon) and the id openFolderWith() takes.
    Q_PROPERTY(QVariantList editors READ editors NOTIFY editorsChanged FINAL)
    Q_PROPERTY(QString fileExplorerIcon READ fileExplorerIcon CONSTANT FINAL)

public:
    explicit OpenTargetRegistry(QObject *parent = nullptr);
    ~OpenTargetRegistry() override;

    QVariantList editors() const;
    QString fileExplorerIcon() const;

    // Detects again in the background; a scan already running absorbs it.
    Q_INVOKABLE void refresh();
    // Starts a detected program on `folder`. The program outlives SpanTui
    // (it is the user's editor, not a helper), so it leaves the application
    // job. Unknown executables are refused.
    Q_INVOKABLE bool openFolderWith(const QString &executable, const QString &folder);
    Q_INVOKABLE bool showInFileExplorer(const QString &folder);

signals:
    void editorsChanged();
    void errorOccurred(const QString &message);

private:
    QVariantList m_editors;
    QString m_fileExplorerIcon;
    bool m_scanning = false;
    bool m_rescan = false;
};

} // namespace span::application
