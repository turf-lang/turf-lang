# install.ps1 -- install TurfLang from a GitHub release archive (Windows x86-64).
# The Windows twin of install.sh; change one, change the other.
#
#   irm https://raw.githubusercontent.com/turf-lang/turf-lang/master/install.ps1 | iex
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Version v2.0.0 -Prefix D:\tools\turf -NoModifyPath
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
#
# What it does: finds the archive for this machine in the release's checksums.txt,
# downloads it, VERIFIES its SHA-256 (it stops, and never installs, if the check fails),
# unpacks it into the prefix (default %LOCALAPPDATA%\turf, so `turf` is
# %LOCALAPPDATA%\turf\bin\turf.exe), and adds that bin directory to your USER PATH (no
# administrator rights, nothing machine-wide) and tells running programs the environment
# changed, so `turf` works from any directory in a NEW terminal.
#
# The checksum comes from the same release as the archive. It catches a damaged or
# truncated download, not a compromised release; archive signing is separate
# (ROADMAP_v2.0.0.md REL-008 stage 7).
#
# Options (a piped `irm | iex` cannot pass them; use the environment variables):
#   -Version V        install release V (v2.0.0 or 2.0.0); default: the latest
#                     (env: TURF_INSTALL_VERSION)
#   -Prefix DIR       install here instead of %LOCALAPPDATA%\turf  (env: TURF_INSTALL_DIR)
#   -NoModifyPath     change no PATH; print the command to run instead
#   -Uninstall        remove what this script installed and its PATH entry
#   -Help             this text
#
# Environment: TURF_INSTALL_BASE_URL points at a mirror, or a directory of
# `download/<tag>/...` files (file:// works), instead of
# https://github.com/turf-lang/turf-lang/releases. TURF_INSTALL_PLATFORM overrides
# platform detection (windows-x86_64). TURF_INSTALL_ENV_KEY names the registry key under
# HKEY_CURRENT_USER whose Path value is edited (default `Environment`, the real user
# environment); only the smoke test sets it, so that it never touches the real PATH.
#
# Uninstall removes exactly what is listed in <prefix>\.turf-install and the one PATH
# entry; it never touches your projects or the package cache (%LOCALAPPDATA%\turf-cache,
# or $env:TURF_CACHE_DIR), which is disposable and not ours.
#
# Windows PowerShell 5.1 (the one in every Windows 10/11) and PowerShell 7 both run this.
# This file is ASCII only on purpose: 5.1 reads a BOM-less file as the ANSI code page.

param(
  [string]$Version = '',
  [string]$Prefix = '',
  [switch]$NoModifyPath,
  [switch]$Uninstall,
  [switch]$Help
)

# What a release archive puts at its top level; install and uninstall touch these names
# and nothing else under the prefix.
$script:Owned = @('bin', 'lib', 'docs', 'share', 'LICENSE', 'README.md')

function Say([string]$Text = '') { Write-Host $Text }

# ---- the user PATH ----------------------------------------------------------------------

function Get-EnvKeyName {
  if ($env:TURF_INSTALL_ENV_KEY) { return $env:TURF_INSTALL_ENV_KEY }
  return 'Environment'
}

# An entry as compared: case-insensitive, with a trailing backslash ignored.
function Get-NormalisedEntry([string]$Entry) { return $Entry.TrimEnd('\').ToLowerInvariant() }

# Writes Path with REG_EXPAND_SZ kept (or chosen, when the value is new), and reads the
# old value WITHOUT expanding %VARIABLES%: reading it expanded and writing it back would
# silently bake every %USERPROFILE% in it into a literal path.
function Update-UserPath([string]$Bin, [bool]$Add) {
  $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey((Get-EnvKeyName))
  try {
    $raw = $key.GetValue('Path', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString
    if ($null -ne $raw) { $kind = $key.GetValueKind('Path') }
    $parts = @()
    if ($raw) { $parts = @($raw.Split(';')) }
    $want = Get-NormalisedEntry $Bin
    $kept = @($parts | Where-Object { (Get-NormalisedEntry $_) -ne $want })
    $changed = $kept.Count -ne $parts.Count
    if ($Add) {
      $new = (@($Bin) + $kept) -join ';'
      $key.SetValue('Path', $new, $kind)
      return $true
    }
    if (-not $changed) { return $false }
    if ($kept.Count -eq 0) { $key.DeleteValue('Path', $false) }
    else { $key.SetValue('Path', ($kept -join ';'), $kind) }
    return $true
  } finally {
    $key.Close()
  }
}

# Tells Explorer and every program listening that the environment changed, so a terminal
# opened from now on sees the new PATH without logging out. Not done for a test key.
function Send-EnvironmentChange {
  if ($env:TURF_INSTALL_ENV_KEY) { return }
  if (-not ('TurfInstall.Native' -as [type])) {
    Add-Type -Namespace TurfInstall -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Auto)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr hWnd, uint Msg, System.UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out System.UIntPtr lpdwResult);
'@
  }
  $result = [UIntPtr]::Zero
  # HWND_BROADCAST, WM_SETTINGCHANGE, SMTO_ABORTIFHUNG; a hung window costs 5 s at most.
  [void][TurfInstall.Native]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$result)
}

