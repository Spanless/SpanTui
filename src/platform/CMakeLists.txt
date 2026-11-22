add_library(SpanPlatform STATIC
    platform_constants.h
    accessibility_preferences.h
    crash_handler.h
    credential_protection.h
    directory_change_watcher.h
    font_conventions.h
    graphics_adapter.h
    instance_lock.h
    keyboard_conventions.h
    local_endpoint.h
    native_dialog.h
    open_with_handlers.h
    process_containment.h
    program_icon_provider.h
    script_launch.h
    window_chrome.h
    window_chrome_geometry.cpp
    window_chrome_geometry.h
    diagnostics/graphics_startup.cpp
    diagnostics/graphics_startup.h
    diagnostics/launch_logging.cpp
    diagnostics/launch_logging.h
)
target_link_libraries(SpanPlatform PUBLIC SpanCore Qt6::Gui Qt6::Quick)
if(WIN32)
    target_sources(SpanPlatform PRIVATE
        windows/accessibility_preferences.cpp
        windows/crash_handler.cpp
        windows/credential_protection.cpp
        windows/directory_change_watcher.cpp
        windows/font_conventions.cpp
        windows/graphics_adapter.cpp
        windows/instance_lock.cpp
        windows/keyboard_conventions.cpp
        windows/local_endpoint.cpp
        windows/native_dialog.cpp
        windows/open_with_handlers.cpp
        windows/process_containment.cpp
        windows/program_icon_provider.cpp
        windows/script_launch.cpp
        windows/window_chrome.cpp
    )
    target_link_libraries(SpanPlatform PRIVATE dwmapi user32 shell32 ole32 dbghelp crypt32)
else()
    target_sources(SpanPlatform PRIVATE
        macos/accessibility_preferences.mm
        macos/crash_handler.cpp
        macos/credential_protection.cpp
        macos/directory_change_watcher.cpp
        macos/font_conventions.cpp
        macos/graphics_adapter.mm
        macos/instance_lock.cpp
        macos/keyboard_conventions.cpp
        macos/local_endpoint.cpp
        macos/native_dialog.cpp
        macos/open_with_handlers.mm
        macos/process_containment.cpp
        macos/program_icon_provider.mm
        macos/script_launch.cpp
        macos/window_chrome.mm
    )
    target_compile_options(SpanPlatform PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(SpanPlatform PRIVATE
        "-framework AppKit"
        "-framework CoreFoundation"
        "-framework CoreServices"
        "-framework Metal"
        "-framework Security"
        "-framework UniformTypeIdentifiers"
    )
endif()
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanPlatform)
