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
                      Colors::RESET + "', but the array only has " +
                      Colors::CYAN + std::to_string(ArraySize) + Colors::RESET +
                      " elements (indices 0 to " +
                      std::to_string(ArraySize - 1) + ").",
                  "Array indices start at 0 and go up to size - 1.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};
// Array initializer size mismatch
class ArraySizeMismatchError : public TurfError {
public:
  ArraySizeMismatchError(SourceLocation Loc, const std::string &ArrayName,
                         int DeclaredSize, int InitSize)
      : TurfError(Loc, Colors::BRIGHT_RED + "Array size mismatch!\n" +
                           Colors::RESET + "  '" + Colors::CYAN + ArrayName +
                           Colors::RESET + "' was declared with size " +
                           Colors::CYAN + std::to_string(DeclaredSize) +
                           Colors::RESET + ", but you gave " + Colors::CYAN +
                           std::to_string(InitSize) + Colors::RESET +
                           " initial values.",
                  "The number of values in [...] must match the "
                  "declared array size.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};
// Non-integer array index
class ArrayNonIntegerIndexError : public TurfError {
public:
  ArrayNonIntegerIndexError(SourceLocation Loc, const std::string &GotType)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Array index must be an integer!\n" + Colors::RESET +
                           "  You used a value of type '" + Colors::CYAN +
                           GotType + Colors::RESET +
                           "' as an array index.",
                  "Use an 'int' value to index into an array, "
                  "like arr[0] or arr[i] where i is an int.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};
// Array element type mismatch
class ArrayTypeMismatchError : public TurfError {
public:
  ArrayTypeMismatchError(SourceLocation Loc, const std::string &ArrayName,
                         const std::string &ExpectedType,
                         const std::string &GotType)
      : TurfError(
            Loc, Colors::BRIGHT_RED + "Array element type mismatch!\n" +
                     Colors::RESET + "  Array '" + Colors::CYAN + ArrayName +
                     Colors::RESET + "' holds '" + Colors::CYAN + ExpectedType +
                     Colors::RESET + "' values, but you tried to store a '" +
                     Colors::CYAN + GotType + Colors::RESET + "'.",
            "All elements in the array must match the declared "
            "element type.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};
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
// for-each target is an unsized array parameter (`T[]`), which carries no
// length at runtime -- there's no safe way to bound the loop.
class ForEachUnsizedArrayError : public TurfError {
public:
  ForEachUnsizedArrayError(SourceLocation Loc, const std::string &GotType)
      : TurfError(Loc,
                  Colors::BRIGHT_RED + "for-each requires a known array size!\n" +
                      Colors::RESET + "  '" + Colors::CYAN + GotType + Colors::RESET +
                      "' is an unsized array parameter, which carries no "
                      "length at runtime.",
                  "Pass a concrete-size array, or add a separate 'len: int' "
                  "parameter and use an ordinary for-loop instead.") {
    Category_ = SLM::ErrorCategory::ARRAY_ERROR;
  }
};

#endif
