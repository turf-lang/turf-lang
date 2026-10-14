// SMP-001: unit tests for the compiler query facade (include/turf/Analysis.h).
//
// Built as `turf-api-tests` next to `turf` (so the facade finds lib/std the
// same way the compiler does) and run by CI and scripts/dev/session_postflight.sh.
// Not a `.tr` test: what is under test is a C++ API and the global state it
// must leave untouched, neither of which a Turf program can observe.
//
// The isolation tests are the point. The facade exists because every tool
// that drove the pipeline by hand forgot some piece of global state; the
// only way to catch the next forgotten piece is to run different programs
// back to back and compare against a clean run, and to check the host's
// own state afterwards field by field.

#include "turf/Analysis.h"

#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "resolve/SymbolTable.h"
#include "sema/Types.h"
#include "tooling/Formatter.h"
#include "tooling/MigratePub.h"
#include "diagnostics/ErrorCategory.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

static int Failures = 0;
static const char *CurrentTest = "";

#define CHECK(Cond)                                                            \
  do {                                                                         \
    if (!(Cond)) {                                                             \
      ++Failures;                                                              \
      std::cerr << "FAIL [" << CurrentTest << "] " << __FILE__ << ":"         \
                << __LINE__ << ": " #Cond "\n";                                \
    }                                                                          \
  } while (0)

#define CHECK_EQ(A, B)                                                         \
  do {                                                                         \
    auto ValA = (A);                                                           \
    auto ValB = (B);                                                           \
    if (!(ValA == ValB)) {                                                     \
      ++Failures;                                                              \
      std::cerr << "FAIL [" << CurrentTest << "] " << __FILE__ << ":"         \
                << __LINE__ << ": " #A " == " #B "\n    got: " << ValA        \
                << "\n    want: " << ValB << "\n";                             \
    }                                                                          \
  } while (0)

static std::string render(const turf::AnalysisResult &R) {
  std::ostringstream OS;
  for (const auto &D : R.diagnostics())
    OS << (D.Level == turf::Severity::Error ? "error" : "warning") << " "
       << D.Code << " " << D.Where.Path << ":" << D.Where.Line << ":"
       << D.Where.Col << " " << D.Message << "\n";
  return OS.str();
}

static bool hasDiagnostic(const turf::AnalysisResult &R,
                          const std::string &Fragment) {
  for (const auto &D : R.diagnostics())
    if (D.Message.find(Fragment) != std::string::npos)
      return true;
  return false;
}

static std::string typeName(const TurfType *T) {
  return T ? T->getName() : std::string("<null>");
}

static const turf::SymbolInfo *findSymbol(const std::vector<turf::SymbolInfo> &S,
                                          const std::string &Name) {
  for (const auto &Sym : S)
    if (Sym.Name == Name)
      return &Sym;
  return nullptr;
}

// A scratch directory for fixtures that must exist on disk (imports resolve
// against real files). Removed on exit.
struct ScratchDir {
  fs::path Root;
  ScratchDir() {
    Root = fs::temp_directory_path() /
           ("turf-api-tests-" + std::to_string(::getpid()));
    fs::create_directories(Root);
  }
  ~ScratchDir() {
    std::error_code EC;
    fs::remove_all(Root, EC);
  }
  std::string write(const std::string &Name, const std::string &Text) const {
    fs::path P = Root / Name;
    std::ofstream(P) << Text;
    return P.string();
  }
};

static const char *CleanProgram = R"(fn add(a: int, b: int): int {
    return a + b;
}

fn main(): int {
    mut total: int = add(1, 2);
    immut label: string = "x";
    return total;
}
)";

static const char *UndefinedVariable = R"(fn main(): int {
    return missing;
}
)";

static const char *NeverReturnsError = R"(fn f(x: int): !int {
    return x;
}

fn main(): int {
    return 0;
}
)";

static void testCleanProgramQueries() {
  CurrentTest = "clean program queries";
  auto R = turf::analyze({{"clean.tr", CleanProgram}});
  CHECK(R.succeeded());
  CHECK_EQ(render(R), std::string());

  // `return total;` -- line 8, `total` at col 12.
  CHECK_EQ(typeName(R.typeAt("clean.tr", 8, 12)), std::string("int"));
  CHECK_EQ(typeName(R.typeAt("clean.tr", 8, 16)), std::string("int"));
  CHECK(R.typeAt("clean.tr", 8, 17) == nullptr); // just past the name
  // `immut label: string` -- the declaration itself.
  CHECK_EQ(typeName(R.typeAt("clean.tr", 7, 11)), std::string("string"));
  // `add(1, 2)` -- the call's result.
  CHECK_EQ(typeName(R.typeAt("clean.tr", 6, 22)), std::string("int"));

  auto TotalDef = R.definitionOf("clean.tr", 8, 12);
  CHECK(TotalDef.has_value());
  if (TotalDef) {
    CHECK_EQ(TotalDef->Path, std::string("clean.tr"));
    CHECK_EQ(TotalDef->Line, 6u);
  }
  auto AddDef = R.definitionOf("clean.tr", 6, 22);
  CHECK(AddDef.has_value());
  if (AddDef) {
    CHECK_EQ(AddDef->Path, std::string("clean.tr"));
    CHECK_EQ(AddDef->Line, 1u);
  }
  // A parameter used in the body.
  auto ParamDef = R.definitionOf("clean.tr", 2, 12);
  CHECK(ParamDef.has_value());
  if (ParamDef)
    CHECK_EQ(ParamDef->Line, 1u);
  // Whitespace has no definition.
  CHECK(!R.definitionOf("clean.tr", 4, 1).has_value());

  auto InMain = R.symbolsInScope("clean.tr", 8, 5);
  const auto *Total = findSymbol(InMain, "total");
  CHECK(Total && Total->Kind == turf::SymbolKind::Variable);
  if (Total)
    CHECK_EQ(typeName(Total->Type), std::string("int"));
  CHECK(findSymbol(InMain, "label"));
  const auto *Add = findSymbol(InMain, "add");
  CHECK(Add && Add->Kind == turf::SymbolKind::Function);
  CHECK(findSymbol(InMain, "strings")); // a prelude module
  CHECK(!findSymbol(InMain, "a"));      // add's parameter, not in main

  auto InAdd = R.symbolsInScope("clean.tr", 2, 5);
  const auto *A = findSymbol(InAdd, "a");
  CHECK(A && A->Kind == turf::SymbolKind::Parameter);
  CHECK(!findSymbol(InAdd, "total"));

  // The internal view: the prelude prefix, then the two functions.
  CHECK_EQ(R.program().size(), R.preludeImportCount() + 2);
}

static void testErrorDiagnostic() {
  CurrentTest = "error diagnostic";
  auto R = turf::analyze({{"undef.tr", UndefinedVariable}});
  CHECK(!R.succeeded());
  CHECK_EQ(R.diagnostics().size(), 1u);
  if (!R.diagnostics().empty()) {
    const auto &D = R.diagnostics().front();
    CHECK(D.Level == turf::Severity::Error);
    CHECK(!D.Code.empty());
    CHECK_EQ(D.Where.Path, std::string("undef.tr"));
    CHECK_EQ(D.Where.Line, 2u);
    CHECK(D.Message.find('\x1b') == std::string::npos);
    CHECK(D.Hint.find('\x1b') == std::string::npos);
  }
}

static void testTopLevelStatementIsADiagnostic() {
  CurrentTest = "top-level statement";
  // Parses fine, so it reaches the check (unlike `mut x: int = 1;`, which
  // the parser already rejects as an unexpected token, CLI and facade
  // alike).
  auto R = turf::analyze({{"top.tr", "fn main(): int {\n    return 0;\n}\n"
                                     "\nmain();\n"}});
  CHECK(!R.succeeded());
  CHECK(hasDiagnostic(R, "Statements are not allowed outside of a function"));
  // The rest of the file is still analysed.
  CHECK(R.definitionOf("top.tr", 1, 4).has_value());
}

// Different programs back to back, then the first again: the repeat must
// match the first run exactly. Anything one analysis leaves behind shows up
// as a difference here.
static void testBackToBackAnalysesAreIndependent() {
  CurrentTest = "back-to-back isolation";
  std::string First = render(turf::analyze({{"undef.tr", UndefinedVariable}}));
  std::string Clean = render(turf::analyze({{"clean.tr", CleanProgram}}));
  std::string Warn = render(turf::analyze({{"warn.tr", NeverReturnsError}}));
  std::string Again = render(turf::analyze({{"undef.tr", UndefinedVariable}}));
  CHECK_EQ(Again, First);
  CHECK_EQ(render(turf::analyze({{"clean.tr", CleanProgram}})), Clean);
  CHECK_EQ(render(turf::analyze({{"warn.tr", NeverReturnsError}})), Warn);
}

// The Resolver's "never returns an error" warning was deduplicated through
// a function-local static that nothing reset: the second analysis of the
// same file in one process silently lost it.
static void testWarningSurvivesReanalysis() {
  CurrentTest = "warning survives re-analysis";
  auto R1 = turf::analyze({{"warn.tr", NeverReturnsError}});
  auto R2 = turf::analyze({{"warn.tr", NeverReturnsError}});
  CHECK(hasDiagnostic(R1, "never actually returns an error"));
  CHECK(hasDiagnostic(R2, "never actually returns an error"));
  CHECK(R1.succeeded());
}

// `unsafe module;` registered its file in a set nothing reset: once a file
// had been analysed with the pragma, deleting the pragma did not take the
// capability away.
static void testUnsafeCapabilityDoesNotOutliveThePragma() {
  CurrentTest = "unsafe capability is per analysis";
  const std::string Body = "fn takesRaw(a: ptr): int {\n    return 0;\n}\n\n"
                           "fn main(): int {\n    return 0;\n}\n";
  turf::AnalysisOptions Opts;
  Opts.AllowUnsafeModules = true;
  auto With = turf::analyze({{"raw.tr", "unsafe module;\n\n" + Body}}, Opts);
  CHECK_EQ(render(With), std::string());
  auto Without = turf::analyze({{"raw.tr", Body}}, Opts);
  CHECK(!Without.succeeded());
}

// What an embedder had set up before calling analyze() -- a CLI compile in
// progress, when the SLM Sandbox calls in -- must be exactly as it was.
static void testHostStateIsRestored() {
  CurrentTest = "host state restored";
  InitializeCompilerGlobals();
  SymbolTable *HostTable = GlobalSymbolTable.get();
  SourceFilename = "host.tr";
  MainSourceFilename = "host.tr";
  SourceLines = {"host line 1", "host line 2"};
  AllSourceLines["host.tr"] = SourceLines;
  CurTok = TOK_IDENTIFIER;
  IdentifierStr = "hostIdent";
  CurLine = 41;
  CurCol = 7;
  g_braceDepth = 2;
  g_recoverActive = true;
  g_AllowUnsafeModules = false;
  DiagnosticEngine::clearDiagnostics();
  DiagnosticEngine::setCollectAllDiagnostics(false);
  DiagnosticEngine::add({3, 4, "host.tr"}, "host diagnostic");
  std::istringstream HostStream("fn host(): int { return 1; }");
  SetSourceBuffer(HostStream);

  turf::AnalysisOptions Opts;
  Opts.AllowUnsafeModules = true;
  auto R = turf::analyze({{"undef.tr", UndefinedVariable}}, Opts);
  CHECK(!R.succeeded());

  CHECK(GlobalSymbolTable.get() == HostTable);
  CHECK_EQ(SourceFilename, std::string("host.tr"));
  CHECK_EQ(MainSourceFilename, std::string("host.tr"));
  CHECK_EQ(SourceLines.size(), 2u);
  CHECK(AllSourceLines.count("host.tr") == 1);
  CHECK(AllSourceLines.count("undef.tr") == 0);
  CHECK_EQ(CurTok, static_cast<int>(TOK_IDENTIFIER));
  CHECK_EQ(IdentifierStr, std::string("hostIdent"));
  CHECK_EQ(CurLine, 41);
  CHECK_EQ(CurCol, 7);
  CHECK_EQ(g_braceDepth, 2);
  CHECK(g_recoverActive);
  CHECK(!g_AllowUnsafeModules);
  CHECK(!DiagnosticEngine::getCollectAllDiagnostics());
  CHECK_EQ(DiagnosticEngine::getDiagnosticCount(), 1u);
  CHECK(DiagnosticEngine::hasErrorAt(3));
  CHECK(SourceFile == &HostStream);
  // The host's stream was not read by the analysis.
  CHECK_EQ(gettok(), static_cast<int>(TOK_FN));

  g_recoverActive = false;
  DiagnosticEngine::clearDiagnostics();
  AllSourceLines.clear();
  SourceLines.clear();
  SourceFilename.clear();
  MainSourceFilename.clear();
  g_braceDepth = 0;
}

