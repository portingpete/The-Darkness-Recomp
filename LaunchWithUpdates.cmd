@echo off
setlocal DisableDelayedExpansion
set "DARKRECOMP_INSTALL_ROOT=%~dp0"
rem This entire block is parsed before updating, so replacing this CMD is safe.
(
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $env:DARKRECOMP_INSTALL_ROOT 'tools\update_commits.ps1')))) -InstallRoot $env:DARKRECOMP_INSTALL_ROOT -AcceptUpdate"
    if errorlevel 20 (
        echo The latest commit could not be built or installed. See the message above.
        pause
        rem Restore the failure status after pause, even with closed stdin.
        cmd.exe /d /c exit 20
    ) else (
        if errorlevel 10 echo Update service unavailable. Starting the installed version.
        call "%~dp0Launch.cmd" %*
    )
    call exit /b %%errorlevel%%
)
