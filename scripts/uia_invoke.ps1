# Press one control by AutomationId, through UI Automation (REQ-290).
#
# This is the point of the identifiers: a test finds a control by a name that is
# part of the control, and invokes it through the same pattern a screen reader or
# any automation client would. No coordinates, so nothing here breaks when a pane
# is re-laid out — which is what made the coordinate-driven harness need a human
# to decide whether a failure was real.
#
#   powershell.exe -NoProfile -File scripts/uia_invoke.ps1 -Aid shelf.threads
#
# ASCII ONLY in code strings; see uia_probe.ps1 for why.
param([Parameter(Mandatory=$true)][string]$Aid)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$proc = Get-Process openchime -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Output 'FAIL no openchime process'; exit 1 }
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$root = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
    [System.Windows.Automation.TreeScope]::Children, $cond)
if (-not $root) { Write-Output 'FAIL no automation element for the process'; exit 1 }

$byId = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Aid)
$el = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $byId)
if (-not $el) { Write-Output "FAIL no control with AutomationId '$Aid'"; exit 1 }

try {
    $p = $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
} catch {
    Write-Output "FAIL '$Aid' is not invokable"; exit 1
}
$p.Invoke()
Write-Output "uia_invoke: OK $Aid"
