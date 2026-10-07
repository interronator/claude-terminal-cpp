@echo off
setlocal
cd /d "%~dp0"
title Claude Terminal build

rem Usage: build.bat [clean] [run]
set CLEAN=0
set RUN=0
for %%A in (%*) do (
  if /I "%%A"=="clean" set CLEAN=1
  if /I "%%A"=="run" set RUN=1
)

echo === Claude Terminal build ===
echo Folder: %CD%
echo.

where cmake >nul 2>&1
if errorlevel 1 goto :nocmake
where git >nul 2>&1
if errorlevel 1 goto :nogit

if "%CLEAN%"=="1" if exist build rmdir /s /q build

echo [1/3] Configuring...
cmake -S . -B build
if errorlevel 1 goto :cfgfail

echo.
echo [2/3] Building (Release)...
cmake --build build --config Release --parallel
if errorlevel 1 goto :buildfail

echo.
echo [3/3] Copying executable...
set EXE=build\claude-terminal.exe
if exist "build\Release\claude-terminal.exe" set EXE=build\Release\claude-terminal.exe
if not exist "%EXE%" goto :noexe
if not exist dist mkdir dist
copy /y "%EXE%" "dist\claude-terminal.exe" >nul
echo.
echo SUCCESS: %CD%\dist\claude-terminal.exe
if "%RUN%"=="1" start "" "dist\claude-terminal.exe"
goto :done

:nocmake
echo [error] CMake was not found on PATH. Install it from https://cmake.org/download/
echo         (choose "Add CMake to the system PATH"), then reopen this window.
goto :done
:nogit
echo [error] Git was not found on PATH. It is needed to download dependencies.
echo         Install from https://git-scm.com/download/win
goto :done
:cfgfail
echo [error] CMake configure failed. Make sure Visual Studio 2022 (or Build Tools)
echo         is installed with the "Desktop development with C++" workload.
goto :done
:buildfail
echo [error] Build failed. See the messages above.
goto :done
:noexe
echo [error] Build finished but claude-terminal.exe was not found.
goto :done

:done
echo.
pause
endlocal
