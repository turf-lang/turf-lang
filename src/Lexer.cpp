/**
 * @file Lexer.cpp
 * @brief Tokenization and lexical analysis for TurfLang.
 * @details Reads the source file character by character and groups them into
 *          meaningful tokens (identifiers, literals, operators, etc.). Also
 *          provides the DiagnosticEngine for error reporting and SLM
 * integration.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "Lexer.h"
#include "Colors.h"
#include "SLMIntegration.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>

std::vector<std::string> SourceLines;
std::map<std::string, std::vector<std::string>> AllSourceLines;
std::string SourceFilename;
// The ORIGINAL top-level file passed to the compiler, set once in main.cpp
// and never touched again (unlike SourceFilename, which is repointed at
// each imported module while it's being parsed, then restored). Used only
// by printDiag() to decide whether to print an "in file: ..." note --
// comparing against SourceFilename wouldn't work reliably since by the time
// flushAll() runs (after every pass has completed) it's already back to
// this same value for every diagnostic, imported-module ones included.
std::string MainSourceFilename;
SourceLocation CurLoc = {1, 0};
int CurLine = 1;
int CurCol = 0;
static int LastChar = ' ';

// DiagnosticEngine : static storage
std::vector<DiagnosticEngine::Diag> DiagnosticEngine::Diagnostics;
std::set<int> DiagnosticEngine::ErrorLines;

// setjmp/longjmp recovery buffers
jmp_buf g_recoverJmp;
bool g_recoverActive = false;
bool g_protoOnlyPass = false; // set by pre-pass 3a to stop after prototype

// Running count of unclosed '{'/'('/'[' minus '}'/')'/']', tracked globally
// by getNextToken() (see Parser.cpp) as it consumes each token -- i.e. the
// current nesting depth relative to the top level (0), regardless of how
// deeply nested inside if/while/compare/etc. the parser currently is.
// Used by main.cpp's SkipToNextTopLevelBoundary() to recover correctly from
// a parse failure at ANY nesting depth, not just one level deep -- see that
// function's doc comment for why a naive "assume depth 1" recovery isn't
// enough. Reset to 0 by resetLexer() so a fresh parse pass (pre-pass vs.
// main pass, or a Sandbox.cpp re-parse) never inherits a stale value.
int g_braceDepth = 0;

// AI/SLM flag: only true when --smart is passed on the CLI.
bool g_AIEnabled = false;

// Tracks if the SLM has generated a diagnostic this pass to suppress
// analysis of subsequent cascading errors.
static bool g_slmFiredThisPass = false;

std::ifstream GlobalSourceFile;
std::ifstream *SourceFile = &GlobalSourceFile;
double NumVal;
long long IntVal;
bool BoolVal;
char CharVal;
std::string IdentifierStr;
std::string StringVal;
int GetLastChar() { return LastChar; }
void SetLastChar(int c) { LastChar = c; }
std::map<std::string, int> Keywords = {{"if", TOK_IF},
                                       {"else", TOK_ELSE},
                                       {"while", TOK_WHILE},
                                       {"do", TOK_DO},
                                       {"int", TOK_TYPE_INT},
                                       {"float", TOK_TYPE_DOUBLE},
                                       {"double", TOK_TYPE_DOUBLE},
                                       {"bool", TOK_TYPE_BOOL},
                                       {"char", TOK_TYPE_CHAR},
                                       {"true", TOK_BOOL_LITERAL},
                                       {"false", TOK_BOOL_LITERAL},
                                       {"string", TOK_TYPE_STRING},
                                       {"void", TOK_TYPE_VOID},
                                       {"return", TOK_RETURN},
                                       {"fn", TOK_FN},
                                       {"break", TOK_BREAK},
                                       {"continue", TOK_CONTINUE},
                                       {"for", TOK_FOR},
                                       {"in", TOK_IN},
                                       {"step", TOK_STEP},
                                       {"elseif", TOK_ELSEIF},
                                       {"struct", TOK_STRUCT},
                                       {"self", TOK_SELF},
                                       {"sizeof", TOK_SIZEOF},
                                       {"import", TOK_IMPORT},
                                       {"enum", TOK_ENUM},

                                       {"null", TOK_NULL},
                                       {"mutable", TOK_MUTABLE},
                                       {"mut", TOK_MUTABLE},
                                       {"immutable", TOK_IMMUTABLE},
                                       {"immut", TOK_IMMUTABLE},
                                       {"to", TOK_TO},
                                       {"give", TOK_GIVE},
                                       {"otherwise", TOK_OTHERWISE},
                                       {"extern", TOK_EXTERN},
                                       {"ptr", TOK_PTR},
                                       {"compare", TOK_COMPARE},
                                       {"matches", TOK_MATCHES},
                                       {"done", TOK_DONE},
                                       {"export", TOK_EXPORT},
                                       {"static", TOK_STATIC},
                                       {"operator", TOK_OPERATOR},
                                       {"int32", TOK_TYPE_INT32},
                                       {"error", TOK_ERROR},
                                       {"recover", TOK_RECOVER},
                                       {"produce", TOK_PRODUCE},
                                       {"run", TOK_RUN},
                                       {"as", TOK_AS}};

/**
 * @brief True if @p Msg looks like a warning rather than a hard error.
 *
 * Used as a fallback classifier for diagnostics that were logged through the
 * untyped LogErrorAt() path (as opposed to a proper TurfError/TurfWarning
 * subclass, which already knows its own severity). Matches on the literal
 * "Warning:"/"warning:" substring that warning-producing call sites embed in
 * their message text.
 */
