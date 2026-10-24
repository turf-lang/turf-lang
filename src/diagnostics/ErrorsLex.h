#ifndef ERRORS_LEX_H
#define ERRORS_LEX_H

#include "diagnostics/Errors.h"

// Below SyntaxError/KeywordError (raised from the PARSER, despite this
// file's name) are five classes raised directly from inside the LEXER's own
// number-scanning branch (gettokScan(), Lexer.cpp) -- SEM-001 (the fifth,
// MalformedNumberError, E139, was added with issue #263)
// (docs/archive/ARCHITECTURE_OVERHAUL.md §8). Lexer.cpp does not normally
// construct a coded TurfError (its own diagnostics go through the raw
// LogErrorAt(), which names its code by ErrCode::, e.g. the malformed-char-
// literal check) precisely
// because Lexer.h stays deliberately LLVM/AST-free and TurfError subclasses
// used to live only in headers reachable from the Parser/Resolver/
// TypeChecker layers -- but SEM-001 needs a real E0NN code on each of these
// (so `--explain` and the SLM grounding docs work for them), and Errors.h
// has zero dependency back on Lexer.CPP (only on Lexer.H, which it already
// includes), so Lexer.cpp including Errors.h directly is architecturally
// clean, not circular. See these four classes' own construction sites in
// Lexer.cpp for exactly what each one catches.

// Syntax Error : Parser issues
//
// A vehicle, not a mistake: the code is the caller's (an ErrCode::), because
// "the shape of this code is wrong" is a dozen different mistakes and a
// beginner needs to be told which. It used to carry one code for all of them
// (E015, now retired) and so `--explain` could only say "something is wrong
// somewhere". The `Code` parameter is required on purpose: forgetting it is a
// compile error, not a quiet return to the old catch-all. (issue #263)
class SyntaxError : public TurfError {
public:
  // `Hint` replaces the default "check your punctuation" advice, which is
  // only right for a missing or misplaced token; a code that is not about
  // punctuation (a duplicate name, an invalid array size) must say its own.
  // The advice for a code that sites raise without a hint of their own
  // (DX-3, issue #291): each says what to write, so the punctuation line
  // below stays only for a code with nothing more specific to say.
  static std::string defaultHintFor(const char *Code) {
    const std::string C = Code;
    if (C == ErrCode::ExpectedType)
      return "Write a type there: `int`, `double`, `string`, `bool`, `char`, a "
             "struct or enum name, or a composite such as `int[5]` or `&int`.";
    if (C == ErrCode::UnclosedDelimiter)
      return "Add the matching closing bracket: every `(` needs a `)`, every "
             "`[` a `]` and every `{` a `}`.";
    if (C == ErrCode::MissingOpeningDelimiter)
      return "Open it with the bracket the message names, e.g. `fn(int): int` "
             "for a function type.";
    if (C == ErrCode::MissingColon)
      return "Put a `:` before the type, e.g. `x: int` or `fn(int): int`.";
    if (C == ErrCode::MissingComma)
      return "Put a `,` between the items, e.g. `Map<string, int>`, and close "
             "the list with its bracket.";
    if (C == ErrCode::MissingSemicolon)
      return "End the statement with `;` right after its last token, e.g. "
             "`unsafe module;`.";
    if (C == ErrCode::DuplicateDeclaration)
      return "Rename one of the two declarations, or delete the duplicate.";
    if (C == ErrCode::VoidTypeNotAllowed)
      return "Use `void` only as a return type, `fn f(): void`; for a variable "
             "or parameter write a real type such as `int`.";
    return "Check your spelling, punctuation like ';' or '}', and make sure "
           "everything is closed properly.";
  }
  SyntaxError(SourceLocation Loc, const char *Code, const std::string &Msg,
              const std::string &Hint = "")
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Oops! There's a tiny mistake here:\n" +
                           Colors::RESET + "  " + Msg,
                  Hint.empty() ? defaultHintFor(Code) : Hint) {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;
    withCode(Code);
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

// A number whose decimal point is wrong (E139). One code for the three ways it
// goes wrong, because to a beginner they are one mistake -- "that isn't a
// number" -- and the message says which:
//   SecondPoint   `3.14.15`, `1e5.5`, `3.14.;` -- a number can only have one
//                 '.', and it comes before any exponent. The lexer used to
//                 stop at `3.14` and hand `.15` to the parser, which then said
//                 "Expected ';'" at the second dot.
//   TrailingPoint `5.` -- a '.' with no digit after it. FREEZE.md's dotted
//                 float needs a fractional digit, so `5.` was never a float;
//                 the lexer left the '.' for the parser, which said "Expected a
//                 method name after '.'" to someone who was writing a decimal.
//                 (`5.name` -- a letter after the '.' -- is still member access
//                 and is NOT this error.)
//   RadixPoint    `0xFF.5` -- hex, binary and octal literals are whole numbers.
class MalformedNumberError : public TurfError {
public:
  enum class Kind { SecondPoint, TrailingPoint, RadixPoint };

  MalformedNumberError(SourceLocation Loc, const std::string &SpellingSoFar,
                       Kind K)
      : TurfError(Loc, Colors::BRIGHT_RED + "'" + Colors::CYAN + SpellingSoFar +
                           (K == Kind::TrailingPoint ? "." : "") +
                           Colors::BRIGHT_RED + "' " + describe(K) +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;
    Hint_ = hintFor(K, SpellingSoFar);
  }

private:
  static std::string describe(Kind K) {
    switch (K) {
    case Kind::SecondPoint:
      return "is followed by a second '.', but a number can have only one "
             "decimal point.";
    case Kind::TrailingPoint:
      return "ends in a '.' with no digit after it.";
    case Kind::RadixPoint:
      return "is followed by a '.', but hexadecimal, binary and octal "
             "numbers are whole numbers.";
    }
    return "is not a valid number.";
  }
  static std::string hintFor(Kind K, const std::string &S) {
    switch (K) {
    case Kind::SecondPoint:
      return "'" + S + "' is already a complete number, so what follows the "
             "second '.' looks like a second number run together with it. "
             "Keep one '.' for a single decimal number, or put a ',' or an "
             "operator between two separate values.";
    case Kind::TrailingPoint:
      return "A decimal number needs a digit after the point. Write '" + S +
             ".0' for the decimal, or just '" + S + "' for the whole number.";
    case Kind::RadixPoint:
      return "Drop the '.' and what follows it, or write the value as a "
             "decimal number (with a '.') instead of hexadecimal, binary or "
             "octal.";
    }
    return "";
  }
};

#endif
