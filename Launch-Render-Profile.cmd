@echo off
setlocal
cd /d "%~dp0"
if not exist "build_native\Release\DarkRecomp.exe" (
    echo The built game was not found beside this launcher.
    pause
    exit /b 1
)
if not exist "build_native\run" mkdir "build_native\run"
:choose_log
set "RENDER_PROFILE_LOG=build_native\run\render-profile-%RANDOM%-%RANDOM%.log"
if exist "%RENDER_PROFILE_LOG%" goto choose_log
echo Recording the rendering thread. Your saved graphics settings will be used.
echo Return to the area with the stutter and look around for about a minute.
echo Close the game normally when finished.
echo This diagnostic recording adds some measurement overhead.
echo Recording to "%RENDER_PROFILE_LOG%"
"build_native\Release\DarkRecomp.exe" --game-dir "%~dp0Darkness" --engine-preview --timeout-ms 0 --trace-frame-hitches --sample-renderer > "%RENDER_PROFILE_LOG%" 2>&1
set "RENDER_PROFILE_EXIT=%ERRORLEVEL%"
echo Recording finished. Game exit code: %RENDER_PROFILE_EXIT%
echo Saved log: "%~dp0%RENDER_PROFILE_LOG%"
pause
exit /b %RENDER_PROFILE_EXIT%
