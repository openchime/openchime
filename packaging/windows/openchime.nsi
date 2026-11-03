; NSIS script for the Windows client's installer (ARCH-80/81).
;
; The download channel. It sits alongside the MSIX under msix/, which is the
; Store channel; the two ship the same two binaries and differ only in how
; Windows installs them. Both exist because neither covers the other's case:
; the Store cannot serve a direct download link, and an MSIX cannot be handed
; to someone who does not want a Store account.
;
; Built on Linux by makensis (packaging/build-windows.sh), so no Windows
; machine is in the build:
;   makensis -NOCD -DVERSION=7 -DBINDIR=build -DOUTDIR=dist/windows-x64 \
;            -DPLUGINDIR=third_party/nsis-plugins -DSRCDIR=. packaging/windows/openchime.nsi
;
; PER-USER, deliberately. RequestExecutionLevel user installs under
; %LOCALAPPDATA%\Programs and asks for no UAC elevation, which matters for a
; chat client people install on a work machine they do not administer. The
; cost is that a machine-wide deployment is not possible from this installer;
; that case is the MSIX.

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user
ManifestDPIAware true

!ifndef VERSION
  !define VERSION 0
!endif
!ifndef BINDIR
  !error "-DBINDIR=<directory holding openchime.exe and openchime-tui.exe>"
!endif
!ifndef OUTDIR
  !define OUTDIR .
!endif
!ifndef SRCDIR
  !define SRCDIR .
!endif
!ifdef PLUGINDIR
  !addplugindir /x86-unicode "${PLUGINDIR}/x86-unicode"
!endif

!include MUI2.nsh
!include x64.nsh
!include LogicLib.nsh
!include StrFunc.nsh
${StrStr}
${UnStrStr}
${UnStrRep}

; The release number is a bare integer, so it lands in the third field of the
; four-part version Windows expects: 1.0.<release>.0 increases monotonically,
; which is the only property Windows requires. This is the installer's own
; version; the files inside carry the candidate build's number the same way.
!define FILEVERSION "1.0.${VERSION}.0"
; The uninstall entry. One key, ours; the Inno Setup installer this replaces
; used {8C4E1F2A-6D3B-4A57-9E80-2F1B7C5D9A43}_is1, and an install of that
; kind is uninstalled first (the main section), so an upgrade leaves one
; entry.
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenChime"
!define INNO_UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\{8C4E1F2A-6D3B-4A57-9E80-2F1B7C5D9A43}_is1"
; AppUserModelID is not decoration. Windows resolves a toast's identity through
; the Start-menu shortcut, so an unpackaged install with no AUMID there cannot
; raise a Notification Center toast at all -- it falls back to the tray balloon
; and nobody is told why. It must match OC_AUMID in the client exactly.
!define AUMID "OpenChime.OpenChime"

Name "OpenChime"
OutFile "${OUTDIR}/openchime-${VERSION}-windows-x64-setup.exe"
InstallDir "$LOCALAPPDATA\Programs\OpenChime"
InstallDirRegKey HKCU "${UNINST}" "InstallLocation"
BrandingText "OpenChime ${VERSION}"

VIProductVersion "${FILEVERSION}"
VIFileVersion "${FILEVERSION}"
VIAddVersionKey "ProductName" "OpenChime"
VIAddVersionKey "CompanyName" "OpenChime"
VIAddVersionKey "LegalCopyright" "Copyright (c) OpenChime LLC"
VIAddVersionKey "FileDescription" "OpenChime Setup"
VIAddVersionKey "FileVersion" "${FILEVERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"

!define MUI_ICON "${SRCDIR}/client/gui/platform/win32/res/openchime.ico"
!define MUI_UNICON "${SRCDIR}/client/gui/platform/win32/res/openchime.ico"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_NODESC
!define MUI_FINISHPAGE_RUN "$INSTDIR\openchime.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch OpenChime"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ; The binaries are x86-64; ARM64 machines run them under emulation.
  ${IfNot} ${RunningX64}
  ${AndIfNot} ${IsNativeARM64}
    MessageBox MB_ICONSTOP "OpenChime needs 64-bit Windows."
    Abort
  ${EndIf}
FunctionEnd

; Offers to close a running copy instead of failing the file replacement; the
; finish page starts it again. Without this an upgrade over a running client
; fails on the locked files.
Function CloseRunning
  nsExec::ExecToStack 'cmd /c tasklist /FI "IMAGENAME eq openchime.exe" /NH'
  Pop $0
  Pop $1
  ${StrStr} $2 $1 "openchime.exe"
  ${If} $2 != ""
    MessageBox MB_OKCANCEL|MB_ICONINFORMATION "OpenChime is running. Close it to continue?" IDOK close
    Abort
    close:
    nsExec::Exec 'taskkill /IM openchime.exe'
    Sleep 1000
  ${EndIf}
