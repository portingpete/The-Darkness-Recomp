@echo off
cd /d "%~dp0"
echo Launching The Darkness muted. Click in the game for mouse look; F1 shows controls.
start "" "%~dp0build_native\Release\DarkRecompPreview.exe"
