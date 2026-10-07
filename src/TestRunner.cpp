// REL-002: `turfc --test` (TestRunner.h). Discovers the test files, runs each
// as a compile + run in child processes, judges the outcome by the file's name
// and its `// EXPECT-*` markers (TestExpectations.cpp), and prints the report
// scripts/test_runner.sh printed before it became a wrapper over this.

#include "TestRunner.h"

#include "TestExpectations.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#else
// REL-008 stage 5: the Windows process runner. WIN32_LEAN_AND_MEAN keeps
// winsock.h out (nothing here needs it, and it would clash with a later
// winsock2.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10: the attribute-list and job APIs below
#endif
#include <io.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr int DefaultTimeoutSec = 5;
/// A compile that has not finished after this long is a hung compiler, not a
/// slow one: the whole suite compiles in about a minute.
constexpr int CompileTimeoutSec = 300;
constexpr size_t MaxCapturedBytes = 4u << 20;

const char *const Bold = "\033[1m";
const char *const Green = "\033[0;32m";
const char *const Red = "\033[0;31m";
const char *const Yellow = "\033[0;33m";
const char *const Cyan = "\033[0;36m";
const char *const Reset = "\033[0m";

struct Palette {
  bool On = false;
  std::string bold(const std::string &S) const { return wrap(Bold, S); }
  std::string green(const std::string &S) const { return wrap(Green, S); }
  std::string red(const std::string &S) const { return wrap(Red, S); }
  std::string yellow(const std::string &S) const { return wrap(Yellow, S); }
  std::string cyan(const std::string &S) const { return wrap(Cyan, S); }

private:
  std::string wrap(const char *Code, const std::string &S) const { return On ? Code + S + Reset : S; }
};

struct ProcResult {
  /// The exit status, or 128 + N when signal N ended the process: the number a
  /// shell would show, which is what the markers and old runner spoke in.
  int Status = 0;
  bool TimedOut = false;
  std::string Output; ///< stdout and stderr together
};

#ifndef _WIN32

std::atomic<bool> g_Interrupted{false};

extern "C" void onInterrupt(int) { g_Interrupted = true; }

