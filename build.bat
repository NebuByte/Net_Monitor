@echo off
setlocal

echo.
echo  NetMon - Build Script
echo  =====================
echo.

REM ── Try MinGW / MSYS2 gcc first ────────────────────────────────────────
where gcc >nul 2>&1
if %errorlevel% == 0 (
    echo  Compiler : gcc (MinGW)
    echo  Building ...
    gcc -O2 -mwindows -o netmon.exe netmon.c ^
        -lws2_32 -liphlpapi -lshell32 -lgdi32 -ldwmapi -luser32 -ladvapi32
    if %errorlevel% == 0 (
        echo  Done     : netmon.exe
        goto success
    ) else (
        echo  Build FAILED.
        goto fail
    )
)

REM ── Try MSVC cl (must be run from VS Developer Command Prompt) ──────────
where cl >nul 2>&1
if %errorlevel% == 0 (
    echo  Compiler : MSVC (cl)
    echo  Building ...
    cl /O2 /W3 /nologo netmon.c ^
       /link ws2_32.lib iphlpapi.lib shell32.lib gdi32.lib dwmapi.lib user32.lib ^
       /SUBSYSTEM:WINDOWS /OUT:netmon.exe
    if %errorlevel% == 0 (
        echo  Done     : netmon.exe
        goto success
    ) else (
        echo  Build FAILED.
        goto fail
    )
)

echo  ERROR: No compiler found.
echo.
echo  Options:
echo    A) Install MSYS2 from https://www.msys2.org/ then run:
echo         pacman -S mingw-w64-ucrt-x86_64-gcc
echo       Add  C:\msys64\ucrt64\bin  to your PATH, then re-run build.bat
echo.
echo    B) Open a  "Developer Command Prompt for VS"  and re-run build.bat
echo.
exit /b 1

:success
echo.
echo  Run netmon.exe  - it will appear in your system tray.
echo  Left-click  the tray icon to show/hide the widget.
echo  Right-click the tray icon for Settings and Exit.
echo.
exit /b 0

:fail
exit /b 1
