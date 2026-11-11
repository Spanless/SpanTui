@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
call "%ROOT%dev.bat" build
if errorlevel 1 (
  echo.
  echo ERROR: build failed; see output above.
  pause
  exit /b 1
)

set "SPANTUI_START_BUILD="
for /f "usebackq tokens=1,* delims==" %%I in (`powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; . (Join-Path $env:ROOT 'tools\toolchain.ps1'); $build = Resolve-SpanTuiBuildDirectory -Root $env:ROOT; Use-SpanTuiToolchainPath (Resolve-SpanTuiToolchain); 'PATH=' + $env:PATH; 'SPANTUI_START_BUILD=' + $build"`) do set "%%I=%%J"
if not defined SPANTUI_START_BUILD (
  echo ERROR: Could not prepare the SpanTui launch environment.
  pause
  exit /b 1
)

rem Run attached so the app's stdout/stderr stream into this console (a
rem GUI-subsystem exe needs these Qt vars to emit logs at all), and a failed
rem launch stays visible instead of the window closing silently.
set "QT_ASSUME_STDERR_HAS_CONSOLE=1"
set "QT_FORCE_STDERR_LOGGING=1"
rem All data of this run stays in the checkout's .dev\data (git-ignored).
set "DEV_DATA_DIR=%ROOT%.dev\data"
echo Starting %SPANTUI_START_BUILD%\SpanTui-test.exe --data-dir "%DEV_DATA_DIR%" %*
"%SPANTUI_START_BUILD%\SpanTui-test.exe" --data-dir "%DEV_DATA_DIR%" %*
set "APP_EXIT=%ERRORLEVEL%"
echo.
if "%APP_EXIT%"=="0" (
  echo SpanTui-test exited cleanly.
) else (
  echo ERROR: SpanTui-test exited with code %APP_EXIT%.
  powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "foreach ($p in @($env:DEV_DATA_DIR + '\last-launch.log', $env:DEV_DATA_DIR + '\crash-latest.log')) { if (Test-Path -LiteralPath $p) { Write-Host ('---- ' + $p + ' ----') -ForegroundColor Yellow; Get-Content -LiteralPath $p -Tail 80 | ForEach-Object { Write-Host $_ } } }"
  pause
)
exit /b %APP_EXIT%
