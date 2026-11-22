#pragma once

namespace span::workspace {

// Workspace canvas geometry, in logical pixels.
inline constexpr int kMinLeftWidth = 250;
inline constexpr int kMinRightWidth = 240;
inline constexpr int kDefaultRightWidth = 300;
inline constexpr int kMinCenterWidth = 440;
// A side seam dragged below this collapses the side.
inline constexpr int kSideCollapseWidth = 120;
// A floating side docks again only once the window has this much more than
// the docked layout needs, so a window resized at the threshold does not
// flip it back and forth.
inline constexpr int kSideDockHysteresis = 48;
inline constexpr int kMinRowHeight = 160;
inline constexpr int kDefaultWindowWidth = 980;
inline constexpr int kDefaultCenterHeight = 640;

} // namespace span::workspace
