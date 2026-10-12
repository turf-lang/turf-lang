#ifndef ERROR_CODES_H
#define ERROR_CODES_H

// Auto-generated registry mapping each TurfError subclass to a stable
// `E0NN` diagnostic code, consumed by `turf --explain E0NN` (main.cpp) and
// by DiagnosticEngine's header line (Lexer.cpp's printDiag()). Codes are
// looked up at raise()-time via typeid(*this) (Errors.h's
// TurfError::getCode()) rather than a per-constructor `Code_ = "E0NN";`
// line -- this means adding a brand-new TurfError subclass elsewhere in the
// compiler costs exactly one line here, not a constructor-body edit at each
// of its call sites. (How many codes exist is AllErrorCodesCount below; a
// count written in this comment went stale at 67 while the table grew past
// 90.) Assignment order is alphabetical by
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
#include <fstream>
#include <string>

// The installation root (src/ParserModule.cpp): $TURF_HOME, else the
// compiler binary's own <dir>/.., the anchor `import` already resolves lib/std
// against. Declared here, not by including Parser.h, so this header stays
// free of the parser.
std::string TurfInstallRoot();

// A doc shipped with the compiler (`docs/errors/codes/E090.txt`,
// `docs/slm/structs.txt`), resolved against the installation root. These
// paths used to be opened relative to the CURRENT DIRECTORY, so `turf explain`
// printed its full write-up only when run from the repository root, and fell
// back to a one-line description everywhere else; `--smart` lost its category
// docs the same way. The docs belong to the installation exactly as lib/std
// does, and the release archive ships them under the same root.
inline std::string InstalledDocPath(const std::string &Relative) {
  if (!Relative.empty() && (Relative[0] == '/' || Relative[0] == '\\' ||
                            (Relative.size() > 1 && Relative[1] == ':')))
    return Relative; // already absolute (POSIX, or a Windows drive path)
  std::string Root = TurfInstallRoot();
  if (Root.empty())
    return Relative;
  char Last = Root.back();
  return Root + (Last == '/' || Last == '\\' ? "" : "/") + Relative;
}

struct ErrorCodeEntry {
  const char *Code;
  const char *ClassName;
  const char *Description;
  SLM::ErrorCategory Category;
  // True for a small, deliberately curated set of codes whose one-line
  // compiler message is unlikely to be enough on its own -- genuinely
  // non-obvious concepts (raw-pointer lifetime rules, unbounded
  // recursion, state-machine transitions) rather than a simple typo/
  // arity mismatch a beginner can usually fix from the message alone.
  // printDiag() (Lexer.cpp) gives these a more insistent footer than the
  // plain "Run turf --explain E0NN for more details." every other coded
  // error gets. Defaults to false via this member initializer, so every
  // existing positional aggregate-init row below (4 values) still
  // compiles unchanged -- only the handful of rows that opt in add a
  // 5th `true`.
  bool Important = false;
};