/// Runs @p Exe with @p Args in directory @p Cwd, stdin from /dev/null, and
/// collects its output. A positive @p TimeoutSec kills the whole process
/// group when it expires: a test that spawned a helper must not leave it
/// behind. The caller's Ctrl-C does the same, since the child is in its own
/// group and would otherwise never see it.
ProcResult RunProcess(const std::string &Exe, const std::vector<std::string> &Args,
                      const std::string &Cwd, int TimeoutSec) {
  ProcResult R;
  int Pipe[2];
  if (pipe(Pipe) != 0) {
    R.Status = 127;
    R.Output = "cannot create a pipe";
    return R;
  }
  fcntl(Pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(Pipe[1], F_SETFD, FD_CLOEXEC);

  // Everything the child needs is built before fork(): only
  // async-signal-safe calls may run between fork and exec in a process that
  // has other threads.
  std::vector<std::string> Strings;
  Strings.push_back(Exe);
  Strings.insert(Strings.end(), Args.begin(), Args.end());
  std::vector<char *> Argv;
  for (auto &S : Strings)
    Argv.push_back(S.data());
  Argv.push_back(nullptr);

  const pid_t Pid = fork();
  if (Pid < 0) {
    close(Pipe[0]);
    close(Pipe[1]);
    R.Status = 127;
    R.Output = "cannot start a process";
    return R;
  }
  if (Pid == 0) {
    setpgid(0, 0);
    if (!Cwd.empty() && chdir(Cwd.c_str()) != 0)
      _exit(127);
    int Nul = open("/dev/null", O_RDONLY);
    if (Nul >= 0)
      dup2(Nul, 0);
    dup2(Pipe[1], 1);
    dup2(Pipe[1], 2);
    execv(Exe.c_str(), Argv.data());
    _exit(127);
  }
  setpgid(Pid, Pid); // also here: whichever runs first wins the race
  close(Pipe[1]);
  fcntl(Pipe[0], F_SETFL, fcntl(Pipe[0], F_GETFL) | O_NONBLOCK);

  using Clock = std::chrono::steady_clock;
  const auto Start = Clock::now();
  bool Killed = false, Eof = false, Reaped = false;
  int WaitStatus = 0;
  char Buf[65536];
  auto Drain = [&] {
    for (;;) {
      ssize_t Got = read(Pipe[0], Buf, sizeof Buf);
      if (Got > 0) {
        if (R.Output.size() < MaxCapturedBytes)
          R.Output.append(Buf, static_cast<size_t>(Got));
        continue;
      }
      if (Got == 0)
        Eof = true;
      return; // EAGAIN (nothing more yet), EOF or an error
    }
  };

  while (!Reaped) {
    if (!Killed) {
      const bool Expired =
          TimeoutSec > 0 && Clock::now() - Start >= std::chrono::seconds(TimeoutSec);
      if (Expired || g_Interrupted) {
        killpg(Pid, SIGKILL);
        Killed = true;
        R.TimedOut = Expired;
      }
    }
    if (!Eof) {
      pollfd P{Pipe[0], POLLIN, 0};
      poll(&P, 1, 50);
      Drain();
    } else {
      poll(nullptr, 0, 20);
    }
    if (waitpid(Pid, &WaitStatus, WNOHANG) == Pid)
      Reaped = true;
  }
  Drain(); // what the process wrote just before it exited
  close(Pipe[0]);
  killpg(Pid, SIGKILL); // a helper the test left running (ESRCH when none)

  if (WIFEXITED(WaitStatus))
    R.Status = WEXITSTATUS(WaitStatus);
  else if (WIFSIGNALED(WaitStatus))
    R.Status = 128 + WTERMSIG(WaitStatus);
  return R;
}

#else // _WIN32

// REL-008 stage 5. The same contract as the POSIX RunProcess above, in Win32
// terms: a child with stdin from NUL and stdout+stderr in one pipe, a working
// directory, a timeout, the caller's Ctrl-C, and the child's whole process TREE
// killed on either (a Job Object does for the tree what a process group does
// on POSIX), so a test that started a helper cannot leave it behind.

std::atomic<bool> g_Interrupted{false};

BOOL WINAPI onConsoleControl(DWORD Type) {
  if (Type == CTRL_C_EVENT || Type == CTRL_BREAK_EVENT || Type == CTRL_CLOSE_EVENT) {
    g_Interrupted = true;
    return TRUE;
  }
  return FALSE;
}

std::wstring Widen(const std::string &S) {
  if (S.empty())
    return {};
  const int N = MultiByteToWideChar(CP_UTF8, 0, S.data(), static_cast<int>(S.size()), nullptr, 0);
  std::wstring W(static_cast<size_t>(N), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, S.data(), static_cast<int>(S.size()), W.data(), N);
  return W;
}

/// One argument, quoted so the C runtime's startup code (and
/// CommandLineToArgvW) parses it back to exactly @p A: wrap in quotes when it
/// holds whitespace, a quote, or nothing; double the backslashes that precede
/// a quote or the closing quote; escape each quote.
std::wstring QuoteArg(const std::wstring &A) {
  if (!A.empty() && A.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    return A;
  std::wstring Out = L"\"";
  for (size_t I = 0;; ++I) {
    size_t Slashes = 0;
    while (I < A.size() && A[I] == L'\\') {
      ++Slashes;
      ++I;
    }
    if (I == A.size()) {
      Out.append(Slashes * 2, L'\\');
      break;
    }
    if (A[I] == L'"') {
      Out.append(Slashes * 2 + 1, L'\\');
      Out.push_back(L'"');
    } else {
      Out.append(Slashes, L'\\');
      Out.push_back(A[I]);
    }
  }
  Out.push_back(L'"');
  return Out;
}

/// A Windows exit code in the vocabulary the markers and the rest of the runner
/// speak: 128 + N for "ended by signal N". The C runtime's abort() exits with 3
/// (SIGABRT, 134), and an unhandled exception ends the process with its NTSTATUS
/// (an access violation or a stack overflow is SIGSEGV, 139). Anything else is
/// the program's own exit code, except an unknown NTSTATUS, which is a crash.
int WindowsExitStatus(DWORD Code) {
  switch (Code) {
  case 3:
    return 134; // abort()
  case 0xC0000005: // STATUS_ACCESS_VIOLATION
  case 0xC00000FD: // STATUS_STACK_OVERFLOW
  case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN (__fastfail)
    return 139;
  case 0xC000001D: // STATUS_ILLEGAL_INSTRUCTION
  case 0xC0000096: // STATUS_PRIVILEGED_INSTRUCTION
    return 132;
  case 0xC0000094: // STATUS_INTEGER_DIVIDE_BY_ZERO
  case 0xC0000095: // STATUS_INTEGER_OVERFLOW
  case 0xC000008E: // STATUS_FLOAT_DIVIDE_BY_ZERO
    return 136;
  case 0xC000013A: // STATUS_CONTROL_C_EXIT
    return 130;
  default:
    break;
  }
  if (Code >= 0xC0000000u)
    return 255; // some other unhandled exception
  return static_cast<int>(Code);
}

ProcResult RunProcess(const std::string &Exe, const std::vector<std::string> &Args,
                      const std::string &Cwd, int TimeoutSec) {
  ProcResult R;
  auto Fail = [&](const char *What) {
    R.Status = 127;
    R.Output = std::string(What) + " (Win32 error " + std::to_string(GetLastError()) + ")";
    return R;
  };

  SECURITY_ATTRIBUTES Inheritable{sizeof Inheritable, nullptr, TRUE};
  HANDLE ReadEnd = nullptr, WriteEnd = nullptr;
  if (!CreatePipe(&ReadEnd, &WriteEnd, &Inheritable, 0))
    return Fail("cannot create a pipe");
  SetHandleInformation(ReadEnd, HANDLE_FLAG_INHERIT, 0); // the child gets only the write end
  HANDLE Nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &Inheritable,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  // Only these two handles are inherited. Without an explicit list, "inherit
  // every inheritable handle" lets a test process started by another worker
  // thread at the same moment inherit THIS pipe's write end, and this pipe then
  // never reaches end-of-file while that unrelated process lives.
  HANDLE Inherit[2] = {WriteEnd, Nul};
  const DWORD InheritCount = Nul != INVALID_HANDLE_VALUE ? 2 : 1;
  SIZE_T AttrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &AttrSize);
  std::vector<char> AttrBuf(AttrSize);
  auto *Attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttrBuf.data());
  InitializeProcThreadAttributeList(Attrs, 1, 0, &AttrSize);
  UpdateProcThreadAttribute(Attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Inherit,
                            InheritCount * sizeof(HANDLE), nullptr, nullptr);

  STARTUPINFOEXW SI{};
  SI.StartupInfo.cb = sizeof SI;
  SI.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  SI.StartupInfo.hStdInput = Nul != INVALID_HANDLE_VALUE ? Nul : GetStdHandle(STD_INPUT_HANDLE);
  SI.StartupInfo.hStdOutput = WriteEnd;
  SI.StartupInfo.hStdError = WriteEnd;
  SI.lpAttributeList = Attrs;

  std::wstring Cmd = QuoteArg(Widen(Exe));
  for (const auto &A : Args)
    Cmd += L" " + QuoteArg(Widen(A));
  const std::wstring CwdW = Widen(Cwd);

  PROCESS_INFORMATION PI{};
  // Suspended, so it is in the job before it can start a child of its own.
  const BOOL Started =
      CreateProcessW(nullptr, Cmd.data(), nullptr, nullptr, TRUE,
                     EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                     CwdW.empty() ? nullptr : CwdW.c_str(), &SI.StartupInfo, &PI);
  const DWORD StartError = GetLastError();
  DeleteProcThreadAttributeList(Attrs);
  CloseHandle(WriteEnd); // the child holds its own copy
  if (Nul != INVALID_HANDLE_VALUE)
    CloseHandle(Nul);
  if (!Started) {
    CloseHandle(ReadEnd);
    R.Status = 127;
    R.Output = "cannot start '" + Exe + "' (Win32 error " + std::to_string(StartError) + ")";
    return R;
  }

  HANDLE Job = CreateJobObjectW(nullptr, nullptr);
  if (Job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Info{};
    Info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Info, sizeof Info);
    AssignProcessToJobObject(Job, PI.hProcess);
  }
  ResumeThread(PI.hThread);

  using Clock = std::chrono::steady_clock;
  const auto Start = Clock::now();
  bool Killed = false;
  char Buf[65536];
  // Reads whatever the pipe holds now; false when there was nothing.
  auto Drain = [&]() -> bool {
    DWORD Avail = 0;
    if (!PeekNamedPipe(ReadEnd, nullptr, 0, nullptr, &Avail, nullptr) || Avail == 0)
      return false;
    DWORD Got = 0;
    if (!ReadFile(ReadEnd, Buf, static_cast<DWORD>(std::min<size_t>(sizeof Buf, Avail)), &Got, nullptr) || Got == 0)
      return false;
    if (R.Output.size() < MaxCapturedBytes)
      R.Output.append(Buf, Got);
    return true;
  };

  for (;;) {
    if (Drain())
      continue;
    if (!Killed) {
      const bool Expired = TimeoutSec > 0 && Clock::now() - Start >= std::chrono::seconds(TimeoutSec);
      if (Expired || g_Interrupted) {
        if (Job)
          TerminateJobObject(Job, 137);
        else
          TerminateProcess(PI.hProcess, 137);
        Killed = true;
        R.TimedOut = Expired;
      }
    }
    if (WaitForSingleObject(PI.hProcess, 20) == WAIT_OBJECT_0)
      break;
  }
  while (Drain()) {
  } // what the process wrote just before it exited
  if (Job)
    TerminateJobObject(Job, 137); // a helper the test left running

  DWORD Code = 0;
  GetExitCodeProcess(PI.hProcess, &Code);
  R.Status = Killed ? 137 : WindowsExitStatus(Code);

  CloseHandle(ReadEnd);
  CloseHandle(PI.hThread);
  CloseHandle(PI.hProcess);
  if (Job)
    CloseHandle(Job);
  return R;
}

