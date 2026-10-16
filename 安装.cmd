@echo off
@rem RESPROBE-ROLE:install
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem ===========================================================================
rem  ResProbe - install (IDEMPOTENT, safe to run repeatedly)
rem
rem  The full flow on a brand new machine (driver, device node and virtual
rem  display may all be missing):
rem    1. make sure resprobe.exe exists (build from source when missing)
rem    2. make sure the OUT output directory exists
rem    3. locate the usbmmidd driver package:
rem         a) bundled in .\driver\usbmmidd_v2      <- preferred, works offline
rem         b) legacy copy at C:\Temp\...           <- migrated automatically
rem         c) download from the vendor             <- last resort only
rem    4. create the ROOT enumerated device node if absent
rem       IMPORTANT: `pnputil /add-driver` only puts the package into the
rem       DriverStore, it does NOT create the device node. The vendor tool
rem       `deviceinstaller64.exe install <inf> <hwid>` is required. Without it
rem       the later `enableidd 1` silently does nothing.
rem    5. write the virtual display mode table (plain registry overwrite)
rem    6. activate EXACTLY ONE virtual display - only when `resprobe ensure`
rem       reports 20 (no virtual display output). This is the idempotency core:
rem       enableidd adds one display per call, so it must never be called
rem       unconditionally.
rem    7. apply the display topology
rem    8. print a summary
rem
rem  NOTE ON ENCODING: the file name of this script is Chinese, therefore the
rem  file CONTENT must be pure ASCII. cmd.exe parses batch content using the
rem  OEM code page and re-reads the file by byte offset; non-ASCII characters
rem  desynchronise that offset and make the tail of a comment line execute as a
rem  command. A Chinese file name cannot appear inside ASCII-only content, so
rem  the shared config below is duplicated in each script on purpose.
rem ===========================================================================

rem === CONFIG BLOCK (keep in sync with the other .cmd files) =================
set "RP_HOME=%~dp0"
if "%RP_HOME:~-1%"=="\" set "RP_HOME=%RP_HOME:~0,-1%"

set "RP_EXE=%RP_HOME%\resprobe.exe"
set "RP_OUT=%RP_HOME%\OUT"
set "RP_DRV=%RP_HOME%\driver\usbmmidd_v2"
set "RP_LEGACY_DRV=C:\Temp\usbmmidd_v2\usbmmidd_v2"

set "RP_REF=1920x1080"
set "RP_REF_REG=1920,1080"

set "RP_IDD_URL=https://www.amyuni.com/downloads/usbmmidd_v2.zip"
set "RP_IDD_HWID=usbmmIdd"
set "RP_IDD_DESC=USB Mobile Monitor"
set "RP_MON_KEY=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\usbmmIdd\Parameters\Monitors"

set "RP_TASK_INIT=ResProbe VDisplay Init"
set "RP_TASK_GUARD=ResProbe Guard"

set "RP_NO_VDISPLAY=20"
set "RP_NOT_READY=21"
rem === END CONFIG BLOCK =====================================================

fltmc >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Administrator privileges required.
    echo         Right-click this script and choose "Run as administrator".
    exit /b 1
)

echo ==========================================================
echo   ResProbe install ^(idempotent^)
echo ==========================================================
echo   home      : %RP_HOME%
echo   output    : %RP_OUT%
echo   reference : %RP_REF%
echo   driver pkg: %RP_DRV%
echo ----------------------------------------------------------

rem --- 1. executable ---------------------------------------------------------
if not exist "%RP_EXE%" (
    echo [1/8] resprobe.exe missing, building from source ...
    if not defined MINGW_BIN set "MINGW_BIN=C:\mingw64\bin"
    set "PATH=%MINGW_BIN%;%PATH%"
    pushd "%RP_HOME%"
    mingw32-make
    popd
    if not exist "%RP_EXE%" (
        echo [ERROR] Build failed. Check that MinGW-w64 is installed and MINGW_BIN points to it.
        exit /b 1
    )
) else (
    echo [1/8] resprobe.exe present, skip build.
)

rem --- 2. output directory ---------------------------------------------------
if not exist "%RP_OUT%" mkdir "%RP_OUT%" >nul 2>&1
echo [2/8] output dir ready: %RP_OUT%

rem --- 3. driver package -----------------------------------------------------
set "RP_DRV_READY="
if exist "%RP_DRV%\deviceinstaller64.exe" if exist "%RP_DRV%\usbmmIdd.inf" set "RP_DRV_READY=1"

if defined RP_DRV_READY (
    echo [3/8] driver package present locally, no download needed.
) else (
    if exist "%RP_LEGACY_DRV%\deviceinstaller64.exe" (
        echo [3/8] migrating driver package from legacy location ...
        if not exist "%RP_HOME%\driver" mkdir "%RP_HOME%\driver" >nul 2>&1
        xcopy /e /i /y /q "%RP_LEGACY_DRV%" "%RP_DRV%" >nul 2>&1
    )
    if exist "%RP_DRV%\deviceinstaller64.exe" (
        echo        migrated from %RP_LEGACY_DRV%
    ) else (
        echo [3/8] driver package not bundled, downloading ...
        echo        ^(offline machines: copy the usbmmidd_v2 folder to %RP_DRV% instead^)
        if not exist "%RP_HOME%\driver" mkdir "%RP_HOME%\driver" >nul 2>&1
        powershell -NoProfile -ExecutionPolicy Bypass -Command ^
          "$ErrorActionPreference='Stop';" ^
          "$d='%RP_HOME%\driver'; $z=Join-Path $env:TEMP 'usbmmidd_v2.zip';" ^
          "Invoke-WebRequest -Uri '%RP_IDD_URL%' -OutFile $z -UseBasicParsing;" ^
          "Expand-Archive -Path $z -DestinationPath $d -Force;" ^
          "Remove-Item $z -Force -ErrorAction SilentlyContinue;" ^
          "Write-Host 'downloaded'"
        if not exist "%RP_DRV%\deviceinstaller64.exe" (
            echo [ERROR] Download or extraction failed.
            echo         Expected: %RP_DRV%\deviceinstaller64.exe
            exit /b 1
        )
    )
)

