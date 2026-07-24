@echo off
setlocal EnableExtensions
REM ============================================================================
REM  Multi-Computer Task Switcher - uninstaller (double-click me).
REM  Reverses install.cmd on both client and servers: stops the agent, removes the
REM  Remote Desktop plugin registration and the auto-start, and deletes the files.
REM ============================================================================

set "DEST=%ProgramFiles%\TaskSwitcher"

if /I "%~1"=="__elevated" goto :elevated

REM Already elevated (elevated prompt, or UAC disabled)? Remove directly.
net session >nul 2>&1
if %errorlevel% EQU 0 goto :direct

REM Not elevated: copy this script to a LOCAL drive, then elevate the local copy.
set "STAGE=%TEMP%\tsw-uninstall"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
copy /y "%~f0" "%STAGE%\uninstall.cmd" >nul || (echo ERROR: cannot stage script. & pause & exit /b 1)
echo Uninstalling ^(a UAC prompt will appear^)...
powershell -NoProfile -Command "try { Start-Process -FilePath '%STAGE%\uninstall.cmd' -ArgumentList '__elevated' -Verb RunAs -Wait } catch { exit 1 }"
set "RC=%ERRORLEVEL%"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
if not "%RC%"=="0" (
  echo ERROR: elevation was cancelled or blocked - nothing was removed.
  pause & exit /b 1
)
goto :okmsg

:direct
call :removeall

:okmsg
echo.
echo Uninstalled. Restart any open Remote Desktop windows to unload the plugin.
echo.
pause
exit /b 0

:elevated
call :removeall
exit /b 0

REM ---- the actual removal -----------------------------------------------------
:removeall
schtasks /end /tn "TaskSwitcher" >nul 2>&1
schtasks /delete /tn "TaskSwitcher" /f >nul 2>&1
taskkill /f /im tsw_agent.exe >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Terminal Server Client\Default\AddIns\TaskSwitcher" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v TaskSwitcher /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v TaskSwitcher /f >nul 2>&1
if exist "%DEST%" rmdir /s /q "%DEST%"
goto :eof
