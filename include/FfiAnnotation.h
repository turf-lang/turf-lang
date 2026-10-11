#pragma once

// FfiAnnotation.h -- the meaning of `@ffi(...)`, owned by the code that reads
// it (extern-function codegen, CodegenFunc.cpp). See include/Annotation.h.
//
//   @ffi(macos: malloc_size)
//   pub extern fn malloc_usable_size(p: void*): int;
//
// says: on macOS, the symbol this declaration binds to is spelled
// `malloc_size` -- a same-signature equivalent -- so the compiler emits a
// one-line wrapper under the Turf-visible name that forwards to it. The fact
// lives on the declaration, not in a compiler table keyed on the libc name.
// The platform keys are `linux`, `macos` and `windows`; the platform is the
// one the program is compiled for (`--target`, else the host).

#include "Annotation.h"
#include "SuggestionIndex.h"
#include "Types.h"

#include <string>
#include <utility>
#include <vector>

namespace FfiAnnotation {

inline constexpr const char *Name = "ffi";

/// The platform key of the host this compiler was built for.
inline const char *hostPlatformKey() {
#if defined(__APPLE__)
  return "macos";
#elif defined(_WIN32)
  return "windows";
#else
  return "linux";
#endif
}

/// The symbol `Decl`'s `@ffi` annotations say to forward to on `Platform`
/// (a platform key), or "" if none names that platform.
inline std::string forwardTarget(const AnnotatedDecl &Decl,
                                 const std::string &Platform) {
  for (const Annotation *A : Decl.getAnnotations(Name))
    if (const AnnotationArg *K = A->findKeyed(Platform))
      return K->Value;
  return "";
}

inline std::string validate(const Annotation &Ann,
                            const std::vector<std::pair<std::string, TurfTypePtr>> &,
                            const TurfTypePtr &, bool IsExtern,
                            const std::vector<Annotation> &All) {
  if (!IsExtern)
    return "'@ffi' describes how an 'extern fn' binds to its C symbol, so it "
           "can only sit on an 'extern fn'.";
  if (Ann.Args.empty())
    return "'@ffi' needs an argument: a platform and the symbol to forward to, "
           "e.g. '@ffi(macos: malloc_size)'.";
  const std::vector<std::string> Platforms = {"linux", "macos", "windows"};
  for (const AnnotationArg &Arg : Ann.Args) {
    if (Arg.Key.empty())
      return "'@ffi' arguments are written 'platform: symbol', e.g. "
             "'@ffi(macos: malloc_size)'.";
    bool Known = false;
    for (const auto &P : Platforms)
      Known = Known || P == Arg.Key;
    if (!Known) {
      std::string Msg = "Unknown '@ffi' platform '" + Arg.Key + "'.";
      auto Near = SuggestionIndex::suggestVisible(Arg.Key, Platforms);
      if (!Near.empty())
        Msg += " Did you mean '" + Near.front() + "'?";
      return Msg + " The platforms are 'linux', 'macos' and 'windows'.";
    }
    if (Arg.IsString || Arg.Value.empty())
      return "'@ffi(" + Arg.Key + ": ...)' names the symbol as a plain word, "
             "not a string.";
    // forwardTarget() takes the first match, so a second entry for the same
    // platform -- in this annotation or an earlier `@ffi` on the same
    // declaration -- would be silently ignored.
    bool Twice = Ann.findKeyed(Arg.Key) != &Arg;
    for (const Annotation &Earlier : All) {
      if (&Earlier == &Ann)
        break;
      Twice = Twice || (Earlier.Name == Name && Earlier.findKeyed(Arg.Key));
    }
    if (Twice)
      return "'@ffi' names the platform '" + Arg.Key + "' twice; give each "
             "platform one symbol.";
  }
  return "";
}

} // namespace FfiAnnotation
