#pragma once

#include <QString>

namespace span::platform {

// Families of the "system" font set. Each is an installed family, or empty
// when the platform has none to offer (an offscreen test host).
//   macOS:   ui = Qt's general theme font (.AppleSystemUIFont, SF Pro);
//            mono = SF Mono when the user installed it (it otherwise ships
//            inside Terminal.app only), else Qt's fixed theme font (Menlo);
//            cjk = PingFang SC.
//   Windows: ui = Segoe UI Variable Text, else Qt's general theme font
//            (Segoe UI); mono = Cascadia Mono, else Consolas (Qt's fixed
//            theme font is Courier New); cjk = Microsoft YaHei UI.
struct SystemFontFamilies {
    QString ui;
    QString mono;
    QString cjk;
};
SystemFontFamilies systemFontFamilies();

// Whether a fresh install uses the system font set instead of the bundled
// Span fonts: true on macOS, false on Windows.
bool prefersSystemFonts();

} // namespace span::platform
