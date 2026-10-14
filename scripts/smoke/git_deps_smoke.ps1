# git_deps_smoke.ps1 -- the Windows twin of git_deps_smoke.sh (LEC-001 stage 6: git
# dependencies, the fetch cache and turf.lock; FREEZE.md 6.16, F-PKG-3: the root's turf.lock
# pins commit + SHA-256, verified on every build; one version per name; --offline, --locked;
# `turf update` is the only thing that moves a ref).
#
# The whole path, on programs whose answer comes from the dependency, so a wrong tree would
# change the exit status: `turf` -> system git -> the cache -> the lock -> the package graph
# -> `turfc`. Every upstream is a local bare repository this script builds, so there is no
# network. Every case checks the exit status and the message or lock line it is about.
#
# Differences from the bash script, each named:
#   * paths in turf's output and in the package graph are compared with the separators
#     normalised ("/"), because turf prints native paths and JSON escapes the backslashes;
#   * git's own configuration is isolated (GIT_CONFIG_GLOBAL=NUL, GIT_CONFIG_NOSYSTEM=1), so
#     Git for Windows' default core.autocrlf=true cannot rewrite line endings of the test data;
#   * "git not on PATH" empties PATH instead of building a decoy toolchain directory;
#   * the permission-bit half of the tree-hash check is skipped by name (Windows has no
#     execute bit); the modification-time half runs.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke\git_deps_smoke.ps1
# Exit:  0 = all checks pass; 1 = a check failed (each failure is printed).

. "$PSScriptRoot\smoke_common.ps1"
Assert-Built
if (-not (Test-Path -LiteralPath $script:Turf)) {
  if ($env:CI) { Write-Host 'FAIL: build\turf.exe is missing in CI' -ForegroundColor Red; exit 1 }
  Write-Host 'SKIP (no build\turf.exe: Go not installed?)'; exit 0
}
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if (-not $gitCmd) {
  if ($env:CI) { Write-Host 'FAIL: git is not installed in CI' -ForegroundColor Red; exit 1 }
  Write-Host 'SKIP (git is not installed)'; exit 0
}
Start-Smoke 'git_deps_smoke'

$work = New-WorkDir
$CacheDir = Join-Path $work 'cache'
# The cache is ours, never the developer's, and git must not read their configuration
# (signing, autocrlf, insteadOf) or need their identity.
$baseEnv = @{
  TURF_CACHE_DIR = $CacheDir
  GIT_CONFIG_GLOBAL = 'NUL'; GIT_CONFIG_NOSYSTEM = '1'; GIT_TERMINAL_PROMPT = '0'
  GIT_AUTHOR_NAME = 't'; GIT_AUTHOR_EMAIL = 't@t'; GIT_COMMITTER_NAME = 't'; GIT_COMMITTER_EMAIL = 't@t'
}
function Merge-Env([hashtable]$Extra) {
  $e = @{}; foreach ($k in $baseEnv.Keys) { $e[$k] = $baseEnv[$k] }
  if ($Extra) { foreach ($k in $Extra.Keys) { $e[$k] = $Extra[$k] } }
  return $e
}

# Slashes only, so a native path and a "/" path compare equal.
function Norm([string]$S) { return ($S -replace '\\\\', '/') -replace '\\', '/' }
function Slash([string]$P) { return $P -replace '\\', '/' }

# Tr <dir> args [env] -> result with .All (stdout+stderr, colours stripped).
function Tr([string]$Dir, [string[]]$A, [hashtable]$Extra = $null) {
  $r = Invoke-Turf -ArgList $A -Cwd $Dir -Env (Merge-Env $Extra)
  $r | Add-Member -NotePropertyName All -NotePropertyValue (Remove-Ansi ($r.Out + $r.Err)) -PassThru
}
function Expect-Rc([string]$Label, $R, [long]$Code) {
  if ($R.Rc -ne $Code) { Write-Fail "${Label}: exit $($R.Rc), expected $Code (got: $($R.All.Substring(0, [Math]::Min(400, $R.All.Length))))" }
}
function Expect-Out([string]$Label, $R, [string]$Text) {
  if (-not (Norm $R.All).Contains((Norm $Text))) { Write-Fail "${Label}: output lacks '$Text' (got: $($R.All.Substring(0, [Math]::Min(500, $R.All.Length))))" }
}
function Expect-NoOut([string]$Label, $R, [string]$Text) {
  if ((Norm $R.All).Contains((Norm $Text))) { Write-Fail "${Label}: output must not contain '$Text'" }
}
function Assert-SameFile([string]$Label, [string]$A, [string]$B) {
  if ([System.IO.File]::ReadAllText($A) -cne [System.IO.File]::ReadAllText($B)) { Write-Fail "${Label}: $A and $B differ" }
}
function Get-LockLines([string]$AppDir) {
  $p = Join-Path $AppDir 'turf.lock'
  if (Test-Path -LiteralPath $p) { return @(Get-Content -LiteralPath $p) } else { return @() }
}
# Lock-Has <label> <app dir> <line>: turf.lock holds exactly this line.
function Lock-Has([string]$Label, [string]$AppDir, [string]$Line) {
  $lines = Get-LockLines $AppDir
  if ($lines -cnotcontains $Line) {
    $has = ($lines -join "`n"); if ($has.Length -gt 700) { $has = $has.Substring(0, 700) }
    Write-Fail "${Label}: turf.lock lacks the line: $Line (has: $has)"
  }
}
function Lock-Lacks([string]$Label, [string]$AppDir, [string]$Line) {
  if ((Get-LockLines $AppDir) -ccontains $Line) { Write-Fail "${Label}: turf.lock must not contain: $Line" }
}
function New-Dir([string]$Path) { New-Item -ItemType Directory -Force -Path $Path | Out-Null }
function Set-Text([string]$Path, [string]$Text) { Write-TextFile $Path $Text }
function Short([string]$Id) { return $Id.Substring(0, 12) }

