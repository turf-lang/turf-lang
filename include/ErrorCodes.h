#ifndef ERROR_CODES_H
#define ERROR_CODES_H

// Auto-generated registry mapping each TurfError subclass to a stable
// `E0NN` diagnostic code, consumed by `turf --explain E0NN` (main.cpp) and
// by DiagnosticEngine's header line (Lexer.cpp's printDiag()). Codes are
// looked up at raise()-time via typeid(*this) (Errors.h's
// TurfError::getCode()) rather than a per-constructor `Code_ = "E0NN";`
// line -- this means adding a brand-new TurfError subclass elsewhere in the
// compiler costs exactly one line here, not a constructor-body edit at each
// of the (currently 67) call sites. Assignment order is alphabetical by
// (header file, class name) -- stable and reproducible, but NOT meaningful
// (an E0NN code is an opaque identifier, not a severity/category ranking).
// A code, once shipped, should never be reassigned to a different class --
// append new entries at the end instead of resorting this table.
//
// Category comes from the same SLM::ErrorCategory each class already sets
// on itself (Errors.h's Category_ field) -- duplicated here as a plain enum
// value (not re-derived from the class at static-init time) so this table
// can be a simple constexpr-friendly array with no include-order dependency
// on the class definitions themselves.

#include "ErrorCategory.h"

#include <cstring>
#include <string>

struct ErrorCodeEntry {
  const char *Code;
  const char *ClassName;
  const char *Description;
  SLM::ErrorCategory Category;
};

