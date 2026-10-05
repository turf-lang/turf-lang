#ifndef ERRORS_ARRAY_H
#define ERRORS_ARRAY_H

#include "Errors.h"

// Array index out of bounds (compile-time constant check)
class ArrayBoundsError : public TurfError {
public:
  ArrayBoundsError(SourceLocation Loc, const std::string &ArrayName,
                   long long Index, int ArraySize)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "Array index out of bounds!\n" +
                      Colors::RESET + "  You tried to access '" + Colors::CYAN +
                      ArrayName + "[" + std::to_string(Index) + "]" +
                      Colors::RESET + "', but '" + Colors::CYAN + ArrayName +
                      Colors::RESET + "' only has " +
                      Colors::CYAN + std::to_string(ArraySize) + Colors::RESET +
                      " elements (indices 0 to " +
                      std::to_string(ArraySize - 1) + ", or -1 to -" +
                      std::to_string(ArraySize) + " counting from the end).",
                  "Array indices start at 0 and go up to size - 1; a "
                  "negative index counts back from the end (-1 is the last "
                  "element).") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};
// (ArraySizeMismatchError E003, ArrayNonIntegerIndexError E002 and
// ArrayTypeMismatchError E004 were deleted in issue #263: documented and
// registered, never constructed. The TypeChecker reports all three through
// reportError() with those codes -- see ErrorCodes.h's ErrCode:: block. E002's
// check did not exist at all until then: `arr["one"]` compiled silently.)
// for-each target isn't an array at all (scalar, bare struct, List<T>/Map<K,V>, etc.)
class ForEachNonArrayError : public TurfError {
public:
  ForEachNonArrayError(SourceLocation Loc, const std::string &GotType)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "for-each requires an array!\n" +
                      Colors::RESET + "  You tried to iterate over a value of type '" +
                      Colors::CYAN + GotType + Colors::RESET + "'.",
                  "for-each only works on Turf's built-in fixed-size array "
                  "type (T[N]). Lists, maps, structs, and scalars aren't "
                  "supported yet.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};
// (ForEachUnsizedArrayError, E006, is gone: a `T[]` parameter carries its
// length since issue #260, so for-each over one is legal.)

#endif