static bool isMessageWarning(const std::string &Msg) {
  return Msg.find("Warning:") != std::string::npos ||
         Msg.find("warning:") != std::string::npos;
}

// Internal helper: physically print one diagnostic to stderr.
static void printDiag(const DiagnosticEngine::Diag &D) {
  bool isWarning = D.IsWarning || isMessageWarning(D.Msg);

  const std::string &AccentColor =
      isWarning ? Colors::BRIGHT_YELLOW : Colors::BRIGHT_RED;
  const char *TypeLabel = isWarning ? "warning" : "error";

  // error/warning at line X, col Y [in file: ...]
  std::cerr << Colors::BOLD << AccentColor << "\u2500\u2500 " << TypeLabel
            << " " << Colors::RESET << Colors::BRIGHT_BLACK << "at line "
            << D.Loc.Line << ", col " << D.Loc.Col;
  // Only worth calling out when it's NOT the file the user actually
  // compiled -- i.e. the diagnostic is about content inside an imported
  // module, which is otherwise easy to mistake for a problem in your own
  // file since nothing else in the output says otherwise.
  if (!D.Filename.empty() && D.Filename != MainSourceFilename)
    std::cerr << ", in file " << D.Filename;
  std::cerr << Colors::RESET << "\n\n";

  // Message body — embedded ANSI codes from error classes render intact.
  std::cerr << "  " << D.Msg << "\n";

  if (!D.Hint.empty()) {
    std::cerr << "\n  " << Colors::BRIGHT_GREEN << "Hint: " << D.Hint << Colors::RESET << "\n";
  }

  // Code snippet with │ gutter. Looked up from AllSourceLines (a persistent,
  // cross-pass, per-file cache) via D.Filename, NOT the transient global
  // SourceLines -- see AllSourceLines' doc comment in Lexer.h for why: by
  // the time flushAll() runs, SourceLines reflects whatever file was parsed
  // LAST (normally back to the main file), which is the wrong content for
  // any diagnostic whose Loc originated inside an imported module.
  auto FileLinesIt = AllSourceLines.find(D.Filename);
  if (FileLinesIt != AllSourceLines.end() && D.Loc.Line > 0 &&
      D.Loc.Line <= static_cast<int>(FileLinesIt->second.size())) {
    const std::string &LineContent = FileLinesIt->second[D.Loc.Line - 1];
    std::string LineNumStr = std::to_string(D.Loc.Line);
    int CaretOffset = (D.Loc.Col > 0) ? D.Loc.Col - 1 : 0;

    std::cerr << "\n"
              << Colors::BRIGHT_BLACK << "  " << LineNumStr << " \u2502 "
              << Colors::RESET << LineContent << "\n"
              << Colors::BRIGHT_BLACK << "  "
              << std::string(LineNumStr.length(), ' ') << " \u2502 "
              << Colors::RESET << std::string(CaretOffset, ' ') << Colors::BOLD
              << AccentColor << "^ Here!" << Colors::RESET << "\n";
  }

  std::cerr << "\n";
}

