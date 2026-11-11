@echo off
setlocal EnableExtensions EnableDelayedExpansion

call "%~dp0dev.bat" package %*
set "ERR=!ERRORLEVEL!"

rem Explorer double-click starts: cmd /c ""...\package.bat" " [args]
rem That session dies when the script ends, so keep it open to read output.
rem Use delayed expansion so CMDCMDLINE values containing & or && are not
rem re-parsed as command separators (which would re-enter this script).
set "PAUSE_AT_END="
echo(!CMDCMDLINE!) | find /I "/c" >nul 2>&1
if errorlevel 1 goto :after_pause_detect
echo(!CMDCMDLINE!) | find /I "%~f0" >nul 2>&1
if errorlevel 1 goto :after_pause_detect
echo(!CMDCMDLINE!) | find "&&" >nul 2>&1
if not errorlevel 1 goto :after_pause_detect
set "PAUSE_AT_END=1"
:after_pause_detect

if defined PAUSE_AT_END (
  echo.
  if !ERR! neq 0 (
    echo [package.bat] failed with exit code !ERR!.
  ) else (
    echo [package.bat] finished; the output paths are listed above.
  )
  pause
)

exit /b !ERR!
