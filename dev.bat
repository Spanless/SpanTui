@echo off
setlocal EnableExtensions

set "DEV_PS=powershell.exe"
where pwsh.exe >nul 2>nul && set "DEV_PS=pwsh.exe"
"%DEV_PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\devtool\dev.ps1" %*
exit /b %ERRORLEVEL%