inline const ErrorCodeEntry AllErrorCodes[] = {
    {"E001", "ArrayBoundsError", "Array bounds", SLM::ErrorCategory::ARRAY_ERROR},
    {"E002", "ArrayNonIntegerIndexError", "Array non integer index", SLM::ErrorCategory::ARRAY_ERROR},
    {"E003", "ArraySizeMismatchError", "Array size mismatch", SLM::ErrorCategory::ARRAY_ERROR},
    {"E004", "ArrayTypeMismatchError", "Array type mismatch", SLM::ErrorCategory::ARRAY_ERROR},
    {"E005", "ForEachNonArrayError", "For each non array", SLM::ErrorCategory::ARRAY_ERROR},
    {"E006", "ForEachUnsizedArrayError", "For each unsized array", SLM::ErrorCategory::ARRAY_ERROR},
    {"E007", "ControlFlowError", "Control flow", SLM::ErrorCategory::CONTROL_FLOW},
    {"E008", "DefiniteAssignmentError", "Definite assignment", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E009", "MissingReturnError", "Missing return", SLM::ErrorCategory::CONTROL_FLOW},
    {"E010", "RecursionWithoutBaseError", "Recursion without base", SLM::ErrorCategory::CONTROL_FLOW},
    {"E011", "SemanticError", "Semantic", SLM::ErrorCategory::GENERAL},
    {"E012", "StatementAfterTerminatorError", "Statement after terminator", SLM::ErrorCategory::CONTROL_FLOW},
    {"E013", "UnreachableCodeError", "Unreachable code", SLM::ErrorCategory::CONTROL_FLOW},
    {"E014", "KeywordError", "Keyword", SLM::ErrorCategory::SYNTAX_ERROR},
    {"E015", "SyntaxError", "Syntax", SLM::ErrorCategory::SYNTAX_ERROR},
    {"E016", "BareErrorAsBindingError", "Bare error as binding", SLM::ErrorCategory::CONTROL_FLOW},
    {"E017", "DoubleFreeError", "Double free", SLM::ErrorCategory::MEMORY_ERROR},
    {"E018", "UseAfterFreeError", "Use after free", SLM::ErrorCategory::MEMORY_ERROR},
    {"E019", "CircularImportError", "Circular import", SLM::ErrorCategory::DECLARATION},
    {"E020", "ConflictingExternDeclarationError", "Conflicting extern declaration", SLM::ErrorCategory::DECLARATION},
    {"E021", "ExportMutNotSupportedError", "Export mut not supported", SLM::ErrorCategory::DECLARATION},
    {"E022", "ExportNonConstantInitializerError", "Export non constant initializer", SLM::ErrorCategory::DECLARATION},
    {"E023", "ExportOutsideTopLevelError", "Export outside top level", SLM::ErrorCategory::DECLARATION},
    {"E024", "ImportSymbolCollisionError", "Import symbol collision", SLM::ErrorCategory::DECLARATION},
    {"E025", "ModuleArityOrTypeError", "Module arity or type", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    {"E026", "ModuleMethodNotFoundError", "Module method not found", SLM::ErrorCategory::GENERAL},
    {"E027", "ModuleNotFoundError", "Module not found", SLM::ErrorCategory::DECLARATION},
    {"E028", "OperatorBuiltinPairError", "Operator builtin pair", SLM::ErrorCategory::DECLARATION},
    {"E029", "ScopeOperatorOnInstanceError", "Scope operator on instance", SLM::ErrorCategory::DECLARATION},
    {"E030", "UnknownModuleSymbolError", "Unknown module symbol", SLM::ErrorCategory::DECLARATION},
    {"E031", "UnknownScopeQualifierError", "Unknown scope qualifier", SLM::ErrorCategory::DECLARATION},
    {"E032", "UnqualifiedTypeNotVisibleError", "Unqualified type not visible", SLM::ErrorCategory::DECLARATION},
    {"E033", "DuplicateDeclarationError", "Duplicate declaration", SLM::ErrorCategory::DECLARATION},
    {"E034", "DuplicateTypeNameError", "Duplicate type name", SLM::ErrorCategory::DECLARATION},
    {"E035", "InvalidTransitionError", "Invalid transition", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E036", "InvalidTransitionTargetError", "Invalid transition target", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E037", "LookupError", "Lookup", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E038", "NestedFunctionError", "Nested function", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    {"E039", "NoSuchVariantError", "No such variant", SLM::ErrorCategory::DECLARATION},
    {"E040", "ReferenceError", "Reference", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E041", "UnknownFunctionError", "Unknown function", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E042", "UnknownTypeError", "Unknown type", SLM::ErrorCategory::DECLARATION},
    {"E043", "UseBeforeDeclarationError", "Use before declaration", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E044", "WrongArityError", "Wrong arity", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    {"E045", "ConstructorCalledOnInstanceError", "Constructor called on instance", SLM::ErrorCategory::DECLARATION},
    {"E046", "ConstructorCannotBeStaticError", "Constructor cannot be static", SLM::ErrorCategory::DECLARATION},
    {"E047", "DefaultConstructorArityError", "Default constructor arity", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    {"E048", "DotOperatorOnNonInstanceError", "Dot operator on non instance", SLM::ErrorCategory::DECLARATION},
    {"E049", "IncompleteConstructorError", "Incomplete constructor", SLM::ErrorCategory::DECLARATION},
    {"E050", "NoConstructorDefinedError", "No constructor defined", SLM::ErrorCategory::DECLARATION},
    {"E051", "ReservedConstructorNameError", "Reserved constructor name", SLM::ErrorCategory::DECLARATION},
    {"E052", "StaticMethodCalledOnInstanceError", "Static method called on instance", SLM::ErrorCategory::DECLARATION},
    {"E053", "StaticOutsideStructError", "Static outside struct", SLM::ErrorCategory::DECLARATION},
    {"E054", "StructMethodNotFoundError", "Struct method not found", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E055", "AbsArgTypeError", "Abs arg type", SLM::ErrorCategory::BUILTIN_FUNCTION},
    {"E056", "ArgumentTypeError", "Argument type", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E057", "ArithmeticError", "Arithmetic", SLM::ErrorCategory::ARITHMETIC},
    {"E058", "CompareError", "Compare", SLM::ErrorCategory::CONTROL_FLOW},
    {"E059", "CompareTypeError", "Compare type", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E060", "ImmutableAssignmentError", "Immutable assignment", SLM::ErrorCategory::DECLARATION},
    {"E061", "InvalidAssignmentTargetError", "Invalid assignment target", SLM::ErrorCategory::DECLARATION},
    {"E062", "NullAssignmentError", "Null assignment", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E063", "PowerArgTypeError", "Power arg type", SLM::ErrorCategory::BUILTIN_FUNCTION},
    {"E064", "ReturnTypeMismatchError", "Return type mismatch", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E065", "StringConversionError", "String conversion", SLM::ErrorCategory::STRING_CONVERSION},
    {"E066", "TypeError", "Type", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E067", "VoidValueError", "Void value", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    // Appended after the initial 67-entry generation (see this file's own
    // doc comment on why new entries go at the end, not resorted in).
    {"E068", "UnknownStructFieldError", "Unknown struct field", SLM::ErrorCategory::UNDECLARED_VAR},
};

inline constexpr size_t AllErrorCodesCount = sizeof(AllErrorCodes) / sizeof(AllErrorCodes[0]);

namespace ErrorCodes {

// Returns nullptr if ClassName has no registered code (e.g. a TurfError
// subclass added after this table was last regenerated, or a diagnostic
// raised through the raw, untyped LogErrorAt() path that has no TurfError
// instance -- and therefore no C++ class -- behind it at all).
inline const ErrorCodeEntry *lookupByClassName(const std::string &ClassName) {
  for (const auto &Entry : AllErrorCodes)
    if (ClassName == Entry.ClassName)
      return &Entry;
  return nullptr;
}

// Returns nullptr for an unrecognized code -- callers (currently just
// `turf --explain`) are expected to print a clean "unknown error code"
// message rather than dereference this.
inline const ErrorCodeEntry *lookupByCode(const std::string &Code) {
  for (const auto &Entry : AllErrorCodes)
    if (Code == Entry.Code)
      return &Entry;
  return nullptr;
}

} // namespace ErrorCodes

#endif
