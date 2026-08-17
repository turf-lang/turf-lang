#ifndef LEXER_H
#define LEXER_H

#include "ErrorCategory.h" // SLM::ErrorCategory -- lightweight, no LLVM/AST deps
#include <cctype>
#include <csetjmp>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Filename defaults to "" (rather than being mandatory) because SourceLocation
// is constructed via 2-argument aggregate-init ({Line, Col}) at hundreds of
// call sites across the codebase that predate this field -- those all still
// compile and just get an empty Filename, meaning "attribute this location to
// whatever file was being compiled/parsed when the diagnostic fired" (the
// DiagnosticEngine::add()-time fallback -- see its doc comment). Locations
// captured from CurLoc during lexing (the overwhelming majority of real AST
// node locations) DO get a real Filename -- see gettok()'s CurLoc assignment.
struct SourceLocation {
  int Line;
  int Col;
  std::string Filename;
};

// Carries the location of a contextually relevant secondary site alongside
// a diagnostic — e.g. "declared here", "previous definition", "overload: …".
// Populated at the exact point each error is raised, using real symbol-table
// data, so the SLM layer receives structurally correct locations with zero
// text-scanning or hardcoded syntax assumptions.
struct RelatedLocation {
  SourceLocation Loc;
  std::string Description; // e.g. "declared here", "overload: fn foo(int)"
};

extern std::ifstream *SourceFile;
extern double NumVal;
extern long long IntVal;
extern bool BoolVal;
extern char CharVal;
extern std::string IdentifierStr;
extern std::string StringVal;

extern std::vector<std::string> SourceLines;
// Persistent, cross-pass cache of every file's source lines, keyed by the
// exact path string used as SourceFilename while that file was being parsed
// (main input file AND every imported module). Unlike SourceLines (which is
// transient -- overwritten while parsing an import, then restored back to
// the importer's content once that import finishes), this is never cleared
// during a compile, so a diagnostic raised by a LATER pass (Resolver,
// TypeChecker, Codegen -- all of which run after every file has already
// finished parsing, at which point SourceLines has long since reverted to
// the main file) can still recover the correct source line for a location
// that originated inside an imported module. Indexed by DiagnosticEngine at
// print time via each Diag's Filename field.
extern std::map<std::string, std::vector<std::string>> AllSourceLines;
extern SourceLocation CurLoc;
extern int CurLine;
extern int CurCol;
// Reserved-word -> token-id table, consulted by gettok() after scanning any
// identifier-shaped run of characters. Mutable (not const) purely because it
// is a translation-unit-scope global initialized at static-init time in
// Lexer.cpp; nothing outside Lexer.cpp writes to it at runtime. Note that
// builtin/stdlib function names (print, malloc, abs, ...) are deliberately
// NOT in this table -- they are ordinary identifiers resolved later by the
// Resolver against lib/std/*.tr, not lexer-level keywords.
extern std::map<std::string, int> Keywords;

// RecoverableError : setjmp/longjmp based (LLVM disables C++ exceptions)
// The main loop calls setjmp(g_recoverJmp) before each statement.
// raise() calls longjmp(g_recoverJmp, 1) to unwind back to that point.
extern jmp_buf g_recoverJmp;
extern bool g_recoverActive; // true only while inside a setjmp guard
extern bool g_protoOnlyPass; // true during sub-pass 3a: emit prototypes only

// Current '{'/'('/'[' nesting depth relative to the top level (0), tracked
// globally by getNextToken() as each token is consumed. See its definition
// in Lexer.cpp for the full story (SkipToNextTopLevelBoundary()'s parse-
// error recovery in main.cpp is what actually consumes this).
extern int g_braceDepth;

// When true, the SLM is loaded and AI suggestions are generated for errors.
// Controlled by the --smart CLI flag. Default is off for deterministic output.
extern bool g_AIEnabled;
extern std::string SourceFilename;
// The original top-level file passed to the compiler -- see its definition
// in Lexer.cpp for why this is distinct from SourceFilename.
extern std::string MainSourceFilename;

// DiagnosticEngine
// Collects errors and warnings, guaranteeing at most one primary error per
// source line (cascading / secondary diagnostics on the same line are
// silently dropped). Call flushAll() after the main loop to print everything.
class DiagnosticEngine {
public:
  struct Diag {
    SourceLocation Loc;
    std::string Msg;
    std::string Hint;
    bool IsWarning;
    std::vector<RelatedLocation> RelatedLocations; // structural context for SLM
    // Ground-truth category, set by the raising TurfError subclass's own
    // constructor (see Errors.h) -- NOT re-derived from message text. Stays
    // GENERAL for diagnostics with no TurfError instance (the raw
    // LogErrorAt() path), which the SLM layer falls back to classifyError()
    // for, same as before this field existed.
    SLM::ErrorCategory Category = SLM::ErrorCategory::GENERAL;
    // Which file Loc's Line/Col are relative to -- defaulted at add()-time
    // from Loc.Filename, falling back to the current SourceFilename if Loc
    // never had one attached (e.g. a hand-built {0,0}-style location). Used
    // to select the right entry out of AllSourceLines at print time, since
    // Line numbers alone are ambiguous across files.
    std::string Filename;
  };