# ---- download ---------------------------------------------------------------------------

function Get-RemoteFile([string]$Url, [string]$Dest) {
  # PowerShell 5.1 on older Windows 10 builds defaults to TLS 1.0/1.1, which GitHub refuses.
  try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072 } catch { }
  $lastError = $null
  for ($try = 1; $try -le 3; $try++) {
    $wc = New-Object System.Net.WebClient
    try {
      $wc.DownloadFile($Url, $Dest)
      return
    } catch {
      $lastError = $_.Exception.Message
      if ($try -lt 3) { Start-Sleep -Seconds 2 }
    } finally {
      $wc.Dispose()
    }
  }
  throw "could not download $Url ($lastError)"
}

function Get-Sha256([string]$Path) {
  return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

# ---- the work ---------------------------------------------------------------------------

function Invoke-Uninstall([string]$PrefixDir) {
  $manifest = Join-Path $PrefixDir '.turf-install'
  if (-not (Test-Path -LiteralPath $manifest)) {
    throw "nothing to uninstall: $manifest does not exist (this script did not install into $PrefixDir)."
  }
  foreach ($name in $script:Owned) {
    $p = Join-Path $PrefixDir $name
    if (Test-Path -LiteralPath $p) {
      try { Remove-Item -LiteralPath $p -Recurse -Force }
      catch { throw "could not remove $p ($($_.Exception.Message)). Is turf running? Close it and run the uninstall again." }
      Say "removed $p"
    }
  }
  Remove-Item -LiteralPath $manifest -Force
  try { Remove-Item -LiteralPath $PrefixDir -ErrorAction Stop } catch { }   # only if empty
  if (Update-UserPath (Join-Path $PrefixDir 'bin') $false) {
    Send-EnvironmentChange
    Say 'removed the turf entry from your user PATH'
  }
  Say 'TurfLang is uninstalled. Your projects and the package cache were not touched.'
}

function Invoke-Install([string]$PrefixDir, [string]$Ver, [bool]$EditPath) {
  $base = 'https://github.com/turf-lang/turf-lang/releases'
  if ($env:TURF_INSTALL_BASE_URL) { $base = $env:TURF_INSTALL_BASE_URL.TrimEnd('/') }

  $platform = 'windows-x86_64'
  if ($env:TURF_INSTALL_PLATFORM) {
    $platform = $env:TURF_INSTALL_PLATFORM
  } else {
    $arch = $env:PROCESSOR_ARCHITECTURE
    if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }   # a 32-bit shell on 64-bit Windows
    if ($arch -ne 'AMD64') {
      throw "there is no TurfLang release for Windows on $arch. Supported: Windows x86-64."
    }
  }

  if ($Ver) {
    $tag = $Ver
    if (-not $tag.StartsWith('v')) { $tag = "v$tag" }
    $dl = "$base/download/$tag"
  } else {
    $dl = "$base/latest/download"
  }

  $work = Join-Path ([IO.Path]::GetTempPath()) ('turf-install-' + [Guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $work | Out-Null
  try {
    Say "[1/4] Looking up the release ($dl)"
    $sums = Join-Path $work 'checksums.txt'
    try { Get-RemoteFile "$dl/checksums.txt" $sums }
    catch {
      $hint = ''
      if ($Ver) { $hint = " (is there a release $Ver?)" }
      throw "could not download $dl/checksums.txt$hint $($_.Exception.Message)"
    }

    # The one line for this platform. `-windows-x86_64.zip` cannot match another platform's
    # archive, so a release that grows a second Windows build is never picked by accident.
    $pattern = '^turf-v.*-' + [regex]::Escape($platform) + '\.zip$'
    $lines = @()
    foreach ($row in [IO.File]::ReadAllLines($sums)) {
      $f = @($row.Trim() -split '\s+')
      if ($f.Count -lt 2) { continue }
      $name = $f[1].TrimStart('*')
      if ($name -match $pattern) { $lines += [pscustomobject]@{ Hash = $f[0].ToLowerInvariant(); Name = $name } }
    }
    if ($lines.Count -eq 0) {
      $listed = ([IO.File]::ReadAllLines($sums) | ForEach-Object { ($_.Trim() -split '\s+')[-1] }) -join ' '
      throw "this release has no $platform archive. checksums.txt lists: $listed"
    }
    if ($lines.Count -gt 1) { throw "this release lists more than one $platform archive; refusing to guess." }
    $want = $lines[0].Hash
    $archive = $lines[0].Name

    Say "[2/4] Downloading $archive"
    $zip = Join-Path $work $archive
    Get-RemoteFile "$dl/$archive" $zip
    $got = Get-Sha256 $zip
    if ($got -ne $want) {
      throw "SHA-256 mismatch for ${archive}: the release says $want, the download is $got. Nothing was installed."
    }
    Say '[3/4] SHA-256 verified.'

    Say "[4/4] Unpacking into $PrefixDir"
    $unpacked = Join-Path $work 'unpacked'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $unpacked)
    $srcDirs = @(Get-ChildItem -LiteralPath $unpacked -Directory -Filter 'turf-v*')
    if ($srcDirs.Count -eq 0 -or
        -not (Test-Path -LiteralPath (Join-Path $srcDirs[0].FullName 'bin\turf.exe')) -or
        -not (Test-Path -LiteralPath (Join-Path $srcDirs[0].FullName 'bin\turfc.exe'))) {
      throw "$archive does not have the expected layout (bin\turf.exe, bin\turfc.exe); nothing was installed."
    }
    $src = $srcDirs[0].FullName

    # ---- install ----
    New-Item -ItemType Directory -Force -Path $PrefixDir | Out-Null
    # Move the old install aside and put it back if the new one cannot be moved in, so a
    # failed upgrade (turf.exe still running locks its own files) leaves the previous
    # working install, not half of each.
    $old = Join-Path $work 'old'
    New-Item -ItemType Directory -Path $old | Out-Null
    $restore = {
      foreach ($name in $script:Owned) { Remove-Item -LiteralPath (Join-Path $PrefixDir $name) -Recurse -Force -ErrorAction SilentlyContinue }
      foreach ($name in $script:Owned) {
        $o = Join-Path $old $name
        if (Test-Path -LiteralPath $o) { Move-Item -LiteralPath $o -Destination (Join-Path $PrefixDir $name) -ErrorAction SilentlyContinue }
      }
    }
    try {
      foreach ($name in $script:Owned) {
        $p = Join-Path $PrefixDir $name
        if (Test-Path -LiteralPath $p) { Move-Item -LiteralPath $p -Destination (Join-Path $old $name) }
      }
      foreach ($name in $script:Owned) {
        $s = Join-Path $src $name
        if (Test-Path -LiteralPath $s) { Move-Item -LiteralPath $s -Destination (Join-Path $PrefixDir $name) }
      }
    } catch {
      $why = $_.Exception.Message
      & $restore
      throw "could not move the new files into $PrefixDir ($why). The previous install (if any) was restored. If turf is running, close it and try again."
    }

    $installed = ''
    try { $installed = (@(& (Join-Path $PrefixDir 'bin\turfc.exe') --version 2>$null))[0] } catch { }
    [IO.File]::WriteAllText((Join-Path $PrefixDir '.turf-install'), "archive=$archive`ncompiler=$installed`n")
    Say "Installed $installed into $PrefixDir"
  } finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
  }

  # ---- PATH ----
  $bin = Join-Path $PrefixDir 'bin'
  if ($EditPath) {
    [void](Update-UserPath $bin $true)
    Send-EnvironmentChange
    # This session too, so a script that runs the installer can call turf straight after.
    if (-not (($env:Path -split ';') | Where-Object { (Get-NormalisedEntry $_) -eq (Get-NormalisedEntry $bin) })) {
      $env:Path = "$bin;$env:Path"
    }
    Say "Added $bin to your user PATH."
    Say 'Open a new terminal to use it (this one already has it).'
  } else {
    Say 'Not changing your PATH (-NoModifyPath). To use turf in a terminal, run:'
    Say "  `$env:Path = `"$bin;`$env:Path`""
  }
  Say ''
  Say 'Try it:  turf new hello; cd hello; turf run'
}

