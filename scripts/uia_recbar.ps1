# The screen recording bar (REQ-162, REQ-166), through UI Automation: is it there,
# what does it say, and press one of its buttons as a screen reader's invoke does.
#
#   powershell.exe -NoProfile -File scripts/uia_recbar.ps1              # report
#   powershell.exe -NoProfile -File scripts/uia_recbar.ps1 -Press Stop  # press a button
#
# The bar is a top-level window of its own, so it is looked for among the
# desktop's children rather than inside the main window's tree.
#
# ASCII ONLY in code strings; see uia_probe.ps1 for why.
param([string]$Press = '')

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$proc = Get-Process openchime -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Output 'FAIL no openchime process'; exit 1 }
$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$bar = $null
foreach ($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)) {
    if ($w.Current.ClassName -eq 'OpenChimeRecordingBar') { $bar = $w; break }
}
if (-not $bar) { Write-Output 'bar=0'; exit 0 }

# Windows PowerShell's UI Automation client does not load the proxies that map a
# plain Win32 control to its control type (Narrator's native client does), so
# the bar's static text and buttons come through as panes named for what they
# say. They are matched by their window class and name; pressing one sends the
# click a button's UIA proxy sends when invoked (BM_CLICK).
$text = ''
$buttons = @()
foreach ($e in $bar.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                             [System.Windows.Automation.Condition]::TrueCondition)) {
    if ($e.Current.ClassName -eq 'Static') { $text = $e.Current.Name }
    if ($e.Current.ClassName -eq 'Button') { $buttons += $e }
}
$names = ($buttons | ForEach-Object { $_.Current.Name }) -join ','
Write-Output ("bar=1 text=""{0}"" buttons={1}" -f $text, $names)

if ($Press) {
    $b = $buttons | Where-Object { $_.Current.Name -eq $Press } | Select-Object -First 1
    if (-not $b) { Write-Output "FAIL no button $Press"; exit 1 }
    Add-Type -Namespace OcUia -Name Native -MemberDefinition '[DllImport("user32.dll")] public static extern System.IntPtr SendMessageW(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);'
    [void][OcUia.Native]::SendMessageW([System.IntPtr]$b.Current.NativeWindowHandle, 0x00F5, [System.IntPtr]::Zero, [System.IntPtr]::Zero)
    Write-Output "pressed $Press"
}
