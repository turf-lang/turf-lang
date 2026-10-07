// BundledLink.cpp -- see include/BundledLink.h (REL-008 stages 3 and 4).
//
// Built two ways, like SLMEngine.cpp / SLMEngineDisabled.cpp but in one file:
// with TURF_EMBED_LLD (CMake, release builds) the driver for the host's object
// format below calls lld in-process; without it every entry point reports "not
// available" and linkObject() uses the system toolchain. One driver per host:
//
//   Linux x86-64   lld's ELF driver, static, against musl            (lib/rt/linux-x86_64)
//   Windows x86-64 lld's MinGW (COFF) driver, against MinGW-w64's UCRT CRT (lib/rt/windows-x86_64)
//   macOS arm64    lld's Mach-O driver, against a stub libSystem     (lib/rt/macos-arm64)
//
// The directory contracts are in the header; scripts/build_bundled_runtime.sh
// produces each directory.
#include "BundledLink.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

#if defined(TURF_EMBED_LLD)
#include "lld/Common/Driver.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Plugins/PassPlugin.h"
#if defined(__linux__) && defined(__x86_64__)
#define TURF_BUNDLED_LINK_ELF 1
LLD_HAS_DRIVER(elf)
#elif defined(_WIN32) && defined(__x86_64__)
#define TURF_BUNDLED_LINK_MINGW 1
LLD_HAS_DRIVER(mingw)
#elif defined(__APPLE__) && defined(__aarch64__)
#define TURF_BUNDLED_LINK_MACHO 1
LLD_HAS_DRIVER(macho)
#endif

// LLVM's LTO (which every lld driver links, to be able to read bitcode objects)
// calls the pass-plugin entry point of every extension LLVM was built with
// (llvm/Support/Extension.def). Debian/Ubuntu's LLVM is built with Polly, so
// libLLVMLTO.a asks for getPollyPluginInfo(), which lives in Polly's own
// archive. turfc does not link Polly (and never links bitcode), so provide the
// entry point as a weak no-op: if a real Polly is ever linked in, its strong
// definition wins. Without this the link fails with "undefined reference to
// getPollyPluginInfo()".
__attribute__((weak)) llvm::PassPluginLibraryInfo getPollyPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "Polly (not linked into turfc)",
          LLVM_VERSION_STRING, [](llvm::PassBuilder &) {}};
}
#endif

