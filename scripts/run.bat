@echo off
REM Build (debug) then launch the app. Any args pass through to hopline.exe
REM (e.g. run.bat "C:\path\to\clip.mp4" auto-opens that file).
call "%~dp0build.bat" || exit /b 1
cd /d "%~dp0.."
start "" "build\win-debug\hopline.exe" %*
