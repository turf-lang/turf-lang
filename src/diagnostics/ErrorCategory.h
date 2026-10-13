#ifndef ERROR_CATEGORY_H
#define ERROR_CATEGORY_H

#include <string>
#include <unordered_map>
#include <vector>

namespace SLM {

// One compiler-made edit to the source text: replace the columns
// [StartCol, EndCol) of `Line` with `Replacement`. Columns are 1-based and
// count bytes, exactly like SourceLocation::Col; EndCol is exclusive, so
// StartCol == EndCol is a pure insertion (before the character at StartCol)
// and an empty Replacement is a pure deletion. `Replacement` may contain '\n'
// to start new lines. Line 0 means "the diagnostic's own line", which is not
// always where the fix goes: a missing ';' is reported at the NEXT statement
// but belongs at the end of the previous one.
struct FixIt {
  int Line = 0;
  int StartCol = 1;
  int EndCol = 1;
  std::string Replacement;
};

// SLM-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §16.3 items 1 and 3):
// structured facts a diagnostic's own raising site already knows, attached
// via TurfError::withKnownType()/withFixIt()/withDeterministicFix()/
// trySetDeterministicRename() (Errors.h) and threaded through
// DiagnosticEngine::add() (Lexer.h) unchanged -- never re-derived from
// message text (the exact anti-pattern DIAG-001 removed for ErrorCategory
// above; see that record's "Removed assumptions"). A default-constructed
// (empty) instance is the common case and changes nothing about the
// existing pipeline. Lives here (rather than SLMIntegration.h, where
// AISuggestion lives) specifically so Lexer.h -- which cannot include
// SLMIntegration.h without a cycle (SLMIntegration.h includes Lexer.h for
// RelatedLocation) -- can still store one by value in DiagnosticEngine::Diag.
struct DiagnosticHints {
  // Item 3: short, human-readable facts about the concrete types involved
  // in this diagnostic that are NOT already visible by reading the pinned
  // source lines -- e.g. a `guess`-inferred local's resolved type, which the
  // declaration line itself never spells out. Sourced directly from
  // TypeChecker::NodeTypes_/VariableSymbol::WasGuessInferred at the call
  // site that already computed them, not re-inferred here.
  std::vector<std::string> KnownTypes;

  // SLM_IMPROVEMENT_PLAN C4: the declaration of the struct a field or method
  // diagnostic is about, from the raise site's own type information --
  // `struct Box<T> { v: T, n: int }`. The pinned source lines can show a
  // `struct Box {` header, but a struct is usually declared across several
  // lines and the model needs ALL its fields to say which one the user meant
  // (`pasword` -> `password`) or what a method may use. Empty unless the raise
  // site knows the receiver's type; shown to the model as its own field,
  // "receiver", never inside the code.
  std::string ReceiverDecl;

  // Item 1 (deterministic-fix layer): the exact edit the raising site already
  // knows fixes this diagnostic -- an unambiguous "did you mean" rename, a
  // missing `<T>`, a misplaced `pub`, a missing ';' (see the withFixIt() family
  // in Errors.h for which sites attach one, and why the rest do not). All of
  // them together are ONE fix: SLM::tryDeterministicFix() (SLMIntegration.cpp)
  // applies them to a fresh sandbox BEFORE ever invoking the model, and the
  // model is never called at all if that sandbox validation succeeds. A fix
  // that does not validate falls through to the model like any other
  // diagnostic.
  std::vector<FixIt> FixIts;
  // Why the edit is right, in a sentence a beginner can read ("'x' isn't
  // declared anywhere visible, but 'y' is an exact match nearby ..."). Shown
  // as the suggestion's explanation, so a fix with FixIts should always set it.
  std::string FixReason;

