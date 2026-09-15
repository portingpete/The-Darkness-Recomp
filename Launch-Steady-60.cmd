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
set "SMOOTHNESS_LOG=build_native\run\smoothness-60-%RANDOM%-%RANDOM%.log"
if exist "%SMOOTHNESS_LOG%" goto choose_log
echo Testing a 60 FPS ceiling for this run. Your other saved graphics settings are used.
echo This launcher does not edit your saved settings file.
echo Use Launch-With-Sound.cmd afterward to return to your saved frame limit.
echo For this comparison, leave Video Settings unchanged and revisit the same area.
echo Close the game normally when finished.
echo Recording to "%SMOOTHNESS_LOG%"
"build_native\Release\DarkRecomp.exe" --game-dir "%~dp0Darkness" --engine-preview --timeout-ms 0 --trace-frame-hitches --fps 60 > "%SMOOTHNESS_LOG%" 2>&1
set "SMOOTHNESS_EXIT=%ERRORLEVEL%"
echo Finished. Game exit code: %SMOOTHNESS_EXIT%
echo Saved log: "%~dp0%SMOOTHNESS_LOG%"
pause
exit /b %SMOOTHNESS_EXIT%
