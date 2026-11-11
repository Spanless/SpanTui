# libghostty-vt, prebuilt for x86_64-windows-gnu by tools/update-libghostty.ps1
# (docs/legal/libghostty-vt.md). The Zig-built archive needs only the CRT and
# ntdll. GLOBAL: the engine's tests include its header too.
add_library(SpanGhosttyVt STATIC IMPORTED GLOBAL)
set_target_properties(SpanGhosttyVt PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libghostty-vt/lib/libghostty-vt.a
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libghostty-vt/include
    INTERFACE_COMPILE_DEFINITIONS GHOSTTY_STATIC
    INTERFACE_LINK_LIBRARIES ntdll
)

add_library(SpanTerminal STATIC
    terminal_constants.h
    api/terminal_manager.cpp
    api/terminal_manager.h
    api/terminal_session.cpp
    api/terminal_session.h
    api/terminal_session_input.cpp
    api/terminal_session_input.h
    api/terminal_input_transaction.h
    api/terminal_process.h
    api/shell_catalog.h
    api/terminal_types.h
    shared/terminal_output_queue.cpp
    shared/terminal_output_queue.h
    shared/terminal_frame_store.cpp
    shared/terminal_frame_store.h
    core/ghostty_engine.cpp
    core/ghostty_engine.h
    core/terminal_color.cpp
    core/terminal_color.h
    core/terminal_core_worker.cpp
    core/terminal_core_worker.h
    core/terminal_presentation_scheduler.cpp
    core/terminal_presentation_scheduler.h
    core/terminal_viewport_trace.cpp
    core/terminal_viewport_trace.h
    core/terminal_win32_input.cpp
    core/terminal_win32_input.h
    platform/windows/win_conpty_process.cpp
    platform/windows/win_conpty_process.h
    platform/windows/win_shell_catalog.cpp
    render/terminal_font_set.cpp
    render/terminal_font_set.h
    render/terminal_glyph_atlas.cpp
    render/terminal_glyph_atlas.h
    render/terminal_glyph_rasterizer.cpp
    render/terminal_glyph_rasterizer.h
    render/terminal_drawn_glyphs.cpp
    render/terminal_drawn_glyphs.h
    render/terminal_row_layout.cpp
    render/terminal_row_layout.h
    render/terminal_row_geometry.cpp
    render/terminal_row_geometry.h
    render/terminal_triangle_raster.h
    render/terminal_cell_material.cpp
    render/terminal_cell_material.h
    render/terminal_frame_preparer.cpp
    render/terminal_frame_preparer.h
    render/terminal_ime_anchor_resolver.cpp
    render/terminal_ime_anchor_resolver.h
    render/terminal_ime_composition_state.cpp
    render/terminal_ime_composition_state.h
    render/terminal_cursor_animation.cpp
    render/terminal_cursor_animation.h
    render/terminal_view.cpp
    render/terminal_view_input.cpp
    render/terminal_viewport_lifecycle.h
    render/terminal_viewport_lifecycle.cpp
    render/terminal_view.h
    render/terminal_scroll_model.cpp
    render/terminal_scroll_model.h
    render/terminal_input_routing.cpp
    render/terminal_input_routing.h
    render/terminal_scene_graph.cpp
    render/terminal_scene_graph.h
)

target_compile_definitions(SpanTerminal PRIVATE SPANTUI_VERSION="${PROJECT_VERSION}")

# Bundled ConPTY (docs/legal/conpty.md). conpty.dll starts the OpenConsole.exe
# in its own folder, so both sit next to every executable of the build tree —
# the app and the terminal tests run the same console host as a release; the
# package step copies them next to SpanTui.exe.
foreach(conpty_file conpty.dll OpenConsole.exe)
    configure_file(vendor/conpty/x64/${conpty_file} ${CMAKE_BINARY_DIR}/${conpty_file} COPYONLY)
endforeach()
target_link_libraries(SpanTerminal
    PUBLIC SpanCore Qt6::Core Qt6::Gui Qt6::Quick
    # QRhi uploads only the new part of a glyph atlas texture, and Qt's
    # distance-field generator gives text glyphs their fields.
    PRIVATE SpanGhosttyVt Qt6::GuiPrivate
)
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanTerminal)

# Terminal cell shaders (render/terminal_cell_material.cpp). Compiled into the
# library so every target that draws a terminal, tests included, has them.
qt_add_shaders(SpanTerminal "spanterminal_shaders"
    BATCHABLE
    PREFIX "/shaders/terminal"
    BASE "${CMAKE_SOURCE_DIR}/shaders"
    FILES
        ${CMAKE_SOURCE_DIR}/shaders/terminal_cell.vert
        ${CMAKE_SOURCE_DIR}/shaders/terminal_cell.frag
)
# Subpixel text needs dual-source blending; it is drawn on Direct3D only.
qt_add_shaders(SpanTerminal "spanterminal_subpixel_shaders"
    NOGLSL
    NOMSL
    PREFIX "/shaders/terminal"
    BASE "${CMAKE_SOURCE_DIR}/shaders"
    FILES
        ${CMAKE_SOURCE_DIR}/shaders/terminal_cell_subpixel.frag
)
