# Sourced by the Win32 harness (lib.sh, and win32/drive.sh and pair.sh, which
# lib.sh does not source): every PowerShell call the harness and the feature
# scripts make, each a named helper here or a .ps1 beside this file, so a
# script says what it does to Windows and never carries PowerShell inline.
#
# PowerShell is Windows' own (powershell.exe, reached from WSL): it is where
# the client, its windows, its clipboard and its saved sign-ins are. Each
# start of it costs a second or more, so a helper does its whole job in one.
WIN_HARNESS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# win_ps1 <file.ps1> [args...]: runs a script beside this file, its output and
# errors on stdout with Windows' line ends taken off; its status is the tr's,
# so a caller reads what it printed.
win_ps1() {
  local f="$1"; shift
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$WIN_HARNESS/$f")" "$@" 2>&1 | tr -d '\r'
}
# A single-quoted PowerShell string of $1 (a quote in it doubled).
_win_quote() { printf "'%s'" "${1//\'/\'\'}"; }

# --- The client --------------------------------------------------------------
# win_close_client: every running client closed, and only forced where that
# fails. Stop-Process -Force is a TerminateProcess, which the client reports as
# a death that ran no handler -- forcing it every time would fill the test dir
# with post-mortems for kills the harness did on purpose, and bury the one that
# matters. CloseMainWindow posts WM_CLOSE, which reaches WM_DESTROY and marks
# the run clean; one that has not gone after 3 s is forced.
win_close_client() {
  powershell.exe -NoProfile -Command "\$p = Get-Process openchime -EA SilentlyContinue; if (\$p) { \$p.CloseMainWindow() | Out-Null; if (-not \$p.WaitForExit(3000)) { \$p | Stop-Process -Force } }" >/dev/null 2>&1 || true
}
# win_client_pids <pid>...: those of the pids that are a running client, one a
# line; nothing when none is.
win_client_pids() {
  [ $# -gt 0 ] || return 0
  local ids; ids="$(printf '%s,' "$@")"; ids="${ids%,}"
  powershell.exe -NoProfile -Command \
    "Get-Process -Id $ids -EA SilentlyContinue | Where-Object Name -eq 'openchime' | ForEach-Object { \$_.Id }" \
    2>/dev/null | tr -d '\r'
}
# win_stop_pids <pid>...: those Windows processes forced to stop, now.
win_stop_pids() {
  [ $# -gt 0 ] || return 0
  local ids; ids="$(printf '%s,' "$@")"; ids="${ids%,}"
  powershell.exe -NoProfile -Command "Get-Process -Id $ids -EA SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1 || true
}
# win_arrange_windows: the two newest clients side by side (a pair's), each
# 900x760, and their pids printed, oldest first, one a line. Positioned from
# outside rather than through a verb: the client's `move` is a mouse move, and
# nothing in the app needs to move its own window.
win_arrange_windows() {
  powershell.exe -NoProfile -Command "
    \$sig='[DllImport(\"user32.dll\")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int t,bool r);'
    \$t=Add-Type -MemberDefinition \$sig -Name W -Namespace N -PassThru
    \$ps=@(Get-Process openchime -EA SilentlyContinue | Sort-Object StartTime | Select-Object -Last 2)
    \$x=40; foreach (\$p in \$ps) { \$t::MoveWindow(\$p.MainWindowHandle, \$x, 60, 900, 760, \$true) | Out-Null; \$x+=940 }
    \$ps | ForEach-Object { \$_.Id }" \
    2>/dev/null | tr -d '\r' | grep -E '^[0-9]+$'
}

# --- Saved sign-ins (Windows Credential Manager) ----------------------------
# The fixtures' sessions are kept under the development service name
# (oc_env_secret_service, client/core/env.c), never the person's own
# "openchime:" entries, and only those are deleted. Through PowerShell, not
# cmd.exe: cmd refuses to run with a UNC working directory, which is what a
# \\wsl.localhost path is, and the delete then failed silently.
# win_forget_signins: every fixture's saved sign-in, listed by that prefix
# and each deleted.
win_forget_signins() {
  powershell.exe -NoProfile -Command "cmdkey /list | Select-String -Pattern 'Target: (?:LegacyGeneric:target=)?(openchime-dev:\S+)' | ForEach-Object { cmdkey /delete:\$(\$_.Matches[0].Groups[1].Value) | Out-Null }" >/dev/null 2>&1 || true
}
# win_forget_signin <host:port>: the fixture's saved sign-in for that workspace.
win_forget_signin() {
  powershell.exe -NoProfile -Command "cmdkey /delete:openchime-dev:$1" >/dev/null 2>&1 || true
}

# --- The clipboard ------------------------------------------------------------
# Set and read as another program does, from outside the client (-STA: the
# clipboard is a single-threaded apartment's).
#   win_clipboard_set_text <text>      # the text on it
#   win_clipboard_set_files <winpath>  # a file list (CF_HDROP), as Explorer's copy
#   win_clipboard_set_png <winpath>    # the picture in that file, as a bitmap
#   win_clipboard_holds <kind>         # 0 when it holds that kind: Text,
#                                      # FileDropList, Image
#   win_clipboard_read                 # the text on it, line ends Windows' taken off
#   win_clipboard_clear                # nothing on it
# A set fails quietly when another program holds the clipboard open: a caller
# that depends on it reads it back (win_clipboard_holds) and sets it again.
win_clipboard_set_text() {
  powershell.exe -NoProfile -STA -Command "Set-Clipboard -Value $(_win_quote "$1")" >/dev/null 2>&1
}
win_clipboard_set_files() {
  powershell.exe -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; \$c = New-Object Collections.Specialized.StringCollection; [void]\$c.Add($(_win_quote "$1")); [Windows.Forms.Clipboard]::SetFileDropList(\$c)" >/dev/null 2>&1
}
win_clipboard_set_png() {
  powershell.exe -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms,System.Drawing; [Windows.Forms.Clipboard]::SetImage([Drawing.Image]::FromFile($(_win_quote "$1")))" >/dev/null 2>&1
}
win_clipboard_holds() {
  [ "$(powershell.exe -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; [Windows.Forms.Clipboard]::Contains$1()" 2>/dev/null | tr -d '\r')" = True ]
}
win_clipboard_clear() {
  powershell.exe -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; [Windows.Forms.Clipboard]::Clear()" >/dev/null 2>&1
}
win_clipboard_read() {
  powershell.exe -NoProfile -STA -Command "Get-Clipboard -Raw" 2>/dev/null | tr -d '\r'
}

# --- Other programs -------------------------------------------------------------
#   win_open_program <name>   # started, as from the Run box (notepad)
#   win_stop_program <name>   # every process of that name, forced
win_open_program() { powershell.exe -NoProfile -Command "Start-Process $(_win_quote "$1")" >/dev/null 2>&1; }
win_stop_program() { powershell.exe -NoProfile -Command "Get-Process $(_win_quote "$1") -EA SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1; }

# --- The browser, played for the person ---------------------------------------
# win_browser_post <get-url> <post-url> <origin> <form-body>: what a browser on
# Windows does with the daemon's page -- GET it, then POST its form from the
# page's own origin, following the redirect (to the client's callback, for a
# sign-in) -- from Windows, where the client's loopback is (browser.ps1).
# Prints the final page, or PowerShell's error.
win_browser_post() {
  win_ps1 browser.ps1 -Get "$1" -Post "$2" -Origin "$3" -Body "$4" || true
}

# --- UI Automation and the desktop ---------------------------------------------
#   win_uia_invoke <aid>       # invokes the client's control of that automation
#                              # id as assistive technology does; 0 when it was
#   win_uia_recbar [args...]   # the screen recorder's bar as UI Automation
#                              # sees it, or a button of it pressed (uia_recbar.ps1)
#   win_capture_desktop <winpath>
#                              # the whole desktop, as the person sees it, into a
#                              # PNG (balloon.ps1); prints its verdict line: ok
#                              # <size>, blank (a locked or disconnected session
#                              # has no desktop), or why not
win_uia_invoke() { grep '^uia_invoke: OK' <<<"$(win_ps1 uia_invoke.ps1 -Aid "$1")" >/dev/null; }
win_uia_recbar() { win_ps1 uia_recbar.ps1 "$@"; }
win_capture_desktop() {
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$WIN_HARNESS/balloon.ps1")" -Out "$1" | tr -d '\r' | tail -1
}
