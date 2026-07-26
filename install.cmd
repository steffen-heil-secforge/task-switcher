@echo off
setlocal EnableExtensions
REM ============================================================================
REM  Multi-Computer Task Switcher - installer (double-click me).
REM  Works on BOTH the client PC and RDP servers: copies tsw_agent.exe and
REM  tsw_plugin.dll into Program Files, registers the Remote Desktop plugin, sets
REM  a machine-wide auto-start, and launches the agent now. The agent picks its
REM  own role at runtime, so the same install is correct everywhere.
REM ============================================================================

set "SRC=%~dp0"
set "DEST=%ProgramFiles%\TaskSwitcher"

if /I "%~1"=="__elevated" goto :elevated

call :resolve
if not defined AGENT (
  echo ERROR: tsw_agent.exe not found next to this script or under build\.
  echo Put tsw_agent.exe and tsw_plugin.dll next to install.cmd, or run build.bat first.
  pause & exit /b 1
)
if not defined PLUGIN (
  echo ERROR: tsw_plugin.dll not found next to this script or under build\.
  pause & exit /b 1
)

REM Already running elevated (elevated prompt / UAC disabled)? Install directly.
net session >nul 2>&1
if %errorlevel% EQU 0 goto :direct

REM Not elevated: stage binaries + this script on a LOCAL drive (an elevated process can't read
REM mapped / WSL drives like Z:), then elevate the local copy. The elevated step reports its
REM outcome via result.txt - we do NOT read the child's exit code, which a non-elevated parent
REM generally cannot access across the elevation boundary.
set "STAGE=%TEMP%\tsw-setup"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
copy /y "%AGENT%"  "%STAGE%\tsw_agent.exe"  >nul || (echo ERROR: cannot stage agent.  & pause & exit /b 1)
copy /y "%PLUGIN%" "%STAGE%\tsw_plugin.dll" >nul || (echo ERROR: cannot stage plugin. & pause & exit /b 1)
copy /y "%~f0"     "%STAGE%\install.cmd"    >nul || (echo ERROR: cannot stage script. & pause & exit /b 1)

echo Installing to "%DEST%" ^(a UAC prompt will appear^)...
powershell -NoProfile -Command "try { Start-Process -FilePath '%STAGE%\install.cmd' -ArgumentList '__elevated' -Verb RunAs -Wait } catch { exit 1 }"
set "PSRC=%ERRORLEVEL%"
set "RC=9"
if "%PSRC%"=="1" set "RC=1"
if not "%PSRC%"=="1" if exist "%STAGE%\result.txt" set /p RC=<"%STAGE%\result.txt"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
goto :done

:direct
echo Installing to "%DEST%"...
set "A=%AGENT%"
set "P=%PLUGIN%"
call :doinstall
set "RC=%ERRORLEVEL%"

:done
if not "%RC%"=="0" goto :failed
if not exist "%DEST%\tsw_agent.exe" goto :failed
REM (the elevated step already launched it via the scheduled task, at highest privileges)
echo.
echo Installed and running.
echo   - Press Ctrl+^^ to open the task switcher.
echo   - On the client: restart any open Remote Desktop windows so the plugin loads.
echo.
pause
exit /b 0

:failed
echo.
if "%RC%"=="1" echo ERROR: elevation was cancelled or blocked - nothing was installed.
if "%RC%"=="6" echo ERROR: Remote Desktop is open and has the plugin loaded, so it cannot be updated. Close ALL Remote Desktop windows, then run the installer again. Nothing was changed.
if "%RC%"=="9" echo ERROR: the elevated step did not run/report - nothing was installed.
if not "%RC%"=="1" if not "%RC%"=="6" if not "%RC%"=="9" echo ERROR: machine setup failed at step %RC% ^(2=copy agent 3=copy plugin 4=reg plugin 5=autostart task^).
pause
exit /b 1

REM ---- elevated re-launch: this copy lives IN the staging dir, so derive it from %~dp0
REM (avoids passing a quoted path through cmd->powershell, which mangles it). ------------
:elevated
set "STG=%~dp0"
set "A=%STG%tsw_agent.exe"
set "P=%STG%tsw_plugin.dll"
call :doinstall
set "R=%ERRORLEVEL%"
> "%STG%result.txt" echo %R%
exit /b %R%

REM ---- the actual machine setup (uses %A% %P% %DEST%) --------------------------
:doinstall
REM Pre-flight (change nothing yet): every component must be updatable or we abort cleanly.
REM An open Remote Desktop loads tsw_plugin.dll in-process; if any process currently has it
REM loaded we can't overwrite it, so refuse up front rather than do a partial install.
set "LOCKED="
for /f "delims=" %%L in ('tasklist /m tsw_plugin.dll /fo csv /nh 2^>nul ^| findstr /i ".exe"') do set "LOCKED=1"
if defined LOCKED exit /b 6
taskkill /f /im tsw_agent.exe >nul 2>&1
>nul ping -n 3 127.0.0.1
if not exist "%DEST%" mkdir "%DEST%"
call :copyretry "%A%" "%DEST%\tsw_agent.exe"  || exit /b 2
call :copyretry "%P%" "%DEST%\tsw_plugin.dll" || exit /b 3
reg add "HKLM\SOFTWARE\Microsoft\Terminal Server Client\Default\AddIns\TaskSwitcher" /v Name /t REG_SZ /d "%DEST%\tsw_plugin.dll" /f >nul || exit /b 4
REM Auto-start the agent ELEVATED at logon via a scheduled task. A medium-integrity agent's
REM low-level keyboard hook is bypassed while an elevated window (Task Manager, admin apps) is
REM focused; running the agent at highest privileges lets the hotkey work over those too.
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v TaskSwitcher /f >nul 2>&1
powershell -NoProfile -Command "try { $a=New-ScheduledTaskAction -Execute '%DEST%\tsw_agent.exe'; $t=New-ScheduledTaskTrigger -AtLogOn; $p=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Highest; $s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero); Register-ScheduledTask -TaskName 'TaskSwitcher' -Action $a -Trigger $t -Principal $p -Settings $s -Force | Out-Null } catch { exit 1 }" || exit /b 5
REM launch it now (elevated, in the current session)
schtasks /run /tn "TaskSwitcher" >nul 2>&1
exit /b 0

REM copy %1 -> %2, retrying while the target is still locked (taskkill is asynchronous, and a
REM running agent / open Remote Desktop can briefly hold the file).
:copyretry
set /a _n=0
:cr
copy /y %1 %2 >nul 2>&1 && goto :eof
set /a _n+=1
if %_n% GEQ 8 exit /b 1
>nul ping -n 2 127.0.0.1
goto :cr

REM ---- locate the binaries (next to the script, else a build tree) ------------
:resolve
set "AGENT="
for %%P in (
  "%SRC%tsw_agent.exe"
  "%SRC%build\src\agent\Release\tsw_agent.exe"
  "%SRC%build\src\agent\Debug\tsw_agent.exe"
) do if not defined AGENT if exist "%%~P" set "AGENT=%%~P"
set "PLUGIN="
for %%P in (
  "%SRC%tsw_plugin.dll"
  "%SRC%build\src\client_plugin\Release\tsw_plugin.dll"
  "%SRC%build\src\client_plugin\Debug\tsw_plugin.dll"
) do if not defined PLUGIN if exist "%%~P" set "PLUGIN=%%~P"
goto :eof
