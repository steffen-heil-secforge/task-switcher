@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ==========================================================================
REM  wsl-build.bat - build a WSL/ext4 checkout without building over the WSL
REM  filesystem boundary (MSBuild/CMake choke on that). Copies the tree to a
REM  local Windows dir, builds there, copies dist\ back, and wipes the scratch.
REM
REM  From WSL:      cmd.exe /c "$(wslpath -w wsl-build.bat)"
REM  From Windows:  just run wsl-build.bat from the checkout.
REM ==========================================================================
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "BUILD=%SystemDrive%\TaskSwitcher-Build"

echo  Source : %SRC%
echo  Build  : %BUILD%
echo.

REM --- wipe + copy source to the local build dir ----------------------------
if exist "%BUILD%" (
    rd /s /q "%BUILD%"
    if exist "%BUILD%" ( echo ERROR: could not remove "%BUILD%" ^(close programs using it^). & exit /b 1 )
)
echo Copying source to build folder...
robocopy "%SRC%" "%BUILD%" /E /XD .git build dist /XF *.user *.suo >nul
if %ERRORLEVEL% GEQ 8 ( echo ERROR: robocopy failed ^(exit %ERRORLEVEL%^). & exit /b 1 )
if not exist "%BUILD%\build.bat" ( echo ERROR: copy failed - build.bat missing. & exit /b 1 )

REM --- build in the local dir -----------------------------------------------
call "%BUILD%\build.bat"
if !ERRORLEVEL! NEQ 0 ( echo ERROR: build failed ^(exit !ERRORLEVEL!^). & rd /s /q "%BUILD%" >nul 2>nul & exit /b !ERRORLEVEL! )

REM --- copy dist\ back to the checkout (staged; survives briefly-locked outputs)
set "DIST=%SRC%\dist"
if not exist "%DIST%" mkdir "%DIST%"
for %%F in (tsw_agent.exe tsw_plugin.dll install.cmd uninstall.cmd README.md) do (
    call :copyback "%%F" || ( rd /s /q "%BUILD%" >nul 2>nul & exit /b 1 )
)

rd /s /q "%BUILD%" >nul 2>nul
echo.
echo ==========================================================================
echo  dist: %DIST%
echo ==========================================================================
exit /b 0

REM ---- staged copy of one dist file: src -> stage -> dst.stage -> rename ----
:copyback
set "F=%~1"
set "S=%BUILD%\dist\%F%"
set "STAGE=%BUILD%\.copy-stage.tmp"
set "DSTAGE=%DIST%\.copy-stage.tmp"
if not exist "%S%" ( echo ERROR: missing build output "%S%". & exit /b 1 )
del /F /Q "%STAGE%" >nul 2>nul
copy /Y "%S%" "%STAGE%" >nul || ( echo ERROR: staging "%F%" failed ^(output may be briefly locked^). & exit /b 1 )
if exist "%DIST%\%F%" ( attrib -R "%DIST%\%F%" >nul 2>nul & del /F /Q "%DIST%\%F%" >nul 2>nul )
if exist "%DIST%\%F%" ( echo ERROR: "%DIST%\%F%" is in use. & exit /b 1 )
del /F /Q "%DSTAGE%" >nul 2>nul
copy /Y "%STAGE%" "%DSTAGE%" >nul || ( echo ERROR: copying "%F%" to dist failed. & del /F /Q "%STAGE%" >nul 2>nul & exit /b 1 )
ren "%DSTAGE%" "%F%" >nul 2>nul || ( echo ERROR: renaming "%F%" in dist failed. & exit /b 1 )
del /F /Q "%STAGE%" >nul 2>nul
exit /b 0
