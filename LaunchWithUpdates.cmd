@echo off
setlocal DisableDelayedExpansion
set "DARKRECOMP_INSTALL_ROOT=%~dp0"
rem This entire block is parsed before updating, so replacing this CMD is safe.
(
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $env:DARKRECOMP_INSTALL_ROOT 'tools\update_release.ps1')))) -InstallRoot $env:DARKRECOMP_INSTALL_ROOT"
    if errorlevel 20 (
        echo The update could not be installed. See the message above.
        pause
        exit /b 20
    )
    if errorlevel 10 echo Update service unavailable. Starting the installed version.
    call "%~dp0Launch.cmd" %*
    call exit /b %%errorlevel%%
)