namespace turf {

#if defined(TURF_BUNDLED_LINK_ELF) || defined(TURF_BUNDLED_LINK_MINGW) ||     \
    defined(TURF_BUNDLED_LINK_MACHO)

static bool Has(const std::string &Dir, const std::string &Name) {
  return llvm::sys::fs::exists(Dir + "/" + Name);
}

static bool HasAll(const std::string &Dir,
                   std::initializer_list<const char *> Names,
                   std::string *Why) {
  for (const char *N : Names) {
    if (!Has(Dir, N)) {
      if (Why)
        *Why = "'" + Dir + "/" + N + "' does not exist";
      return false;
    }
  }
  return true;
}

// Run lld on `Args` (Args[0] is the program name lld uses to pick a flavour)
// and collect everything it said. lldMain wants streams that are not the
// process's own, so a failed link reports through turfc's diagnostics instead
// of writing over them; with exit-early off a failure returns and the process
// goes on.
static int RunLld(const std::vector<std::string> &Args,
                  llvm::ArrayRef<lld::DriverDef> Drv,
                  std::string &Diagnostics) {
  std::vector<const char *> Argv;
  for (const auto &A : Args)
    Argv.push_back(A.c_str());
  std::string OutText, ErrText;
  llvm::raw_string_ostream Out(OutText);
  llvm::raw_string_ostream Err(ErrText);
  lld::Result R = lld::lldMain(Argv, Out, Err, Drv);
  Out.flush();
  Err.flush();
  Diagnostics = OutText + ErrText;
  return R.retCode;
}

#endif

// ═════ Linux x86-64: ELF ════════════════════════════════════════════════════
#ifdef TURF_BUNDLED_LINK_ELF

std::string BundledRuntimePlatform() { return "linux-x86_64"; }

bool BundledRuntimeProvides(const std::string &Lib) {
  // Folded into libc by musl and by glibc 2.34+.
  for (const char *Own : {"c", "m", "pthread", "dl", "rt", "util"})
    if (Lib == Own)
      return true;
  return false;
}

bool BundledLinkAvailable(const std::string &RuntimeDir, std::string *Why) {
  return HasAll(RuntimeDir, {"crt1.o", "crti.o", "crtn.o", "libc.a"}, Why);
}

int BundledLink(const BundledLinkRequest &Req, std::string &Diagnostics) {
  const std::string &Rt = Req.RuntimeDir;
  std::vector<std::string> Args = {"ld.lld", "-static", "-o", Req.Output,
                                   "-z", "noexecstack", "--eh-frame-hdr"};

  // Start-up objects first, the program, the end-up objects last: the order a
  // C compiler driver uses, and the one crti.o/crtn.o's prologue/epilogue
  // sections need.
  Args.push_back(Rt + "/crt1.o");
  Args.push_back(Rt + "/crti.o");
  if (Has(Rt, "crtbeginT.o"))
    Args.push_back(Rt + "/crtbeginT.o");
  Args.push_back(Req.ObjectFile);

  // The runtime, then the C library it needs. A group, because libturfrt needs
  // libc, libc (static glibc) needs libgcc, and libgcc_eh needs libc back.
  // The runtime directory is searched FIRST: it holds its own libturfrt.a,
  // built against the bundled C library (musl on Linux). The one in lib/ is
  // built against the system's glibc and cannot be linked into a program that
  // carries musl; it is only the fallback for a runtime directory that has
  // none (the glibc stand-in of scripts/bundled_link_smoke.sh).
  Args.push_back("-L" + Rt);
  Args.push_back("-L" + Req.TurfLibDir);
  Args.push_back("--start-group");
  Args.push_back("-l" + Req.TurfRtName);
  for (const char *Lib : {"m", "c", "gcc", "gcc_eh"})
    if (Has(Rt, std::string("lib") + Lib + ".a"))
      Args.push_back(std::string("-l") + Lib);
  Args.push_back("--end-group");

  if (Has(Rt, "crtend.o"))
    Args.push_back(Rt + "/crtend.o");
  Args.push_back(Rt + "/crtn.o");

  return RunLld(Args, {{lld::Gnu, &lld::elf::link}}, Diagnostics);
}

// ═════ Windows x86-64: MinGW-w64 on UCRT ════════════════════════════════════
#elif defined(TURF_BUNDLED_LINK_MINGW)

std::string BundledRuntimePlatform() { return "windows-x86_64"; }

bool BundledRuntimeProvides(const std::string &Lib) {
  // The C runtime (UCRT, through MinGW's import library) and the Win32 DLLs
  // every Windows has; their import libraries are in the runtime directory.
  for (const char *Own : {"c", "m", "ucrt", "kernel32", "user32", "advapi32",
                          "shell32", "ws2_32"})
    if (Lib == Own)
      return true;
  return false;
}

bool BundledLinkAvailable(const std::string &RuntimeDir, std::string *Why) {
  return HasAll(RuntimeDir,
                {"crt2.o", "libmingw32.a", "libmingwex.a", "libucrt.a",
                 "libkernel32.a", "libgcc.a"},
                Why);
}

int BundledLink(const BundledLinkRequest &Req, std::string &Diagnostics) {
  const std::string &Rt = Req.RuntimeDir;
  // `-m i386pep` is what makes lld's GNU-flavoured front end hand over to its
  // MinGW (COFF) driver. The library order is the one gcc passes to ld on
  // MinGW (crt2.o and crtbegin.o first, crtend.o last; libmingw32, libgcc,
  // libgcc_eh, libmoldname, libmingwex, the UCRT import library, then the
  // Win32 import libraries); COFF linking does not need the repeats gcc adds
  // for GNU ld's one-pass archive scan.
  std::vector<std::string> Args = {"ld.lld", "-m", "i386pep", "-o", Req.Output};
  Args.push_back(Rt + "/crt2.o");
  if (Has(Rt, "crtbegin.o"))
    Args.push_back(Rt + "/crtbegin.o");
  Args.push_back(Req.ObjectFile);
  Args.push_back("-L" + Rt);
  Args.push_back("-L" + Req.TurfLibDir);
  Args.push_back("-l" + Req.TurfRtName);
  for (const char *Lib :
       {"mingw32", "gcc", "gcc_eh", "moldname", "mingwex", "ucrt", "kernel32",
        "advapi32", "shell32", "user32", "ws2_32"})
    if (Has(Rt, std::string("lib") + Lib + ".a"))
      Args.push_back(std::string("-l") + Lib);
  if (Has(Rt, "crtend.o"))
    Args.push_back(Rt + "/crtend.o");

  return RunLld(Args, {{lld::MinGW, &lld::mingw::link}}, Diagnostics);
}

// ═════ macOS arm64: Mach-O, against a stub libSystem ════════════════════════
#elif defined(TURF_BUNDLED_LINK_MACHO)

std::string BundledRuntimePlatform() { return "macos-arm64"; }

bool BundledRuntimeProvides(const std::string &Lib) {
  // Everything libSystem re-exports: the C library, libm, pthreads, dl.
  for (const char *Own : {"c", "m", "System", "pthread", "dl"})
    if (Lib == Own)
      return true;
  return false;
}

bool BundledLinkAvailable(const std::string &RuntimeDir, std::string *Why) {
  return HasAll(RuntimeDir, {"libSystem.tbd", "libturfrt.a"}, Why);
}

int BundledLink(const BundledLinkRequest &Req, std::string &Diagnostics) {
  const std::string &Rt = Req.RuntimeDir;
  // libSystem.tbd in the runtime directory is our own stub (an install name and
  // one symbol, no Apple file): it makes the executable load
  // /usr/lib/libSystem.B.dylib. The libSystem symbols the program and
  // libturfrt use are bound by name when the program starts
  // (-undefined dynamic_lookup), so no symbol list has to be shipped.
  // macOS 11 is the first arm64 macOS. Ad-hoc code signing, which an arm64
  // executable must have to run, is lld's default for this target.
  std::vector<std::string> Args = {
      "ld64.lld", "-arch", "arm64", "-platform_version", "macos", "11.0.0",
      "11.0.0", "-o", Req.Output, "-L" + Rt, "-L" + Req.TurfLibDir,
      "-undefined", "dynamic_lookup", Req.ObjectFile,
      // libturfrt built for this machine, from the runtime directory first.
      (Has(Rt, "lib" + Req.TurfRtName + ".a")
           ? Rt + "/lib" + Req.TurfRtName + ".a"
           : Req.TurfLibDir + "/lib" + Req.TurfRtName + ".a"),
      "-lSystem"};
  return RunLld(Args, {{lld::Darwin, &lld::macho::link}}, Diagnostics);
}

#else // no embedded linker for this build/host

std::string BundledRuntimePlatform() { return ""; }

bool BundledRuntimeProvides(const std::string &) { return false; }

bool BundledLinkAvailable(const std::string &, std::string *Why) {
  if (Why)
    *Why = "this turfc has no embedded linker";
  return false;
}

int BundledLink(const BundledLinkRequest &, std::string &Diagnostics) {
  Diagnostics = "this turfc has no embedded linker";
  return 1;
}

#endif

} // namespace turf