  // Record a diagnostic. Errors on a line that already has an error are
  // dropped. Warnings on a line that has an error are also dropped.
  static void add(SourceLocation Loc, const std::string &Msg,
                  const std::string &Hint, bool IsWarning = false,
                  const std::vector<RelatedLocation> &Related = {},
                  SLM::ErrorCategory Category = SLM::ErrorCategory::GENERAL);

  static void add(SourceLocation Loc, const std::string &Msg,
                  bool IsWarning = false,
                  const std::vector<RelatedLocation> &Related = {});

  // True if any error (not just warning) has been recorded.
  static bool hasErrors();

  // True if an error already exists for the given source line.
  static bool hasErrorAt(int Line);

  // Print all collected diagnostics (sorted by line) then clear them.
  static void flushAll();

  // Discard everything (used between passes if needed).
  static void reset();

  // Restore a saved list of diagnostics without triggering SLM evaluation.
  static void restoreDiagnostics(const std::vector<Diag> &Saved);

  // Public accessors for diagnostics (used by sandbox validation)
  static const std::vector<Diag> &getDiagnostics() { return Diagnostics; }
  static void clearDiagnostics() {
    Diagnostics.clear();
    ErrorLines.clear();
  }
  static size_t getDiagnosticCount() { return Diagnostics.size(); }

private:
  static std::vector<Diag> Diagnostics;
  static std::set<int> ErrorLines; // lines that already have an error
};

enum Token {
  TOK_EOF = -1,
  TOK_NUMBER = -2,
  TOK_IDENTIFIER = -3, // Variable
  TOK_ASSIGN = -4,
  TOK_ELSE = -6,
  TOK_IF = -7,
  TOK_WHILE = -8,
  TOK_EQ = -9,
  TOK_NEQ = -10,
  TOK_LEQ = -11,
  TOK_GEQ = -12,
  TOK_INT_LITERAL = -13,
  TOK_BOOL_LITERAL = -14,
  TOK_TYPE_INT = -15,
  TOK_TYPE_DOUBLE = -16,
  TOK_TYPE_BOOL = -17,
  TOK_STRING_LITERAL = -18,
  TOK_TYPE_STRING = -19,
  TOK_AND = -20,
  TOK_OR = -21,
  TOK_CONV_INT = -22,    // for int(...)
  TOK_CONV_DOUBLE = -23, // for double(...)
  TOK_TYPE_VOID = -24,
  TOK_RETURN = -25,
  TOK_FN = -26,
  TOK_BREAK = -27,
  TOK_CONTINUE = -28,
  TOK_PLUS_ASSIGN = -29,
  TOK_MINUS_ASSIGN = -30,
  TOK_MUL_ASSIGN = -31,
  TOK_DIV_ASSIGN = -32,
  TOK_MOD_ASSIGN = -33,
  TOK_PLUS_PLUS = -34,
  TOK_MINUS_MINUS = -35,
  TOK_FOR = -36,
  TOK_IN = -37,
  TOK_STEP = -38,
  TOK_RANGE = -39,
  TOK_ELSEIF = -40,
  TOK_STRUCT = -42,
  TOK_SELF = -43,
  TOK_SIZEOF = -44,
  TOK_IMPORT = -45,
  TOK_AT = -46,
  TOK_LOOP = -50,
  TOK_NULL = -51,
  TOK_MUTABLE = -65,
  TOK_IMMUTABLE = -66,
  TOK_COLON = -67,
  TOK_TO = -68,
  TOK_GIVE = -69,
  TOK_OTHERWISE = -70,
  TOK_EXTERN = -71,   ///< `extern` keyword — FFI function declaration
  TOK_PTR    = -72,   ///< `ptr`    keyword — opaque pointer type (Phase 3 FFI)
  TOK_ELLIPSIS = -73, ///< `...`   token   — variadic marker in extern fn params
  TOK_SCOPE  = -74,   ///< `::`    token   — scope resolution
  TOK_COMPARE  = -75, ///< `compare`  keyword — switch-like compare statement
  TOK_MATCHES  = -76, ///< `matches`  keyword — arm pattern in compare
  TOK_DONE     = -77, ///< `done`     keyword — terminates a matches arm body
  TOK_POWER    = -78, ///< `**`       token   — exponentiation operator
  TOK_ENUM     = -81, ///< `enum`     token   — enum declaration
  TOK_LSHIFT   = -79, ///< `<<`       token   — bitwise left shift
  TOK_RSHIFT   = -80, ///< `>>`       token   — bitwise right shift
  TOK_EXPORT   = -82, ///< `export`   keyword — top-level exported constant declaration
  TOK_STATIC   = -83, ///< `static`   keyword — no-`self` struct-bound method declaration
  TOK_TYPE_CHAR    = -84, ///< `char`   keyword — 1-byte primitive type (no arithmetic)
  TOK_CHAR_LITERAL = -85, ///< `'x'`    token   — single-quoted char literal
  TOK_OPERATOR     = -86, ///< `operator` keyword — top-level operator overload declaration
  TOK_TYPE_INT32   = -87, ///< `int32`   keyword — honest 32-bit extern-fn ABI declaration type
  TOK_ERROR   = -88, ///< `error`   keyword — user-defined error type declaration (docs/planning/Errors.md)
  TOK_RECOVER = -89, ///< `recover` keyword — handles a `!T` call site's failure
  TOK_PRODUCE = -90, ///< `produce` keyword — `recover produce value` form
  TOK_RUN     = -91, ///< `run`     keyword — `recover run {}` / `recover as x run {}` block form
  TOK_AS      = -92, ///< `as`      keyword — `recover as x run {}` binding form
  TOK_DO      = -93, ///< `do`      keyword — `do { } while (cond)` loop (Issue #180)
  TOK_AND_ASSIGN    = -94, ///< `&=`  token — bitwise-AND compound assignment
  TOK_OR_ASSIGN     = -95, ///< `|=`  token — bitwise-OR compound assignment
  TOK_XOR_ASSIGN    = -96, ///< `^=`  token — bitwise-XOR compound assignment
  TOK_LSHIFT_ASSIGN = -97, ///< `<<=` token — left-shift compound assignment
  TOK_RSHIFT_ASSIGN = -98, ///< `>>=` token — right-shift compound assignment
  TOK_POWER_ASSIGN  = -99, ///< `**=` token — exponentiation compound assignment
  TOK_MACHINE  = -100, ///< `machine` keyword — state-machine declaration (enum + transition graph)
  TOK_ARROW    = -101  ///< `->`      token   — transition edge separator in a `machine` block
};

