# opt_levels_smoke.ps1 -- the Windows twin of opt_levels_smoke.sh (OPT-001, F-OPT-1:
# optimization levels never change observable behaviour). Same three sets, same
# checks: the deterministic programs must print the same bytes and exit with the same
# code at -O0/-O1/-O2/-O3/-Os; the nondeterministic ones must build and exit 0 at every
# level; the server/client ones must compile at every level.
#
# One program is left out on Windows, by name: showcase/llvm_ffi_selfhost_demo.tr binds
# LLVM-C (`extern ... from "LLVM"`), which needs the LLVM import library at LINK time, and
# a Windows machine does not have one next to a Turf install (the archive's turfc embeds
# LLVM statically; it ships no libLLVM). It is the program that proves nothing on Windows
# about optimizer correctness and everything about whether LLVM is installed.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\opt_levels_smoke.ps1
# Exit:  0 = every level agrees; 1 = a program disagrees or fails to compile/run.

. "$PSScriptRoot\smoke_common.ps1"
Assert-Built
Start-Smoke 'opt_levels_smoke'

$levels = @('-O0', '-O1', '-O2', '-O3', '-Os')
$work = New-WorkDir
$sizes = @{}

# Compiles $src at $level; returns the compile result, and records the binary's size.
function Build-One([string]$Src, [string]$Level) {
  $base = [System.IO.Path]::GetFileNameWithoutExtension($Src)
  $out = Join-Path $work "${base}_$Level.exe"
  $c = Invoke-Turfc -ArgList @($Src, $Level, '-o', $out)
  if ($c.Rc -eq 0 -and (Test-Path -LiteralPath $out)) { $sizes["$base $Level"] = (Get-Item -LiteralPath $out).Length }
  $c | Add-Member -NotePropertyName Exe -NotePropertyValue $out -PassThru
}

function Write-CompileFailure([string]$Src, [string]$Level, $C) {
  Write-Fail "$Src does not compile at ${Level}:"
  Write-Indented ((($C.Out + $C.Err) -split "`r?`n" | Select-Object -First 5) -join "`n")
}

# --- Deterministic set: exact stdout + exit-code equality across all levels --------------
$deterministic = @(Get-ChildItem -Path 'tests\08_e2e_stress' -Filter '*.tr' -Recurse | Sort-Object FullName |
    ForEach-Object { $_.FullName.Substring($script:Root.Length + 1) })
$deterministic += 'showcase\llvm_ffi_selfhost_demo.tr'

Write-Heading '1. Deterministic set -- exact output + exit code must match -O0'
foreach ($src in $deterministic) {
  if ($src -like '*llvm_ffi_selfhost_demo.tr') {
    Write-Skip "$src (needs the LLVM import library at link time; see this script's header)"
    continue
  }
  $ref = Build-One $src '-O0'
  if ($ref.Rc -ne 0) { Write-CompileFailure $src '-O0' $ref; continue }
  $refRun = Invoke-Proc -File $ref.Exe -TimeoutSec 120
  $refOut = $refRun.Out + $refRun.Err
  $bad = $false
  foreach ($level in $levels[1..4]) {
    $b = Build-One $src $level
    if ($b.Rc -ne 0) { Write-CompileFailure $src $level $b; $bad = $true; continue }
    $run = Invoke-Proc -File $b.Exe -TimeoutSec 120
    if ($run.Rc -ne $refRun.Rc) {
      Write-Fail "${src}: exit code differs at $level (got $($run.Rc), -O0 was $($refRun.Rc))"
      $bad = $true
    } elseif (($run.Out + $run.Err) -cne $refOut) {
      Write-Fail "${src}: stdout differs at $level vs -O0:"
      $a = $refOut -split "`r?`n"; $c2 = ($run.Out + $run.Err) -split "`r?`n"
      $diff = Compare-Object $a $c2 | Select-Object -First 6 | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }
      Write-Indented ($diff -join "`n")
      $bad = $true
    }
  }
  if (-not $bad) { Write-Pass "$src identical at every level (exit $($refRun.Rc))" }
}

# --- Nondeterministic set: build at every level, exit code only ---------------------------
$nondeterministic = @('showcase\config_and_dice.tr', 'showcase\text_toolkit.tr')
Write-Host ''
Write-Heading '2. Nondeterministic set -- builds + exits 0 at every level (no stdout diff)'
foreach ($src in $nondeterministic) {
  $bad = $false
  foreach ($level in $levels) {
    $b = Build-One $src $level
    if ($b.Rc -ne 0) { Write-CompileFailure $src $level $b; $bad = $true; continue }
    $run = Invoke-Proc -File $b.Exe -TimeoutSec 10
    if ($run.TimedOut -or $run.Rc -ne 0) { Write-Fail "$src exits non-zero (or times out) at $level"; $bad = $true }
  }
  if (-not $bad) { Write-Pass "$src builds and exits 0 at every level" }
}

# --- Server/client set: compile-only -------------------------------------------------------
$serverClient = @('showcase\http_server.tr', 'showcase\http_client.tr', 'showcase\kv_store_server.tr', 'showcase\kv_store_client.tr')
Write-Host ''
Write-Heading '3. Server/client set -- compiles at every level (not run)'
foreach ($src in $serverClient) {
  $bad = $false
  foreach ($level in $levels) {
    $b = Build-One $src $level
    if ($b.Rc -ne 0) { Write-CompileFailure $src $level $b; $bad = $true }
  }
  if (-not $bad) { Write-Pass "$src compiles at every level" }
}

# --- Binary-size table --------------------------------------------------------------------
Write-Host ''
Write-Heading '4. Binary sizes by level (bytes)'
$programs = @($deterministic + $nondeterministic + $serverClient) |
    Where-Object { $_ -notlike '*llvm_ffi_selfhost_demo.tr' } |
    ForEach-Object { [System.IO.Path]::GetFileNameWithoutExtension($_) }
$header = '   {0,-28}' -f 'program'
foreach ($l in $levels) { $header += '{0,10}' -f $l }
Write-Host $header
foreach ($p in $programs) {
  $row = '   {0,-28}' -f $p
  foreach ($l in $levels) {
    $v = if ($sizes.ContainsKey("$p $l")) { $sizes["$p $l"] } else { '-' }
    $row += '{0,10}' -f $v
  }
  Write-Host $row
}

Complete-Smoke
