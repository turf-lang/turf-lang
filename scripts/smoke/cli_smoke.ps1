# cli_smoke.ps1 -- the Windows twin of cli_smoke.sh (LEC-001 stage 2: the `turf` front end
# drives `turfc` without changing anything about it; FREEZE.md 6.16, F-PKG-1). Same
# checks, same inputs: `turf <args>` is `turfc <args>` byte for byte (stdout, stderr,
# exit status), every compiler mode is reachable as a subcommand, the driver-interface
# handshake refuses a compiler that does not fit and accepts one whose release version
# merely differs, and the CLI writes no file the compiler would not.
#
# Differences from the bash script, each named:
#   * the fake compilers are .cmd files (a Windows process cannot run a shell script);
#   * the temp directory is TMP/TEMP, not TMPDIR;
#   * `turfc --version`'s `root` line starts with a drive letter, not `/`;
#   * the ABI-symbol check searches libturfrt.a's bytes for the name (no `nm` here), which
#     proves the name is present in the archive, not that it is a defined `T` symbol;
#   * the symlinked-turf check is SKIPPED, by name, when the account may not create
#     symlinks (Developer Mode or an administrator is needed).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke\cli_smoke.ps1
# Exit:  0 = all checks pass; 1 = a check failed (each failure is printed).

. "$PSScriptRoot\smoke_common.ps1"
Assert-Built
if (-not (Test-Path -LiteralPath $script:Turf)) { Write-Host 'build\turf.exe missing' -ForegroundColor Red; exit 1 }
Start-Smoke 'cli_smoke'

$V = (Get-Content -LiteralPath (Join-Path $script:Root 'VERSION') -TotalCount 1).Trim()
$versionPath = Join-Path $script:CliDir 'internal\version\version.go'
$versionGo = if (Test-Path -LiteralPath $versionPath) { Get-Content -Raw -LiteralPath $versionPath } else { '' }
if ($versionGo -match '(?m)^const Version = "([^"]*)"') { $CLI_V = $Matches[1] }
else { Write-Host "cannot read turf's version from $versionPath (set TURF_CLI_DIR to turf's source)" -ForegroundColor Red; exit 1 }

$work = New-WorkDir
$caseN = 0

# Cap <exe> <cwd> <env> args... -> the process result (stdout, stderr, status).
function Cap([string]$Exe, [string]$Cwd, [hashtable]$Env, [string[]]$A) {
  return Invoke-Proc -File $Exe -ArgList $A -Cwd $Cwd -Env $Env
}
function Assert-Identical([string]$Label, $A, $B) {
  if ($A.Out -cne $B.Out) { Write-Fail "${Label}: stdout differs" }
  if ($A.Err -cne $B.Err) { Write-Fail "${Label}: stderr differs" }
  if ($A.Rc -ne $B.Rc) { Write-Fail "${Label}: exit status differs ($($A.Rc) vs $($B.Rc))" }
}
function Assert-Rc([string]$Label, $R, [long]$Code) {
  if ($R.Rc -ne $Code) { Write-Fail "${Label}: exit status $($R.Rc), expected $Code" }
}
function Assert-Err([string]$Label, $R, [string]$Text) {
  if (-not $R.Err.Contains($Text)) { Write-Fail "${Label}: stderr lacks '$Text' (got: $($R.Err.Substring(0, [Math]::Min(300, $R.Err.Length))))" }
}
# A fresh directory holding only the given inputs, so "no file was created" is a listing comparison.
function New-Case([string[]]$Files) {
  $script:caseN++
  $d = Join-Path $work "case$($script:caseN)"
  New-Item -ItemType Directory -Path $d | Out-Null
  foreach ($f in $Files) { Copy-Item -LiteralPath (Join-Path $work $f) -Destination $d }
  return $d
}
function Get-Listing([string]$Dir) { return ((Get-ChildItem -LiteralPath $Dir -Force -Name | Sort-Object) -join ' ') }
function Write-Cmd([string]$Path, [string]$Body) { Write-TextFile $Path ($Body -replace "`r?`n", "`r`n") }