/**
 * @brief Record one diagnostic (error or warning) and, for errors only, run
 *        the --smart/--online AI-suggestion pipeline against it.
 *
 * This is the single choke point every diagnostic in the compiler passes
 * through -- TurfError::raise(), TurfWarning::emit(), and the raw
 * LogErrorAt() helper all funnel here.
 *
 * Two independent things happen, in order:
 *
 *  1. Cascade suppression: if a *primary error* (IsWarning == false) has
 *     already been recorded on this exact source line, everything else on
 *     that line -- further errors AND warnings -- is dropped silently. This
 *     keeps one bad statement from spamming five near-duplicate diagnostics.
 *
 *  2. AI augmentation (only reached for actual errors -- see below): when
 *     --smart/--online is enabled (g_AIEnabled), the first error of the
 *     current compilation pass is sent to SLM::validateAndApplySuggestion(),
 *     and a high-confidence, sandbox-validated fix replaces the compiler's
 *     own message. Every error *after* that first one in the same pass just
 *     gets a short "this may be a cascading effect" note appended instead of
 *     a second model call -- see g_slmFiredThisPass.
 *
 * IMPORTANT: step 2 is gated on `!IsWarning`. Warnings (shadowing, redundant
 * comparisons, etc.) must never trigger a model call or carry the cascade
 * note -- the documented --smart/--online contract is "on error only, zero
 * cost on a clean compile," and a warning is not evidence of a preceding
 * error worth explaining away. A prior version of this function omitted that
 * check and ran full SLM inference (and attached a nonsensical "cascading
 * effect of the error above" note) on ordinary warnings.
 *
 * @param Loc      Source location the diagnostic is anchored to.
 * @param Msg      Fully-formatted (ANSI-colored) message body.
 * @param Hint     Optional one-line remediation hint shown under the message.
 * @param IsWarning  false = hard error (blocks compilation); true = warning.
 * @param Related  Structural "declared here" / "candidate: ..." locations,
 *                 threaded through to the SLM prompt as pinned context lines.
 */
void DiagnosticEngine::add(SourceLocation Loc, const std::string &Msg,
                           const std::string &Hint, bool IsWarning,
                           const std::vector<RelatedLocation> &Related,
                           SLM::ErrorCategory Category) {
  // If there is already an error on this line, suppress everything else
  // (both additional errors and warnings) to avoid cascading noise.
  if (ErrorLines.count(Loc.Line))
    return;

  std::string finalMessage = Msg;

  // Conditionally invoke SLM analysis based on CLI configuration.
  // Errors only -- see the "IMPORTANT" note in the function doc comment
  // above for why warnings must never reach this block.
  if (g_AIEnabled && !IsWarning) {
    if (g_slmFiredThisPass) {
      // Append advisory AI note for cascading errors.
      finalMessage +=
          "\n\n  " + Colors::BRIGHT_BLACK +
          "[Smart Assist]  This error may be a cascading effect of the one "
          "reported above. Fix the first issue and run --smart again for a "
          "fresh analysis." +
          Colors::RESET;
    } else {
      SLM::AISuggestion ai = SLM::validateAndApplySuggestion(Msg, Loc.Line, Hint, Related, Category);

      if (ai.valid && ai.confidence >= 0.8f && ai.fix_mode != "NONE") {
        // Replace compiler message with high-confidence AI suggestion.
        g_slmFiredThisPass = true;
        finalMessage = Colors::BOLD + Colors::BRIGHT_CYAN + "[AI Suggestion]" +
                       Colors::RESET + "  " + ai.suggested_comment;

        if (!ai.explanation.empty()) {
          finalMessage += "\n  " + Colors::CYAN +
                          "Explanation: " + Colors::RESET + ai.explanation;
        }

        if (ai.fix_mode == "MODIFY" && !ai.code_patch.empty()) {
          finalMessage += "\n  " + Colors::BRIGHT_GREEN +
                          "Suggested fix:" + Colors::RESET + "\n    " +
                          Colors::BRIGHT_GREEN + ai.code_patch + Colors::RESET;
        }

        if (!ai.warning.empty()) {
          finalMessage +=
              "\n  " + Colors::YELLOW + "Note: " + Colors::RESET + ai.warning;
        }
      }
    }
  }

  // Which file this diagnostic belongs to: Loc's own Filename if it has one
  // (the normal case -- captured from CurLoc at parse time, see gettok()),
  // else fall back to whatever file is currently active. AllSourceLines is a
  // PERSISTENT cache (never overwritten/restored like SourceLines), so this
  // resolves correctly even for diagnostics raised by a pass that runs after
  // every file has already finished parsing (Resolver/TypeChecker/Codegen).
  std::string Filename = Loc.Filename.empty() ? SourceFilename : Loc.Filename;

  Diagnostics.push_back({Loc, finalMessage, Hint, IsWarning, Related, Category,
                         Filename});

  if (!IsWarning)
    ErrorLines.insert(Loc.Line);
}

