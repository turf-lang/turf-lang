// SMP-001: unit tests for the compiler query facade (include/turf/Analysis.h).
//
// Built as `turf-api-tests` next to `turf` (so the facade finds lib/std the
// same way the compiler does) and run by CI and scripts/session_postflight.sh.
// Not a `.tr` test: what is under test is a C++ API and the global state it
// must leave untouched, neither of which a Turf program can observe.
//
// The isolation tests are the point. The facade exists because every tool
// that drove the pipeline by hand forgot some piece of global state; the
// only way to catch the next forgotten piece is to run different programs
// back to back and compare against a clean run, and to check the host's
// own state afterwards field by field.

#include "turf/Analysis.h"

#include "Lexer.h"
#include "MigratePub.h"
#include "Parser.h"
#include "SymbolTable.h"
#include "Types.h"

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
      {"before decorator", {"", "pub @caller_location\nfn f(): string { return caller_location; }", "decorator first"}},
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

int main() {
  ScratchDir Dir;
  testCleanProgramQueries();
  testErrorDiagnostic();
  testTopLevelStatementIsADiagnostic();
  testBackToBackAnalysesAreIndependent();
  testWarningSurvivesReanalysis();
  testUnsafeCapabilityDoesNotOutliveThePragma();
  testHostStateIsRestored();
  testImportedModuleOverlay(Dir);
  testWarningDedupeIsPerFile(Dir);
  testLintIsOptIn();
  testEmptyInput();
  testVisibilityModifierPlacement();
  testPrivateSymbolAccess(Dir);
  testPrivateInPublic(Dir);
  testMigratePub(Dir);
  testAmbiguousModuleImport(Dir);
  testModuleNotFoundHintsAtProject(Dir);
  testPatternDiagnostics();

  if (Failures) {
    std::cerr << Failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "turf-api-tests: all checks passed\n";
  return 0;
}
