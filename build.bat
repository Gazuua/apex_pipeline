@echo off
setlocal

set PRESET=%~1
if "%PRESET%"=="" set PRESET=debug

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set VCPKG_ROOT=C:\Users\JHG\vcpkg
cd /d D:\.workspace

:: Ensure build dir and compile_commands.json exist for first configure (clangd symlink)
if not exist "build\%PRESET%" mkdir "build\%PRESET%"
if not exist "build\%PRESET%\compile_commands.json" type nul > "build\%PRESET%\compile_commands.json"

:: Always configure (cached runs are fast, ensures compile_commands.json stays fresh)
echo [build.bat] Configuring preset: %PRESET%
cmake --preset %PRESET%
if errorlevel 1 exit /b 1

:: Copy compile_commands.json to project root for clangd (after configure generates it)
copy /Y "build\%PRESET%\compile_commands.json" compile_commands.json >nul 2>&1

:: Build
cmake --build "build/%PRESET%"
if errorlevel 1 exit /b 1

:: Test
ctest --test-dir "build/%PRESET%" --output-on-failure