#endif // !_WIN32

std::string ReadFile(const fs::path &P) {
  std::ifstream In(P, std::ios::binary);
  std::ostringstream S;
  S << In.rdbuf();
  return S.str();
}

bool EndsWith(const std::string &S, const std::string &Suffix) {
  return S.size() >= Suffix.size() && S.compare(S.size() - Suffix.size(), Suffix.size(), Suffix) == 0;
}

bool StartsWith(const std::string &S, const std::string &Prefix) {
  return S.compare(0, Prefix.size(), Prefix) == 0;
}

struct TestFile {
  std::string Rel;  ///< path under the tests directory, '/'-separated
  std::string Key;  ///< the register's spelling: `<Dir>/<Rel>`
  fs::path Path;    ///< absolute
  std::string Suite() const { return Path.parent_path().filename().string(); }
  std::string Name() const { return Path.filename().string(); }
};

struct TestResult {
  bool Pass = false;
  /// Printed under "Failed Tests Details" for a failure.
  std::string Detail;
};

/// Joins output lines for a failure's detail block.
std::string JoinLines(const std::vector<std::string> &Lines) {
  std::string Out;
  for (const auto &L : Lines)
    Out += (Out.empty() ? "" : "\n") + L;
  return Out;
}

class Runner {
public:
  Runner(const TestRunOptions &O, std::vector<TestFile> Tests, fs::path TempDir, int Jobs,
         int Timeout, const TestExpect::Register *Reg, Palette Pal)
      : O_(O), Tests_(std::move(Tests)), Temp_(std::move(TempDir)), Jobs_(Jobs), Timeout_(Timeout),
        Reg_(Reg), Pal_(Pal) {}

