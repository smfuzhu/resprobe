@echo off
@rem RESPROBE-ROLE:uninstall
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem ===========================================================================
rem  ResProbe - uninstall
rem
rem  Usage:
rem    this script            remove tasks + stop guard + remove virtual displays
rem                           (keeps driver package and files; re-installable)
rem    ... /full              additionally stop and remove the usbmmidd driver
rem    ... /purge             /full plus delete OUT and the bundled driver folder
rem
rem  WARNING: once the virtual displays are gone the original problem returns -
rem           powering off the physical monitor will drop the desktop to 1024x768.
rem
rem  NOTE ON ENCODING: the file name of this script is Chinese, so the file
rem  CONTENT must stay pure ASCII. See the install script for the full explanation.
rem ===========================================================================

rem === CONFIG BLOCK (keep in sync with the other .cmd files) =================
set "RP_HOME=%~dp0"
if "%RP_HOME:~-1%"=="\" set "RP_HOME=%RP_HOME:~0,-1%"

set "RP_EXE=%RP_HOME%\resprobe.exe"
set "RP_OUT=%RP_HOME%\OUT"
set "RP_DRV=%RP_HOME%\driver\usbmmidd_v2"

set "RP_REF=1920x1080"
set "RP_TASK_INIT=ResProbe VDisplay Init"
set "RP_TASK_GUARD=ResProbe Guard"
rem === END CONFIG BLOCK =====================================================

fltmc >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Administrator privileges required.
    echo         Right-click this script and choose "Run as administrator".
    exit /b 1
)

set "RP_FULL=0"
set "RP_PURGE=0"
if /i "%~1"=="/full"  set "RP_FULL=1"
if /i "%~1"=="/purge" (set "RP_FULL=1" & set "RP_PURGE=1")
if /i "%~2"=="/purge" set "RP_PURGE=1"

echo ==========================================================
echo   ResProbe uninstall ^(full=%RP_FULL% purge=%RP_PURGE%^)
echo ==========================================================

rem --- 1. remove scheduled tasks ---------------------------------------------
rem  Matched by the ASCII prefix "ResProbe*" so that leftovers with any
rem  encoding or any historical name are cleaned up as well.
echo [1/5] removing scheduled tasks ...
powershell -NoProfile -Command ^
  "Get-ScheduledTask | Where-Object { $_.TaskName -like 'ResProbe*' } | Unregister-ScheduledTask -Confirm:$false -ErrorAction SilentlyContinue" >nul 2>&1
echo        done ^(any task whose name starts with ResProbe^)

rem --- 2. stop the guard process ---------------------------------------------
echo [2/5] stopping resprobe processes ...
taskkill /im resprobe.exe /f >nul 2>&1
if not errorlevel 1 echo        stopped
rem  NOTE: `timeout` fails when this script's output is redirected, use PowerShell.
powershell -NoProfile -Command "Start-Sleep -Seconds 2" >nul 2>&1

rem --- 3. remove virtual displays --------------------------------------------
rem  "enableidd 0" removes one display per call, up to four exist in total.
echo [3/5] removing virtual displays ...
if exist "%RP_DRV%\deviceinstaller64.exe" (
    for /l %%i in (1,1,4) do (
        "%RP_DRV%\deviceinstaller64.exe" enableidd 0 >nul 2>&1
    )
    echo        ran "enableidd 0" x4
    powershell -NoProfile -Command "Start-Sleep -Seconds 4" >nul 2>&1
) else (
    echo        [skip] deviceinstaller64.exe not found in %RP_DRV%
)

rem --- 4. driver package -----------------------------------------------------
if "%RP_FULL%"=="1" (
    echo [4/5] stopping and removing usbmmidd driver ...
    if exist "%RP_DRV%\deviceinstaller64.exe" (
        "%RP_DRV%\deviceinstaller64.exe" stop usbmmidd >nul 2>&1
        "%RP_DRV%\deviceinstaller64.exe" remove usbmmidd >nul 2>&1
    )
    pnputil /enum-drivers 2>nul | findstr /i "usbmmidd" >nul 2>&1
    if not errorlevel 1 (
        for /f "tokens=2 delims=:" %%o in ('pnputil /enum-drivers ^| findstr /i "Published Name"') do (
            echo %%o | findstr /i "oem" >nul && pnputil /delete-driver %%o /uninstall /force >nul 2>&1
        )
    )
    echo        driver removal attempted
) else (
    echo [4/5] keeping usbmmidd driver ^(no /full^)
)

rem --- 5. optional file cleanup ----------------------------------------------
if "%RP_PURGE%"=="1" (
    echo [5/5] cleaning files ...
    if exist "%RP_OUT%" rmdir /s /q "%RP_OUT%" 2>nul
    if exist "%RP_HOME%\driver" rmdir /s /q "%RP_HOME%\driver" 2>nul
    echo        deleted OUT and driver folders
) else (
    echo [5/5] keeping logs and the bundled driver ^(no /purge^)
)

echo ----------------------------------------------------------
echo Uninstall finished.
echo   re-install         : the install script in this folder
echo   soft rollback only : "%RP_EXE%" ensure --mode mirror
echo ==========================================================
endlocal
exit /b 0
