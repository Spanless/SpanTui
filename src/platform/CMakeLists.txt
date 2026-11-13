add_library(SpanPlatform STATIC
    platform_constants.h
    diagnostics/graphics_startup.cpp
    diagnostics/graphics_startup.h
    diagnostics/launch_logging.cpp
    diagnostics/launch_logging.h
    windows/crash_handler.cpp
    windows/crash_handler.h
    windows/credential_protection.cpp
    windows/credential_protection.h
    windows/directory_change_watcher.cpp
    windows/directory_change_watcher.h
    windows/native_dialog.cpp
    windows/native_dialog.h
    windows/open_with_handlers.cpp
    windows/open_with_handlers.h
    windows/process_containment.cpp
    windows/process_containment.h
    windows/program_icon_provider.cpp
    windows/program_icon_provider.h
    windows/window_chrome.cpp
    windows/window_chrome.h
    windows/window_chrome_geometry.cpp
    windows/window_chrome_geometry.h
)
target_link_libraries(SpanPlatform
    PUBLIC SpanCore Qt6::Gui Qt6::Quick
    PRIVATE dwmapi user32 shell32 ole32 dbghelp crypt32
)
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanPlatform)
