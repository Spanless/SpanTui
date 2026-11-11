#pragma once

#include "terminal/api/terminal_types.h"
#include <QJsonObject>

namespace span::terminal {

// Metadata only: never records terminal text or input bytes.
class TerminalViewportTrace final {
public:
    // On by default in Debug builds, off in Release; --trace-viewport[=0|1]
    // overrides it for one process.
    static bool enabled();
    static void setEnabled(bool enabled);
    static void record(const QString &session, const QString &stage,
                       QJsonObject details);
    static QJsonObject describeFrame(const Frame &frame);
};

} // namespace span::terminal
