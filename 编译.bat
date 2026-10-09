@echo off
rem  EnableDelayedExpansion 是必需的：下面 if 块里的 %ERRORLEVEL% 会在**块解析时**
rem  就展开，读到的是运行 make 之前的旧值，导致构建失败的退出码被吞掉。
rem  用 !ERRORLEVEL! 才是"执行到那一行时"的值。
setlocal EnableExtensions EnableDelayedExpansion

rem ===========================================================================
rem  ResProbe - build entry point (MinGW-w64)
rem
rem  Usage:  this script           incremental build
rem          this script clean     clean
rem
rem  The real build rules live in Makefile (make discovers that name
rem  automatically, which is why it is kept in English rather than renamed).
rem
rem  MinGW installed elsewhere?  set MINGW_BIN first, e.g.
rem      set MINGW_BIN=D:\mingw64\bin
rem
rem  NOTE ON ENCODING: the file name of this script is Chinese, so the file
rem  CONTENT must stay pure ASCII. See the install script for details.
rem ===========================================================================

if not defined MINGW_BIN set "MINGW_BIN=C:\mingw64\bin"
set "PATH=%MINGW_BIN%;%PATH%"

where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++ not found. Install MinGW-w64 and set MINGW_BIN.
    echo         Current search path: %MINGW_BIN%
    exit /b 1
)

where mingw32-make >nul 2>nul
if errorlevel 1 (
    echo [ERROR] mingw32-make not found.
    exit /b 1
)

pushd "%~dp0"

if /i "%~1"=="clean" (
    mingw32-make clean
    set "RC=!ERRORLEVEL!"
) else (
    mingw32-make %*
    set "RC=!ERRORLEVEL!"
)

popd
endlocal & exit /b %RC%
