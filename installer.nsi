; ═══════════════════════════════════════════════════════════════════════════
;  NetMon Installer - NSIS Script
;  Creates a single-EXE installer with Next/Back wizard, firewall rule,
;  optional "Start with Windows", and a clean uninstaller.
; ═══════════════════════════════════════════════════════════════════════════

!include "MUI2.nsh"

; ── General ────────────────────────────────────────────────────────────────
Name "NetMon"
OutFile "NetMon_Setup.exe"
InstallDir "$PROGRAMFILES64\NetMon"
InstallDirRegKey HKLM "Software\NetMon" "InstallDir"
RequestExecutionLevel admin          ; needed for firewall + Program Files
Unicode True

; ── Version Info (embedded in the installer EXE) ──────────────────────────
VIProductVersion "1.0.0.0"
VIFileVersion    "1.0.0.0"
VIAddVersionKey "ProductName"     "NetMon"
VIAddVersionKey "CompanyName"     "Ibrahim Mohammed"
VIAddVersionKey "FileDescription" "NetMon Installer"
VIAddVersionKey "FileVersion"     "1.0.0.0"
VIAddVersionKey "LegalCopyright"  "(c) 2026 Ibrahim Mohammed"

; ── UI Settings ───────────────────────────────────────────────────────────
!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

; ── Pages ─────────────────────────────────────────────────────────────────
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\netmon.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch NetMon now"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_FINISH

; ── Uninstaller Pages ─────────────────────────────────────────────────────
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; ── Language ──────────────────────────────────────────────────────────────
!insertmacro MUI_LANGUAGE "English"

; ══════════════════════════════════════════════════════════════════════════
;  INSTALL SECTION
; ══════════════════════════════════════════════════════════════════════════
Section "Install"
    SetOutPath "$INSTDIR"

    ; Kill running instance if any
    nsExec::ExecToLog 'taskkill /F /IM netmon.exe'

    ; Copy files
    File "netmon.exe"

    ; Save install directory to registry
    WriteRegStr HKLM "Software\NetMon" "InstallDir" "$INSTDIR"

    ; ── Firewall exception (inbound + outbound) ──────────────────────────
    nsExec::ExecToLog 'netsh advfirewall firewall add rule name="NetMon" dir=in action=allow program="$INSTDIR\netmon.exe" enable=yes profile=any'
    nsExec::ExecToLog 'netsh advfirewall firewall add rule name="NetMon" dir=out action=allow program="$INSTDIR\netmon.exe" enable=yes profile=any'

    ; ── Start Menu shortcut (makes it searchable in Start) ────────────────
    CreateDirectory "$SMPROGRAMS\NetMon"
    CreateShortcut  "$SMPROGRAMS\NetMon\NetMon.lnk" "$INSTDIR\netmon.exe" "" "$INSTDIR\netmon.exe" 0
    CreateShortcut  "$SMPROGRAMS\NetMon\Uninstall NetMon.lnk" "$INSTDIR\uninstall.exe"

    ; ── Start with Windows (current user) ────────────────────────────────
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "NetMon" '"$INSTDIR\netmon.exe"'

    ; ── Add/Remove Programs entry ────────────────────────────────────────
    WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NetMon" \
                       "DisplayName"     "NetMon - Network Monitor"
    WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NetMon" \
                       "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NetMon" \
                       "InstallLocation" "$INSTDIR"
    WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NetMon" \
                       "Publisher"       "Ibrahim Mohammed"
    WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NetMon" \
                       "DisplayVersion"  "1.0.0.0"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NetMon" \
                       "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NetMon" \
                       "NoRepair" 1

    ; Create uninstaller
    WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

; ══════════════════════════════════════════════════════════════════════════
;  UNINSTALL SECTION
; ══════════════════════════════════════════════════════════════════════════
Section "Uninstall"
    ; Kill running instance
    nsExec::ExecToLog 'taskkill /F /IM netmon.exe'

    ; Remove firewall rules
    nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="NetMon"'

    ; Remove startup entry
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "NetMon"

    ; Remove Start Menu shortcuts
    Delete "$SMPROGRAMS\NetMon\NetMon.lnk"
    Delete "$SMPROGRAMS\NetMon\Uninstall NetMon.lnk"
    RMDir  "$SMPROGRAMS\NetMon"

    ; Remove files
    Delete "$INSTDIR\netmon.exe"
    Delete "$INSTDIR\uninstall.exe"
    RMDir  "$INSTDIR"

    ; Remove registry keys
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NetMon"
    DeleteRegKey HKLM "Software\NetMon"
SectionEnd
