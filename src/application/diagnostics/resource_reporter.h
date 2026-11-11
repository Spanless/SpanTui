#pragma once

#include <QObject>

class QFile;
class FileTreeModel;
class QQuickWindow;
class QTimer;

namespace span::terminal {
class TerminalManager;
}

namespace span::application::diagnostics {

// Developer-only JSONL diagnostics. It is inert unless --resource-report is
// supplied and deliberately keeps all collection on a low-frequency timer.
class ResourceReporter final : public QObject {
    Q_OBJECT
public:
    ResourceReporter(span::terminal::TerminalManager *terminalManager,
                     FileTreeModel *fileTreeModel,
                     QObject *parent = nullptr);
    ~ResourceReporter() override;

    bool start(const QString &path, QQuickWindow *window, QString *errorMessage = nullptr);

private:
    void writeSnapshot();

    span::terminal::TerminalManager *m_terminalManager = nullptr;
    FileTreeModel *m_fileTreeModel = nullptr;
    QQuickWindow *m_window = nullptr;
    QFile *m_file = nullptr;
    QTimer *m_timer = nullptr;
};

} // namespace span::application::diagnostics