$T = $script:Turf
$C = $script:Turfc

# Inputs, written here so the script depends on nothing but the two binaries and the stdlib.
Write-TextFile "$work\pass.tr" "import io;`nfn main(): int {`n    io::printline(`"hello from pass.tr`");`n    return 0;`n}`n"
Write-TextFile "$work\fail_x.tr" "import io;`nfn main(): int {`n    mut a: string = 10;`n    return 0;`n}`n"
Write-TextFile "$work\ret3.tr" "import io;`nfn main(): int {`n    io::printline(`"hello from ret3.tr`");`n    return 3;`n}`n"
Write-TextFile "$work\m.tr" "fn helper(): int {`n    return 1;`n}`n"

# --- passthrough: `turf t.tr -o a` is `turfc t.tr -o a` ------------------------------------
foreach ($name in 'pass', 'fail_x') {  # not $t/$c: PowerShell variables are case-insensitive, and $T/$C are the binaries
  $a = Cap $T $work $null @("$name.tr", '-o', "out_$name")
  $b = Cap $C $work $null @("$name.tr", '-o', "out_$name")
  Assert-Identical "passthrough $name.tr" $a $b
  if ($name -eq 'pass') { $ptPass = $a } else { $ptFail = $a }
}
Assert-Rc 'passthrough pass.tr' $ptPass 0
Assert-Rc 'passthrough fail_x.tr' $ptFail 1
if ($ptFail.Err.Length -eq 0) { Write-Fail 'passthrough fail_x.tr: the failing case printed no diagnostics, so the comparison proves nothing' }
if (-not $ptPass.Out.Contains('Successfully compiled')) { Write-Fail "passthrough pass.tr: expected the compiler's success message on stdout" }
if (-not (Test-Path -LiteralPath "$work\out_pass.exe")) { Write-Fail 'passthrough pass.tr produced no executable' }

# --- `turf compile` is the same thing spelled out ------------------------------------------
Assert-Identical 'compile pass.tr' (Cap $T $work $null @('compile', 'pass.tr', '-o', 'c1_out')) (Cap $C $work $null @('pass.tr', '-o', 'c1_out'))
Assert-Identical 'compile fail_x.tr' (Cap $T $work $null @('compile', 'fail_x.tr', '-o', 'c2_out')) (Cap $C $work $null @('fail_x.tr', '-o', 'c2_out'))

# An unknown flag is forwarded and rejected by turfc in turfc's own words.
$o9a = Cap $T $work $null @('compile', 'pass.tr', '-O9'); $o9b = Cap $C $work $null @('pass.tr', '-O9')
Assert-Identical 'compile -O9' $o9a $o9b
Assert-Rc 'compile -O9' $o9a 2
Assert-Err 'compile -O9' $o9a 'unknown optimization level'

# --- check: every diagnostic, no file ------------------------------------------------------
$d = New-Case @('pass.tr', 'fail_x.tr')
$before = Get-Listing $d
$ckPass = Cap $T $d $null @('check', 'pass.tr')
$ckFail = Cap $T $d $null @('check', 'fail_x.tr')
$ckFailC = Cap $C $d $null @('--check', 'fail_x.tr')
Assert-Rc 'check pass.tr' $ckPass 0
Assert-Rc 'check fail_x.tr' $ckFail 1
Assert-Identical 'check fail_x.tr vs turfc --check' $ckFail $ckFailC
if ((Get-Listing $d) -ne $before) { Write-Fail "check created a file: before='$before' after='$(Get-Listing $d)'" }

