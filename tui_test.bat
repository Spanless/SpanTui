@echo off
setlocal

for %%I in ("%~dp0.") do set "PROJECT_ROOT=%%~fI"
set "STRESS_BUILD_SCRIPT=%~dp0tools\terminal_stress_tui\build.ps1"

rem Build tree: target\build, or this checkout's tree under SPANTUI_BUILD_ROOT (environment or .env).
set "BUILD_DIR="
for /f "usebackq delims=" %%B in (`powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; . (Join-Path $env:PROJECT_ROOT 'tools\toolchain.ps1'); Resolve-SpanTuiBuildDirectory -Root $env:PROJECT_ROOT"`) do set "BUILD_DIR=%%B"
if not defined BUILD_DIR exit /b 1
set "APP_EXE=%BUILD_DIR%\SpanTui-test.exe"
set "STRESS_EXE=%BUILD_DIR%\terminal-stress-tui\terminal-stress-tui.exe"

set "STRESS_ARGS="
:stress_args
if "%~1"=="" goto stress_build
set "STRESS_ARGS=%STRESS_ARGS% %1"
shift
goto stress_args

:stress_build
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%STRESS_BUILD_SCRIPT%"
if errorlevel 1 exit /b %errorlevel%
if not exist "%STRESS_EXE%" exit /b 1
if /I "%TERM_PROGRAM%"=="SpanTui" goto run_stress_here

if not exist "%APP_EXE%" call "%~dp0dev.bat" build
if errorlevel 1 exit /b %errorlevel%
start "" "%APP_EXE%" --data-dir "%~dp0.dev\data" --terminal --cwd "%PROJECT_ROOT%" --command "\"%STRESS_EXE%\"%STRESS_ARGS%"
exit /b %errorlevel%

:run_stress_here
"%STRESS_EXE%"%STRESS_ARGS%
exit /b %errorlevel%
