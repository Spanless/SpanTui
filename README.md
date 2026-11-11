# SpanTui

[简体中文](README.zh-CN.md)

SpanTui is a native Windows workspace and terminal app built with Qt 6 and QML. It puts project browsing, code and Markdown viewing, Git diffs, SSH remote hosts and ConPTY terminals into one compact window, so you switch windows less.

## Features

- Manage multiple local projects and their file trees.
- Read-only code and text viewer with find, go to line, line numbers, Git hunk markers and Markdown preview.
- Git status, commit history graph and side-by-side or unified diffs. Write operations stay in the terminal.
- Multiple ConPTY terminals in split panes, with scrollback kept for the current run, modern escape-sequence support powered by libghostty-vt (OSC 8 links, Kitty images and keyboard protocol, synchronized output) and a bundled ConPTY.
- SSH remote hosts: browse remote files over SFTP and open remote terminals, using the system OpenSSH client.
- Light and dark themes, window materials, colors, fonts, cursor and terminal refresh rate are all configurable.
- Explorer folder context-menu entry, a standalone single-terminal mode, and in-app updates that run the installer silently (portable copies are told where to download).
- UI in English and Simplified Chinese.

## Requirements

- Windows 10 1809 (build 17763) or later, x64. Windows 11 on ARM64 runs it through x64 emulation.

## Install

Download the `SpanTui-<version>-Setup.exe` installer or the portable package from the project's Releases page. The installer needs no administrator rights, with one exception: on Windows 11 the Explorer context-menu option (ticked by default) asks for one approval to trust SpanTui's self-signed publisher certificate. Declining falls back to the classic entry under "Show more options"; unticking the option adds no entry.

Your project files stay where they are. SpanTui keeps everything it writes (settings, project list, workspace restore state, logs and caches) in one data directory: `%LOCALAPPDATA%\SpanTui` for the installer, or the `data` folder next to `SpanTui.exe` for the portable package (the `portable.txt` file beside it turns that on). Start it with `--data-dir <folder>` to use any other folder. Terminal output is not restored on the next launch. Removing a project from SpanTui never deletes the folder on disk.

## Build from source

You need:

- Qt 6.12.x for MinGW 64-bit, with the Qt Shader Tools and Qt SVG modules
- MinGW 13.1 64-bit, CMake 3.22 or later, and Ninja (all available in the Qt installer's Tools section)
- Inno Setup 6 and the Windows SDK, only if you build the installer

If Qt is installed under `C:\Qt`, the build scripts find it on their own. Otherwise copy `.env.example` to `.env` and set `SPANTUI_QT_SDK_ROOT` to the Qt install root (or `SPANTUI_QT_ROOT`, `SPANTUI_CMAKE`, `SPANTUI_MINGW_BIN` and `SPANTUI_NINJA_DIR` one by one); environment variables of the same name still win. See [docs/workflow/build.md](docs/workflow/build.md).

Build trees and packages go to the git-ignored `target\` folder of the checkout; set `SPANTUI_BUILD_ROOT` and `SPANTUI_ARTIFACT_ROOT` in `.env` to put them somewhere else. Development runs, tests and scratch files write only into the git-ignored `.dev\` folder, never into your own `%APPDATA%` or TEMP. `dev.bat clean` deletes it.

```cmd
dev.bat build                    :: incremental debug build -> target\build\SpanTui-test.exe
start.bat                        :: build, then launch the development build
dev.bat test -R terminal         :: build and run the tests matching a regex
package.bat                      :: portable package and installer in target\release\<version>\
dev.bat clean                    :: delete .dev\ (development data, test scratch)
```

The bundled ConPTY binaries in `src/terminal/vendor/conpty/x64/` can be refreshed from NuGet with `tools/update-conpty.ps1`.

## Documentation

Developer documentation is in [docs/](docs/index.md) (mostly in Chinese). Start with the [architecture overview](docs/architecture/overview.md). [AGENTS.md](AGENTS.md) holds the rules that AI coding assistants follow in this repository.

## Contributing

Issues and pull requests are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Report security problems as described in [SECURITY.md](SECURITY.md), not in public issues.

## License

SpanTui is released under the [MIT License](LICENSE). It ships third-party components under their own licenses: Qt (LGPL-3.0), libghostty-vt and ConPTY (MIT), and fonts derived from Geist, Figtree and Nerd Fonts. See [third-party notices](docs/legal/third-party-notices.md).
