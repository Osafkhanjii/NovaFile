@echo off
REM ============================================================
REM  Build the NovaFile Explorer installer (Inno Setup 6)
REM
REM  Reads the version from include\version.h (the single source
REM  of truth) and compiles installer\novafile.iss to
REM  dist\NovaFile-Setup-<version>.exe
REM
REM  Inno Setup search order:
REM    1. %ISCC% (override)
REM    2. %LOCALAPPDATA%\Programs\InnoSetup6 (per-user install)
REM    3. C:\Program Files (x86)\Inno Setup 6 (default install)
REM ============================================================
setlocal

set "ROOT=%~dp0.."
set "VH=%ROOT%\include\version.h"

if not exist "%VH%" (
    echo ERROR: include\version.h not found.
    exit /b 1
)

REM Parse NOVA_VERSION_MAJOR/MINOR/PATCH + the NOVA_VERSION string.
for /f "tokens=3" %%A in ('findstr /R /C:"#define NOVA_VERSION_MAJOR" "%VH%"') do set "VMAJ=%%A"
for /f "tokens=3" %%A in ('findstr /R /C:"#define NOVA_VERSION_MINOR" "%VH%"') do set "VMIN=%%A"
for /f "tokens=3" %%A in ('findstr /R /C:"#define NOVA_VERSION_PATCH" "%VH%"') do set "VPAT=%%A"
for /f "tokens=3" %%A in ('findstr /R /C:"#define NOVA_VERSION " "%VH%"') do set "VERRAW=%%A"
set "VER=%VERRAW:"=%"

if "%VMAJ%"=="" (
    echo ERROR: could not read the version from include\version.h
    exit /b 1
)
if "%VER%"=="" (
    echo ERROR: could not read NOVA_VERSION from include\version.h
    exit /b 1
)
set "VERNUM=%VMAJ%.%VMIN%.%VPAT%.0"
echo Building NovaFile Explorer installer %VER% (%VERNUM%)...

if not exist "%ROOT%\build\NovaFile.exe" (
    echo ERROR: build\NovaFile.exe missing - run compile.bat first.
    exit /b 1
)

if not "%ISCC%"=="" goto :have_iscc
if exist "%LOCALAPPDATA%\Programs\InnoSetup6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\InnoSetup6\ISCC.exe"
if not "%ISCC%"=="" goto :have_iscc
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not "%ISCC%"=="" goto :have_iscc
if exist "C:\Program Files\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
if not "%ISCC%"=="" goto :have_iscc

echo ERROR: Inno Setup 6 ^(ISCC.exe^) not found.
echo Install it from https://jrsoftware.org/isdl.php or set %%ISCC%%.
exit /b 1

:have_iscc
echo Using ISCC: %ISCC%
"%ISCC%" /DAppVer=%VER% /DAppVerNum=%VERNUM% "%ROOT%\installer\novafile.iss"
if errorlevel 1 (
    echo.
    echo INSTALLER BUILD FAILED
    exit /b 1
)

echo.
echo INSTALLER OK: dist\NovaFile-Setup-%VER%.exe
exit /b 0
