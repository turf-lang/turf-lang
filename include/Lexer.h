#ifndef LEXER_H
#define LEXER_H

#include "ErrorCategory.h" // SLM::ErrorCategory -- lightweight, no LLVM/AST deps
#include "ErrorCodes.h"    // ErrCode:: -- LogErrorAt() requires one (issue #263)
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
// EndLine/EndCol added after Filename (not between Line/Col and Filename)
// for the exact same reason Filename itself defaults rather than being
// mandatory: several hundred call sites construct a SourceLocation via
// positional aggregate-init, including a real 3-arg `{Line, Col, Filename}`
// form (gettok()'s own CurLoc assignment) -- appending new fields at the
// end, each with its own default member initializer, keeps every existing
// 2-arg and 3-arg call site compiling unchanged; they simply don't get an
// end position (EndLine/EndCol default to -1, meaning "not computed" --
// NOT "same as Line/Col", so a caller can tell the difference between "this
// is a single-column token" and "no end position was ever recorded here").
//
// Populated for every single-token span -- identifiers/keywords, operators,
// and string/char/number literals -- via gettok()'s wrapper around
// gettokScan() (Lexer.cpp), which stamps EndLine/EndCol uniformly right
// after each token finishes scanning (originally only identifiers stamped
// this manually, added for #230's name-token AST locations; broadened to
// every token kind for #241 Track D's "underline the full token" DiagnosticEngine
// output). Still NOT populated for genuine multi-token spans (a whole
// declaration, a qualified `A::B::C` path, an entire failing sub-expression
// like `add(3, "4")`) -- that requires each AST node to track its own
// end location built from its last consumed token, not just a lexer-level
// change, and remains open scope (see issue #241 Track D / #243).
struct SourceLocation {
  int Line;
  int Col;
  std::string Filename;
  int EndLine = -1;
  int EndCol = -1;
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

// Generalized to std::istream* (was std::ifstream*) so a buffer-based
// embedder (an LSP's unsaved editor buffer, a REPL line, a fuzzer corpus
// entry) can point this at an in-memory std::istringstream via
// SetSourceBuffer() instead of round-tripping through a real temp file on
// disk -- see issue #232. gettok() (Lexer.cpp) only ever calls ->get()/
// ->peek(), both plain std::istream members, so nothing about token
// scanning itself changes; only code that opens/closes the underlying file
// needs to go through the helpers below instead of calling
// ->open()/->close()/->is_open() directly (istream has no such members).
extern std::istream *SourceFile;

// Opens Path as the real on-disk source file and points SourceFile at it.
// Returns false (mirrors std::ifstream::is_open()) if Path can't be opened.
// The disk-file counterpart to SetSourceBuffer() below.
bool OpenSourceFile(const std::string &Path);

// Closes the on-disk file SourceFile currently points at. Safe to call even
// when SourceFile points at a caller-owned buffer (SetSourceBuffer()) --
// a no-op in that case, since there's nothing of ours to close.
void CloseSourceFile();

// Points SourceFile at a caller-owned in-memory stream (typically a
// std::istringstream wrapping an unsaved editor buffer) instead of a real
// on-disk file -- see issue #232. The caller owns Buffer's lifetime and
// must keep it alive for as long as SourceFile points at it; call
// OpenSourceFile() or SetSourceBuffer() again (or let the process exit) to
// stop using it. Import resolution still needs a real filesystem path to
// search relative to -- set SourceFilename separately to whatever path the
// buffer conceptually represents (its unsaved editor path, say), same as a
// real file would.
void SetSourceBuffer(std::istream &Buffer);
// SEM-001 (docs/planning/ARCHITECTURE_OVERHAUL.md §8): the lexer used to
// collapse every integer literal into a `long long` (via strtoll, silently
// saturating on overflow) and every float literal into a `double` (via
// strtod, silently rounding to `inf` on overflow) -- destroying the
// literal's exact written value before the parser ever saw it. These three
// globals replace `NumVal`/`IntVal` and carry the RAW, exact information
// instead, deliberately kept LLVM-free (plain string/int, no APSInt/APFloat)
// so Lexer.h itself stays free of an LLVM/AST dependency, same as it always
// has been -- the caller (ParseNumberExpr, ParserLiteral.cpp, which already
// needs LLVM for other reasons) is what actually builds the exact
// llvm::APSInt/llvm::APFloat from these.
//   NumLitSpelling -- the literal's exact original source text, including
//     its radix prefix and every '_' separator, verbatim. For diagnostics
//     and NumberExprAST::getSpelling() (round-trip proof).
//   NumLitClean -- the same literal with the radix prefix and every '_'
//     stripped: just the digits (int) or the digits/'.'/exponent (float),
//     ready to hand directly to llvm::APInt's or llvm::APFloat's own
//     string-parsing constructor with no further cleanup.
//   NumLitRadix -- 10, 16, 2, or 8. Meaningful only when the token just
//     returned was TOK_INT_LITERAL; unspecified (not reset) otherwise.
extern std::string NumLitSpelling;
extern std::string NumLitClean;
extern int NumLitRadix;
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

// LEC-005: the '{'/'}' share of g_braceDepth alone. A parse error inside a
// function body leaves the body's own `{` open; resynchronising on that
// count (not on the all-bracket one, which an unclosed `(` makes useless --
// issue #245) is what lets recovery skip to the function's closing brace
// instead of stopping at the next `;` inside the body and parsing the rest of
// the body as top-level statements. Maintained by getNextToken() and
// saved/restored everywhere g_braceDepth is.
extern int g_curlyDepth;

// The recursive-descent parser has no built-in recursion-depth limit --
// ParseExpression()/ParseUnary()/ParsePrimary()/ParseBlock()/ParseType()
// etc. are ordinary unbounded C++ recursion. Deeply nested malformed input
// (a long run of unmatched '(' or '{', exactly what a half-typed
// expression in an LSP's live edit buffer produces constantly while the
// user is still typing) risks a genuine C++ stack overflow -- a hard
// process crash, not something a diagnostic or setjmp/longjmp recovery can
// catch. Guarded at four entry points: ParseExpression() and ParseBlock()
// (the two top-level recursive-descent entry points reached from
// statement/declaration parsing), plus ParseUnary() and ParseType()
// (each recurses directly into ITSELF for chained prefix operators
// (`!!!!x`) and nested generic/error-union types (`list<list<...>>`,
// `!!T`) respectively, WITHOUT going back through ParseExpression() in
// between -- confirmed via code review to be completely unbounded even
// after the ParseExpression()/ParseBlock() guards existed, since neither
// of those is re-entered by this kind of direct self-recursion). Every
// OTHER recursive parse function either goes through one of these four on
// its way to recursing, or is bounded by the token stream itself (e.g. a
// parameter list can only be as long as the tokens the user actually
// typed) rather than by unbounded self-recursion.
constexpr int MaxParseRecursionDepth = 2000;
extern int g_parseRecursionDepth;

// RAII guard: increments on construction, decrements on destruction, so
// every one of ParseExpression()/ParseBlock()'s many early-return paths is
// covered automatically with no manual bookkeeping at each one. Construct
// one as the very first statement in the guarded function and check
// tooDeep() before doing any real work.
struct ParseRecursionGuard {
  ParseRecursionGuard() { ++g_parseRecursionDepth; }
  ~ParseRecursionGuard() { --g_parseRecursionDepth; }
  bool tooDeep() const { return g_parseRecursionDepth > MaxParseRecursionDepth; }
};

// When true, the SLM is loaded and AI suggestions are generated for errors.
// Controlled by the --smart CLI flag. Default is off for deterministic output.
extern bool g_AIEnabled;
// SLM-001 follow-up: when non-empty, the FULL patched source of the first
// --smart suggestion that clears the display gate (confidence >= 0.8) this
// compile is written here verbatim, instead of only ever being shown as a
// rendered diagnostic. Controlled by --smart-write-fix=<path>; empty (the
// default) means "don't write anything" -- an ordinary --smart run's
// behavior is completely unchanged. Exists so a caller (a benchmark tool, a
// future editor integration) can get the corrected PROGRAM as a real,
// compilable file, not just a description of the fix -- see
// scripts/slm_benchmark/run_benchmark.py, which uses this to actually
// recompile and RUN a fixed program and check whether it still does what
// the pre-mutation original did (§16.4's "semantic-correctness rate").
extern std::string g_SmartFixOutPath;
// LANG-002: when true, a file OUTSIDE the resolved install root may still
// declare the raw-pointer capability (`unsafe module;`, HasRawPointerCapability()
// in ParserModule.cpp) -- controlled by the --allow-unsafe-modules CLI flag.
// A file UNDER the install root never needs this flag: see
// HasRawPointerCapability()'s own doc comment for the full rule. Default off,
// same reasoning as g_AIEnabled -- a capability that broadens what raw
// pointers can do must be an explicit, visible opt-in, never ambient.
extern bool g_AllowUnsafeModules;
// OVH-012: when false (the default), a warning whose diagnostic location
// falls inside the install root's `lib/std` is a "system-module warning" --
// recorded (still counts toward getDiagnosticCount(), still visible to
// CollectAllDiagnostics-based tooling like the sandbox) but never printed by
// flushAll(), matching `-isystem` practice: a normal user program that
// merely `import`s a stdlib module (`import io; import json; import http;`)
// must never see a warning whose primary location is code it didn't write
// and can't fix. Controlled by the --stdlib-warnings CLI flag (main.cpp).
// Errors are never suppressed by this -- only DiagnosticEngine::add()'s
// IsWarning branch consults it (Lexer.cpp).
extern bool g_ShowStdlibWarnings;
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
    // constructor (see Errors.h), or by the explicit argument a raw
    // LogErrorAt() call site passes (see this file's own LogErrorAt()
    // declaration) -- NOT re-derived from message text (DIAG-001 deleted
    // that fallback, formerly SLM::classifyError()). GENERAL is a real,
    // audited answer for a site, not "unclassified."
    SLM::ErrorCategory Category = SLM::ErrorCategory::GENERAL;
    // Which file Loc's Line/Col are relative to -- defaulted at add()-time
    // from Loc.Filename, falling back to the current SourceFilename if Loc
    // never had one attached (e.g. a hand-built {0,0}-style location). Used
    // to select the right entry out of AllSourceLines at print time, since
    // Line numbers alone are ambiguous across files.
    std::string Filename;
    // Stable "E0NN" code (see ErrorCodes.h). A TurfError subclass supplies it
    // (TurfError::raise() passes getCode(): the class's code, or the
    // instance's withCode() override); the raw LogErrorAt() path takes a
    // REQUIRED ErrCode:: argument. Empty only for a warning -- a TurfWarning
    // subclass that has no registry row prints without a "[E0NN]" tag in
    // printDiag() -- so every ERROR carries one (issue #263).
    std::string Code;
    // SLM-001: structured hints (deterministic-fix candidate, known types)
    // from the raising TurfError, if any -- see DiagnosticHints's doc
    // comment (SLMIntegration.h). Always empty for a warning (the SLM path
    // is gated on !IsWarning, same as Category) and for the raw LogErrorAt()
    // path (which has no TurfError instance to carry one).
    SLM::DiagnosticHints Hints;
  };

  // Record a diagnostic. Errors on a line that already has an error are
  // dropped. Warnings on a line that has an error are also dropped.
  static void add(SourceLocation Loc, const std::string &Msg,
                  const std::string &Hint, bool IsWarning = false,
                  const std::vector<RelatedLocation> &Related = {},
                  SLM::ErrorCategory Category = SLM::ErrorCategory::GENERAL,
                  const std::string &Code = "",
                  const SLM::DiagnosticHints &Hints = {});

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

  // Off by default: add() caps at one primary error per source line (see
  // its own doc comment) -- deliberate CLI noise reduction, unchanged for
  // existing callers. A tooling embedder wanting every real diagnostic for
  // a buffer (e.g. an LSP populating squiggly lines needs to know about a
  // SECOND genuine error on one physical line, not just the first) should
  // set this true before its parse/resolve/typecheck pass and restore it
  // afterward -- same save/restore convention g_AIEnabled already uses at
  // Sandbox.cpp's own pass boundaries. Purely additive: does not change
  // hasErrors()/hasErrorAt() semantics (ErrorLines is still populated the
  // same way either way), only whether add() suppresses a diagnostic on an
  // already-errored line.
  static void setCollectAllDiagnostics(bool V) { CollectAllDiagnostics = V; }
  static bool getCollectAllDiagnostics() { return CollectAllDiagnostics; }

  // Everything this class keeps between calls, for the query facade's
  // isolated compiles (src/AnalysisFacade.cpp). Not restoreDiagnostics():
  // that rebuilds ErrorLines from the list, but flushAll() empties the list
  // and keeps ErrorLines, so after a flush the two disagree and only a
  // verbatim copy puts hasErrors() back the way it was.
  struct State {
    std::vector<Diag> Diagnostics;
    std::set<int> ErrorLines;
    bool CollectAllDiagnostics = false;
    bool SLMFiredThisPass = false;
  };
  static State saveState();
  static void restoreState(State S);

