@echo off
REM build.bat - compile halo.cpp into dist\windows\halo.exe
REM
REM Run from a "x64 / ARM64 Native Tools Command Prompt for VS"
REM (or any shell where vcvars has been applied, so cl.exe is on PATH).
REM No third-party deps: Direct2D / DirectComposition / D3D11 ship with the Windows SDK.
REM
REM Comments are ASCII-only on purpose: a UTF-8 batch misparses under code page 936.

setlocal
set SCRIPT_DIR=%~dp0
set DIST=%SCRIPT_DIR%..\..\dist\windows
set OBJ=%SCRIPT_DIR%obj

where cl >nul 2>nul
if errorlevel 1 (
    echo [halo] cl.exe not found on PATH.
    echo        Open "ARM64 Native Tools Command Prompt for VS" ^(or x64^), or install
    echo        Visual Studio Build Tools with the "Desktop development with C++" workload.
    exit /b 1
)

if not exist "%DIST%" mkdir "%DIST%"
if not exist "%OBJ%"  mkdir "%OBJ%"

echo [halo] building...
cl /nologo /utf-8 /std:c++17 /O2 /EHsc /DUNICODE /D_UNICODE ^
   "%SCRIPT_DIR%halo.cpp" ^
   /Fo:"%OBJ%\\" ^
   /Fe:"%DIST%\halo.exe" ^
   /link /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo [halo] build failed.
    exit /b 1
)

rmdir /s /q "%OBJ%" 2>nul

echo [halo] done -^> %DIST%\halo.exe
echo.
echo Test:
echo   "%DIST%\halo.exe" --duration 3 --style rainbow
echo   "%DIST%\halo.exe" --duration 3 --style pulse --color "#FF3B30"
endlocal
