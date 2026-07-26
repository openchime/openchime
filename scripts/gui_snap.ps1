# Capture the OpenChime GUI window to a PNG using PrintWindow, which renders the
# window's own contents to a bitmap directly — so it works even when the physical
# display is asleep/locked or the window is occluded (unlike CopyFromScreen, which
# scrapes the composited desktop framebuffer). Used for the WSL feedback loop.
#   powershell.exe -File gui_snap.ps1 -Title OpenChime -Out C:\Windows\Temp\oc.png
param(
  [string]$Title = "OpenChime",
  [string]$Out   = "C:\Windows\Temp\ocsnap.png"
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int cb);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
}
"@

$h = [IntPtr]::Zero
foreach ($p in Get-Process | Where-Object { $_.MainWindowTitle -like "*$Title*" }) {
  $h = $p.MainWindowHandle; break
}
if ($h -eq [IntPtr]::Zero -or -not [Win]::IsWindow($h)) {
  Write-Output "window '$Title' not found"; exit 1
}

# Full window rect (frame included) — PrintWindow renders the whole window.
$r = New-Object RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
if ($w -le 0 -or $hh -le 0) { Write-Output "bad rect"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g   = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
# PW_RENDERFULLCONTENT (0x2) forces DirectX/Direct2D-composited content to render.
$ok = [Win]::PrintWindow($h, $hdc, 0x2)
$g.ReleaseHdc($hdc)

# Crop away the invisible resize border using the DWM extended frame bounds.
$b = New-Object RECT
if ([Win]::DwmGetWindowAttribute($h, 9, [ref]$b, 16) -eq 0) {
  $cx = $b.Left - $r.Left; $cy = $b.Top - $r.Top
  $cw = $b.Right - $b.Left; $ch = $b.Bottom - $b.Top
  if ($cw -gt 0 -and $ch -gt 0 -and $cx -ge 0 -and $cy -ge 0 -and ($cx+$cw) -le $w -and ($cy+$ch) -le $hh) {
    $crop = New-Object System.Drawing.Bitmap $cw, $ch
    $cg = [System.Drawing.Graphics]::FromImage($crop)
    $cg.DrawImage($bmp, (New-Object System.Drawing.Rectangle 0,0,$cw,$ch),
                  (New-Object System.Drawing.Rectangle $cx,$cy,$cw,$ch),
                  [System.Drawing.GraphicsUnit]::Pixel)
    $bmp = $crop; $w = $cw; $hh = $ch
  }
}

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output ("PRINTWINDOW ok={0} {1}x{2} -> {3}" -f $ok, $w, $hh, $Out)
