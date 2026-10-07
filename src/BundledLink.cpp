// BundledLink.cpp -- see include/BundledLink.h (REL-008 stage 3).
//
// Built two ways, like SLMEngine.cpp / SLMEngineDisabled.cpp but in one file:
// with TURF_EMBED_LLD (CMake, release builds) the Linux x86-64 driver below
// calls lld in-process; without it every entry point reports "not available"
// and linkObject() uses the system toolchain.
#include "BundledLink.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

#if defined(TURF_EMBED_LLD) && defined(__linux__) && defined(__x86_64__)
#define TURF_BUNDLED_LINK_ELF 1
#include "lld/Common/Driver.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
LLD_HAS_DRIVER(elf)

// LLVM's LTO (which lld's ELF driver links, to be able to read bitcode objects)
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

bool BundledRuntimeProvides(const std::string &Lib) {
  for (const char *Own : {"c", "m", "pthread", "dl", "rt", "util"})
    if (Lib == Own)
      return true;
  return false;
}

#ifdef TURF_BUNDLED_LINK_ELF

std::string BundledRuntimePlatform() { return "linux-x86_64"; }

static bool Has(const std::string &Dir, const char *Name) {
  return llvm::sys::fs::exists(Dir + "/" + Name);
}

bool BundledLinkAvailable(const std::string &RuntimeDir, std::string *Why) {
  for (const char *Required : {"crt1.o", "crti.o", "crtn.o", "libc.a"}) {
    if (!Has(RuntimeDir, Required)) {
      if (Why)
        *Why = "'" + RuntimeDir + "/" + Required + "' does not exist";
      return false;
    }
  }
  return true;
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
    if (Has(Rt, (std::string("lib") + Lib + ".a").c_str()))
      Args.push_back(std::string("-l") + Lib);
  Args.push_back("--end-group");

  if (Has(Rt, "crtend.o"))
    Args.push_back(Rt + "/crtend.o");
  Args.push_back(Rt + "/crtn.o");

  std::vector<const char *> Argv;
  for (const auto &A : Args)
    Argv.push_back(A.c_str());

  // lldMain wants the streams, never the process's own, so a failed link
  // reports through turfc's diagnostics instead of writing over them. With
  // exit-early off (lldMain's way) a failure returns, and the process goes on.
  std::string OutText, ErrText;
  llvm::raw_string_ostream Out(OutText);
  llvm::raw_string_ostream Err(ErrText);
  lld::Result R = lld::lldMain(Argv, Out, Err, {{lld::Gnu, &lld::elf::link}});
  Out.flush();
  Err.flush();
  Diagnostics = OutText + ErrText;
  return R.retCode;
}

#else // no embedded linker for this build/host

std::string BundledRuntimePlatform() { return ""; }

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
