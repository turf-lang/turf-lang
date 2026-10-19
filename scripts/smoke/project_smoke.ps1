# project_smoke.ps1 -- the Windows twin of project_smoke.sh (LEC-001 stage 5: `turf` reads
# projects -- turf.toml, path dependencies, the package graph handed to
# `turfc --package-graph`). Covers new/init/add/remove/build/run/check/clean against the
# real compiler, the manifest rules, name conflicts and cycles, [unsafe] allow, and the
# lexer/CLI reserved-word drift check -- the same cases as the bash script, in the same
# order, against the same expectations.
#
# Differences, each named: a built program is build\<name>.exe; the temp directory
# counter replaces $RANDOM; the executable-bit check becomes "the .exe exists".
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke\project_smoke.ps1
# Exit:  0 = all checks pass; 1 = a check failed (each failure is printed).

. "$PSScriptRoot\smoke_common.ps1"
Assert-Built
if (-not (Test-Path -LiteralPath $script:Turf)) {
  if ($env:CI) { Write-Host 'FAIL: build\turf.exe is missing in CI (was Go installed before the build?)' -ForegroundColor Red; exit 1 }
  Write-Host 'SKIP (no build\turf.exe: Go not installed?)'; exit 0
}
Start-Smoke 'project_smoke'

$Ver = (Get-Content -LiteralPath (Join-Path $script:Root 'VERSION') -TotalCount 1).Trim()
$work = New-WorkDir
# Git dependencies fetch into a cache; never the developer's own.
$envs = @{ TURF_CACHE_DIR = (Join-Path $work 'cache') }
$counter = 0

# Tr <dir> args... -> result with .All (stdout+stderr, colours stripped) and .Rc.
function Tr([string]$Dir, [string[]]$A) {
  $r = Invoke-Turf -ArgList $A -Cwd $Dir -Env $envs
  $r | Add-Member -NotePropertyName All -NotePropertyValue (Remove-Ansi ($r.Out + $r.Err)) -PassThru
}
function Expect-Rc([string]$Label, $R, [long]$Code) {
  if ($R.Rc -ne $Code) { Write-Fail "${Label}: exit $($R.Rc), expected $Code (got: $($R.All.Substring(0, [Math]::Min(300, $R.All.Length))))" }
}
function Expect-Out([string]$Label, $R, [string]$Text) {
  if (-not $R.All.Contains($Text)) { Write-Fail "${Label}: output lacks '$Text' (got: $($R.All.Substring(0, [Math]::Min(400, $R.All.Length))))" }
}
function Expect-NoOut([string]$Label, $R, [string]$Text) {
  if ($R.All.Contains($Text)) { Write-Fail "${Label}: output must not contain '$Text'" }
}
function Assert-SameFile([string]$Label, [string]$A, [string]$B) {
  if ([System.IO.File]::ReadAllText($A) -cne [System.IO.File]::ReadAllText($B)) { Write-Fail "${Label}: $A and $B differ" }
}
function Set-Text([string]$Path, [string]$Text) { Write-TextFile $Path $Text }

# Mk-Lib <dir> <name> <root module source>: a library package by hand, so the module bodies are
# ours; the manifest goes through `turf new` where it can.
function Mk-Lib([string]$Dir, [string]$Name, [string]$Source) {
  $r = Tr $Dir @('new', $Name, '--lib')
  if ($r.Rc -ne 0) { Write-Fail "setup: turf new $Name --lib" }
  Set-Text "$Dir\$Name\src\$Name.tr" ($Source + "`n")
}
function New-Dir([string]$Path) { New-Item -ItemType Directory -Force -Path $Path | Out-Null }

