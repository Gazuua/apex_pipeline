@echo off
REM apex-agent build script — Windows double-click friendly
REM Usage: build.bat [test|clean]
REM Default (no args): build + install

cd /d "%~dp0"

for /f "delims=" %%v in ('git describe --tags --always --dirty 2^>nul') do set VERSION=%%v
if "%VERSION%"=="" set VERSION=dev

set LDFLAGS=-s -w -X github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/version.Version=%VERSION%
set INSTALL_DIR=%LOCALAPPDATA%\apex-agent

if "%1"=="test" (
    echo === Running tests (unit + e2e) ===
    go test ./... -race -cover -v -count=1 -timeout 120s
    goto :end
)

if "%1"=="clean" (
    echo === Cleaning ===
    del /q apex-agent.exe 2>nul
    goto :end
)

echo === Building + Installing apex-agent %VERSION% ===
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
go build -trimpath -ldflags="%LDFLAGS%" -o "%INSTALL_DIR%\apex-agent.exe" ./cmd/apex-agent
if %ERRORLEVEL%==0 (
    echo Installed: %INSTALL_DIR%\apex-agent.exe [%VERSION%]
    "%INSTALL_DIR%\apex-agent.exe" daemon stop >nul 2>&1
    "%INSTALL_DIR%\apex-agent.exe" daemon start >nul 2>&1 && echo daemon restarted
) else (
    echo Build FAILED
)

:end
if "%1"=="" pause