private:
  static std::vector<Diag> Diagnostics;
  static std::set<int> ErrorLines; // lines that already have an error
  static bool CollectAllDiagnostics;
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
  TOK_ERROR   = -88, ///< `error`   keyword — user-defined error type
                     ///< declaration (docs/architecture/errors_and_recover.md)
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
  TOK_ARROW    = -101, ///< `->`      token   — transition edge separator in a `machine` block
  TOK_UNSAFE   = -102, ///< `unsafe`  keyword — lexical gate for raw pointer types (issue #238)
  TOK_GUESS    = -103, ///< `guess`   keyword — type-position placeholder for initializer type
                        ///< inference (SEM-003, ARCHITECTURE_OVERHAUL.md §11). Omitting a
                        ///< declaration's `: Type` annotation entirely is sugar for `: guess`;
                        ///< see ParseLetStatement().
  TOK_DEFER    = -104, ///< `defer`   keyword — registers a statement to run when the enclosing
                        ///< `{ }` block exits, by any path except a panic (LANG-003,
                        ///< ARCHITECTURE_OVERHAUL.md §8). See ASTStmt.h's DeferExprAST.
  TOK_PUB = -106,  ///< `pub`/`public` keyword (synonyms, one token, exactly like `mut`/`mutable`
                   ///< share TOK_MUTABLE) -- a declaration's visibility prefix (LEC-002,
                   ///< FREEZE.md §6.15). Reserved: no identifier may be spelled either way.
  TOK_WILDCARD = -107, ///< `_` ALONE -- the wildcard pattern (LEC-003, FREEZE.md §6.17). A token
                       ///< of its own, not an identifier named "_", so it is reserved everywhere
                       ///< a name is expected without any per-site check; `_name` (an underscore
                       ///< FOLLOWED by anything) is an ordinary TOK_IDENTIFIER.
  TOK_TRY = -108, ///< `try` keyword -- prefix error propagation, `try f()` (LEC-004,
                  ///< FREEZE.md §6.18 / F-ERR-1). Reserved: no identifier may be spelled `try`.
  TOK_CALLER_LOCATION = -105
                        ///< `caller_location` keyword — a bare, non-callable expression of type
                        ///< `string`, legal only inside a function declared `@caller_location`
                        ///< (STD-002, ARCHITECTURE_OVERHAUL.md §8/§15.6(a)). Reserved as a real
                        ///< keyword (like `self`, TOK_SELF above) rather than an ordinary
                        ///< identifier specifically so a user can never declare a variable or
                        ///< parameter with this exact name -- see CallerLocationExprAST
                        ///< (ASTExpr.h) and ParserDecl.cpp's ParseFuncDef() for how the implicit
                        ///< hidden parameter this keyword reads is threaded through Codegen only
                        ///< (never through Params_ / the ordinary arity-checked signature).
};

