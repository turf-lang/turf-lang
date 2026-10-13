# smoke_common.ps1 -- shared by scripts\smoke\*_smoke.ps1, the Windows twins of the bash
# smoke scripts (REL-008 stage 5). Dot-source it: `. "$PSScriptRoot\smoke_common.ps1"`.
#
# Why twins and not "run the .sh under Git Bash": those scripts assume a POSIX host
# (mktemp's /tmp paths that a native .exe cannot open, shell-script fakes, symlinks,
# /usr/bin/time, exit 134 for abort). Each .ps1 checks the same things as its .sh,
# against the same files, with the Windows spelling of the host parts; where a bash
# check has no Windows meaning (a POSIX signal, a symlink) the .ps1 says so by name
# and skips it, never silently.
#
# Written for Windows PowerShell 5.1 (what ships with Windows) and also runs under
# PowerShell 7. ASCII only, so the file's encoding cannot matter.

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$script:Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $script:Root
# turf's source: cli\, or a turf-cli checkout once cli\ is retired (CMake's TURF_CLI_DIR).
$script:CliDir = if ($env:TURF_CLI_DIR) { $env:TURF_CLI_DIR } else { Join-Path $script:Root 'cli' }
if (-not (Test-Path -LiteralPath (Join-Path $script:CliDir 'go.mod'))) { $script:CliDir = Join-Path $script:Root '..\turf-cli' }
$script:Turfc = Join-Path $script:Root 'build\turfc.exe'
$script:Turf = Join-Path $script:Root 'build\turf.exe'
$script:Failed = 0
$script:WorkDirs = @()
$script:SmokeName = ''

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace TurfSmoke {
  public class ProcResult {
    public string Out;
    public string Err;
    public long Rc;        // the exit code as an unsigned 32-bit value (a crash is 0xC0000005, not -1073741819)
    public bool TimedOut;
    public long PeakKB;    // peak working set of the process, in KB
  }

  public static class Proc {
    [StructLayout(LayoutKind.Sequential)]
    struct PMC {
      public uint cb, PageFaultCount;
      public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
                     QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
    }
    [DllImport("psapi.dll", SetLastError = true)]
    static extern bool GetProcessMemoryInfo(IntPtr h, out PMC counters, uint size);

    // Win32 command-line quoting (CommandLineToArgvW's rules), since 5.1's .NET has no ArgumentList.
    public static string Quote(string a) {
      if (a.Length > 0 && a.IndexOfAny(new char[] { ' ', '\t', '\n', '\v', '"' }) < 0) return a;
      var sb = new StringBuilder("\"");
      int slashes = 0;
      foreach (char c in a) {
        if (c == '\\') { slashes++; continue; }
        if (c == '"') { sb.Append('\\', slashes * 2 + 1); sb.Append('"'); }
        else { sb.Append('\\', slashes); sb.Append(c); }
        slashes = 0;
      }
      sb.Append('\\', slashes * 2);
      sb.Append('"');
      return sb.ToString();
    }

    public static Running Start(string file, string[] args, string cwd, Dictionary<string, string> env,
                                string stdin) {
      var psi = new ProcessStartInfo(file);
      var parts = new List<string>();
      foreach (var a in args) parts.Add(Quote(a));
      psi.Arguments = string.Join(" ", parts.ToArray());
      if (cwd != null) psi.WorkingDirectory = cwd;
      psi.UseShellExecute = false;
      psi.CreateNoWindow = true;
      psi.RedirectStandardOutput = true;
      psi.RedirectStandardError = true;
      psi.RedirectStandardInput = true;
      psi.StandardOutputEncoding = new UTF8Encoding(false);
      psi.StandardErrorEncoding = new UTF8Encoding(false);
      if (env != null)
        foreach (var kv in env) {
          if (kv.Value == null) psi.EnvironmentVariables.Remove(kv.Key);
          else psi.EnvironmentVariables[kv.Key] = kv.Value;
        }
      var run = new Running();
      run.P = Process.Start(psi);
      run.OutTask = run.P.StandardOutput.ReadToEndAsync();
      run.ErrTask = run.P.StandardError.ReadToEndAsync();
      try {
        if (stdin != null) run.P.StandardInput.Write(stdin);
        run.P.StandardInput.Close();
      } catch (IOException) { /* the program did not read its stdin */ }
      return run;
    }

    public static ProcResult Run(string file, string[] args, string cwd, Dictionary<string, string> env,
                                 int timeoutSec, string stdin) {
      return Start(file, args, cwd, env, stdin).Finish(timeoutSec);
    }

    // A started process: Finish() waits (or kills the process tree at the timeout) and collects everything.
    public class Running {
      public Process P;
      public System.Threading.Tasks.Task<string> OutTask, ErrTask;

      public ProcResult Finish(int timeoutSec) {
        var r = new ProcResult();
        if (!P.WaitForExit(timeoutSec * 1000)) {
          r.TimedOut = true;
          Kill();
          P.WaitForExit();
        }
        r.Out = OutTask.Result;
        r.Err = ErrTask.Result;
        r.Rc = (long)(uint)P.ExitCode;
        PMC c;
        if (GetProcessMemoryInfo(P.Handle, out c, (uint)Marshal.SizeOf(typeof(PMC))))
          r.PeakKB = (long)((ulong)c.PeakWorkingSetSize / 1024UL);
        P.Dispose();
        return r;
      }

      public void Kill() {
        try {
          var k = Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + P.Id) { CreateNoWindow = true, UseShellExecute = false });
          k.WaitForExit();
        } catch (Exception) { }
      }
    }
  }
}
'@