// turf::keywords() is the lexer's own table: every word the language reserves,
// with its kind, sorted -- so a tool's completion list cannot go stale.
static void testKeywords() {
  CurrentTest = "keywords";
  const auto &K = turf::keywords();
  auto Kind = [&](const std::string &Name) {
    for (const auto &W : K)
      if (W.Name == Name)
        return static_cast<int>(W.Kind);
    return -1;
  };
  const int Kw = static_cast<int>(turf::KeywordKind::Keyword);
  const int Ty = static_cast<int>(turf::KeywordKind::Type);
  const int Lit = static_cast<int>(turf::KeywordKind::Literal);
  for (const char *W : {"fn", "if", "elseif", "compare", "matches", "recover", "try", "defer",
                        "pub", "public", "mut", "mutable", "self", "import", "operator"})
    CHECK_EQ(Kind(W), Kw);
  for (const char *W : {"int", "int32", "double", "bool", "char", "string", "void", "ptr"})
    CHECK_EQ(Kind(W), Ty);
  for (const char *W : {"true", "false", "null"})
    CHECK_EQ(Kind(W), Lit);
  CHECK_EQ(Kind("notakeyword"), -1);
  for (std::size_t I = 1; I < K.size(); ++I)
    CHECK(K[I - 1].Name < K[I].Name);
}

// AnalysisOptions::ShouldCancel (an editor abandons an analysis of text a
// later keystroke has already replaced). Cancelling at either boundary gives
// an empty result, not a partial one, and leaves the host's state as it
// found it; a callback that never fires changes nothing.
static void testCancellation() {
  CurrentTest = "cancellation";
  const std::string Uncancelled = render(turf::analyze({{"c.tr", UndefinedVariable}}));
  CHECK(!Uncancelled.empty());

  // A callback that never fires: same diagnostics, same answers, and it was
  // asked exactly at the two boundaries.
  int Asked = 0;
  turf::AnalysisOptions Never;
  Never.ShouldCancel = [&] { ++Asked; return false; };
  auto Full = turf::analyze({{"c.tr", UndefinedVariable}}, Never);
  CHECK(!Full.cancelled());
  CHECK_EQ(render(Full), Uncancelled);
  CHECK_EQ(Asked, 2);
  auto CleanFull = turf::analyze({{"clean.tr", CleanProgram}}, Never);
  CHECK(!CleanFull.cancelled());
  CHECK(!CleanFull.program().empty());
  CHECK(CleanFull.typeAt("clean.tr", 2, 12) != nullptr);

  // Cancelled after each boundary in turn (the first asks, the second asks).
  for (int FireAt : {1, 2}) {
    InitializeCompilerGlobals();
    SymbolTable *HostTable = GlobalSymbolTable.get();
    SourceFilename = "host.tr";
    MainSourceFilename = "host.tr";
    SourceLines = {"host line 1"};
    CurTok = TOK_IDENTIFIER;
    IdentifierStr = "hostIdent";
    CurLine = 41;
    g_braceDepth = 2;
    DiagnosticEngine::clearDiagnostics();
    DiagnosticEngine::setCollectAllDiagnostics(false);
    DiagnosticEngine::add({3, 4, "host.tr"}, "host diagnostic");

    int Calls = 0;
    turf::AnalysisOptions Opts;
    Opts.ShouldCancel = [&] { return ++Calls == FireAt; };
    auto R = turf::analyze({{"clean.tr", CleanProgram}}, Opts);

    CHECK(R.cancelled());
    CHECK_EQ(Calls, FireAt);
    // Empty, not partial.
    CHECK(R.diagnostics().empty());
    CHECK(R.program().empty());
    CHECK_EQ(R.preludeImportCount(), 0u);
    CHECK(R.typeAt("clean.tr", 2, 12) == nullptr);
    CHECK(!R.definitionOf("clean.tr", 2, 12).has_value());
    CHECK(R.symbolsInScope("clean.tr", 2, 12).empty());
    // And the host is exactly where it left off.
    CHECK(GlobalSymbolTable.get() == HostTable);
    CHECK_EQ(SourceFilename, std::string("host.tr"));
    CHECK_EQ(SourceLines.size(), 1u);
    CHECK_EQ(CurTok, static_cast<int>(TOK_IDENTIFIER));
    CHECK_EQ(IdentifierStr, std::string("hostIdent"));
    CHECK_EQ(CurLine, 41);
    CHECK_EQ(g_braceDepth, 2);
    CHECK(!DiagnosticEngine::getCollectAllDiagnostics());
    CHECK_EQ(DiagnosticEngine::getDiagnosticCount(), 1u);
    CHECK(DiagnosticEngine::hasErrorAt(3));
  }
  DiagnosticEngine::clearDiagnostics();
  AllSourceLines.clear();
  SourceLines.clear();
  SourceFilename.clear();
  MainSourceFilename.clear();
  g_braceDepth = 0;

  // The next analysis is not affected by a cancelled one.
  CHECK_EQ(render(turf::analyze({{"c.tr", UndefinedVariable}})), Uncancelled);
}

// A toolchain whose standard library cannot be found (an editor extension
// installed without its lib/std, a TURF_HOME pointing nowhere) must produce a
// diagnostic that says so, not end the process: the facade's promise is that
// a problem with the analysed source never does, and an editor's language
// server that dies on the first keystroke gives the user nothing to act on.
// The prelude imports are what fail first (they load lib/std/core).
static void testMissingStdlibIsADiagnostic(const ScratchDir &Dir) {
  CurrentTest = "missing stdlib";
  const std::string EmptyRoot = Dir.Root.string() + "/no-stdlib";
  fs::create_directories(EmptyRoot);

  turf::AnalysisOptions NoStdlib;
  NoStdlib.InstallRoot = EmptyRoot;
  auto R = turf::analyze({{"nostd.tr", "import io;\n\nfn main(): int {\n    return 0;\n}\n"}},
                         NoStdlib);
  CHECK(!R.succeeded());
  CHECK(hasDiagnostic(R, "Could not find module"));
  CHECK_EQ(R.preludeImportCount(), 0u);

  // The failure left nothing behind: the next analysis, with the real
  // standard library, is exactly what it would have been.
  const std::string Clean = render(turf::analyze({{"clean.tr", CleanProgram}}));
  CHECK_EQ(Clean, std::string());
  auto Again = turf::analyze({{"clean.tr", CleanProgram}});
  CHECK(Again.preludeImportCount() > 0);
}

// AnalysisOptions::PackageGraphFile: a project's imports resolve as `turf
// build` resolves them. src/lib/util.tr is not next to src/main.tr, so only
// the graph (`import util;` means src/**/util.tr) finds it; `dep` is a
// dependency package named in the graph. A module created after an analysis
// already ran must be found by the next one (the per-package source index is
// dropped between analyses), and a graph that cannot be used becomes the
// first diagnostic instead of ending anything.
static void testPackageGraph(const ScratchDir &Dir) {
  CurrentTest = "package graph";
  const fs::path Proj = Dir.Root / "pkg" / "proj";
  const fs::path Dep = Dir.Root / "pkg" / "dep";
  fs::create_directories(Proj / "src" / "lib");
  fs::create_directories(Dep / "src");
  const std::string MainText = "import util;\nimport dep;\n\n"
                               "fn main(): int {\n    return util::double_it(dep::answer());\n}\n";
  const std::string MainPath = (Proj / "src" / "main.tr").string();
  std::ofstream(MainPath) << MainText;
  std::ofstream(Proj / "src" / "lib" / "util.tr")
      << "pub fn double_it(x: int): int {\n    return x * 2;\n}\n";
  std::ofstream(Dep / "src" / "dep.tr") << "pub fn answer(): int {\n    return 21;\n}\n";
  const std::string GraphPath = (Dir.Root / "pkg" / "package-graph.json").string();
  std::ofstream(GraphPath) << "{\"format\": 1, \"root\": \"proj\", \"packages\": {"
                              "\"proj\": {\"src\": \"" << (Proj / "src").string() << "\", \"deps\": [\"dep\"]}, "
                              "\"dep\": {\"src\": \"" << (Dep / "src").string() << "\"}}}";

  // Without the graph the nested module and the dependency are invisible.
  CHECK(!turf::analyze({{MainPath, MainText}}).succeeded());

  turf::AnalysisOptions Opts;
  Opts.PackageGraphFile = GraphPath;
  auto R = turf::analyze({{MainPath, MainText}}, Opts);
  CHECK_EQ(render(R), std::string());
  // `import <cursor>` completion lists what an import here could name: the
  // package's own modules wherever they are under src/, its dependency, the
  // standard library -- and not the file itself. Without the graph `util` (in
  // a subdirectory) and `dep` (another package) are not on offer.
  auto Has = [](const std::vector<std::string> &L, const char *N) {
    return std::find(L.begin(), L.end(), N) != L.end();
  };
  CHECK(Has(R.importableModules(), "util"));
  CHECK(Has(R.importableModules(), "dep"));
  CHECK(Has(R.importableModules(), "io"));
  CHECK(!Has(R.importableModules(), "main"));
  auto Alone = turf::analyze({{MainPath, MainText}});
  CHECK(!Has(Alone.importableModules(), "util"));
  CHECK(!Has(Alone.importableModules(), "dep"));
  CHECK(Has(Alone.importableModules(), "io"));

  // The file behind a module name, outside any analysis, under the same
  // options: the project's modules and dependency resolve, single-file mode
  // does not know them.
  const std::string UtilFile = turf::resolveModuleFile("util", MainPath, Opts);
  CHECK(UtilFile.size() > 7 && UtilFile.compare(UtilFile.size() - 7, 7, "util.tr") == 0);
  const std::string DepFile = turf::resolveModuleFile("dep", MainPath, Opts);
  CHECK(DepFile.size() > 6 && DepFile.compare(DepFile.size() - 6, 6, "dep.tr") == 0);
  CHECK(turf::resolveModuleFile("util", MainPath).empty());
  CHECK(!turf::resolveModuleFile("io", MainPath).empty());

  auto Def = R.definitionOf(MainPath, 5, 20); // inside `double_it`
  CHECK(Def.has_value());
  if (Def)
    CHECK(Def->Path.find("util.tr") != std::string::npos);

  // A module another file of the package gains later is importable by the
  // next analysis. The first analysis above already indexed src/.
  std::ofstream(Proj / "src" / "late.tr") << "pub fn late(): int {\n    return 7;\n}\n";
  auto Late = turf::analyze({{MainPath, "import late;\n\nfn main(): int {\n    return late::late();\n}\n"}}, Opts);
  CHECK_EQ(render(Late), std::string());

  // Any file of the package can be the entry, not only src/main.tr.
  const std::string UtilPath = (Proj / "src" / "lib" / "util.tr").string();
  auto AsModule = turf::analyze({{UtilPath, "pub fn double_it(x: int): int {\n    return x * 2;\n}\n"}}, Opts);
  CHECK_EQ(render(AsModule), std::string());

  // A graph the file is not part of, or that is not there: the first
  // diagnostic says so, and the file is still analysed on its own.
  const std::string Outside = Dir.write("outside.tr", CleanProgram);
  auto NotIn = turf::analyze({{Outside, CleanProgram}}, Opts);
  CHECK(!NotIn.diagnostics().empty());
  CHECK(NotIn.diagnostics().front().Message.find("package graph") != std::string::npos);
  CHECK(NotIn.diagnostics().front().Code.empty());
  turf::AnalysisOptions Missing;
  Missing.PackageGraphFile = (Dir.Root / "pkg" / "nope.json").string();
  auto NoGraph = turf::analyze({{MainPath, MainText}}, Missing);
  CHECK(!NoGraph.diagnostics().empty());
  CHECK(NoGraph.diagnostics().front().Message.find("cannot read it") != std::string::npos);

  // And none of it outlives the call: with no graph again, a plain analysis
  // behaves as before.
  CHECK_EQ(render(turf::analyze({{"clean.tr", CleanProgram}})), std::string());
}

