@echo off
REM Build hopline. Usage: build.bat [win-debug^|win-release]  (default: win-debug)
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=win-debug

call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."

if not exist "build\%CONFIG%\CMakeCache.txt" ( cmake --preset %CONFIG% || exit /b 1 )

REM A running app locks its own exe, so the relink would fail - close it first.
taskkill /IM hopline.exe /F >nul 2>&1

cmake --build build/%CONFIG%
