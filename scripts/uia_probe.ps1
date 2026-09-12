# Walk OpenChime's UI Automation tree from OUTSIDE the process (REQ-269, ARCH-99).
#
# WHY THIS EXISTS. The app can publish a perfect accessible tree into its own
# dump and still expose nothing: the dump proves what we *published*, not what a
# client can *see*. Those are different claims, and only the second one is the
# feature. This is a real UIA client — the same API a screen reader uses — so a
# pass here means an assistive technology can actually read the app.
#
#   powershell.exe -NoProfile -File scripts/uia_probe.ps1            # print the tree
#   powershell.exe -NoProfile -File scripts/uia_probe.ps1 -Assert    # exit 1 on failure
#
# ASCII ONLY in code strings. Windows PowerShell reads a .ps1 without a BOM as
# ANSI, so a UTF-8 em dash inside a quoted string becomes three characters and
# the parser reports "Missing closing '}'" pointing at a block forty lines away.
# Comments are safe; strings are not.
param([switch]$Assert)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$proc = Get-Process openchime -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { Write-Output 'FAIL no openchime process'; exit 1 }

# By process id rather than window title: the title changes with the workspace,
# and matching on it made this fail for a reason that had nothing to do with UIA.
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$root = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
    [System.Windows.Automation.TreeScope]::Children, $cond)
if (-not $root) { Write-Output 'FAIL no automation element for the process'; exit 1 }

$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$counts = @{}
$ids = @{}          # AutomationId -> how many elements claim it (REQ-290)
$noId = @()         # buttons and tabs with no id at all
$notInvokable = @() # ...and ones a client could find but not press
$composerText = $null
$textDocs = @{}
$firstWord = @{}

# MAX_DEPTH bounds the printed indentation, and it used to bound the WALK -- but
# $ids, $noId and $notInvokable are only populated in here, so "every control has
# a unique id" was really "every control in the first seven levels". The four
# named-id checks below use FindFirst(Descendants) and are unbounded, which made
# the inconsistency easy to miss. The cap now limits printing only; the walk is
# complete, and $seen stops a cyclic provider looping.
$script:MAX_PRINT_DEPTH = 6
$script:seen = @{}

function Walk($el, $depth) {
    $rt = $null
    try { $rt = $el.GetRuntimeId() -join '.' } catch { }
    if ($rt) {
        if ($script:seen.ContainsKey($rt)) { return }
        $script:seen[$rt] = $true
    }
    $c = $el.Current
    $type = $c.ControlType.ProgrammaticName -replace 'ControlType\.', ''
    $script:counts[$type] = 1 + ($script:counts[$type] | ForEach-Object { $_ })
    $pad = ' ' * ($depth * 2)
    $name = $c.Name
    if ($name.Length -gt 72) { $name = $name.Substring(0, 72) + '...' }
    if ($depth -le $script:MAX_PRINT_DEPTH) {
        Write-Output ("{0}{1}: {2}" -f $pad, $type, $name)
    }

    if ($type -eq 'Edit') {
        try {
            $vp = $el.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
            $script:composerText = $vp.Current.Value
        } catch { }
    }
    # TextPattern is the claim that the conversation can be READ, not merely
    # listed. Exercised rather than detected: a pattern that answers an empty
    # document is indistinguishable from an absent one until you ask it for text.
    if ($type -eq 'Document' -or $type -eq 'Edit') {
        try {
            $tp  = $el.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
            $doc = $tp.DocumentRange.GetText(-1)
            $script:textDocs[$type] = $doc
            # And that ranges move in units — "read the next word" is the whole
            # point of the pattern over a plain Name property. The range is
            # COLLAPSED to its start first: expanding a whole-document range by
            # Word correctly returns the whole document, which proves nothing.
            $r = $tp.DocumentRange.Clone()
            $r.MoveEndpointByRange(
                [System.Windows.Automation.Text.TextPatternRangeEndpoint]::End,
                $r, [System.Windows.Automation.Text.TextPatternRangeEndpoint]::Start)
            $r.ExpandToEnclosingUnit([System.Windows.Automation.Text.TextUnit]::Word)
            $script:firstWord[$type] = $r.GetText(-1)
        } catch { $script:textDocs[$type] = "<no TextPattern: $($_.Exception.Message)>" }
    }
    # REQ-290: every element you can act on carries a stable, UNIQUE id, and the
    # ones that are buttons can actually be pressed. A tree you can read and not
    # drive is half the feature.
    if ($type -eq 'Button' -or $type -eq 'TabItem') {
        $aid = $c.AutomationId
        if (-not $aid) { $script:noId += "$type '$name'" }
        else {
            $script:ids[$aid] = 1 + ($script:ids[$aid] | ForEach-Object { $_ })
            try { $null = $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern) }
            catch { $script:notInvokable += $aid }
        }
    }
    $child = $walker.GetFirstChild($el)
    while ($child) { Walk $child ($depth + 1); $child = $walker.GetNextSibling($child) }
}

Walk $root 0

Write-Output ''
Write-Output ('counts: ' + (($counts.GetEnumerator() | Sort-Object Name |
    ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '))
if ($null -ne $composerText) { Write-Output ("composer_value: '" + $composerText + "'") }
foreach ($k in $textDocs.Keys) {
    $t = $textDocs[$k]
    if ($t.Length -gt 90) { $t = $t.Substring(0, 90) + '...' }
    Write-Output ("text[$k]: '" + ($t -replace "`r?`n", '\n') + "'")
    Write-Output ("word[$k]: '" + $firstWord[$k] + "'")
}

if ($Assert) {
    $fail = 0
    foreach ($want in 'List', 'ListItem', 'Edit') {
        if (-not $counts.ContainsKey($want)) { Write-Output "FAIL no $want in the tree"; $fail = 1 }
    }
    if (-not $counts.ContainsKey('Document')) { Write-Output 'FAIL no Document (the transcript)'; $fail = 1 }
    foreach ($k in 'Document', 'Edit') {
        if (-not $textDocs.ContainsKey($k) -or $textDocs[$k] -like '<no TextPattern*') {
            Write-Output "FAIL $k has no working TextPattern"; $fail = 1
        }
    }
    # The identifiers (REQ-290). Named ones first: these are the contract, and a
    # rename is a breaking change rather than a test to update.
    foreach ($want in 'rail.home', 'shelf.threads', 'composer.send', 'composer') {
        $byId = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $want)
        if (-not $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $byId)) {
            Write-Output "FAIL no element with AutomationId '$want'"; $fail = 1
        }
    }
    if ($noId.Count) {
        Write-Output ("FAIL " + $noId.Count + " actionable element(s) with no AutomationId: " +
                      (($noId | Select-Object -First 4) -join ', '))
        $fail = 1
    }
    $dupes = $ids.GetEnumerator() | Where-Object { $_.Value -gt 1 }
    if ($dupes) {
        Write-Output ("FAIL duplicate AutomationId(s): " +
                      (($dupes | ForEach-Object { "$($_.Key) x$($_.Value)" }) -join ', '))
        $fail = 1
    }
    if ($notInvokable.Count) {
        Write-Output ("FAIL found but not pressable: " +
                      (($notInvokable | Select-Object -First 4) -join ', '))
        $fail = 1
    }
    if ($fail) { exit 1 }
    Write-Output ("uia_probe: OK - " + $ids.Count + " identified controls, all invokable")
}
