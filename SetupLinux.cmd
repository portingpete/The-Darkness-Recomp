@echo off
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup_linux.ps1" %*
set "DARK_LINUX_EXIT=%errorlevel%"
if not "%DARK_LINUX_EXIT%"=="0" pause
exit /b %DARK_LINUX_EXIT%
