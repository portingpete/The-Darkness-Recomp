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
set "PROFILE_LOG=build_native\run\performance-%RANDOM%-%RANDOM%.log"
if exist "%PROFILE_LOG%" goto choose_log
echo Performance recording is enabled. Your saved graphics settings will be used.
echo Resume your game and return to the area with the slowdown.
echo Look at the ground for 10 seconds, then look up for 20 seconds.
echo Repeat once, then close the game normally.
echo.
echo Profiling adds overhead, so use your normal launcher for regular play.
echo Recording to "%PROFILE_LOG%"
"build_native\Release\DarkRecomp.exe" --game-dir "%~dp0Darkness" --engine-preview --timeout-ms 0 --profile-engine --sample-engine > "%PROFILE_LOG%" 2>&1
set "PROFILE_EXIT=%ERRORLEVEL%"
echo.
echo Recording finished. Game exit code: %PROFILE_EXIT%
echo Saved log: "%~dp0%PROFILE_LOG%"
pause
exit /b %PROFILE_EXIT%
