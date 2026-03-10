@echo off
setlocal enabledelayedexpansion

set PRESET=%~1
if "%PRESET%"=="" set PRESET=debug

:: Pre-flight checks (vcvarsall, cmake, ninja, vcpkg)
call "%~dp0..\apex_tools\build-preflight.bat"
if errorlevel 1 exit /b 1

:: ── Build ──────────────────────────────────────────
set BUILD_DIR=build\Windows\%PRESET%

cd /d %~dp0

:: Ensure build dir and compile_commands.json exist for first configure (clangd)
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%BUILD_DIR%\compile_commands.json" type nul > "%BUILD_DIR%\compile_commands.json"

:: Configure
echo [build.bat] Configuring preset: %PRESET%
cmake --preset %PRESET%
if errorlevel 1 exit /b 1

:: Copy compile_commands.json to project root for clangd
copy /Y "%BUILD_DIR%\compile_commands.json" compile_commands.json >nul 2>&1

:: Build
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1

:: Test
ctest --preset %PRESET% --output-on-failure