# A file with no main is checked as a program by default, and as a module only with --module.
$r = Cap $T $work $null @('check', 'm.tr')
Assert-Rc 'check m.tr (no main)' $r 1
Assert-Err 'check m.tr (no main)' $r 'No entry point found'
Assert-Err 'check m.tr (no main)' $r '--module'
Assert-Rc 'check --module m.tr' (Cap $T $work $null @('check', '--module', 'm.tr')) 0
Assert-Rc 'check --module pass.tr (a program is a fine module too)' (Cap $T $work $null @('check', '--module', 'pass.tr')) 0
$r = Cap $C $work $null @('--module', 'pass.tr')
Assert-Rc 'turfc --module without --check' $r 2
Assert-Err 'turfc --module without --check' $r 'only applies with --check'

# --- dump: turfc --check --module --dump-<what>, no file ----------------------------------
$d = New-Case @('pass.tr')
$before = Get-Listing $d
$dd = Cap $T $d $null @('dump', 'ast', 'pass.tr')
$ddc = Cap $C $d $null @('--check', '--module', '--dump-ast', 'pass.tr')
Assert-Identical 'dump ast' $dd $ddc
Assert-Rc 'dump ast' $dd 0
if ($dd.Out.Length -eq 0) { Write-Fail 'dump ast printed nothing' }
if ((Get-Listing $d) -ne $before) { Write-Fail "dump created a file: '$(Get-Listing $d)'" }
$dm = Cap $T $work $null @('dump', 'ast', 'm.tr')
Assert-Rc 'dump ast m.tr (no main)' $dm 0
if ($dm.Out.Length -eq 0) { Write-Fail 'dump ast m.tr printed nothing' }
$db = Cap $T $work $null @('dump', 'bogus', 'pass.tr')
Assert-Rc 'dump bogus' $db 2
Assert-Err 'dump bogus' $db 'instantiations'
Assert-Rc 'dump (no <what>)' (Cap $T $work $null @('dump')) 2

# --- explain -------------------------------------------------------------------------------
$exa = Cap $T $work $null @('explain', 'E001'); $exb = Cap $C $work $null @('--explain', 'E001')
Assert-Identical 'explain E001' $exa $exb
Assert-Rc 'explain E001' $exa 0
# This script runs from a temp directory, not the checkout: the full write-up must appear from anywhere.
if (-not (Remove-Ansi (Cap $T $work $null @('explain', 'E090')).Out).Contains('A module name matches more than one file')) {
  Write-Fail 'explain E090 outside the checkout: the per-code page was not found'
}
if (-not (Remove-Ansi (Cap $T $work $null @('explain', 'E001')).Out).Contains('reach past the end of an array')) {
  Write-Fail 'explain E001 outside the checkout: the category page was not found'
}
Assert-Rc 'explain (no code)' (Cap $T $work $null @('explain')) 2

# --- fmt -----------------------------------------------------------------------------------
$f1a = Cap $T $work $null @('fmt', 'pass.tr'); $f1b = Cap $C $work $null @('--format', 'pass.tr')
Assert-Identical 'fmt pass.tr' $f1a $f1b
Assert-Rc 'fmt pass.tr' $f1a 0
Assert-Rc 'fmt two files without --write' (Cap $T $work $null @('fmt', 'pass.tr', 'ret3.tr')) 2

# --- migrate -------------------------------------------------------------------------------
$mga = Cap $T $work $null @('migrate', 'pub', 'm.tr'); $mgb = Cap $C $work $null @('--migrate=pub', 'm.tr')
Assert-Identical 'migrate pub' $mga $mgb
Assert-Rc 'migrate pub' $mga 0
if (-not $mga.Out.Contains('would migrate')) { Write-Fail "migrate pub: report lacks 'would migrate'" }
$r = Cap $T $work $null @('migrate')
Assert-Rc 'migrate (no name)' $r 2
Assert-Err 'migrate (no name)' $r 'pub'

