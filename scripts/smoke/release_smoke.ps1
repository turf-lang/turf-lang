# release_smoke.ps1 -- the Windows twin of release_smoke.sh (REL-003) and, because it runs
# the installed toolchain on a PATH that holds only the Windows system directories and the
# install, the Windows clean-machine proof of REL-008 stage 8.
#
# Takes a release archive (turf-v<ver>-windows-x86_64.zip with its .sha256 beside it),
# serves it from a local "release" directory, installs it with install.ps1 into a
# throwaway prefix, and then uses only what that install put on PATH.
#
# Proves, each against the real files:
#   * the archive has the documented layout;
#   * install.ps1 verifies the SHA-256 (a corrupted archive installs nothing and edits no
#     PATH), finds its archive in a `<hash> *<name>` checksums.txt too, and a second install
#     leaves one PATH entry;
#   * the user PATH entry is added once, ahead of the others, with the user's own entries
#     kept and their %VARIABLES% still unexpanded; -NoModifyPath edits nothing;
#   * a NEW process whose PATH is built from that registry value resolves `turf`;
#   * with a PATH of System32 plus the install and NO gcc, git, MSYS2 or MinGW anywhere on
#     it, `turf new hello`, `turf run`, `turf build`, a path-dependency project, a program
#     that talks to itself over a loopback socket, and `turf explain` all work;
#   * uninstall puts the registry value back exactly (text and type), leaves the package
#     cache alone, and uninstalling twice is an error.
#
# The registry edit is made to a throwaway key (HKCU\Software\TurfSmoke\Environment, via
# TURF_INSTALL_ENV_KEY), never to your real user PATH. What that leaves unchecked is the
# real `Environment` key and the WM_SETTINGCHANGE broadcast; do those by hand once:
# `install.ps1`, open a new terminal, `where turf`, `install.ps1 -Uninstall`.
#
# Not here: the bash twin's tag-mismatch refusal and gen_recipes.sh checks (they test
# scripts/release/package_release.sh and scripts/release/gen_recipes.sh, which run on Linux), a zsh/bash
# start-up file (there is none on Windows), and a git-dependency project (git is, by
# definition, not on the clean PATH; scripts/smoke/git_deps_smoke.ps1 covers git dependencies).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke\release_smoke.ps1 [-Archive FILE]
#   -Archive FILE   test this archive (its .sha256 beside it). Without it, the archive is
#                   packaged now from build\ with scripts/release/package_release.sh, which needs
#                   `bash` (Git for Windows or MSYS2) on PATH.
# Exit:  0 = every check passed; 1 = at least one failed.

param([string]$Archive = '')

. "$PSScriptRoot\smoke_common.ps1"
Start-Smoke 'release_smoke'

$work = New-WorkDir
$testKeyRoot = 'Software\TurfSmoke'
$testKey = "$testKeyRoot\Environment"
$powershell = (Get-Process -Id $PID).Path
$installer = Join-Path $script:Root 'scripts\install\install.ps1'

function Get-Raw([string]$Sub) {
  $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Sub)
  if ($null -eq $k) { return $null }
  try { return $k.GetValue('Path', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
  finally { $k.Close() }
}
function Get-Kind([string]$Sub) {
  $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Sub)
  if ($null -eq $k) { return $null }
  try { if ($null -eq $k.GetValue('Path', $null)) { return $null }; return [string]$k.GetValueKind('Path') }
  finally { $k.Close() }
}
function Set-Raw([string]$Sub, [string]$Value, [Microsoft.Win32.RegistryValueKind]$Kind) {
  $k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($Sub)
  try { $k.SetValue('Path', $Value, $Kind) } finally { $k.Close() }
}
function Join-Output($R) { return ($R.Out + $R.Err) }

function Invoke-Installer([string[]]$InstallerArgs, [string]$BaseUrl, [hashtable]$ExtraEnv = @{}) {
  $e = @{ TURF_INSTALL_BASE_URL = $BaseUrl; TURF_INSTALL_ENV_KEY = $testKey; TURF_CACHE_DIR = (Join-Path $work 'cache') }
  foreach ($k in $ExtraEnv.Keys) { $e[$k] = $ExtraEnv[$k] }
  return Invoke-Proc -File $powershell -ArgList (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $installer) + $InstallerArgs) -Env $e -TimeoutSec 300
}

