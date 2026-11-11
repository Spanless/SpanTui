#pragma once

#include "stress_tui.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Well-behaved VT protocol coverage pages for the `protocol` scenario: one
// page per protocol family, every frame emitted as one complete write.
namespace spantui::stresstui::protocol {

enum class Stage : std::size_t {
    LegacyCharsets,
    CursorAndEditing,
    MarginsAndScrolling,
    LegacySgr,
    ExtendedColor,
    UnicodeWidth,
    CellGeometry,
    ModernOsc,
    ModernModes,
    InlineGraphics,
    Count,
};

struct FrameRequest {
    Stage stage = Stage::LegacyCharsets;
    TerminalSize size;
    std::uint64_t frameIndex = 0; // frame within the stage
    bool firstFrame = true;       // stage start or resize: repaint the whole page
    bool synchronizedUpdates = false;
};

std::vector<std::string_view> stageNames();
std::string renderFrame(const FrameRequest &request);
// Undoes every mode a stage may set: margins, origin/autowrap, mouse, focus,
// paste, Kitty keyboard, cursor shape, hyperlinks, images and the title.
std::string resetModes();

} // namespace spantui::stresstui::protocol