# --- new, run, build ------------------------------------------------------------------------
$P1 = "$work\p1"; New-Dir $P1
$r = Tr $P1 @('new', 'app'); Expect-Rc 'turf new app' $r 0
if (-not ((Test-Path "$P1\app\turf.toml") -and (Test-Path "$P1\app\src\main.tr") -and (Test-Path "$P1\app\.gitignore"))) {
  Write-Fail 'turf new app: turf.toml, src/main.tr and .gitignore should all exist'
}
if (@(Get-Content "$P1\app\.gitignore") -notcontains '/build/') { Write-Fail 'turf new app: .gitignore lacks /build/' }
$majorMinor = ($Ver -split '\.')[0..1] -join '.'
if (@(Get-Content "$P1\app\turf.toml") -notcontains "turf = `"$majorMinor`"") { Write-Fail "turf new app: turf key should be the compiler's major.minor ($Ver)" }

$r = Tr "$P1\app" @('run'); Expect-Rc 'turf run in a new project' $r 0
Expect-Out 'turf run in a new project' $r 'Hello, TurfLang! This is app.'
if (-not ((Test-Path "$P1\app\README.md") -and (Get-Content -Raw "$P1\app\README.md").Contains('`turf run`'))) {
  Write-Fail 'turf new app: README.md with the everyday commands was not created'
}
if (-not (Test-Path "$P1\app\build\app.exe")) { Write-Fail 'turf run in a new project: build\app.exe was not left behind' }
if (-not (Test-Path "$P1\app\build\package-graph.json")) { Write-Fail 'turf run: build\package-graph.json was not written' }
# From a subdirectory: found by walking up.
$r = Tr "$P1\app\src" @('run'); Expect-Rc 'turf run from src/' $r 0

$r = Tr $P1 @('new', 'app'); Expect-Rc 'turf new over an existing directory' $r 1
Expect-Out 'turf new over an existing directory' $r 'already exists'

$r = Tr $P1 @('new', 'geometry', '--lib'); Expect-Rc 'turf new geometry --lib' $r 0
if (-not ((Test-Path "$P1\geometry\src\geometry.tr") -and (Test-Path "$P1\geometry\src\main.tr"))) { Write-Fail 'turf new --lib: both src/geometry.tr and src/main.tr should exist' }
$r = Tr "$P1\geometry" @('build'); Expect-Rc 'turf build in a library' $r 0
if (-not (Test-Path "$P1\geometry\build\package-graph.json")) { Write-Fail 'library build: no graph file' }
$r = Tr "$P1\geometry" @('run'); Expect-Rc 'turf run in a new library' $r 0
Expect-Out 'turf run in a new library' $r 'geometry::answer() is 42.'
# Without src/main.tr there is nothing to run, and turf says what to create.
Move-Item "$P1\geometry\src\main.tr" "$P1\geometry\main.tr.aside"
$r = Tr "$P1\geometry" @('build'); Expect-Rc 'turf build in a library with no main.tr' $r 0
Remove-Item -Force -ErrorAction SilentlyContinue "$P1\geometry\build\geometry.exe"
$r = Tr "$P1\geometry" @('run'); Expect-Rc 'turf run with no src/main.tr' $r 1
Expect-Out 'turf run with no src/main.tr' $r 'has no src/main.tr, so there is nothing to run'
Move-Item "$P1\geometry\main.tr.aside" "$P1\geometry\src\main.tr"

# A library with an error: checked even though nothing imports it.
Set-Text "$P1\geometry\src\geometry.tr" "pub fn broken(): int {`n    return nope;`n}`n"
$r = Tr "$P1\geometry" @('build'); Expect-Rc 'library errors surface' $r 1
Expect-Out 'library errors surface' $r 'nope'
Expect-Out 'library errors keep their code' $r '[E040'
Set-Text "$P1\geometry\src\geometry.tr" "pub fn answer(): int {`n    return 42;`n}`n"

# --- add, and the library's value through the CLI --------------------------------------------
$r = Tr "$P1\app" @('add', 'geometry', '--path', '../geometry'); Expect-Rc 'turf add' $r 0
if (@(Get-Content "$P1\app\turf.toml") -cnotcontains 'geometry = { path = "../geometry" }') { Write-Fail 'turf add: dependency line not written' }
Set-Text "$P1\app\src\main.tr" "import geometry;`n`nfn main(): int {`n    return geometry::answer();`n}`n"
$r = Tr "$P1\app" @('run'); Expect-Rc "a dependency's function through turf run" $r 42

$r = Tr "$P1\app" @('build', '-o', 'x'); Expect-Rc 'turf build -o' $r 2; Expect-Out 'turf build -o' $r '-o is not accepted'
$r = Tr "$P1\app" @('build', '--package-graph', 'x.json'); Expect-Rc 'turf build --package-graph' $r 2; Expect-Out 'turf build --package-graph' $r '--package-graph is not accepted'
$r = Tr "$P1\app" @('run', '-o', 'x'); Expect-Rc 'turf run -o in a project' $r 2

# check: no executable, graph only
$r = Tr "$P1\app" @('clean'); Expect-Rc 'turf clean' $r 0
if (Test-Path "$P1\app\build") { Write-Fail 'turf clean: build/ still exists' }
$r = Tr "$P1\app" @('clean'); Expect-Rc 'turf clean twice' $r 0
$r = Tr "$P1\app" @('check'); Expect-Rc 'turf check in a project' $r 0
if ((Test-Path "$P1\app\build\app.exe") -or -not (Test-Path "$P1\app\build\package-graph.json")) { Write-Fail 'turf check: should write only the graph file' }
# The graph is a pure function of the manifests: a rebuild writes the same bytes.
Copy-Item "$P1\app\build\package-graph.json" "$work\graph1.json"
$r = Tr "$P1\app" @('build'); Expect-Rc 'rebuild' $r 0
Assert-SameFile 'graph is byte-identical on rebuild' "$work\graph1.json" "$P1\app\build\package-graph.json"
$graph = Get-Content -Raw "$P1\app\build\package-graph.json"
if ($graph -notmatch '"format": 1') { Write-Fail 'graph lacks format 1' }
# An absolute path: a drive letter (JSON may escape the backslashes) or a leading slash.
if ($graph -notmatch '"src": "([A-Za-z]:[/\\]|/)') { Write-Fail 'graph src paths should be absolute' }

# A type error is reported by turfc, unchanged, with its code.
Copy-Item "$P1\app\src\main.tr" "$work\main_good.tr"
Set-Text "$P1\app\src\main.tr" "import geometry;`n`nfn main(): int {`n    return geometry::answer() + missing;`n}`n"
$r = Tr "$P1\app" @('check'); Expect-Rc 'turf check reports a compile error' $r 1; Expect-Out 'turf check reports a compile error' $r 'missing'
Copy-Item -Force "$work\main_good.tr" "$P1\app\src\main.tr"

# The single-file path inside a project: E027 with the `turf build` hint.
$r = Tr "$P1\app" @('compile', 'src/main.tr')
Expect-Rc 'turf compile of a file that imports a dependency' $r 1
Expect-Out 'turf compile of a file that imports a dependency' $r '[E027'
Expect-Out 'turf compile of a file that imports a dependency' $r 'only visible to `turf build`'

# --- manifest edits leave a hand-written file alone --------------------------------------------
$P2 = "$work\p2"; New-Dir $P2
$null = Tr $P2 @('new', 'app')
Mk-Lib $P2 'geometry' 'pub fn answer(): int { return 7; }'
$orig = [System.IO.File]::ReadAllText("$P2\app\turf.toml")
Set-Text "$P2\app\turf.toml" ("# my own comment, kept`n" + $orig)
Copy-Item "$P2\app\turf.toml" "$work\toml.before"
$r = Tr "$P2\app" @('add', 'geometry', '--path', '../geometry'); Expect-Rc 'turf add into a commented manifest' $r 0
if (@(Get-Content "$P2\app\turf.toml") -cnotcontains '# my own comment, kept') { Write-Fail 'turf add dropped a comment' }
if ((@(Get-Content "$P2\app\turf.toml").Count - @(Get-Content "$work\toml.before").Count) -ne 1) { Write-Fail 'turf add changed more than one line' }
$r = Tr "$P2\app" @('add', 'geometry', '--path', '../geometry'); Expect-Rc 'turf add of a dependency already present' $r 1
Expect-Out 'turf add of a dependency already present' $r 'already in [dependencies]'
$r = Tr "$P2\app" @('remove', 'geometry'); Expect-Rc 'turf remove' $r 0
Assert-SameFile 'add then remove is byte-identical' "$work\toml.before" "$P2\app\turf.toml"
$r = Tr "$P2\app" @('remove', 'geometry'); Expect-Rc 'turf remove of an absent dependency' $r 1
Expect-Out 'turf remove of an absent dependency' $r 'is not in [dependencies]'

# add refuses what build would reject, and writes nothing.
Mk-Lib $P2 'other' 'pub fn f(): int { return 1; }'
Copy-Item "$P2\app\turf.toml" "$work\toml.before2"
$r = Tr "$P2\app" @('add', 'geo', '--path', '../other'); Expect-Rc 'turf add with a key that is not the package name' $r 1
Expect-Out 'turf add with a key that is not the package name' $r 'the key must be its package name'
$r = Tr "$P2\app" @('add', 'ghost', '--path', '../ghost'); Expect-Rc 'turf add of a missing directory' $r 1
$r = Tr "$P2\app" @('add', 'net', '--git', '/nonexistent/turf-smoke/net.git'); Expect-Rc 'turf add --git without a ref' $r 2
Expect-Out 'turf add --git without a ref' $r 'needs exactly one of --tag, --branch or --rev'
Assert-SameFile 'a refused add writes nothing' "$work\toml.before2" "$P2\app\turf.toml"

# A dependency spread over several lines is refused, not half-deleted.
Set-Text "$P2\app\turf.toml" "[package]`nname = `"app`"`nversion = `"0.1.0`"`n`n[dependencies]`ngeometry = {`n    path = `"../geometry`",`n}`n"
Copy-Item "$P2\app\turf.toml" "$work\toml.multi"
$r = Tr "$P2\app" @('remove', 'geometry'); Expect-Rc 'turf remove of a multi-line dependency' $r 1
Expect-Out 'turf remove of a multi-line dependency' $r 'edit turf.toml by hand'
Assert-SameFile 'a refused remove writes nothing' "$work\toml.multi" "$P2\app\turf.toml"

# --- packages with the same internal module name -------------------------------------------------
# The stage 4 identity case, through the CLI: both packages have a util::Node, and they are two types.
$P3 = "$work\p3"; New-Dir $P3
$null = Tr $P3 @('new', 'app')
Mk-Lib $P3 'shapes' "import util;`npub fn area(): int {`n    mut n: util::Node = util::Node::default();`n    n.v = util::g();`n    return n.v * 4;`n}"
Set-Text "$P3\shapes\src\util.tr" "pub struct Node {`n    pub v: int`n}`npub fn g(): int {`n    return 5;`n}`n"
Set-Text "$P3\app\src\util.tr" "pub struct Node {`n    pub v: int`n}`npub fn f(): int {`n    return 1;`n}`n"
Set-Text "$P3\app\src\main.tr" "import shapes;`nimport util;`n`nfn main(): int {`n    mut n: util::Node = util::Node::default();`n    n.v = util::f();`n    return shapes::area() + n.v;`n}`n"
$r = Tr "$P3\app" @('add', 'shapes', '--path', '../shapes'); Expect-Rc 'setup: add shapes' $r 0
$r = Tr "$P3\app" @('run'); Expect-Rc 'two packages each with util::Node' $r 21