try {
  # ---- the archive -------------------------------------------------------------------------
  Write-Heading '== the archive'
  $version = (Get-Content -LiteralPath (Join-Path $script:Root 'VERSION') -TotalCount 1).Trim()
  $tag = "v$version"
  $rel = Join-Path $work "release\download\$tag"
  New-Item -ItemType Directory -Path $rel | Out-Null
  if ($Archive) {
    if (-not ((Test-Path -LiteralPath $Archive) -and (Test-Path -LiteralPath "$Archive.sha256"))) {
      Write-Fail "cannot read $Archive and its .sha256"; Complete-Smoke
    }
    Copy-Item -LiteralPath $Archive, "$Archive.sha256" -Destination $rel
  } else {
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if (-not $bash -or -not (Test-Path -LiteralPath $script:Turfc)) {
      Write-Fail 'without -Archive this needs build\turfc.exe and bash on PATH to package one; pass -Archive FILE'; Complete-Smoke
    }
    $outPosix = ($rel -replace '\\', '/')
    $p = Invoke-Proc -File $bash.Source -ArgList @('scripts/release/package_release.sh', '--platform', 'windows-x86_64', '--out', $outPosix)
    if ($p.Rc -ne 0) { Write-Fail 'packaging failed:'; Write-Indented (Join-Output $p); Complete-Smoke }
  }
  $archiveFile = @(Get-ChildItem -LiteralPath $rel -Filter 'turf-v*.zip')[0].Name
  # `<hash>  <name>` lines, as sha256sum writes them; checksums.txt is their concatenation.
  Set-Content -LiteralPath (Join-Path $rel 'checksums.txt') -Value (Get-Content -LiteralPath "$rel\$archiveFile.sha256") -Encoding ascii
  $wantHash = ((Get-Content -LiteralPath "$rel\$archiveFile.sha256" -TotalCount 1) -split '\s+')[0].ToLowerInvariant()
  $gotHash = (Get-FileHash -Algorithm SHA256 -LiteralPath "$rel\$archiveFile").Hash.ToLowerInvariant()
  if ($wantHash -eq $gotHash) { Write-Pass 'the .sha256 file verifies the archive' } else { Write-Fail 'the .sha256 file does not verify the archive' }

  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $zip = [IO.Compression.ZipFile]::OpenRead("$rel\$archiveFile")
  try { $entries = @($zip.Entries | ForEach-Object { $_.FullName -replace '\\', '/' }) } finally { $zip.Dispose() }
  $top = $entries[0].Split('/')[0]
  foreach ($want in 'bin/turf.exe', 'bin/turfc.exe', 'lib/libturfrt.a', 'lib/std/core/char.tr', 'docs/errors/codes/E001.txt',
                    'docs/slm/syntax_card.txt', 'share/turf/examples/plotter/turf.toml', 'LICENSE', 'README.md') {
    if ($entries -contains "$top/$want") { Write-Pass "contains $want" } else { Write-Fail "archive lacks $want" }
  }
  if (@($entries | Where-Object { $_ -like '*libturfrt_asan*' }).Count -gt 0) { Write-Fail 'the sanitizer runtime is in the archive' } else { Write-Pass 'no sanitizer runtime' }
  if (@($entries | Where-Object { $_ -match "^$([regex]::Escape($top))/(turf|turfc)(\.exe)?$" }).Count -gt 0) { Write-Fail 'a binary sits at the archive top level' } else { Write-Pass 'binaries only in bin/' }
  Write-Skip 'the tag-mismatch refusal and gen_recipes.sh checks test scripts that run on Linux (release_smoke.sh)'

  # ---- install -----------------------------------------------------------------------------
  Write-Heading '== install.ps1'
  $base = 'file:///' + (($work -replace '\\', '/') + '/release')
  $prefix = Join-Path $work 'prefix'
  $bin = Join-Path $prefix 'bin'
  $cache = Join-Path $work 'cache'
  New-Item -ItemType Directory -Path $cache | Out-Null
  [IO.File]::WriteAllText((Join-Path $cache 'marker'), 'keep')

  # The user's PATH before: a literal directory and one that holds a %VARIABLE%, as a
  # user-set PATH commonly does. Reading it expanded and writing it back would lose the second.
  $before = 'C:\Tools;%USERPROFILE%\bin'
  Set-Raw $testKey $before ([Microsoft.Win32.RegistryValueKind]::ExpandString)

  # A corrupted archive must install nothing and edit no PATH.
  $badRel = Join-Path $work "bad\download\$tag"
  New-Item -ItemType Directory -Path $badRel | Out-Null
  Copy-Item -LiteralPath "$rel\checksums.txt" -Destination $badRel
  $bytes = [IO.File]::ReadAllBytes("$rel\$archiveFile")
  $short = New-Object byte[] 100000
  [Array]::Copy($bytes, $short, 100000)
  [IO.File]::WriteAllBytes("$badRel\$archiveFile", $short)
  $r = Invoke-Installer @('-Version', $tag, '-Prefix', $prefix) ('file:///' + (($work -replace '\\', '/') + '/bad'))
  if ($r.Rc -ne 0 -and (Join-Output $r).Contains('SHA-256 mismatch') -and -not (Test-Path -LiteralPath $bin) -and (Get-Raw $testKey) -eq $before) {
    Write-Pass 'a corrupted archive is refused, and nothing is installed or edited'
  } else { Write-Fail "corrupted archive: rc $($r.Rc), $((Join-Output $r).Trim())" }

  # `sha256sum` on Windows writes `<hash> *<name>`; the installer must read that form.
  $starRel = Join-Path $work "star\download\$tag"
  New-Item -ItemType Directory -Path $starRel | Out-Null
  Copy-Item -LiteralPath "$rel\$archiveFile" -Destination $starRel
  Set-Content -LiteralPath "$starRel\checksums.txt" -Value ("$wantHash *$archiveFile") -Encoding ascii
  $starPrefix = Join-Path $work 'starprefix'
  $r = Invoke-Installer @('-Version', $tag, '-Prefix', $starPrefix, '-NoModifyPath') ('file:///' + (($work -replace '\\', '/') + '/star'))
  if ($r.Rc -eq 0 -and (Test-Path -LiteralPath "$starPrefix\bin\turf.exe")) { Write-Pass "install.ps1 finds its archive in a '<hash> *<name>' checksums.txt" }
  else { Write-Fail "starred names: rc $($r.Rc), $((Join-Output $r).Trim())" }

  $r = Invoke-Installer @('-Version', $tag, '-Prefix', $prefix) $base
  if ($r.Rc -ne 0) { Write-Fail 'install failed:'; Write-Indented (Join-Output $r); Complete-Smoke }
  if ((Join-Output $r).Contains('SHA-256 verified')) { Write-Pass 'checksum verified before installing' } else { Write-Fail 'no checksum verification line' }
  if ((Test-Path -LiteralPath "$bin\turf.exe") -and (Test-Path -LiteralPath "$bin\turfc.exe") -and (Test-Path -LiteralPath "$prefix\lib\libturfrt.a")) { Write-Pass 'installed into the prefix' }
  else { Write-Fail 'the prefix is missing bin/lib files' }
  $after = Get-Raw $testKey
  $entries1 = @($after.Split(';'))
  if (@($entries1 | Where-Object { $_ -eq $bin }).Count -eq 1 -and $entries1[0] -eq $bin) { Write-Pass 'one PATH entry, ahead of the others' } else { Write-Fail "PATH is: $after" }
  if ($after.EndsWith(";$before")) { Write-Pass "the user's own entries are kept, %USERPROFILE% unexpanded" } else { Write-Fail "the user's entries changed: $after" }
  if ((Get-Kind $testKey) -eq 'ExpandString') { Write-Pass 'the value is still REG_EXPAND_SZ' } else { Write-Fail "the value type is now $(Get-Kind $testKey)" }

  $r = Invoke-Installer @('-Version', $tag, '-Prefix', $prefix) $base
  if ($r.Rc -ne 0) { Write-Fail 'a second install failed:'; Write-Indented (Join-Output $r) }
  $again = Get-Raw $testKey
  if (@($again.Split(';') | Where-Object { $_ -eq $bin }).Count -eq 1 -and $again -eq $after) { Write-Pass 'installing again leaves the same single entry' } else { Write-Fail "PATH after a second install: $again" }

  # The latest-release lookup, by way of the directory install.ps1 builds it from.
  $latestPrefix = Join-Path $work 'latestprefix'
  $latestDir = Join-Path $work 'release\latest'
  New-Item -ItemType Directory -Path $latestDir | Out-Null
  Copy-Item -LiteralPath $rel -Destination "$latestDir\download" -Recurse
  $rawBefore = Get-Raw $testKey
  $r = Invoke-Installer @('-Prefix', $latestPrefix, '-NoModifyPath') $base
  if ($r.Rc -eq 0 -and (Test-Path -LiteralPath "$latestPrefix\bin\turf.exe") -and (Get-Raw $testKey) -eq $rawBefore) { Write-Pass 'default (latest) lookup, and -NoModifyPath edits no PATH' }
  else { Write-Fail "latest/-NoModifyPath install: rc $($r.Rc), $((Join-Output $r).Trim())" }

  # ---- a new process finds turf, with nothing of the checkout in sight -----------------------
  Write-Heading '== a new process, on a PATH of the system directories and the install'
  $sys = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
  # What a terminal opened after the install gets: the user PATH, expanded, then the system's.
  $newPath = [Environment]::ExpandEnvironmentVariables((Get-Raw $testKey)) + ';' + $sys
  $cleanEnv = @{ PATH = $newPath; TURF_CACHE_DIR = $cache; TERM = 'dumb' }
  $w = Invoke-Proc -File "$env:SystemRoot\System32\where.exe" -ArgList @('turf') -Env $cleanEnv
  $found = ((Join-Output $w) -split "`r?`n")[0].Trim()
  if ($found -eq "$bin\turf.exe") { Write-Pass 'a new process resolves turf to the installed one' } else { Write-Fail "where turf: '$found'" }

  # ---- use it, with no C toolchain anywhere --------------------------------------------------
  Write-Heading '== turf, from the installed archive only'
  foreach ($tool in 'gcc', 'cc', 'clang', 'ld', 'git') {
    $w = Invoke-Proc -File "$env:SystemRoot\System32\where.exe" -ArgList @($tool) -Env $cleanEnv
    if ($w.Rc -eq 0) { Write-Fail "$tool is on the clean PATH ($((Join-Output $w).Trim())); this machine cannot prove the clean-machine claim" }
  }
  Write-Pass 'the PATH under test holds no gcc, cc, clang, ld or git'
  function Invoke-Installed([string]$Exe, [string[]]$ArgList, [string]$Cwd) {
    return Invoke-Proc -File "$bin\$Exe" -ArgList $ArgList -Cwd $Cwd -Env $cleanEnv -TimeoutSec 300
  }
  $r = Invoke-Installed 'turf.exe' @('version') $work
  $ver = Remove-Ansi (Join-Output $r)
  if ($ver -match '(?m)root\s+(\S.*?)\s*$' -and ($Matches[1] -replace '/', '\').TrimEnd('\') -eq $prefix.TrimEnd('\')) { Write-Pass "turfc's install root is the prefix" } else { Write-Fail "turf version: $ver" }
  if ($ver -match 'is not on your PATH|not this one') { Write-Fail 'turf version says it is not the one on PATH' } else { Write-Pass 'turf version raises no PATH note' }

  $proj = Join-Path $work 'proj'
  New-Item -ItemType Directory -Path $proj | Out-Null
  $r = Invoke-Installed 'turf.exe' @('new', 'hello') $proj
  if ($r.Rc -eq 0) { Write-Pass 'turf new hello' } else { Write-Fail "turf new: $(Join-Output $r)" }
  $hello = Join-Path $proj 'hello'
  $r = Invoke-Installed 'turf.exe' @('run') $hello
  if ((Remove-Ansi (Join-Output $r)) -match 'hello') { Write-Pass 'turf run prints hello' } else { Write-Fail "turf run: rc $($r.Rc) $(Join-Output $r)" }
  $r = Invoke-Installed 'turf.exe' @('build') $hello
  if ($r.Rc -eq 0 -and (Test-Path -LiteralPath "$hello\build\hello.exe")) { Write-Pass 'turf build leaves build\hello.exe' } else { Write-Fail "turf build: rc $($r.Rc) $(Join-Output $r)" }

  # A path dependency: the examples the archive ships, copied out the way a user would.
  Copy-Item -LiteralPath "$prefix\share\turf\examples" -Destination "$work\examples" -Recurse
  $r = Invoke-Installed 'turf.exe' @('run') "$work\examples\plotter"
  if ((Remove-Ansi (Join-Output $r)).Contains('distance squared: 25')) { Write-Pass 'a project with a path dependency (examples\plotter) runs' } else { Write-Fail "plotter: rc $($r.Rc) $(Join-Output $r)" }

  # A program that serves and fetches over loopback inside one process: Winsock linked in,
  # with no compiler on the machine.
  $loop = Join-Path $script:Root 'tests\34_http_and_json\03_http_manual_routing_roundtrip.tr'
  $loopExe = Join-Path $work 'loopback.exe'
  $r = Invoke-Installed 'turfc.exe' @($loop, '-o', $loopExe) $work
  if ($r.Rc -eq 0) {
    $run = Invoke-Proc -File $loopExe -Env $cleanEnv -TimeoutSec 60
    if ($run.Rc -eq 0) { Write-Pass 'a net program talks to itself over loopback' } else { Write-Fail "loopback program: rc $($run.Rc) $(Join-Output $run)" }
  } else { Write-Fail "loopback program failed to compile: $(Join-Output $r)" }

  # An error explanation reads docs\ from the install root.
  $r = Invoke-Installed 'turf.exe' @('explain', 'E001') $work
  if (@((Remove-Ansi (Join-Output $r)) -split "`r?`n").Count -gt 3) { Write-Pass 'turf explain reads the installed docs\errors' } else { Write-Fail "turf explain E001 was short: $(Join-Output $r)" }

  # ---- uninstall -----------------------------------------------------------------------------
  Write-Heading '== uninstall'
  $r = Invoke-Installer @('-Uninstall', '-Prefix', $prefix) $base
  if ($r.Rc -ne 0) { Write-Fail "uninstall failed: $(Join-Output $r)" }
  if (-not (Test-Path -LiteralPath $prefix)) { Write-Pass 'the prefix is gone' } else { Write-Fail "the prefix is still there: $((Get-ChildItem -LiteralPath $prefix -Force -Name) -join ' ')" }
  if ((Get-Raw $testKey) -eq $before -and (Get-Kind $testKey) -eq 'ExpandString') { Write-Pass 'the PATH value is back exactly (text and type)' } else { Write-Fail "PATH after uninstall: $(Get-Raw $testKey) ($(Get-Kind $testKey))" }
  if ([IO.File]::ReadAllText((Join-Path $cache 'marker')) -eq 'keep') { Write-Pass 'the package cache was not touched' } else { Write-Fail 'the cache marker file vanished' }
  $r = Invoke-Installer @('-Uninstall', '-Prefix', $prefix) $base
  if ($r.Rc -ne 0) { Write-Pass 'uninstalling when nothing is installed is an error, not a silent pass' } else { Write-Fail 'uninstalling twice did not complain' }

  # A model the user put in models\ stays, and the uninstall still finishes (the twin of the
  # kept-model checks in release_smoke.sh). Not a guard for the interactive "has children,
  # continue?" prompt install.ps1 once raised on the non-empty prefix: the installer runs here
  # with stdin closed, where that prompt resolves to "no" by itself, so only a person at a
  # console sees it.
  $r = Invoke-Installer @('-Version', $tag, '-Prefix', $prefix) $base
  if ($r.Rc -ne 0) { Write-Fail "reinstall for the kept-model check failed: $(Join-Output $r)" }
  New-Item -ItemType Directory -Force -Path (Join-Path $prefix 'models') | Out-Null
  [IO.File]::WriteAllText((Join-Path $prefix 'models\my.gguf'), 'x')
  $r = Invoke-Installer @('-Uninstall', '-Prefix', $prefix) $base
  if ($r.Rc -eq 0 -and (Test-Path -LiteralPath (Join-Path $prefix 'models\my.gguf')) -and -not (Test-Path -LiteralPath (Join-Path $prefix 'bin'))) { Write-Pass 'uninstall with a model in models\ finishes and keeps the model' } else { Write-Fail "uninstall with a model: rc $($r.Rc), $(Join-Output $r)" }
  if ((Get-Raw $testKey) -eq $before -and (Get-Kind $testKey) -eq 'ExpandString') { Write-Pass 'and it still removed the PATH entry' } else { Write-Fail "PATH after the kept-model uninstall: $(Get-Raw $testKey)" }
} finally {
  try { [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($testKeyRoot, $false) } catch { }
}

Complete-Smoke