// Buffers after the first replace the disk text of the file an import
// loads; diagnostics and definitions inside it name that file.
static void testImportedModuleOverlay(const ScratchDir &Dir) {
  CurrentTest = "import overlay";
  const std::string Main = Dir.write(
      "main.tr", "import helper;\n\nfn main(): int {\n"
                 "    return helper::value();\n}\n");
  const std::string Helper =
      Dir.write("helper.tr", "pub fn value(): int {\n    return 1;\n}\n");
  const std::string MainText =
      "import helper;\n\nfn main(): int {\n    return helper::value();\n}\n";

  auto OnDisk = turf::analyze({{Main, MainText}});
  CHECK_EQ(render(OnDisk), std::string());
  auto Def = OnDisk.definitionOf(Main, 4, 20);
  CHECK(Def.has_value());
  if (Def) {
    CHECK(fs::equivalent(Def->Path, Helper));
    CHECK_EQ(Def->Line, 1u);
  }

  // The unsaved edit changes value()'s return type: main no longer checks.
  auto Edited = turf::analyze(
      {{Main, MainText},
       {Helper, "pub fn value(): string {\n    return \"one\";\n}\n"}});
  CHECK(!Edited.succeeded());
  // ...and it is a TYPE error, not a visibility one: value() is still `pub`,
  // so a failure that is really "helper::value is private" cannot pass here.
  for (const auto &D : Edited.diagnostics())
    CHECK(D.Code != "E088");

  // An error inside the edited module is reported against that module.
  auto Broken = turf::analyze(
      {{Main, MainText},
       {Helper, "pub fn value(): int {\n    return nothing;\n}\n"}});
  bool InHelper = false;
  for (const auto &D : Broken.diagnostics())
    if (D.Level == turf::Severity::Error && fs::equivalent(D.Where.Path, Helper) &&
        D.Where.Line == 2)
      InHelper = true;
  CHECK(InHelper);

  // And the disk copy was never touched.
  CHECK_EQ(render(turf::analyze({{Main, MainText}})), std::string());
}

// The same dedupe was keyed by line and column alone, so within ONE compile
// a fallible function in an imported module hid the warning for the entry
// file's function at the same line:col.
static void testWarningDedupeIsPerFile(const ScratchDir &Dir) {
  CurrentTest = "warning dedupe is per file";
  const std::string Lib =
      Dir.write("fallible.tr", "fn g(x: int): !int {\n    return x;\n}\n");
  const std::string Main = Dir.write("usesfallible.tr", "");
  auto R = turf::analyze(
      {{Main, "import fallible;\n\nfn f(x: int): !int {\n    return x;\n}\n"
              "\nfn main(): int {\n    return 0;\n}\n"},
       {Lib, "\n\nfn g(x: int): !int {\n    return x;\n}\n"}});
  int Warnings = 0;
  for (const auto &D : R.diagnostics())
    if (D.Message.find("never actually returns an error") != std::string::npos)
      ++Warnings;
  CHECK_EQ(Warnings, 2);
}

static void testLintIsOptIn() {
  CurrentTest = "lint is opt-in";
  const char *SelfCompare = "fn main(): int {\n    mut x: int = 1;\n"
                            "    if x == x {\n        return 1;\n    }\n"
                            "    return 0;\n}\n";
  auto Plain = turf::analyze({{"lint.tr", SelfCompare}});
  turf::AnalysisOptions Opts;
  Opts.RunLint = true;
  auto Linted = turf::analyze({{"lint.tr", SelfCompare}}, Opts);
  CHECK(Linted.diagnostics().size() > Plain.diagnostics().size());
}

// The flow checks a real compile runs on each function's CFG during Codegen
// (runFunctionFlowPasses()); analyze() reports them only when asked (the LSP
// asks), with the same code, line and related notes the CLI prints.
// F-MEM-5 per-field tracking (2026-10-12) needs struct layouts, which the
// facade has from parsing (ParseStructDecl() registers them), not from
// codegen. Each of these is E086 in a compile; the editor must see it too.
static void testEscapeThroughStructFieldsInFacade() {
  CurrentTest = "escape through struct fields (facade)";
  const std::string Prelude = "struct N { val: int, next: &?N }\n"
                              "struct H { k: int, head: &?N }\n"
                              "struct H2 { k: int, own: &N }\n"
                              "fn id(p: &N): &N {\n    return p;\n}\n";
  const char *Escaping[] = {
      "fn f(): H {\n    mut n: N = N::default();\n    mut h: H = H::default();\n"
      "    h.head = &n;\n    mut g: H = h;\n    return g;\n}\n",
      "fn f(): &N {\n    mut n: N = N::default();\n    mut m: N = N::default();\n"
      "    m.next = &n;\n    return heap(m);\n}\n",
      "fn f(): &N {\n    mut n: N = N::default();\n    return id(&n);\n}\n",
      "fn f(): &N {\n    mut a: N[2] = [N::default(), N::default()];\n    return &a[0];\n}\n",
      "fn f(h: &H2): void {\n    mut n: N = N::default();\n    h.own.next = &n;\n}\n",
      "fn f(h: H2): void {\n    mut n: N = N::default();\n    h.own.next = &n;\n}\n",
  };
  turf::AnalysisOptions Opts;
  Opts.RunFlowAnalysis = true;
  for (const char *Fn : Escaping) {
    std::string Source = Prelude + Fn + "fn main(): int {\n    return 0;\n}\n";
    auto R = turf::analyze({{"esc.tr", Source}}, Opts);
    int N = 0;
    for (const auto &D : R.diagnostics())
      N += D.Code == "E086";
    CHECK_EQ(N, 1);
  }
  // Kept local, so allowed: the struct and the node it points at share a frame.
  std::string Local = Prelude +
                      "fn f(): int {\n    mut n: N = N::default();\n    n.val = 3;\n"
                      "    mut h: H = H::default();\n    h.head = &n;\n"
                      "    mut q: &?N = h.head;\n    if (q != null) {\n        return q.val;\n"
                      "    }\n    return 0;\n}\nfn main(): int {\n    return f();\n}\n";
  auto L = turf::analyze({{"local.tr", Local}}, Opts);
  for (const auto &D : L.diagnostics())
    CHECK(D.Code != "E086");
}

static void testFlowAnalysisIsOptIn() {
  CurrentTest = "flow analysis is opt-in";
  // One error per function, as in a compile (an error ends that function's
  // analysis); the facade then goes on to the next function.
  const char *Source = "import mem;\n"                          // 1
                       "\n"                                     // 2
                       "fn twice(): int {\n"                    // 3
                       "    unsafe {\n"                         // 4
                       "        mut p: void* = mem::alloc(8);\n" // 5
                       "        mem::free(p);\n"                // 6
                       "        p.free();\n"                    // 7
                       "    }\n"                                // 8
                       "    return 0;\n"                        // 9
                       "}\n"                                    // 10
                       "\n"                                     // 11
                       "fn late(): int {\n"                     // 12
                       "    unsafe {\n"                         // 13
                       "        mut q: int* = mem::alloc(8);\n" // 14
                       "        mem::free(q);\n"                // 15
                       "        q[0] = 1;\n"                    // 16
                       "    }\n"                                // 17
                       "    return 0;\n"                        // 18
                       "}\n"                                    // 19
                       "\n"                                     // 20
                       "fn main(): int {\n"                     // 21
                       "    return twice() + late();\n"         // 22
                       "}\n";
  auto count = [](const turf::AnalysisResult &R, const std::string &Code) {
    int N = 0;
    for (const auto &D : R.diagnostics())
      N += D.Code == Code;
    return N;
  };
  auto Plain = turf::analyze({{"mem.tr", Source}});
  CHECK_EQ(count(Plain, "E017"), 0);
  CHECK_EQ(count(Plain, "E018"), 0);

  turf::AnalysisOptions Opts;
  Opts.RunFlowAnalysis = true;
  auto R = turf::analyze({{"mem.tr", Source}}, Opts);
  CHECK_EQ(count(R, "E017"), 1);
  CHECK_EQ(count(R, "E018"), 1);
  for (const auto &D : R.diagnostics()) {
    if (D.Code == "E017") {
      CHECK_EQ(D.Where.Line, 7); // the UFCS release, p.free()
      CHECK_EQ(D.Related.size(), 1u);
      if (!D.Related.empty())
        CHECK_EQ(D.Related[0].Where.Line, 6);
    }
    if (D.Code == "E018")
      CHECK_EQ(D.Where.Line, 16);
  }

  // Released on some paths only: the warning, not an error.
  const char *Maybe = "import mem;\n\nfn main(): int {\n    mut c: int = 1;\n"
                      "    unsafe {\n        mut p: void* = mem::alloc(8);\n"
                      "        if c == 1 {\n            mem::free(p);\n        }\n"
                      "        mem::free(p);\n    }\n    return 0;\n}\n";
  auto M = turf::analyze({{"maybe.tr", Maybe}}, Opts);
  CHECK_EQ(count(M, "E017"), 0);
  int Warned = 0;
  for (const auto &D : M.diagnostics())
    if (D.Level == turf::Severity::Warning &&
        D.Message.find("may already have been freed") != std::string::npos)
      ++Warned;
  CHECK_EQ(Warned, 1);

  // The other passes, one function each: missing return (E009), use before
  // assignment (E008), recursion with no base case (E010), an unused
  // variable (a warning with no code).
  const char *Flow = "fn noret(c: bool): int {\n"   // 1
                     "    if c {\n"                 // 2
                     "        return 1;\n"          // 3
                     "    }\n"                      // 4
                     "}\n"                          // 5
                     "\n"                           // 6
                     "fn unset(c: bool): int {\n"   // 7
                     "    mut x: int;\n"            // 8
                     "    if c {\n"                 // 9
                     "        x = 1;\n"             // 10
                     "    }\n"                      // 11
                     "    return x;\n"              // 12
                     "}\n"                          // 13
                     "\n"                           // 14
                     "fn forever(n: int): int {\n"  // 15
                     "    return forever(n - 1);\n" // 16
                     "}\n"                          // 17
                     "\n"                           // 18
                     "fn idle(): int {\n"           // 19
                     "    mut y: int = 1;\n"        // 20
                     "    return 0;\n"              // 21
                     "}\n"                          // 22
                     "\n"                           // 23
                     "fn main(): int {\n"           // 24
                     "    return noret(true) + unset(true) + forever(1) + idle();\n"
                     "}\n";
  auto unusedWarnings = [](const turf::AnalysisResult &Res) {
    int N = 0;
    for (const auto &D : Res.diagnostics())
      N += D.Level == turf::Severity::Warning &&
           D.Message.find("'y' is declared but never used") != std::string::npos;
    return N;
  };
  auto FlowPlain = turf::analyze({{"flow.tr", Flow}});
  CHECK_EQ(count(FlowPlain, "E009"), 0);
  CHECK_EQ(count(FlowPlain, "E008"), 0);
  CHECK_EQ(count(FlowPlain, "E010"), 0);
  CHECK_EQ(unusedWarnings(FlowPlain), 0);
  auto F = turf::analyze({{"flow.tr", Flow}}, Opts);
  CHECK_EQ(count(F, "E009"), 1);
  CHECK_EQ(count(F, "E008"), 1);
  CHECK_EQ(count(F, "E010"), 1);
  CHECK_EQ(unusedWarnings(F), 1);
  for (const auto &D : F.diagnostics()) {
    if (D.Code == "E009")
      CHECK_EQ(D.Where.Line, 1);
    if (D.Code == "E008")
      CHECK_EQ(D.Where.Line, 12);
    if (D.Code == "E010")
      CHECK_EQ(D.Where.Line, 15);
  }

  // Not checked, as in a compile: a function that already has an error (its
  // flow would be judged on a half-checked body), and a generic function.
  const char *Skipped = "fn broken(c: bool): int {\n"   // 1
                        "    mut s: int = \"text\";\n"  // 2
                        "    if c {\n"                  // 3
                        "        return s;\n"           // 4
                        "    }\n"                       // 5
                        "}\n"                           // 6
                        "\n"                            // 7
                        "fn<T> pick(c: bool, a: T): T {\n"
                        "    if c {\n"
                        "        return a;\n"
                        "    }\n"
                        "}\n"
                        "\n"
                        "fn main(): int {\n"
                        "    return 0;\n"
                        "}\n";
  auto S = turf::analyze({{"skipped.tr", Skipped}}, Opts);
  CHECK(count(S, "E009") == 0);
  CHECK(!S.succeeded()); // the type error itself is still reported
}

// The unused-variable warnings used to come out in pointer-hash order, so one
// buffer analyzed twice in the same process could list them differently
// (found by turf-fuzz's re-entrancy check; the CLI hid it by sorting at print
// time). Source order is checked directly, on every run, so the test cannot
// pass by an allocator's luck.
static void testFlowWarningsAreInSourceOrder() {
  CurrentTest = "flow warnings are in source order";
  std::string Source = "fn main(): int {\n";
  for (char C = 'a'; C <= 'l'; ++C)
    Source += std::string("    mut ") + C + ": int = 1;\n";
  Source += "    return 0;\n}\n";
  turf::AnalysisOptions Opts;
  Opts.RunFlowAnalysis = true;
  std::string First;
  for (int Run = 0; Run < 8; ++Run) {
    auto R = turf::analyze({{"order.tr", Source}}, Opts);
    unsigned Prev = 0, Count = 0;
    for (const auto &D : R.diagnostics()) {
      if (D.Message.find("declared but never used") == std::string::npos)
        continue;
      ++Count;
      CHECK(D.Where.Line > Prev);
      Prev = D.Where.Line;
    }
    CHECK_EQ(Count, 12u);
    if (Run == 0)
      First = render(R);
    else
      CHECK_EQ(render(R), First);
  }
}

