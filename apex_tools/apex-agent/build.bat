@echo off
REM apex-agent build script — Windows double-click friendly
REM Usage: build.bat [test|clean|install]

cd /d "%~dp0"

for /f "delims=" %%v in ('git describe --tags --always --dirty 2^>nul') do set VERSION=%%v
if "%VERSION%"=="" set VERSION=dev

set GOFLAGS=-trimpath -ldflags="-s -w -X main.Version=%VERSION%"

if "%1"=="test" (
    echo === Running tests ===
    go test ./... -race -cover -v
    goto :end
)

if "%1"=="clean" (
    echo === Cleaning ===
    del /q apex-agent.exe 2>nul
    goto :end
)

if "%1"=="install" (
    echo === Installing to %LOCALAPPDATA%\apex-agent ===
    if not exist "%LOCALAPPDATA%\apex-agent" mkdir "%LOCALAPPDATA%\apex-agent"
    go build %GOFLAGS% -o "%LOCALAPPDATA%\apex-agent\apex-agent.exe" ./cmd/apex-agent
    echo Installed: %LOCALAPPDATA%\apex-agent\apex-agent.exe [%VERSION%]
    goto :end
)

echo === Building apex-agent %VERSION% ===
go build %GOFLAGS% -o apex-agent.exe ./cmd/apex-agent
if %ERRORLEVEL%==0 (
    echo Build OK: apex-agent.exe [%VERSION%]
) else (
    echo Build FAILED
)

:end
if "%1"=="" pause
