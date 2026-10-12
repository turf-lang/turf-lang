# runtime_smoke.ps1 -- the Windows twin of runtime_smoke.sh (RT-001 / OVH-001 / FDN-006:
# libturfrt links into a compiled program, the wrapper around main, a panic's message
# shape, TurfTypeDesc bitmaps, the runtime's C API, stack overflow / out of memory /
# missing-runtime diagnostics).
#
# Differences from the bash script, each deliberate and named:
#   1. "turf_rt_init is linked in" reads the symbol out of the .exe's bytes (there is no
#      `nm` on a Windows machine). The runtime is linked with its COFF symbol table.
#   5. The C API test needs a C compiler. A Windows machine has none by default, so this
#      uses gcc from $env:CC / PATH (CI's MSYS2 has one) and is SKIPPED, by name, if there
#      is none -- never reported as passed.
#   6. A Turf program's abort() exits with 0xE0545254 on Windows (runtime/turfrt_signal.c),
#      the bash script's 134.
#   7. The SIGBUS check is skipped: Windows has no SIGBUS (a bad mapped-page access is
#      EXCEPTION_IN_PAGE_ERROR, handled as an access violation, which tests/ covers).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\runtime_smoke.ps1
# Exit:  0 = every check passed; 1 = at least one failed.

. "$PSScriptRoot\smoke_common.ps1"
Assert-Built
Start-Smoke 'runtime_smoke'

$libturfrt = Join-Path $script:Root 'lib\libturfrt.a'
if (-not (Test-Path -LiteralPath $libturfrt)) {
  Write-Host "lib\libturfrt.a missing -- build before running this smoke test." -ForegroundColor Red
  exit 1
}
$work = New-WorkDir
function Join-Output($r) { return ($r.Out + $r.Err) }
function Get-Lines([string]$Text) { return @($Text -split "`r?`n") }

# --- 1 ---------------------------------------------------------------------------------
Write-Heading '1. libturfrt links into a compiled program'
$hello = Join-Path $work 'hello.tr'
Write-TextFile $hello "import io;`nfn main(): int {`n    io::printline(`"runtime_smoke ok`");`n    return 0;`n}`n"
$c = Invoke-Turfc -ArgList @($hello, '-o', (Join-Path $work 'hello.exe'))
if ($c.Rc -eq 0) { Write-Pass 'compiled' } else { Write-Fail 'compile failed'; Write-Indented (Join-Output $c) }
$bytes = [System.IO.File]::ReadAllBytes((Join-Path $work 'hello.exe'))
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
if ($text.Contains('turf_rt_init')) { Write-Pass 'turf_rt_init linked in (symbol table)' }
else { Write-Fail 'turf_rt_init not found in linked binary' }
$r = Invoke-Proc -File (Join-Path $work 'hello.exe')
if ($r.Rc -eq 0 -and $r.Out.TrimEnd() -eq 'runtime_smoke ok') { Write-Pass 'ran, exit 0, correct output' }
else { Write-Fail "unexpected run result (status=$($r.Rc), output='$(Join-Output $r)')" }

# --- 2 ---------------------------------------------------------------------------------
Write-Heading '2. turf_rt_init/turf_rt_shutdown on the call path around main (-O0 IR)'
$c = Invoke-Turfc -ArgList @($hello, '--emit-llvm', '-O0', '-o', (Join-Path $work 'hello_o0'))
if ($c.Rc -eq 0) {
  $ir = Get-Content -Raw -LiteralPath (Join-Path $work 'hello_o0.ll')
  if ($ir.Contains('@turf_rt_init()') -and $ir.Contains('@turf_rt_shutdown()') -and $ir.Contains('@__turf_user_main')) {
    Write-Pass 'wrapper calls both + delegates to __turf_user_main'
  } else { Write-Fail 'wrapper shape missing from -O0 IR' }
} else { Write-Fail '-O0 --emit-llvm compile failed' }