static void testSkipPrelude() {
  CurrentTest = "SkipPrelude";
  turf::AnalysisOptions Opts;
  Opts.SkipPrelude = true;
  auto Explicit = turf::analyze(
      {{"explicit.tr", "import io;\n"
                       "fn main(): int {\n"
                       "    io::printline(1);\n"
                       "    return 0;\n"
                       "}\n"}},
      Opts);
  CHECK(Explicit.succeeded());
  CHECK_EQ(Explicit.preludeImportCount(), 0u);

  const char *Bare = "fn main(): int {\n"
                     "    printline(1);\n"
                     "    return 0;\n"
                     "}\n";
  CHECK(turf::analyze({{"bare.tr", Bare}}).succeeded());
  CHECK(!turf::analyze({{"bare.tr", Bare}}, Opts).succeeded());
  // And the next ordinary analysis has its prelude back.
  CHECK(turf::analyze({{"bare.tr", Bare}}).succeeded());
}

// A lexer error in the very first token was raised before any recovery guard
// was armed, so analyze() called exit() and took its host (turf-lsp) down.
// Reaching the checks below at all is most of the test.
static void testLexErrorInFirstToken() {
  CurrentTest = "lex error in the first token";
  for (const char *Source : {"0E\n", "0E\nfn main(): int {\n    return 0;\n}\n"}) {
    auto R = turf::analyze({{"lex.tr", Source}});
    CHECK(!R.succeeded());
    CHECK(!R.diagnostics().empty());
  }
  // The next analysis is unaffected.
  CHECK(turf::analyze({{"ok.tr", "fn main(): int {\n    return 0;\n}\n"}})
            .succeeded());
}

static void testEmptyInput() {
  CurrentTest = "empty input";
  auto R = turf::analyze({});
  CHECK(R.diagnostics().empty());
  CHECK(R.succeeded());
  CHECK(R.typeAt("x.tr", 1, 1) == nullptr);
  CHECK(R.symbolsInScope("x.tr", 1, 1).empty());
}

// LEC-002: `pub`/`public` in a place where visibility means nothing must be
// rejected with E087 AND a hint that names the fix. `test_runner.sh` only
// checks that a `fail_*.tr` fails to compile, not why -- so a case that
// started failing for an unrelated reason (a typo in the test, a later
// unrelated diagnostic) would keep passing there. Asserting the code and a
// fragment of the hint here is what keeps each shape failing for the right
// reason and keeps the hints from silently degrading into generic text.
static void testVisibilityModifierPlacement() {
  CurrentTest = "visibility modifier placement";
  struct Case {
    const char *Label;
    const char *Source;
    const char *HintFragment; // must appear in the hint
  };
  const std::string Main = "\nfn main(): int { return 0; }\n";
  const std::vector<std::pair<std::string, Case>> Rejected = {
      {"pub export", {"", "pub export immut X: int = 1;", "Just remove `pub`"}},
      {"pub operator", {"", "pub operator +(a: string, b: int): string { return a; }", "make the type itself `pub`"}},
      {"pub import", {"", "pub import io;", "never re-exported"}},
      {"pub immut", {"", "pub immut X: int = 1;", "export immut NAME"}},
      {"enum variant", {"", "enum E { pub A, B }", "pub enum Name"}},
      {"error variant", {"", "error E { pub A }", "pub error Name"}},
      {"machine state", {"", "machine M { pub A -> B }", "pub machine Name"}},
      {"parameter", {"", "fn f(pub x: int): int { return x; }", "before `fn`"}},
      {"pub unsafe", {"", "pub unsafe { }", "declarations inside the block"}},
      {"before decorator", {"", "pub @caller_location\nfn f(): string { return caller_location; }", "annotations first"}},
      {"not a declaration", {"", "pub 5;", "right before `fn`"}},
      {"public spelling", {"", "public export immut X: int = 1;", "Just remove `pub`"}},
      {"reserved as variable", {"", "fn g(): int { mut pub: int = 1; return pub; }", "is_public"}},
      {"reserved as function", {"", "fn public(): int { return 1; }", "is_public"}},
      {"reserved as type", {"", "struct pub { x: int }", "is_public"}},
      {"local struct", {"", "fn g(): int { pub struct S { x: int } return 0; }", "top level"}},
      {"local enum", {"", "fn g(): int { public enum E { A } return 0; }", "top level"}},
      {"local struct field", {"", "fn g(): int { struct S { pub x: int } return 0; }", "top level"}},
  };
  for (const auto &[Label, C] : Rejected) {
    auto R = turf::analyze({{"vis.tr", std::string(C.Source) + Main}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != "E087")
        continue;
      Found = true;
      CHECK(D.Hint.find(C.HintFragment) != std::string::npos);
      if (D.Hint.find(C.HintFragment) == std::string::npos)
        std::cerr << "    case '" << Label << "': hint was: " << D.Hint << "\n";
      CHECK(D.Message.find('\x1b') == std::string::npos);
      break;
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << Label << "' produced:\n" << render(R);
  }

  // Accepted: every declaration form, both spellings, field-level `pub`,
  // `pub` after a decorator, and `pub` on the main file's own declarations
  // (a single-file program never needs it, and it must not be an error).
  const char *Accepted = R"(pub struct Box { pub value: int, public other: int, cache: int }
public enum Color { Red, Green }
pub error Oops { A, B }
pub machine M { A -> B }
pub extern fn abs(x: int32): int32;
@caller_location
pub fn here(): string { return caller_location; }
pub fn Box.get(): int { return self.value; }
fn helper(): int { return 1; }
public fn main(): int { return helper(); }
)";
  auto R = turf::analyze({{"ok.tr", Accepted}});
  CHECK(R.succeeded());
  if (!R.succeeded())
    std::cerr << render(R);
}

// LEC-008 (FREEZE.md 6.24): `m::` is spellable only in a file that imports `m`
// itself. The runner cannot see WHY a fail_*.tr failed, so the code, the module
// the message names and the hint (which prints the fix, and says why a symbol
// import is not enough) are asserted here, along with what must keep working.
static void testModuleNotImported(const ScratchDir &Dir) {
  CurrentTest = "module not imported";
  Dir.write("nideep.tr", R"(pub struct Deep { pub v: int }
pub fn deep(): int { return 7; }
pub fn make(): Deep { return Deep::default(); }
pub fn twice(d: Deep): int { return d.v * 2; }
export immut DEEP_LIMIT: int = 9;
)");
  Dir.write("nimid.tr", R"(import nideep;
pub fn mid(): int { return nideep::deep(); }
pub fn made(): nideep::Deep { return nideep::make(); }
)");
  const std::string Main = Dir.write("niuser.tr", "");
  struct Case {
    const char *Label;
    const char *Src;
    const char *HintFragment;
    bool SymbolImport;
  };
  const Case Cases[] = {
      {"transitive call", "import nimid;\nfn main(): int { return nideep::deep(); }",
       "Add `import nideep;`", false},
      {"transitive constant",
       "import nimid;\nfn main(): int { return nideep::DEEP_LIMIT; }",
       "Add `import nideep;`", false},
      {"transitive type",
       "import nimid;\nfn main(): int { mut d: nideep::Deep = nimid::made(); return d.v; }",
       "Add `import nideep;`", false},
      {"static construction",
       "import nimid;\nfn main(): int { mut d = nideep::Deep::default(); return d.v; }",
       "Add `import nideep;`", false},
      {"symbol import is not the qualifier",
       "import nideep::deep;\nfn main(): int { return nideep::make().v + deep(); }",
       "brings in only that name", true},
  };
  for (const auto &C : Cases) {
    auto R = turf::analyze({{Main, C.Src}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != "E145")
        continue;
      Found = true;
      CHECK(D.Message.find("Module 'nideep' is not imported in this file") != std::string::npos);
      CHECK(D.Hint.find(C.HintFragment) != std::string::npos);
      if (D.Hint.find(C.HintFragment) == std::string::npos)
        std::cerr << "    case '" << C.Label << "': hint was: " << D.Hint << "\n";
      // The "symbol import" sentence appears exactly when the file has one.
      CHECK((D.Hint.find("brings in only that name") != std::string::npos) == C.SymbolImport);
      break;
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }

  // A module is held to the rule too, and the message names it.
  Dir.write("nileaky.tr", "import nimid;\npub fn leaky(): int { return nideep::deep(); }\n");
  {
    auto R = turf::analyze({{Main, "import nileaky;\nfn main(): int { return nileaky::leaky(); }\n"}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics())
      if (D.Code == "E145" && D.Message.find("in module 'nileaky'") != std::string::npos)
        Found = true;
    CHECK(Found);
  }

  // What must NOT be rejected: the file's own imports in every form, and a
  // value of a type from a module it did not import -- fields, methods, UFCS.
  const char *Accepted = R"(import nimid;
import nideep;
import nideep::Deep;
fn main(): int {
  mut a: Deep = nideep::make();
  mut b: nideep::Deep = nimid::made();
  return a.v + b.twice() + nideep::deep() + nideep::DEEP_LIMIT;
}
)";
  auto R = turf::analyze({{Main, Accepted}});
  CHECK(R.succeeded());
  if (!R.succeeded())
    std::cerr << render(R);
  const char *HeldOnly = R"(import nimid;
fn main(): int {
  mut d = nimid::made();
  return d.v + d.twice();
}
)";
  auto H = turf::analyze({{Main, HeldOnly}});
  CHECK(H.succeeded());
  if (!H.succeeded())
    std::cerr << render(H);

  // The prelude (FREEZE.md 6.25), as a tool sees it: every prelude module is
  // "imported" by the entry file with no import written, `list` is loaded (the
  // prelude's own `strings` imports it) but is NOT this file's to spell, and the
  // bare-callable ones are the three named. A language server that offers
  // `printline` unqualified, or an auto-import for `list::`, reads exactly this.
  auto P = turf::analyze({{Main, "fn main(): int { return 0; }\n"}});
  CHECK(P.succeeded());
  for (const char *M : {"int", "double", "bool", "char", "strings", "array", "io", "math", "sys"})
    CHECK(P.symbols().isModuleImportedBy(M, ""));
  CHECK(P.symbols().hasModule("list"));
  CHECK(!P.symbols().isModuleImportedBy("list", ""));
  const auto &Bare = turf::preludeBareModules();
  CHECK(Bare.size() == 3);
  for (const char *M : {"io", "math", "sys"})
    CHECK(std::find(Bare.begin(), Bare.end(), M) != Bare.end());
}