# --- git helpers ---------------------------------------------------------------------------------
function Git([string[]]$A, [string]$Cwd = $work) {
  $r = Invoke-Proc -File $gitCmd.Source -ArgList $A -Cwd $Cwd -Env (Merge-Env $null)
  return $r
}
$UpDir = "$work\up"; $ScDir = "$work\sc"
New-Dir $UpDir; New-Dir $ScDir
function Url([string]$Name) { return (Slash "$UpDir\$Name.git") }
function Mk-Up([string]$Name) {
  $null = Git @('init', '-q', '--bare', '-b', 'main', "$UpDir\$Name.git")
  $null = Git @('clone', '-q', "$UpDir\$Name.git", "$ScDir\$Name")
  $null = Git @('-C', "$ScDir\$Name", 'checkout', '-q', '-B', 'main')
}
function Git-Commit-Push([string]$Name, [string]$Msg) {
  $null = Git @('-C', "$ScDir\$Name", 'add', '-A')
  $null = Git @('-C', "$ScDir\$Name", 'commit', '-q', '-m', $Msg)
  $null = Git @('-C', "$ScDir\$Name", 'push', '-q', 'origin', 'main')
}
# Publish <name> <answer>: a library whose answer() returns <answer>, as a new commit on main.
function Publish([string]$Name, [int]$Answer) {
  New-Dir "$ScDir\$Name\src"
  Set-Text "$ScDir\$Name\turf.toml" "[package]`nname = `"$Name`"`nversion = `"1.0.0`"`n"
  Set-Text "$ScDir\$Name\src\$Name.tr" "pub fn answer(): int {`n    return $Answer;`n}`n"
  Git-Commit-Push $Name "answer $Answer"
}
function Head-Of([string]$Name) { return (Git @('-C', "$ScDir\$Name", 'rev-parse', 'HEAD')).Out.Trim() }
function Tag-It([string]$Name, [string]$Tag) {  # create or force-move a tag, and push it forcibly
  $null = Git @('-C', "$ScDir\$Name", 'tag', '-f', $Tag)
  $null = Git @('-C', "$ScDir\$Name", 'push', '-q', '--force', 'origin', "refs/tags/$Tag")
}
function Dep-Git([string]$Name, [string]$Repo, [string]$Ref) { return "$Name = { git = `"$(Url $Repo)`", $Ref }" }

# Mk-App <dir> <turf.toml [dependencies] lines> <modules imported and summed>: a program whose
# exit status is the sum of its dependencies' answers.
function Mk-App([string]$Dir, [string]$Deps, [string[]]$Mods) {
  New-Dir "$Dir\src"
  Set-Text "$Dir\turf.toml" "[package]`nname = `"app`"`nversion = `"0.1.0`"`n`n[dependencies]`n$Deps`n"
  $imports = ''; $sum = ''
  foreach ($m in $Mods) {
    $imports += "import $m;`n"
    $sum = if ($sum) { "$sum + ${m}::answer()" } else { "${m}::answer()" }
  }
  Set-Text "$Dir\src\main.tr" "$imports`nfn main(): int {`n    return $sum;`n}`n"
}
# Mk-PathLib <dir> <name> <deps lines> <modules imported> <expr>: a path library exposing v() = <expr>.
function Mk-PathLib([string]$Dir, [string]$Name, [string]$Deps, [string[]]$Mods, [string]$Expr) {
  New-Dir "$Dir\src"
  Set-Text "$Dir\turf.toml" "[package]`nname = `"$Name`"`nversion = `"0.1.0`"`n`n[dependencies]`n$Deps`n"
  $imports = ''
  foreach ($m in $Mods) { $imports += "import $m;`n" }
  Set-Text "$Dir\src\$Name.tr" "$imports`npub fn v(): int {`n    return $Expr;`n}`n"
}
function Mk-Diamond-App([string]$Dir) {
  New-Dir "$Dir\src"
  Set-Text "$Dir\turf.toml" "[package]`nname = `"app`"`nversion = `"0.1.0`"`n`n[dependencies]`nalpha = { path = `"../alpha`" }`nbeta = { path = `"../beta`" }`n"
  Set-Text "$Dir\src\main.tr" "import alpha;`nimport beta;`n`nfn main(): int {`n    return alpha::v() + beta::v();`n}`n"
}

# --- 1: a tag dependency builds, runs, and is pinned ---------------------------------------------------
Mk-Up 'jsonx'
Publish 'jsonx' 7
$C1 = Head-Of 'jsonx'
Tag-It 'jsonx' 'v1'
if ($C1.Length -ne 40) { Write-Fail "setup: commit id is not 40 hex ($C1)" }
$A1 = "$work\app1"
Mk-App $A1 (Dep-Git 'jsonx' 'jsonx' 'tag = "v1"') @('jsonx')
$r = Tr $A1 @('run')
Expect-Rc 'a tag dependency through turf run' $r 7
Expect-Out 'a tag dependency is fetched once' $r 'turf: fetching jsonx'
Lock-Has 'tag dependency' $A1 'version = 1'
Lock-Has 'tag dependency' $A1 'name = "jsonx"'
Lock-Has 'tag dependency' $A1 "source = `"git+$(Url 'jsonx')`""
Lock-Has 'tag dependency' $A1 'ref = "tag=v1"'
Lock-Has 'tag dependency records the FULL commit' $A1 "commit = `"$C1`""
if (@(Get-LockLines $A1 | Where-Object { $_ -match '^hash = "sha256:[0-9a-f]{64}"$' }).Count -eq 0) { Write-Fail 'tag dependency: hash is not sha256:<64 hex>' }
if (-not (Test-Path "$CacheDir\src\jsonx-$C1\src\jsonx.tr")) { Write-Fail 'tag dependency: the tree is not at cache/src/jsonx-<commit>' }
if (@(Get-ChildItem -Recurse -Force -Filter .git "$CacheDir\src" -ErrorAction SilentlyContinue).Count -ne 0) { Write-Fail 'an extracted tree must never contain .git' }
if (@(Get-ChildItem "$CacheDir\git").Count -ne 1) { Write-Fail 'one URL should have exactly one mirror' }
if (-not (Norm (Get-Content -Raw "$A1\build\package-graph.json")).Contains((Norm "$CacheDir\src\jsonx-$C1\src"))) { Write-Fail 'the package graph should point at the extracted tree' }

# --- 16: building twice leaves turf.lock byte-identical -----------------------------------------------
Copy-Item "$A1\turf.lock" "$work\lock1"
$r = Tr $A1 @('build'); Expect-Rc 'a second build' $r 0
Expect-NoOut 'a second build needs no fetch' $r 'turf: fetching'
Assert-SameFile 'building twice: turf.lock is byte-identical' "$work\lock1" "$A1\turf.lock"
$r = Tr $A1 @('build', '--locked'); Expect-Rc '--locked with a correct lock' $r 0
Assert-SameFile '--locked writes nothing' "$work\lock1" "$A1\turf.lock"

# --- 2: the upstream is gone; the cache and the lock are enough --------------------------------------------
Move-Item "$UpDir\jsonx.git" "$UpDir\jsonx.git.away"
$r = Tr $A1 @('build'); Expect-Rc 'build with the upstream gone (cache)' $r 0
$r = Tr $A1 @('build', '--offline'); Expect-Rc 'build --offline with the upstream gone' $r 0
$r = Tr $A1 @('run', '--offline'); Expect-Rc 'run --offline' $r 7
Move-Item "$UpDir\jsonx.git.away" "$UpDir\jsonx.git"

# --- hash: content counts, mtimes and permission bits do not -----------------------------------------------
Get-ChildItem -Recurse -File "$CacheDir\src\jsonx-$C1" | ForEach-Object { $_.LastWriteTime = [datetime]'2000-01-01T00:00:00' }
$r = Tr $A1 @('build'); Expect-Rc 'mtimes do not change the tree hash' $r 0
Write-Skip 'permission bits do not change the tree hash (Windows has no execute bit to flip; the modification-time half ran above)'

# --- 3: an edited cached file is detected on the next build -----------------------------------------------
[System.IO.File]::AppendAllText("$CacheDir\src\jsonx-$C1\src\jsonx.tr", "// tampered`n")
$r = Tr $A1 @('build'); Expect-Rc 'an edited cached file' $r 1
Expect-Out 'an edited cached file' $r 'does not match turf.lock'
Expect-Out 'an edited cached file names the cache path' $r "$CacheDir\src\jsonx-$C1"
Expect-Out 'an edited cached file says how to recover' $r "delete $CacheDir\src\jsonx-$C1 and build again"
$r = Tr $A1 @('build', '--offline'); Expect-Rc 'an edited cached file, --offline' $r 1
Expect-Out 'an edited cached file, --offline' $r 'does not match turf.lock'
$r = Tr $A1 @('fetch'); Expect-Rc 'an edited cached file, turf fetch' $r 1
# The advice in the message works, with no network.
Remove-Item -Recurse -Force "$CacheDir\src\jsonx-$C1"
$r = Tr $A1 @('build', '--offline'); Expect-Rc 'deleting the tree re-extracts it from the mirror' $r 0
# An added file counts as well as an edit.
[System.IO.File]::WriteAllText("$CacheDir\src\jsonx-$C1\extra.tr", '')
$r = Tr $A1 @('build'); Expect-Rc 'an extra file in the cached tree' $r 1
Expect-Out 'an extra file in the cached tree' $r 'does not match turf.lock'
Remove-Item -Recurse -Force "$CacheDir\src\jsonx-$C1"
$r = Tr $A1 @('build'); Expect-Rc 'recovery after an extra file' $r 0

# --- 4: the manifest changed; --locked refuses and writes nothing ----------------------------------------------
Publish 'jsonx' 8
$C2 = Head-Of 'jsonx'
Tag-It 'jsonx' 'v2'
Copy-Item "$A1\turf.lock" "$work\lock4"
Set-Text "$A1\turf.toml" ([System.IO.File]::ReadAllText("$A1\turf.toml").Replace('tag = "v1"', 'tag = "v2"'))
$r = Tr $A1 @('build', '--locked')
Expect-Rc '--locked with a changed manifest' $r 1
Expect-Out '--locked names the changed entry' $r "changed 'jsonx'"
Expect-Out '--locked names what changed' $r 'ref tag=v1 -> tag=v2'
Expect-Out '--locked says nothing was written' $r 'nothing was written'
Assert-SameFile '--locked leaves turf.lock alone' "$work\lock4" "$A1\turf.lock"
$r = Tr $A1 @('build'); Expect-Rc 'the same change without --locked' $r 0
Lock-Has 'a changed manifest ref is re-resolved' $A1 "commit = `"$C2`""
Lock-Has 'a changed manifest ref is re-resolved' $A1 'ref = "tag=v2"'
$r = Tr $A1 @('run'); Expect-Rc "the new tag's code runs" $r 8

# --- 5: --locked needs a lock ----------------------------------------------------------------------------------
$A5 = "$work\app5"
Mk-App $A5 (Dep-Git 'jsonx' 'jsonx' 'tag = "v1"') @('jsonx')
$r = Tr $A5 @('build', '--locked')
Expect-Rc '--locked with no turf.lock' $r 1
Expect-Out '--locked with no turf.lock' $r '--locked needs turf.lock, and there is none'
if (Test-Path "$A5\turf.lock") { Write-Fail '--locked must not create turf.lock' }
$r = Tr $A5 @('fetch', '--locked'); Expect-Rc 'turf fetch --locked with no turf.lock' $r 1

# --- 6: a branch dependency: build keeps the commit, only update moves it -----------------------------------------
Mk-Up 'brnch'
Publish 'brnch' 1
$B1 = Head-Of 'brnch'
$AB = "$work\appb"
Mk-App $AB (Dep-Git 'brnch' 'brnch' 'branch = "main"') @('brnch')
$r = Tr $AB @('run'); Expect-Rc 'a branch dependency' $r 1
Lock-Has 'a branch dependency' $AB 'ref = "branch=main"'
Lock-Has 'a branch dependency' $AB "commit = `"$B1`""
Publish 'brnch' 2
$B2 = Head-Of 'brnch'
$r = Tr $AB @('run'); Expect-Rc 'a new commit on the branch: build keeps the locked one' $r 1
Lock-Has 'build keeps the old branch commit' $AB "commit = `"$B1`""
Expect-NoOut 'build does not touch the network for a locked branch' $r 'turf: fetching'
$r = Tr $AB @('update'); Expect-Rc 'turf update on a moved branch' $r 0
Expect-Out 'turf update prints old -> new' $r "brnch $(Short $B1) -> $(Short $B2)"
Lock-Has 'turf update writes the new commit' $AB "commit = `"$B2`""
$r = Tr $AB @('run'); Expect-Rc 'after update the new commit runs' $r 2
$r = Tr $AB @('update'); Expect-Rc 'update with nothing to move' $r 0; Expect-Out 'update with nothing to move' $r 'brnch unchanged'
$r = Tr $AB @('update', 'brnch'); Expect-Out 'update by name' $r 'brnch unchanged'

# --- 7: a force-moved tag: build keeps the commit, update moves it ------------------------------------------------
Mk-Up 'movtag'
Publish 'movtag' 10
$M1 = Head-Of 'movtag'
Tag-It 'movtag' 'v1'
$AM = "$work\appm"
Mk-App $AM (Dep-Git 'movtag' 'movtag' 'tag = "v1"') @('movtag')
$r = Tr $AM @('run'); Expect-Rc 'a tag dependency (before the tag moves)' $r 10
Publish 'movtag' 11
$M2 = Head-Of 'movtag'
Tag-It 'movtag' 'v1'
$r = Tr $AM @('run'); Expect-Rc 'a force-moved tag: build keeps the locked commit' $r 10
Lock-Has 'a force-moved tag: build keeps the locked commit' $AM "commit = `"$M1`""
$r = Tr $AM @('update', 'movtag'); Expect-Out 'update moves a force-moved tag' $r "movtag $(Short $M1) -> $(Short $M2)"
$r = Tr $AM @('run'); Expect-Rc "the moved tag's code runs after update" $r 11

# --- 8: an abbreviated rev is recorded in full ----------------------------------------------------------------------
Mk-Up 'revrepo'
Publish 'revrepo' 5
$R1 = Head-Of 'revrepo'
$AR = "$work\appr"
Mk-App $AR (Dep-Git 'revrepo' 'revrepo' "rev = `"$($R1.Substring(0, 7))`"") @('revrepo')
$r = Tr $AR @('run'); Expect-Rc 'an abbreviated rev' $r 5
Lock-Has 'an abbreviated rev: the lock records the full commit' $AR "commit = `"$R1`""
Lock-Has 'an abbreviated rev: the ref is what the manifest asked' $AR "ref = `"rev=$($R1.Substring(0, 7))`""
Publish 'revrepo' 6
$r = Tr $AR @('update'); Expect-Out 'update of a rev changes nothing' $r 'revrepo unchanged'

# --- an annotated tag resolves to its commit, not the tag object -----------------------------------------------------
Mk-Up 'annot'
Publish 'annot' 4
$N1 = Head-Of 'annot'
$null = Git @('-C', "$ScDir\annot", 'tag', '-a', 'v1', '-m', 'annotated')
$null = Git @('-C', "$ScDir\annot", 'push', '-q', 'origin', 'refs/tags/v1')
$AN = "$work\appn"
Mk-App $AN (Dep-Git 'annot' 'annot' 'tag = "v1"') @('annot')
$r = Tr $AN @('run'); Expect-Rc 'an annotated tag' $r 4
Lock-Has 'an annotated tag: the lock holds the commit, not the tag object' $AN "commit = `"$N1`""

# --- a dependency's own turf.lock is ignored (F-PKG-3: the root's is the only one) -------------------------------------
Mk-Up 'lockrepo'
Set-Text "$ScDir\lockrepo\turf.lock" "this is not a lockfile`n"
Publish 'lockrepo' 9
Tag-It 'lockrepo' 'v1'
$AL = "$work\appl"
Mk-App $AL (Dep-Git 'lockrepo' 'lockrepo' 'tag = "v1"') @('lockrepo')
$r = Tr $AL @('run'); Expect-Rc 'a dependency with its own (garbage) turf.lock' $r 9

# --- 9: a diamond on one URL and tag: one tree ----------------------------------------------------------------------------
Mk-Up 'shared'
Publish 'shared' 3
$S1 = Head-Of 'shared'
Tag-It 'shared' 'v1'
$D9 = "$work\d9"
Mk-PathLib "$D9\alpha" 'alpha' (Dep-Git 'shared' 'shared' 'tag = "v1"') @('shared') 'shared::answer()'
Mk-PathLib "$D9\beta" 'beta' (Dep-Git 'shared' 'shared' 'tag = "v1"') @('shared') 'shared::answer() + 10'
Mk-Diamond-App "$D9\app"
$r9 = Tr "$D9\app" @('run'); Expect-Rc 'a diamond on one URL and tag' $r9 16
if (@(Get-ChildItem -Directory "$CacheDir\src" -Filter 'shared-*').Count -ne 1) { Write-Fail 'a diamond must extract one tree' }
if (@(Get-LockLines "$D9\app" | Where-Object { $_ -ceq 'name = "shared"' }).Count -ne 1) { Write-Fail 'a diamond must be one lock entry' }
if (@(($r9.All -split "`r?`n") | Where-Object { $_ -like '*turf: fetching shared*' }).Count -ne 1) { Write-Fail 'a diamond must fetch once' }
Lock-Has 'path packages are in the lock too' "$D9\app" 'source = "path+../alpha"'
Lock-Has 'path packages are in the lock too' "$D9\app" 'source = "path+../beta"'

# --- 10: the same URL at two tags with different commits ------------------------------------------------------------------------
Publish 'shared' 4
$S2 = Head-Of 'shared'
Tag-It 'shared' 'v2'
$D10 = "$work\d10"
Mk-PathLib "$D10\alpha" 'alpha' (Dep-Git 'shared' 'shared' 'tag = "v1"') @('shared') 'shared::answer()'
Mk-PathLib "$D10\beta" 'beta' (Dep-Git 'shared' 'shared' 'tag = "v2"') @('shared') 'shared::answer()'
Mk-Diamond-App "$D10\app"
$r = Tr "$D10\app" @('build')
Expect-Rc 'a diamond conflict on two tags' $r 1
Expect-Out 'a diamond conflict' $r "package 'shared' is required at two different versions of git"
Expect-Out 'a diamond conflict names the first requirer' $r "package 'alpha'"
Expect-Out 'a diamond conflict names the second requirer' $r "package 'beta'"
Expect-Out 'a diamond conflict names the first ref' $r 'tag=v1'
Expect-Out 'a diamond conflict names the second ref' $r 'tag=v2'
Expect-Out 'a diamond conflict names both commits' $r "commit $(Short $S1)"
Expect-Out 'a diamond conflict names both commits' $r "commit $(Short $S2)"
if (Test-Path "$D10\app\turf.lock") { Write-Fail 'a failed resolution must not write turf.lock' }

# Two spellings of one commit are not a conflict.
$D10b = "$work\d10b"
Mk-PathLib "$D10b\alpha" 'alpha' (Dep-Git 'shared' 'shared' 'tag = "v1"') @('shared') 'shared::answer()'
Mk-PathLib "$D10b\beta" 'beta' (Dep-Git 'shared' 'shared' "rev = `"$($S1.Substring(0, 10))`"") @('shared') 'shared::answer()'
Mk-Diamond-App "$D10b\app"
$r = Tr "$D10b\app" @('run'); Expect-Rc 'a tag and a rev naming one commit agree' $r 6

# --- 11: one name as a path dependency and as a git dependency -----------------------------------------------------------------
$D11 = "$work\d11"
Mk-PathLib "$D11\alpha" 'alpha' (Dep-Git 'shared' 'shared' 'tag = "v1"') @('shared') 'shared::answer()'
Mk-PathLib "$D11\beta" 'beta' 'shared = { path = "../shared" }' @('shared') 'shared::answer()'
New-Dir "$D11\shared\src"
Set-Text "$D11\shared\turf.toml" "[package]`nname = `"shared`"`nversion = `"0.1.0`"`n"
Set-Text "$D11\shared\src\shared.tr" "pub fn answer(): int {`n    return 99;`n}`n"
Mk-Diamond-App "$D11\app"
$r = Tr "$D11\app" @('build')
Expect-Rc 'one name as a git and a path dependency' $r 1
Expect-Out 'one name as a git and a path dependency' $r "package 'shared' is required as both a git dependency and a path dependency"
Expect-Out 'one name as a git and a path dependency names the git source' $r "git $(Url 'shared') (tag=v1"
Expect-Out 'one name as a git and a path dependency names the directory' $r "$D11\shared"

# --- 12: --offline with an empty cache -----------------------------------------------------------------------------------------------
$emptyCache = "$work\emptycache"
$r = Tr $A1 @('build', '--offline') @{ TURF_CACHE_DIR = $emptyCache }
Expect-Rc '--offline with an empty cache' $r 1
Expect-Out '--offline with an empty cache' $r 'needs the network; run without --offline'
Expect-Out '--offline names the dependency' $r "'jsonx'"
if (Test-Path "$emptyCache\git") { Write-Fail '--offline must not clone anything' }
$r = Tr $A1 @('fetch', '--offline') @{ TURF_CACHE_DIR = $emptyCache }
Expect-Rc 'turf fetch --offline with an empty cache' $r 1
Expect-Out 'turf fetch --offline with an empty cache' $r 'needs the network; run without --offline'
# A dependency that is not locked is refused offline even though the cache holds a mirror of its URL.
$A12 = "$work\app12"
Mk-App $A12 (Dep-Git 'jsonx' 'jsonx' 'branch = "main"') @('jsonx')
$r = Tr $A12 @('build', '--offline')
Expect-Rc '--offline with a dependency that is not locked' $r 1
Expect-Out '--offline with a dependency that is not locked' $r 'is not in turf.lock'
Expect-Out '--offline with a dependency that is not locked' $r 'needs the network; run without --offline'
if (Test-Path "$A12\turf.lock") { Write-Fail 'a refused --offline build must not write turf.lock' }

# --- 13: git not on PATH ------------------------------------------------------------------------------------------------------------------
$noGit = "$work\nogit-bin"; New-Dir $noGit
$r = Tr $A1 @('build') @{ PATH = $noGit }
Expect-Rc 'git not on PATH' $r 1
Expect-Out 'git not on PATH' $r "git dependencies need the 'git' program, which is not on PATH ('jsonx' in "
Expect-Out 'git not on PATH names the manifest' $r "$A1\turf.toml"
# A project with no git dependency does not care.
$A13 = "$work\app13"
New-Dir "$A13\src"
Set-Text "$A13\turf.toml" "[package]`nname = `"app`"`nversion = `"0.1.0`"`n"
Set-Text "$A13\src\main.tr" "fn main(): int {`n    return 3;`n}`n"
$r = Tr $A13 @('run') @{ PATH = $noGit }
Expect-Rc 'a project without git dependencies needs no git' $r 3

# --- 14: a git package's path dependencies must stay inside its repository ----------------------------------------------------------------------
Mk-Up 'mono'
New-Dir "$ScDir\mono\src"
Publish 'mono' 1
Set-Text "$ScDir\mono\turf.toml" "[package]`nname = `"mono`"`nversion = `"1.0.0`"`n`n[dependencies]`nsib = { path = `"../sib`" }`n"
Git-Commit-Push 'mono' 'depends outside'
Tag-It 'mono' 'v1'
$AE = "$work\appe"
Mk-App $AE (Dep-Git 'mono' 'mono' 'tag = "v1"') @('mono')
$r = Tr $AE @('build')
Expect-Rc "a git package's path dependency that escapes its repository" $r 1
Expect-Out "a git package's path dependency that escapes its repository" $r "a git dependency's path dependencies must stay inside its repository"
Expect-Out 'the escape names the dependency' $r "dependency 'sib'"
if (Test-Path "$AE\turf.lock") { Write-Fail 'a failed resolution must not write turf.lock' }
# ...and one that stays inside works, and is covered by its parent's tree hash.
Mk-Up 'mono2'
New-Dir "$ScDir\mono2\packages\inner\src"
Set-Text "$ScDir\mono2\packages\inner\turf.toml" "[package]`nname = `"inner`"`nversion = `"1.0.0`"`n"
Set-Text "$ScDir\mono2\packages\inner\src\inner.tr" "pub fn answer(): int {`n    return 12;`n}`n"
New-Dir "$ScDir\mono2\src"
Set-Text "$ScDir\mono2\turf.toml" "[package]`nname = `"mono2`"`nversion = `"1.0.0`"`n`n[dependencies]`ninner = { path = `"packages/inner`" }`n"
Set-Text "$ScDir\mono2\src\mono2.tr" "import inner;`n`npub fn answer(): int {`n    return inner::answer();`n}`n"
Git-Commit-Push 'mono2' 'monorepo'
Tag-It 'mono2' 'v1'
$AI = "$work\appi"
Mk-App $AI (Dep-Git 'mono2' 'mono2' 'tag = "v1"') @('mono2')
$r = Tr $AI @('run'); Expect-Rc 'a path dependency inside a git repository' $r 12
Lock-Has 'the git package is in the lock' $AI 'name = "mono2"'
Lock-Lacks 'a path dependency inside a git tree has no lock entry of its own' $AI 'name = "inner"'

# --- 15: turf add --git ---------------------------------------------------------------------------------------------------------------------------
Mk-Up 'addrepo'
Publish 'addrepo' 21
Tag-It 'addrepo' 'v1'
$AA = "$work\appa"
Mk-App $AA '' @()
Set-Text "$AA\src\main.tr" "fn main(): int {`n    return 0;`n}`n"
Copy-Item "$AA\turf.toml" "$work\toml_a0"
$addUrl = Url 'addrepo'
$r = Tr $AA @('add', 'addrepo', '--git', $addUrl)
Expect-Rc 'turf add --git with no ref' $r 2
Expect-Out 'turf add --git with no ref' $r 'needs exactly one of --tag, --branch or --rev'
Assert-SameFile 'a refused add writes nothing' "$work\toml_a0" "$AA\turf.toml"
$r = Tr $AA @('add', 'addrepo', '--git', $addUrl, '--tag', 'v1', '--branch', 'main'); Expect-Rc 'turf add --git with two refs' $r 2
$r = Tr $AA @('add', 'addrepo', '--tag', 'v1'); Expect-Rc 'turf add --tag without --git' $r 2; Expect-Out 'turf add --tag without --git' $r 'only apply with --git'
$r = Tr $AA @('add', 'addrepo', '--git', $addUrl, '--tag', 'v1', '--offline'); Expect-Rc 'turf add --git --offline' $r 2
$r = Tr $AA @('add', 'addrepo', '--git', $addUrl, '--tag', 'nope')
Expect-Rc 'turf add --git of a tag that does not exist' $r 1
Expect-Out 'turf add --git of a tag that does not exist' $r "has no tag named 'nope'"
Assert-SameFile 'a failed add writes nothing' "$work\toml_a0" "$AA\turf.toml"
$r = Tr $AA @('add', 'other', '--git', $addUrl, '--tag', 'v1')
Expect-Rc "turf add --git with a key that is not the package's name" $r 1
Expect-Out "turf add --git with a key that is not the package's name" $r 'the key must be its package name'
Assert-SameFile 'a refused add writes nothing (wrong name)' "$work\toml_a0" "$AA\turf.toml"
$r = Tr $AA @('add', 'addrepo', '--git', $addUrl, '--tag', 'v1'); Expect-Rc 'turf add --git --tag' $r 0
if (@(Get-Content "$AA\turf.toml") -cnotcontains "addrepo = { git = `"$addUrl`", tag = `"v1`" }") { Write-Fail 'turf add --git: the dependency line was not written' }
if ((@(Get-Content "$AA\turf.toml").Count - @(Get-Content "$work\toml_a0").Count) -ne 1) { Write-Fail 'turf add --git changed more than one line' }
Set-Text "$AA\src\main.tr" "import addrepo;`n`nfn main(): int {`n    return addrepo::answer();`n}`n"
$r = Tr $AA @('run'); Expect-Rc 'the build works after turf add --git' $r 21
Lock-Has 'the build after add pins the dependency' $AA "commit = `"$(Head-Of 'addrepo')`""
# A repository that is not a package is refused before anything is written.
Mk-Up 'plain'
Set-Text "$ScDir\plain\README" "hello`n"
Git-Commit-Push 'plain' 'no manifest'
Copy-Item "$AA\turf.toml" "$work\toml_a1"
$r = Tr $AA @('add', 'plain', '--git', (Url 'plain'), '--branch', 'main')
Expect-Rc 'turf add --git of a repository with no turf.toml' $r 1
Expect-Out 'turf add --git of a repository with no turf.toml' $r 'has no turf.toml at its root'
Assert-SameFile 'a refused add writes nothing (not a package)' "$work\toml_a1" "$AA\turf.toml"

# --- 17: removing a dependency drops its lock entry -----------------------------------------------------------------------------------------------------
$AD = "$work\appd"
Mk-App $AD ((Dep-Git 'jsonx' 'jsonx' 'tag = "v1"') + "`n" + (Dep-Git 'brnch' 'brnch' 'branch = "main"')) @('jsonx', 'brnch')
$r = Tr $AD @('build'); Expect-Rc 'two git dependencies' $r 0
Lock-Has 'two git dependencies' $AD 'name = "brnch"'
Lock-Has 'two git dependencies' $AD 'name = "jsonx"'
$r = Tr $AD @('remove', 'brnch'); Expect-Rc 'turf remove of a git dependency' $r 0
Set-Text "$AD\src\main.tr" "import jsonx;`n`nfn main(): int {`n    return jsonx::answer();`n}`n"
$r = Tr $AD @('build'); Expect-Rc 'build after removing a dependency' $r 0
Lock-Lacks 'removing a dependency drops its lock entry' $AD 'name = "brnch"'
Lock-Has 'the other dependency stays' $AD 'name = "jsonx"'

# --- 18: turf fetch ---------------------------------------------------------------------------------------------------------------------------------------------
$AF = "$work\appf"
Mk-App $AF (Dep-Git 'jsonx' 'jsonx' 'tag = "v1"') @('jsonx')
$r = Tr $AF @('fetch') @{ TURF_COMPILER = 'C:\nonexistent\turfc.exe' }
Expect-Rc 'turf fetch (it needs no compiler)' $r 0
Expect-Out 'turf fetch prints one line per git dependency' $r "jsonx $(Short $C1) (tag=v1)"
Lock-Has 'turf fetch writes the lock' $AF "commit = `"$C1`""
if (Test-Path "$AF\build") { Write-Fail 'turf fetch must compile nothing (no build/ directory)' }
$r = Tr $AF @('fetch', '--locked'); Expect-Rc 'turf fetch --locked with a correct lock' $r 0
$r = Tr $AF @('fetch', 'jsonx'); Expect-Rc 'turf fetch takes no names' $r 2
$r = Tr $AF @('build'); Expect-Rc 'build after fetch' $r 0
if (-not (Test-Path "$AF\build\app.exe")) { Write-Fail 'build after fetch: no executable' }

# --- a project with no git dependency still gets a lock ------------------------------------------------------------------------------------------------------
$A19 = "$work\app19"; New-Dir $A19
$null = Tr $A19 @('new', 'plainapp')
$r = Tr "$A19\plainapp" @('build'); Expect-Rc 'a path-only project builds' $r 0
Lock-Has "a path-only project's lock" "$A19\plainapp" 'version = 1'
if (@(Get-LockLines "$A19\plainapp" | Where-Object { $_ -ceq '[[package]]' }).Count -ne 0) { Write-Fail 'a project with no dependencies has an empty lock' }
Lock-Has 'the lock says it is generated' "$A19\plainapp" '# This file is generated by turf. Commit it; do not edit it.'

# --- unknown ref, malformed lock, update's refusals ----------------------------------------------------------------------------------------------------------
$AU = "$work\appu"
Mk-App $AU (Dep-Git 'jsonx' 'jsonx' 'tag = "nope"') @('jsonx')
$r = Tr $AU @('build')
Expect-Rc 'a tag that does not exist' $r 1
Expect-Out 'a tag that does not exist names the URL' $r "$(Url 'jsonx') has no tag named 'nope'"
Expect-Out 'a tag that does not exist names the dependency' $r "dependency 'jsonx'"
$AV = "$work\appv"
Mk-App $AV (Dep-Git 'jsonx' 'jsonx' 'tag = "v1"') @('jsonx')
Set-Text "$AV\turf.lock" "version = 9`n"
$r = Tr $AV @('build')
Expect-Rc 'a lock with an unknown version' $r 1
Expect-Out 'a lock with an unknown version names the file' $r 'turf.lock'
Expect-Out 'a lock with an unknown version' $r 'version 9 is not supported'
Expect-Out 'a lock with an unknown version says how to fix it' $r 'delete it and run `turf build` to regenerate it'
Set-Text "$AV\turf.lock" "version = 1`nfrobnicate = true`n"
$r = Tr $AV @('build'); Expect-Rc 'a lock with an unknown key' $r 1; Expect-Out 'a lock with an unknown key' $r 'unknown key `frobnicate`'
Remove-Item -Force "$AV\turf.lock"

$r = Tr "$D9\app" @('update', 'alpha')
Expect-Rc 'turf update of a path dependency' $r 1
Expect-Out 'turf update of a path dependency' $r "'alpha' is a path dependency; it is always used as it is on disk"
$r = Tr $AB @('update', 'nosuchpkg')
Expect-Rc 'turf update of an unknown name' $r 1
Expect-Out 'turf update of an unknown name' $r "'nosuchpkg' is not a package in this project's dependency graph"
$r = Tr $AB @('update', '--offline'); Expect-Rc 'turf update --offline' $r 2
$r = Tr "$A19\plainapp" @('update')
Expect-Rc 'turf update in a project with no git dependencies' $r 0
Expect-Out 'turf update in a project with no git dependencies' $r 'no git dependencies to update'

Complete-Smoke