# --- reporting ----------------------------------------------------------------------

function Start-Smoke([string]$Name) {
  $script:SmokeName = $Name
  Write-Host $Name -ForegroundColor White
  Write-Host ''
}
function Write-Pass([string]$Msg) { Write-Host "  ok   $Msg" -ForegroundColor Green }
function Write-Fail([string]$Msg) {
  Write-Host "  FAIL $Msg" -ForegroundColor Red
  $script:Failed++
}
function Write-Skip([string]$Msg) { Write-Host "  skip $Msg" -ForegroundColor Yellow }
function Write-Heading([string]$Msg) { Write-Host $Msg -ForegroundColor White }
function Write-Indented([string]$Text) {
  foreach ($l in ($Text -split "`r?`n")) { Write-Host "       $l" }
}
function Remove-Ansi([string]$Text) { return [regex]::Replace($Text, "\x1b\[[0-9;]*m", '') }

# Ends the script with the bash twins' convention: 0 = clean, 1 = something failed.
function Complete-Smoke {
  foreach ($d in $script:WorkDirs) { Remove-Item -Recurse -Force -LiteralPath $d -ErrorAction SilentlyContinue }
  Write-Host ''
  if ($script:Failed -eq 0) {
    Write-Host "$($script:SmokeName): clean." -ForegroundColor Green
    exit 0
  }
  Write-Host "$($script:SmokeName): FAILED ($($script:Failed) check(s))." -ForegroundColor Red
  exit 1
}

# --- environment --------------------------------------------------------------------

function Assert-Built {
  if (-not (Test-Path -LiteralPath $script:Turfc)) {
    Write-Host "build\turfc.exe missing -- build before running this smoke test." -ForegroundColor Red
    exit 1
  }
}

function New-WorkDir {
  $d = Join-Path ([System.IO.Path]::GetTempPath()) ('turfsmoke-' + [guid]::NewGuid().ToString('N').Substring(0, 12))
  New-Item -ItemType Directory -Path $d | Out-Null
  $script:WorkDirs += $d
  return $d
}

# The environment a smoke test runs in. A developer's shell often has TURF_HOME or
# TURF_COMPILER pointing at another checkout; neither may decide a result.
function Get-CleanEnv([hashtable]$Extra) {
  $e = New-Object 'System.Collections.Generic.Dictionary[string,string]'
  $e['TURF_HOME'] = $null
  $e['TURF_COMPILER'] = $null
  if ($Extra) { foreach ($k in $Extra.Keys) { $e[$k] = $Extra[$k] } }
  return $e
}

# Invoke-Proc <exe> [args] -> object with Out, Err, Rc (unsigned), TimedOut, PeakKB.
# Never throws on a non-zero exit: the caller states what exit it expects.
function Invoke-Proc {
  param(
    [Parameter(Mandatory = $true)][string]$File,
    [string[]]$ArgList = @(),
    [string]$Cwd = $script:Root,
    [hashtable]$Env = $null,
    [int]$TimeoutSec = 600,
    [string]$StdIn = $null
  )
  $e = Get-CleanEnv $Env
  return [TurfSmoke.Proc]::Run($File, [string[]]$ArgList, $Cwd, $e, $TimeoutSec, $StdIn)
}

# Start-Proc: the same, but not waited for. Call .Finish(<timeout seconds>) on the result
# for the Invoke-Proc object (or .Kill() to give up on it).
function Start-Proc {
  param(
    [Parameter(Mandatory = $true)][string]$File,
    [string[]]$ArgList = @(),
    [string]$Cwd = $script:Root,
    [hashtable]$Env = $null
  )
  $e = Get-CleanEnv $Env
  return [TurfSmoke.Proc]::Start($File, [string[]]$ArgList, $Cwd, $e, $null)
}

function Invoke-Turfc {
  param([string[]]$ArgList = @(), [string]$Cwd = $script:Root, [hashtable]$Env = $null, [int]$TimeoutSec = 600)
  return Invoke-Proc -File $script:Turfc -ArgList $ArgList -Cwd $Cwd -Env $Env -TimeoutSec $TimeoutSec
}

function Invoke-Turf {
  param([string[]]$ArgList = @(), [string]$Cwd = $script:Root, [hashtable]$Env = $null, [int]$TimeoutSec = 600)
  return Invoke-Proc -File $script:Turf -ArgList $ArgList -Cwd $Cwd -Env $Env -TimeoutSec $TimeoutSec
}

# What a Turf program that called abort() exits with on Windows (runtime/turfrt_signal.c
# defines it; WindowsExitStatus() in src/tooling/TestRunner.cpp reads it). The bash twins say 134.
$script:AbortStatus = [Convert]::ToInt64('E0545254', 16)  # not a 0x literal: PowerShell reads those as signed 32-bit

# Writes text as UTF-8 without a BOM, with LF line endings kept as given.
function Write-TextFile([string]$Path, [string]$Text) {
  $dir = Split-Path -Parent $Path
  if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
  [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}
