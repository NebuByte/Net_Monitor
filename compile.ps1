Set-Location "D:\PersonalProjects\Network Monitor"

# Set up MSVC environment
$vsdev = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"

# Capture environment after running VsDevCmd
$envBlock = cmd /c "`"$vsdev`" -arch=x64 -host_arch=x64 && set" 2>&1
foreach ($line in $envBlock) {
    if ($line -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

Write-Host "Building netmon.exe..." -ForegroundColor Cyan

# Compile resource file (version info + manifest)
$rcProc = Start-Process -FilePath "rc.exe" `
    -ArgumentList @("/nologo", "netmon.rc") `
    -WorkingDirectory "D:\PersonalProjects\Network Monitor" `
    -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput "rc_stdout.txt" `
    -RedirectStandardError  "rc_stderr.txt"

Get-Content "rc_stdout.txt" -ErrorAction SilentlyContinue
Get-Content "rc_stderr.txt" -ErrorAction SilentlyContinue
Remove-Item "rc_stdout.txt","rc_stderr.txt" -ErrorAction SilentlyContinue

if ($rcProc.ExitCode -ne 0) {
    Write-Host "`nRESOURCE COMPILE FAILED" -ForegroundColor Red
    exit $rcProc.ExitCode
}

$proc = Start-Process -FilePath "cl.exe" `
    -ArgumentList @(
        "/O2", "/W3", "/nologo",
        "/utf-8",
        "/D_CRT_SECURE_NO_WARNINGS",
        "netmon.c", "netmon.res",
        "/link",
        "ws2_32.lib", "iphlpapi.lib", "shell32.lib", "gdi32.lib", "dwmapi.lib", "user32.lib", "advapi32.lib", "wlanapi.lib", "ole32.lib", "winhttp.lib",
        "/SUBSYSTEM:WINDOWS",
        "/OUT:netmon.exe"
    ) `
    -WorkingDirectory "D:\PersonalProjects\Network Monitor" `
    -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput "cl_stdout.txt" `
    -RedirectStandardError  "cl_stderr.txt"

Get-Content "cl_stdout.txt" -ErrorAction SilentlyContinue
Get-Content "cl_stderr.txt" -ErrorAction SilentlyContinue
Remove-Item "cl_stdout.txt","cl_stderr.txt" -ErrorAction SilentlyContinue

if ($proc.ExitCode -eq 0) {
    Write-Host "`nSUCCESS: netmon.exe built!" -ForegroundColor Green
} else {
    Write-Host "`nBUILD FAILED (exit $($proc.ExitCode))" -ForegroundColor Red
    exit $proc.ExitCode
}
