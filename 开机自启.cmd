@echo off
@rem RESPROBE-ROLE:autostart
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem ===========================================================================
rem  ResProbe - autostart management. Default mode = resident guard.
rem
rem  Usage:
rem    this script                  register (default: guard)
rem    this script /mode guard      register resident guard        <- default
rem    this script /mode once       register one-shot setup only
rem    this script /status          show current registration state
rem    this script /remove          unregister everything
rem
rem  Two logon-triggered, highest-privilege tasks are registered:
rem    "ResProbe VDisplay Init"  -> one-shot startup, 20s delay
rem                                 (activate the IDD + apply topology)
rem    "ResProbe Guard"          -> resident guard,   60s delay
rem
rem  The order matters: usbmmidd virtual displays do not survive a reboot, so the
rem  init task must re-issue `enableidd 1` before the guard can be of any use.
rem  Highest privileges are required because changing display topology needs it.
rem
rem  Task names are deliberately ASCII. Chinese task names would have to be
rem  embedded in this file, which must stay pure ASCII.
rem
rem  NOTE ON ENCODING: the file name of this script is Chinese, so the file
rem  CONTENT must stay pure ASCII. See the install script for the full explanation.
rem ===========================================================================

rem === CONFIG BLOCK (keep in sync with the other .cmd files) =================
set "RP_HOME=%~dp0"
if "%RP_HOME:~-1%"=="\" set "RP_HOME=%RP_HOME:~0,-1%"

set "RP_TASK_INIT=ResProbe VDisplay Init"
set "RP_TASK_GUARD=ResProbe Guard"
rem === END CONFIG BLOCK =====================================================

fltmc >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Administrator privileges required.
    echo         Right-click this script and choose "Run as administrator".
    exit /b 1
)

rem  Locating the two sibling scripts:
rem  their file names are Chinese and therefore cannot be written inside this
rem  ASCII-only file. Instead each of them carries an ASCII marker comment on
rem  its own line - the text RESPROBE-ROLE: followed by once or guard - and this
rem  script discovers their paths by scanning *.cmd for those markers.
rem  The match is anchored at the start of the line (/b) and the script skips
rem  itself, otherwise this very comment would match and the script would pick
rem  its own path.
rem  Remember to add the marker when creating a new role script.

set "RP_ONCE="
set "RP_GUARD="
for /f "delims=" %%F in ('dir /b /a-d "%~dp0*.cmd" 2^>nul') do (
    set "RP_CAND=%~dp0%%F"
    if /i not "!RP_CAND!"=="%~f0" (
        findstr /b /c:"@rem RESPROBE-ROLE:once" "!RP_CAND!" >nul 2>&1
        if not errorlevel 1 set "RP_ONCE=!RP_CAND!"
        findstr /b /c:"@rem RESPROBE-ROLE:guard" "!RP_CAND!" >nul 2>&1
        if not errorlevel 1 set "RP_GUARD=!RP_CAND!"
    )
)

if not defined RP_ONCE (
    echo [ERROR] could not locate the one-shot startup script in %RP_HOME%
    exit /b 1
)
if not defined RP_GUARD (
    echo [ERROR] could not locate the resident guard script in %RP_HOME%
    exit /b 1
)
if /i "%~1"=="/status" goto :STATUS
if /i "%~1"=="/remove" goto :REMOVE

set "RP_MODE=guard"
if /i "%~1"=="/mode" if not "%~2"=="" set "RP_MODE=%~2"

if /i not "%RP_MODE%"=="guard" if /i not "%RP_MODE%"=="once" (
    echo [ERROR] /mode accepts only: guard or once
    exit /b 1
)

echo ==========================================================
echo   ResProbe autostart registration ^(mode=%RP_MODE%^)
echo ==========================================================
echo   init  script : %RP_ONCE%
echo   guard script : %RP_GUARD%

rem --- clean slate so re-running is idempotent -------------------------------
powershell -NoProfile -Command ^
  "Get-ScheduledTask | Where-Object { $_.TaskName -like 'ResProbe*' } | Unregister-ScheduledTask -Confirm:$false -ErrorAction SilentlyContinue" >nul 2>&1

rem --- task 1: logon init ----------------------------------------------------
schtasks /create /tn "%RP_TASK_INIT%" /tr "\"%RP_ONCE%\"" ^
  /sc onlogon /rl highest /delay 0000:20 /f
if errorlevel 1 (
    echo [ERROR] failed to create task: %RP_TASK_INIT%
    exit /b 1
)
echo   registered: %RP_TASK_INIT%

rem --- task 2: resident guard (default) --------------------------------------
if /i "%RP_MODE%"=="guard" (
    schtasks /create /tn "%RP_TASK_GUARD%" /tr "\"%RP_GUARD%\"" ^
      /sc onlogon /rl highest /delay 0001:00 /f
    if errorlevel 1 (
        echo [ERROR] failed to create task: %RP_TASK_GUARD%
        exit /b 1
    )
    echo   registered: %RP_TASK_GUARD%   ^(default^)
) else (
    echo   resident guard NOT registered ^(/mode once^)
)

echo ----------------------------------------------------------
echo Done.  status: this script /status
echo        remove: this script /remove
echo ==========================================================
endlocal
exit /b 0

:STATUS
echo === scheduled tasks ===
powershell -NoProfile -Command ^
  "Get-ScheduledTask | Where-Object { $_.TaskName -like 'ResProbe*' } | ForEach-Object { $_.TaskName + '  [' + $_.State + ']  ->  ' + $_.Actions[0].Execute }"
echo.
echo === guard process ===
tasklist /fi "imagename eq resprobe.exe" 2>nul | findstr /i resprobe
if errorlevel 1 echo   (not running)
endlocal
exit /b 0

:REMOVE
echo Removing autostart registration ...
powershell -NoProfile -Command ^
  "Get-ScheduledTask | Where-Object { $_.TaskName -like 'ResProbe*' } | Unregister-ScheduledTask -Confirm:$false -ErrorAction SilentlyContinue"
echo Done.
endlocal
exit /b 0