# --- run: the program's status, its stdout, and the temp directory gone ---------------------
$tmp = Join-Path $work 'tmp'
New-Item -ItemType Directory -Path $tmp | Out-Null
$tmpEnv = @{ TMP = $tmp; TEMP = $tmp; TMPDIR = $tmp }
$r = Cap $T $work $tmpEnv @('run', 'ret3.tr')
Assert-Rc 'run ret3.tr' $r 3
if (-not $r.Out.Contains('hello from ret3.tr')) { Write-Fail "run: the program's stdout did not pass through" }
if (@(Get-ChildItem -LiteralPath $tmp -Force).Count -ne 0) { Write-Fail "run left a temporary directory behind: $(Get-Listing $tmp)" }
# A failed compile: turfc's status, turfc's diagnostics, and still no leftovers.
$rf = Cap $T $work $tmpEnv @('run', 'fail_x.tr')
$rfc = Cap $C $work $null @('fail_x.tr', '-o', "$work\rf_out")
Assert-Rc 'run fail_x.tr' $rf $rfc.Rc
if (@(Get-ChildItem -LiteralPath $tmp -Force).Count -ne 0) { Write-Fail 'a failed run left a temporary directory behind' }
$r = Cap $T $work $null @('run', 'ret3.tr', '-o', 'x')
Assert-Rc 'run -o' $r 2
Assert-Err 'run -o' $r 'temporary file'
$r = Cap $T $work $null @('run')
Assert-Rc 'run (no file)' $r 2
Assert-Err 'run (no file)' $r 'not inside a TurfLang project'

# --- version and the handshake -------------------------------------------------------------
$verRes = Cap $T $work $null @('version')
Assert-Rc 'version' $verRes 0
$vLines = $verRes.Out -split "`r?`n"
if (-not $vLines[0].StartsWith("turf $CLI_V ")) { Write-Fail "version: line 1 is '$($vLines[0])', expected 'turf $CLI_V ...'" }
if (-not $verRes.Out.Contains("turfc $V")) { Write-Fail "version: output lacks 'turfc $V'" }
if (-not $verRes.Out.Contains('interface')) { Write-Fail 'version: output does not show the driver interface' }
Assert-Identical '--version vs version' $verRes (Cap $T $work $null @('--version'))
$tv = Cap $C $work $null @('--version')
$tvLines = $tv.Out -split "`r?`n"
if ($tvLines[1] -notmatch '^interface [0-9]+\.[0-9]+$') { Write-Fail "turfc --version line 2 is not 'interface M.m'" }

# --- REL-001: what --version reports, and the frozen exit statuses ---------------------------
Assert-Rc 'turfc --version' $tv 0
if ($tv.Out -notmatch '(?m)^llvm [0-9]+\.[0-9]+') { Write-Fail "turfc --version has no 'llvm <version>' line" }
if ($tv.Out -notmatch '(?m)^root ([A-Za-z]:[/\\]|/)') { Write-Fail "turfc --version has no absolute 'root <path>' line" }
$abi = ''
if ($tv.Out -match '(?m)^runtime (\S+)') { $abi = $Matches[1] }
if (-not $abi) { Write-Fail "turfc --version has no 'runtime <abi symbol>' line" }
else {
  # The symbol is defined in C (runtime/turfrt_abi.c) and named in C++ (RuntimeAbiSymbol); a
  # check against the real archive keeps them from drifting. No `nm` here: the name must be
  # present in the archive's bytes (weaker than "defined as T", the best this host can say).
  $lib = Join-Path $script:Root 'lib\libturfrt.a'
  if (Test-Path -LiteralPath $lib) {
    $libText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($lib))
    if (-not $libText.Contains($abi)) { Write-Fail "lib\libturfrt.a does not contain the ABI symbol '$abi' that turfc --version reports" }
  }
}
if ($verRes.Out -notmatch '(?m)^  llvm ') { Write-Fail "turf version does not show turfc's llvm line" }
if ($verRes.Out -notmatch '(?m)^  runtime ') { Write-Fail "turf version does not show turfc's runtime line" }
if ($verRes.Out -notmatch '(?m)^  root ') { Write-Fail "turf version does not show turfc's root line" }

