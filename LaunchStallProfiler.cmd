@echo off
rem Play normally with lightweight runtime stall profiling enabled for this run.
rem Extra arguments are forwarded exactly as with Launch.cmd play.
setlocal DisableDelayedExpansion
cd /d "%~dp0"
set "DARKRECOMP_STALL_PROFILE=1"
echo Runtime stall profiling is enabled for this run.
echo Reproduce the hitch, then close the game normally.
echo [STALL] and [WAIT] diagnostics are saved in build_native\run\desktop-*\runtime.log.
call "%~dp0Launch.cmd" play %*
exit /b %ERRORLEVEL%
