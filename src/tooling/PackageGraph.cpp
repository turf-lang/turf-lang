// LEC-001 stage 4: reading and validating the package graph (PackageGraph.h).
//
// Validation happens once, up front, and is strict: a graph that names a
// missing directory, an unknown dependency or a dependency with no root
// module would otherwise surface as a confusing "module not found" at some
// import deep inside a dependency, far from the actual mistake (which is in
// whatever wrote the graph -- normally `turf`, from turf.toml).

#include "tooling/PackageGraph.h"
#include "frontend/Parser.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <cctype>
#include <set>

PackageGraph g_PackageGraph;

const PackageInfo *PackageGraph::find(const std::string &Name) const {
  for (const auto &P : Packages)
    if (P.Name == Name)
      return &P;
  return nullptr;
}

static std::string Canonical(const std::string &Path) {
  llvm::SmallString<256> Out;
  if (llvm::sys::fs::real_path(Path, Out))
    return "";
  return Out.str().str();
}

static bool IsUnder(const std::string &Path, const std::string &Dir) {
  if (Path.size() <= Dir.size() || Path.compare(0, Dir.size(), Dir) != 0)
    return false;
  char Sep = Path[Dir.size()];
  return Sep == '/' || Sep == '\\';
}

const PackageInfo *PackageGraph::packageOf(const std::string &Path) const {
  if (!Active || Path.empty())
    return nullptr;
  std::string C = Canonical(Path);
  if (C.empty())
    return nullptr;
  // Longest SrcRoot wins, so a package vendored inside another package's
  // directory tree is still attributed to itself.
  const PackageInfo *Best = nullptr;
  size_t BestLen = 0;
  auto Consider = [&](const PackageInfo &P, const std::string &Dir) {
    if (!Dir.empty() && IsUnder(C, Dir) && (!Best || Dir.size() > BestLen)) {
      Best = &P;
      BestLen = Dir.size();
    }
  };
  for (const auto &P : Packages) {
    Consider(P, P.SrcRoot);
    // A test file is the root package's own code (REL-002), so an import
    // inside it resolves with that package's rules.
    Consider(P, P.TestsRoot);
  }
  return Best;
}

bool PackageGraph::inTests(const std::string &Path) const {
  if (!Active || Path.empty())
    return false;
  const PackageInfo *R = find(Root);
  if (!R || R->TestsRoot.empty())
    return false;
  std::string C = Canonical(Path);
  return !C.empty() && IsUnder(C, R->TestsRoot);
}

// A package name is spelled in `import`, so it must lex as one identifier.
static bool IsIdentifier(const std::string &S) {
  if (S.empty() || !(std::isalpha((unsigned char)S[0]) || S[0] == '_'))
    return false;
  for (char C : S)
    if (!(std::isalnum((unsigned char)C) || C == '_'))
      return false;
  return true;
}

