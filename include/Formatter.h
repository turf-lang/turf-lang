#ifndef FORMATTER_H
#define FORMATTER_H

#include <string>
#include <string_view>

// A deterministic, idempotent source formatter (doc 09/doc 02 §D of
// turf-lsp's implementation plan) -- reused by both the CLI (`turf
// --format`) and the LSP (`textDocument/formatting`), so there is exactly
// ONE formatting implementation, never a parallel reimplementation that
// could drift from the real grammar (the bug class AGENTS.md warns about
// repeatedly for exactly this kind of tooling-adjacent code).
//
// Deliberately scoped to INDENTATION ONLY for this first version (the
// plan doc's own "ship a token-stream formatter first" recommendation,
// taken one step further): re-derives each line's indentation depth from
// the REAL lexer's brace/paren/bracket token stream (so a `{` inside a
// string or `//` comment can never be miscounted -- the lexer already
// skips those correctly), then rewrites ONLY each line's leading
// whitespace based on that depth, plus trailing-whitespace stripping,
// blank-line-run collapsing, and a single final newline. Every other
// byte of every line -- identifiers, literals, comments, existing
// interior spacing -- passes through completely unchanged. This
// intentionally leaves operator/punctuation spacing normalization (doc
// 09's fuller rule set) for a later pass: getting bracket-depth
// indentation right, safely, on real code is the highest-value, lowest-
// ambiguity piece, and doesn't risk corrupting content the way a
// token-stream RE-EMISSION would if any operator/generic-bracket
// ambiguity (`List<T>` vs `a < b`) were gotten wrong.
namespace turf::format {

struct FormatOptions {
  int IndentWidth = 4;
  bool InsertSpaces = true; // spaces vs tabs for indentation
  bool TrimTrailingWhitespace = true;
  bool InsertFinalNewline = true;
};

struct FormatResult {
  std::string Text;
  // False only if the input couldn't be tokenized at all (never on a
  // merely-unbalanced-but-lexable buffer, which still produces a best-
  // effort result rather than refusing outright -- see format()'s own
  // doc comment on why never destroying content is the higher priority).
  bool Ok = true;
  // Why Ok is false, for formatChecked() (empty otherwise): which of the
  // guarantees below the formatter would have broken.
  std::string Error;
};

// Formats `Source`. Never deletes or reorders any non-whitespace content
// -- the worst a bug here can do is mis-indent a line, never lose code.
// Idempotent by construction: indentation is a pure function of the
// token stream's bracket depth, which formatting itself (a whitespace-
// only rewrite) cannot change, so format(format(x).Text) == format(x).
FormatResult format(std::string_view Source, const FormatOptions &Options = {});

// REL-002 stage 2: the guarantees of `turf fmt`, checked instead of argued.
// "Idempotent by construction" and "never loses code" are properties of the
// design above; a tool that rewrites a user's files should not rest on a
// design argument alone, so `turf fmt` verifies every result before it
// writes or reports it. For a formatted text `Formatted` of `Original`:
//   1. no non-whitespace byte was added, removed or reordered (so comments,
//      which are not tokens, are covered too);
//   2. the REAL lexer reads the same token stream from both (kind and
//      value, never line or column), which is what "the program is
//      unchanged" means -- it catches a re-indented line that was inside a
//      literal, which check 1 cannot see;
//   3. formatting `Formatted` again changes nothing.
// Returns "" when all three hold, else a sentence saying which failed and
// where. Takes both texts so a test can hand it a corrupted one.
std::string verifyFormatting(std::string_view Original, std::string_view Formatted,
                             const FormatOptions &Options = {});

// format() followed by verifyFormatting(). On a violation Ok is false, Text is
// empty and Error says why: the caller leaves the file alone and reports a
// formatter bug rather than writing a result it cannot vouch for.
FormatResult formatChecked(std::string_view Source, const FormatOptions &Options = {});

} // namespace turf::format

#endif // FORMATTER_H