function Invoke-Main {
  $ErrorActionPreference = 'Stop'
  Set-StrictMode -Version Latest

  if ($Help) {
    $self = $PSCommandPath
    if ($self) { Get-Content -LiteralPath $self | Select-Object -First 36 | ForEach-Object { $_ -replace '^# ?', '' } }
    else { Say 'install.ps1: see the comment at the top of the file (https://github.com/turf-lang/turf-lang).' }
    return
  }

  $ver = $Version
  if (-not $ver -and $env:TURF_INSTALL_VERSION) { $ver = $env:TURF_INSTALL_VERSION }
  $prefixDir = $Prefix
  if (-not $prefixDir -and $env:TURF_INSTALL_DIR) { $prefixDir = $env:TURF_INSTALL_DIR }
  if (-not $prefixDir) {
    if (-not $env:LOCALAPPDATA) { throw 'LOCALAPPDATA is not set; pass -Prefix DIR.' }
    $prefixDir = Join-Path $env:LOCALAPPDATA 'turf'
  }
  $prefixDir = [IO.Path]::GetFullPath($prefixDir)

  if ($Uninstall) { Invoke-Uninstall $prefixDir; return }
  Invoke-Install $prefixDir $ver (-not $NoModifyPath)
}

try {
  Invoke-Main
} catch {
  [Console]::Error.WriteLine("install.ps1: $($_.Exception.Message)")
  # `exit` in a script that was piped to iex would close the user's terminal.
  if ($PSCommandPath) { exit 1 }
}
