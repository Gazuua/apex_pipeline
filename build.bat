@echo off
setlocal

set PRESET=%~1
if "%PRESET%"=="" set PRESET=debug

:: Pre-flight checks (vcvarsall, cmake, ninja, vcpkg)
call "%~dp0apex_tools\build-preflight.bat"
if errorlevel 1 exit /b 1

cd /d %~dp0

:: Ensure build dir and compile_commands.json exist for first configure (clangd symlink)
if not exist "build\Windows\%PRESET%" mkdir "build\Windows\%PRESET%"
if not exist "build\Windows\%PRESET%\compile_commands.json" type nul > "build\Windows\%PRESET%\compile_commands.json"

:: Always configure (cached runs are fast, ensures compile_commands.json stays fresh)
echo [build.bat] Configuring preset: %PRESET%
cmake --preset %PRESET%
if errorlevel 1 exit /b 1

:: Copy compile_commands.json to project root for clangd (after configure generates it)
copy /Y "build\Windows\%PRESET%\compile_commands.json" compile_commands.json >nul 2>&1

:: Build
cmake --build "build/Windows/%PRESET%"
if errorlevel 1 exit /b 1

:: Test (exclude E2E tests — they require docker infrastructure)
ctest --preset %PRESET% --output-on-failure -LE e2e