// The codes the raw LogErrorAt() path (Lexer.h) uses -- named, so a call site
// reads `ErrCode::MissingSemicolon`, not a magic "E102" that nothing checks.
// Each constant is also the first column of its row in AllErrorCodes below
// (one source of truth), and tests/api/analysis_test.cpp checks that every
// one resolves. The first block is the SYNTAX FAMILIES added for issue #263;
// the last block names pre-existing codes a raw site reuses because they
// already describe that exact mistake (their `docs/errors/codes/` pages are
// written for it). Never reassign a number: append, like the table.
namespace ErrCode {
inline constexpr const char *UnterminatedLiteral = "E099";
inline constexpr const char *InvalidEscapeSequence = "E100";
inline constexpr const char *InvalidCharLiteral = "E101";
inline constexpr const char *MissingSemicolon = "E102";
inline constexpr const char *UnexpectedToken = "E103";
inline constexpr const char *UnclosedDelimiter = "E104";
inline constexpr const char *MissingComma = "E105";
inline constexpr const char *MissingColon = "E106";
inline constexpr const char *MissingOpeningDelimiter = "E107";
inline constexpr const char *ExpectedName = "E108";
inline constexpr const char *ExpectedType = "E109";
inline constexpr const char *ExpectedKeyword = "E110";
inline constexpr const char *NestingTooDeep = "E111";
inline constexpr const char *VoidTypeNotAllowed = "E112";
inline constexpr const char *RemovedSyntax = "E113";
inline constexpr const char *DuplicateMember = "E114";
inline constexpr const char *InvalidOperatorDeclaration = "E115";
inline constexpr const char *ExternFunctionBody = "E116";
inline constexpr const char *InvalidErrorDeclaration = "E117";
inline constexpr const char *StatementOutsideFunction = "E118";
inline constexpr const char *MissingEntryPoint = "E119";
inline constexpr const char *InvalidLoopForm = "E120";
inline constexpr const char *InternalCompilerError = "E121";
inline constexpr const char *NonAddressableExpression = "E122";
inline constexpr const char *InvalidArraySize = "E123";
// The TypeChecker's (TypeChecker::reportError() requires one, issue #263):
// new families first, then pre-existing rows whose class was never raised
// (E002 E003 E004 E056 E057 E064 E067) or whose class is raised elsewhere.
inline constexpr const char *InvalidComparison = "E124";
inline constexpr const char *AmbiguousCall = "E125";
inline constexpr const char *UnsupportedOperation = "E126";
inline constexpr const char *EnumValueOutOfRange = "E127";
inline constexpr const char *MissingInitializer = "E128";
inline constexpr const char *NothingToInfer = "E129";
inline constexpr const char *RawPointerOutsideUnsafe = "E130";
inline constexpr const char *MethodOnNonStruct = "E131";
inline constexpr const char *InvalidRecover = "E132";
inline constexpr const char *ArrayNonIntegerIndex = "E002";
inline constexpr const char *ArraySizeMismatch = "E003";
inline constexpr const char *ArrayTypeMismatch = "E004";
inline constexpr const char *WrongArity = "E044";
inline constexpr const char *ArgumentType = "E056";
inline constexpr const char *ArithmeticOperand = "E057";
inline constexpr const char *ReturnTypeMismatch = "E064";
inline constexpr const char *TypeMismatch = "E066";
inline constexpr const char *VoidValue = "E067";
inline constexpr const char *DuplicateMatchArm = "E133";
inline constexpr const char *StringConversion = "E065";
inline constexpr const char *InvalidCast = "E134";
inline constexpr const char *NotCallable = "E135";
inline constexpr const char *InvalidModuleQualifier = "E136";
inline constexpr const char *NotAValue = "E137";
inline constexpr const char *MissingTypeArguments = "E138";
// Issue #260: `T[]` is a slice that only a parameter can hold.
inline constexpr const char *UnsizedArrayPlacement = "E140";
inline constexpr const char *ImmutableArrayWrittenThroughSlice = "E141";
// Range slicing, `arr[a..b]`.
inline constexpr const char *SliceOfNonArray = "E142";
inline constexpr const char *SliceRangeOutOfBounds = "E143";
inline constexpr const char *MutableViewOfImmutableArray = "E144";
inline constexpr const char *DefaultThroughReferenceCycle = "E147";
inline constexpr const char *ErrorPayload = "E096";
// Pre-existing codes a raw site reuses (see above).
inline constexpr const char *ExportMutNotSupported = "E021";
inline constexpr const char *ExportNonConstantInitializer = "E022";
inline constexpr const char *UnknownStructField = "E068";
inline constexpr const char *OperatorTemporaryWrite = "E098";
inline constexpr const char *ModuleArityOrType = "E025";
inline constexpr const char *DuplicateDeclaration = "E033";
} // namespace ErrCode

