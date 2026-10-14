# windows_smokes.ps1 -- runs every PowerShell smoke script (the Windows twins of the bash
# smoke scripts) one after another, each in its own PowerShell process, and prints one
# summary table. Exit 0 only if every one is clean. This is what CI runs on the Windows
# runner (.github/workflows/build-archives.yml); run it by hand the same way:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke\windows_smokes.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke\windows_smokes.ps1 -Only cli_smoke,runtime_smoke
#
# Needs build\turfc.exe, build\turf.exe and lib\libturfrt.a (a built tree or an unpacked
# archive laid out like one); git on PATH for git_deps_smoke.

param([string[]]$Only = @())

$ErrorActionPreference = 'Stop'
$scripts = $PSScriptRoot
$all = @(
  'cli_smoke', 'project_smoke', 'git_deps_smoke', 'opt_levels_smoke',
  'runtime_smoke', 'gc_rss_smoke', 'gc_stress_corpus_smoke', 'std002_caller_location_smoke'
)
$run = if ($Only.Count -gt 0) { $all | Where-Object { $Only -contains $_ } } else { $all }
$powershell = (Get-Process -Id $PID).Path

$results = @()
foreach ($name in $run) {
  Write-Host "=== $name" -ForegroundColor Cyan
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  & $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $scripts "$name.ps1")
  $rc = $LASTEXITCODE
  $sw.Stop()
  $results += [pscustomobject]@{ Script = $name; Status = $(if ($rc -eq 0) { 'ok' } else { 'FAILED' }); Seconds = [int]$sw.Elapsed.TotalSeconds }
}

Write-Host ''
Write-Host '=== summary' -ForegroundColor Cyan
foreach ($r in $results) {
  $color = if ($r.Status -eq 'ok') { 'Green' } else { 'Red' }
  Write-Host ('  {0,-32} {1,-7} {2,5}s' -f $r.Script, $r.Status, $r.Seconds) -ForegroundColor $color
}
$bad = @($results | Where-Object { $_.Status -ne 'ok' })
if ($bad.Count -gt 0) {
  Write-Host "windows_smokes: $($bad.Count) of $($results.Count) failed." -ForegroundColor Red
  exit 1
}
Write-Host "windows_smokes: all $($results.Count) clean." -ForegroundColor Green
exit 0
