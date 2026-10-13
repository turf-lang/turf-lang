// BundledLink.h -- REL-008 stage 3: link a compiled program with the linker
// built into turfc (lld, in-process) against the C runtime shipped in the
// release archive, instead of running the machine's own `cc`.
//
// Why: `turf run` has to work where there is no C compiler, no libc development
// files and no `ld`. That needs two things, which are two stages of REL-008:
//   * stage 3 (this): a linker inside turfc;
//   * stage 4: the C runtime it links against, under <root>/lib/rt/<platform>/.
// With no runtime directory (every checkout and dev build, and any archive
// before stage 4), nothing here is available and linkObject() runs `cc` exactly
// as it always did.
//
// The hybrid rule (DEC-14, decided by turfc per program): a program that names
// no system C library (no `extern from "lib"`, no `--link`) is linked here,
// statically, against the bundled runtime. A program that does name one needs
// that library's own development files, so it goes to the system toolchain.
//
// The runtime directory contract (Linux; <root>/lib/rt/linux-x86_64/ or linux-arm64/):
//   required   crt1.o  crti.o  crtn.o  libc.a
//   optional   crtbeginT.o crtend.o   (a glibc-style start-up pair)
//              libm.a libgcc.a libgcc_eh.a
// Every optional file that exists is used; the link is always static.
#pragma once

#include <string>

namespace turf {

// The directory name under <root>/lib/rt for the machine turfc runs on
// ("linux-x86_64", "linux-arm64"), or "" when this host has no bundled link (a platform whose
// runtime and linker driver have not landed yet).
std::string BundledRuntimePlatform();

// True when `Lib` (a name from `extern from "Lib"` or --link) is part of the
// bundled C runtime itself, so naming it does not make a program need the
// system's development files: libc and the pieces musl and glibc 2.34+ fold
// into it (m, pthread, dl, rt, util). The standard library's own `math` module
// names "m"; anything else is the user's own C library.
bool BundledRuntimeProvides(const std::string &Lib);

// True when this turfc embeds a linker for the host and `RuntimeDir` holds the
// required files. `Why`, when given, says what is missing (for a diagnostic
// that explains why the system toolchain was used instead).
bool BundledLinkAvailable(const std::string &RuntimeDir,
                          std::string *Why = nullptr);

struct BundledLinkRequest {
  std::string ObjectFile;  // the program's object file
  std::string Output;      // the executable to write
  std::string RuntimeDir;  // <root>/lib/rt/<platform>
  std::string TurfLibDir;  // <root>/lib, where libturfrt.a is
  std::string TurfRtName;  // "turfrt" (or "turfrt_asan")
};

// Link in-process. Returns 0 on success; otherwise the linker's own messages
// are in `Diagnostics` (never printed here) and the result is non-zero.
int BundledLink(const BundledLinkRequest &Req, std::string &Diagnostics);

} // namespace turf