  /// Runs everything; the results are in input order.
  std::vector<TestResult> runAll() {
    std::vector<TestResult> Results(Tests_.size());
    std::atomic<size_t> Next{0};
    auto Worker = [&] {
      for (;;) {
        size_t I = Next++;
        if (I >= Tests_.size() || g_Interrupted)
          return;
        Results[I] = runOne(I);
      }
    };
    std::vector<std::thread> Pool;
    for (int J = 0; J < Jobs_; ++J)
      Pool.emplace_back(Worker);
    for (auto &T : Pool)
      T.join();
    return Results;
  }

private:
  const TestRunOptions &O_;
  std::vector<TestFile> Tests_;
  fs::path Temp_;
  int Jobs_, Timeout_;
  const TestExpect::Register *Reg_;
  Palette Pal_;
  std::mutex PrintMu_;

  void say(const TestFile &T, const std::string &Status) {
    std::lock_guard<std::mutex> L(PrintMu_);
    std::cout << "[" << Pal_.cyan(T.Suite()) << "] " << T.Name() << " ... " << Status << std::endl;
  }

  void sayExtra(const std::string &Text) {
    std::lock_guard<std::mutex> L(PrintMu_);
    std::cout << Text << std::endl;
  }

  TestResult fail(const TestFile &T, const std::string &Headline, const std::string &Detail) {
    say(T, Pal_.red(Headline));
    return {false, T.Key + " " + Detail};
  }

