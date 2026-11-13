#include "application/open_target_registry.h"

#include "application/application_constants.h"
#include "platform/windows/open_with_handlers.h"
#include "platform/windows/process_containment.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QThreadPool>
#include <QUrl>

#include <algorithm>

namespace span::application {

namespace {

QStringList probeExtensions()
{
    QStringList extensions;
    for (const QLatin1StringView extension : kOpenWithProbeExtensions) {
        extensions.append(QString(extension));
    }
    return extensions;
}

} // namespace

OpenTargetRegistry::OpenTargetRegistry(QObject *parent)
    : QObject(parent),
      m_fileExplorerIcon(span::platform::fileExplorerExecutable())
{}

OpenTargetRegistry::~OpenTargetRegistry() = default;

QVariantList OpenTargetRegistry::editors() const
{
    return m_editors;
}

QString OpenTargetRegistry::fileExplorerIcon() const
{
    return m_fileExplorerIcon;
}

void OpenTargetRegistry::refresh()
{
    if (m_scanning) {
        m_rescan = true;
        return;
    }
    m_scanning = true;
    const QString self = QCoreApplication::applicationFilePath();
    QPointer<OpenTargetRegistry> guard(this);
    QThreadPool::globalInstance()->start([guard, self]() {
        const QList<span::platform::OpenWithHandler> handlers =
            span::platform::recommendedOpenWithHandlers(probeExtensions(), self);
        QVariantList editors;
        for (const span::platform::OpenWithHandler &handler : handlers) {
            editors.append(QVariantMap{{QStringLiteral("name"), handler.name},
                                       {QStringLiteral("executable"), handler.executable}});
        }
        // `guard` is only read on the GUI thread, inside the queued call.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, editors]() {
                if (!guard) {
                    return;
                }
                guard->m_scanning = false;
                if (guard->m_editors != editors) {
                    guard->m_editors = editors;
                    emit guard->editorsChanged();
                }
                if (guard->m_rescan) {
                    guard->m_rescan = false;
                    guard->refresh();
                }
            },
            Qt::QueuedConnection);
    });
}

bool OpenTargetRegistry::openFolderWith(const QString &executable, const QString &folder)
{
    const bool known = std::any_of(m_editors.cbegin(), m_editors.cend(), [&executable](const QVariant &editor) {
        return editor.toMap().value(QStringLiteral("executable")).toString().compare(executable, Qt::CaseInsensitive)
            == 0;
    });
    const QFileInfo target(folder);
    if (!known || !target.isDir()) {
        return false;
    }
    const QString directory = QDir::toNativeSeparators(QDir::cleanPath(target.absoluteFilePath()));
    QString error;
    if (span::platform::process::startUnaffiliatedProcess(QDir::toNativeSeparators(executable), {directory}, directory,
                                                          &error)
        == 0) {
        emit errorOccurred(tr("Could not open %1: %2").arg(directory, error));
        return false;
    }
    return true;
}

bool OpenTargetRegistry::showInFileExplorer(const QString &folder)
{
    const QFileInfo target(folder);
    if (!target.isDir()) {
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(target.absoluteFilePath()));
}

} // namespace span::application