# --- graph shapes ---------------------------------------------------------------------------------
# pa -> pc and pb -> pc: a diamond that agrees.
$P4 = "$work\p4"; New-Dir $P4
$null = Tr $P4 @('new', 'app')
Mk-Lib $P4 'pc' 'pub fn one(): int { return 1; }'
Mk-Lib $P4 'pa' "import pc;`npub fn fa(): int { return pc::one() + 10; }"
Mk-Lib $P4 'pb' "import pc;`npub fn fb(): int { return pc::one() + 20; }"
$null = Tr "$P4\pa" @('add', 'pc', '--path', '../pc')
$null = Tr "$P4\pb" @('add', 'pc', '--path', '../pc')
$null = Tr "$P4\app" @('add', 'pa', '--path', '../pa')
$r = Tr "$P4\app" @('add', 'pb', '--path', '../pb'); Expect-Rc 'setup: diamond adds' $r 0
Set-Text "$P4\app\src\main.tr" "import pa;`nimport pb;`n`nfn main(): int {`n    return pa::fa() + pb::fb();`n}`n"
$r = Tr "$P4\app" @('run'); Expect-Rc 'a diamond that agrees' $r 32

# Transitive import: pc is reachable only through pa and pb, so app cannot import it.
Copy-Item "$P4\app\src\main.tr" "$work\main4.tr"
Set-Text "$P4\app\src\main.tr" "import pa;`nimport pc;`n`nfn main(): int {`n    return pa::fa() + pc::one();`n}`n"
$r = Tr "$P4\app" @('build'); Expect-Rc 'importing a transitive dependency' $r 1; Expect-Out 'importing a transitive dependency' $r '[E091'
Copy-Item -Force "$work\main4.tr" "$P4\app\src\main.tr"

