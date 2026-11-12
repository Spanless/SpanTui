cmake_minimum_required(VERSION 3.22)

project(SpanTui VERSION 0.2.4 LANGUAGES C CXX)

find_program(SPANTUI_SCCACHE_EXECUTABLE NAMES sccache)
if(SPANTUI_SCCACHE_EXECUTABLE)
    set(CMAKE_C_COMPILER_LAUNCHER "${SPANTUI_SCCACHE_EXECUTABLE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${SPANTUI_SCCACHE_EXECUTABLE}")
    message(STATUS "Compiler cache: ${SPANTUI_SCCACHE_EXECUTABLE}")
else()
    message(STATUS "Compiler cache: none (sccache not on PATH)")
endif()

# Linker the compiler driver runs (-fuse-ld=<value>, e.g. lld); empty keeps the
# toolchain default. The build scripts pass it from SPANTUI_LINKER.
set(SPANTUI_LINKER "" CACHE STRING "Linker for -fuse-ld (empty: the toolchain default)")
if(SPANTUI_LINKER)
    add_link_options("-fuse-ld=${SPANTUI_LINKER}")
    message(STATUS "Linker: ${SPANTUI_LINKER}")
endif()

# OFF compiles without debug info (-g0 overrides the build type's -g), for
# builds nobody debugs. The build scripts pass it from SPANTUI_DEBUG_INFO.
option(SPANTUI_DEBUG_INFO "Keep the build type's debug info (OFF: -g0)" ON)
if(NOT SPANTUI_DEBUG_INFO)
    add_compile_options(-g0)
    message(STATUS "Debug info: off")
endif()

# ON instruments every target with gcov counters (MinGW --coverage) for
# `dev test --coverage`, which builds it in its own tree (build.md). Counters
# update atomically: worker threads racing on them yield negative hit counts.
option(SPANTUI_COVERAGE "Instrument all targets with gcov line/branch counters" OFF)
if(SPANTUI_COVERAGE)
    add_compile_options(--coverage -fprofile-update=atomic)
    add_link_options(--coverage -fprofile-update=atomic)
    message(STATUS "Coverage instrumentation: on")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# GuiPrivate/QuickPrivate are used on purpose and the Qt version is pinned
# (tools/toolchain-versions.json); the per-configure warning says nothing new.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
# Optional for Qt Gui and unused here; looking for it only prints
# "Could NOT find WrapVulkanHeaders".
set(CMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders TRUE)

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Gui GuiPrivate Network Qml Quick QuickPrivate QuickControls2 ShaderTools Sql Svg Test LinguistTools)

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

option(SPANTUI_INSTALLED_IMAGE "Build the installed product image instead of the local development image" OFF)

# Temporary basename split so local start and package outputs do not collide as
# the same process image name (SpanTui-test vs SpanTui-pre). CMake target stays SpanTui.
# Default to the local-dev image; package-worker overrides to SpanTui-pre.
set(SPANTUI_OUTPUT_NAME "SpanTui-test" CACHE STRING "Output basename for the SpanTui executable")

add_subdirectory(src)

