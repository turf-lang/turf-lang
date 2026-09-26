#ifndef ERRORS_LEX_H
#define ERRORS_LEX_H

#include "Errors.h"

// Below SyntaxError/KeywordError (raised from the PARSER, despite this
// file's name) are four classes raised directly from inside the LEXER's own
// number-scanning branch (gettokScan(), Lexer.cpp) -- SEM-001
// (docs/planning/ARCHITECTURE_OVERHAUL.md §8). Lexer.cpp does not normally
// construct a coded TurfError (its own diagnostics go through the raw,
// code-less LogErrorAt(), e.g. the malformed-char-literal check) precisely
// because Lexer.h stays deliberately LLVM/AST-free and TurfError subclasses
// used to live only in headers reachable from the Parser/Resolver/
// TypeChecker layers -- but SEM-001 needs a real E0NN code on each of these
// (so `--explain` and the SLM grounding docs work for them), and Errors.h
// has zero dependency back on Lexer.CPP (only on Lexer.H, which it already
// includes), so Lexer.cpp including Errors.h directly is architecturally
// clean, not circular. See these four classes' own construction sites in
// Lexer.cpp for exactly what each one catches.

// Syntax Error : Parser issues
class SyntaxError : public TurfError {
public:
  SyntaxError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Oops! There's a tiny mistake here:\n" +
                           Colors::RESET + "  " + Msg,
                  "Check your spelling, punctuation like ';' or "
                  "'}', and make sure everything is closed properly.") {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;
  }
};
// Keyword Error : Misspelled keywords
class KeywordError : public TurfError {
public:
  KeywordError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED + "Hmm, I don't know the word '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;

    std::vector<std::string> Candidates = SuggestionIndex::suggestKeywords(Name);

    if (!Candidates.empty()) {
      std::string sug = "Did you mean: ";
      for (size_t i = 0; i < Candidates.size(); ++i) {
        sug += Colors::BRIGHT_YELLOW + "'" + Candidates[i] + "'" +
                   Colors::BRIGHT_GREEN;
        if (i != Candidates.size() - 1)
          sug += ", ";
      }
      sug += "?";
      Hint_ = sug;
      // SLM-001 item 1: unlike every other deterministic-fix site (Errors
      // Scope.h/Struct.h/Module.h), this isn't RankedSuggestions-shaped
      // (suggestKeywords() returns a plain vector, no module-symbol half) --
      // unambiguous here just means exactly one candidate.
      if (Candidates.size() == 1)
        withDeterministicFix(Name, Candidates[0]);
    } else {
      Hint_ = "Check if you spelled the keyword correctly.";
    }
  }
};

// A radix prefix (0x/0X, 0b/0B, 0o/0O) with no valid digit for that radix
// anywhere after it -- `0x` alone, `0x;`, `0x ` (whitespace), `0x` at EOF.
// Distinct from InvalidRadixDigitError below: this fires when NOTHING
// usable followed the prefix at all, not when a recognizable-but-wrong
// digit did.
class MalformedRadixPrefixError : public TurfError {
public:
  MalformedRadixPrefixError(SourceLocation Loc, const std::string &Prefix)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + Prefix +
                           Colors::BRIGHT_RED +
                           "' isn't followed by any digit." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;
    Hint_ = "'" + Prefix + "' starts a " +
            (Prefix.size() > 1 && (Prefix[1] == 'x' || Prefix[1] == 'X')
                 ? std::string("hexadecimal (0-9, a-f)")
             : Prefix.size() > 1 && (Prefix[1] == 'b' || Prefix[1] == 'B')
                 ? std::string("binary (0-1)")
                 : std::string("octal (0-7)")) +
            " integer literal, but needs at least one digit after it.";
  }
};

// A digit was found where one was expected for this literal's radix, but
// it's not a valid digit for that radix -- `0b012` ('2' isn't binary),
// `0o8` ('8' isn't octal), `0xZZ` ('Z' isn't hex). Fires only when the
// offending character looks like it was meant to continue the literal
// (alphanumeric immediately following consumed digits) -- see
// gettokScan()'s own comment for why that heuristic is what tells this
// apart from the literal simply, legitimately, ending (`0b01 + 5` is not
// an error; '+' doesn't look like a continuation attempt).
class InvalidRadixDigitError : public TurfError {
public:
  InvalidRadixDigitError(SourceLocation Loc, char BadDigit, int Radix,
                         const std::string &SpellingSoFar)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN +
                           std::string(1, BadDigit) + Colors::BRIGHT_RED +
                           "' isn't a valid digit in '" + Colors::CYAN +
                           SpellingSoFar + Colors::BRIGHT_RED + "' (" +
                           (Radix == 16   ? "hexadecimal, 0-9/a-f"
                            : Radix == 2  ? "binary, 0-1"
                                         : "octal, 0-7") +
                           ")." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;
    Hint_ = "Every digit after '" + SpellingSoFar.substr(0, 2) +
            "' must be valid in that radix. Double-check the digit, or "
            "did you mean to write a decimal number instead (drop the '" +
            SpellingSoFar.substr(0, 2) + "' prefix)?";
  }
};

// `_` used as a digit separator (F-NUM-1 / FREEZE.md §6.2 row 1) in the
// first or last position of a digit run -- `_5`, `5_`, `0x_5`, `0x5_`,
// `1_.5`/`1._5` (immediately touching the dot), `1e_5`/`1e5_` (immediately
// touching the exponent marker or ending the exponent). A separator between
// two digits (`1_000`) is always fine; only a LEADING or TRAILING one in
// any single digit run is rejected -- matches the frozen rule exactly,
// deliberately not stricter (e.g. `1__000`, a doubled separator, is not
// rejected by this class; FREEZE.md never says to).
class MisplacedSeparatorError : public TurfError {
public:
  MisplacedSeparatorError(SourceLocation Loc, const std::string &SpellingSoFar,
                          bool Leading)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           std::string(Leading ? "A digit run can't start"
                                               : "A digit run can't end") +
                           " with '" + Colors::CYAN + "_" + Colors::BRIGHT_RED +
                           "'." + Colors::RESET) {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;
    Hint_ = "'_' is only a separator BETWEEN digits (like '1_000_000'), "
            "never at the very start or end of a digit run. Remove the " +
            std::string(Leading ? "leading" : "trailing") +
            " underscore in '" + SpellingSoFar + "'.";
  }
};

// `e`/`E` immediately after a digit run, but not followed by a
// syntactically valid exponent (an optional '+'/'-' then at least one
// digit) -- `1e`, `1e+`, `1e-`, `1ex`. Once 'e'/'E' appears right after a
// decimal digit run, it is ALWAYS committed to as an exponent marker (never
// silently reinterpreted as the start of a separate following token) --
// this matches how every mainstream language with C-style float exponents
// treats a bare trailing 'e', and is why this needs to be a real lexical
// error rather than the literal just ending early.
class MalformedExponentError : public TurfError {
public:
  MalformedExponentError(SourceLocation Loc, const std::string &SpellingSoFar)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "'" + Colors::CYAN + SpellingSoFar + Colors::BRIGHT_RED +
                           "' has an exponent marker with no exponent after it." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;
    Hint_ = "After 'e'/'E' (optionally followed by '+' or '-'), at least "
            "one digit is required -- e.g. '1e6', '1.5e-3', '1E+10'.";
  }
};

#endif
