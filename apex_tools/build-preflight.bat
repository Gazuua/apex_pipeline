:: ── Build pre-flight checks (called by build.bat scripts) ──────────
:: Usage: call "%~dp0apex_tools\build-preflight.bat"
:: Note: Do NOT use setlocal — vcvarsall environment must persist in caller.

:: VS2022 vcvarsall.bat (must run first — cmake/ninja may be in VS tools PATH)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
) else (
    set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\2022\Community"
)
if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo Error: vcvarsall.bat not found at %VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (echo Error: vcvarsall.bat failed & exit /b 1)

:: cmake check (after vcvarsall — cmake may be in VS tools PATH)
where cmake >nul 2>&1 || (echo Error: cmake not found ^(required: cmake ^>= 3.25^) & exit /b 1)
for /f "tokens=3" %%v in ('cmake --version 2^>^&1 ^| findstr /c:"cmake version"') do set CMAKE_VER=%%v
for /f "tokens=1,2 delims=." %%a in ("%CMAKE_VER%") do (
    if %%a LSS 3 (echo Error: cmake %CMAKE_VER% found, but ^>= 3.25 required & exit /b 1)
    if %%a EQU 3 if %%b LSS 25 (echo Error: cmake %CMAKE_VER% found, but ^>= 3.25 required & exit /b 1)
)

:: ninja check
where ninja >nul 2>&1 || (echo Error: ninja not found ^(required: ninja ^>= 1.11^) & exit /b 1)

:: VCPKG_ROOT check
if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=%USERPROFILE%\vcpkg
if not exist "%VCPKG_ROOT%" (
    echo Error: VCPKG_ROOT path does not exist: %VCPKG_ROOT%
    exit /b 1
)
