# Capture the whole desktop to a PNG and say whether there was a desktop to
# capture. CopyFromScreen, deliberately: the tray balloon is drawn by the shell,
# not by our window, so PrintWindow (gui_snap.ps1) cannot see it. The price is
# that a disconnected or locked session has no composited desktop and yields
# solid black -- reported as "blank" so the caller never mistakes it for a
# picture of a missing balloon.
#   powershell.exe -File gui_balloon.ps1 -Out C:\Windows\Temp\ocballoon.png
param([string]$Out = "C:\Windows\Temp\ocballoon.png")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$b   = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g   = [System.Drawing.Graphics]::FromImage($bmp)
# A disconnected session has no desktop to copy from and throws rather than
# returning black on some builds; both mean the same thing here.
try { $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size) } catch { }
$lit = 0
for ($y = 0; $y -lt $b.Height -and -not $lit; $y += 40) {
  for ($x = 0; $x -lt $b.Width; $x += 40) {
    $c = $bmp.GetPixel($x, $y)
    if ($c.R -or $c.G -or $c.B) { $lit = 1; break }
  }
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
if ($lit) { Write-Output "ok $($b.Width)x$($b.Height)" } else { Write-Output "blank $($b.Width)x$($b.Height)" }
