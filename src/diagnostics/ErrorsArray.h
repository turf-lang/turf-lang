#ifndef ERRORS_ARRAY_H
#define ERRORS_ARRAY_H

#include "diagnostics/Errors.h"

// Array index out of bounds (compile-time constant check)
class ArrayBoundsError : public TurfError {
public:
  ArrayBoundsError(SourceLocation Loc, const std::string &ArrayName,
                   long long Index, int ArraySize)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "I cannot use `" + Colors::CYAN +
                      ArrayName + "[" + std::to_string(Index) + "]" +
                      Colors::BRIGHT_RED + "`, because `" + Colors::CYAN +
                      ArrayName + Colors::BRIGHT_RED + "` has only " +
                      std::to_string(ArraySize) + " element" +
                      (ArraySize == 1 ? "" : "s") + "." + Colors::RESET,
                  "Use an index from 0 to " + std::to_string(ArraySize - 1) +
                      ", or `-1` to `-" + std::to_string(ArraySize) +
                      "` to count from the end.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};
// (ArraySizeMismatchError E003, ArrayNonIntegerIndexError E002 and
// ArrayTypeMismatchError E004 were deleted in issue #263: documented and
// registered, never constructed. The TypeChecker reports all three through
// reportError() with those codes -- see ErrorCodes.h's ErrCode:: block. E002's
// check did not exist at all until then: `arr["one"]` compiled silently.)
// for-each target is neither an array nor a string (scalar, bare struct, List<T>/Map<K,V>, etc.)
class ForEachNonArrayError : public TurfError {
public:
  ForEachNonArrayError(SourceLocation Loc, const std::string &GotType)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "I cannot use for-each on the type `" +
                      Colors::CYAN + GotType + Colors::BRIGHT_RED +
                      "`, which is not an array or a string." +
                      Colors::RESET,
                  "Loop over an array (`T[N]` or a `T[]` parameter) or a "
                  "string, walk a `List` by index (`for i in 0..l.length()`), "
                  "or loop over a `Map` through `keys()`.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};
// (ForEachUnsizedArrayError, E006, is gone: a `T[]` parameter carries its
// length since issue #260, so for-each over one is legal.)

#endif
