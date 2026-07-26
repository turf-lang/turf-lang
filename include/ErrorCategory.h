#ifndef ERROR_CATEGORY_H
#define ERROR_CATEGORY_H

#include <string>
#include <unordered_map>

namespace SLM {

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
  BUILTIN_FUNCTION, // Errors specific to a builtin function (lengthof, typeof,
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
  // typeof/lengthof/assert/fail/sizeof, the only names actually wired
  // directly into the compiler (see general.txt's "TRUE INTRINSICS vs.
  // STANDARD LIBRARY" section for the full explanation). This list
  // previously also contained "printline"/"print"/"input" (ordinary
  // stdlib functions declared in lib/std/io.tr, requiring `import io;`
  // like any other module -- no longer even implemented via @intrinsic,
  // which has been removed from the compiler entirely) and "abs"/"power"
  // (used to be @intrinsic stubs, now pure-Turf/extern-fn stdlib in
  // lib/std/math.tr, per Builtins.cpp's own comments) -- all five were
  // wrongly routed to the BUILTIN_FUNCTION
  // category below as a result. Order: longer/more-specific names first
  // to avoid partial matches.
  static const char *builtinNames[] = {"lengthof", "typeof", "assert",
                                       "fail",      "sizeof"};
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
  // mechanism already used for lengthof/typeof/print/input -- see that
  // map's doc comment.
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
  // about it, contains the substring "every module you import" in its Note
  // text -- checking "module" before "struct"/"enum" would misroute that
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
  if (msg.find("module") != std::string::npos ||
      msg.find("Module") != std::string::npos ||
      msg.find("import") != std::string::npos ||
      msg.find("Import") != std::string::npos)
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

// Deterministic classifier: pattern-matches the (ANSI-stripped) error message
// to determine which language construct caused the error.
// The optional `contextKeyword` (extracted from the error message or code
// context) allows more precise classification when the error message itself
// is generic (e.g. "unknown token when expecting an expression").
inline ErrorCategory classifyError(const std::string &msg,
                                   const std::string &contextKeyword = "") {
  // Keyword-based sub-classification (highest priority)
  // If we know which construct triggered the error, use that directly.
  // Only TRUE intrinsics (see extractContextKeyword's doc comment) map to
  // BUILTIN_FUNCTION -- printline/print/input used to be included here too,
  // but they're ordinary lib/std/io.tr stdlib functions, not intrinsics;
  // an error calling one of those now falls through to the generic
  // pattern-based classification below, same as any other stdlib/user
  // function.
  if (!contextKeyword.empty()) {
    if (contextKeyword == "lengthof" || contextKeyword == "typeof" ||
        contextKeyword == "assert" || contextKeyword == "fail" ||
        contextKeyword == "sizeof")
      return ErrorCategory::BUILTIN_FUNCTION;
  }

  // Pattern-based classification
  // Order matters: more specific patterns first.

  // Array errors
  if (msg.find("Array index") != std::string::npos ||
      msg.find("Array size") != std::string::npos ||
      msg.find("Array element") != std::string::npos ||
      msg.find("array") != std::string::npos)
    return ErrorCategory::ARRAY_ERROR;

  // Type mismatch
  if (msg.find("Type mismatch") != std::string::npos ||
      msg.find("Wrong return type") != std::string::npos)
    return ErrorCategory::TYPE_MISMATCH;

  // Undeclared / use-before-declaration
  if (msg.find("can't find anything named") != std::string::npos ||
      msg.find("before telling me what it is") != std::string::npos)
    return ErrorCategory::UNDECLARED_VAR;

  // Control flow
  if (msg.find("Control flow error") != std::string::npos ||
      msg.find("promises to give back a result") != std::string::npos ||
      msg.find("will never happen") != std::string::npos ||
      msg.find("impossible to reach") != std::string::npos)
    return ErrorCategory::CONTROL_FLOW;

  // String conversion / lengthof / builtin arg types
  if (msg.find("can't convert") != std::string::npos ||
      msg.find("lengthof only works") != std::string::npos ||
      msg.find("lengthof") != std::string::npos ||
      msg.find("'abs' only works") != std::string::npos ||
      msg.find("'power' only works") != std::string::npos)
    return ErrorCategory::STRING_CONVERSION;

  // Arithmetic
  if (msg.find("Math trouble") != std::string::npos ||
      msg.find("with zero") != std::string::npos)
    return ErrorCategory::ARITHMETIC;

  // Syntax / keyword
  if (msg.find("tiny mistake") != std::string::npos ||
      msg.find("don't know the word") != std::string::npos ||
      msg.find("Expected '") != std::string::npos ||
      msg.find("unknown token") != std::string::npos)
    return ErrorCategory::SYNTAX_ERROR;

  // Declaration issues
  if (msg.find("already created a variable") != std::string::npos ||
      msg.find("hides an older one") != std::string::npos)
    return ErrorCategory::DECLARATION;

  // Void value misuse
  if (msg.find("void value") != std::string::npos)
    return ErrorCategory::FUNCTION_SIGNATURE;

  // Unknown / misspelled function name.
  // SemanticError wraps these as "Oops! Something doesn't add up: Unknown
  // function: 'x'" so we match the inner text. Must be checked here, after
  // SYNTAX_ERROR, because SemanticError does NOT produce "Expected '" or "tiny
  // mistake".
  if (msg.find("Unknown function") != std::string::npos ||
      msg.find("Unknown builtin function") != std::string::npos)
    return ErrorCategory::BUILTIN_FUNCTION;

  // For-loop structural errors -- all produced by ParseForExpr in Parser.cpp
  // via LogErrorAt (no SyntaxError class wrapper), so they don't match
  // "Expected '" above.
  if (msg.find("for loop range") != std::string::npos ||
      msg.find("Expected 'in' after loop variable") != std::string::npos ||
      msg.find("Expected 'step' after range") != std::string::npos ||
      msg.find("Expected '{' after step expression") != std::string::npos ||
      msg.find("Expected identifier after 'for'") != std::string::npos)
    return ErrorCategory::CONTROL_FLOW;

  // While / if structural errors produced via LogErrorAt (no SyntaxError
  // wrapper)
  if (msg.find("Expected '{' after while condition") != std::string::npos ||
      msg.find("Expected 'then' or '{'") != std::string::npos ||
      msg.find("Expected '{' after elseif condition") != std::string::npos)
    return ErrorCategory::CONTROL_FLOW;

  return ErrorCategory::GENERAL;
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
// that instead of the generic category doc (e.g. "lengthof" →
// builtin_lengthof.txt).
inline std::string getCategoryDocPath(ErrorCategory cat,
                                      const std::string &contextKeyword = "") {
  // Construct-specific docs (highest priority)
  if (!contextKeyword.empty()) {
    static const std::unordered_map<std::string, std::string> constructDocs = {
        {"lengthof", "docs/slm/builtin_lengthof.txt"},
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