// LEC-002 stage 3: every way of reaching another module's private function or
// type is E088, and the diagnostic carries what a reader needs to act on it --
// a note at the declaration and a hint naming the file, the keyword and the
// module's own public alternatives. As with the placement test above, the
// runner cannot see WHY a fail_*.tr failed, so the code and the hint are
// asserted here.
static void testPrivateSymbolAccess(const ScratchDir &Dir) {
  CurrentTest = "private symbol access";
  Dir.write("vislib.tr", R"(pub fn api(): int { return helper(); }
fn helper(): int { return 1; }
pub fn pick(x: int): int { return x; }
fn pick(x: string): int { return 0; }
pub struct Shown { pub v: int, secret: int }
pub fn Shown.get(): int { return self.secret; }
fn Shown.hidden(): int { return 1; }
pub fn Shown.make() static: Shown { return Shown::default(); }
fn Shown.make_private() static: Shown { return Shown::default(); }
fn secret_twice(x: Shown): int { return 2; }
pub struct Guarded { pub v: int }
fn Guarded.new(v: int): void { self.v = v; }
struct Hidden { v: int }
enum HiddenColor { A }
extern fn strlen(s: string): int;
)");
  const std::string Main = Dir.write("visuser.tr", "");
  struct Case {
    const char *Label;
    const char *Body;
    const char *HintFragment;
  };
  const Case Cases[] = {
      {"qualified call", "fn main(): int { return vislib::helper(); }",
       "`pub fn helper(...)`"},
      {"symbol import", "import vislib::helper;\nfn main(): int { return 0; }",
       "in vislib.tr"},
      {"list import", "import vislib::[api, helper];\nfn main(): int { return 0; }",
       "vislib' offers these public functions: api, pick"},
      {"type annotation",
       "fn main(): int { mut h: vislib::Hidden = vislib::Hidden::default(); return 0; }",
       "`pub struct Hidden`"},
      {"type import", "import vislib::Hidden;\nfn main(): int { return 0; }",
       "public types: Guarded, Shown"},
      {"enum variant",
       "fn main(): int { immut x: int = vislib::HiddenColor::A to int; return x; }",
       "`pub enum HiddenColor`"},
      {"extern fn", "fn main(): int { return vislib::strlen(\"a\"); }",
       "`pub extern fn strlen(...)`"},
      // Stage 4: members. A private FIELD's hint names the public METHODS
      // (the way to get at the data), a private method's names its siblings.
      {"field read",
       "fn main(): int { mut s: vislib::Shown = vislib::Shown::default(); return s.secret; }",
       "`pub secret: ...` inside `struct Shown`. Otherwise, 'Shown' offers these public methods: get, make"},
      {"field write",
       "fn main(): int { mut s: vislib::Shown = vislib::Shown::default(); s.secret = 1; return 0; }",
       "`pub secret: ...`"},
      {"instance method",
       "fn main(): int { mut s: vislib::Shown = vislib::Shown::default(); return s.hidden(); }",
       "`pub fn Shown.hidden(...)`"},
      {"static method",
       "fn main(): int { mut s: vislib::Shown = vislib::Shown::make_private(); return s.v; }",
       "`pub fn Shown.make_private(...)`"},
      {"constructor",
       "fn main(): int { mut g: vislib::Guarded = vislib::Guarded::new(1); return g.v; }",
       "`pub fn Guarded.new(...)`"},
      {"UFCS candidate",
       "fn main(): int { mut s: vislib::Shown = vislib::Shown::default(); return s.secret_twice(); }",
       "`pub fn secret_twice(...)`"},
  };
  for (const auto &C : Cases) {
    const std::string Src = std::string("import vislib;\n") + C.Body + "\n";
    auto R = turf::analyze({{Main, Src}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != "E088")
        continue;
      Found = true;
      CHECK(D.Hint.find(C.HintFragment) != std::string::npos);
      if (D.Hint.find(C.HintFragment) == std::string::npos)
        std::cerr << "    case '" << C.Label << "': hint was: " << D.Hint << "\n";
      CHECK(!D.Related.empty()); // the note at the declaration
      CHECK(D.Message.find("private to module 'vislib'") != std::string::npos);
      // Never a raw `.`-joined internal key in a user-facing message: it
      // would read "Function 'Shown.hidden'" instead of naming what it is.
      CHECK(D.Message.find("Function 'Shown") == std::string::npos);
      break;
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }

  // What must NOT be rejected: the pub members of the same module, a
  // wildcard import that simply skips the private names, and this file's own
  // declarations that share a name with the module's private ones.
  const char *Accepted = R"(import vislib::[*];
fn helper(): int { return 100; }
struct Hidden { z: int }
fn main(): int {
    mut s: Shown = Shown::default();
    mut h: Hidden = Hidden::default();
    return api() + pick(1) + helper() + s.v + h.z;
}
)";
  auto R = turf::analyze({{Main, Accepted}});
  CHECK(R.succeeded());
  if (!R.succeeded())
    std::cerr << render(R);
}