# Name conflict: two different directories both holding a package called pc.
$P5 = "$work\p5"; New-Dir $P5
$null = Tr $P5 @('new', 'app')
Mk-Lib $P5 'pa' 'pub fn fa(): int { return 1; }'
Mk-Lib $P5 'pb' 'pub fn fb(): int { return 2; }'
New-Dir "$P5\one"; New-Dir "$P5\two"
Mk-Lib "$P5\one" 'pc' 'pub fn one(): int { return 1; }'
Mk-Lib "$P5\two" 'pc' 'pub fn one(): int { return 2; }'
$null = Tr "$P5\pa" @('add', 'pc', '--path', '../one/pc')
$null = Tr "$P5\pb" @('add', 'pc', '--path', '../two/pc')
$r = Tr "$P5\app" @('add', 'pa', '--path', '../pa'); Expect-Rc 'setup: add pa' $r 0
# `turf add` resolves what it writes, so it refuses the second one...
Copy-Item "$P5\app\turf.toml" "$work\toml5"
$r = Tr "$P5\app" @('add', 'pb', '--path', '../pb')
Expect-Rc 'turf add that would create a name conflict' $r 1
Expect-Out 'turf add that would create a name conflict' $r "package 'pc' is required from two different directories"
Assert-SameFile 'the refused add wrote nothing' "$work\toml5" "$P5\app\turf.toml"
# ...so the conflict is written by hand to prove `build` refuses it too.
[System.IO.File]::AppendAllText("$P5\app\turf.toml", "pb = { path = `"../pb`" }`n")
$r = Tr "$P5\app" @('build')
Expect-Rc 'one package name at two directories' $r 1
Expect-Out 'one package name at two directories' $r "package 'pc' is required from two different directories"
Expect-Out 'one package name at two directories' $r "package 'pa'"
Expect-Out 'one package name at two directories' $r "package 'pb'"

# Cycle: written by hand, because `turf add` would refuse to create it.
$P6 = "$work\p6"; New-Dir $P6
Mk-Lib $P6 'pa' 'pub fn fa(): int { return 1; }'
Mk-Lib $P6 'pb' 'pub fn fb(): int { return 2; }'
[System.IO.File]::AppendAllText("$P6\pa\turf.toml", "pb = { path = `"../pb`" }`n")
[System.IO.File]::AppendAllText("$P6\pb\turf.toml", "pa = { path = `"../pa`" }`n")
$r = Tr "$P6\pa" @('build')
Expect-Rc 'a dependency cycle' $r 1
Expect-Out 'a dependency cycle' $r 'dependency cycle'
Expect-Out 'a dependency cycle' $r 'pa -> pb -> pa'
# `add` resolves what it writes, so it refuses to close the cycle.
Mk-Lib $P6 'pc' 'pub fn fc(): int { return 3; }'
Mk-Lib $P6 'pd' 'pub fn fd(): int { return 4; }'
$null = Tr "$P6\pc" @('add', 'pd', '--path', '../pd')
$r = Tr "$P6\pd" @('add', 'pc', '--path', '../pc')
Expect-Rc 'turf add that would close a cycle' $r 1
Expect-Out 'turf add that would close a cycle' $r 'dependency cycle'

# Key/name mismatch.
$P7 = "$work\p7"; New-Dir $P7
$null = Tr $P7 @('new', 'app')
Mk-Lib $P7 'geo' 'pub fn f(): int { return 1; }'
[System.IO.File]::AppendAllText("$P7\app\turf.toml", "geometry = { path = `"../geo`" }`n")
$r = Tr "$P7\app" @('build')
Expect-Rc 'dependency key differs from the package name' $r 1
Expect-Out 'dependency key differs from the package name' $r 'the key must be its package name'
# A dependency with no root module.
$r = Tr $P7 @('new', 'tool'); if ($r.Rc -ne 0) { Write-Fail 'setup: turf new tool' }
Set-Text "$P7\app\turf.toml" "[package]`nname = `"app`"`nversion = `"0.1.0`"`n`n[dependencies]`ntool = { path = `"../tool`" }`n"
$r = Tr "$P7\app" @('build')
Expect-Rc 'a dependency that is not a library' $r 1
Expect-Out 'a dependency that is not a library' $r 'is not a library package: it has no src/tool.tr'

# --- manifest rules, through the CLI ------------------------------------------------------------------
function Test-Manifest([string]$Label, [int]$WantRc, [string]$Fragment, [string]$TomlText) {
  $script:counter++
  $d = "$work\m_$($script:counter)"
  New-Dir "$d\src"
  Set-Text "$d\src\main.tr" "fn main(): int {`n    return 0;`n}`n"
  Set-Text "$d\turf.toml" ($TomlText + "`n")
  $r = Tr $d @('build')
  Expect-Rc "manifest: $Label" $r $WantRc
  Expect-Out "manifest: $Label" $r $Fragment
  return $r
}
$null = Test-Manifest 'unknown key in [package]' 1 'unknown key `authors` in [package]' "[package]`nname = `"app`"`nversion = `"0.1.0`"`nauthors = [`"me`"]"
$null = Test-Manifest 'unknown table' 1 'unknown key `build` at the top level' "[package]`nname = `"app`"`nversion = `"0.1.0`"`n[build]`nx = 1"
$null = Test-Manifest 'registry-style dependency' 1 'registry versions are not part of TurfLang 2.0' "[package]`nname = `"app`"`nversion = `"0.1.0`"`n[dependencies]`nmath = `"^1.2.0`""
$null = Test-Manifest 'path and git together' 1 'has both `path` and `git`' "[package]`nname = `"app`"`nversion = `"0.1.0`"`n[dependencies]`nm = { path = `"../m`", git = `"https://example.invalid/m`", tag = `"v1`" }"
# A git dependency is fetched, so this one names a repository that cannot exist (a local path:
# no network). The behaviour of real ones is git_deps_smoke.ps1's.
$null = Test-Manifest 'a git dependency that cannot be fetched' 1 "cannot fetch 'm' from /nonexistent/turf-smoke/m.git" "[package]`nname = `"app`"`nversion = `"0.1.0`"`n[dependencies]`nm = { git = `"/nonexistent/turf-smoke/m.git`", tag = `"v1`" }"
$null = Test-Manifest 'reserved word as the package name' 1 "'fn' is a reserved word" "[package]`nname = `"fn`"`nversion = `"0.1.0`""
$null = Test-Manifest 'no name' 1 'missing the required key `name`' "[package]`nversion = `"0.1.0`""
New-Dir "$work\empty"
Set-Text "$work\empty\turf.toml" "[package]`nname = `"empty`"`nversion = `"0.1.0`"`n"
$r = Tr "$work\empty" @('build')
Expect-Rc 'a package with neither main.tr nor a root module' $r 1
Expect-Out 'a package with neither main.tr nor a root module' $r 'has neither src/main.tr (a program) nor src/empty.tr (a library)'
$pin = Test-Manifest 'turf pin, wrong major' 1 'requires turf "9.0"' "[package]`nname = `"app`"`nversion = `"0.1.0`"`nturf = `"9.0`""
Expect-Out 'turf pin names the compiler version too' $pin "turfc $Ver"
$null = Test-Manifest 'turf pin, newer minor' 1 'which is older' "[package]`nname = `"app`"`nversion = `"0.1.0`"`nturf = `"2.99`""

# --- [unsafe] allow (F-PKG-4) ---------------------------------------------------------------------------
$P8 = "$work\p8"; New-Dir $P8
$null = Tr $P8 @('new', 'app')
Mk-Lib $P8 'ffi' "import rawmod;`npub fn go(): int { return rawmod::raw(); }"
Set-Text "$P8\ffi\src\rawmod.tr" "unsafe module;`npub fn raw(): int {`n    mut p: int* = null;`n    return 1;`n}`n"
$null = Tr "$P8\app" @('add', 'ffi', '--path', '../ffi')
Set-Text "$P8\app\src\main.tr" "import ffi;`n`nfn main(): int {`n    return ffi::go();`n}`n"
$toml8 = [System.IO.File]::ReadAllText("$P8\app\turf.toml")
$r = Tr "$P8\app" @('build')
Expect-Rc "a dependency's unsafe module, not allowed" $r 1
Expect-Out "a dependency's unsafe module, not allowed" $r '[E130'
function Test-Unsafe([string]$Label, [int]$WantRc, [string]$Fragment, [string]$AllowList) {
  Set-Text "$P8\app\turf.toml" ($toml8 + "`n[unsafe]`nallow = [$AllowList]`n")
  $r = Tr "$P8\app" @('build')
  Expect-Rc "unsafe: $Label" $r $WantRc
  if ($Fragment) { Expect-Out "unsafe: $Label" $r $Fragment }
}
Test-Unsafe 'allowed' 0 '' '"ffi"'
Test-Unsafe 'an unknown package' 1 "which is not a package in this project's dependency graph" '"nope"'
Test-Unsafe 'the root itself' 1 'its own modules need no listing' '"app"'
# The dependency's own [unsafe] table grants nothing.
Set-Text "$P8\app\turf.toml" $toml8
[System.IO.File]::AppendAllText("$P8\ffi\turf.toml", "`n[unsafe]`nallow = [`"ffi`"]`n")
$r = Tr "$P8\app" @('build')
Expect-Rc 'a dependency cannot allow itself' $r 1
Expect-Out 'a dependency cannot allow itself' $r '[E130'

# --- [lints] (V-1) ----------------------------------------------------------------------------------------
# The manifest's levels reach the compiler as flags ahead of the user's own.
$P9 = "$work\p9"; New-Dir $P9
$null = Tr $P9 @('new', 'app')
Set-Text "$P9\app\src\main.tr" "fn main(): int {`n    mut unused: int = 5;`n    return 0;`n}`n"
New-Dir "$P9\app\tests"
Set-Text "$P9\app\tests\unused_var.tr" "fn main(): int {`n    mut unused: int = 5;`n    return 0;`n}`n"
$toml9 = [System.IO.File]::ReadAllText("$P9\app\turf.toml")
function Test-Lints([string]$Label, [int]$WantRc, [string]$Fragment, [string]$Body, [string[]]$TurfArgs = @('build')) {
  $text = $toml9
  if ($Body) { $text += "`n[lints]`n$Body" }
  Set-Text "$P9\app\turf.toml" $text
  $r = Tr "$P9\app" $TurfArgs
  Expect-Rc "lints: $Label" $r $WantRc
  if ($Fragment) { Expect-Out "lints: $Label" $r $Fragment }
  return $r
}
$null = Test-Lints 'no table: the warning is shown and the build succeeds' 0 '[E163' ''
$r = Test-Lints 'allow' 0 '' "E163 = `"allow`"`n"
Expect-NoOut 'lints: allow' $r 'E163'
$null = Test-Lints 'deny' 1 'error at line 2, col 5 [E163' "E163 = `"deny`"`n"
$null = Test-Lints 'a flag on the command line beats the table' 0 '' "E163 = `"deny`"`n" @('build', '--allow', 'E163')
$null = Test-Lints 'lint-set is accepted' 0 '[E163' "lint-set = `"2.0`"`n"
$null = Test-Lints "an unknown code is the compiler's usage error" 2 "unknown diagnostic code 'E999'" "E999 = `"deny`"`n"
$null = Test-Lints "an error's code cannot be allowed" 2 'E102 is an error, not a warning' "E102 = `"allow`"`n"
$null = Test-Lints 'a key that is not a code' 1 'unknown key `unused` in [lints]' "unused = `"allow`"`n"
$null = Test-Lints 'a level that is not one' 1 "'forbid' is not allow, warn or deny" "E163 = `"forbid`"`n"
$null = Test-Lints 'a lint set newer than the compiler' 2 'is newer than this compiler' "lint-set = `"2.99`"`n"
$null = Test-Lints 'turf test passes the table on' 1 'COMPILE FAILED' "E163 = `"deny`"`n" @('test')
Set-Text "$P9\app\turf.toml" $toml9

# --- refusals ---------------------------------------------------------------------------------------------
$Loose = "$work\loose"; New-Dir $Loose
$r = Tr $Loose @('build'); Expect-Rc 'turf build outside a project' $r 2; Expect-Out 'turf build outside a project' $r 'turf new'
$r = Tr $Loose @('run'); Expect-Rc 'turf run with no file outside a project' $r 2
$r = Tr $Loose @('add', 'x', '--path', '../x'); Expect-Rc 'turf add outside a project' $r 2

$r = Tr $Loose @('new', 'list', '--lib')
Expect-Rc 'turf new of a library named like a stdlib module' $r 1
Expect-Out 'turf new of a library named like a stdlib module' $r 'standard-library module'
if (Test-Path "$Loose\list") { Write-Fail 'a refused turf new must not create the directory' }
$r = Tr $Loose @('new', 'fn'); Expect-Rc 'turf new with a reserved word' $r 1; Expect-Out 'turf new with a reserved word' $r 'reserved word'
$r = Tr $Loose @('new', 'my-app'); Expect-Rc 'turf new with an invalid name' $r 1
# A program named like a stdlib module is fine: nothing can import it.
$r = Tr $Loose @('new', 'json'); Expect-Rc 'turf new of a program named like a stdlib module' $r 0

# --- init -----------------------------------------------------------------------------------------------------
New-Dir "$Loose\myinit\src"
Set-Text "$Loose\myinit\src\main.tr" "fn main(): int {`n    return 3;`n}`n"
Set-Text "$Loose\myinit\.gitignore" "*.o`n"
$r = Tr "$Loose\myinit" @('init'); Expect-Rc 'turf init in an existing directory' $r 0
if (-not (Get-Content -Raw "$Loose\myinit\src\main.tr").Contains('return 3;')) { Write-Fail 'turf init overwrote src/main.tr' }
$gi = @(Get-Content "$Loose\myinit\.gitignore")
if (($gi -notcontains '/build/') -or ($gi -notcontains '*.o')) { Write-Fail 'turf init should append /build/ and keep the existing .gitignore lines' }
if (@(Get-Content "$Loose\myinit\turf.toml") -notcontains 'name = "myinit"') { Write-Fail "turf init should default the name to the directory's" }
$r = Tr "$Loose\myinit" @('init'); Expect-Rc 'turf init twice' $r 1; Expect-Out 'turf init twice' $r 'already exists'
$r = Tr "$Loose\myinit" @('run'); Expect-Rc 'the initialised project runs' $r 3
New-Dir "$Loose\bad-name"
$r = Tr "$Loose\bad-name" @('init')
Expect-Rc 'turf init in a directory whose name is not a package name' $r 1
Expect-Out 'turf init in a directory whose name is not a package name' $r 'not a valid package name'

# --- drift: the reserved words the CLI carries vs the lexer's -----------------------------------------------------
$manifestPath = Join-Path $script:CliDir 'internal\manifest\manifest.go'
if (Test-Path -LiteralPath $manifestPath) {
  $lexer = Get-Content -Raw (Join-Path $script:Root 'src\frontend\Lexer.cpp')
  $lexBlock = [regex]::Match($lexer, '(?s)std::map<std::string, int> Keywords.*?;')
  $kwLexer = @([regex]::Matches($lexBlock.Value, '\{"([a-z_0-9]*)", TOK') | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
  $manifest = Get-Content -Raw $manifestPath
  $cliBlock = [regex]::Match($manifest, '(?ms)^var reserved = .*?^\}')
  $kwCli = @([regex]::Matches($cliBlock.Value, '"([a-z_0-9]*)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
  if ($kwLexer.Count -le 40) { Write-Fail "drift: could not extract the lexer's keyword table" }
  $diff = Compare-Object $kwLexer $kwCli
  if ($diff) {
    Write-Fail "drift: $manifestPath's reserved list differs from src/frontend/Lexer.cpp's Keywords ($(($diff | ForEach-Object { $_.SideIndicator + $_.InputObject }) -join ' '))"
  }
} elseif ($env:CI) {
  Write-Fail "drift: turf's source not found (..\turf-cli; set TURF_CLI_DIR)"
} else {
  Write-Skip "drift: turf's source not found (..\turf-cli; set TURF_CLI_DIR)"
}

Complete-Smoke