$th = Cap $C $work $null @('help')
Assert-Rc 'turfc help' $th 0
if (-not $th.Out.Contains('Usage: turfc')) { Write-Fail 'turfc help does not print the usage' }
if (-not $th.Out.Contains('Exit status')) { Write-Fail 'turfc help does not document the exit statuses' }
$thf = Cap $C $work $null @('--help')
Assert-Rc 'turfc --help' $thf 2
Assert-Err 'turfc --help' $thf 'turfc help'

# An unknown flag is a usage error (2), with a suggestion when one is near; `turf` passes it
# through byte for byte. Nothing is compiled, so no file appears.
$d = New-Case @('pass.tr')
$before = Get-Listing $d
$ufc = Cap $C $d $null @('pass.tr', '--chek'); $uft = Cap $T $d $null @('pass.tr', '--chek')
Assert-Identical 'unknown flag --chek' $uft $ufc
Assert-Rc 'unknown flag --chek' $ufc 2
Assert-Err 'unknown flag --chek' $ufc 'Unknown option: --chek'
Assert-Err 'unknown flag --chek' $ufc "did you mean '--check'"
if ((Get-Listing $d) -ne $before) { Write-Fail "an unknown flag still produced a file: before='$before' after='$(Get-Listing $d)'" }
$r = Cap $C $work $null @('--verison'); Assert-Rc 'unknown flag --verison' $r 2; Assert-Err 'unknown flag --verison' $r "did you mean '--version'"
$r = Cap $C $work $null @('pass.tr', '--targt=x86_64-linux-gnu'); Assert-Rc 'unknown flag --targt=' $r 2; Assert-Err 'unknown flag --targt=' $r "did you mean '--target='"
$r = Cap $T $work $null @('--bogus'); Assert-Rc 'turf --bogus' $r 2; Assert-Err 'turf --bogus' $r 'Unknown option: --bogus'
$r = Cap $C $work $null @('pass.tr', '--zzzzzzzz')
Assert-Rc 'unknown flag with no near match' $r 2
if ($r.Err.Contains('did you mean')) { Write-Fail "a far-off flag got a 'did you mean' guess" }

# The other usage errors are 2 as well, and a missing value says so.
$r = Cap $C $work $null @('pass.tr', '-o'); Assert-Rc '-o without a value' $r 2; Assert-Err '-o without a value' $r 'needs a value'
Assert-Rc 'turfc with no input' (Cap $C $work $null @()) 2
Assert-Rc 'turfc non-.tr input' (Cap $C $work $null @('notes.txt')) 2
Assert-Rc 'turfc --explain (no code)' (Cap $C $work $null @('--explain')) 2
Assert-Rc 'turfc --explain unknown code' (Cap $C $work $null @('--explain', 'E99999')) 2
Assert-Rc 'turfc --migrate=bogus' (Cap $C $work $null @('--migrate=bogus', 'pass.tr')) 2
# ...while a program with a mistake, and a file that is not there, are 1.
Assert-Rc 'compile error' (Cap $C $work $null @('--check', 'fail_x.tr')) 1
Assert-Rc 'missing input file' (Cap $C $work $null @('--check', 'no_such_file.tr')) 1

# A different RELEASE version is fine when the interface fits (TurfLang 2.0.0 driven by CLI
# 1.0.0). The fake forwards everything but --version to the real compiler, so the compile
# must actually succeed through it. (.cmd: a Windows process cannot run a shell script.)
$iface = $tvLines[1]
Write-Cmd "$work\fake_other_release.cmd" @"
@echo off
if "%~1"=="--version" (
  echo turfc 9.9.9
  echo $iface
  exit /b 0
)
"$C" %*
exit /b %ERRORLEVEL%
"@
$d = New-Case @('pass.tr')
$r = Cap $T $d @{ TURF_COMPILER = "$work\fake_other_release.cmd" } @('check', 'pass.tr')
Assert-Rc 'compiler with another release version, same interface' $r 0