set_source_files_properties(qml/Theme.qml qml/PixelGrid.qml qml/ScrollPolicy.qml
    qml/components/base/PopupLayerStack.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

set(TS_FILES
    translations/SpanTui_zh_CN.ts
)

# The ICO is generated from assets/brand/spantui.svg before every dependent
# build; identical bytes leave the file untouched.
set(SPANTUI_BRAND_SVG "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/spantui.svg")
set(SPANTUI_BRAND_ICO "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/spantui.ico")
set(SPANTUI_APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/spantui.manifest")
set(SPANTUI_DEV_APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/spantui_dev.manifest")
add_executable(spantui_brand_icon_generator tools/brand_icon_generator.cpp)
target_link_libraries(spantui_brand_icon_generator PRIVATE Qt6::Core Qt6::Gui Qt6::Svg)
set_target_properties(spantui_brand_icon_generator PROPERTIES WIN32_EXECUTABLE FALSE)
add_custom_command(
    OUTPUT "${SPANTUI_BRAND_ICO}"
    COMMAND $<TARGET_FILE:spantui_brand_icon_generator>
            "${SPANTUI_BRAND_SVG}" "${SPANTUI_BRAND_ICO}"
    DEPENDS spantui_brand_icon_generator "${SPANTUI_BRAND_SVG}"
    COMMENT "Generating multi-size Windows icon from assets/brand/spantui.svg"
    VERBATIM)
add_custom_target(spantui_brand_icon DEPENDS "${SPANTUI_BRAND_ICO}")
# Script entrypoints invoke this target in a separate build phase before the
# application build. It checks SVG content even when a copied file preserved an
# older timestamp; the following build can then see any changed ICO immediately.
add_custom_target(spantui_brand_icon_refresh
    COMMAND $<TARGET_FILE:spantui_brand_icon_generator>
            "${SPANTUI_BRAND_SVG}" "${SPANTUI_BRAND_ICO}"
    DEPENDS spantui_brand_icon_generator
    COMMENT "Refreshing Windows icon from SVG content"
    VERBATIM)

qt_add_executable(SpanTui
    src/main.cpp
    src/application/runtime/internal_launch_options.cpp
    src/application/runtime/internal_launch_options.h
    src/application/runtime/qml_types.h
    src/application/runtime/terminal_application.cpp
    src/application/runtime/terminal_application.h
    src/application/runtime/workspace_application.cpp
    src/application/runtime/workspace_application.h
    src/application/runtime/workspace_runtime.cpp
    src/application/runtime/workspace_runtime.h
    src/application/runtime/workspace_visual_harness.cpp
    src/application/runtime/workspace_visual_harness.h
)
set_target_properties(SpanTui PROPERTIES
    OUTPUT_NAME "${SPANTUI_OUTPUT_NAME}"
    # Windows GUI app: without this the PE subsystem is CONSOLE and a black
    # terminal window appears next to the Qt window on every launch.
    WIN32_EXECUTABLE TRUE
)
add_dependencies(SpanTui spantui_brand_icon)
# The generated QML type registration includes qml_types.h by its bare name.
target_include_directories(SpanTui PRIVATE src/application/runtime)
# WIN32_EXECUTABLE needs Qt's WinMain shim. Setting the property after
# qt_add_executable does not always pull it in via the finalizer alone.
target_link_libraries(SpanTui PRIVATE Qt6::EntryPointPrivate)

# Stamp next to the built binary so portable installs can detect newer local builds.
add_custom_command(TARGET SpanTui POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DOUT=$<TARGET_FILE_DIR:SpanTui>/spantui-build.json
            -DVERSION=${PROJECT_VERSION}
            -DEXE=$<TARGET_FILE_NAME:SpanTui>
            -P "${CMAKE_SOURCE_DIR}/tools/write_build_stamp.cmake"
    COMMENT "Writing spantui-build.json next to SpanTui"
    VERBATIM)

# windres does not reliably list the ICO in its depfile, hence OBJECT_DEPENDS.
# The manifests never bind the process to the sparse package: a packaged
# process makes the taskbar use package visuals instead of the PE icon.
if(SPANTUI_INSTALLED_IMAGE)
    set(SPANTUI_APP_RESOURCE assets/brand/spantui.rc)
    set(SPANTUI_SELECTED_APP_MANIFEST "${SPANTUI_APP_MANIFEST}")
else()
    set(SPANTUI_APP_RESOURCE assets/brand/spantui_dev.rc)
    set(SPANTUI_SELECTED_APP_MANIFEST "${SPANTUI_DEV_APP_MANIFEST}")
endif()
set_property(SOURCE ${SPANTUI_APP_RESOURCE} APPEND PROPERTY
    OBJECT_DEPENDS "${SPANTUI_BRAND_ICO};${SPANTUI_SELECTED_APP_MANIFEST}")
target_sources(SpanTui PRIVATE ${SPANTUI_APP_RESOURCE})

add_library(SpanTui-shell-extension SHARED
    src/platform/windows/shell_extension.cpp
    src/platform/windows/shell_extension_contract.h
    src/platform/windows/spantui_shell_extension.def
)
target_include_directories(SpanTui-shell-extension PRIVATE src)
target_compile_definitions(SpanTui-shell-extension PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(SpanTui-shell-extension PRIVATE ole32 shell32 uuid)
set_target_properties(SpanTui-shell-extension PROPERTIES
    PREFIX ""
    OUTPUT_NAME "SpanTui-shell-extension"
)
add_dependencies(SpanTui SpanTui-shell-extension)

qt_add_qml_module(SpanTui
    URI SpanTui
    VERSION 1.0
    # C++ types derive from QQuickItem and take QFont: without QtQuick the
    # QML compiler cannot resolve them and interprets those bindings.
    DEPENDENCIES QtQuick
    QML_FILES
        qml/Theme.qml
        qml/PixelGrid.qml
        qml/ScrollPolicy.qml
        qml/Main.qml
        qml/TerminalPanel.qml
        qml/BoundaryGallery.qml
        qml/Settings.qml
        qml/components/base/PopupEnter.qml
        qml/components/base/PopupExit.qml
        qml/components/base/PopupLayerStack.qml
        qml/components/base/PopupToggleGuard.qml
        qml/components/common/ActionList.qml
        qml/components/common/AnchorPanel.qml
        qml/components/common/AnchorPopup.qml
        qml/components/common/AppComboBox.qml
        qml/components/common/AppContextMenu.qml
        qml/components/common/AppDialog.qml
        qml/components/common/AppPopup.qml
        qml/components/common/AppSwitch.qml
        qml/components/common/ChromeButton.qml
        qml/components/common/ComboOptionRow.qml
        qml/components/common/ComboPopupList.qml
        qml/components/common/ConfirmDialog.qml
        qml/components/common/CrispSurface.qml
        qml/components/common/DesktopFlickable.qml
        qml/components/common/DesktopListView.qml
        qml/components/common/DesktopScrollView.qml
        qml/components/common/DesktopWheelHandler.qml
        qml/components/common/DiffStat.qml
        qml/components/common/EmptyState.qml
        qml/components/common/FadeText.qml
        qml/components/common/FileIcon.qml
        qml/components/common/FilterBar.qml
        qml/components/common/FindBar.qml
        qml/components/common/FontPicker.qml
        qml/components/common/FontSettingsGroup.qml
        qml/components/common/FontWeightPicker.qml
        qml/components/common/GlideHighlight.qml
        qml/components/common/GridCol.qml
        qml/components/common/GridRow.qml
        qml/components/common/HelpTooltip.qml
        qml/components/common/Icon.qml
        qml/components/common/InlineBanner.qml
        qml/components/common/ListItem.qml
        qml/components/common/LoadingSpinner.qml
        qml/components/common/MonoText.qml
        qml/components/common/NameDialog.qml
        qml/components/common/NavRow.qml
        qml/components/common/OverflowProgressBar.qml
        qml/components/common/PaneHeader.qml
        qml/components/common/PaneLoading.qml
        qml/components/common/PixelLine.qml
        qml/components/common/PopupSurface.qml
        qml/components/common/ProgramIcon.qml
        qml/components/common/RenderText.qml
        qml/components/common/RenderTextEdit.qml
        qml/components/common/ResizeHandle.qml
        qml/components/common/SectionLabel.qml
        qml/components/common/SegmentedControl.qml
        qml/components/common/SettingRow.qml
        qml/components/common/SlimScrollBar.qml
        qml/components/common/StatusIcon.qml
        qml/components/common/Stepper.qml
        qml/components/common/TerminalText.qml
        qml/components/common/Tip.qml
        qml/components/common/TreeChevron.qml
        qml/components/common/TreeExpandHost.qml
        qml/components/common/TreeGroupHeader.qml
        qml/components/common/TreeListView.qml
        qml/components/common/UiField.qml
        qml/components/common/UiText.qml
        qml/components/common/WindowControls.qml
        qml/components/business/editor/CodePreview.qml
        qml/components/business/editor/EditorSearchBar.qml
        qml/components/business/editor/EditorStatusBar.qml
        qml/components/business/editor/TextLinesView.qml
        qml/components/business/files/FileTreePane.qml
        qml/components/business/git/BranchPopup.qml
        qml/components/business/git/CommitHoverCard.qml
        qml/components/business/git/DiffDirRow.qml
        qml/components/business/git/DiffFileRow.qml
        qml/components/business/git/GitCommitDiffList.qml
        qml/components/business/git/GitFileTreeRow.qml
        qml/components/business/git/GitLogView.qml
        qml/components/business/git/GitPane.qml
        qml/components/business/git/GitRefBadge.qml
        qml/components/business/git/GitRefsTree.qml
        qml/components/business/git/RefsSectionHeader.qml
        qml/components/business/messages/MessageCard.qml
        qml/components/business/messages/MessageCenterPopup.qml
        qml/components/business/messages/MessageToastStack.qml
        qml/components/business/messages/TaskProgressPopup.qml
        qml/components/business/preview/MarkdownPreview.qml
        qml/components/business/preview/PreviewPane.qml
        qml/components/business/remote/RemoteFilePane.qml
        qml/components/business/remote/RemoteHostDialog.qml
        qml/components/business/remote/SshPromptDialog.qml
        qml/components/business/settings/ColorPickerPopup.qml
        qml/components/business/settings/ColorSchemeEditor.qml
        qml/components/business/settings/DeveloperSettingsSection.qml
        qml/components/business/settings/EnvironmentSettingsSection.qml
        qml/components/business/settings/GeneralSettingsSection.qml
        qml/components/business/settings/InterfaceSettingsSection.qml
        qml/components/business/settings/RemoteSettingsSection.qml
        qml/components/business/settings/SettingsSection.qml
        qml/components/business/settings/ShortcutRecorder.qml
        qml/components/business/settings/ShortcutSettingsSection.qml
        qml/components/business/settings/TerminalColorSchemeSettings.qml
        qml/components/business/settings/TerminalSettingsSection.qml
        qml/components/business/terminal/ResourceStrip.qml
        qml/components/business/terminal/TerminalCloseDialog.qml
        qml/components/business/terminal/TerminalPane.qml
        qml/components/business/terminal/TerminalSurface.qml
        qml/components/business/update/UpdateProgressDialog.qml
        qml/components/business/workspace/ContentContainer.qml
        qml/components/business/workspace/RightDock.qml
        qml/components/business/workspace/Sidebar.qml
        qml/components/business/workspace/CanvasDragGhost.qml
        qml/components/business/workspace/CanvasDropTarget.qml
        qml/components/business/workspace/CanvasGeometry.qml
        qml/components/business/workspace/CanvasSeamController.qml
        qml/components/business/workspace/CanvasSeamHandle.qml
        qml/components/business/workspace/TileDragController.qml
        qml/components/business/workspace/WorkspaceCanvas.qml
        qml/components/business/workspace/ZoneTileDelegate.qml
        qml/components/business/workspace/WorkspaceTab.qml
)

add_custom_command(TARGET SpanTui POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DQML_MODULE_DIR=${CMAKE_CURRENT_BINARY_DIR}/SpanTui/qml
            -DSOURCE_QML_DIR=${CMAKE_CURRENT_SOURCE_DIR}/qml
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/prune_stale_qml_module.cmake"
    COMMENT "Prune QML module copies that no longer exist in source"
    VERBATIM
)

# Bundled Span Mono (terminal/editor), Span Sans (UI) and Span Symbols (Nerd
# icon fallback) faces, registered by main.cpp. Rebuilt by
# tools/fonts/build-fonts.ps1.
file(GLOB SPAN_FONT_FILES CONFIGURE_DEPENDS
    assets/fonts/Span*.ttf assets/fonts/SpanFonts-LICENSE-*.txt)
qt_add_resources(SpanTui "spantui_fonts"
    PREFIX "/fonts"
    BASE assets/fonts
    FILES ${SPAN_FONT_FILES}
)

qt_add_resources(SpanTui "spantui_brand"
    PREFIX "/"
    FILES
        assets/brand/spantui.ico
        assets/brand/spantui.svg
)

# Chrome stroke icons — one SVG per iconName (plus <name>.motion.json for
# animated ones), extracted by UiIconCatalog.
# file-icons.json maps file/folder names onto those stems for the explorer tree.
file(GLOB_RECURSE UI_ICON_FILES CONFIGURE_DEPENDS
    assets/icons/*.svg assets/icons/*.motion.json)
# Compiled icon resources travel with the code that loads them
# (UiIconCatalog, FileIconTheme), not with the executable.
add_library(SpanUiIcons STATIC)
qt_add_resources(SpanUiIcons ui_icons
    PREFIX "/icons"
    BASE assets/icons
    FILES assets/icons/file-icons.json ${UI_ICON_FILES}
)
target_link_libraries(SpanUiIcons PUBLIC Qt6::Core)
target_link_libraries(SpanWorkspace PUBLIC SpanUiIcons)

qt_add_translations(SpanTui
    TS_FILES ${TS_FILES}
    RESOURCE_PREFIX "/i18n"
)

target_link_libraries(SpanTui
    PRIVATE
        SpanApplication
        # Direct links give qmltyperegistrar the Qt Gui/Quick metatypes
        # (QFont, QQuickItem) the foreign types in qml_types.h refer to.
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
)

target_compile_definitions(SpanTui PRIVATE SPANTUI_VERSION="${PROJECT_VERSION}")

enable_testing()

get_target_property(SPANTUI_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT SPANTUI_QT_CORE_LOCATION)
    get_target_property(SPANTUI_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(SPANTUI_QT_BIN_DIR "${SPANTUI_QT_CORE_LOCATION}" DIRECTORY)
get_filename_component(SPANTUI_CXX_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
# Without a console QtTest logs to OutputDebugString, leaving
# --output-on-failure empty.
#
# TEMP/TMP/APPDATA/LOCALAPPDATA point into .dev/tmp/tests/<name>/, one per
# test: CTest runs in parallel and fixed file names there would collide.
set(SPANTUI_DEV_DIR "${CMAKE_SOURCE_DIR}/.dev")
set(SPANTUI_TEST_ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${SPANTUI_QT_BIN_DIR}"
    "PATH=path_list_prepend:${SPANTUI_CXX_BIN_DIR}"
    "QT_FORCE_STDERR_LOGGING=set:1"
    # Git exports these to hooks (GIT_DIR points into .git/worktrees/<name>
    # in a linked worktree); a test's git child must find its own fixture
    # repository, not the checkout that ran the hook.
    "GIT_DIR=unset:"
    "GIT_WORK_TREE=unset:"
    "GIT_INDEX_FILE=unset:"
    "GIT_COMMON_DIR=unset:"
    "GIT_OBJECT_DIRECTORY=unset:"
    "GIT_ALTERNATE_OBJECT_DIRECTORIES=unset:"
    "GIT_PREFIX=unset:"
)
# Window tests run offscreen, whose font database scans only QT_QPA_FONTDIR
# (default <Qt>/lib/fonts, absent on Windows).
file(TO_CMAKE_PATH "$ENV{SystemRoot}/Fonts" SPANTUI_SYSTEM_FONT_DIR)
set(SPANTUI_OFFSCREEN_ENVIRONMENT_MODIFICATION
    "QT_QPA_PLATFORM=set:offscreen"
    "QT_QPA_FONTDIR=set:${SPANTUI_SYSTEM_FONT_DIR}"
)

# The scratch directory of test `name`; its TEMP/TMP is the temp/ child.
set(SPANTUI_TEST_SCRATCH_DIR "${SPANTUI_DEV_DIR}/tmp/tests")

# The TEMP/TMP directory of test `name`, in native form.
function(spantui_test_temp_dir name out)
    file(TO_NATIVE_PATH "${SPANTUI_TEST_SCRATCH_DIR}/${name}/temp" _dir)
    set(${out} "${_dir}" PARENT_SCOPE)
endfunction()

# Gives test `name` the shared environment plus its own TEMP, TMP, APPDATA and
# LOCALAPPDATA. OFFSCREEN adds the offscreen platform; EXTRA appends entries.
function(spantui_set_test_environment name)
    cmake_parse_arguments(ARG "OFFSCREEN" "" "EXTRA" ${ARGN})
    spantui_test_temp_dir(${name} _temp)
    file(TO_NATIVE_PATH "${SPANTUI_TEST_SCRATCH_DIR}/${name}/appdata" _appdata)
    file(TO_NATIVE_PATH "${SPANTUI_TEST_SCRATCH_DIR}/${name}/localappdata" _localappdata)
    file(MAKE_DIRECTORY "${_temp}" "${_appdata}" "${_localappdata}")
    set(_env ${SPANTUI_TEST_ENVIRONMENT_MODIFICATION}
        "TEMP=set:${_temp}"
        "TMP=set:${_temp}"
        "APPDATA=set:${_appdata}"
        "LOCALAPPDATA=set:${_localappdata}")
    if(ARG_OFFSCREEN)
        list(APPEND _env ${SPANTUI_OFFSCREEN_ENVIRONMENT_MODIFICATION})
    endif()
    list(APPEND _env ${ARG_EXTRA})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_env}")
endfunction()

# qmlimportscanner runs per Qml executable at configure time to link static
# plugins; with a shared Qt it links nothing yet dominates a reconfigure.
function(spantui_skip_qml_import_scan target)
    set_target_properties(${target} PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endfunction()

# Unit tests link the domain library that owns the code under test; the
# library's own link edges bring in everything below it.
function(spantui_add_library_test target source)
    cmake_parse_arguments(ARG "OFFSCREEN" "" "LIBS;SOURCES" ${ARGN})
    qt_add_executable(${target} ${source} ${ARG_SOURCES})
    spantui_skip_qml_import_scan(${target})
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/tests/support")
    target_link_libraries(${target} PRIVATE ${ARG_LIBS} Qt6::Test)
    add_test(NAME ${target} COMMAND ${target})
    if(ARG_OFFSCREEN)
        spantui_set_test_environment(${target} OFFSCREEN)
    else()
        spantui_set_test_environment(${target})
    endif()
endfunction()

spantui_add_library_test(shell_extension_contract_test
    tests/platform/shell_extension_contract_test.cpp
    LIBS Qt6::Core ole32 shell32 uuid
    SOURCES src/platform/windows/shell_extension_contract.h)
target_include_directories(shell_extension_contract_test PRIVATE src)
target_compile_definitions(shell_extension_contract_test PRIVATE
    SPANTUI_SHELL_EXTENSION_PATH="$<TARGET_FILE:SpanTui-shell-extension>"
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
add_dependencies(shell_extension_contract_test SpanTui-shell-extension SpanTui)

# The composition root's only test: the Debug visual harness starts the
# workspace offscreen with isolated data, opens a real terminal, saves one
# frame and quits. Its .dev/tmp directory keeps git from climbing into this
# checkout.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(TO_NATIVE_PATH "${SPANTUI_DEV_DIR}" SPANTUI_TEST_DEV_DIR)
    spantui_test_temp_dir(workspace_startup_smoke _smoke_temp)
    add_test(NAME workspace_startup_smoke
        COMMAND SpanTui
                --data-dir "${_smoke_temp}\\data"
                "--workspace-shot=${_smoke_temp}\\workspace-smoke.png"
                "--workspace-shot-project=${_smoke_temp}"
                --workspace-shot-terminal
                --workspace-shot-delay-ms=500
                "--resource-report=${_smoke_temp}\\workspace-smoke-resources.jsonl"
                --trace-viewport)
    set_tests_properties(workspace_startup_smoke PROPERTIES TIMEOUT 60)
    spantui_set_test_environment(workspace_startup_smoke OFFSCREEN
        EXTRA "GIT_CEILING_DIRECTORIES=set:${SPANTUI_TEST_DEV_DIR}")
endif()

# CTest runs product tests only: the static checks (tools/check-*.ps1) run
# in `dev.bat check`, the CI check stage and the pre-commit hook.

spantui_add_library_test(settings_test
    tests/application/settings_test.cpp LIBS SpanApplication)
target_compile_definitions(settings_test PRIVATE
    SPANTUI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
spantui_add_library_test(shortcut_registry_test
    tests/application/shortcut_registry_test.cpp LIBS SpanApplication)
spantui_add_library_test(project_workspace_registry_test
    tests/application/project_workspace_registry_test.cpp LIBS SpanApplication)
spantui_add_library_test(tool_registry_test
    tests/application/tool_registry_test.cpp LIBS SpanApplication OFFSCREEN)
spantui_add_library_test(graphics_mode_service_test
    tests/application/graphics_mode_service_test.cpp LIBS SpanApplication OFFSCREEN)
spantui_add_library_test(shell_profile_registry_test
    tests/application/shell_profile_registry_test.cpp LIBS SpanApplication OFFSCREEN)
spantui_add_library_test(workspace_controller_test
    tests/application/workspace_controller_test.cpp LIBS SpanApplication OFFSCREEN)
spantui_add_library_test(ui_health_monitor_test
    tests/application/ui_health_monitor_test.cpp LIBS SpanApplication OFFSCREEN)
spantui_add_library_test(terminal_resource_targets_test
    tests/application/terminal_resource_targets_test.cpp LIBS SpanApplication OFFSCREEN)
spantui_add_library_test(message_center_test
    tests/application/message_center_test.cpp LIBS SpanApplication)
spantui_add_library_test(task_progress_test
    tests/application/task_progress_test.cpp LIBS SpanApplication)
spantui_add_library_test(workspace_session_routing_test
    tests/application/workspace_session_routing_test.cpp
    LIBS SpanApplication OFFSCREEN)
spantui_add_library_test(message_center_wiring_test
    tests/application/message_center_wiring_test.cpp LIBS SpanApplication)

spantui_add_library_test(remote_host_registry_test
    tests/remote/remote_host_registry_test.cpp LIBS SpanRemote Qt6::Sql OFFSCREEN)
spantui_add_library_test(ssh_command_test
    tests/remote/ssh_command_test.cpp LIBS SpanRemote)
spantui_add_library_test(ssh_askpass_test
    tests/remote/ssh_askpass_test.cpp LIBS SpanRemote)
spantui_add_library_test(remote_shell_channel_test
    tests/remote/remote_shell_channel_test.cpp LIBS SpanRemote)
spantui_add_library_test(remote_system_monitor_test
    tests/remote/remote_system_monitor_test.cpp LIBS SpanRemote)
spantui_add_library_test(tailscale_devices_test
    tests/remote/tailscale_devices_test.cpp LIBS SpanRemote)

spantui_add_library_test(file_tree_model_test
    tests/workspace/file_tree_model_test.cpp LIBS SpanWorkspace)
spantui_add_library_test(workspace_layout_test
    tests/workspace/workspace_layout_test.cpp LIBS SpanWorkspace)
spantui_add_library_test(session_model_test
    tests/workspace/session_model_test.cpp LIBS SpanWorkspace)
spantui_add_library_test(file_icon_theme_test
    tests/workspace/file_icon_theme_test.cpp
    LIBS SpanWorkspace)

add_library(spantui_git_test_support STATIC
    tests/git/git_test_support.cpp
    tests/git/git_test_support.h
)
target_include_directories(spantui_git_test_support PUBLIC tests/git)
target_link_libraries(spantui_git_test_support PUBLIC SpanGit Qt6::Test)
spantui_add_library_test(git_parsers_test
    tests/git/git_parsers_test.cpp LIBS spantui_git_test_support)
spantui_add_library_test(git_repository_read_test
    tests/git/git_repository_read_test.cpp LIBS spantui_git_test_support)
spantui_add_library_test(git_repository_concurrency_test
    tests/git/git_repository_concurrency_test.cpp LIBS spantui_git_test_support)
spantui_add_library_test(git_lane_scheduler_test
    tests/git/git_lane_scheduler_test.cpp LIBS spantui_git_test_support)

spantui_add_library_test(process_containment_test
    tests/platform/process_containment_test.cpp LIBS SpanPlatform)
spantui_add_library_test(window_chrome_geometry_test
    tests/platform/window_chrome_geometry_test.cpp LIBS SpanPlatform)
spantui_add_library_test(launch_diagnostics_test
    tests/platform/launch_diagnostics_test.cpp LIBS SpanPlatform)
spantui_add_library_test(directory_change_watcher_test
    tests/platform/directory_change_watcher_test.cpp LIBS SpanPlatform)

spantui_add_library_test(document_manager_test
    tests/documents/document_manager_test.cpp LIBS SpanDocuments)
spantui_add_library_test(editor_controller_test
    tests/documents/editor_controller_test.cpp LIBS SpanDocuments)
spantui_add_library_test(update_service_test
    tests/update/update_service_test.cpp LIBS SpanUpdate)
spantui_add_library_test(product_paths_test
    tests/core/product_paths_test.cpp LIBS SpanCore)
spantui_add_library_test(gpu_engine_metrics_test
    tests/monitoring/gpu_engine_metrics_test.cpp LIBS SpanMonitoring)
spantui_add_library_test(resource_monitor_test
    tests/monitoring/resource_monitor_test.cpp LIBS SpanMonitoring Qt6::Gui OFFSCREEN)

# Analytic rounded surfaces are compiled once into portable Qt Shader Baker
# packages. The runtime path stays on Qt Quick's RHI (D3D11 on normal Windows
# systems) without opting the whole window into MSAA or an offscreen layer.
qt_add_shaders(SpanTui "spantui_boundary_shaders"
    BATCHABLE
    PREFIX "/shaders"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    FILES
        shaders/crisp_surface.vert
        shaders/crisp_surface.frag
)

add_custom_target(spantui_update_translations
    COMMAND "$<TARGET_FILE:Qt6::lupdate>"
            "${CMAKE_SOURCE_DIR}/qml" "${CMAKE_SOURCE_DIR}/src"
            -recursive -locations none -no-obsolete
            -ts "${CMAKE_SOURCE_DIR}/translations/SpanTui_zh_CN.ts"
    COMMENT "Updating the committed translation catalog"
    VERBATIM
)

function(spantui_add_terminal_test target source)
    spantui_add_library_test(${target} ${source} LIBS SpanTerminal Qt6::Gui Qt6::Quick OFFSCREEN)
endfunction()

spantui_add_terminal_test(ghostty_engine_test tests/terminal/ghostty_engine_test.cpp)
target_link_libraries(ghostty_engine_test PRIVATE SpanGhosttyVt)
spantui_add_terminal_test(terminal_font_set_test tests/terminal/terminal_font_set_test.cpp)
target_compile_definitions(terminal_font_set_test PRIVATE
    SPANTUI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
spantui_add_terminal_test(terminal_session_test tests/terminal/terminal_session_test.cpp)
# Builds engines of its own for the session's engine factory.
target_link_libraries(terminal_session_test PRIVATE SpanGhosttyVt)
spantui_add_terminal_test(terminal_view_binding_test tests/terminal/terminal_view_binding_test.cpp)
spantui_add_terminal_test(terminal_core_worker_test tests/terminal/terminal_core_worker_test.cpp)
# Ten-second throughput benchmark of the same worker: built with the tests,
# never registered in CTest; run by hand (docs/workflow/testing.md §3).
qt_add_executable(terminal_core_worker_benchmark tests/terminal/terminal_core_worker_benchmark.cpp)
target_link_libraries(terminal_core_worker_benchmark PRIVATE SpanTerminal Qt6::Gui Qt6::Quick Qt6::Test)
# Frame benchmark of the render path, also run by hand only. The private
# modules serve the probe alone: QRhi reads back what the frame shows, and
# QQuickItemPrivate exposes the view's scene-graph nodes for counting.
qt_add_executable(terminal_render_benchmark tests/terminal/terminal_render_benchmark.cpp)
target_link_libraries(terminal_render_benchmark PRIVATE
    SpanTerminal Qt6::Gui Qt6::GuiPrivate Qt6::Quick Qt6::QuickPrivate)
target_compile_definitions(terminal_render_benchmark PRIVATE
    SPANTUI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
spantui_skip_qml_import_scan(terminal_render_benchmark)
spantui_add_terminal_test(terminal_manager_test tests/terminal/terminal_manager_test.cpp)
spantui_add_terminal_test(win_conpty_process_test tests/terminal/win_conpty_process_test.cpp)
spantui_add_terminal_test(terminal_input_routing_test tests/terminal/terminal_input_routing_test.cpp)

spantui_add_library_test(markdown_gfm_adapter_test
    tests/markdown/markdown_gfm_adapter_test.cpp LIBS SpanDocuments)
spantui_add_library_test(markdown_network_policy_test
    tests/markdown/markdown_network_policy_test.cpp LIBS SpanDocuments)
spantui_add_library_test(markdown_remote_image_loader_test
    tests/markdown/markdown_remote_image_loader_test.cpp LIBS SpanDocuments)
spantui_add_library_test(markdown_document_renderer_test
    tests/markdown/markdown_document_renderer_test.cpp LIBS SpanDocuments)

spantui_add_library_test(workspace_database_migration_test
    tests/core/workspace_database_migration_test.cpp LIBS SpanCore Qt6::Sql)

# internal_launch_options belongs to the executable's runtime/, so the test
# compiles it directly.
spantui_add_library_test(launch_options_test
    tests/application/launch_options_test.cpp
    LIBS SpanApplication
    SOURCES src/application/runtime/internal_launch_options.cpp
            src/application/runtime/internal_launch_options.h)

spantui_add_library_test(single_instance_router_test
    tests/application/single_instance_router_test.cpp LIBS SpanApplication)
qt_add_executable(single_instance_client_helper
    tests/application/single_instance_client_helper.cpp
)
target_link_libraries(single_instance_client_helper PRIVATE SpanApplication)
spantui_skip_qml_import_scan(single_instance_client_helper)
add_dependencies(single_instance_router_test single_instance_client_helper)

add_executable(pty_echo_child tests/terminal/pty_echo_child.cpp)
target_compile_definitions(pty_echo_child PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)
add_dependencies(terminal_session_test pty_echo_child)
# Observes the window's IME context the way Windows sees it.
target_link_libraries(terminal_session_test PRIVATE imm32)
add_dependencies(win_conpty_process_test pty_echo_child)

# Records the command line it was started with (tests/platform/argv_echo_child.cpp).
add_executable(argv_echo_child tests/platform/argv_echo_child.cpp)
target_link_libraries(argv_echo_child PRIVATE shell32)
add_dependencies(process_containment_test argv_echo_child)
add_dependencies(shell_extension_contract_test argv_echo_child)
add_dependencies(update_service_test argv_echo_child)
add_dependencies(tool_registry_test argv_echo_child)

# Core tests: see docs/workflow/testing.md.
set(SPANTUI_CORE_TESTS
    ghostty_engine_test win_conpty_process_test
    terminal_core_worker_test terminal_session_test terminal_manager_test
    terminal_input_routing_test
    workspace_database_migration_test settings_test product_paths_test
    document_manager_test workspace_layout_test workspace_controller_test
    process_containment_test
    workspace_startup_smoke single_instance_router_test update_service_test
    message_center_test message_center_wiring_test)
foreach(_core_test IN LISTS SPANTUI_CORE_TESTS)
    if(TEST ${_core_test})
        set_tests_properties(${_core_test} PROPERTIES LABELS core)
    elseif(NOT _core_test STREQUAL "workspace_startup_smoke") # Debug trees only
        message(FATAL_ERROR "SPANTUI_CORE_TESTS names an unregistered test: ${_core_test}")
    endif()
endforeach()
