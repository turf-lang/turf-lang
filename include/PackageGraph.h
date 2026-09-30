#ifndef TURF_PACKAGE_GRAPH_H
#define TURF_PACKAGE_GRAPH_H

// LEC-001 stage 4 (FREEZE.md 6.16, F-PKG-1/2/4): the package graph a driver
// (the `turf` front end) hands the compiler with `--package-graph <file>`.
//
// This is the whole seam between the two programs. The compiler never reads
// turf.toml or turf.lock and never looks for packages: it is told, for each
// package, where its sources are, which packages it may import (its DIRECT
// dependencies only), and whether its `unsafe module;` declarations are
// honored. Everything that depends on TurfLang source -- which file an import
// names, the collision errors, the internal-module rule -- is then decided
// here, from this graph and nothing else.
//
// With no graph the compiler is in single-file mode, exactly as before
// stage 4.

#include <optional>
#include <string>
#include <vector>

struct PackageInfo {
  std::string Name;
  std::string SrcRoot;           ///< canonical absolute path of the package's src/
  std::vector<std::string> Deps; ///< direct dependencies, by package name
  bool AllowUnsafe = false;      ///< honor `unsafe module;` in this package's files
};

struct PackageGraph {
  bool Active = false;
  std::string Root; ///< the package being built
  std::vector<PackageInfo> Packages;

  const PackageInfo *find(const std::string &Name) const;
  /// The package whose src/ contains @p Path (canonicalized), or nullptr.
  /// A file under lib/std is in no package here: the stdlib is resolved by
  /// its own rule (imports inside it are stdlib-only).
  const PackageInfo *packageOf(const std::string &Path) const;
};

/// The graph in force for this compile. Set once by the driver before
/// parsing; inactive (single-file mode) unless --package-graph was given.
extern PackageGraph g_PackageGraph;

/// Reads and validates a package-graph file. On failure returns std::nullopt
/// and sets @p Error to a message naming the file and the problem; the caller
/// reports it and stops, since no import can be resolved against a graph that
/// is not well formed. @p EntryFile must lie inside the root package's src/,
/// and unless @p EntryIsModule (`--check --module`, checking a library's root
/// module) it must be exactly the root package's src/main.tr: a package has
/// one program entry point.
std::optional<PackageGraph> LoadPackageGraph(const std::string &File,
                                             const std::string &EntryFile,
                                             bool EntryIsModule,
                                             std::string &Error);

#endif
