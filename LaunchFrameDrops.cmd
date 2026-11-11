@echo off
rem Short frame-drop capture: every frame's timings plus stall attribution.
rem Extra arguments are forwarded exactly as with Launch.cmd frame-drops.
setlocal DisableDelayedExpansion
cd /d "%~dp0"
call "%~dp0Launch.cmd" frame-drops %*
exit /b %ERRORLEVEL%