void DiagnosticEngine::add(SourceLocation Loc, const std::string &Msg,
                           bool IsWarning,
                           const std::vector<RelatedLocation> &Related) {
  add(Loc, Msg, "", IsWarning, Related);
}

bool DiagnosticEngine::hasErrors() { return !ErrorLines.empty(); }

bool DiagnosticEngine::hasErrorAt(int Line) {
  return ErrorLines.count(Line) != 0;
}

void DiagnosticEngine::flushAll() {
  // Sort by line, then column
  std::sort(Diagnostics.begin(), Diagnostics.end(),
            [](const Diag &A, const Diag &B) {
              return A.Loc.Line < B.Loc.Line ||
                     (A.Loc.Line == B.Loc.Line && A.Loc.Col < B.Loc.Col);
            });

  for (const auto &D : Diagnostics)
    printDiag(D);

  Diagnostics.clear();
}

void DiagnosticEngine::reset() {
  Diagnostics.clear();
  ErrorLines.clear();
  g_slmFiredThisPass =
      false; // Reset cascade guard for the next compilation pass
}

void DiagnosticEngine::restoreDiagnostics(const std::vector<Diag> &Saved) {
  Diagnostics = Saved;
  ErrorLines.clear();
  for (const auto &D : Diagnostics) {
    if (!D.IsWarning) {
      ErrorLines.insert(D.Loc.Line);
    }
  }
}

// Emits and collects a generic diagnostic message at the given source location.
void LogErrorAt(SourceLocation Loc, const std::string &Msg) {
  DiagnosticEngine::add(Loc, Msg, isMessageWarning(Msg));
}

void resetLexer() {
  LastChar = ' ';
  CurLine = 1;
  CurCol = 0;
  CurLoc = {1, 0};
  g_braceDepth = 0;
}

/**
 * @brief Retrieves the next token from the standard input or source file.
 *
 * Scans characters sequentially, skipping whitespace, identifying keywords,
 * operators, numeric literals, and strings. Updates CurLoc for diagnostics.
 *
 * @return int The token type identifier (e.g., TOK_IDENTIFIER, TOK_NUMBER, or
 * ASCII value).
 */
