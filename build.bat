@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ==========================================================================
REM  build.bat - build Release and assemble dist\ from the current source tree.
REM  Run it where the source lives (a local Windows dir builds fastest; use
REM  wsl-build.bat to copy an ext4/WSL checkout to a local dir first).
REM
REM  dist\ = the minimum to install in production:
REM     tsw_agent.exe  tsw_plugin.dll  install.cmd  uninstall.cmd  README.md
REM  The picker logo is embedded via the committed src\client_agent\logo_png.h,
REM  so no SVG toolchain is needed here (see tools\update-logo.sh to refresh it).
REM ==========================================================================
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

REM --- locate CMake: VS-bundled copy (via vswhere), else PATH ----------------
set "CMAKE="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%I"
)
if defined VSPATH if exist "!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined CMAKE ( where cmake >nul 2>&1 && set "CMAKE=cmake" )
if not defined CMAKE (
    echo ERROR: CMake not found. Install the "C++ CMake tools" VS component, or add cmake to PATH.
    exit /b 1
)
echo Using CMake: !CMAKE!

"!CMAKE!" -S "%ROOT%" -B "%ROOT%\build"                             || ( echo Configure failed. & exit /b 1 )
"!CMAKE!" --build "%ROOT%\build" --config Release                  || ( echo Build failed.     & exit /b 1 )
"!CMAKE!" --build "%ROOT%\build" --config Release --target package  || ( echo Package failed.   & exit /b 1 )

echo.
echo dist\ assembled at "%ROOT%\dist".
exit /b 0
