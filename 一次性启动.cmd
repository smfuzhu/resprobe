@echo off
@rem RESPROBE-ROLE:once
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem ===========================================================================
rem  ResProbe - one-shot startup
rem
rem  For machines with NO monitor attached, or where the monitor is powered off
rem  and will not be plugged back in.
rem
rem  Effect: make sure a virtual display exists, then promote it to be the
rem          single primary display at the reference resolution. The desktop and
rem          any remote session then stay at 1920x1080 without a physical sink.
rem
rem  Usage:  this script            apply (default)
rem          this script /status    report only, change nothing
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

if /i "%~1"=="/status" (
    "%RP_EXE%" snapshot --reference %RP_REF%
    "%RP_EXE%" ensure --reference %RP_REF% >nul 2>&1
    echo ensure exit code = %ERRORLEVEL%  ^(0=ready 20=no virtual display 21=not ready^)
    exit /b 0
)

echo ==========================================================
echo   ResProbe one-shot startup ^(headless / monitor not coming back^)
echo ==========================================================

rem --- 1. make sure a virtual display exists (idempotent) --------------------
"%RP_EXE%" ensure --reference %RP_REF% >nul 2>&1
set "RC=!ERRORLEVEL!"
if "!RC!"=="%RP_NO_VDISPLAY%" (
    echo [1/3] no virtual display found, activating one ...
    if not exist "%RP_DRV%\deviceinstaller64.exe" (
        echo [ERROR] Missing %RP_DRV%\deviceinstaller64.exe
        echo         Run the install script first ^(or copy the usbmmidd_v2 folder there^).
        exit /b 1
    )
    "%RP_DRV%\deviceinstaller64.exe" enableidd 1
    rem  NOTE: `timeout` fails when output is redirected, use PowerShell to sleep.
    powershell -NoProfile -Command "Start-Sleep -Seconds 6" >nul 2>&1
) else (
    echo [1/3] virtual display available ^(ensure=!RC!^).
)

rem --- 2. promote it to the primary display ----------------------------------
echo [2/3] promoting virtual display to %RP_REF% and detaching dead outputs ...
"%RP_EXE%" ensure --mode promote --reference %RP_REF%
set "RC=!ERRORLEVEL!"

rem --- 3. verify -------------------------------------------------------------
echo [3/3] current state:
"%RP_EXE%" snapshot --reference %RP_REF%

echo ----------------------------------------------------------
if "!RC!"=="0" (
    echo Ready: primary is %RP_REF%; remote sessions no longer depend on the monitor.
) else (
    echo [WARN] ensure returned !RC!; primary may not be %RP_REF%.
    echo        Diagnose with: "%RP_EXE%" paths --verbose
)
echo ==========================================================
endlocal
exit /b 0
