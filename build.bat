@echo off
setlocal enabledelayedexpansion

echo =========================================================
echo   FSR-NG-Scaling: Building Release (MSVC x64 C++20)
echo =========================================================

set "VS_PATH=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VS_PATH%" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_PATH=%%i"
    )
)

if exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [Build] Setting up MSVC environment from: %VS_PATH%
    call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
) else (
    echo [Build] Warning: vcvars64.bat not found automatically. Proceeding with existing PATH.
)

if not exist build (
    mkdir build
)

echo [Build] Configuring CMake project...
cmake -B build -A x64
if %errorlevel% neq 0 (
    echo [Build] CMake configuration failed!
    exit /b %errorlevel%
)

echo [Build] Compiling Release executable...
cmake --build build --config Release --parallel
if %errorlevel% neq 0 (
    echo [Build] Compilation failed!
    exit /b %errorlevel%
)

echo =========================================================
echo   Build completed successfully
echo   Executable located at: FSR-NG-Scaling.exe
echo =========================================================
exit /b 0