std::optional<PackageGraph> LoadPackageGraph(const std::string &File,
                                             std::string &Error) {
  auto Fail = [&](const std::string &Msg) -> std::optional<PackageGraph> {
    Error = "package graph " + File + ": " + Msg;
    return std::nullopt;
  };

  auto Buf = llvm::MemoryBuffer::getFile(File);
  if (!Buf)
    return Fail("cannot read it (" + Buf.getError().message() + ")");
  auto Parsed = llvm::json::parse((*Buf)->getBuffer());
  if (!Parsed)
    return Fail("not valid JSON (" + llvm::toString(Parsed.takeError()) + ")");
  const llvm::json::Object *Top = Parsed->getAsObject();
  if (!Top)
    return Fail("the top level must be an object");

  // Checked first so a future format fails with a message about the format,
  // not with whatever field it renamed.
  auto Format = Top->getInteger("format");
  if (!Format || *Format != PackageGraphFormat)
    return Fail("\"format\" must be " + std::to_string(PackageGraphFormat) +
                " (this turfc reads package-graph format " +
                std::to_string(PackageGraphFormat) + ")");
  auto RootName = Top->getString("root");
  if (!RootName)
    return Fail("missing \"root\" (the name of the package being built)");
  const llvm::json::Object *Pkgs = Top->getObject("packages");
  if (!Pkgs || Pkgs->empty())
    return Fail("missing \"packages\" (an object from package name to package)");

  // Relative "src" paths are relative to the graph file, so a graph can be
  // written once and moved with the tree it describes.
  llvm::SmallString<256> BaseDir(File);
  llvm::sys::path::remove_filename(BaseDir);

  PackageGraph G;
  G.Active = true;
  G.Root = RootName->str();
  for (const auto &KV : *Pkgs) {
    std::string Name = KV.first.str();
    if (!IsIdentifier(Name))
      return Fail("package name '" + Name + "' is not an identifier (it is spelled in `import`)");
    const llvm::json::Object *P = KV.second.getAsObject();
    if (!P)
      return Fail("package '" + Name + "' must be an object");
    PackageInfo Info;
    Info.Name = Name;

    auto Src = P->getString("src");
    if (!Src)
      return Fail("package '" + Name + "' has no \"src\" directory");
    llvm::SmallString<256> SrcPath(*Src);
    if (!llvm::sys::path::is_absolute(SrcPath)) {
      llvm::SmallString<256> Joined(BaseDir);
      llvm::sys::path::append(Joined, SrcPath);
      SrcPath = Joined;
    }
    if (!llvm::sys::fs::is_directory(SrcPath))
      return Fail("package '" + Name + "': src directory " + SrcPath.str().str() + " does not exist");
    Info.SrcRoot = Canonical(SrcPath.str().str());

    if (const llvm::json::Array *Deps = P->getArray("deps")) {
      for (const auto &D : *Deps) {
        auto DName = D.getAsString();
        if (!DName)
          return Fail("package '" + Name + "': \"deps\" must be a list of package names");
        Info.Deps.push_back(DName->str());
      }
    }
    if (auto U = P->getBoolean("allow_unsafe"))
      Info.AllowUnsafe = *U;
    if (P->get("tests")) {
      auto Tests = P->getString("tests");
      if (!Tests)
        return Fail("package '" + Name + "': \"tests\" must be a directory path");
      if (Name != G.Root)
        return Fail("package '" + Name + "': only the root package can have \"tests\"");
      llvm::SmallString<256> TestsPath(*Tests);
      if (!llvm::sys::path::is_absolute(TestsPath)) {
        llvm::SmallString<256> Joined(BaseDir);
        llvm::sys::path::append(Joined, TestsPath);
        TestsPath = Joined;
      }
      if (!llvm::sys::fs::is_directory(TestsPath))
        return Fail("package '" + Name + "': tests directory " + TestsPath.str().str() +
                    " does not exist");
      Info.TestsRoot = Canonical(TestsPath.str().str());
    }

    // Unknown keys are an error, the same rule the manifest follows
    // (FREEZE.md 6.16): a misspelled "allow_unsafe" must not silently mean
    // "no".
    for (const auto &Field : *P) {
      llvm::StringRef K = Field.first;
      if (K != "src" && K != "deps" && K != "allow_unsafe" && K != "tests")
        return Fail("package '" + Name + "': unknown key \"" + K.str() + "\"");
    }
    G.Packages.push_back(std::move(Info));
  }

  const PackageInfo *Root = G.find(G.Root);
  if (!Root)
    return Fail("\"root\" names '" + G.Root + "', which is not in \"packages\"");

  std::set<std::string> Depended;
  for (const auto &P : G.Packages) {
    std::set<std::string> Seen;
    for (const auto &D : P.Deps) {
      if (!Seen.insert(D).second)
        return Fail("package '" + P.Name + "' lists dependency '" + D + "' twice");
      if (D == P.Name)
        return Fail("package '" + P.Name + "' lists itself as a dependency");
      if (!G.find(D))
        return Fail("package '" + P.Name + "' depends on '" + D + "', which is not in \"packages\"");
      Depended.insert(D);
    }
  }
  for (const auto &Name : Depended) {
    const PackageInfo *D = G.find(Name);
    // A dependency is imported by its package name, which would always be
    // ambiguous with the stdlib module of the same name (FREEZE.md 6.16).
    if (IsStdlibModuleName(Name))
      return Fail("dependency '" + Name + "' has the name of a standard-library module, "
                  "so `import " + Name + ";` could never name it; rename the package");
    // Only a library package can be depended on: its root module is what
    // `import <name>;` loads.
    llvm::SmallString<256> RootModule(D->SrcRoot);
    llvm::sys::path::append(RootModule, Name + ".tr");
    if (!llvm::sys::fs::is_regular_file(RootModule))
      return Fail("package '" + Name + "' is a dependency but has no root module " +
                  RootModule.str().str() + " (only a library package can be depended on)");
  }
  return G;
}

std::optional<PackageGraph> LoadPackageGraph(const std::string &File,
                                             const std::string &EntryFile,
                                             bool EntryIsModule,
                                             std::string &Error) {
  auto Fail = [&](const std::string &Msg) -> std::optional<PackageGraph> {
    Error = "package graph " + File + ": " + Msg;
    return std::nullopt;
  };
  std::optional<PackageGraph> Loaded = LoadPackageGraph(File, Error);
  if (!Loaded)
    return std::nullopt;
  PackageGraph &G = *Loaded;
  const PackageInfo *Root = G.find(G.Root);

  std::string Entry = Canonical(EntryFile);
  // A graph that names tests/ is the one `turfc --test` hands each test
  // compile (REL-002): the file may be any test, not just src/main.tr.
  const bool EntryIsTest =
      !Entry.empty() && !Root->TestsRoot.empty() && IsUnder(Entry, Root->TestsRoot);
  if (Entry.empty() || (!IsUnder(Entry, Root->SrcRoot) && !EntryIsTest))
    return Fail("the file being compiled (" + EntryFile + ") is not inside the root package's src " +
                Root->SrcRoot + (Root->TestsRoot.empty() ? "" : " or tests " + Root->TestsRoot));
  // A package has ONE program entry point, src/main.tr (FREEZE.md 6.16's
  // layout; owner decision 2026-09-30). Accepting any file under src/ as a
  // program made "which file is the program?" a question each driver answered
  // for itself, and let a helper module with a stray `fn main` be built as a
  // second program. Any other file is a module, checked with --module.
  if (!EntryIsModule && !EntryIsTest) {
    llvm::SmallString<256> Main(Root->SrcRoot);
    llvm::sys::path::append(Main, "main.tr");
    if (Entry != Canonical(Main.str().str()))
      return Fail("in a package build the program's entry point is the root package's "
                  "src/main.tr, not " + EntryFile +
                  " (check another file of the package with --check --module; run a "
                  "single file on its own without --package-graph)");
  }
  return Loaded;
}