# An interface this turf does not speak is refused, naming both sides.
Write-Cmd "$work\fake_turfc.cmd" "@echo off`necho turfc 0.0.0`necho interface 99.0`n"
$r = Cap $T $work @{ TURF_COMPILER = "$work\fake_turfc.cmd" } @('pass.tr')
Assert-Rc 'incompatible compiler' $r 1
foreach ($want in @('does not speak the interface', $CLI_V, '0.0.0', 'interface 99.0', 'upgrade turf')) { Assert-Err 'incompatible compiler' $r $want }

# A compiler from before the interface existed prints only line 1.
Write-Cmd "$work\fake_old_turfc.cmd" "@echo off`necho turfc 0.0.0`n"
$r = Cap $T $work @{ TURF_COMPILER = "$work\fake_old_turfc.cmd" } @('pass.tr')
Assert-Rc 'pre-interface compiler' $r 1
Assert-Err 'pre-interface compiler' $r 'reports no driver interface'

$r = Cap $T $work @{ TURF_COMPILER = 'C:\nonexistent\turfc.exe' } @('pass.tr')
Assert-Rc 'TURF_COMPILER=C:\nonexistent' $r 1
Assert-Err 'TURF_COMPILER=C:\nonexistent' $r 'TURF_COMPILER'

# Discovery follows symlinks: a `turf` symlinked from elsewhere still finds the turfc beside
# the real binary. Creating a symlink needs Developer Mode or an administrator.
New-Item -ItemType Directory -Path "$work\linkdir" | Out-Null
$linked = $true
try { New-Item -ItemType SymbolicLink -Path "$work\linkdir\turf.exe" -Target $T | Out-Null } catch { $linked = $false }
if ($linked) {
  $r = Cap "$work\linkdir\turf.exe" $work $null @('version')
  Assert-Rc 'symlinked turf' $r 0
  if (-not $r.Out.Contains("turfc $V")) { Write-Fail 'symlinked turf did not find build\turfc.exe' }
} else {
  Write-Skip 'symlinked turf (this account cannot create symlinks: needs Developer Mode or an administrator)'
}

# --- the command surface -------------------------------------------------------------------
$r = Cap $T $work $null @('biuld'); Assert-Rc 'unknown command' $r 2; Assert-Err 'unknown command' $r "did you mean 'build'"
$r = Cap $T $work $null @('install'); Assert-Rc 'turf install' $r 2; Assert-Err 'turf install' $r 'turf fetch'
$r = Cap $T $work $null @('publish'); Assert-Rc 'turf publish' $r 1; Assert-Err 'turf publish' $r 'F-7c'
$r = Cap $T $work $null @('build'); Assert-Rc 'turf build' $r 2; Assert-Err 'turf build' $r 'not inside a TurfLang project'
Assert-Rc 'no arguments' (Cap $T $work $null @()) 2
$h = Cap $T $work $null @('compile', '--help')
Assert-Rc 'compile --help' $h 0
if (-not $h.Out.Contains('--check')) { Write-Fail 'compile --help does not list --check' }
$h2 = Cap $T $work $null @('--help')
Assert-Rc '--help' $h2 0
if (-not $h2.Out.Contains('Project')) { Write-Fail '--help does not list the project commands' }
$flat = [regex]::Replace($h2.Out, '\s+', ' ')
if ($flat -notmatch 'Exit status: 0 success, 1 .*2 the command line is wrong') { Write-Fail '--help does not document the exit statuses' }
$bh = Cap $T $work $null @('build', '--help')
$h3 = Cap $T $work $null @('help', 'build')
Assert-Rc 'help build' $h3 0
Assert-Identical 'help build vs build --help' $h3 $bh
Assert-Rc 'help biuld' (Cap $T $work $null @('help', 'biuld')) 2

Complete-Smoke
