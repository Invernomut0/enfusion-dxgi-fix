@echo off
REM Build DXGI Hook DLL for x64 (required for CrossOver)
REM 
REM Since you're on Windows ARM64, you need to cross-compile for x64.
REM 
REM Option 1: Use "x64 on ARM64 Cross Tools Command Prompt for VS 2022"
REM           (Search for it in Start Menu)
REM
REM Option 2: Run this from regular command prompt after setting up environment:
REM           "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsamd64_arm64.bat"
REM
REM Then run this script.

echo Building DXGI Hook DLL for x64...
echo.

REM Check if we're targeting x64
cl 2>&1 | findstr /C:"x64" > nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Not targeting x64!
    echo.
    echo You're on ARM64 Windows but CrossOver needs x64 binaries.
    echo.
    echo Please open "x64_arm64 Cross Tools Command Prompt for VS 2022"
    echo from the Start Menu, then run this script again.
    echo.
    pause
    exit /b 1
)

cl /LD /EHsc /O2 /DUNICODE /D_UNICODE dxgi_hook.cpp /link /DEF:dxgi.def /OUT:dxgi.dll

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ====================================================
    echo SUCCESS! dxgi.dll created for x64.
    echo ====================================================
    echo.
    echo Installation steps:
    echo.
    echo 1. Copy dxgi.dll to your Mac
    echo.
    echo 2. Put it in the Arma Reforger folder:
    echo    ~/Library/Application Support/CrossOver/Bottles/Steam/
    echo    drive_c/Program Files (x86)/Steam/steamapps/common/Arma Reforger/
    echo.
    echo 3. If there's already a dxgi.dll there, rename it to dxgi_original.dll first
    echo.
    echo 4. Launch the game!
    echo.
) else (
    echo.
    echo BUILD FAILED! Check errors above.
)

pause
