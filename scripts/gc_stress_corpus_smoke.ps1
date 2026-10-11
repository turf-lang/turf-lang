# gc_stress_corpus_smoke.ps1 -- the Windows twin of gc_stress_corpus_smoke.sh (FDN-005
# stage 1: the three long-running stress programs under tests\custom\gc_stress_corpus,
# each compiled and run directly, asserting a clean exit and BOUNDED memory). See the
# bash script's header for why these are not part of the ordinary suite and are never
# run under TURF_GC_STRESS.
#
# Memory is the process's peak working set (see gc_rss_smoke.ps1), so each bound is
# the bash script's RSS bound plus the measured baseline of an empty Turf program.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\gc_stress_corpus_smoke.ps1
# Exit:  0 = every check passed; 1 = at least one failed.

. "$PSScriptRoot\smoke_common.ps1"
Assert-Built
Start-Smoke 'gc_stress_corpus_smoke'

$corpus = 'tests\custom\gc_stress_corpus'
$work = New-WorkDir

$src = Join-Path $work 'empty.tr'
Write-TextFile $src "fn main(): int {`n    return 0;`n}`n"
$c = Invoke-Turfc -ArgList @($src, '-o', (Join-Path $work 'empty.exe'))
if ($c.Rc -ne 0) { Write-Fail 'the baseline program failed to compile'; Write-Indented ($c.Out + $c.Err); Complete-Smoke }
$baselineKB = (Invoke-Proc -File (Join-Path $work 'empty.exe')).PeakKB
Write-Host "   baseline (an empty Turf program): $baselineKB KB peak working set"
Write-Host ''

function Get-LastLine([string]$Text) {
  $lines = @($Text -split "`r?`n" | Where-Object { $_ -ne '' })
  if ($lines.Count -eq 0) { return '' }
  return $lines[$lines.Count - 1]
}

function Test-BoundedRss([string]$TestFile, [int]$LinuxBoundKB, [string]$Label) {
  $boundKB = $LinuxBoundKB + $baselineKB
  Write-Heading $Label
  $exe = Join-Path $work 'prog.exe'
  $c = Invoke-Turfc -ArgList @($TestFile, '-o', $exe)
  if ($c.Rc -ne 0) { Write-Fail 'failed to compile'; Write-Indented ($c.Out + $c.Err); return }
  Write-Pass 'compiled'

  $r = Invoke-Proc -File $exe -TimeoutSec 600
  if ($r.TimedOut -or $r.Rc -ne 0) {
    Write-Fail "the program itself failed (exit $($r.Rc)$(if ($r.TimedOut) { ', timed out' })):"
    Write-Indented ($r.Out + $r.Err)
    return
  }
  Write-Pass "ran, exit 0 -- $(Get-LastLine $r.Out)"
  Write-Host "   Peak working set: $($r.PeakKB) KB (bound: $boundKB KB)"
  if ($r.PeakKB -le $boundKB) { Write-Pass "$($r.PeakKB) KB is within the $boundKB KB bound" }
  else { Write-Fail "$($r.PeakKB) KB EXCEEDS the $boundKB KB bound" }
  Write-Host ''
}

# 1 and 2: see the bash script for how each bound was derived.
Test-BoundedRss "$corpus\million_node_graph_churn.tr" 300000 '1. A 1,000,000-node graph, built and rewired 4 times'
Test-BoundedRss "$corpus\map_of_lists_churn.tr" 40000 '2. Map<string, List<string>> churned for 10,000,000 operations'

# 3. A long-lived server driven by a scripted client for 10^5 sequential requests over
# loopback; the server exits on its own after its request count, so its peak is real.
Write-Heading '3. http_server-shaped load: 100,000 sequential requests'
# Not run by default on Windows, and not because of the collector: the client opens one
# TCP connection per request, and Windows' default ephemeral port range is 16384 ports
# (49152-65535; `netsh int ipv4 show dynamicport tcp`), each held in TIME_WAIT for
# 120 s after it closes. Measured 2026-10-11: connect() fails at request 16593, with the
# server's memory flat at ~6 MB. Linux has ~28000 ports and reuses TIME_WAIT ones, so the
# bash twin gets through all 100000. Set TURF_SMOKE_HTTP_LOAD=1 to run it anyway (on a
# machine whose range was widened: `netsh int ipv4 set dynamicport tcp start=10000 num=55000`).
if (-not $env:TURF_SMOKE_HTTP_LOAD) {
  Write-Skip 'skipped on Windows: 100000 sequential connections exceed the default 16384-port range (see the comment above); set TURF_SMOKE_HTTP_LOAD=1 to run'
  Complete-Smoke
}
$serverExe = Join-Path $work 'http_load_server.exe'
$clientExe = Join-Path $work 'http_load_client.exe'
$sc = Invoke-Turfc -ArgList @("$corpus\http_load_server.tr", '-o', $serverExe)
if ($sc.Rc -eq 0) { Write-Pass 'server compiled' } else { Write-Fail 'server failed to compile'; Write-Indented ($sc.Out + $sc.Err) }
$cc = Invoke-Turfc -ArgList @("$corpus\http_load_client.tr", '-o', $clientExe)
if ($cc.Rc -eq 0) { Write-Pass 'client compiled' } else { Write-Fail 'client failed to compile'; Write-Indented ($cc.Out + $cc.Err) }

if ((Test-Path $serverExe) -and (Test-Path $clientExe) -and $sc.Rc -eq 0 -and $cc.Rc -eq 0) {
  $server = Start-Proc -File $serverExe
  Start-Sleep -Milliseconds 300
  $client = Invoke-Proc -File $clientExe -TimeoutSec 120
  if (-not $client.TimedOut -and $client.Rc -eq 0) { Write-Pass "client: $(Get-LastLine $client.Out)" }
  else { Write-Fail 'client failed or timed out:'; Write-Indented ($client.Out + $client.Err) }

  # The server exits on its own once it has handled its request count; wait for it
  # rather than killing it, so its peak figure is real.
  $s = $server.Finish(120)
  if (-not $s.TimedOut -and $s.Rc -eq 0) { Write-Pass "server: $(Get-LastLine $s.Out)" }
  else { Write-Fail "server exited non-zero (exit $($s.Rc)):"; Write-Indented ($s.Out + $s.Err) }

  $boundKB = 30000 + $baselineKB
  Write-Host "   Server peak working set: $($s.PeakKB) KB (bound: $boundKB KB)"
  if ($s.PeakKB -le $boundKB) { Write-Pass "$($s.PeakKB) KB is within the $boundKB KB bound" }
  else { Write-Fail "$($s.PeakKB) KB EXCEEDS the $boundKB KB bound" }
} else {
  Write-Fail 'skipping run: compile failure above'
}

Complete-Smoke