# --- 3 ---------------------------------------------------------------------------------
Write-Heading '3. a panic aborts with the documented message shape'
$panic = Join-Path $work 'panic.tr'
Write-TextFile $panic "fn main(): int {`n    mut z: int = 0;`n    mut x: int = 10 / z;`n    return x;`n}`n"
$c = Invoke-Turfc -ArgList @($panic, '-o', (Join-Path $work 'panic.exe'))
if ($c.Rc -eq 0) {
  $r = Invoke-Proc -File (Join-Path $work 'panic.exe') -TimeoutSec 30
  if (-not $r.TimedOut -and $r.Rc -ne 0) { Write-Pass ("non-zero, non-timeout exit (0x{0:X})" -f $r.Rc) }
  else { Write-Fail "expected a runtime abort, got exit $($r.Rc)" }
  $first = (Get-Lines (Join-Output $r)) | Where-Object { $_ -match '^panic:' } | Select-Object -First 1
  if ($first -match '^panic: runtime check: Division by zero \(.*panic\.tr:[0-9]+\)$') {
    Write-Pass "message matches 'panic: <kind>: <msg> (<file>:<line>)'"
  } else { Write-Fail 'unexpected panic message:'; Write-Indented (Join-Output $r) }
} else { Write-Fail 'panic.tr failed to compile' }

# --- 4 ---------------------------------------------------------------------------------
Write-Heading '4. TurfTypeDesc bitmaps for representative types'
$big70 = (0..69 | ForEach-Object { "f${_}: int, " }) -join ''
$types = Join-Path $work 'types.tr'
Write-TextFile $types @"
import list::List;

struct Plain {
    a: int,
    b: double
}

struct WithPointer {
    p: Plain,
    tag: string
}

// OVH-001: 70 int fields (words 0-69) then a pointer field at word 70 --
// past the old fixed 64-word bitmap cap.
struct Big70 {
    ${big70}name: string
}

fn main(): int {
    mut pl: Plain = Plain::default();
    mut wp: WithPointer = WithPointer::default();
    mut l: List<int> = List::default();
    l.push(1);
    mut big: Big70 = Big70::default();
    return pl.a + wp.p.a + l.length() + big.f0;
}
"@
$c = Invoke-Turfc -ArgList @($types, '--emit-llvm', '-o', (Join-Path $work 'types'))
if ($c.Rc -eq 0) {
  $irLines = Get-Lines (Get-Content -Raw -LiteralPath (Join-Path $work 'types.ll'))
  # $WantBitmapWords is the expected [N x i64] bitmap array's elements, e.g. "0,64".
  function Test-Desc([string]$Name, [int]$WantSize, [int]$WantWordCount, [string]$WantBitmapWords) {
    $esc = [regex]::Escape($Name)
    $descLine = $irLines | Where-Object { $_ -match "@__turf_typedesc_$esc = " } | Select-Object -First 1
    if ($descLine -match "i64 $WantSize, ptr @__turf_typedesc_name\.$esc, i64 $WantWordCount, ptr @__turf_typedesc_bitmap\.$esc \}") {
      Write-Pass "${Name}: size=$WantSize word_count=$WantWordCount"
    } else { Write-Fail "${Name}: expected size=$WantSize word_count=$WantWordCount, got: $descLine" }
    $words = $WantBitmapWords -split ','
    $n = $words.Count
    $bitmapLine = $irLines | Where-Object { $_ -match "@__turf_typedesc_bitmap\.$esc = " } | Select-Object -First 1
    # LLVM's printer collapses an all-zero constant array to `zeroinitializer`.
    if (@($words | Where-Object { $_ -ne '0' }).Count -eq 0) { $wantLit = "[$n x i64] zeroinitializer" }
    else { $wantLit = "[$n x i64] [" + (($words | ForEach-Object { "i64 $_" }) -join ', ') + ']' }
    if ($bitmapLine -and $bitmapLine.Contains($wantLit)) { Write-Pass "${Name}: bitmap=[$WantBitmapWords] ($n word(s))" }
    else { Write-Fail "${Name}: expected bitmap literal '$wantLit', got: $bitmapLine" }
  }
  # Plain { a: int @0, b: double @8 } -- no pointer words.
  Test-Desc 'Plain' 16 2 '0'
  # WithPointer { p: Plain (16B), tag: string (8B @16, a pointer) } -- bit 2 => 4.
  Test-Desc 'WithPointer' 24 3 '4'
  # ListHeader<int> { data: T* @0, len @8, cap @16 } -- bit 0 => 1; List<int> is
  # one `h: &ListHeader<int>` pointer (LEC-009). Module-owned names (FDN-003).
  Test-Desc 'list.ListHeader_gen1_int' 24 3 '1'
  Test-Desc 'list.List_gen1_int' 8 1 '1'
  # OVH-001: Big70 -- word 70 falls in the SECOND bitmap word at bit 6 => 64.
  Test-Desc 'Big70' 568 71 '0,64'
} else { Write-Fail 'types.tr failed to compile' }

