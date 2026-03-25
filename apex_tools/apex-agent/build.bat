@echo off
REM apex-agent build script — Windows double-click friendly
REM Usage: build.bat [test|clean]
REM Default (no args): build + install (build → daemon stop → rename → copy → daemon start)
REM
REM Windows allows renaming a running exe. This avoids killing other workspaces'
REM apex-agent CLI processes. The old binary continues running in memory and the
REM .old file is cleaned up on next install.

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

REM Step 2: Stop daemon (creates maintenance lock → suppresses auto-restart)
"%INSTALL_DIR%\apex-agent.exe" daemon stop >nul 2>&1

REM Step 3: Rename-then-replace (running exe can be renamed on Windows)
del /q "%INSTALL_DIR%\apex-agent.exe.old" 2>nul
ren "%INSTALL_DIR%\apex-agent.exe" apex-agent.exe.old 2>nul
copy /y apex-agent.exe "%INSTALL_DIR%\apex-agent.exe" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Install FAILED: could not copy to %INSTALL_DIR%\apex-agent.exe
    REM Restore old binary if rename succeeded
    if not exist "%INSTALL_DIR%\apex-agent.exe" (
        ren "%INSTALL_DIR%\apex-agent.exe.old" apex-agent.exe 2>nul
    )
    del /q apex-agent.exe 2>nul
    goto :end
)
del /q apex-agent.exe 2>nul

REM Step 4: Start daemon with new binary (clears maintenance lock)
"%INSTALL_DIR%\apex-agent.exe" daemon start >nul 2>&1 && echo daemon restarted
echo Installed: %INSTALL_DIR%\apex-agent.exe [%VERSION%]

:end
if "%1"=="" pause
