; Inno Setup script for the Windows client (ARCH-80/81).
;
; The download channel. It sits alongside the MSIX under msix/, which is the
; Store channel; the two ship the same two binaries and differ only in how
; Windows installs them. Both exist because neither covers the other's case:
; the Store cannot serve a direct download link, and an MSIX cannot be handed
; to someone who does not want a Store account.
;
; PER-USER, deliberately. PrivilegesRequired=lowest puts the install under
; %LOCALAPPDATA%\Programs and asks for no UAC elevation, which matters for a
; chat client people install on a work machine they do not administer. The
; cost is that a machine-wide deployment is not possible from this installer;
; that case is the MSIX, or a future MSI.
;
; Built by .github/workflows/release.yml, which passes the release number:
;   ISCC.exe /DVersion=7 /Odist packaging/windows/openchime.iss

#ifndef Version
  #define Version "0"
#endif

; The four-part form Windows expects in file version metadata. version.sh
; hands out a bare integer, so it lands in the third field: 1.0.<release>.0
; increases monotonically, which is the only property Windows requires.
#define FileVersion "1.0." + Version + ".0"

[Setup]
; NEVER CHANGE THIS GUID. It is the identity Windows matches an existing
; install against; a new one turns every upgrade into a second parallel
; install with its own uninstall entry.
AppId={{8C4E1F2A-6D3B-4A57-9E80-2F1B7C5D9A43}
AppName=OpenChime
AppVersion={#Version}
AppVerName=OpenChime {#Version}
VersionInfoVersion={#FileVersion}
AppPublisher=OpenChime
AppPublisherURL=https://openchime.io
AppSupportURL=https://openchime.io
DefaultDirName={autopf}\OpenChime
DefaultGroupName=OpenChime
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
; x64compatible rather than x64: the binaries are x86-64, and this spelling
; also admits ARM64 machines running them under emulation.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=openchime-{#Version}-windows-x64-setup
SetupIconFile=..\..\client\gui\win32\res\openchime.ico
UninstallDisplayIcon={app}\openchime.exe
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Offers to close a running copy instead of failing the file replacement, and
; restarts it afterwards. Without this an upgrade over a running client aborts.
CloseApplications=yes
RestartApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"
Name: "startup"; Description: "Start OpenChime when I sign in"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
; The TUI is a console program and is useless without a shell that can find
; it, so PATH is offered rather than assumed. HKCU only -- a per-user install
; has no business writing the machine PATH.
Name: "addtopath"; Description: "Add the &terminal client (openchime-tui) to PATH"; GroupDescription: "Command line:"; Flags: unchecked

[Files]
; Both binaries are statically linked by the mingw-w64 build, so there is no
; redistributable to chain and nothing else to place here.
Source: "..\..\build\openchime.exe";     DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\build\openchime-tui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\README.md";               DestDir: "{app}"; Flags: ignoreversion

[Icons]
; AppUserModelID is not decoration. Windows resolves a toast's identity through
; the Start-menu shortcut, so an unpackaged install with no AUMID here cannot
; raise a Notification Center toast at all -- it falls back to the tray balloon
; and nobody is told why. The string must match OC_AUMID in the client exactly.
Name: "{group}\OpenChime";        Filename: "{app}\openchime.exe"; \
    AppUserModelID: "BronzeVenture.OpenChime"
Name: "{group}\Uninstall OpenChime"; Filename: "{uninstallexe}"
Name: "{autodesktop}\OpenChime";  Filename: "{app}\openchime.exe"; Tasks: desktopicon
Name: "{userstartup}\OpenChime";  Filename: "{app}\openchime.exe"; Tasks: startup

[Registry]
; openchime:// -- how a notification's click gets back to the app. The client
; also writes these on every start (a portable copy or a dev build has no
; installer), but a fresh install should work before the first launch rather
; than after it.
Root: HKCU; Subkey: "Software\Classes\openchime"; ValueType: string; \
    ValueName: ""; ValueData: "URL:OpenChime"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\openchime"; ValueType: string; \
    ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\openchime\shell\open\command"; ValueType: string; \
    ValueName: ""; ValueData: """{app}\openchime.exe"" ""%1"""
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
    ValueData: "{olddata};{app}"; Tasks: addtopath; \
    Check: NeedsAddPath(ExpandConstant('{app}'))

[Run]
Filename: "{app}\openchime.exe"; Description: "Launch OpenChime"; \
    Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The install directory only ever holds what [Files] put there; user data
; lives under {userappdata}\OpenChime and is deliberately left behind, so a
; reinstall does not lose the message store or the saved account.
Type: dirifempty; Name: "{app}"

[Code]
// Appending unconditionally would grow PATH by one copy of the install
// directory on every repair or upgrade.
//
// Line comments, not a { } block: Pascal brace comments do not nest, so a
// block comment mentioning an Inno constant like the app directory ends at
// that constant's own closing brace and the rest of the sentence is compiled
// as code. That is what "Column 66: 'BEGIN' expected" meant here.
function NeedsAddPath(Param: string): Boolean;
var
  Existing: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Existing) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(Existing) + ';') = 0;
end;