  TestResult pass(const TestFile &T, const std::string &Note = "") {
    say(T, Pal_.green("PASS" + Note));
    return {true, ""};
  }

  ProcResult compile(const TestFile &T, const fs::path &Program) {
    std::vector<std::string> Args;
    if (!O_.PackageGraph.empty()) {
      Args.push_back("--package-graph");
      Args.push_back(O_.PackageGraph);
    }
    Args.push_back(T.Name());
    Args.push_back("-o");
    Args.push_back(Program.string());
    Args.insert(Args.end(), O_.CompileFlags.begin(), O_.CompileFlags.end());
    return RunProcess(O_.Compiler, Args, T.Path.parent_path().string(), CompileTimeoutSec);
  }

  ProcResult runProgram(const TestFile &T, const fs::path &Program) {
    return RunProcess(Program.string(), {}, T.Path.parent_path().string(), Timeout_);
  }

  /// The detail block of a failure that quotes the lines of a check followed by
  /// the output it checked.
  static std::string With(const std::vector<std::string> &Lines, const std::string &Output) {
    return JoinLines(Lines) + (Output.empty() ? "" : "\n" + Output);
  }

  TestResult runOne(size_t Index) {
    const TestFile &T = Tests_[Index];
    const std::string Source = ReadFile(T.Path);
#ifdef _WIN32
    // turfc writes <name>.exe for `-o <name>` (and keeps a name that already ends
    // in .exe), so name the program with the suffix the existence checks and the
    // run below look for.
    const fs::path Program = Temp_ / ("t" + std::to_string(Index) + ".exe");
#else
    const fs::path Program = Temp_ / ("t" + std::to_string(Index));
#endif
    struct Cleanup {
      fs::path P;
      ~Cleanup() {
        std::error_code EC;
        fs::remove(P, EC);
      }
    } Clean{Program};

    const bool FailPrefix = StartsWith(T.Name(), "fail_");
    ProcResult C = compile(T, Program);
    if (g_Interrupted)
      return {false, T.Key + " (interrupted)"};

    // fail_*.tr with RUNTIME_ABORT_EXPECTED: not about rejecting the input at
    // all -- it checks a stdlib guard that calls fail() at RUNTIME in code that
    // compiles. Without the opt-in marker such a test could "pass" forever
    // because an unrelated compile error (a typo'd import) stopped it before
    // the behaviour under test was reached.
    if (FailPrefix && TestExpect::IsRuntimeAbortTest(Source)) {
      if (C.Status != 0)
        return fail(T, "FAILED (Expected to compile successfully, but got a compile error)",
                    "(Unexpected compile error):\n" + C.Output);
      std::error_code EC;
      if (!fs::exists(Program, EC))
        return fail(T, "NO EXECUTABLE PRODUCED", "(No executable)");
      if (auto Miss = TestExpect::CheckDiag(Source, C.Output); !Miss.empty())
        return fail(T, "FAILED (EXPECT-DIAG)", "(EXPECT-DIAG):\n" + With(Miss, C.Output));
      ProcResult R = runProgram(T, Program);
      if (R.Status == 0 || R.Status == 137 || R.Status == 143)
        return fail(T, "FAILED (Expected a runtime abort, got exit " + std::to_string(R.Status) + ")",
                    "(Expected runtime abort, got exit " + std::to_string(R.Status) + "):\n" + R.Output);
      if (auto Miss = TestExpect::CheckRuntime(Source, R.Output); !Miss.empty())
        return fail(T, "FAILED (Aborted, but EXPECT-RUNTIME not met)",
                    "(Aborted at runtime, but EXPECT-RUNTIME not met):\n" + With(Miss, R.Output));
      if (auto Miss = TestExpect::CheckExit(Source, R.Status); !Miss.empty())
        return fail(T, "FAILED (Aborted, but EXPECT-EXIT not met)",
                    "(Aborted at runtime, but EXPECT-EXIT not met):\n" + With(Miss, R.Output));
      if (Reg_)
        if (auto Miss = Reg_->checkRuntimePolicy(Source, T.Key); !Miss.empty())
          return fail(T, "FAILED (Aborted, but its reason is not pinned: see DIAGNOSTIC_GAPS.txt)",
                      "(Diagnostic policy, tests/DIAGNOSTIC_GAPS.txt):\n" + JoinLines(Miss));
      return pass(T, " (Aborted at runtime as expected)");
    }

    if (FailPrefix) {
      if (C.Status == 0)
        return fail(T, "FAILED (Should have failed compilation)", "(Compiled successfully but expected to fail)");
      // A status >= 128 means the compiler died from a signal rather than
      // reporting a diagnostic. A fail_* test's point is that the compiler
      // REJECTS the input; a crash is a compiler bug that merely also exits
      // non-zero, and counting it as a pass would hide a regression to the
      // crash forever.
      if (C.Status >= 128)
        return fail(T,
                    "FAILED (Compiler crashed, signal " + std::to_string(C.Status - 128) +
                        ", instead of rejecting cleanly)",
                    "(Compiler crashed with exit " + std::to_string(C.Status) +
                        " instead of a clean diagnostic):\n" + C.Output);
      if (auto Miss = TestExpect::CheckDiag(Source, C.Output); !Miss.empty())
        return fail(T, "FAILED (Rejected, but EXPECT-DIAG not met)",
                    "(Rejected, but EXPECT-DIAG not met):\n" + With(Miss, C.Output));
      if (Reg_)
        if (auto Miss = Reg_->checkDiagPolicy(Source, T.Key, C.Output); !Miss.empty())
          return fail(T, "FAILED (Rejected, but its reason is not pinned: see DIAGNOSTIC_GAPS.txt)",
                      "(Diagnostic policy, tests/DIAGNOSTIC_GAPS.txt):\n" + JoinLines(Miss));
      return pass(T, " (Rejected properly)");
    }

    if (C.Status != 0) {
      // The exit status is the only clue when the compiler dies from a signal
      // with no output at all.
      return fail(T, C.TimedOut ? "COMPILE TIMED OUT" : "COMPILE FAILED",
                  "(Compile Error, exit " + std::to_string(C.Status) + "):\n" + C.Output);
    }
    std::error_code EC;
    if (!fs::exists(Program, EC))
      return fail(T, "NO EXECUTABLE PRODUCED", "(No executable)");
    // A plain test compiles, so what it can pin is its WARNINGS (or their
    // absence), judged on the compile output before the program runs.
    if (auto Miss = TestExpect::CheckDiag(Source, C.Output); !Miss.empty())
      return fail(T, "FAILED (EXPECT-DIAG)", "(EXPECT-DIAG):\n" + With(Miss, C.Output));

    ProcResult R = runProgram(T, Program);
    if (R.TimedOut || R.Status == 137 || R.Status == 143 || R.Status == 211) {
      const char *Stress = std::getenv("TURF_GC_STRESS");
      if (Stress && *Stress && std::string(Stress) != "0") {
        // Under TURF_GC_STRESS a collection per allocation makes an
        // allocation-heavy test slow, not hung.
        say(T, Pal_.yellow("TIMEOUT (TURF_GC_STRESS is on: possibly just slow, see TURF_TEST_TIMEOUT_SEC)"));
        return {false, T.Key + " (Timed out after " + std::to_string(Timeout_) +
                           "s under TURF_GC_STRESS -- a collection per allocation makes allocation-heavy "
                           "tests slow, not hung: time it with a larger TURF_TEST_TIMEOUT_SEC before "
                           "assuming a loop)"};
      }
      say(T, Pal_.yellow("TIMEOUT (Likely infinite loop)"));
      return {false, T.Key + " (Timed out after " + std::to_string(Timeout_) + "s)"};
    }
    if (R.Status != 0) {
      say(T, Pal_.red("RUNTIME FAILED") + " (Exit code " + std::to_string(R.Status) + ")");
      return {false, T.Key + " (Runtime Error exit " + std::to_string(R.Status) + "):\n" + R.Output};
    }
    TestResult Done = pass(T);
    // A passing test's output is otherwise discarded, which would hide
    // TURF_GC_VALIDATE's report-only findings (runtime/turfrt_gc.c) from the
    // CI step that greps for them. Nothing else prints this prefix.
    std::istringstream Lines(R.Output);
    for (std::string L; std::getline(Lines, L);)
      if (StartsWith(L, "TURF_GC_VALIDATE:"))
        sayExtra("[" + T.Suite() + "/" + T.Name() + "] " + L);
    return Done;
  }
};

