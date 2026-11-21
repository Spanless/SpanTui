#pragma once

namespace span::platform {

// The user asked the system for less motion.
//   Windows: client-area animations turned off (Accessibility > Visual
//   effects > Animation effects).
//   macOS: Accessibility > Display > Reduce motion.
bool reduceMotionRequested();

} // namespace span::platform
