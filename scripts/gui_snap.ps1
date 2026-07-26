# Capture the OpenChime GUI window (or the full screen as a fallback) to a PNG.
# Used to give a screenshot feedback loop for the native Win32 client from WSL.
#   powershell.exe -File gui_snap.ps1 -Title OpenChime -Out C:\Windows\Temp\oc.png
param(
  [string]$Title = "OpenChime",
  [string]$Out   = "C:\Windows\Temp\ocsnap.png"
)
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int cb);
}
"@

# Find the top-level window whose title contains $Title.
$h = [IntPtr]::Zero
foreach ($p in Get-Process | Where-Object { $_.MainWindowTitle -like "*$Title*" }) {
  $h = $p.MainWindowHandle; break
}

if ($h -ne [IntPtr]::Zero) {
  if ([Win]::IsIconic($h)) { [Win]::ShowWindow($h, 9) | Out-Null }  # SW_RESTORE
  [Win]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 500
  $r = New-Object RECT
  # DWMWA_EXTENDED_FRAME_BOUNDS (9) excludes the invisible resize border, so the
  # capture is tight to the visible window; fall back to GetWindowRect.
  if ([Win]::DwmGetWindowAttribute($h, 9, [ref]$r, [System.Runtime.InteropServices.Marshal]::SizeOf($r)) -ne 0) {
    [Win]::GetWindowRect($h, [ref]$r) | Out-Null
  }
  $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
  if ($w -gt 0 -and $hh -gt 0) {
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $hh)))
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output ("WINDOW {0}x{1} -> {2}" -f $w, $hh, $Out)
    exit 0
  }
}

# Fallback: whole virtual screen.
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output ("SCREEN {0}x{1} -> {2} (window '{3}' not found)" -f $b.Width, $b.Height, $Out, $Title)