/// Finds every test under @p Dir, sorted by path.
std::vector<TestFile> Discover(const fs::path &Dir, const std::string &DirSpelling,
                               const std::vector<std::string> &Excludes) {
  std::vector<TestFile> Found;
  std::error_code EC;
  for (fs::recursive_directory_iterator I(Dir, EC), E; !EC && I != E; I.increment(EC)) {
    const fs::path &P = I->path();
    const std::string Name = P.filename().string();
    // A hidden entry is a tool's scratch (editor swap files, the old runner's
    // `.testrun_*` binaries), never a test.
    if (!Name.empty() && Name[0] == '.') {
      if (I->is_directory(EC))
        I.disable_recursion_pending();
      continue;
    }
    if (!I->is_regular_file(EC) || !EndsWith(Name, ".tr") || EndsWith(Name, "_module.tr"))
      continue;
    std::string Rel = fs::relative(P, Dir, EC).generic_string();
    bool Skipped = false;
    for (const auto &X : Excludes)
      if (Rel == X || StartsWith(Rel, X + "/"))
        Skipped = true;
    if (Skipped)
      continue;
    Found.push_back({Rel, DirSpelling + "/" + Rel, P});
  }
  std::sort(Found.begin(), Found.end(), [](const TestFile &A, const TestFile &B) { return A.Rel < B.Rel; });
  return Found;
}

} // namespace

