@echo off
setlocal
cd /d "%~dp0"
if not exist "build_native\Release\DarkRecompPreview.exe" (
    echo The built game is missing from build_native\Release.
    pause
    exit /b 1
)
start "" "%~dp0build_native\Release\DarkRecompPreview.exe" --sound --shadow-capture
