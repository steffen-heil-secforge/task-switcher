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
set "PSRC=%ERRORLEVEL%"
set "RC=9"
if "%PSRC%"=="1" set "RC=1"
if not "%PSRC%"=="1" if exist "%STAGE%\result.txt" set /p RC=<"%STAGE%\result.txt"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
if not "%RC%"=="0" (
  if "%RC%"=="1" echo ERROR: elevation was cancelled or blocked - nothing was removed.
  if "%RC%"=="2" echo ERROR: Remote Desktop has the plugin loaded. Close ALL Remote Desktop windows, then run the uninstaller again. Nothing was removed.
  if "%RC%"=="3" echo ERROR: setup files could not be completely removed.
  if "%RC%"=="9" echo ERROR: the elevated step did not run/report - nothing was removed.
  pause & exit /b 1
)
goto :okmsg

:direct
call :removeall
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  if "%RC%"=="2" echo ERROR: Remote Desktop has the plugin loaded. Close ALL Remote Desktop windows, then run the uninstaller again. Nothing was removed.
  if "%RC%"=="3" echo ERROR: setup files could not be completely removed.
  pause & exit /b 1
)

:okmsg
echo.
echo Uninstalled. Restart any open Remote Desktop windows to unload the plugin.
echo.
pause
exit /b 0

:elevated
call :removeall
set "R=%ERRORLEVEL%"
> "%~dp0result.txt" echo %R%
exit /b %R%

REM ---- the actual removal -----------------------------------------------------
:removeall
REM Refuse before changing anything if mstsc still has the DLL loaded; otherwise the final
REM directory removal would fail after the registry and scheduled task had already been deleted.
set "LOCKED="
for /f "delims=" %%L in ('tasklist /m tsw_plugin.dll /fo csv /nh 2^>nul ^| findstr /i ".exe"') do set "LOCKED=1"
if defined LOCKED exit /b 2
schtasks /end /tn "TaskSwitcher" >nul 2>&1
schtasks /delete /tn "TaskSwitcher" /f >nul 2>&1
taskkill /f /im tsw_agent.exe >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Terminal Server Client\Default\AddIns\TaskSwitcher" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v TaskSwitcher /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v TaskSwitcher /f >nul 2>&1
if exist "%DEST%" rmdir /s /q "%DEST%"
if exist "%DEST%" exit /b 3
exit /b 0
goto :eof