int gettok();
void resetLexer();
void LogErrorAt(SourceLocation Loc, const std::string &Msg);
int GetLastChar();
void SetLastChar(int c);

/**
 * @brief Render a binary/unary operator token code as a human-readable
 *        string (e.g. for --dump-ast output and diagnostic messages).
 *
 * A single-character operator (`+`, `-`, `<`, `&`, ...) is genuinely a
 * printable ASCII value and can be shown as-is. Every multi-character
 * operator (`==`, `&&`, `**`, `++`, ...), however, tokenizes to a negative
 * int constant (see the enum above) that is NOT a printable character --
 * `static_cast<char>(Op)` on one of those produces garbage (typically the
 * Unicode replacement character when printed). This has repeatedly shown up
 * as a real bug at multiple independent call sites: `--dump-ast` output
 * like `BinaryExpr(op='<63>')` for `p == &n`, and user-facing TypeChecker
 * error messages naming the wrong (garbage) operator for `&&`/`||` and
 * other multi-character operators. Originally lived in ASTPrinter.h (its
 * first-found use site) before being promoted here once TypeChecker.cpp
 * needed the identical fix for several of its own diagnostics -- this is
 * the natural shared home since it only depends on the token constants
 * already defined in this file.
 */
inline std::string OpToDisplayString(int Op) {
  switch (Op) {
    case TOK_EQ: return "==";
    case TOK_NEQ: return "!=";
    case TOK_LEQ: return "<=";
    case TOK_GEQ: return ">=";
    case TOK_AND: return "&&";
    case TOK_OR: return "||";
    case TOK_PLUS_PLUS: return "++";
    case TOK_MINUS_MINUS: return "--";
    case TOK_POWER: return "**";
    case TOK_LSHIFT: return "<<";
    case TOK_RSHIFT: return ">>";
    case TOK_AND_ASSIGN: return "&=";
    case TOK_OR_ASSIGN: return "|=";
    case TOK_XOR_ASSIGN: return "^=";
    case TOK_LSHIFT_ASSIGN: return "<<=";
    case TOK_RSHIFT_ASSIGN: return ">>=";
    case TOK_POWER_ASSIGN: return "**=";
    case TOK_ARROW: return "->";
    default:
      if (Op >= 0 && Op <= 127 && isprint(Op))
        return std::string(1, static_cast<char>(Op));
      return "op#" + std::to_string(Op);
  }
}

#endif
