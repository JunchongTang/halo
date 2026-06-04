@echo off
REM build.bat — 编译 halo.cpp 为 halo.exe，输出到仓库的 dist\windows\
REM
REM 前提：在 "x64 Native Tools Command Prompt for VS"（或已运行 vcvars64.bat
REM 的命令行）里执行，确保 cl.exe 可用。无需任何第三方库，
REM 依赖的 Direct2D / DirectComposition / D3D11 都是系统自带。
REM
REM 用法: build.bat

setlocal
set SCRIPT_DIR=%~dp0
set DIST=%SCRIPT_DIR%..\..\dist\windows
if not exist "%DIST%" mkdir "%DIST%"

echo 编译中...
cl /nologo /std:c++17 /O2 /EHsc /DUNICODE /D_UNICODE ^
   "%SCRIPT_DIR%halo.cpp" ^
   /Fo:"%TEMP%\halo_build\\" ^
   /Fe:"%DIST%\halo.exe" ^
   /link /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo 编译失败
    exit /b 1
)

echo 完成 -^> %DIST%\halo.exe
echo.
echo 测试运行（彩色流光）:
echo   "%DIST%\halo.exe" --duration 3 --style rainbow
echo 测试运行（单色呼吸）:
echo   "%DIST%\halo.exe" --duration 3 --style pulse --color "#FF3B30"
endlocal
