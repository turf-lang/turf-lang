#ifndef MIGRATE_PUB_H
#define MIGRATE_PUB_H

#include <string>
#include <string_view>
#include <vector>

// `turf --migrate=pub` (LEC-002 stage 6, docs/development/changes/LEC-002.md).
//
// Before 2.0 every declaration in a module was visible to every importer. From
// 2.0 a declaration is private unless it says `pub`. This tool rewrites a
// module so that it keeps exactly its pre-2.0 surface: `pub` goes before every
// top-level `fn`, `struct`, `enum`, `error`, `machine` and `extern fn`, and
// before every struct field. It is a starting point, not a review -- the
// author is expected to then remove `pub` from what should never have been
// public (`docs/development/changes/LEC-002.md`, "The migration").
//
// Like the formatter it is a token-stream rewrite over the REAL lexer, so a
// keyword inside a string or a comment can never be mistaken for a
// declaration, and it never touches anything but the insertion points: every
// other byte of the file passes through unchanged. It is idempotent -- a
// declaration that already says `pub`/`public` is left alone -- and it makes
// no attempt to decide what is a "main" program file (that is the caller's
// choice of which files to pass); `pub` in a main program is accepted and
// merely redundant.
namespace turf::migrate {

/// One inserted `pub `.
struct Insertion {
  int Line = 0; ///< 1-based, in the ORIGINAL source
  int Col = 0;  ///< 1-based column of the token `pub ` was inserted before
  /// The declaration head, e.g. "fn helper", "struct Point", or for a field
  /// its name -- for the report only.
  std::string What;
};

struct MigrateResult {
  std::string Text; ///< the migrated source (== the input when nothing changed)
  std::vector<Insertion> Insertions;
};

/// Migrates @p Source. Never fails: input that does not lex cleanly simply
/// yields fewer insertion points, and the rest of the text is untouched.
MigrateResult migratePub(std::string_view Source);

} // namespace turf::migrate

#endif // MIGRATE_PUB_H
