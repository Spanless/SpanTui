#pragma once

#include <QString>

namespace span::core {

// Running image channel from the executable basename:
//   SpanTui-test.exe       → "SpanTui-test"   (dev.bat / smoke)
//   SpanTui-pre.exe        → "SpanTui-pre"    (package image)
//   SpanTui.exe            → "SpanTui"        (installed product)
//   SpanTui-test-watch.exe → "SpanTui-test-watch"
//
// Used for window titles and single-instance isolation so a checkout-local
// SpanTui-test never steals or forwards into the installed desktop instance.
QString productImageName();

} // namespace span::core
