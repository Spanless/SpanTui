#pragma once

#include <QLatin1StringView>

namespace span::application {

// The QML module URI (qt_add_qml_module in CMakeLists.txt).
inline constexpr QLatin1StringView kQmlModuleUri{"SpanTui"};

// Keys of the settings table. Renaming one needs a WorkspaceDatabaseMigration
// step.
inline constexpr QLatin1StringView kSettingLanguage{"language"};
inline constexpr QLatin1StringView kSettingThemeMode{"themeMode"};
inline constexpr QLatin1StringView kSettingRenderMode{"renderMode"};
inline constexpr QLatin1StringView kSettingEditorLineHeight{"editorLineHeight"};
// A font lane stores <lane key><suffix>, e.g. terminalFontSize.
inline constexpr QLatin1StringView kSettingPrimaryFontFamilySuffix{"PrimaryFontFamily"};
inline constexpr QLatin1StringView kSettingFallbackFontFamilySuffix{"FallbackFontFamily"};
inline constexpr QLatin1StringView kSettingFontSizeSuffix{"FontSize"};
inline constexpr QLatin1StringView kSettingFontWeightSuffix{"FontWeight"};
inline constexpr QLatin1StringView kSettingTerminalColorSchemes{"terminal.colorSchemes"};
inline constexpr QLatin1StringView kSettingTerminalColorSchemeDark{"terminal.colorSchemeDark"};
inline constexpr QLatin1StringView kSettingTerminalColorSchemeLight{"terminal.colorSchemeLight"};
inline constexpr QLatin1StringView kSettingTerminalCursorStyle{"terminal.cursorStyle"};
inline constexpr QLatin1StringView kSettingTerminalRefreshRate{"terminal.refreshRate"};
inline constexpr QLatin1StringView kSettingTerminalShellProfiles{"terminal.shellProfiles"};
inline constexpr QLatin1StringView kSettingWorkspaceLayoutJson{"workspace.layoutJson"};
inline constexpr QLatin1StringView kSettingWorkspaceScriptRunMode{"workspace.scriptRunMode"};
inline constexpr QLatin1StringView kSettingWorkspaceTerminateOnTileClose{"workspace.terminateTerminalOnTileClose"};
inline constexpr QLatin1StringView kSettingMonitorIntervalSeconds{"monitor.intervalSeconds"};
inline constexpr QLatin1StringView kSettingShortcutBindings{"shortcuts.bindings"};
inline constexpr QLatin1StringView kSettingToolPaths{"tools.paths"};
inline constexpr QLatin1StringView kSettingWindowX{"window.x"};
inline constexpr QLatin1StringView kSettingWindowY{"window.y"};
inline constexpr QLatin1StringView kSettingWindowWidth{"window.width"};
inline constexpr QLatin1StringView kSettingWindowHeight{"window.height"};
inline constexpr QLatin1StringView kSettingWindowMaximized{"window.maximized"};
inline constexpr QLatin1StringView kSettingActiveSessionByProject{"sessions.activeByProject"};

// Setting values.
inline constexpr QLatin1StringView kLanguageChinese{"zh_CN"};
inline constexpr QLatin1StringView kLanguageEnglish{"en"};
inline constexpr QLatin1StringView kThemeSystem{"system"};
inline constexpr QLatin1StringView kThemeLight{"light"};
inline constexpr QLatin1StringView kThemeDark{"dark"};
inline constexpr QLatin1StringView kRenderAuto{"auto"};
inline constexpr QLatin1StringView kRenderSoftware{"software"};
inline constexpr QLatin1StringView kScriptRunProjectSession{"projectSession"};
inline constexpr QLatin1StringView kScriptRunIndependentWindow{"independentWindow"};

inline constexpr int kMonitorIntervalDefaultSeconds = 2;
inline constexpr int kMonitorIntervalMinSeconds = 1;
inline constexpr int kMonitorIntervalMaxSeconds = 60;

// Time for an icon motion to return to rest once its trigger ends.
inline constexpr double kIconMotionSettleMs = 280;

} // namespace span::application
