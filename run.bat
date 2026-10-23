@echo off
setlocal
cd /d "%~dp0"
taskkill /f /im claude-terminal.exe >nul 2>&1
if not exist "Binaries\claude-terminal.exe" (
  echo [error] Binaries\claude-terminal.exe not found. Run build.bat first.
  exit /b 1
)
start "" "Binaries\claude-terminal.exe"
endlocal
