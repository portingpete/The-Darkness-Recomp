@echo off
rem The Darkness Recomp - single launcher.
rem Usage: Launch.cmd [mode] [extra game arguments...]
rem   play (default) - play with sound
rem   mute           - play muted
rem   preview        - engine preview build (muted by default)
rem   performance    - record an engine profile log for analysis
rem   render-profile - record a render-thread profile log for analysis
rem   steady-60      - one run with a 60 FPS ceiling plus a smoothness log
rem   stutter        - play with sound while recording slow-frame timings
rem Extra arguments are forwarded to the game, e.g. Launch.cmd play --fps 120
setlocal
cd /d "%~dp0"

set "MODE=play"
if not "%~1"=="" (
    for %%M in (play mute preview performance render-profile steady-60 stutter) do (
        if /i "%~1"=="%%M" set "MODE=%~1"
    )
    if /i "%~1"=="help" goto usage
    if "%~1"=="--help" goto usage
    if "%~1"=="-h" goto usage
    if "%~1"=="/?" goto usage
)
if /i "%MODE%"=="%~1" shift
set "REST="
:collect_args
if "%~1"=="" goto collected_args
set "REST=%REST% %1"
shift
goto collect_args
:collected_args

if /i "%MODE%"=="play" goto play
if /i "%MODE%"=="mute" goto mute
if /i "%MODE%"=="preview" goto preview
if /i "%MODE%"=="performance" goto performance
if /i "%MODE%"=="render-profile" goto render_profile
if /i "%MODE%"=="steady-60" goto steady_60
if /i "%MODE%"=="stutter" goto stutter
goto usage

:play
if not exist "build_native\Release\DarkRecompPreview.exe" goto missing
echo Launching The Darkness with sound. Click in the game for mouse look; F1 shows controls.
start "" "%~dp0build_native\Release\DarkRecompPreview.exe" --sound %REST%
exit /b 0

:mute
if not exist "build_native\Release\DarkRecompPreview.exe" goto missing
echo Launching The Darkness muted. Click in the game for mouse look; F1 shows controls.
start "" "%~dp0build_native\Release\DarkRecompPreview.exe" --mute %REST%
exit /b 0

:preview
if not exist "build_native\Release\DarkRecompPreview.exe" goto missing
echo Launching The Darkness preview. Click in the game for mouse look; F1 shows controls.
start "" "%~dp0build_native\Release\DarkRecompPreview.exe" %REST%
exit /b 0

:performance
if not exist "build_native\Release\DarkRecomp.exe" goto missing
if not exist "build_native\run" mkdir "build_native\run"
:choose_perf_log
set "PROFILE_LOG=build_native\run\performance-%RANDOM%-%RANDOM%.log"
if exist "%PROFILE_LOG%" goto choose_perf_log
echo Performance recording is enabled. Your saved graphics settings will be used.
echo Resume your game and return to the area with the slowdown.
echo Look at the ground for 10 seconds, then look up for 20 seconds.
echo Repeat once, then close the game normally.
echo.
echo Profiling adds overhead, so use the normal play mode for regular play.
echo Recording to "%PROFILE_LOG%"
"build_native\Release\DarkRecomp.exe" --game-dir "%~dp0Darkness" --engine-preview --timeout-ms 0 --profile-engine --sample-engine %REST% > "%PROFILE_LOG%" 2>&1
set "PROFILE_EXIT=%ERRORLEVEL%"
echo.
echo Recording finished. Game exit code: %PROFILE_EXIT%
echo Saved log: "%~dp0%PROFILE_LOG%"
pause
exit /b %PROFILE_EXIT%

