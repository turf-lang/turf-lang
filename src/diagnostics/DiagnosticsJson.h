/**
 * @file DiagnosticsJson.h
 * @brief DX-2: `turfc --diagnostics=json`, one JSON object per diagnostic.
 *
 * @details The text form of a diagnostic is for people and is outside the
 *          compatibility promise (D-6). This form is for tools: each
 *          diagnostic is one line of JSON on stderr, written when the text
 *          form would be, so it reaches the reader at every exit point the
 *          text does. The schema is versioned (DiagnosticsJson::SchemaVersion),
 *          published in docs/reference/schemas/diagnostics.schema.json and
 *          described in docs/reference/cli_and_diagnostics.md: a minor adds
 *          fields, a major changes or removes one.
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#ifndef TURF_DIAGNOSTICS_JSON_H
#define TURF_DIAGNOSTICS_JSON_H

#include "frontend/Lexer.h"

#include <string>

namespace DiagnosticsJson {

/// The schema's version, "major.minor". Also printed by `turfc --version
/// --json` (IF-1) so a tool can check it before reading a line.
inline constexpr const char *SchemaVersion = "1.0";

/// Set by the driver's `--diagnostics=json`. Off by default.
extern bool Enabled;

/// `D` as one line of JSON (no trailing newline). `IsWarning` is the severity
/// the text form prints (a denied warning is an error).
std::string render(const DiagnosticEngine::Diag &D, bool IsWarning);

} // namespace DiagnosticsJson

#endif // TURF_DIAGNOSTICS_JSON_H
