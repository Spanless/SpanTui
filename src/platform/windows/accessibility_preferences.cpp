#include "platform/accessibility_preferences.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace span::platform {

bool reduceMotionRequested()
{
    BOOL clientAreaAnimation = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAreaAnimation, 0) && !clientAreaAnimation;
}

} // namespace span::platform
