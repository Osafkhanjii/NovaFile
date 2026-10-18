@echo off
REM ============================================================
REM  Nova File - Windows build script (C / GCC)
REM  C only. External lib: lib/stb_sprintf.h
REM ============================================================

setlocal

REM GCC ships with Dev-Cpp and is not on this machine's PATH.
set "GCC=C:\Program Files (x86)\Dev-Cpp\MinGW64\bin\gcc.exe"
set "WINDRES=C:\Program Files (x86)\Dev-Cpp\MinGW64\bin\windres.exe"
REM windres runs the C preprocessor itself and cannot cope with the spaces
REM in "C:\Program Files (x86)" - exposing the toolchain dir on PATH lets it
REM find a plain "cpp". setlocal keeps this PATH change to this build only.
set "PATH=C:\Program Files (x86)\Dev-Cpp\MinGW64\bin;%PATH%"

if exist "%GCC%" goto :build
echo ERROR: gcc.exe not found.
echo Edit the GCC variable in compile.bat and point it to your gcc.exe
pause
exit /b 1

:build
if not exist build mkdir build

REM App icon (water-wave circle): embed NovaFile.ico into the exe. The
REM build still succeeds without windres - just with the default icon.
set "RESOBJ="
if exist "%WINDRES%" (
    "%WINDRES%" --preprocessor=cpp -i resource.rc -O coff -o build\resource_res.o
    if errorlevel 1 goto :failed
    set "RESOBJ=build\resource_res.o"
) else (
    echo WARNING: windres.exe not found - building without the app icon.
)

echo Building Nova File (C)...
echo.

"%GCC%" -std=gnu11 -O2 -Wall -Iinclude -Ilib ^
    %RESOBJ% ^
    src\core\memory.c ^
    src\core\indexer.c ^
    src\platform\window_win.c ^
    src\renderer\renderer_gl.c ^
    src\ui\ui_framework.c ^
    src\app\update.c ^
    src\app\main.c ^
    -mwindows ^
    -lole32 -loleaut32 -lshell32 -luser32 -lkernel32 -lgdi32 -lmsimg32 ^
    -lcomdlg32 ^
    -lwinhttp -lbcrypt -lcrypt32 -lwintrust ^
    -Wl,-Map=build\NovaFile.map ^
    -o build\NovaFile.exe

if errorlevel 1 goto :failed

echo.
echo BUILD OK: build\NovaFile.exe
echo.
echo Controls:
echo   Up/Down/PageUp/PageDown  Navigate the file list
echo   Enter                    Open folder / run file
echo   Backspace / Left         Go to the parent folder
echo   F3                       Toggle Details / Thumbnails
echo   Bottom bar slider / - +  Size icons (drag the slider line)
echo   Ctrl+wheel               Size in / out (animated)
echo   Ctrl+L / Ctrl+F          Focus the search box
echo   Ctrl+N / Ctrl+Tab        New tab / switch tabs
echo   Hamburger button         Menu: tab, bookmark, refresh, theme, exit
echo   Breadcrumb click         Jump to that folder segment
echo   Wheel                    Scrolls the list 2x (sidebar too)
echo   Ctrl+H                   Show / hide hidden files
echo   Ctrl+T                   Dark / Light theme
echo   Ctrl+B                   Bookmark current folder
echo   Tab                      Address bar (breadcrumb)
echo   F2                       Rename the selected item inline
echo   Type "This PC" + Enter   All drives view
echo   F5                       Refresh
echo   Ctrl+Q or Esc            Exit (Esc closes the menu first)
echo.
pause
exit /b 0

:failed
echo.
echo BUILD FAILED - check the errors above.
pause
exit /b 1
