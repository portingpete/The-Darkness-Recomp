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
set "STUTTER_LOG=build_native\run\stutter-%RANDOM%-%RANDOM%.log"
if exist "%STUTTER_LOG%" goto choose_log
echo Stutter capture: plays normally WITH sound using your saved graphics settings.
echo No screenshots or profiling overhead; only lightweight slow-frame timings.
echo.
echo 1. Resume your game and go to the area that stutters.
echo 2. Play normally for 2-3 minutes; include one area transition if relevant.
echo 3. Close the game normally when finished.
echo.
echo Intro videos are skipped automatically until gameplay starts.
echo Recording to "%STUTTER_LOG%"
"build_native\Release\DarkRecomp.exe" --game-dir "%~dp0Darkness" --engine-preview --timeout-ms 0 --trace-frame-hitches --test-skip-intros > "%STUTTER_LOG%" 2>&1
set "STUTTER_EXIT=%ERRORLEVEL%"
echo.
echo Recording finished. Game exit code: %STUTTER_EXIT%
for /f %%N in ('findstr /c:"[FrameOutlier]" "%STUTTER_LOG%" ^| find /c /v ""') do set OUTLIERS=%%N
echo Slow-frame events recorded: %OUTLIERS% (a few at startup/transitions is normal).
echo If the game stuttered, send "%~dp0%STUTTER_LOG%" for analysis.
pause
exit /b %STUTTER_EXIT%