rem --- 4. device node --------------------------------------------------------
pnputil /enum-devices /class Display 2>nul | findstr /i "%RP_IDD_DESC%" >nul 2>&1
if not errorlevel 1 (
    echo [4/8] usbmmidd device node present, skip creation.
) else (
    echo [4/8] creating usbmmidd device node ...
    pushd "%RP_DRV%"
    deviceinstaller64.exe install usbmmIdd.inf %RP_IDD_HWID%
    set "RC_INSTALL=!ERRORLEVEL!"
    popd
    echo        installer returned !RC_INSTALL!
    rem  NOTE: `timeout` fails with "Input redirection is not supported" whenever
    rem  this script's output is redirected, so PowerShell is used to sleep.
    powershell -NoProfile -Command "Start-Sleep -Seconds 5" >nul 2>&1
    pnputil /enum-devices /class Display 2>nul | findstr /i "%RP_IDD_DESC%" >nul 2>&1
    if errorlevel 1 (
        echo [WARN] device node still not visible after creation.
        echo        Run manually:
        echo          cd /d "%RP_DRV%"
        echo          deviceinstaller64.exe install usbmmIdd.inf %RP_IDD_HWID%
    )
)

rem --- 5. mode table ---------------------------------------------------------
echo [5/8] writing virtual display mode table ...
reg add "%RP_MON_KEY%" /f >nul 2>&1
reg add "%RP_MON_KEY%" /ve /t REG_SZ /d "%RP_REF_REG%" /f >nul 2>&1
reg add "%RP_MON_KEY%" /v 0 /t REG_SZ /d "1024,768"  /f >nul 2>&1
reg add "%RP_MON_KEY%" /v 1 /t REG_SZ /d "1360,768"  /f >nul 2>&1
reg add "%RP_MON_KEY%" /v 2 /t REG_SZ /d "1440,900"  /f >nul 2>&1
reg add "%RP_MON_KEY%" /v 3 /t REG_SZ /d "1600,900"  /f >nul 2>&1
reg add "%RP_MON_KEY%" /v 4 /t REG_SZ /d "1600,1200" /f >nul 2>&1
reg add "%RP_MON_KEY%" /v 5 /t REG_SZ /d "1920,1080" /f >nul 2>&1
reg add "%RP_MON_KEY%" /v 6 /t REG_SZ /d "1920,1200" /f >nul 2>&1
reg add "%RP_MON_KEY%" /v 7 /t REG_SZ /d "2560,1440" /f >nul 2>&1
reg add "%RP_MON_KEY%" /v 8 /t REG_SZ /d "3840,2160" /f >nul 2>&1

rem --- 6. activate exactly one virtual display -------------------------------
echo [6/8] checking virtual display state ...
"%RP_EXE%" ensure --reference %RP_REF% >nul 2>&1
set "RC=!ERRORLEVEL!"
if "!RC!"=="%RP_NO_VDISPLAY%" (
    echo        no virtual display found, activating one ...
    "%RP_DRV%\deviceinstaller64.exe" enableidd 1
    powershell -NoProfile -Command "Start-Sleep -Seconds 6" >nul 2>&1
) else (
    if "!RC!"=="0" (
        echo        virtual display already available ^(ensure=0^), not activating again.
    ) else (
        if "!RC!"=="%RP_NOT_READY%" (
            echo        virtual display present, topology not applied yet ^(ensure=!RC!^); step 7 will retry.
        ) else (
            echo        [WARN] ensure returned !RC! ^(expected 0/20/21^), skipping activation to stay idempotent.
        )
    )
)

rem --- 7. apply topology -----------------------------------------------------
echo [7/8] applying display topology ...
"%RP_EXE%" ensure --reference %RP_REF%
set "RC=!ERRORLEVEL!"

rem --- 8. summary ------------------------------------------------------------
echo [8/8] result:
if "!RC!"=="0" (
    echo        primary display is %RP_REF%, ready.
) else (
    if "!RC!"=="%RP_NOT_READY%" (
        echo        [WARN] primary is not %RP_REF%.
        echo               With no monitor attached, run the one-shot startup script.
    ) else (
        echo        [WARN] ensure returned !RC!
        echo               Diagnose with: resprobe.exe ensure --verbose
    )
)

echo ----------------------------------------------------------
echo Install finished. Scripts available in this folder:
for /f "delims=" %%F in ('dir /b /a-d "%~dp0*.cmd" 2^>nul') do echo        %%F
echo   ^(see DEVELOPMENT.md for what each one does^)
echo ==========================================================
endlocal
exit /b 0
