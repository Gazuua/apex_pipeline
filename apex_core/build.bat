@echo off
setlocal

set PRESET=%~1
if "%PRESET%"=="" set PRESET=debug

:: VS2022 vcvarsall.bat 동적 탐색 (vswhere), fallback으로 기본 경로 사용
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
) else (
    set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\2022\Community"
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=C:\Users\JHG\vcpkg
cd /d %~dp0

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