FunctionEnd

Section "OpenChime" SecMain
  SectionIn RO
  Call CloseRunning
  ; An install from the Inno Setup installer this replaces: removed first,
  ; quietly, so one OpenChime is installed and listed.
  ReadRegStr $0 HKCU "${INNO_UNINST}" "QuietUninstallString"
  ${If} $0 != ""
    ExecWait $0
  ${EndIf}
  SetOutPath "$INSTDIR"
  ; Both binaries are statically linked by the mingw-w64 build, so there is no
  ; redistributable to chain and nothing else to place here.
  File "${BINDIR}/openchime.exe"
  File "${BINDIR}/openchime-tui.exe"
  File "${SRCDIR}/README.md"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateDirectory "$SMPROGRAMS\OpenChime"
  CreateShortCut "$SMPROGRAMS\OpenChime\OpenChime.lnk" "$INSTDIR\openchime.exe"
  WinShell::SetLnkAUMI "$SMPROGRAMS\OpenChime\OpenChime.lnk" "${AUMID}"
  CreateShortCut "$SMPROGRAMS\OpenChime\Uninstall OpenChime.lnk" "$INSTDIR\uninstall.exe"

  ; openchime:// -- how a notification's click gets back to the app. The client
  ; also writes these on every start (a portable copy or a dev build has no
  ; installer), but a fresh install should work before the first launch rather
  ; than after it.
  WriteRegStr HKCU "Software\Classes\openchime" "" "URL:OpenChime"
  WriteRegStr HKCU "Software\Classes\openchime" "URL Protocol" ""
  WriteRegStr HKCU "Software\Classes\openchime\shell\open\command" "" '"$INSTDIR\openchime.exe" "%1"'

  WriteRegStr HKCU "${UNINST}" "DisplayName" "OpenChime"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "Publisher" "OpenChime"
  WriteRegStr HKCU "${UNINST}" "URLInfoAbout" "https://openchime.io"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\openchime.exe"
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
SectionEnd

Section "Create a desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\OpenChime.lnk" "$INSTDIR\openchime.exe"
SectionEnd

Section /o "Start OpenChime when I sign in" SecStartup
  CreateShortCut "$SMSTARTUP\OpenChime.lnk" "$INSTDIR\openchime.exe"
SectionEnd

; The TUI is a console program and is useless without a shell that can find
; it, so PATH is offered rather than assumed. HKCU only -- a per-user install
; has no business writing the machine PATH -- and only when the directory is
; not there already, so a repair or an upgrade does not add a second copy.
Section /o "Add the terminal client (openchime-tui) to PATH" SecPath
  ReadRegStr $0 HKCU "Environment" "Path"
  ${StrStr} $1 ";$0;" ";$INSTDIR;"
  ${If} $1 == ""
    ${If} $0 == ""
      WriteRegExpandStr HKCU "Environment" "Path" "$INSTDIR"
    ${Else}
      WriteRegExpandStr HKCU "Environment" "Path" "$0;$INSTDIR"
    ${EndIf}
    WriteRegDWORD HKCU "${UNINST}" "AddedToPath" 1
    SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${EndIf}
SectionEnd

; The install directory only ever holds what the main section put there. The
; client keeps no message store: the saved account is in Windows Credential
; Manager and the preferences are under %LOCALAPPDATA%\openchime, and the
; uninstaller touches neither, so a reinstall keeps both.
Section "Uninstall"
  ReadRegDWORD $0 HKCU "${UNINST}" "AddedToPath"
  ${If} $0 == 1
    ReadRegStr $1 HKCU "Environment" "Path"
    ${UnStrRep} $1 ";$1;" ";$INSTDIR;" ";"
    ${UnStrRep} $1 "$1" ";;" ";"
    ; Trim the separators added above.
    StrCpy $2 $1 1
    ${If} $2 == ";"
      StrCpy $1 $1 "" 1
    ${EndIf}
    StrCpy $2 $1 1 -1
    ${If} $2 == ";"
      StrCpy $1 $1 -1
    ${EndIf}
    WriteRegExpandStr HKCU "Environment" "Path" "$1"
    SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${EndIf}

  Delete "$INSTDIR\openchime.exe"
  Delete "$INSTDIR\openchime-tui.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\OpenChime\OpenChime.lnk"
  Delete "$SMPROGRAMS\OpenChime\Uninstall OpenChime.lnk"
  RMDir "$SMPROGRAMS\OpenChime"
  Delete "$DESKTOP\OpenChime.lnk"
  Delete "$SMSTARTUP\OpenChime.lnk"

  DeleteRegKey HKCU "Software\Classes\openchime"
  DeleteRegKey HKCU "${UNINST}"
SectionEnd
