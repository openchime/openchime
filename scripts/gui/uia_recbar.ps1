# The screen recording bar (REQ-162, REQ-166), through UI Automation: is it there,
# what does it say, and press one of its buttons as a screen reader's invoke does.
#
#   powershell.exe -NoProfile -File scripts/gui/uia_recbar.ps1              # report
#   powershell.exe -NoProfile -File scripts/gui/uia_recbar.ps1 -Press Stop  # press a button
#
# The bar is a top-level window of its own, so it is looked for among the
# desktop's children, by its automation id, rather than inside the main
# window's tree.
#
# ASCII ONLY in code strings; see scripts/gui/uia_probe.ps1 for why.
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
    if ($w.Current.AutomationId -eq 'recbar') { $bar = $w; break }
}
if (-not $bar) { Write-Output 'bar=0'; exit 0 }

# The bar is drawn, not built from Win32 controls; the app serves its line and
# its buttons as the window's own UI Automation tree, by automation id
# (recbar.text, recbar.stop, recbar.discard), and a button is pressed through
# its InvokePattern, as a screen reader presses it.
$text = ''
$buttons = @()
foreach ($e in $bar.FindAll([System.Windows.Automation.TreeScope]::Children,
                             [System.Windows.Automation.Condition]::TrueCondition)) {
    if ($e.Current.AutomationId -eq 'recbar.text') { $text = $e.Current.Name }
    if ($e.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) { $buttons += $e }
}
$names = ($buttons | ForEach-Object { $_.Current.Name }) -join ','
Write-Output ("bar=1 text=""{0}"" buttons={1}" -f $text, $names)

if ($Press) {
    $b = $buttons | Where-Object { $_.Current.Name -eq $Press } | Select-Object -First 1
    if (-not $b) { Write-Output "FAIL no button $Press"; exit 1 }
    $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Write-Output "pressed $Press"
}
