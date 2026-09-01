@echo off
rem Runs the port with everything it prints captured to console.txt next to
rem this file.
rem
rem Use this when icorecomp.log does not appear. The redirect below is done
rem by cmd.exe, not by the program, so it works even if the program cannot
rem open a file of its own. That makes it a test as well as a capture: if
rem console.txt is also missing afterwards, nothing running from this folder
rem can write to it, and the folder is the problem rather than the port.
rem
rem If console.txt does appear, its first lines name the log file the port
rem chose, or every location it tried and the reason each one failed.

setlocal
cd /d "%~dp0"

echo Running icorecomp-runtime.exe
echo Console output is being captured to console.txt
echo.

"%~dp0icorecomp-runtime.exe" %* > "%~dp0console.txt" 2>&1
set RC=%ERRORLEVEL%

echo. >> "%~dp0console.txt"
echo exit code: %RC% >> "%~dp0console.txt"

echo.
echo Finished, exit code %RC%.
if exist "%~dp0console.txt" (
    echo   console.txt written.
) else (
    echo   console.txt was NOT created. This folder is not writable by
    echo   programs started from it, which is also why icorecomp.log is
    echo   missing. Copy the whole folder somewhere local and retry.
)
if exist "%~dp0icorecomp.log" (
    echo   icorecomp.log written.
) else (
    echo   icorecomp.log was NOT created. Read console.txt for the reason.
)
echo.
pause