// LEC-002 stage 5: private-in-public. A `pub` signature or field that mentions
// a private type of its own module is E089, reported at the public item with a
// note at the private type and a hint offering both fixes. Asserted here (not
// just by fail_*.tr files) because an earlier draft of one of those files
// "failed" for an unrelated reason -- a reserved word in its companion module
// -- and the runner counted it as passing.
static void testPrivateInPublic(const ScratchDir &Dir) {
  CurrentTest = "private in public";
  const std::string Main = Dir.write("pipuser.tr", "");
  struct Case {
    const char *Label;
    const char *Module;
    const char *Item;         // the public item the message must name
    const char *HintFragment; // must appear in the hint
  };
  const Case Cases[] = {
      {"return type",
       "struct Hidden { pub n: int }\npub fn make(): Hidden { return Hidden::default(); }",
       "Public function 'make'", "`pub struct Hidden` in pipmod.tr"},
      {"parameter",
       "enum Mode { A }\npub fn start(m: Mode): int { return 1; }",
       "Public function 'start'", "`pub enum Mode` in pipmod.tr"},
      {"field",
       "struct Inner { pub n: int }\npub struct Outer { pub inner: Inner }",
       "Public field 'Outer.inner'", "keep the field internal by removing its `pub`"},
      {"generic argument",
       "import list::List;\nstruct H { pub n: int }\npub fn all(): List<H> { return List::default(); }",
       "Public function 'all'", "`pub struct H` in pipmod.tr"},
      {"method",
       "struct Token { pub id: int }\npub struct Lexer { pub pos: int }\n"
       "pub fn Lexer.accept(t: Token): int { return t.id; }",
       "Public method 'Lexer.accept'", "keep the method internal"},
      {"error union",
       "struct R { pub n: int }\npub fn try_make(): !R { return R::default(); }",
       "Public function 'try_make'", "`pub struct R` in pipmod.tr"},
  };
  for (const auto &C : Cases) {
    Dir.write("pipmod.tr", C.Module);
    auto R = turf::analyze({{Main, "import pipmod;\nfn main(): int { return 0; }\n"}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != "E089")
        continue;
      Found = true;
      CHECK(D.Message.find(C.Item) != std::string::npos);
      CHECK(D.Hint.find(C.HintFragment) != std::string::npos);
      if (D.Hint.find(C.HintFragment) == std::string::npos)
        std::cerr << "    case '" << C.Label << "': hint was: " << D.Hint << "\n";
      CHECK(!D.Related.empty()); // the note at the private type
      break;
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }

  // Accepted: private types in private signatures and inside pub bodies, a
  // `pub` method on a private type, generic signatures, pub over pub -- and,
  // in the main file, `pub` over a private type (a single-file program has no
  // importer to leak to).
  Dir.write("pipmod.tr", R"(struct Secret { pub n: int }
pub struct Open { pub v: int, hidden: Secret }
fn make(n: int): Secret { mut s: Secret = Secret::default(); s.n = n; return s; }
pub fn compute(n: int): int { mut s: Secret = make(n); return s.n; }
pub fn Secret.doubled(): int { return self.n * 2; }
pub fn<T> identity(x: T): T { return x; }
pub fn sum(a: Open, b: Open): int { return a.v + b.v; }
)");
  auto R = turf::analyze({{Main, R"(import pipmod;
struct Local { n: int }
pub fn from_local(x: Local): int { return x.n; }
fn main(): int { return pipmod::compute(1); }
)"}});
  CHECK(R.succeeded());
  if (!R.succeeded())
    std::cerr << render(R);
}

// LEC-002 stage 6: `turf --migrate=pub`'s engine. It rewrites source text, so
// the properties worth pinning are exact: which tokens get `pub`, which must
// not (a keyword in a comment or string, a function-local struct, a function
// TYPE, an operator, `export immut`, `import`), that everything else passes
// through byte for byte (CRLF included), that it is idempotent, and that the
// result is what makes a 1.x module usable again.
static void testMigratePub(const ScratchDir &Dir) {
  CurrentTest = "migrate pub";

  {
    auto R = turf::migrate::migratePub("fn a(): int { return 1; }\n"
                                       "struct S { x: int, y: int }\n");
    CHECK_EQ(R.Text, std::string("pub fn a(): int { return 1; }\n"
                                 "pub struct S { pub x: int, pub y: int }\n"));
    CHECK_EQ(R.Insertions.size(), 4u);
    if (R.Insertions.size() == 4) {
      CHECK_EQ(R.Insertions[0].Line, 1);
      CHECK_EQ(R.Insertions[0].Col, 1);
      CHECK(R.Insertions[0].What.find("fn a") == 0);
      CHECK(R.Insertions[2].What == "field x");
    }
  }

  // Nothing to touch: a comment naming keywords, an import, an `export
  // immut`, an operator and the `unsafe module;` pragma come back
  // byte-identical.
  {
    const std::string Src =
        "// fn struct enum -- only a comment\n"
        "import io;\n"
        "export immut N: int = 1;\n"
        "operator +(a: int32, b: string): string { return b; }\n"
        "unsafe module;\n";
    auto R = turf::migrate::migratePub(Src);
    CHECK_EQ(R.Text, Src);
    CHECK(R.Insertions.empty());
  }

  // Function-local struct, a function TYPE, and a string that names keywords:
  // only the top-level `fn` is migrated.
  {
    auto R = turf::migrate::migratePub(
        "fn g(f: fn(int): int): int {\n"
        "    struct L { a: int }\n"
        "    mut s: string = \"struct fn\";\n"
        "    return f(1);\n"
        "}\n");
    CHECK_EQ(R.Insertions.size(), 1u);
    CHECK(R.Text.find("pub fn g(f: fn(int): int)") == 0);
    CHECK(R.Text.find("pub struct L") == std::string::npos);
    CHECK(R.Text.find("pub a:") == std::string::npos);
  }

  // `extern from "m" fn` is ONE declaration; a decorator comes first.
  {
    auto R = turf::migrate::migratePub("extern from \"m\" fn sqrt(x: double): double;\n"
                                       "@caller_location\nfn w(): string { return caller_location; }\n");
    CHECK_EQ(R.Text, std::string("pub extern from \"m\" fn sqrt(x: double): double;\n"
                                 "@caller_location\npub fn w(): string { return caller_location; }\n"));
  }

  // A top-level `unsafe { }` block is flattened into the file, so what is
  // inside it is top level.
  {
    auto R = turf::migrate::migratePub("unsafe {\n    struct P { p: int* }\n}\n");
    CHECK_EQ(R.Text, std::string("unsafe {\n    pub struct P { pub p: int* }\n}\n"));
  }

  // Already `pub`/`public` (any spelling): left alone.
  {
    const std::string Src = "pub fn a(): int { return 1; }\npublic struct S { pub x: int, public y: int }\n";
    auto R = turf::migrate::migratePub(Src);
    CHECK_EQ(R.Text, Src);
    CHECK(R.Insertions.empty());
  }

  // CRLF and a missing final newline pass through untouched.
  {
    auto R = turf::migrate::migratePub("fn a(): int {\r\n    return 1;\r\n}");
    CHECK_EQ(R.Text, std::string("pub fn a(): int {\r\n    return 1;\r\n}"));
  }

  // Idempotent.
  {
    const std::string Src = "struct S { x: int }\nfn a(): int { return 1; }\nenum E { A }\n";
    auto Once = turf::migrate::migratePub(Src);
    auto Twice = turf::migrate::migratePub(Once.Text);
    CHECK_EQ(Twice.Text, Once.Text);
    CHECK(Twice.Insertions.empty());
  }

  // The point of it all: a 1.x module is unusable as written (E088) and
  // usable once migrated.
  {
    const std::string Old = "struct Point { x: int, y: int }\n"
                            "fn Point.sum(): int { return self.x + self.y; }\n"
                            "fn make(): Point { mut p: Point = Point::default(); p.x = 1; return p; }\n";
    const std::string Main = Dir.write("migmain.tr", "");
    const std::string Use =
        "import migmod;\nfn main(): int { mut p: migmod::Point = migmod::make(); "
        "return p.sum() + p.x; }\n";
    Dir.write("migmod.tr", Old);
    auto Before = turf::analyze({{Main, Use}});
    CHECK(!Before.succeeded());
    bool SawE088 = false;
    for (const auto &D : Before.diagnostics())
      SawE088 = SawE088 || D.Code == "E088";
    CHECK(SawE088);
    Dir.write("migmod.tr", turf::migrate::migratePub(Old).Text);
    auto After = turf::analyze({{Main, Use}});
    CHECK(After.succeeded());
    if (!After.succeeded())
      std::cerr << render(After);
  }
}

// LEC-001 stage 3 (FREEZE.md 6.16, F-PKG-2): `import m;` that more than one
// search root can answer is E090, naming every file -- never the old "first
// root wins", under which a local list.tr was silently replaced by the
// stdlib's. Asserted by code and hint here because the cwd case cannot be a
// tests/ file at all: the runner always cds into the test's own directory,
// which makes "next to the importing file" and "the cwd" the same place.
static void testAmbiguousModuleImport(const ScratchDir &Dir) {
  CurrentTest = "ambiguous module import";
  fs::create_directories(Dir.Root / "amb" / "app");
  fs::create_directories(Dir.Root / "amb" / "elsewhere");
  const std::string Main = Dir.write("amb/app/main.tr", "");
  auto FindE090 = [](const turf::AnalysisResult &R) -> const turf::Diagnostic * {
    for (const auto &D : R.diagnostics())
      if (D.Code == "E090")
        return &D;
    return nullptr;
  };

  // A local module named like a stdlib module: the user's file must move.
  Dir.write("amb/app/list.tr", "pub fn f(): int { return 1; }\n");
  {
    auto R = turf::analyze({{Main, "import list;\nfn main(): int { return 0; }\n"}});
    const turf::Diagnostic *D = FindE090(R);
    CHECK(D != nullptr);
    if (D) {
      CHECK(D->Message.find("standard library") != std::string::npos);
      CHECK(D->Message.find("next to main.tr") != std::string::npos);
      CHECK(D->Hint.find("may not share its name with a standard-library module") !=
            std::string::npos);
      CHECK(D->Hint.find("my_list.tr") != std::string::npos);
    } else {
      std::cerr << render(R);
    }
  }
  // ...but only when the program imports it. The prelude (`strings` imports
  // `list` internally) resolves stdlib-only, so a local list.tr never breaks
  // a program that does not name it.
  {
    auto R = turf::analyze(
        {{Main, "fn main(): int {\n    mut s: string = \"a\" + \"b\";\n    return 0;\n}\n"}});
    CHECK(R.succeeded());
    if (!R.succeeded())
      std::cerr << render(R);
  }
  fs::remove(Dir.Root / "amb" / "app" / "list.tr");

  // Two user files: next to the importing file, and in the cwd.
  Dir.write("amb/app/util.tr", "pub fn f(): int { return 1; }\n");
  Dir.write("amb/elsewhere/util.tr", "pub fn f(): int { return 2; }\n");
  const std::string UsesUtil = "import util;\nfn main(): int { return util::f(); }\n";
  const fs::path SavedCwd = fs::current_path();
  fs::current_path(Dir.Root / "amb" / "elsewhere");
  {
    auto R = turf::analyze({{Main, UsesUtil}});
    const turf::Diagnostic *D = FindE090(R);
    CHECK(D != nullptr);
    if (D) {
      CHECK(D->Message.find("current directory") != std::string::npos);
      CHECK(D->Hint.find("Rename one of them") != std::string::npos);
    } else {
      std::cerr << render(R);
    }
  }
  // The importing file's directory IS the cwd: one file found twice is one
  // candidate, not an ambiguity. (Every test in tests/ runs this way.)
  fs::current_path(Dir.Root / "amb" / "app");
  {
    auto R = turf::analyze({{Main, UsesUtil}});
    CHECK(FindE090(R) == nullptr);
    CHECK(R.succeeded());
    if (!R.succeeded())
      std::cerr << render(R);
  }
  fs::current_path(SavedCwd);
}

// LEC-001 stage 5 (FREEZE.md 6.16's one exception to "turfc never looks for a
// manifest"): E027 inside a turf.toml project names `turf build`; outside one
// it is unchanged. The manifest is never read, so an EMPTY turf.toml is enough
// to prove the compiler only checks existence.
static void testModuleNotFoundHintsAtProject(const ScratchDir &Dir) {
  CurrentTest = "E027 project hint";
  fs::create_directories(Dir.Root / "proj" / "src" / "deep");
  fs::create_directories(Dir.Root / "loose");
  Dir.write("proj/turf.toml", "");
  const std::string InProject = Dir.write("proj/src/deep/main.tr", "");
  const std::string Loose = Dir.write("loose/main.tr", "");
  const std::string Source = "import geometry;\nfn main(): int { return 0; }\n";
  auto FindE027 = [](const turf::AnalysisResult &R) -> const turf::Diagnostic * {
    for (const auto &D : R.diagnostics())
      if (D.Code == "E027")
        return &D;
    return nullptr;
  };
  {
    auto R = turf::analyze({{InProject, Source}});
    const turf::Diagnostic *D = FindE027(R);
    CHECK(D != nullptr);
    if (D) {
      // The ancestor search starts at the importing file's own directory and
      // walks up, so a manifest two levels above is found.
      CHECK(D->Hint.find("This file is inside a TurfLang project") != std::string::npos);
      CHECK(D->Hint.find("turf.toml") != std::string::npos);
      CHECK(D->Hint.find("only visible to `turf build`") != std::string::npos);
      // The standard hint is kept, in front of the new sentence.
      CHECK(D->Hint.find("Make sure the file 'geometry.tr' exists") != std::string::npos);
    } else {
      std::cerr << render(R);
    }
  }
  {
    auto R = turf::analyze({{Loose, Source}});
    const turf::Diagnostic *D = FindE027(R);
    CHECK(D != nullptr);
    if (D)
      CHECK(D->Hint.find("turf build") == std::string::npos);
    else
      std::cerr << render(R);
  }
}


// LEC-003: destructuring patterns and guards. The runner cannot see WHY a
// fail_*.tr failed, so each diagnostic's code and a fragment of its hint are
// asserted here, and the warnings the runner never sees at all (an arm that
// can never run, an unused binding, `_name` silencing one) are asserted too.
// E146: an annotation the declaration cannot carry. The runner only sees that a
// fail_*.tr failed, so the code, and which half of the diagnostic names the
// problem, are asserted here; and so is the line between E146 and a syntax
// error, and the alias pass's error/warning split for `@ownership` calls.
static void testAnnotationDiagnostics() {
  CurrentTest = "annotation diagnostics";
  struct Case {
    const char *Label;
    const char *Source;
    const char *Code;
    const char *MessageFragment;
    const char *HintFragment;
  };
  const Case Cases[] = {
      {"unknown name", "@ownrship(acquires)\nextern fn m(n: int): int*;\nfn main(): int { return 0; }\n",
       "E146", "Unknown annotation '@ownrship'", "Did you mean '@ownership'"},
      {"argument does not fit", "@ownership(acquires)\nfn n(): int { return 0; }\nfn main(): int { return 0; }\n",
       "E146", "does not return a pointer", "states a fact about the declaration"},
      {"ffi on a function", "@ffi(macos: x)\nfn g(): int { return 0; }\nfn main(): int { return 0; }\n",
       "E146", "only sit on an 'extern fn'", "states a fact about the declaration"},
      {"keyword key reaches the owning pass", "@ownership(error: x)\nextern fn m(n: int): int*;\nfn main(): int { return 0; }\n",
       "E146", "Unknown '@ownership' argument 'error:'", "states a fact about the declaration"},
      {"malformed is still a syntax error", "@ownership(1)\nextern fn m(n: int): int*;\nfn main(): int { return 0; }\n",
       "E103", "Expected a word or a string", ""},
  };
  for (const auto &C : Cases) {
    auto R = turf::analyze({{"ann.tr", C.Source}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != C.Code)
        continue;
      Found = true;
      CHECK(D.Message.find(C.MessageFragment) != std::string::npos);
      CHECK(D.Hint.find(C.HintFragment) != std::string::npos);
      break;
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }

  // An annotated `extern fn` inside a function body parses, and a release on
  // only one path is not E017. (The "may already have been freed" warning
  // itself comes from the alias pass, which runs at codegen and so not in the
  // facade; tests/49_annotations/conditional_free_is_a_warning.tr pins it.)
  auto Ok = turf::analyze(
      {{"annok.tr",
        "fn main(): int {\n"
        "    @ownership(acquires)\n    extern fn malloc(size: int): int*;\n"
        "    @ownership(releases: p)\n    extern fn free(p: int*): void;\n"
        "    mut released: bool = false;\n"
        "    unsafe {\n"
        "        mut p: int* = malloc(8);\n"
        "        if !released { free(p); released = true; }\n"
        "        if !released { free(p); }\n"
        "    }\n"
        "    return 0;\n}\n"}});
  CHECK(Ok.succeeded());
  if (!Ok.succeeded())
    std::cerr << render(Ok);
}

static void testPatternDiagnostics() {
  CurrentTest = "pattern diagnostics";
  const std::string Prelude = "struct P { x: int, y: int }\nenum Color { Red, Green }\n";
  struct Case {
    const char *Label;
    const char *Body;
    const char *Code;
    const char *HintFragment;
  };
  const Case Cases[] = {
      {"struct pattern on an int",
       "fn main(): int { mut n: int = 1; compare n { matches P { x }: return x; done otherwise: return 0; } }",
       "E092", "destructures a struct value"},
      {"wrong struct",
       "struct Q { x: int }\nfn main(): int { mut p: P = P::default(); compare p { matches Q { x }: return x; done otherwise: return 0; } }",
       "E092", "Name the value's own struct"},
      {"field left out without ..",
       "fn main(): int { mut p: P = P::default(); compare p { matches P { x }: return x; done otherwise: return 0; } }",
       "E092", "end the pattern with `..`"},
      {"non-literal field pattern",
       "fn main(): int { mut p: P = P::default(); compare p { matches P { x: 1 + 2, .. }: return 1; done otherwise: return 0; } }",
       "E092", "bind it and use a guard"},
      {"name bound twice",
       "fn main(): int { mut p: P = P::default(); compare p { matches P { x, y: x }: return x; done otherwise: return 0; } }",
       "E092", "write '_' to ignore it"},
      {"stacked headers bind different names",
       "fn main(): int { mut p: P = P::default(); mut r: int = 0; compare p { matches P { x, .. }: matches P { y, .. }: r = 1; done otherwise: r = 2; } return r; }",
       "E092", "exactly the same names"},
      {"guard not bool",
       "fn main(): int { mut p: P = P::default(); compare p { matches P { x, .. } if x + 1: return x; done otherwise: return 0; } }",
       "E092", "Write a comparison"},
      {"wildcard as a name", "fn main(): int { mut _: int = 1; return 0; }", "E092",
       "prefix one with an underscore"},
      {"guarded arm is not coverage",
       "fn main(): int { mut r: int = 0; compare Color::Red { matches Color::Red: r = 1; done matches Color::Green if r == 0: r = 2; done } return r; }",
       "E093", "A guarded arm doesn't count"},
      {"bool missing a value",
       "fn main(): int { mut r: int = 0; compare true { matches true: r = 1; done } return r; }",
       "E093", "`matches false:`"},
  };
  for (const auto &C : Cases) {
    auto R = turf::analyze({{"pat.tr", Prelude + C.Body + "\n"}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != C.Code)
        continue;
      Found = true;
      CHECK(D.Hint.find(C.HintFragment) != std::string::npos);
      if (D.Hint.find(C.HintFragment) == std::string::npos)
        std::cerr << "    case '" << C.Label << "': hint was: " << D.Hint << "\n";
      break;
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }

  // A well-formed program succeeds.
  auto Ok = turf::analyze(
      {{"patok.tr", Prelude + "fn main(): int { mut p: P = P::default(); mut r: int; "
                              "compare p { matches P { x: 0, y } if y == 0: r = 1; done "
                              "matches P { x, .. }: r = x; done } return r; }\n"}});
  CHECK(Ok.succeeded());
  if (!Ok.succeeded())
    std::cerr << render(Ok);

  // An arm behind a catch-all is ONE warning, not one per checking pass (the
  // TypeChecker runs over every body twice), and it is still reported when
  // the same file is analysed again.
  const std::string Unreachable =
      Prelude + "fn main(): int { mut p: P = P::default(); mut r: int; "
                "compare p { matches _: r = 1; done matches P { x: 3, .. }: r = 2; done } "
                "return r; }\n";
  for (int Round = 0; Round < 2; ++Round) {
    auto R = turf::analyze({{"unreach.tr", Unreachable}});
    CHECK(R.succeeded());
    int Count = 0;
    for (const auto &D : R.diagnostics())
      if (D.Code == "E094")
        ++Count;
    CHECK_EQ(Count, 1);
  }
}

// LEC-004: error-variant payloads, variant patterns and `try`. Same reason as
// testPatternDiagnostics(): the runner cannot see WHY a fail_*.tr failed, so each
// code and a hint fragment is asserted, and so are the warnings it never sees.
static void testErrorPayloadDiagnostics() {
  CurrentTest = "error payload diagnostics";
  const std::string Prelude =
      "error IOError { NotFound(path: string), Denied(path: string, mode: int), Timeout }\n"
      "fn open(p: string): !int { return IOError::NotFound(p); }\n"
      "fn plain(): int { return 3; }\n";
  struct Case {
    const char *Label;
    const char *Body;
    const char *Code;
    const char *HintFragment;
  };
  const Case Cases[] = {
      {"too few arguments",
       "fn f(): !int { return IOError::Denied(\"p\"); }\nfn main(): int { return 0; }",
       "E096", "takes 2, or 3 with a trailing 'string' message"},
      {"wrong field type",
       "fn f(): !int { return IOError::Denied(\"p\", \"rw\"); }\nfn main(): int { return 0; }",
       "E096", "Pass a value of type 'int' for 'mode'"},
      {"wrong message type",
       "fn f(): !int { return IOError::NotFound(\"p\", 3); }\nfn main(): int { return 0; }",
       "E096", "must be a 'string'"},
      {"bare reference to a fielded variant",
       "fn f(): !int { return IOError::NotFound; }\nfn main(): int { return 0; }",
       "E096", "compare with '=='"},
      {"field that is an error type",
       "error Outer { Wrapped(cause: IOError) }\nfn main(): int { return 0; }", "E096",
       "not another failure"},
      {"bare variant pattern over a fielded variant",
       "fn f(): int { immut v: int = open(\"x\") recover as err run { compare err { "
       "matches IOError::NotFound: return 1; done otherwise: return 2; } }; return v; }\n"
       "fn main(): int { return 0; }",
       "E092", "IOError::NotFound(..)"},
      {"variant pattern leaves a field out",
       "fn f(): int { immut v: int = open(\"x\") recover as err run { compare err { "
       "matches IOError::Denied(p): return 1; done otherwise: return 2; } }; return v; }\n"
       "fn main(): int { return 0; }",
       "E092", "end the pattern with `..`"},
      {"open error union needs otherwise",
       "fn f(): int { immut v: int = open(\"x\") recover as err run { compare err { "
       "matches IOError::Timeout: return 1; done } }; return v; }\n"
       "fn main(): int { return 0; }",
       "E093", "End the compare with 'otherwise:'"},
      {"declared error type, variant missing",
       "fn main(): int { mut e: IOError = IOError::Timeout; compare e { "
       "matches IOError::NotFound(_): return 1; done matches IOError::Timeout: return 2; done } "
       "return 0; }",
       "E093", "neither does one whose payload pattern can fail to match"},
      {"try in main",
       "fn main(): int { immut n: int = try open(\"x\"); return n; }", "E095",
       "Handle the error here instead"},
      {"try in a function returning int",
       "fn f(): int { return try open(\"x\"); }\nfn main(): int { return 0; }", "E095",
       "Declare the function's return type as '!int'"},
      {"try on a non-fallible operand",
       "fn f(): !int { return try plain(); }\nfn main(): int { return 0; }", "E095",
       "drop the `try`"},
      {"try inside a defer",
       "fn f(): !int { mut n: int = 0; defer n = try open(\"x\"); return n; }\n"
       "fn main(): int { return 0; }",
       "E095", "Handle the error inside the deferred statement"},
  };
  for (const auto &C : Cases) {
    auto R = turf::analyze({{"payload.tr", Prelude + C.Body + "\n"}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != C.Code)
        continue;
      Found = true;
      CHECK(D.Hint.find(C.HintFragment) != std::string::npos);
      if (D.Hint.find(C.HintFragment) == std::string::npos)
        std::cerr << "    case '" << C.Label << "': hint was: " << D.Hint << "\n";
      break;
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }

  // A well-formed program succeeds.
  auto Ok = turf::analyze(
      {{"payok.tr",
        Prelude + "fn relay(): !int { immut n: int = try open(\"x\"); return n; }\n"
                  "fn pass(): !int { return open(\"x\"); }\n"
                  "fn f(): int { immut v: int = relay() recover as err run { compare err { "
                  "matches IOError::NotFound(path): return 1; done "
                  "matches IOError::Denied(_, 2): return 2; done otherwise: return 3; } }; "
                  "return v; }\n"
                  "fn main(): int { return f(); }\n"}});
  CHECK(Ok.succeeded());
  if (!Ok.succeeded())
    std::cerr << render(Ok);

  // A function whose only way to fail is a `try`, or a `return` of another
  // fallible call, really can fail: no "never returns an error" warning. One
  // that only returns a plain value still gets it.
  auto NeverReturns = [](const turf::AnalysisResult &R, const char *Fn) {
    for (const auto &D : R.diagnostics())
      if (D.Message.find("never actually returns an error") != std::string::npos &&
          D.Message.find(Fn) != std::string::npos)
        return true;
    return false;
  };
  auto W = turf::analyze(
      {{"never.tr",
        Prelude + "fn viaTry(): !int { immut n: int = try open(\"x\"); return n; }\n"
                  "fn viaReturn(): !int { return open(\"x\"); }\n"
                  "fn neverFails(): !int { return plain(); }\n"
                  "fn main(): int { return 0; }\n"}});
  CHECK(!NeverReturns(W, "viaTry"));
  CHECK(!NeverReturns(W, "viaReturn"));
  CHECK(NeverReturns(W, "neverFails"));
}

// LEC-006: generic `operator<T>` declarations. The runner only sees that a
// fail_*.tr failed, not why, and these two rules (E097) are new, so each code
// and a hint fragment is asserted here. The exact-match cases assert the
// message, since E066 is shared with every other type mismatch.
static void testGenericOperatorDiagnostics() {
  CurrentTest = "generic operator diagnostics";
  const std::string Prelude =
      "struct Cell<T> { v: T, n: int }\n"
      "fn<T> Cell.new(v: T): void { self.v = v; self.n = 1; }\n";
  struct Case {
    const char *Label;
    const char *Body;
    const char *Code;
    const char *Fragment; // in the hint for E097, in the message otherwise
  };
  const Case Cases[] = {
      {"type parameter only in the return type",
       "operator<T> [](p: Cell<int>, i: int): T { return p.v; }\n"
       "fn main(): int { return 0; }",
       "E097", "Use 'T' in an operand type"},
      {"no operand is an owned type",
       "operator<T> +(a: T, b: T): T { return a; }\nfn main(): int { return 0; }", "E097",
       "A bare type parameter matches every type"},
      {"right operand of the wrong type is not widened",
       "operator<T> +(a: Cell<T>, k: int): Cell<T> { return a; }\n"
       "fn main(): int { mut c: Cell<string> = Cell::new(\"s\"); mut d: Cell<string> = c + 1.5; "
       "return 0; }",
       "E057", "cannot apply '+' to 'Cell<string>' and 'double'"},
      {"two instantiations do not unify",
       "operator<T> ==(a: Cell<T>, b: Cell<T>): bool { return true; }\n"
       "fn main(): int { mut a: Cell<int> = Cell::new(1); mut b: Cell<string> = Cell::new(\"x\"); "
       "if (a == b) { return 1; } return 0; }",
       "E124", "Cannot compare 'Cell<int>' with 'Cell<string>'"},
  };
  for (const auto &C : Cases) {
    auto R = turf::analyze({{"genop.tr", Prelude + C.Body + "\n"}});
    CHECK(!R.succeeded());
    const bool InHint = std::string(C.Code) == "E097";
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != C.Code)
        continue;
      const std::string &Haystack = InHint ? D.Hint : D.Message;
      if (Haystack.find(C.Fragment) == std::string::npos)
        continue;
      Found = true;
      break;
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }

  // E098: a write into the by-value result of an `operator[]` read. Uses its
  // own `operator[]` so the case needs no import. Each is a write that would
  // be silently lost; the last two shapes are legal and must NOT be flagged.
  const std::string IndexPrelude =
      "struct Item { a: int }\n"
      "fn Item.new(a: int): void { self.a = a; }\n"
      "fn Item.bump(): void { self.a = self.a + 1; }\n"
      "fn Item.wrapper(): void { self.bump(); }\n"
      "struct Bag { it: Item }\n"
      "fn Bag.new(): void { self.it = Item::new(1); }\n"
      "operator [](b: Bag, i: int): Item { return b.it; }\n";
  const Case TempCases[] = {
      {"field assignment through an index result",
       "fn main(): int { mut b: Bag = Bag::new(); b[0].a = 5; return 0; }", "E098",
       "store it back"},
      {"compound assignment through an index result",
       "fn main(): int { mut b: Bag = Bag::new(); b[0].a += 5; return 0; }", "E098",
       "store it back"},
      {"++ through an index result",
       "fn main(): int { mut b: Bag = Bag::new(); b[0].a++; return 0; }", "E098",
       "store it back"},
      {"mutating method on an index result",
       "fn main(): int { mut b: Bag = Bag::new(); b[0].bump(); return 0; }", "E098",
       "store it back"},
      {"method that only reaches a mutation through self.m()",
       "fn main(): int { mut b: Bag = Bag::new(); b[0].wrapper(); return 0; }", "E098",
       "store it back"},
  };
  for (const auto &C : TempCases) {
    auto R = turf::analyze({{"temp.tr", IndexPrelude + C.Body + "\n"}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics())
      if (D.Code == C.Code && D.Hint.find(C.Fragment) != std::string::npos)
        Found = true;
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }
  // Reading through an index result, and the read-modify-write the hint
  // recommends, are fine.
  auto TempOk = turf::analyze(
      {{"tempok.tr",
        IndexPrelude + "fn main(): int { mut b: Bag = Bag::new(); mut x: int = b[0].a; "
                       "mut e: Item = b[0]; e.a = 5; e.bump(); return x + e.a; }\n"}});
  CHECK(TempOk.succeeded());
  if (!TempOk.succeeded())
    std::cerr << render(TempOk);

  // A well-formed generic operator, used at two instantiations, succeeds.
  auto Ok = turf::analyze(
      {{"genok.tr",
        Prelude + "operator<T> +(a: Cell<T>, k: int): Cell<T> { return a; }\n"
                  "operator<T> [](c: Cell<T>, i: int): T { return c.v; }\n"
                  "fn main(): int { mut a: Cell<int> = Cell::new(1); "
                  "mut b: Cell<string> = Cell::new(\"x\"); mut a2: Cell<int> = a + 1; "
                  "mut b2: Cell<string> = b + 1; mut x: int = a2[0]; mut y: string = b2[0]; "
                  "return x; }\n"}});
  CHECK(Ok.succeeded());
  if (!Ok.succeeded())
    std::cerr << render(Ok);
}

// Issue #260. A `T[]` parameter is a slice of the caller's array: E140 keeps
// it out of every place but a parameter, E141 keeps an `immut` array from
// being written through one. E141 is checked after every body (it follows a
// slice handed on to another function), so this also covers the facade's
// per-declaration recovery running that deferred pass.
// A candidate overload of an IMPORTED function is declared in the module's file,
// not the program's. Its location used to carry no file, which every consumer
// reads as "a line of the file being compiled": the note printed
// `(line 40, col 8)` as if that were in the user's program, and --smart showed
// the model an unrelated line of it as "the candidate" (SLM_IMPROVEMENT_PLAN
// C4). Both the arity error (Resolver) and the overload error (TypeChecker)
// must name the module's own file.
// The --smart model's page for the built-in `typeof` is chosen by the raise
// site (DiagnosticHints::Construct), never from the name in the message: a
// program's own `fn typeof(...)` produces the same text and got the built-in's
// page.
static void testBuiltinDocRouting() {
  const std::string Msg = "'typeof' was called with 2 arguments, but no overload accepts that many.";
  CHECK(SLM::extractContextKeyword(Msg) != "typeof");
  CHECK_EQ(SLM::getCategoryDocPath(SLM::ErrorCategory::FUNCTION_SIGNATURE,
                                   SLM::extractContextKeyword(Msg)),
           std::string("docs/slm/function_signature.txt"));
  CHECK_EQ(SLM::getCategoryDocPath(SLM::ErrorCategory::FUNCTION_SIGNATURE, "typeof"),
           std::string("docs/slm/builtin_typeof.txt"));
}

static void testImportedCandidateLocation() {
  CurrentTest = "imported candidate overloads name their own file";
  struct Case {
    const char *Label;
    const char *Body;
    const char *Code;
  };
  const Case Cases[] = {
      {"wrong argument count",
       "import math;\nfn main(): int { return math::abs(0, 0); }", "E044"},
      {"wrong argument type",
       "import math;\nfn main(): int { return math::abs(true); }", "E025"},
  };
  for (const auto &C : Cases) {
    auto R = turf::analyze({{"callsite.tr", std::string(C.Body) + "\n"}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != C.Code)
        continue;
      Found = true;
      CHECK(!D.Related.empty());
      for (const auto &Rel : D.Related) {
        const bool InMath = Rel.Where.Path.size() >= 7 &&
                            Rel.Where.Path.compare(Rel.Where.Path.size() - 7, 7,
                                                   "math.tr") == 0;
        CHECK(InMath);
        CHECK(Rel.Where.Line > 0);
        if (!InMath)
          std::cerr << "    case '" << C.Label << "': related note at '"
                    << Rel.Where.Path << "' line " << Rel.Where.Line << "\n";
      }
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }
}

static void testUnsizedArrayDiagnostics() {
  CurrentTest = "unsized array (slice) diagnostics";
  struct Case {
    const char *Label;
    const char *Body;
    const char *Code;
    const char *MessageFragment;
  };
  const Case Cases[] = {
      {"local slice with an unsized inner dimension",
       "fn main(): int { mut a: int[2][] = [[1], [2]]; return 0; }", "E140",
       "Variable 'a' can't use the unsized array type 'int[]'"},
      {"slicing a string",
       "fn main(): int { immut s: string = \"hey\"; immut t = s[1..]; return 0; }", "E142",
       "Cannot slice a value of type 'string'"},
      {"constant slice range past the end",
       "fn main(): int { immut a: int[3] = [1, 2, 3]; immut t = a[1..4]; return 0; }", "E143",
       "the end is outside 0..3"},
      {"mut view of an immutable array",
       "fn main(): int { immut a: int[3] = [1, 2, 3]; mut t = a[1..]; return t[0]; }", "E144",
       "'t' is a 'mut' view of 'a', which is immutable."},
      {"return type", "fn f(): int[] { mut a: int[1] = [1]; return a; }\n"
                      "fn main(): int { return 0; }",
       "E140", "The return type of 'f'"},
      {"inner dimension", "fn f(r: int[3][]): int { return 0; }\nfn main(): int { return 0; }",
       "E140", "(inside 'int[3][]')"},
      {"written through a forwarded slice",
       "fn inner(x: int[]): void { x[0] = 1; }\n"
       "fn outer(a: int[]): void { inner(a); }\n"
       "fn main(): int { immut v: int[2] = [1, 2]; outer(v); return 0; }",
       "E141", "'v' is immutable, but 'outer' writes to the array"},
  };
  for (const auto &C : Cases) {
    auto R = turf::analyze({{"slice.tr", std::string(C.Body) + "\n"}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics())
      if (D.Code == C.Code && D.Message.find(C.MessageFragment) != std::string::npos)
        Found = true;
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }

  // Reading an `immut` array through a slice, for-each and length included,
  // is fine.
  auto Ok = turf::analyze(
      {{"sliceok.tr",
        "fn sum(a: int[]): int { mut t: int = 0; for x in a { t += x; } "
        "return t + a.length(); }\n"
        "fn main(): int { immut v: int[2] = [1, 2]; return sum(v) - 5; }\n"}});
  CHECK(Ok.succeeded());
  if (!Ok.succeeded())
    std::cerr << render(Ok);

  // Range slicing: local views, negative and omitted bounds, writes through
  // a `mut` view of a `mut` array.
  auto SliceOk = turf::analyze(
      {{"rangeok.tr",
        "fn main(): int { immut nums: int[] = [10, 20, 30, 40, 50]; "
        "immut a = nums[2..]; immut b = nums[..3]; immut c = nums[1..-1]; "
        "mut m: int[4] = [1, 2, 3, 4]; mut v: int[] = m[1..3]; v[0] = 9; "
        "return a[0] + b[0] + c.length() + m[1] - 52; }\n"}});
  CHECK(SliceOk.succeeded());
  if (!SliceOk.succeeded())
    std::cerr << render(SliceOk);
}

// A written type's arguments must match its declaration's parameters one for
// one; a struct with none, or an enum, takes none.
static void testTypeArgumentCount() {
  CurrentTest = "type argument count";
  struct Case {
    const char *Label;
    const char *Body;
    const char *MessageFragment;
  };
  const Case Cases[] = {
      {"non-generic struct", "struct P { x: int }\n"
                             "fn main(): int { mut p: P<int> = P::default(); return p.x; }",
       "'P' is not generic"},
      {"enum", "enum E { A, B }\nfn main(): int { mut e: E<int> = E::A; return 0; }",
       "'E' is not generic"},
      {"too many", "struct B<T> { v: T }\n"
                   "fn main(): int { mut b: B<int, int> = B::default(); return b.v; }",
       "'B' takes 1 type argument (T), but 2 were given"},
  };
  for (const auto &C : Cases) {
    auto R = turf::analyze({{"targs.tr", std::string(C.Body) + "\n"}});
    CHECK(!R.succeeded());
    bool Found = false;
    for (const auto &D : R.diagnostics())
      if (D.Code == "E148" && D.Message.find(C.MessageFragment) != std::string::npos)
        Found = true;
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }
}

// LEC-005. The facade recovers from a parse error with the same
// SkipToNextTopLevelBoundary() the compiler driver uses, so an editor shows the
// same cascade -- or the lack of one -- as the command line.
static void testParseErrorRecoveryAndTypeNameHints() {
  CurrentTest = "parse-error recovery and type-name hints";

  // An error INSIDE a body must resynchronise at that function's closing
  // brace. Before, recovery stopped at the next `;` still inside the body, and
  // the rest of the body parsed as top-level statements.
  {
    auto R = turf::analyze({{"cascade.tr",
                             "fn main(): int {\n"
                             "    mut x: int = 1;\n"
                             "    mut f = fn(a: int): int { return a; };\n"
                             "    x = x + 1;\n"
                             "    return x;\n"
                             "}\n"}});
    CHECK(!R.succeeded());
    CHECK(hasDiagnostic(R, "Expected a function name after 'fn'"));
    CHECK(!hasDiagnostic(R, "Statements are not allowed outside of a function"));
    if (R.diagnostics().size() != 1)
      std::cerr << "    expected exactly one diagnostic, got:\n" << render(R);
    CHECK_EQ(R.diagnostics().size(), static_cast<size_t>(1));
  }

  // ...and it must land on THAT function's brace: not before it (the
  // cascade), and not after it (swallowing `second`, hiding its own error).
  {
    auto R = turf::analyze({{"resync.tr",
                             "fn first(): int {\n"
                             "    mut i: int = 0;\n"
                             "    while (i < 3) {\n"
                             "        if (i == 1) {\n"
                             "            mut f = fn(a: int): int { return a; };\n"
                             "        }\n"
                             "        i = i + 1;\n"
                             "    }\n"
                             "    return i;\n"
                             "}\n"
                             "fn second(): int {\n"
                             "    mut z: int = 5\n"
                             "    return z;\n"
                             "}\n"
                             "fn main(): int { return 0; }\n"}});
    CHECK(!R.succeeded());
    CHECK(hasDiagnostic(R, "Expected a function name after 'fn'"));
    CHECK(hasDiagnostic(R, "Expected ';' after statement"));
    CHECK(!hasDiagnostic(R, "Statements are not allowed outside of a function"));
  }

  // (A real stray top-level statement is still reported -- the fix removes the
  // false positive, not the check; testTopLevelStatementIsADiagnostic() pins it.)

  // E040/E041 hints follow what the name IS (resolver: visibleTypeKind()).
  struct Case {
    const char *Label;
    const char *Body;
    const char *Code;
    const char *HintFragment;
    const char *HintMustNotContain;
  };
  const Case Cases[] = {
      {"struct used as a value",
       "struct Node { v: int }\nfn main(): int { mut a: int = Node; return a; }",
       "E040", "'Node' is a struct, not a value", "mut Node: int = 5"},
      {"struct called like a function",
       "struct Node { v: int }\nfn main(): int { mut a: Node = Node(5); return a.v; }",
       "E041", "no `Node(...)` constructor-call syntax", "import the module"},
      {"enum used as a value", "enum Color { Red }\nfn main(): int { mut c: int = Color; return c; }",
       "E040", "'Color' is an enum, not a value", "mut Color: int = 5"},
      {"unknown name keeps the create-it hint",
       "fn main(): int { return counter; }", "E040", "Did you forget to create this variable",
       "is a struct"},
  };
  for (const auto &C : Cases) {
    auto R = turf::analyze({{"hint.tr", C.Body}});
    bool Found = false;
    for (const auto &D : R.diagnostics()) {
      if (D.Code != C.Code)
        continue;
      Found = D.Hint.find(C.HintFragment) != std::string::npos &&
              D.Hint.find(C.HintMustNotContain) == std::string::npos;
      break;
    }
    CHECK(Found);
    if (!Found)
      std::cerr << "    case '" << C.Label << "' produced:\n" << render(R);
  }
}

// REL-002 stage 2: `turf fmt` verifies every result before writing or reporting
// it. A verifier that has only ever seen correct output proves nothing, so this
// hands verifyFormatting() texts a buggy formatter could produce and requires
// each to be refused for ITS reason, and checks that a file the lexer rejects
// is declined without ending the process (a TurfError raised outside a recovery
// guard calls exit(1): it used to end `turfc --format` at the first such file).
static void testFormatterGuarantees() {
  CurrentTest = "formatter guarantees";
  using namespace turf::format;

  const std::string Src = "fn main(): int {\n        return 1;\n}\n\n\n";
  FormatResult R = formatChecked(Src);
  CHECK(R.Ok);
  CHECK_EQ(R.Text, std::string("fn main(): int {\n    return 1;\n}\n"));
  CHECK_EQ(verifyFormatting(Src, R.Text), std::string(""));
  // Already formatted text is a fixed point.
  CHECK_EQ(formatChecked(R.Text).Text, R.Text);

  auto Refused = [&](const std::string &Bad, const char *Fragment) {
    std::string Why = verifyFormatting(Src, Bad);
    CHECK(Why.find(Fragment) != std::string::npos);
    if (Why.find(Fragment) == std::string::npos)
      std::cerr << "    '" << Bad << "' gave: '" << Why << "'\n";
  };
  // A byte other than whitespace moved or vanished.
  Refused("fn main(): int {\n    return 2;\n}\n", "something other than whitespace");
  Refused("fn main(): int {\n    return 1;\n", "something other than whitespace");
  // Same non-whitespace bytes, different program: whitespace moved inside a
  // token, which only the token stream can see.
  Refused("fn ma in(): int {\n    return 1;\n}\n", "token stream");
  // Right tokens, but not what formatting again would leave.
  Refused("fn main(): int {\n  return 1;\n}\n", "not idempotent");
  Refused("fn main(): int {\n    return 1;\n}\n\n", "not idempotent");

  // A source the lexer rejects is declined with the lexer's reason, and the
  // process survives to answer the next call. Both the raising kind (a
  // malformed number) and the merely-recorded kind (an unterminated string).
  for (const char *Bad : {"fn main(): int {\n    return 3.14.15;\n}\n",
                          "fn main(): int {\n    mut s: string = \"open;\n    return 0;\n}\n"}) {
    FormatResult B = formatChecked(Bad);
    CHECK(!B.Ok);
    CHECK(B.Error.find("lexical error") != std::string::npos);
    CHECK(B.Text.empty());
    // Marked as such, because `--skip-unlexable` may skip exactly this and
    // never a verification failure ("formatter bug"), which has no such mark.
    CHECK(B.Unlexable);
  }
  CHECK(!formatChecked(Src).Unlexable);
  // The refusal left no diagnostics behind for the host to print.
  CHECK(!DiagnosticEngine::hasErrors());
  FormatResult After = formatChecked(Src);
  CHECK(After.Ok);

  // Empty input, and CRLF files keep their line endings.
  CHECK(formatChecked("").Ok);
  FormatResult Crlf = formatChecked("fn main(): int {\r\nreturn 1;\r\n}\r\n");
  CHECK(Crlf.Ok);
  CHECK_EQ(Crlf.Text, std::string("fn main(): int {\r\n    return 1;\r\n}\r\n"));
}

int main() {
  ScratchDir Dir;
  testFormatterGuarantees();
  testParseErrorRecoveryAndTypeNameHints();
  testCleanProgramQueries();
  testErrorDiagnostic();
  testTopLevelStatementIsADiagnostic();
  testBackToBackAnalysesAreIndependent();
  testWarningSurvivesReanalysis();
  testUnsafeCapabilityDoesNotOutliveThePragma();
  testHostStateIsRestored();
  testCancellation();
  testKeywords();
  testImportedModuleOverlay(Dir);
  testMissingStdlibIsADiagnostic(Dir);
  testPackageGraph(Dir);
  testWarningDedupeIsPerFile(Dir);
  testLintIsOptIn();
  testFlowAnalysisIsOptIn();
  testEscapeThroughStructFieldsInFacade();
  testFlowWarningsAreInSourceOrder();
  testSkipPrelude();
  testLexErrorInFirstToken();
  testEmptyInput();
  testVisibilityModifierPlacement();
  testPrivateSymbolAccess(Dir);
  testModuleNotImported(Dir);
  testPrivateInPublic(Dir);
  testMigratePub(Dir);
  testAmbiguousModuleImport(Dir);
  testModuleNotFoundHintsAtProject(Dir);
  testAnnotationDiagnostics();
  testPatternDiagnostics();
  testErrorPayloadDiagnostics();
  testGenericOperatorDiagnostics();
  testUnsizedArrayDiagnostics();
  testTypeArgumentCount();
  testImportedCandidateLocation();
  testBuiltinDocRouting();

  if (Failures) {
    std::cerr << Failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "turf-api-tests: all checks passed\n";
  return 0;
}
