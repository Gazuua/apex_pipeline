@echo off
setlocal

set PRESET=%~1
if "%PRESET%"=="" set PRESET=debug

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set VCPKG_ROOT=C:\Users\JHG\vcpkg
cd /d D:\.workspace\BoostAsioCore

:: Configure only if build directory doesn't exist yet
if not exist "build\%PRESET%\build.ninja" (
    echo [build.bat] Configuring preset: %PRESET%
    cmake --preset %PRESET%
    if errorlevel 1 exit /b 1
) else (
    echo [build.bat] Build directory exists, skipping configure
)

:: Build
cmake --build "build/%PRESET%"
if errorlevel 1 exit /b 1

:: Test
ctest --test-dir "build/%PRESET%" --output-on-failure
