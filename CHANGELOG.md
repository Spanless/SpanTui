# Changelog

All notable changes to SpanTui are recorded here. Versions follow [Semantic Versioning](https://semver.org/); the version number lives in `CMakeLists.txt` (`project(SpanTui VERSION ...)`) and releases are tagged `v<version>`.

## [Unreleased]

## [0.1.1]

- Paste images into terminals: a program that takes Kitty paste events (mode 5522) receives the image itself; otherwise a local session saves the image and pastes its path. Pasted text also goes to such programs as a paste event, and each paste keeps its place among the keys typed around it.
- Font presets in Settings > Interface: System (Segoe UI, Cascadia Mono or Consolas, Microsoft YaHei UI), Span (the bundled fonts) or Custom; font sizes stay per area, and all font settings now live on the Interface page.
- Workspace layout: the title bar, sidebar, pane headers and right dock share one top row; Files, Changes and History switch in the right dock's header; the sidebar footer is a single compact row.
- Fixed: an intermittent crash when resource monitoring stopped.
- macOS (Apple Silicon) builds from source; releases remain Windows-only.

## [0.1.0]

First public release, for Windows 10 1809 or later (x64), as an installer (`SpanTui-0.1.0-Setup.exe`) and a portable zip.

- Workspace with multiple local projects and their file trees.
- Read-only code and text viewer with find, go to line, line numbers, Git hunk markers and Markdown preview.
- Git status, commit history graph, branches, and side-by-side or unified diffs; write operations stay in the terminal.
- Multiple ConPTY terminals in split panes, with scrollback for the current run and modern escape-sequence support powered by libghostty-vt (OSC 8 links, Kitty images and keyboard protocol, synchronized output); a ConPTY is bundled.
- SSH remote hosts: browse remote files over SFTP and open remote terminals with the system OpenSSH client.
- Light and dark themes; window material, colors, fonts, cursor and terminal refresh rate are configurable.
- Explorer folder context-menu entry and a standalone single-terminal mode.
- In-app updates: an installed copy downloads the setup of the latest stable release, checks its digest and installs it with only a progress window; portable copies are pointed to the download page.
- Interface in Simplified Chinese (the default) and English.
- All data lives under one folder: `%LOCALAPPDATA%\SpanTui`, the `data` folder of a portable copy, or the folder given with `--data-dir`.
