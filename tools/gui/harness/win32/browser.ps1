# The browser, on Windows, where the client's loopback is: GET the daemon's
# page, then POST its form from the page's own origin, following the redirect
# (to the client's callback, for a sign-in). Prints the final page.
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File browser.ps1 -Get <url> -Post <url> -Origin <origin> -Body <form>
# Run through win_browser_post (win.sh).
param([string]$Get, [string]$Post, [string]$Origin, [string]$Body)
$ErrorActionPreference = 'Stop'
$null = Invoke-WebRequest -UseBasicParsing -Uri $Get
$r = Invoke-WebRequest -UseBasicParsing -Uri $Post -Method POST -Body $Body `
     -ContentType 'application/x-www-form-urlencoded' -Headers @{ Origin = $Origin }
$r.Content