:render_profile
if not exist "build_native\Release\DarkRecomp.exe" goto missing
if not exist "build_native\run" mkdir "build_native\run"
:choose_render_log
set "RENDER_PROFILE_LOG=build_native\run\render-profile-%RANDOM%-%RANDOM%.log"
if exist "%RENDER_PROFILE_LOG%" goto choose_render_log
echo Recording the rendering thread. Your saved graphics settings will be used.
echo Return to the area with the stutter and look around for about a minute.
echo Close the game normally when finished.
echo This diagnostic recording adds some measurement overhead.
echo Recording to "%RENDER_PROFILE_LOG%"
"build_native\Release\DarkRecomp.exe" --game-dir "%~dp0Darkness" --engine-preview --timeout-ms 0 --trace-frame-hitches --sample-renderer %REST% > "%RENDER_PROFILE_LOG%" 2>&1
set "RENDER_PROFILE_EXIT=%ERRORLEVEL%"
echo Recording finished. Game exit code: %RENDER_PROFILE_EXIT%
echo Saved log: "%~dp0%RENDER_PROFILE_LOG%"
pause
exit /b %RENDER_PROFILE_EXIT%

:steady_60
if not exist "build_native\Release\DarkRecomp.exe" goto missing
if not exist "build_native\run" mkdir "build_native\run"
:choose_smooth_log
set "SMOOTHNESS_LOG=build_native\run\smoothness-60-%RANDOM%-%RANDOM%.log"
if exist "%SMOOTHNESS_LOG%" goto choose_smooth_log
echo Testing a 60 FPS ceiling for this run. Your other saved graphics settings are used.
echo This run does not edit your saved settings file.
echo Use the default play mode afterward to return to your saved frame limit.
echo For this comparison, leave Video Settings unchanged and revisit the same area.
echo Close the game normally when finished.
echo Recording to "%SMOOTHNESS_LOG%"
"build_native\Release\DarkRecomp.exe" --game-dir "%~dp0Darkness" --engine-preview --timeout-ms 0 --trace-frame-hitches --fps 60 %REST% > "%SMOOTHNESS_LOG%" 2>&1
set "SMOOTHNESS_EXIT=%ERRORLEVEL%"
echo Finished. Game exit code: %SMOOTHNESS_EXIT%
echo Saved log: "%~dp0%SMOOTHNESS_LOG%"
pause
exit /b %SMOOTHNESS_EXIT%

:stutter
if not exist "build_native\Release\DarkRecomp.exe" goto missing
if not exist "build_native\run" mkdir "build_native\run"
:choose_stutter_log
set "STUTTER_LOG=build_native\run\stutter-%RANDOM%-%RANDOM%.log"
if exist "%STUTTER_LOG%" goto choose_stutter_log
echo Stutter capture: plays normally WITH sound using your saved graphics settings.
echo No screenshots or profiling overhead; only lightweight slow-frame timings.
echo.
echo 1. Resume your game and go to the area that stutters.
echo 2. Play normally for 2-3 minutes; include one area transition if relevant.
echo 3. Close the game normally when finished.
echo.
echo Intro videos are skipped automatically until gameplay starts.
echo Recording to "%STUTTER_LOG%"
"build_native\Release\DarkRecomp.exe" --game-dir "%~dp0Darkness" --engine-preview --timeout-ms 0 --trace-frame-hitches --test-skip-intros %REST% > "%STUTTER_LOG%" 2>&1
set "STUTTER_EXIT=%ERRORLEVEL%"
echo.
echo Recording finished. Game exit code: %STUTTER_EXIT%
for /f %%N in ('findstr /c:"[FrameOutlier]" "%STUTTER_LOG%" ^| find /c /v ""') do set OUTLIERS=%%N
echo Slow-frame events recorded: %OUTLIERS% (a few at startup/transitions is normal).
echo If the game stuttered, send "%~dp0%STUTTER_LOG%" for analysis.
pause
exit /b %STUTTER_EXIT%

:missing
echo The built game was not found. Build it first with:
echo   powershell -ExecutionPolicy Bypass -File tools\build.ps1
pause
exit /b 1

:usage
echo Usage: Launch.cmd [mode] [extra game arguments...]
echo.
echo   play ^(default^)  Play with sound.
echo   mute            Play muted.
echo   preview         Engine preview build (muted by default).
echo   performance     Record an engine profile log for analysis.
echo   render-profile  Record a render-thread profile log for analysis.
echo   steady-60       One run with a 60 FPS ceiling plus a smoothness log.
echo   stutter         Play with sound while recording slow-frame timings.
echo.
echo Examples:
echo   Launch.cmd
echo   Launch.cmd mute
echo   Launch.cmd play --fps 120
echo   Launch.cmd stutter
exit /b 0
