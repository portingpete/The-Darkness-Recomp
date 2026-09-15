@echo off
cd /d "%~dp0"
echo Launching The Darkness in Xenia Canary...
start "" "%~dp0xenia\canary\xenia_canary.exe" "%~dp0Darkness\default.xex"
