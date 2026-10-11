# gc_rss_smoke.ps1 -- the Windows twin of gc_rss_smoke.sh (MEM-002 / FDN-001: the GC
# stress tests' memory is BOUNDED, not proportional to what the program allocates in
# total). tests/08_e2e_stress/06 and 07 allocate ~80 MB of cumulative requests and keep
# only a little live at a time; if the collector stopped reclaiming, peak memory would
# approach the cumulative figure.
#
# What is measured: the process's PEAK WORKING SET (GetProcessMemoryInfo, read from the
# exited process's handle) -- Windows' name for peak resident set, what GNU time's
# "Maximum resident set size" reports on Linux. Windows counts the loader, the C
# runtime and the program's image in it, so the bound is higher than the bash twin's
# 20000 KB by that baseline; it is measured below and stated, not guessed.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\gc_rss_smoke.ps1
# Exit:  0 = peak memory within bound; 1 = compile/run failure or bound exceeded.

. "$PSScriptRoot\smoke_common.ps1"
Assert-Built
Start-Smoke 'gc_rss_smoke'

# Linux: 20000 KB, measured ~2.7 MB (06) and ~10.4 MB (07). Windows: see Get-Baseline
# below; the bound is the Linux bound plus the measured size of a program that does
# nothing, so it stays the same distance above "no leak" and far below ~80000 KB.
$LinuxBoundKB = 20000
$work = New-WorkDir

# The peak working set of a Turf program that allocates nothing: the loader + C runtime
# + libturfrt baseline this platform adds to every measurement.
function Get-Baseline {
  $src = Join-Path $work 'empty.tr'
  Write-TextFile $src "fn main(): int {`n    return 0;`n}`n"
  $c = Invoke-Turfc -ArgList @($src, '-o', (Join-Path $work 'empty.exe'))
  if ($c.Rc -ne 0) { Write-Fail 'the baseline program failed to compile'; Write-Indented ($c.Out + $c.Err); Complete-Smoke }
  $r = Invoke-Proc -File (Join-Path $work 'empty.exe')
  return $r.PeakKB
}
$baselineKB = Get-Baseline
$boundKB = $LinuxBoundKB + $baselineKB
Write-Host "   baseline (an empty Turf program): $baselineKB KB peak working set"
Write-Host ''

function Test-BoundedRss([string]$TestFile, [string]$Label) {
  Write-Heading "Compile $Label"
  $exe = Join-Path $work 'stress.exe'
  $c = Invoke-Turfc -ArgList @($TestFile, '-o', $exe)
  if ($c.Rc -ne 0) { Write-Fail 'failed to compile'; Write-Indented ($c.Out + $c.Err); return }
  Write-Pass 'compiled'

  Write-Heading 'Run and measure peak working set'
  $r = Invoke-Proc -File $exe -TimeoutSec 300
  if ($r.TimedOut -or $r.Rc -ne 0) {
    Write-Fail "the program itself failed (exit $($r.Rc)$(if ($r.TimedOut) { ', timed out' })) -- not a memory question, see its own output"
    Write-Indented ($r.Out + $r.Err)
    return
  }
  Write-Pass "ran, exit 0 (its own test::assert()s on the total held)"

  Write-Heading 'Peak memory is bounded, not proportional to cumulative allocation'
  Write-Host "   Peak working set: $($r.PeakKB) KB  (bound: $boundKB KB; cumulative requests: ~80000 KB if nothing collected)"
  if ($r.PeakKB -le $boundKB) {
    Write-Pass "$($r.PeakKB) KB is within the $boundKB KB bound"
  } else {
    Write-Fail "$($r.PeakKB) KB EXCEEDS the $boundKB KB bound -- looks like the collector stopped reclaiming"
  }
  Write-Host ''
}

Test-BoundedRss 'tests\08_e2e_stress\06_gc_bounded_allocation.tr' 'the List<int> bounded-allocation stress test'
Test-BoundedRss 'tests\08_e2e_stress\07_heap_bounded_allocation.tr' 'the heap()-node bounded-allocation stress test (FDN-001)'

Complete-Smoke