# --- 5 ---------------------------------------------------------------------------------
Write-Heading "5. libturfrt's own C API, called directly (no Turf compiler involved)"
$cc = $null
foreach ($cand in @($env:CC, 'gcc', 'cc', 'x86_64-w64-mingw32-gcc')) {
  if ($cand) { $found = Get-Command $cand -ErrorAction SilentlyContinue; if ($found) { $cc = $found.Source; break } }
}
if (-not $cc) {
  Write-Skip 'no C compiler (gcc/cc/$env:CC) on this machine; the check needs one to build a C program against libturfrt.a'
} else {
  $apiC = Join-Path $work 'api_test.c'
  Write-TextFile $apiC @'
#include "turfrt.h"
#include <assert.h>
#include <string.h>

int main(void) {
    turf_rt_init();

    void *p = turf_rt_alloc(64, NULL);
    assert(p != NULL);

    uint64_t bits[1] = {4}; /* bit 2 set */
    TurfTypeDesc desc = {24, "WithPointer", 3, bits};
    assert(turf_rt_typedesc_is_pointer_word(&desc, 0) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&desc, 1) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&desc, 2) == 1);
    assert(turf_rt_typedesc_is_pointer_word(&desc, 3) == 0); /* out of range (word_count=3) */
    assert(turf_rt_typedesc_is_pointer_word(NULL, 0) == 0);

    /* OVH-001: a descriptor wider than the old fixed 64-word cap. */
    uint64_t big_bits[3] = {0, 0, 1};
    TurfTypeDesc big_desc = {1040, "Big", 130, big_bits};
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 63) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 64) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 128) == 1);
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 129) == 0);
    assert(turf_rt_typedesc_is_pointer_word(&big_desc, 130) == 0); /* out of range */

    turf_rt_shutdown();
    return 0;
}
'@
  $c = Invoke-Proc -File $cc -ArgList @('-std=c11', '-I', (Join-Path $script:Root 'runtime'), $apiC, $libturfrt, '-o', (Join-Path $work 'api_test.exe'))
  if ($c.Rc -eq 0) {
    Write-Pass 'compiled against libturfrt.a directly'
    $r = Invoke-Proc -File (Join-Path $work 'api_test.exe')
    if ($r.Rc -eq 0) { Write-Pass 'ran, all assertions held' } else { Write-Fail 'assertion failed at runtime'; Write-Indented (Join-Output $r) }
  } else { Write-Fail 'failed to compile against libturfrt.a'; Write-Indented (Join-Output $c) }
}

# --- 6 ---------------------------------------------------------------------------------
Write-Heading '6. FDN-006: stack overflow, OOM, and missing-runtime diagnostics'

# 6a. Deep, non-tail recursion -> "panic: stack overflow", then abort. (`n > 2000000000`
# only satisfies the unconditional-infinite-recursion check E010; the stack overflows
# long before.)
$so = Join-Path $work 'stack_overflow.tr'
Write-TextFile $so @'
fn recurse(n: int): int {
    mut buf: int[8] = [1, 2, 3, 4, 5, 6, 7, 8];
    if n > 2000000000 {
        return buf[0];
    }
    return recurse(n + 1) + buf[0];
}
fn main(): int {
    return recurse(0);
}
'@
$c = Invoke-Turfc -ArgList @($so, '-o', (Join-Path $work 'stack_overflow.exe'))
if ($c.Rc -eq 0) {
  $r = Invoke-Proc -File (Join-Path $work 'stack_overflow.exe') -TimeoutSec 60
  if ($r.Rc -eq $script:AbortStatus) { Write-Pass 'deep recursion aborts (not a bare access-violation crash)' }
  else { Write-Fail ('expected abort status 0x{0:X8}, got 0x{1:X}' -f $script:AbortStatus, $r.Rc); Write-Indented (Join-Output $r) }
  if ((Join-Output $r) -match '(?m)^panic: stack overflow') { Write-Pass "message starts 'panic: stack overflow'" }
  else { Write-Fail 'unexpected message:'; Write-Indented (Join-Output $r) }
} else { Write-Fail 'stack_overflow.tr failed to compile' }

