@echo off
rem Runs the port with the frame-time profiler and the VU1 state capture on.
rem Both write next to this file: icorecomp.log holds the profile report,
rem vu1cap.bin holds the microprogram states the geometry gate replays.
rem
rem Play until it chugs, then close the window normally so the log is
rem flushed. Do not kill it from Task Manager.

setlocal
cd /d "%~dp0"

rem Frame-time breakdown, one ranked block every 180 fields.
set ICORECOMP_PROFILE=180

rem Real VU1 entry states for the differential gate, 8 per program entry.
set ICORECOMP_VU1_CAPTURE=%~dp0vu1cap.bin

echo Running with profiling and VU1 capture enabled.
echo Play until it chugs, then close the window normally.
echo.

"%~dp0icorecomp-runtime.exe" %*
set RC=%ERRORLEVEL%

echo.
echo Finished, exit code %RC%.
if exist "%~dp0icorecomp.log" (echo   icorecomp.log written.) else (echo   icorecomp.log MISSING.)
if exist "%~dp0vu1cap.bin" (echo   vu1cap.bin written.) else (echo   vu1cap.bin MISSING.)
echo.
pause
