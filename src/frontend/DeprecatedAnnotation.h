#pragma once

// DeprecatedAnnotation.h -- the meaning of `@deprecated("...")`, owned by the
// pass that reads it to warn at a use of the item it sits on (the Lint pass,
// src/analysis/Lint.cpp). See src/frontend/Annotation.h.
//
// TDP-2-003 V-2 (issue #286, docs/planning/ROADMAP_v3.0.0.md 6.1): marks a
// function or `extern fn` obsolete, with a message naming what to use
// instead. Every call to it then warns (E172, DeprecatedUseWarning) with
// that exact message -- the compilation still succeeds; removing a
// deprecated item is a 3.0 job (B-3), not this annotation's concern.
//
//   @deprecated("use List::sorted() instead")
//   fn old_sort(l: List<int>): List<int> { ... }

#include "frontend/Annotation.h"
#include "sema/Types.h"

#include <string>
#include <utility>
#include <vector>

namespace DeprecatedAnnotation {

inline constexpr const char *Name = "deprecated";

inline std::string validate(const Annotation &Ann,
                            const std::vector<std::pair<std::string, TurfTypePtr>> &,
                            const TurfTypePtr &, bool /*IsExtern*/,
                            const std::vector<Annotation> &/*All*/) {
  // Exactly one positional argument: the message shown at every use. Unlike
  // `@ffi`'s `platform: symbol` pairs, there is nothing to key this on --
  // one declaration deprecates in exactly one way.
  if (Ann.Args.size() != 1)
    return "'@deprecated' takes exactly one argument: the message to show at "
           "each use, e.g. '@deprecated(\"use new_fn() instead\")'.";
  if (!Ann.Args[0].Key.empty())
    return "'@deprecated's argument is positional, not 'key: value' -- write "
           "'@deprecated(\"use new_fn() instead\")'.";
  if (!Ann.Args[0].IsString)
    return "'@deprecated's argument must be a string literal, e.g. "
           "'@deprecated(\"use new_fn() instead\")'.";
  if (Ann.Args[0].Value.empty())
    return "'@deprecated(\"\")' needs a non-empty message saying what to use "
           "instead.";
  return "";
}

} // namespace DeprecatedAnnotation
