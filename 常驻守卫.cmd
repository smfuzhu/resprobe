@echo off
@rem RESPROBE-ROLE:guard
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem ===========================================================================
rem  ResProbe - resident guard  (the DEFAULT autostart mode)
rem
rem  Behaviour:
rem    1. idempotently make sure a virtual display exists
rem       (usbmmidd virtual displays do NOT survive a reboot)
rem    2. apply the display topology once
rem    3. stay resident watching display events. When a physical link drops
rem       (EDID unreadable) it promotes the virtual display to the reference
rem       resolution and detaches the dead output.
rem
rem  Usage:  this script          foreground (use this form as a scheduled task)
rem          this script /bg      start detached, return immediately
rem          this script /json    also write OUT\guard.json (machine readable)
rem
rem  Stop:   taskkill /im resprobe.exe /f
rem  Log:    %RP_OUT%\guard.log   (UTF-8 - resprobe writes UTF-8 console output;
rem                                read it with: Get-Content ... -Encoding UTF8)
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
set "RP_NO_VDISPLAY=20"
rem === END CONFIG BLOCK =====================================================

fltmc >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Administrator privileges required.
    echo         Right-click this script and choose "Run as administrator".
    exit /b 1
)

set "RP_LOG=%RP_OUT%\guard.log"
if not exist "%RP_OUT%" mkdir "%RP_OUT%" >nul 2>&1

set "RP_OUT_ARG="
if /i "%~1"=="/json" set "RP_OUT_ARG=--out guard.json"

echo ==========================================================>>"%RP_LOG%"
echo [%date% %time%] resident guard starting>>"%RP_LOG%"
echo ==========================================================>>"%RP_LOG%"

rem --- 1. make sure a virtual display exists (idempotent) --------------------
"%RP_EXE%" ensure --reference %RP_REF% >nul 2>&1
set "RC=!ERRORLEVEL!"

if "!RC!"=="%RP_NO_VDISPLAY%" (
    if exist "%RP_DRV%\deviceinstaller64.exe" (
        echo [%date% %time%] no virtual display, activating one>>"%RP_LOG%"
        "%RP_DRV%\deviceinstaller64.exe" enableidd 1 >>"%RP_LOG%" 2>&1
        rem  NOTE: `timeout` fails when output is redirected, use PowerShell to sleep.
        powershell -NoProfile -Command "Start-Sleep -Seconds 6" >nul 2>&1
    ) else (
        echo [%date% %time%] [WARN] deviceinstaller64.exe missing, skip activation>>"%RP_LOG%"
    )
)

rem --- 2. apply topology once ------------------------------------------------
"%RP_EXE%" ensure --reference %RP_REF% >>"%RP_LOG%" 2>&1

rem --- 3. stay resident ------------------------------------------------------
echo [%date% %time%] entering resident watch ^(interval=10s, guard=on^)>>"%RP_LOG%"

if /i "%~1"=="/bg" (
    start "ResProbe Guard" /min cmd /c ^
      ""%RP_EXE%" watch --duration 0 --interval 10 --guard --reference %RP_REF% %RP_OUT_ARG% >>"%RP_LOG%" 2>&1"
    echo Resident guard started in background. Log: %RP_LOG%
    endlocal
    exit /b 0
)

"%RP_EXE%" watch --duration 0 --interval 10 --guard --reference %RP_REF% %RP_OUT_ARG% >>"%RP_LOG%" 2>&1

echo [%date% %time%] resident guard exited, code=%ERRORLEVEL%>>"%RP_LOG%"
endlocal
exit /b 0
