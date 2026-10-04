@echo off
rem Choose game settings and language, then start the game with sound.
setlocal DisableDelayedExpansion
cd /d "%~dp0"
if not exist "build_native\Release\DarkRecompSettings.exe" (
    echo The settings launcher is missing from this folder.
    echo Extract the ENTIRE Windows release ZIP, then run LaunchWithSettings.cmd again.
    if exist "tools\build.ps1" echo Building from source? Run: powershell -ExecutionPolicy Bypass -File tools\build.ps1
    pause
    exit /b 1
)
start "" "%~dp0build_native\Release\DarkRecompSettings.exe"
exit /b 0
