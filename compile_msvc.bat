@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1

echo Compiling resources...
rc /nologo netmon.rc
if %errorlevel% neq 0 goto fail

echo Building netmon.exe...

cl /O2 /W3 /nologo /utf-8 /D_CRT_SECURE_NO_WARNINGS netmon.c netmon.res ^
   /link ws2_32.lib iphlpapi.lib shell32.lib gdi32.lib dwmapi.lib user32.lib advapi32.lib wlanapi.lib ole32.lib winhttp.lib ^
   /SUBSYSTEM:WINDOWS /OUT:netmon.exe

if %errorlevel% == 0 (
    echo.
    echo SUCCESS: netmon.exe built.
    echo  Left-click  tray icon  = show/hide widget
    echo  Right-click tray icon  = Settings / Exit
) else (
    echo BUILD FAILED.
)
endlocal
