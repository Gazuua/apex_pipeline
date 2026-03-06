@echo off
setlocal

set PRESET=%~1
if "%PRESET%"=="" set PRESET=debug

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set VCPKG_ROOT=C:\Users\JHG\vcpkg
cd /d D:\.workspace\BoostAsioCore

:: Always configure (cached runs are fast, ensures compile_commands.json stays fresh)
echo [build.bat] Configuring preset: %PRESET%
cmake --preset %PRESET%
if errorlevel 1 exit /b 1

:: Build
cmake --build "build/%PRESET%"
if errorlevel 1 exit /b 1

:: Test
ctest --test-dir "build/%PRESET%" --output-on-failure
