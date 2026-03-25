@echo off
REM apex-agent build script — Windows double-click friendly
REM Usage: build.bat [test|clean]
REM Default (no args): build + install (daemon stop → build → copy → daemon start)

cd /d "%~dp0"

for /f "delims=" %%v in ('git describe --tags --always --dirty 2^>nul') do set VERSION=%%v
if "%VERSION%"=="" set VERSION=dev

set LDFLAGS=-s -w -X github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/version.Version=%VERSION%
set INSTALL_DIR=%LOCALAPPDATA%\apex-agent

if "%1"=="test" (
    echo === Running tests (unit + e2e) ===
    go test ./... -cover -v -count=1 -timeout 120s
    goto :end
)

if "%1"=="clean" (
    echo === Cleaning ===
    del /q apex-agent.exe 2>nul
    goto :end
)

echo === Building + Installing apex-agent %VERSION% ===
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"

REM Step 1: Build to local temp file (no file lock issues)
go build -trimpath -ldflags="%LDFLAGS%" -o apex-agent.exe ./cmd/apex-agent
if %ERRORLEVEL% NEQ 0 (
    echo Build FAILED
    goto :end
)

REM Step 2: Stop daemon + kill all apex-agent processes (release file locks)
"%INSTALL_DIR%\apex-agent.exe" daemon stop >nul 2>&1
REM Wait briefly for graceful shutdown, then force-kill stragglers
timeout /t 1 /nobreak >nul 2>&1
taskkill /F /IM apex-agent.exe >nul 2>&1

REM Step 3: Copy to install dir (retry once after brief wait if still locked)
copy /y apex-agent.exe "%INSTALL_DIR%\apex-agent.exe" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Retrying copy after 2s...
    timeout /t 2 /nobreak >nul 2>&1
    copy /y apex-agent.exe "%INSTALL_DIR%\apex-agent.exe" >nul 2>&1
    if %ERRORLEVEL% NEQ 0 (
        echo Install FAILED: %INSTALL_DIR%\apex-agent.exe is locked.
        echo Kill all apex-agent processes and retry.
        del /q apex-agent.exe 2>nul
        goto :end
    )
)
del /q apex-agent.exe 2>nul

REM Step 4: Start daemon with new binary
"%INSTALL_DIR%\apex-agent.exe" daemon start >nul 2>&1 && echo daemon restarted
echo Installed: %INSTALL_DIR%\apex-agent.exe [%VERSION%]

:end
if "%1"=="" pause