int RunTests(const TestRunOptions &O) {
  std::error_code EC;
  fs::path Dir = fs::absolute(O.Dir, EC);
  if (!fs::exists(Dir, EC)) {
    std::cerr << "Error: no tests directory: '" << O.Dir << "' does not exist.\n"
              << "  hint: create it and put test files (`.tr`) in it.\n";
    return 1;
  }
  if (!fs::is_directory(Dir, EC)) {
    std::cerr << "Error: '" << O.Dir << "' is not a directory.\n";
    return 1;
  }
  Dir = Dir.lexically_normal();
  // `tests/` and `tests` are the same directory; the register spells a path
  // without the trailing slash.
  std::string Spelling = O.Dir;
  while (Spelling.size() > 1 && (Spelling.back() == '/' || Spelling.back() == '\\'))
    Spelling.pop_back();

  std::vector<TestFile> Tests = Discover(Dir, Spelling, O.Excludes);
  if (Tests.empty()) {
    std::cerr << "Error: no tests found in '" << O.Dir << "'.\n"
              << "  hint: a test is a `.tr` file (not one ending in `_module.tr`, which is a companion "
                 "to import).\n";
    return 1;
  }

  TestExpect::Register Reg;
  if (!O.DiagRegister.empty()) {
    std::string Err;
    if (!Reg.load(O.DiagRegister, Err)) {
      std::cerr << "Error: " << Err << ".\n";
      return 1;
    }
  }

  int Jobs = O.Jobs;
  if (Jobs <= 0)
    if (const char *E = std::getenv("TURF_TEST_JOBS"))
      Jobs = std::atoi(E);
  if (Jobs <= 0) {
    Jobs = static_cast<int>(std::thread::hardware_concurrency());
#ifdef __APPLE__
    // Gatekeeper's first-run check of each fresh executable serialises across
    // processes (measured ~0.3-0.5s each), so full parallelism queues up
    // enough of them to push tests past the run timeout.
    Jobs /= 2;
#endif
    if (Jobs <= 0)
      Jobs = 4;
  }
  Jobs = std::min<int>(Jobs, static_cast<int>(Tests.size()));
  int Timeout = O.TimeoutSec;
  if (Timeout <= 0)
    if (const char *E = std::getenv("TURF_TEST_TIMEOUT_SEC"))
      Timeout = std::atoi(E);
  if (Timeout <= 0)
    Timeout = DefaultTimeoutSec;

  fs::path Temp = fs::temp_directory_path(EC) / "turf-test-XXXXXX";
  std::string TempTemplate = Temp.string();
#ifdef _WIN32
  // No mkdtemp: a name made of the process id and the clock, created exclusively
  // (create_directory fails if it exists, so two runs never share one).
  {
    bool Made = false;
    for (int Try = 0; Try < 100 && !Made; ++Try) {
      const auto Tick = std::chrono::steady_clock::now().time_since_epoch().count();
      TempTemplate = (Temp.parent_path() / ("turf-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                                            std::to_string(Tick + Try)))
                         .string();
      Made = fs::create_directory(TempTemplate, EC) && !EC;
    }
    if (!Made) {
      std::cerr << "Error: cannot create a temporary directory under " << Temp.parent_path().string() << ".\n";
      return 1;
    }
  }
  SetConsoleCtrlHandler(onConsoleControl, TRUE);
#else
  if (!mkdtemp(TempTemplate.data())) {
    std::cerr << "Error: cannot create a temporary directory under " << Temp.parent_path().string() << ".\n";
    return 1;
  }

  struct sigaction SA {};
  SA.sa_handler = onInterrupt;
  sigemptyset(&SA.sa_mask);
  sigaction(SIGINT, &SA, nullptr);
  sigaction(SIGTERM, &SA, nullptr);
#endif

  Palette Pal;
#ifdef _WIN32
  Pal.On = _isatty(_fileno(stdout)) && !std::getenv("NO_COLOR");
  if (Pal.On) {
    // A console does not interpret the colour escapes until asked to.
    HANDLE Out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD Mode = 0;
    if (Out != INVALID_HANDLE_VALUE && GetConsoleMode(Out, &Mode))
      SetConsoleMode(Out, Mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
  }
#else
  Pal.On = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
#endif

  std::string OptNote;
  for (const auto &F : O.CompileFlags)
    if (F.size() >= 2 && F[0] == '-' && F[1] == 'O')
      OptNote = " (" + F + ")";
  std::cout << Pal.bold("Running Turf Test Suites..." + OptNote) << "\n" << std::endl;

  Runner R(O, std::move(Tests), TempTemplate, Jobs, Timeout, O.DiagRegister.empty() ? nullptr : &Reg, Pal);
  const auto Tests_ = R.runAll();
  fs::remove_all(TempTemplate, EC);

  if (g_Interrupted) {
    std::cout << "\nInterrupted.\n";
    return 130;
  }

  size_t Passed = 0;
  std::vector<std::string> Failures;
  for (const auto &Res : Tests_) {
    if (Res.Pass)
      ++Passed;
    else
      Failures.push_back(Res.Detail);
  }
  std::cout << "\n" << Pal.bold("=== Test Summary ===") << "\n"
            << "Total Tests: " << Tests_.size() << "\n"
            << "Passed:      " << Pal.green(std::to_string(Passed)) << "\n"
            << "Failed:      " << Pal.red(std::to_string(Failures.size())) << "\n";
  // The register is owed work, never "expected": say how much, every run, so
  // the number is visible and can only be seen to go down.
  if (!O.DiagRegister.empty())
    std::cout << "Diagnostic register (" << O.DiagRegister << "): " << Reg.count("UNPINNED") << " unpinned, "
              << Reg.count("NO-CODE") << " no-code, " << Reg.count("WRONG-REASON") << " wrong-reason, "
              << Reg.count("GENERIC-CODE") << " generic-code, " << Reg.count("CASCADE") << " cascade\n";

  if (!Failures.empty()) {
    std::cout << "\n" << Pal.bold(Pal.red("Failed Tests Details:")) << "\n";
    for (const auto &F : Failures)
      std::cout << "\n" << Pal.yellow("--- " + F + " ---") << "\n";
    return 1;
  }
  std::cout << "\n" << Pal.green(Pal.bold("All tests passed successfully!")) << "\n";
  return 0;
}