int gettok();
void resetLexer();
// `Category` is the ground-truth SLM::ErrorCategory for this diagnostic,
// same role as TurfError::Category_ (Errors.h) plays for a typed error --
// LogErrorAt() is the untyped path (no TurfError subclass instance exists to
// read one from), used throughout the Parser/Lexer/Codegen for messages that
// don't warrant their own class. Every call site names its own category
// explicitly (DIAG-001 audit); GENERAL is a deliberate choice for a site,
// never an unaudited default. See ErrorCategory.h's removed classifyError()
// -- category is no longer re-derived from message text.
//
// `Code` is REQUIRED (issue #263): one of the ErrCode:: constants
// (ErrorCodes.h), which must be a row of AllErrorCodes. Before this, this path
// was the one way to print an error with no `[E0NN]` and so no
// `turf --explain` page -- 181 call sites, every parse error a beginner can
// hit. A typed TurfError gets its code from its C++ class; this untyped path
// has no class, so the call site names the KIND of mistake instead. Making the
// parameter mandatory (and not defaulted) is the point: a new raw diagnostic
// that forgets its code is a compile error, not a silent dead end. Warnings
// never come through here (they are TurfWarning, or a "Warning:" message that
// isMessageWarning() recognises and which carries no code by design).
void LogErrorAt(SourceLocation Loc, const char *Code, const std::string &Msg,
                SLM::ErrorCategory Category);
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