  bool empty() const {
    return KnownTypes.empty() && ReceiverDecl.empty() && FixIts.empty();
  }
};

enum class ErrorCategory {
  TYPE_MISMATCH,
  UNDECLARED_VAR,
  CONTROL_FLOW,
  SYNTAX_ERROR,
  ARITHMETIC,
  STRING_CONVERSION,
  ARRAY_ERROR,
  DECLARATION,
  FUNCTION_SIGNATURE,
  BUILTIN_FUNCTION, // Errors specific to a builtin function (typeof, sizeof,
                    // etc.)
  MEMORY_ERROR,     // Use-after-free, double-free (AliasAnalysisPass)
  GENERAL
};

// Extract a keyword/construct name from the error message ONLY.
// Scanning codeContext for keyword hints is intentionally NOT done here:
// the code context window (10 lines) often contains unrelated builtins
// (e.g. a printline() call near a for-loop error), which would poison
// the classification and serve the wrong documentation to the SLM.
// Returns empty string if no recognized keyword is found in the message.
inline std::string
extractContextKeyword(const std::string &msg,
                      const std::string & /*codeContext*/ = "") {
  // Check for TRUE intrinsic names mentioned in the error message itself --
  // typeof/sizeof, the only names actually wired directly into
  // the compiler (see general.txt's "TRUE INTRINSICS vs. STANDARD LIBRARY"
  // section for the full explanation). This list previously also contained
  // "printline"/"print"/"input" (ordinary stdlib functions declared in
  // lib/std/io.tr, requiring `import io;` like any other module -- no
  // longer even implemented via @intrinsic, which has been removed from
  // the compiler entirely), "abs"/"power" (used to be @intrinsic stubs, now
  // pure-Turf/extern-fn stdlib in lib/std/math.tr, per Builtins.cpp's own
  // comments), "assert"/"fail" (used to be compiler intrinsics with
  // their own CodegenIntrinsic branches; now ordinary, real overloaded Turf
  // functions in lib/std/test.tr), and "lengthof" (deleted entirely,
  // STRING_UFCS_OPERATOR_CHAR_RFC.md §7 step 7 -- strings::length(s) is the
  // real replacement, an ordinary stdlib function) -- all eight were
  // wrongly routed to the BUILTIN_FUNCTION category below as a result.
  // Order: longer/more-specific names first to avoid partial matches.
  static const char *builtinNames[] = {"typeof", "sizeof"};
  for (const char *name : builtinNames) {
    if (msg.find(name) != std::string::npos)
      return name;
  }

  // Struct/enum construct keywords -- checked BEFORE controlKeywords below,
  // not after. "for" (a controlKeyword) is a short, common substring that
  // appears INSIDE unrelated phrases too -- concretely, "...out of range
  // for enum 'Color'..." (the enum-cast-bounds error) contains "for" as
  // part of "for enum", which used to match the controlKeywords loop FIRST
  // and return "for", so this message never reached the enum check below
  // at all. Struct/enum detection is more specific and more valuable here,
  // so it takes priority.
  //
  // Both casings are checked because real messages use both ("Struct 'X'
  // has no field..." vs. "...a struct named..."; "Enum 'X' already
  // has..." vs. "...out of range for enum"). Routes to docs/slm/structs.txt
  // / enums.txt via getCategoryDocPath()'s constructDocs map, the same
  // mechanism already used for typeof -- see that map's doc comment.
  //
  // "has no method named" is ALSO checked here even though it never
  // contains the literal word "struct" -- StructMethodNotFoundError's
  // message is just "'<StructName>' has no method named '<Method>'.".
  // Confirmed unambiguous: the module-call equivalent
  // (ModuleMethodNotFoundError) says "has no FUNCTION named" instead, so
  // this phrase is never emitted for anything but a struct method lookup.
  // Without this, arguably the single most common struct-related error
  // (calling a method that doesn't exist) fell through to
  // undeclared_var.txt instead of the much more targeted structs.txt.
  if (msg.find("Struct") != std::string::npos ||
      msg.find("struct") != std::string::npos ||
      msg.find("has no method named") != std::string::npos)
    return "struct";
  if (msg.find("Enum") != std::string::npos ||
      msg.find("enum") != std::string::npos)
    return "enum";

  // Module/import construct keywords -- checked AFTER struct/enum above
  // (struct/enum must win first: DuplicateTypeNameError's own message, for
  // a plain same-file struct-name collision with nothing module-related
  // about it, contains the substring "module" in its Note text ("another
  // file or module may reuse it", FDN-003) -- checking "module" before
  // "struct"/"enum" would misroute that
  // error to modules.txt instead of structs.txt/enums.txt, exactly the
  // ordering trap this function's struct/enum-before-controlKeywords
  // reordering already fixed once for "for enum"/"for"). Still checked
  // before controlKeywords, for the same reason struct/enum is. Routes to
  // docs/slm/modules.txt via getCategoryDocPath()'s constructDocs map.
  // Covers CircularImportError ("Circular import detected"),
  // UnknownModuleSymbolError/ModuleMethodNotFoundError/ModuleArityOrTypeError
  // ("Module 'X' has..."), ImportSymbolCollisionError ("...imported from
  // two different modules..."), ModuleNotFoundError ("Could not find
  // module..."), and UnknownScopeQualifierError ("...isn't a known enum or
  // module.") -- the last one is deliberately included here even though it
  // could also be an enum-shaped mistake, since a bad `::` qualifier is
  // fundamentally an import/namespace question, not a struct/enum one.
  // "export" is included here too: ExportOutsideTopLevelError/
  // ExportMutNotSupportedError/ExportNonConstantInitializerError are all
  // about the module-visibility feature `export immut` (docs/slm/
  // modules.txt now documents it directly), even though their own message
  // text doesn't happen to also contain "module"/"import".
  if (msg.find("module") != std::string::npos ||
      msg.find("Module") != std::string::npos ||
      msg.find("import") != std::string::npos ||
      msg.find("Import") != std::string::npos ||
      msg.find("export") != std::string::npos ||
      msg.find("Export") != std::string::npos)
    return "module";

  // Check for control-flow construct keywords in the error message.
  static const char *controlKeywords[] = {"elseif", "while", "for",     "if",
                                          "return", "break", "continue"};
  for (const char *kw : controlKeywords) {
    if (msg.find(kw) != std::string::npos)
      return kw;
  }

  return "";
}

inline ErrorCategory categoryFromString(const std::string &str) {
  if (str == "TYPE_MISMATCH")
    return ErrorCategory::TYPE_MISMATCH;
  if (str == "UNDECLARED_VARIABLE")
    return ErrorCategory::UNDECLARED_VAR;
  if (str == "CONTROL_FLOW")
    return ErrorCategory::CONTROL_FLOW;
  if (str == "SYNTAX_ERROR")
    return ErrorCategory::SYNTAX_ERROR;
  if (str == "ARITHMETIC")
    return ErrorCategory::ARITHMETIC;
  if (str == "STRING_CONVERSION")
    return ErrorCategory::STRING_CONVERSION;
  if (str == "ARRAY_ERROR")
    return ErrorCategory::ARRAY_ERROR;
  if (str == "DECLARATION")
    return ErrorCategory::DECLARATION;
  if (str == "FUNCTION_SIGNATURE")
    return ErrorCategory::FUNCTION_SIGNATURE;
  if (str == "BUILTIN_FUNCTION")
    return ErrorCategory::BUILTIN_FUNCTION;
  if (str == "MEMORY_ERROR")
    return ErrorCategory::MEMORY_ERROR;
  return ErrorCategory::GENERAL;
}

// Human-readable category name (also used in the prompt).
inline const char *getCategoryName(ErrorCategory cat) {
  switch (cat) {
  case ErrorCategory::TYPE_MISMATCH:
    return "TYPE_MISMATCH";
  case ErrorCategory::UNDECLARED_VAR:
    return "UNDECLARED_VARIABLE";
  case ErrorCategory::CONTROL_FLOW:
    return "CONTROL_FLOW";
  case ErrorCategory::SYNTAX_ERROR:
    return "SYNTAX_ERROR";
  case ErrorCategory::ARITHMETIC:
    return "ARITHMETIC";
  case ErrorCategory::STRING_CONVERSION:
    return "STRING_CONVERSION";
  case ErrorCategory::ARRAY_ERROR:
    return "ARRAY_ERROR";
  case ErrorCategory::DECLARATION:
    return "DECLARATION";
  case ErrorCategory::FUNCTION_SIGNATURE:
    return "FUNCTION_SIGNATURE";
  case ErrorCategory::BUILTIN_FUNCTION:
    return "BUILTIN_FUNCTION";
  case ErrorCategory::MEMORY_ERROR:
    return "MEMORY_ERROR";
  case ErrorCategory::GENERAL:
    return "GENERAL";
  }
  return "GENERAL";
}

// Returns the path to the external documentation file for a given category.
// When a contextKeyword is provided and a construct-specific doc exists, use
// that instead of the generic category doc (e.g. "typeof" →
// builtin_typeof.txt).
inline std::string getCategoryDocPath(ErrorCategory cat,
                                      const std::string &contextKeyword = "") {
  // Construct-specific docs (highest priority)
  if (!contextKeyword.empty()) {
    static const std::unordered_map<std::string, std::string> constructDocs = {
        {"typeof", "docs/slm/builtin_typeof.txt"},
        // structs.txt also covers generic structs/functions (fn<T>,
        // struct<T>) -- there's no reliable standalone keyword for
        // "generic" in actual error message text (confirmed: the word
        // "generic" never appears in any raised diagnostic), so a separate
        // constructKeyword trigger for it isn't derivable the same way
        // struct/enum are. Almost all real generic usage in this language
        // goes through a generic STRUCT (List<T>, Box<T>) anyway, so
        // folding the content in here still reaches the relevant errors.
        {"struct", "docs/slm/structs.txt"},
        {"enum", "docs/slm/enums.txt"},
        {"module", "docs/slm/modules.txt"},
    };
    auto it = constructDocs.find(contextKeyword);
    if (it != constructDocs.end())
      return it->second;
  }

  // Category-level docs
  static const std::unordered_map<ErrorCategory, std::string> paths = {
      {ErrorCategory::TYPE_MISMATCH, "docs/slm/type_mismatch.txt"},
      {ErrorCategory::UNDECLARED_VAR, "docs/slm/undeclared_var.txt"},
      {ErrorCategory::CONTROL_FLOW, "docs/slm/control_flow.txt"},
      {ErrorCategory::SYNTAX_ERROR, "docs/slm/syntax_error.txt"},
      {ErrorCategory::ARITHMETIC, "docs/slm/arithmetic.txt"},
      {ErrorCategory::STRING_CONVERSION, "docs/slm/string_conversion.txt"},
      {ErrorCategory::ARRAY_ERROR, "docs/slm/array_error.txt"},
      {ErrorCategory::DECLARATION, "docs/slm/declaration.txt"},
      {ErrorCategory::FUNCTION_SIGNATURE, "docs/slm/function_signature.txt"},
      {ErrorCategory::BUILTIN_FUNCTION,
       "docs/slm/general.txt"}, // fallback for unlisted builtins
      // MEMORY_ERROR has no dedicated doc file yet -- documentation content
      // is explicitly out of scope for this pass. Falls back to
      // general.txt for now.
      {ErrorCategory::MEMORY_ERROR, "docs/slm/general.txt"},
      {ErrorCategory::GENERAL, "docs/slm/general.txt"},
  };

  auto it = paths.find(cat);
  if (it != paths.end())
    return it->second;
  return "docs/slm/general.txt";
}

} // namespace SLM

#endif // ERROR_CATEGORY_H
