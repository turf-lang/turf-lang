# std002_caller_location_smoke.ps1 -- the Windows twin of std002_caller_location_smoke.sh
# (STD-002: test::assert's failure message names the CALLER's file and line, not
# lib/std/testing/test.tr's). Same three checks, same probe program, compiled and run
# for real with its stderr captured.
#
# Difference from the bash script: a Turf program's abort() exits with
# 0xE0545254 on Windows (runtime/turfrt_signal.c), not 134 -- 134 is the test
# runner's name for it, and a POSIX shell's.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke\std002_caller_location_smoke.ps1
# Exit:  0 = every check passed; 1 = at least one failed.

. "$PSScriptRoot\smoke_common.ps1"
Assert-Built
Start-Smoke 'std002_caller_location_smoke'

$work = New-WorkDir
Write-TextFile (Join-Path $work 'user_test.tr') @'
import test;

fn main(): int {
    test::assert(1 == 2, "should never happen");
    return 0;
}
'@

$c = Invoke-Turfc -ArgList @((Join-Path $work 'user_test.tr'), '-o', (Join-Path $work 'user_test.exe'))
if ($c.Rc -ne 0) {
  Write-Fail 'the probe program failed to compile:'
  Write-Indented ($c.Out + $c.Err)
  Complete-Smoke
}

$r = Invoke-Proc -File (Join-Path $work 'user_test.exe')
$out = $r.Out + $r.Err

Write-Heading '1. A failing test::assert() aborts'
if ($r.Rc -eq $script:AbortStatus) {
  Write-Pass ('exit 0x{0:X8} (abort), as expected' -f $r.Rc)
} else {
  Write-Fail ('expected exit 0x{0:X8} (abort), got {1}' -f $script:AbortStatus, $r.Rc)
}

Write-Host ''
Write-Heading "2. The failure message names the CALLER's file and line"
if ($out -match 'user_test\.tr:4') {
  Write-Pass "message contains 'user_test.tr:4' (the probe's own test::assert call site)"
} else {
  Write-Fail "message does not name the caller's file:line. Got:"
  Write-Indented $out
}

Write-Host ''
Write-Heading '3. The failure message does NOT name lib/std/testing/test.tr'
if ($out -match 'testing[/\\]test\.tr') {
  Write-Fail 'message still names lib/std/testing/test.tr -- the bug STD-002 fixes. Got:'
  Write-Indented $out
} else {
  Write-Pass "message does not leak test.tr's own internal file/line"
}

Complete-Smoke
