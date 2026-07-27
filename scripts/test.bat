@echo off
REM Build and run the Catch2 test suite (debug). Extra args pass through to the runner,
REM e.g. test.bat "[commands]"  or  test.bat --list-tests
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."

if not exist "build\win-debug\CMakeCache.txt" ( cmake --preset win-debug || exit /b 1 )

REM Only the test target - no need to relink the app (or close a running instance).
cmake --build build/win-debug --target hopline_tests || exit /b 1
build\win-debug\hopline_tests.exe %*
