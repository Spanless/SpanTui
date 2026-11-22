#include "platform/accessibility_preferences.h"

#import <AppKit/AppKit.h>

namespace span::platform {

bool reduceMotionRequested()
{
    return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion];
}

} // namespace span::platform
