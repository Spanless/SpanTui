#include "platform/open_with_handlers.h"

#include <QDir>
#include <QSet>

#include <algorithm>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dispatch/dispatch.h>

namespace span::platform {

namespace {

// Apple's own applications (TextEdit, Preview, …) cannot open a folder.
constexpr QLatin1StringView kSystemRoot{"/System/"};
// How long openFolderWith waits for Launch Services to answer.
constexpr int64_t kOpenTimeoutSeconds = 15;

QString bundleOf(const QString &path)
{
    // An executable inside Foo.app/Contents/MacOS names Foo.app.
    const qsizetype at = path.indexOf(QLatin1String(".app/"));
    return at < 0 ? path : path.left(at + 4);
}

} // namespace

QList<OpenWithHandler> recommendedOpenWithHandlers(const QStringList &extensions, const QString &excludeExecutable)
{
    const QString excluded = bundleOf(QDir::cleanPath(excludeExecutable));
    QList<OpenWithHandler> handlers;
    QSet<QString> seen;
    @autoreleasepool {
        NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
        NSFileManager *files = [NSFileManager defaultManager];
        for (QString extension : extensions) {
            if (extension.startsWith(QLatin1Char('.'))) extension.remove(0, 1);
            UTType *type = [UTType typeWithFilenameExtension:extension.toNSString()];
            if (!type) continue;
            for (NSURL *url in [workspace URLsForApplicationsToOpenContentType:type]) {
                const QString bundle = QDir::cleanPath(QString::fromNSString(url.path));
                if (bundle.isEmpty() || bundle.startsWith(kSystemRoot) || bundle == excluded || seen.contains(bundle)) {
                    continue;
                }
                seen.insert(bundle);
                handlers.append({QString::fromNSString([files displayNameAtPath:url.path]), bundle});
            }
        }
    }
    std::sort(handlers.begin(), handlers.end(), [](const OpenWithHandler &a, const OpenWithHandler &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return handlers;
}

QString fileExplorerExecutable()
{
    return QStringLiteral("/System/Library/CoreServices/Finder.app");
}

bool openFolderWith(const QString &executable, const QString &folder, QString *error)
{
    __block QString failure;
    @autoreleasepool {
        NSURL *application = [NSURL fileURLWithPath:executable.toNSString() isDirectory:YES];
        NSURL *target = [NSURL fileURLWithPath:folder.toNSString() isDirectory:YES];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        // The completion runs on a Launch Services queue, never this thread.
        [[NSWorkspace sharedWorkspace] openURLs:@[ target ]
                           withApplicationAtURL:application
                                  configuration:[NSWorkspaceOpenConfiguration configuration]
                              completionHandler:^(NSRunningApplication *, NSError *openError) {
                                  if (openError) failure = QString::fromNSString(openError.localizedDescription);
                                  dispatch_semaphore_signal(done);
                              }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, kOpenTimeoutSeconds * NSEC_PER_SEC)) != 0) {
            failure = QStringLiteral("The application did not respond.");
        }
    }
    if (failure.isEmpty()) return true;
    if (error) *error = failure;
    return false;
}

} // namespace span::platform