int gettok() {
  while (isspace(LastChar)) {
    // Handle newlines to track line numbers
    if (LastChar == '\n') {
      CurLine++;
      CurCol = 0;
    }

    LastChar = SourceFile->get();

    // Increment column for every character read
    if (LastChar != EOF) {
      CurCol++;
    }
  }

  // Snapshot the location before parsing the token body. Stamping Filename
  // here (rather than leaving SourceLocation's Filename field to default) is
  // what lets every AST node's Loc -- captured from CurLoc at parse time --
  // correctly remember which file it came from, even after later compiler
  // passes (Resolver/TypeChecker/Codegen) have moved on to a different file
  // and SourceFilename itself has changed. See AllSourceLines' doc comment.
  CurLoc = {CurLine, CurCol, SourceFilename};

  // Identifiers of the types: id = [a-zA-Z][a-zA-Z0-9_]*
  if (isalpha(LastChar)) {
    IdentifierStr = LastChar;
    LastChar = SourceFile->get();
    CurCol++;

    while (isalnum(LastChar) || LastChar == '_') {
      IdentifierStr += LastChar;
      LastChar = SourceFile->get();
      CurCol++;
    }

    // Check for keywords
    auto It = Keywords.find(IdentifierStr);
    if (It != Keywords.end()) {
      if (It->second == TOK_BOOL_LITERAL)
        BoolVal = (IdentifierStr == "true");
      return It->second;
    }

    return TOK_IDENTIFIER;
  }

  // String literals: "..."
  //
  // A Turf string literal must open and close on the same physical source
  // line -- a raw newline byte inside the quotes is always a mistake (a
  // forgotten closing '"'), never a way to write a multi-line string; that
  // requires the \n escape instead. The scan loop below stops at '"', EOF,
  // OR a raw '\n' for exactly that reason: without the '\n' stop condition,
  // an unclosed string would silently swallow the rest of the file --
  // including real code on later lines -- as string contents, and because
  // CurLine is only ever incremented by the whitespace-skipping logic at the
  // very top of gettok() (which this inner loop bypasses), every diagnostic
  // for the remainder of the file would then report the wrong line number.
  if (LastChar == '"') {
    StringVal = "";
    LastChar = SourceFile->get();
    CurCol++;

    while (LastChar != '"' && LastChar != EOF && LastChar != '\n') {
      if (LastChar == '\\') {
        // Handle escape sequences. Snapshot the backslash's own location
        // before consuming the escape character, so an "unknown escape"
        // diagnostic points at the actual '\' rather than wherever CurLoc
        // (the opening-quote location, unchanged since) happens to be.
        SourceLocation EscapeLoc{CurLine, CurCol, ""};
        LastChar = SourceFile->get();
        CurCol++;
        if (LastChar == 'n') {
          StringVal += '\n';
        } else if (LastChar == 't') {
          StringVal += '\t';
        } else if (LastChar == 'r') {
          StringVal += '\r';
        } else if (LastChar == '0') {
          StringVal += '\0';
        } else if (LastChar == 'a') {
          StringVal += '\a';
        } else if (LastChar == 'b') {
          StringVal += '\b';
        } else if (LastChar == 'f') {
          StringVal += '\f';
        } else if (LastChar == 'v') {
          StringVal += '\v';
        } else if (LastChar == '\\') {
          StringVal += '\\';
        } else if (LastChar == '"') {
          StringVal += '"';
        } else if (LastChar == '\'') {
          StringVal += '\'';
        } else {
          // Anything else after a '\' used to be silently accepted as if
          // the backslash weren't there at all -- e.g. "a\rb" used to
          // produce the 3-character string a/r/b (a literal 'r', not a
          // carriage return), because \r wasn't in the recognized set.
          // That's exactly the kind of mistake that should be caught at
          // compile time, not silently miscompiled: an unrecognized
          // escape is now a clean error instead of a quiet pass-through.
          LogErrorAt(EscapeLoc,
                     std::string("Unknown escape sequence '\\") +
                         static_cast<char>(LastChar) +
                         "' in string literal.\n  Hint: Recognized escapes "
                         "are \\n \\t \\r \\0 \\a \\b \\f \\v \\\\ \\\" \\'. "
                         "Did you mean to write a literal backslash? Use "
                         "'\\\\' instead.");
          StringVal += LastChar;
        }
      } else {
        StringVal += LastChar;
      }
      LastChar = SourceFile->get();
      CurCol++;
    }

    // Both the EOF and raw-newline paths below report the same underlying
    // mistake ("you never closed this string") with the same remediation, so
    // the hint text -- every diagnostic in Turf carries one; it's the main
    // thing that makes an error actually actionable rather than just a
    // location -- is written once here instead of duplicated per call site.
    // Uses the "\n  Hint: ..." convention that every other raw LogErrorAt()
    // call in the parser/lexer follows (TurfError subclasses in Errors.h use
    // a structured Hint_ field instead; LogErrorAt has no such field, so the
    // hint is appended directly into the message text).
    static const char *const UnterminatedStringHint =
        "\n  Hint: Add a closing '\"' before the end of the line -- Turf "
        "string literals can't span multiple lines. Need an actual line "
        "break inside the string? Use the \\n escape instead.";

    if (LastChar == EOF) {
      // Ran off the end of the file with no closing '"' anywhere -- there is
      // truly nothing left to lex, so end the token stream here exactly as
      // the pre-existing behavior did.
      LogErrorAt(CurLoc, std::string("Unterminated string literal") +
                             UnterminatedStringHint);
      return TOK_EOF;
    }

    if (LastChar == '\n') {
      // Hit end-of-line before a closing '"'. Report it at the opening-quote
      // location (CurLoc was snapshotted there at the top of gettok() and is
      // untouched since) and return the partial token WITHOUT consuming the
      // newline. Leaving '\n' in LastChar means the *next* gettok() call
      // walks into it via the ordinary isspace()/whitespace-skip path at the
      // top of this function, which correctly bumps CurLine and resets
      // CurCol -- so lexing (and line tracking) of the rest of the file
      // recovers cleanly instead of desyncing for every line that follows.
      LogErrorAt(CurLoc, std::string("Unterminated string literal") +
                             UnterminatedStringHint);
      return TOK_STRING_LITERAL;
    }

    // Consume closing "
    LastChar = SourceFile->get();
    CurCol++;

    return TOK_STRING_LITERAL;
  }

  // Char literals: 'x'
  //
  // A char is a single byte (RFC STRING_UFCS_OPERATOR_CHAR_RFC §3), not a
  // Unicode codepoint -- exactly one character, or one escape sequence, may
  // appear between the quotes. Reuses the identical escape-sequence set
  // recognized inside double-quoted strings above (\n \t \r \0 \a \b \f \v
  // \\ \" \'), so the same source text means the same byte in both string
  // and char literals.
  if (LastChar == '\'') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '\'') {
      // Empty literal '' -- consume the closing quote so the lexer doesn't
      // desync, report, and recover with a placeholder value.
      LogErrorAt(CurLoc,
                 "Empty char literal.\n  Hint: A char literal must contain "
                 "exactly one character, e.g. 'a'.");
      LastChar = SourceFile->get();
      CurCol++;
      CharVal = '\0';
      return TOK_CHAR_LITERAL;
    }

    if (LastChar == EOF || LastChar == '\n') {
      // Same rationale as the unterminated-string case above: don't consume
      // the newline, so the next gettok() call's whitespace-skip recovers
      // line tracking cleanly instead of desyncing for the rest of the file.
      LogErrorAt(CurLoc,
                 std::string("Unterminated char literal") +
                     "\n  Hint: Add a closing ''' after the character.");
      return LastChar == EOF ? TOK_EOF : TOK_CHAR_LITERAL;
    }

    if (LastChar == '\\') {
      SourceLocation EscapeLoc{CurLine, CurCol, ""};
      LastChar = SourceFile->get();
      CurCol++;
      if (LastChar == 'n') {
        CharVal = '\n';
      } else if (LastChar == 't') {
        CharVal = '\t';
      } else if (LastChar == 'r') {
        CharVal = '\r';
      } else if (LastChar == '0') {
        CharVal = '\0';
      } else if (LastChar == 'a') {
        CharVal = '\a';
      } else if (LastChar == 'b') {
        CharVal = '\b';
      } else if (LastChar == 'f') {
        CharVal = '\f';
      } else if (LastChar == 'v') {
        CharVal = '\v';
      } else if (LastChar == '\\') {
        CharVal = '\\';
      } else if (LastChar == '"') {
        CharVal = '"';
      } else if (LastChar == '\'') {
        CharVal = '\'';
      } else {
        LogErrorAt(EscapeLoc,
                   std::string("Unknown escape sequence '\\") +
                       static_cast<char>(LastChar) +
                       "' in char literal.\n  Hint: Recognized escapes are "
                       "\\n \\t \\r \\0 \\a \\b \\f \\v \\\\ \\\" \\'.");
        CharVal = static_cast<char>(LastChar);
      }
    } else {
      CharVal = static_cast<char>(LastChar);
    }

    // Consume the character (or the last byte of the escape) just recorded.
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar != '\'') {
      // More than one character before the closing quote, e.g. 'ab'. Skip
      // forward to the next ''' (or EOF/newline) so the rest of the file
      // keeps lexing from a sane point instead of every later token being
      // offset by the stray characters.
      LogErrorAt(CurLoc,
                 "Char literal must contain exactly one character.\n  Hint: "
                 "Use a string literal (\"ab\") for multiple characters, or "
                 "check for a missing closing '.");
      while (LastChar != '\'' && LastChar != EOF && LastChar != '\n') {
        LastChar = SourceFile->get();
        CurCol++;
      }
      if (LastChar == '\'') {
        LastChar = SourceFile->get();
        CurCol++;
      }
      return TOK_CHAR_LITERAL;
    }

    // Consume closing '
    LastChar = SourceFile->get();
    CurCol++;

    return TOK_CHAR_LITERAL;
  }

  // Ellipsis '...' (variadic marker in extern fn) must be checked before '..'
  // (range operator) so three dots aren't consumed as range + stray dot.
  if (LastChar == '.' && SourceFile->peek() == '.') {
    // Peek at the character after the second dot
    SourceFile->get(); CurCol++;
    int third  = SourceFile->peek();
    if (third == '.') {
      SourceFile->get(); CurCol++; // consume third dot
      LastChar = SourceFile->get(); CurCol++;
      return TOK_ELLIPSIS;
    }
    // Only two dots — range operator
    LastChar = SourceFile->get(); CurCol++;
    return TOK_RANGE;
  }

  // Numbers: [0-9.]+ (a leading '.' only starts a number if followed by a
  // digit)
  if (isdigit(LastChar) || (LastChar == '.' && isdigit(SourceFile->peek()))) {
    std::string NumStr;
    bool SeenDot = false;
    do {
      if (LastChar == '.') SeenDot = true;
      NumStr += LastChar;
      LastChar = SourceFile->get();
      CurCol++;
    } while (isdigit(LastChar) ||
             (!SeenDot && LastChar == '.' && SourceFile->peek() != '.'));

    bool IsFloat = NumStr.find('.') != std::string::npos;
    if (IsFloat) {
      NumVal = strtod(NumStr.c_str(), 0);
      return TOK_NUMBER;
    }

    IntVal = strtoll(NumStr.c_str(), nullptr, 10);
    return TOK_INT_LITERAL;
  }

  if (LastChar == '=') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_EQ;
    }

    return TOK_ASSIGN;
  }

  if (LastChar == ':') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == ':') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_SCOPE;
    }

    return TOK_COLON;
  }

  if (LastChar == '!') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_NEQ;
    }

    return '!';
  }

  if (LastChar == '>') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_GEQ;
    }

    if (LastChar == '>') {
      LastChar = SourceFile->get();
      CurCol++;
      if (LastChar == '=') {
        LastChar = SourceFile->get();
        CurCol++;
        return TOK_RSHIFT_ASSIGN;
      }
      return TOK_RSHIFT;
    }

    return '>';
  }

  if (LastChar == '<') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_LEQ;
    }

    if (LastChar == '<') {
      LastChar = SourceFile->get();
      CurCol++;
      if (LastChar == '=') {
        LastChar = SourceFile->get();
        CurCol++;
        return TOK_LSHIFT_ASSIGN;
      }
      return TOK_LSHIFT;
    }

    return '<';
  }

  if (LastChar == '&') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '&') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_AND;
    }

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_AND_ASSIGN;
    }

    return '&';
  }

  if (LastChar == '|') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '|') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_OR;
    }

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_OR_ASSIGN;
    }

    return '|';
  }

  if (LastChar == '^') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_XOR_ASSIGN;
    }

    return '^';
  }

  if (LastChar == '+') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '+') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_PLUS_PLUS;
    }

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_PLUS_ASSIGN;
    }

    return '+';
  }

  if (LastChar == '-') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '-') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_MINUS_MINUS;
    }

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_MINUS_ASSIGN;
    }

    return '-';
  }

  if (LastChar == '*') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_MUL_ASSIGN;
    }

    if (LastChar == '*') {
      LastChar = SourceFile->get();
      CurCol++;
      if (LastChar == '=') {
        LastChar = SourceFile->get();
        CurCol++;
        return TOK_POWER_ASSIGN;
      }
      return TOK_POWER;
    }

    return '*';
  }

  if (LastChar == '%') {
    LastChar = SourceFile->get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_MOD_ASSIGN;
    }

    return '%';
  }

  if (LastChar == '/') {
    if (SourceFile->peek() == '/') {
      do {
        LastChar = SourceFile->get();
        CurCol++;
      } while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

      if (LastChar != EOF)
        return gettok();
    } else if (SourceFile->peek() == '=') {
      LastChar = SourceFile->get();
      CurCol++;
      LastChar = SourceFile->get();
      CurCol++;
      return TOK_DIV_ASSIGN;
    }
  }

  if (LastChar == '@') {
    LastChar = SourceFile->get();
    CurCol++;
    return TOK_AT;
  }

  if (LastChar == EOF)
    return TOK_EOF;

  // Handle ASCII characters
  int ThisChar = LastChar;
  LastChar = SourceFile->get();
  CurCol++;

  return ThisChar;
}

