#ifndef LINT_LEVELS_H
#define LINT_LEVELS_H

// V-1 (TDP-2-003): lint levels. Every warning code is `allow`, `warn` or
// `deny`, chosen by the person building the program and never by the source:
// there is no syntax for it. The compiler takes them from its command line
// (`--allow E163`, `--warn E163`, `--deny E163`, `--lint-set 2.1`); `turf`
// reads the same choices from `turf.toml` ([lints]) and forwards them as those
// flags, so this file is the one place a level is decided.
//
//   allow  the warning is not reported at all
//   warn   the warning is reported and the build still succeeds (the default)
//   deny   the warning is reported as an error and the build fails
//
// An error has no level to choose: it is always an error, so `--allow` and
// `--warn` of an error's code are rejected, and `--deny` of one changes
// nothing. (Letting a program through with a type error is not a setting the
// compiler could honour: the code that follows depends on the type being
// right.)
//
// `--lint-set 2.N` pins the set of warnings to the ones 2.N shipped: a warning
// added in a later 2.x (ErrorCodeEntry::SinceMinor) starts at `allow`, so a new
// compiler never fails or even nags a build that pinned an older set. A level
// named explicitly wins over the pin, in either direction.

#include <string>

namespace LintLevels {

enum class Level { Allow, Warn, Deny };

const char *name(Level L);

/// Sets the level of `Code` ("E163", any case). False, with `Err` naming what
/// is wrong, for a string that is not a code, a code the compiler does not
/// have, or `allow`/`warn` of an error's code. A later call for the same code
/// replaces the earlier one, so the order on the command line decides.
bool setLevel(const std::string &Code, Level L, std::string &Err);

/// Pins the lint set: `Spec` is "2.N". `CompilerMinor` is this compiler's own
/// 2.x minor; a set newer than the compiler is an error (a manifest written for
/// a newer compiler must say so, not be quietly read as an older one).
bool setLintSet(const std::string &Spec, int CompilerMinor, std::string &Err);

/// The level a diagnostic with this code is reported at. Anything that is not
/// a warning code (an error, a diagnostic with no code) is `Warn` here: the
/// caller only asks for a warning.
Level levelOf(const std::string &Code);

/// levelOf() for a code that was added in 2.<SinceMinor>: the registry row's
/// own value is what levelOf() passes. Exists so the pin can be tested before a
/// warning newer than 2.0 does.
Level levelOfSince(const std::string &Code, int SinceMinor);

/// True once any level or pin was set; DiagnosticEngine::add() skips the
/// lookup otherwise.
bool anySet();

/// Forgets every level and the pin (a tool that analyses several programs in
/// one process, tests).
void reset();

} // namespace LintLevels

#endif
