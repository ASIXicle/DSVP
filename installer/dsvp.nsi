; ═══════════════════════════════════════════════════════════════════
; DSVP — Dead Simple Video Player — NSIS Installer
; ═══════════════════════════════════════════════════════════════════
;
; Prerequisites:
;   1. Run package.ps1 first to produce DSVP-portable/
;   2. Install NSIS:  pacman -S mingw-w64-x86_64-nsis
;   3. Build:         makensis installer/dsvp.nsi
;
; Output: DSVP-0.2.0-beta-setup.exe in repo root
;
; ─── Configuration ──────────────────────────────────────────────

!define PRODUCT_NAME    "DSVP"
!define PRODUCT_VERSION "0.2.0-beta"
!define PRODUCT_PUBLISHER "Holden"
!define PRODUCT_WEB     "https://github.com/ASIXicle/DSVP"
!define PRODUCT_EXE     "dsvp.exe"
!define PORTABLE_DIR    "DSVP-portable"

; Installer display name (appears in welcome page, title bar, etc.)
Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"

; Output installer filename
OutFile "..\DSVP-${PRODUCT_VERSION}-setup.exe"

; Default installation directory
InstallDir "$PROGRAMFILES\${PRODUCT_NAME}"

; Request admin privileges for Program Files install
RequestExecutionLevel admin

; Modern UI
!include "MUI2.nsh"
!include "FileFunc.nsh"

; ─── UI Settings ────────────────────────────────────────────────

!define MUI_ABORTWARNING
!define MUI_ICON "..\src\dsvp.ico"
!define MUI_UNICON "..\src\dsvp.ico"

; ─── Pages ──────────────────────────────────────────────────────

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; Uninstaller pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; Language
!insertmacro MUI_LANGUAGE "English"

; ─── Installer Sections ────────────────────────────────────────

Section "DSVP (required)" SecCore
    SectionIn RO  ; read-only — always installed

    SetOutPath "$INSTDIR"

    ; Copy everything from DSVP-portable/ (embedded at compile time)
    File /r "..\${PORTABLE_DIR}\*.*"

    ; Write uninstaller
    WriteUninstaller "$INSTDIR\uninstall.exe"

    ; Start Menu shortcuts
    CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
    CreateShortCut  "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" \
                    "$INSTDIR\${PRODUCT_EXE}" "" "$INSTDIR\${PRODUCT_EXE}" 0
    CreateShortCut  "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall.lnk" \
                    "$INSTDIR\uninstall.exe" "" "$INSTDIR\uninstall.exe" 0

    ; Add/Remove Programs registry entry
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                     "DisplayName" "${PRODUCT_NAME} ${PRODUCT_VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                     "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                     "DisplayIcon" "$INSTDIR\${PRODUCT_EXE}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                     "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                     "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                     "URLInfoAbout" "${PRODUCT_WEB}"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                       "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                       "NoRepair" 1

    ; Calculate installed size for Add/Remove Programs
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                       "EstimatedSize" $0
SectionEnd

Section "File Associations" SecAssoc
    ; Register DSVP as handler for common video formats.
    ; Each extension gets an "Open with DSVP" verb — this does NOT
    ; steal the default association from the user's current player.

    ; Register application capabilities
    WriteRegStr HKLM "Software\${PRODUCT_NAME}\Capabilities" \
                     "ApplicationDescription" "Dead Simple Video Player — reference-quality playback"
    WriteRegStr HKLM "Software\${PRODUCT_NAME}\Capabilities" \
                     "ApplicationName" "${PRODUCT_NAME}"

    ; Register the open command
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\shell\open\command" "" \
                     "$\"$INSTDIR\${PRODUCT_EXE}$\" $\"%1$\""

    ; Register supported file types (Open With menu integration)
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".mkv"  ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".mp4"  ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".avi"  ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".mov"  ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".webm" ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".wmv"  ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".m4v"  ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".ts"   ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".mpg"  ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".mpeg" ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".flv"  ""
    WriteRegStr HKCR "Applications\${PRODUCT_EXE}\SupportedTypes" ".vob"  ""

    ; Register in RegisteredApplications so Windows knows about DSVP
    WriteRegStr HKLM "Software\RegisteredApplications" "${PRODUCT_NAME}" \
                     "Software\${PRODUCT_NAME}\Capabilities"

    ; Notify shell of association changes
    System::Call 'shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
SectionEnd

; ─── Section Descriptions ──────────────────────────────────────

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecCore}  "Install DSVP video player (required)."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecAssoc}  "Add DSVP to the 'Open with' menu for video files. Does not change your default player."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ─── Uninstaller ───────────────────────────────────────────────

Section "Uninstall"
    ; Remove files — delete the entire install directory
    RMDir /r "$INSTDIR"

    ; Remove Start Menu shortcuts
    RMDir /r "$SMPROGRAMS\${PRODUCT_NAME}"

    ; Remove registry keys
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
    DeleteRegKey HKLM "Software\${PRODUCT_NAME}"
    DeleteRegKey HKCR "Applications\${PRODUCT_EXE}"
    DeleteRegValue HKLM "Software\RegisteredApplications" "${PRODUCT_NAME}"

    ; Notify shell
    System::Call 'shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
SectionEnd