# 6b. A direct huge allocation -> "panic: out of memory", then abort.
$oom = Join-Path $work 'oom.tr'
Write-TextFile $oom "fn main(): int {`n    unsafe {`n        mut p: void* = gc_alloc(9223372036854775807);`n    }`n    return 0;`n}`n"
$c = Invoke-Turfc -ArgList @($oom, '-o', (Join-Path $work 'oom.exe'))
if ($c.Rc -eq 0) {
  $r = Invoke-Proc -File (Join-Path $work 'oom.exe') -TimeoutSec 60
  if ($r.Rc -eq $script:AbortStatus) { Write-Pass 'huge direct allocation aborts' }
  else { Write-Fail ('expected abort status 0x{0:X8}, got 0x{1:X}' -f $script:AbortStatus, $r.Rc); Write-Indented (Join-Output $r) }
  if ((Join-Output $r) -match '(?m)^panic: out of memory') { Write-Pass "message starts 'panic: out of memory'" }
  else { Write-Fail 'unexpected message:'; Write-Indented (Join-Output $r) }
} else { Write-Fail 'oom.tr failed to compile' }

# 6c. TURF_HOME pointing at a runtime-less tree -> a TurfLang diagnostic naming the missing
# libturfrt.a, reported BEFORE any linker runs. lib\std is COPIED in, not linked: see the
# bash script for why a link would silently change which capability gates apply.
$fakeHome = New-WorkDir
New-Item -ItemType Directory -Path (Join-Path $fakeHome 'lib') | Out-Null
Copy-Item -Recurse -LiteralPath (Join-Path $script:Root 'lib\std') -Destination (Join-Path $fakeHome 'lib\std')
$c = Invoke-Turfc -ArgList @($hello, '-o', (Join-Path $work 'hello_fakehome.exe')) -Env @{ TURF_HOME = $fakeHome }
$o = Join-Output $c
if ($c.Rc -eq 0) {
  Write-Fail 'expected compilation to fail against a runtime-less TURF_HOME, it succeeded'
} elseif ($o.Contains('TurfLang runtime library not found') -and (($o -replace '\\', '/').Contains(($fakeHome -replace '\\', '/') + '/lib/libturfrt.a'))) {
  Write-Pass 'missing libturfrt.a reported as a TurfLang diagnostic naming the path'
} else {
  Write-Fail 'expected a TurfLang diagnostic naming the missing path, got:'
  Write-Indented $o
}

# 6d. A program that names a library the C runtime does not provide goes to the system
# toolchain (gcc). With no gcc on PATH that is a TurfLang diagnostic naming the library, not
# a bare spawn failure; with one, a library the linker cannot find gets the hint that follows
# the linker's own line.
$emptyPath = New-WorkDir
$c = Invoke-Turfc -ArgList @($hello, '-o', (Join-Path $work 'nolib_nocc.exe'), '--link', 'nosuchlib') -Env @{ PATH = $emptyPath }
$o = Join-Output $c
if ($c.Rc -eq 0) {
  Write-Fail 'expected the link to fail with no C compiler on PATH, it succeeded'
} elseif ($o.Contains('linking needs a C compiler') -and $o.Contains('"nosuchlib"')) {
  Write-Pass 'no C compiler: a diagnostic that names the library the program links'
} else {
  Write-Fail "expected 'linking needs a C compiler' naming nosuchlib, got:"
  Write-Indented $o
}
$c = Invoke-Turfc -ArgList @($hello, '-o', (Join-Path $work 'nolib_nofile.exe'), '--link', 'nosuchlib')
$o = Join-Output $c
if ($c.Rc -eq 0) {
  Write-Fail 'expected the link to fail for a library that does not exist, it succeeded'
} elseif ($o.Contains('links against "nosuchlib", which Turf does not bundle')) {
  Write-Pass 'a library the linker cannot find: the hint names it and says what to install'
} elseif ($o.Contains('linking needs a C compiler')) {
  Write-Skip 'no gcc on PATH on this machine, so the linker-ran-and-failed hint cannot be checked'
} else {
  Write-Fail "expected the 'does not bundle' hint, got:"
  Write-Indented $o
}

# --- 7 ---------------------------------------------------------------------------------
Write-Heading '7. a SIGBUS is reported like a SIGSEGV, not a bare crash'
Write-Skip 'no SIGBUS on Windows; a bad access to a mapped page raises EXCEPTION_IN_PAGE_ERROR, which the vectored handler reports as an access violation (covered by the stack-overflow check above and tests/)'

if ($script:Failed -eq 0) { Write-Host ''; Write-Host 'All runtime smoke checks passed.' -ForegroundColor Green }
Complete-Smoke
