#ifndef UNSAFE_REPORT_H
#define UNSAFE_REPORT_H

#include "tooling/PackageGraph.h"

#include <string>
#include <string_view>
#include <vector>

// `turfc --unsafe-report` (LB-2, issue #325): where a project's unsafe code is.
//
// For each package of a package graph, and for the standard library, it
// counts the files that declare `unsafe module;` (the raw-pointer capability
// for the whole file), the `unsafe { }` blocks, the `extern fn` declarations
// and the C libraries those name with `extern from "lib"`, and says whether
// the package's `unsafe module;` is honored (the graph's `allow_unsafe`, which
// the root's `[unsafe] allow` decides; the stdlib is always trusted). `turf
// audit` prints it as a table and `turf graph --unsafe` carries it in the
// graph's JSON. The format is docs/reference/schemas/unsafe-report.schema.json.
//
// Like --migrate=pub it reads tokens with the real lexer, so a keyword in a
// string or a comment is never counted, and it resolves nothing: it reads
// every .tr file under each package's src/ (and lib/std), imported or not,
// since a file that is in the package is part of what the package ships.
// Native sources are LB-1's `[native]` table, which turf.toml does not have
// yet, so they are not here.
namespace turf::unsafe_report {

/// What one file contains.
struct FileSurface {
  std::string Path;     ///< relative to its package's src/ (or lib/std), with '/'
  bool UnsafeModule = false;
  unsigned UnsafeBlocks = 0;
  unsigned ExternFns = 0;
  std::vector<std::string> LinkLibraries; ///< `extern from "lib"` names, sorted, once each
  /// The first lexical error, "line N: message", or empty. The counts are
  /// those of the tokens before it.
  std::string Error;

  bool any() const {
    return UnsafeModule || UnsafeBlocks || ExternFns || !Error.empty();
  }
};

/// The sum over one package's files.
struct PackageSurface {
  std::string Name;
  bool AllowUnsafe = false;
  unsigned Modules = 0; ///< .tr files read
  unsigned UnsafeModules = 0;
  unsigned UnsafeBlocks = 0;
  unsigned ExternFns = 0;
  std::vector<std::string> LinkLibraries; ///< over every file, sorted, once each
  std::vector<FileSurface> Files;         ///< only the files with something to report, by path
};

struct Report {
  std::vector<PackageSurface> Packages; ///< by name
  PackageSurface Stdlib;                ///< lib/std, Name "std", always AllowUnsafe
};

/// Counts @p Source. Never fails: text that does not lex cleanly sets Error.
FileSurface scanSource(std::string_view Source);

/// Every package of @p Graph and the stdlib under @p StdlibDir. A file that
/// cannot be read is an error: returns false with the reason in @p Error.
bool buildReport(const PackageGraph &Graph, const std::string &StdlibDir,
                 Report &Out, std::string &Error);

/// The report as one line of JSON, keys sorted (the schema's format 1).
std::string reportJson(const Report &R);

/// The files given on a command line (single-file mode), as
/// `{"format":1,"files":[...]}` with each path as given; every file is
/// listed, whether or not it has anything to report.
std::string filesJson(const std::vector<FileSurface> &Files);

} // namespace turf::unsafe_report

#endif // UNSAFE_REPORT_H