inline const ErrorCodeEntry AllErrorCodes[] = {
    {"E001", "ArrayBoundsError", "Array bounds", SLM::ErrorCategory::ARRAY_ERROR},
    // E002/E003/E004 had classes and pages and nothing raised them (found by
    // scripts/error_codes_check.sh, issue #263); the TypeChecker raises them now,
    // through TypeError's code override -- the dead classes are deleted.
    {ErrCode::ArrayNonIntegerIndex, "ArrayNonIntegerIndexError", "Index must be an integer", SLM::ErrorCategory::ARRAY_ERROR},
    {ErrCode::ArraySizeMismatch, "ArraySizeMismatchError", "Array size mismatch", SLM::ErrorCategory::ARRAY_ERROR},
    {ErrCode::ArrayTypeMismatch, "ArrayTypeMismatchError", "Array element type mismatch", SLM::ErrorCategory::ARRAY_ERROR},
    {"E005", "ForEachNonArrayError", "For each non array", SLM::ErrorCategory::ARRAY_ERROR},
    {"E006", "ForEachUnsizedArrayError", "For each unsized array (retired: T[] carries its length now)", SLM::ErrorCategory::ARRAY_ERROR},
    {"E007", "ControlFlowError", "Control flow", SLM::ErrorCategory::CONTROL_FLOW},
    {"E008", "DefiniteAssignmentError", "Definite assignment", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E009", "MissingReturnError", "Missing return", SLM::ErrorCategory::CONTROL_FLOW},
    {"E010", "RecursionWithoutBaseError", "Recursion without base", SLM::ErrorCategory::CONTROL_FLOW, true},
    {"E011", "SemanticError", "Semantic", SLM::ErrorCategory::GENERAL},
    {"E012", "StatementAfterTerminatorError", "Statement after terminator", SLM::ErrorCategory::CONTROL_FLOW},
    // RETIRED (issue #263): the same mistake as E012 ("code after a return /
    // break / continue"), reported only from a Codegen backstop that the CFG
    // pass always pre-empted with E012. The backstop now raises E012 itself.
    {"E013", "UnreachableCodeError", "Unreachable code (retired: now E012)", SLM::ErrorCategory::CONTROL_FLOW},
    {"E014", "KeywordError", "Keyword", SLM::ErrorCategory::SYNTAX_ERROR},
    // RETIRED (issue #263): one code for every "the shape of this code is wrong"
    // message told a beginner nothing about WHICH shape. Nothing raises E015
    // any more -- SyntaxError now takes its code from the caller (ErrCode::,
    // E099-E123) -- and the number is never reassigned. The row stays so an old
    // log's `turf --explain E015` still answers, pointing at the specific codes.
    {"E015", "SyntaxError", "Syntax (retired: now E099-E123)", SLM::ErrorCategory::SYNTAX_ERROR},
    {"E016", "BareErrorAsBindingError", "Bare error as binding", SLM::ErrorCategory::CONTROL_FLOW},
    {"E017", "DoubleFreeError", "Double free", SLM::ErrorCategory::MEMORY_ERROR, true},
    {"E018", "UseAfterFreeError", "Use after free", SLM::ErrorCategory::MEMORY_ERROR, true},
    {"E019", "CircularImportError", "Circular import", SLM::ErrorCategory::DECLARATION},
    {"E020", "ConflictingExternDeclarationError", "Conflicting extern declaration", SLM::ErrorCategory::DECLARATION},
    {"E021", "ExportMutNotSupportedError", "Export mut not supported", SLM::ErrorCategory::DECLARATION},
    {"E022", "ExportNonConstantInitializerError", "Export non constant initializer", SLM::ErrorCategory::DECLARATION},
    {"E023", "ExportOutsideTopLevelError", "Export outside top level", SLM::ErrorCategory::DECLARATION},
    {"E024", "ImportSymbolCollisionError", "Import symbol collision", SLM::ErrorCategory::DECLARATION},
    {ErrCode::ModuleArityOrType, "ModuleArityOrTypeError", "No matching overload", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    {"E026", "ModuleMethodNotFoundError", "Module method not found", SLM::ErrorCategory::GENERAL},
    {"E027", "ModuleNotFoundError", "Module not found", SLM::ErrorCategory::DECLARATION},
    {"E028", "OperatorBuiltinPairError", "Operator builtin pair", SLM::ErrorCategory::DECLARATION},
    {"E029", "ScopeOperatorOnInstanceError", "Scope operator on instance", SLM::ErrorCategory::DECLARATION},
    {"E030", "UnknownModuleSymbolError", "Unknown module symbol", SLM::ErrorCategory::DECLARATION},
    {"E031", "UnknownScopeQualifierError", "Unknown scope qualifier", SLM::ErrorCategory::DECLARATION},
    {"E032", "UnqualifiedTypeNotVisibleError", "Unqualified type not visible", SLM::ErrorCategory::DECLARATION},
    {"E033", "DuplicateDeclarationError", "Duplicate declaration", SLM::ErrorCategory::DECLARATION},
    {"E034", "DuplicateTypeNameError", "Duplicate type name", SLM::ErrorCategory::DECLARATION},
    {"E035", "InvalidTransitionError", "Invalid transition", SLM::ErrorCategory::TYPE_MISMATCH, true},
    {"E036", "InvalidTransitionTargetError", "Invalid transition target", SLM::ErrorCategory::TYPE_MISMATCH, true},
    // RETIRED (issue #263): `LookupError` was never constructed anywhere -- an
    // unknown function is E041 (UnknownFunctionError). Its own page said so.
    {"E037", "LookupError", "Lookup (retired: unknown functions are E041)", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E038", "NestedFunctionError", "Nested function", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    {"E039", "NoSuchVariantError", "No such variant", SLM::ErrorCategory::DECLARATION},
    {"E040", "ReferenceError", "Reference", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E041", "UnknownFunctionError", "Unknown function", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E042", "UnknownTypeError", "Unknown type", SLM::ErrorCategory::DECLARATION},
    {"E043", "UseBeforeDeclarationError", "Use before declaration", SLM::ErrorCategory::UNDECLARED_VAR},
    {ErrCode::WrongArity, "WrongArityError", "Wrong number of arguments", SLM::ErrorCategory::FUNCTION_SIGNATURE},
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
    // RETIRED (issue #263): E055/E063 described `abs`/`power` as compiler
    // intrinsics. They have been ordinary overloaded stdlib functions for a long
    // time, so a bad argument is now the ordinary E056 -- and the codes sat
    // documented, raised by nothing. Numbers are never reassigned; the pages
    // say so and point at E056.
    {"E055", "AbsArgTypeError", "Abs arg type (retired: now E056)", SLM::ErrorCategory::BUILTIN_FUNCTION},
    {ErrCode::ArgumentType, "ArgumentTypeError", "Wrong argument type", SLM::ErrorCategory::TYPE_MISMATCH},
    {ErrCode::ArithmeticOperand, "ArithmeticError", "Operator applied to the wrong kind of value", SLM::ErrorCategory::ARITHMETIC},
    {"E058", "CompareError", "Compare", SLM::ErrorCategory::CONTROL_FLOW},
    {"E059", "CompareTypeError", "Compare type", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E060", "ImmutableAssignmentError", "Immutable assignment", SLM::ErrorCategory::DECLARATION},
    {"E061", "InvalidAssignmentTargetError", "Invalid assignment target", SLM::ErrorCategory::DECLARATION},
    {"E062", "NullAssignmentError", "Null assignment", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E063", "PowerArgTypeError", "Power arg type (retired: now E056)", SLM::ErrorCategory::BUILTIN_FUNCTION},
    {ErrCode::ReturnTypeMismatch, "ReturnTypeMismatchError", "Wrong return type", SLM::ErrorCategory::TYPE_MISMATCH},
    {"E065", "StringConversionError", "String conversion", SLM::ErrorCategory::STRING_CONVERSION},
    {"E066", "TypeError", "Type", SLM::ErrorCategory::TYPE_MISMATCH},
    {ErrCode::VoidValue, "VoidValueError", "A void value can't be used here", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    // Appended after the initial 67-entry generation (see this file's own
    // doc comment on why new entries go at the end, not resorted in).
    {"E068", "UnknownStructFieldError", "Unknown struct field", SLM::ErrorCategory::UNDECLARED_VAR},
    {"E069", "StructLayoutCycleError", "Struct layout cycle", SLM::ErrorCategory::DECLARATION, true},
    // SEM-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): four new lexical
    // errors raised directly from the Lexer's own number-scanning branch
    // (see ErrorsLex.h's own top comment for why that's safe). Routed to
    // the existing SYNTAX_ERROR category, per §8's explicit instruction not
    // to add a new SLM::ErrorCategory for these.
    {"E070", "MalformedRadixPrefixError", "Malformed radix prefix", SLM::ErrorCategory::SYNTAX_ERROR},
    {"E071", "InvalidRadixDigitError", "Invalid radix digit", SLM::ErrorCategory::SYNTAX_ERROR},
    {"E072", "MisplacedSeparatorError", "Misplaced separator", SLM::ErrorCategory::SYNTAX_ERROR},
    {"E073", "MalformedExponentError", "Malformed exponent", SLM::ErrorCategory::SYNTAX_ERROR},
    // HYG-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): these two classes
    // already existed, fully written, but were never constructed --
    // CFG::reportFlowDiagnostics() wrote directly to std::cerr instead,
    // bypassing DiagnosticEngine entirely (no code, no color, no caret, no
    // hint). Routed through the existing classes; routed to the existing
    // CONTROL_FLOW category, per §8's explicit instruction not to add a new
    // SLM::ErrorCategory for these.
    {"E074", "UnreachableBlockWarning", "Unreachable block", SLM::ErrorCategory::CONTROL_FLOW},
    {"E075", "DeadBranchWarning", "Dead branch", SLM::ErrorCategory::CONTROL_FLOW},
    // SEM-002 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): destination/
    // literal-representability diagnostics (FREEZE.md §6.2 rows 3/6/8/9).
    // Routed to the existing ARITHMETIC category, per §8's explicit
    // instruction not to add a new SLM::ErrorCategory for these.
    {"E076", "IntegerLiteralOutOfRangeError", "Integer literal out of range", SLM::ErrorCategory::ARITHMETIC},
    {"E077", "IntegerLiteralNotExactAsDoubleError", "Integer literal not exact as double", SLM::ErrorCategory::ARITHMETIC},
    {"E078", "FloatLiteralOverflowError", "Float literal overflow", SLM::ErrorCategory::ARITHMETIC},
    // TYPE-002 (docs/planning/ARCHITECTURE_OVERHAUL.md §9.5 step 4): the
    // monomorphizer's instantiation worklist is depth-capped, not run to
    // real exhaustion. Registered in S11b alongside the class and the
    // depth constant it guards (Monomorphize::kMaxInstantiationDepth) even
    // though nothing raises it until S12a's worklist actually drains --
    // see docs/development/changes/TYPE-002.md.
    {"E079", "GenericInstantiationDepthError", "Generic instantiation depth exceeded", SLM::ErrorCategory::DECLARATION},
    // LANG-001 (docs/development/changes/LANG-001.md, §13.4's orphan rule):
    // an operator declaration where no operand is a struct/enum declared in
    // the SAME module as the operator. Prevents a third-party module from
    // redefining an operator's meaning on two types it does not own, which
    // would make the operator's behavior depend on import order.
    {"E080", "OperatorOrphanRuleError", "Operator orphan rule violation", SLM::ErrorCategory::DECLARATION},
    // LANG-002 (docs/development/changes/LANG-002.md): NullabilityPass's two
    // checks, raised at Codegen time from the same CFG dataflow state --
    // field-accessing a `&?T` that isn't proven non-null on this path, and
    // casting one to `&T` under the same condition.
    {"E081", "PossibleNullDereferenceError", "Possible null dereference", SLM::ErrorCategory::MEMORY_ERROR},
    {"E082", "UnnarrowedNullableCastError", "Unnarrowed nullable-to-safe-reference cast", SLM::ErrorCategory::TYPE_MISMATCH},
    // LANG-003 (docs/development/changes/LANG-003.md): a `return`/`break`/
    // `continue` found anywhere inside a `defer <statement>;`'s own
    // statement -- always illegal, since it can only ever escape to the
    // enclosing function/loop, never "just" the defer. Routed to the
    // existing CONTROL_FLOW category, per §8's explicit instruction not to
    // add a new SLM::ErrorCategory for these.
    {"E083", "DeferControlFlowError", "Defer control flow", SLM::ErrorCategory::CONTROL_FLOW},

    // STD-002 (ARCHITECTURE_OVERHAUL.md §8/§15.6(a)): `caller_location`
    // read outside a function declared `@caller_location`. DECLARATION,
    // matching StaticOutsideStructError's identical "this construct is
    // only valid in one specific declaration context" shape.
    {"E084", "CallerLocationOutsideMarkedFunctionError", "Caller location outside a marked function", SLM::ErrorCategory::DECLARATION},

    // FDN-003 (docs/development/changes/FDN-003.md): a bare type name that
    // could mean two visible types (the file's own and an imported one) now
    // that type identity is (module, name) rather than one global namespace.
    {"E085", "AmbiguousTypeReferenceError", "Ambiguous type reference", SLM::ErrorCategory::DECLARATION},

    // FDN-002 (docs/development/changes/FDN-002.md): EscapePass's one
    // diagnostic -- a Stack-provenance reference (points into the current
    // function's own frame) proven to flow into a return, a field of a
    // longer-lived object, a collection mutator/operator[]=, or heap()'s
    // own operand. Marked Important: "why can't I return &local" is exactly
    // the non-obvious-lifetime-rule case this flag exists for (same
    // reasoning as the raw-pointer/recursion examples in this file's own
    // header comment).
    {"E086", "DanglingReferenceEscapeError", "Dangling reference escape", SLM::ErrorCategory::MEMORY_ERROR, true},

    // LEC-002 (docs/development/changes/LEC-002.md): `pub`/`public` written
    // where visibility means nothing (on an operator, an import, a variable,
    // an enum variant, a parameter, inside a function, ...). Rejected rather
    // than ignored; the hint names what to write instead.
    {"E087", "MisplacedVisibilityError", "Misplaced visibility modifier", SLM::ErrorCategory::DECLARATION},

    // LEC-002: a reference from one module to a function, type (or, from
    // stage 4, method/field) another module did not mark `pub`.
    {"E088", "PrivateSymbolAccessError", "Private symbol access", SLM::ErrorCategory::DECLARATION, true},

    // LEC-002 stage 5: a `pub` function signature or `pub` field mentions a
    // type its own module did not mark `pub` (private-in-public).
    {"E089", "PrivateInPublicError", "Private type in public signature", SLM::ErrorCategory::DECLARATION, true},

    // LEC-001 stage 3: `import m;` matches more than one file across the
    // module search roots (never silent shadowing, FREEZE.md 6.16 F-PKG-2).
    {"E090", "AmbiguousModuleError", "Ambiguous module import", SLM::ErrorCategory::DECLARATION},

    // LEC-001 stage 4: in a package build, `import m;` names a module that
    // exists but that this package may not import (a transitive dependency,
    // or another package's internal module).
    {"E091", "ModuleNotImportableError", "Module not importable from this package", SLM::ErrorCategory::DECLARATION},

    // LEC-003 (docs/development/changes/LEC-003.md): destructuring patterns
    // and guards in `compare`. One class for every structural pattern rule
    // (the message and hint name which), one for a compare that does not
    // cover its subject, one warning for an arm that can never run.
    {"E092", "InvalidPatternError", "Invalid pattern", SLM::ErrorCategory::CONTROL_FLOW},
    {"E093", "NonExhaustiveCompareError", "Non-exhaustive compare", SLM::ErrorCategory::CONTROL_FLOW},
    {"E094", "UnreachableArmWarning", "Unreachable compare arm", SLM::ErrorCategory::CONTROL_FLOW},

    // LEC-004 (docs/development/changes/LEC-004.md): error-variant payloads
    // and `try`. One class for every payload rule (declaration, construction,
    // a bare reference to a variant that has fields), one for `try` used where
    // it cannot work. A destructuring pattern over an error value reuses
    // E092/E093.
    {"E095", "TryMisuseError", "Misplaced try", SLM::ErrorCategory::CONTROL_FLOW},
    {"E096", "ErrorPayloadError", "Invalid error payload", SLM::ErrorCategory::DECLARATION},

    // LEC-006 (docs/development/changes/LEC-006.md): a generic `operator<T>`
    // declaration whose type parameter no operand binds, or that owns none
    // of its operand types.
    {"E097", "GenericOperatorError", "Invalid generic operator", SLM::ErrorCategory::DECLARATION},
    // LEC-006: a write into the by-value result of an `operator[]` read.
    {"E098", "OperatorTemporaryWriteError", "Write to a temporary", SLM::ErrorCategory::TYPE_MISMATCH},

    // Issue #263 (parse-error codes): the SYNTAX FAMILIES. The 181 diagnostics
    // the Lexer/Parser/Codegen/driver used to print through the raw, untyped
    // LogErrorAt() path had no C++ class and so, because a code is looked up
    // BY CLASS, no code and no `--explain` page -- every parse error a beginner
    // can hit was a dead end. These rows are not classes: each is one KIND of
    // mistake (a missing ';', a bracket that is never closed, a name that was
    // expected) shared by every site that makes it, so a beginner learns the
    // kind once. The "ClassName" column is only the label `--explain` prints;
    // LogErrorAt() takes the code from the ErrCode:: constants above, and
    // LogErrorAt() REQUIRES one, so a raw diagnostic can never be codeless again.
    // Each category is the family's dominant one; a call site keeps passing its
    // own SLM category, exactly as before.
    {ErrCode::UnterminatedLiteral, "UnterminatedLiteralError", "Unterminated string or character literal", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::InvalidEscapeSequence, "InvalidEscapeSequenceError", "Invalid escape sequence", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::InvalidCharLiteral, "InvalidCharLiteralError", "Invalid character literal", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::MissingSemicolon, "MissingSemicolonError", "Missing semicolon", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::UnexpectedToken, "UnexpectedTokenError", "Unexpected token", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::UnclosedDelimiter, "UnclosedDelimiterError", "Unclosed bracket, brace or parenthesis", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::MissingComma, "MissingCommaError", "Missing comma", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::MissingColon, "MissingColonError", "Missing colon", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::MissingOpeningDelimiter, "MissingOpeningDelimiterError", "Missing opening brace or parenthesis", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::ExpectedName, "ExpectedNameError", "Expected a name", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::ExpectedType, "ExpectedTypeError", "Expected a type", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::ExpectedKeyword, "ExpectedKeywordError", "Expected a specific word or symbol", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::NestingTooDeep, "NestingTooDeepError", "Nested too deeply", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::VoidTypeNotAllowed, "VoidTypeNotAllowedError", "void used as a type", SLM::ErrorCategory::DECLARATION},
    {ErrCode::RemovedSyntax, "RemovedSyntaxError", "Syntax that no longer exists", SLM::ErrorCategory::DECLARATION},
    {ErrCode::DuplicateMember, "DuplicateMemberError", "Duplicate member", SLM::ErrorCategory::DECLARATION},
    {ErrCode::InvalidOperatorDeclaration, "InvalidOperatorDeclarationError", "Invalid operator declaration", SLM::ErrorCategory::DECLARATION},
    {ErrCode::ExternFunctionBody, "ExternFunctionBodyError", "extern function with a body", SLM::ErrorCategory::DECLARATION},
    {ErrCode::InvalidErrorDeclaration, "InvalidErrorDeclarationError", "Invalid error declaration", SLM::ErrorCategory::DECLARATION},
    {ErrCode::StatementOutsideFunction, "StatementOutsideFunctionError", "Statement outside a function", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::MissingEntryPoint, "MissingEntryPointError", "No main function", SLM::ErrorCategory::DECLARATION},
    {ErrCode::InvalidLoopForm, "InvalidLoopFormError", "Unsupported loop form", SLM::ErrorCategory::CONTROL_FLOW},
    {ErrCode::InternalCompilerError, "InternalCompilerError", "Internal compiler error", SLM::ErrorCategory::GENERAL, true},
    {ErrCode::NonAddressableExpression, "NonAddressableExpressionError", "Cannot take the address of this", SLM::ErrorCategory::GENERAL},
    {ErrCode::InvalidArraySize, "InvalidArraySizeError", "Invalid array size", SLM::ErrorCategory::ARRAY_ERROR},

    // The TypeChecker's kinds that had no code of their own (all of them were
    // E066 "Type mismatch!" with a free-text banner). Same convention as the
    // syntax families above: a kind of mistake, not a class.
    {ErrCode::InvalidComparison, "InvalidComparisonError", "Values can't be compared", SLM::ErrorCategory::TYPE_MISMATCH},
    {ErrCode::AmbiguousCall, "AmbiguousCallError", "Ambiguous call", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    {ErrCode::UnsupportedOperation, "UnsupportedOperationError", "Operation not supported here", SLM::ErrorCategory::TYPE_MISMATCH},
    {ErrCode::EnumValueOutOfRange, "EnumValueOutOfRangeError", "Enum value out of range", SLM::ErrorCategory::ARITHMETIC},
    {ErrCode::MissingInitializer, "MissingInitializerError", "Missing initializer", SLM::ErrorCategory::DECLARATION},
    {ErrCode::NothingToInfer, "NothingToInferError", "Nothing to infer a type from", SLM::ErrorCategory::DECLARATION},
    {ErrCode::RawPointerOutsideUnsafe, "RawPointerOutsideUnsafeError", "Raw pointer outside unsafe", SLM::ErrorCategory::MEMORY_ERROR, true},
    {ErrCode::MethodOnNonStruct, "MethodOnNonStructError", "Method called on a non-struct", SLM::ErrorCategory::UNDECLARED_VAR},
    {ErrCode::InvalidRecover, "InvalidRecoverError", "Invalid recover", SLM::ErrorCategory::CONTROL_FLOW},
    {ErrCode::DuplicateMatchArm, "DuplicateMatchArmError", "Duplicate matches pattern", SLM::ErrorCategory::CONTROL_FLOW},

    // The Resolver/Codegen constructed TypeError directly (not through
    // TypeChecker::reportError), so they were E066 too: five more kinds.
    {ErrCode::InvalidCast, "InvalidCastError", "That conversion isn't allowed", SLM::ErrorCategory::TYPE_MISMATCH},
    {ErrCode::NotCallable, "NotCallableError", "Not callable", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    {ErrCode::InvalidModuleQualifier, "InvalidModuleQualifierError", "Invalid module qualifier", SLM::ErrorCategory::DECLARATION},
    {ErrCode::NotAValue, "NotAValueError", "Not a value", SLM::ErrorCategory::FUNCTION_SIGNATURE},
    {ErrCode::MissingTypeArguments, "MissingTypeArgumentsError", "Missing type arguments", SLM::ErrorCategory::DECLARATION},
    {"E139", "MalformedNumberError", "Malformed number", SLM::ErrorCategory::SYNTAX_ERROR},
    {ErrCode::UnsizedArrayPlacement, "UnsizedArrayPlacementError", "Unsized array where a size is needed", SLM::ErrorCategory::ARRAY_ERROR},
    {ErrCode::ImmutableArrayWrittenThroughSlice, "ImmutableArrayWrittenThroughSliceError", "Immutable array passed to a function that changes it", SLM::ErrorCategory::ARRAY_ERROR},
    {ErrCode::SliceOfNonArray, "SliceOfNonArrayError", "Slicing something that isn't an array", SLM::ErrorCategory::ARRAY_ERROR},
    {ErrCode::SliceRangeOutOfBounds, "SliceRangeOutOfBoundsError", "Slice range out of bounds", SLM::ErrorCategory::ARRAY_ERROR},
    {ErrCode::MutableViewOfImmutableArray, "MutableViewOfImmutableArrayError", "Mutable view of an immutable array", SLM::ErrorCategory::ARRAY_ERROR},
    // LEC-008: `m::` spelled in a file that did not import `m` itself.
    {"E145", "ModuleNotImportedError", "Module not imported in this file", SLM::ErrorCategory::DECLARATION},
    // `@name(args)` that no pass claims, or whose arguments do not fit the
    // declaration under it (ResolverDecl.cpp's checkAnnotations()).
    {"E146", "InvalidAnnotationError", "Invalid annotation", SLM::ErrorCategory::DECLARATION},
    // LEC-009: `T::default()` on a type that reaches itself through `&`
    // fields, which would allocate a fresh default forever (CodegenCore.cpp's
    // EmitDefaultValue()).
    {ErrCode::DefaultThroughReferenceCycle, "DefaultThroughReferenceCycleError", "No default through a reference cycle", SLM::ErrorCategory::DECLARATION},
};

inline constexpr size_t AllErrorCodesCount = sizeof(AllErrorCodes) / sizeof(AllErrorCodes[0]);

namespace ErrorCodes {

// Returns nullptr if ClassName has no registered code (e.g. a TurfError
// subclass added after this table was last regenerated). A diagnostic raised
// through the raw LogErrorAt() path never reaches this lookup at all: it has no
// TurfError class to look up, so it names its code directly (ErrCode::, above),
// which LogErrorAt() requires.
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

// Per-code documentation lives in TWO parallel trees, one per audience --
// the split is deliberate, not duplication for its own sake:
//
//   docs/slm/codes/E0NN.txt     -- machine-facing. Plain text, no color
//                                  codes, written as dense grounding text
//                                  for a future `--explain-to-slm` to feed
//                                  the model directly.
//   docs/errors/codes/E0NN.txt  -- human-facing. Warmer, more explanatory
//                                  language, meant to be read by a person
//                                  in a terminal via `turf --explain`
//                                  (which colorizes it -- see main.cpp's
//                                  printHumanExplainDoc()).
//
// Both are opt-in overrides, checked with a real file-open (not just a
// string build) so callers get an honest "does this exist" answer -- most
// codes don't have either one yet. This exists for codes where the shared
// category/construct doc (SLM::getCategoryDocPath()) conflates genuinely
// distinct mistakes under one explanation (e.g. E068 "unknown struct
// field" was falling into the same structs.txt as E054 "unknown struct
// method", even though a misspelled field and a misspelled method call
// call for completely different fixes). Returns "" when no override
// exists in that tree; callers should fall back to the coarser
// keyword/category doc in that case, exactly like today.
inline std::string getSlmCodeDocPath(const std::string &Code) {
  std::string Path = InstalledDocPath("docs/slm/codes/" + Code + ".txt");
  std::ifstream Test(Path);
  return Test.is_open() ? Path : "";
}

inline std::string getHumanCodeDocPath(const std::string &Code) {
  std::string Path = InstalledDocPath("docs/errors/codes/" + Code + ".txt");
  std::ifstream Test(Path);
  return Test.is_open() ? Path : "";
}

} // namespace ErrorCodes

#endif
