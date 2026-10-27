/**
 * @file UnsafeReport.cpp
 * @brief `turfc --unsafe-report` -- see UnsafeReport.h for what it counts and why.
 */

#include "tooling/UnsafeReport.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "turf/FrontEndIsolation.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace turf::unsafe_report {

namespace {

// Runs inside the recovery guard; an error raised by getNextToken() skips the
// rest of it, and the counts so far stand.
void scanTokens(FileSurface &F, std::set<std::string> &Libs) {
  ParserState &P = Parser();
  // The two tokens before the current one, and whether the second is the
  // identifier `from` -- all the context any pattern below needs.
  int Prev2 = 0, Prev1 = 0;
  bool Prev1IsFrom = false;
  // `unsafe module;` counts only as the file's first statement, where
  // ParseLeadingUnsafeModuleDecl() honors it; anywhere else it is an error.
  bool LeadingPragma = false;
  unsigned Index = 0;
  getNextToken();
  while (P.CurTok != TOK_EOF) {
    const int Tok = P.CurTok;
    const bool IsFrom = Tok == TOK_IDENTIFIER && Lex().IdentifierStr == "from";
    if (Index == 1 && Prev1 == TOK_UNSAFE && Tok == TOK_IDENTIFIER &&
        Lex().IdentifierStr == "module")
      LeadingPragma = true;
    if (Index == 2 && LeadingPragma && Tok == ';')
      F.UnsafeModule = true;
    if (Tok == '{' && Prev1 == TOK_UNSAFE)
      ++F.UnsafeBlocks;
    if (Tok == TOK_EXTERN)
      ++F.ExternFns;
    if (Tok == TOK_STRING_LITERAL && Prev1IsFrom && Prev2 == TOK_EXTERN)
      Libs.insert(Lex().StringVal);
    Prev2 = Prev1;
    Prev1 = Tok;
    Prev1IsFrom = IsFrom;
    ++Index;
    getNextToken();
  }
}

std::string withSlashes(std::string Path) {
  std::replace(Path.begin(), Path.end(), '\\', '/');
  return Path;
}

// Every .tr file under @p Dir, relative to it, sorted.
std::vector<std::string> sourceFiles(const std::string &Dir) {
  std::vector<std::string> Files;
  std::error_code EC;
  for (llvm::sys::fs::recursive_directory_iterator It(Dir, EC), End;
       It != End && !EC; It.increment(EC)) {
    llvm::StringRef Path = It->path();
    if (llvm::sys::path::extension(Path) != ".tr")
      continue;
    llvm::sys::fs::file_status St;
    if (llvm::sys::fs::status(Path, St) || !llvm::sys::fs::is_regular_file(St))
      continue;
    llvm::SmallString<256> Rel(Path);
    llvm::sys::path::replace_path_prefix(Rel, Dir, "");
    std::string R = withSlashes(Rel.str().str());
    while (!R.empty() && R.front() == '/')
      R.erase(R.begin());
    Files.push_back(R);
  }
  std::sort(Files.begin(), Files.end());
  return Files;
}

bool readFile(const std::string &Path, std::string &Text) {
  std::ifstream In(Path, std::ios::binary);
  if (!In)
    return false;
  std::ostringstream S;
  S << In.rdbuf();
  Text = S.str();
  return true;
}

bool scanPackage(const std::string &Dir, PackageSurface &Pkg, std::string &Error) {
  std::set<std::string> Libs;
  for (const auto &Rel : sourceFiles(Dir)) {
    llvm::SmallString<256> Full(Dir);
    llvm::sys::path::append(Full, Rel);
    std::string Text;
    if (!readFile(Full.str().str(), Text)) {
      Error = "cannot read " + Full.str().str();
      return false;
    }
    FileSurface F = scanSource(Text);
    F.Path = Rel;
    ++Pkg.Modules;
    Pkg.UnsafeModules += F.UnsafeModule;
    Pkg.UnsafeBlocks += F.UnsafeBlocks;
    Pkg.ExternFns += F.ExternFns;
    Libs.insert(F.LinkLibraries.begin(), F.LinkLibraries.end());
    if (F.any())
      Pkg.Files.push_back(std::move(F));
  }
  Pkg.LinkLibraries.assign(Libs.begin(), Libs.end());
  return true;
}

llvm::json::Array stringArray(const std::vector<std::string> &V) {
  llvm::json::Array A;
  for (const auto &S : V)
    A.push_back(S);
  return A;
}

llvm::json::Object fileObject(const FileSurface &F) {
  llvm::json::Object O{{"path", F.Path},
                       {"unsafe_module", F.UnsafeModule},
                       {"unsafe_blocks", static_cast<int64_t>(F.UnsafeBlocks)},
                       {"extern_fns", static_cast<int64_t>(F.ExternFns)},
                       {"link_libraries", stringArray(F.LinkLibraries)}};
  if (!F.Error.empty())
    O["error"] = F.Error;
  return O;
}

llvm::json::Object packageObject(const PackageSurface &P) {
  llvm::json::Array Files;
  for (const auto &F : P.Files)
    Files.push_back(fileObject(F));
  return llvm::json::Object{{"allow_unsafe", P.AllowUnsafe},
                            {"modules", static_cast<int64_t>(P.Modules)},
                            {"unsafe_modules", static_cast<int64_t>(P.UnsafeModules)},
                            {"unsafe_blocks", static_cast<int64_t>(P.UnsafeBlocks)},
                            {"extern_fns", static_cast<int64_t>(P.ExternFns)},
                            {"link_libraries", stringArray(P.LinkLibraries)},
                            {"files", std::move(Files)}};
}

std::string print(llvm::json::Value V) {
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << V;
  return OS.str();
}

} // namespace

FileSurface scanSource(std::string_view Source) {
  FileSurface F;
  std::set<std::string> Libs;

  // Isolated like the formatter's lexing (Formatter.cpp, tokenKeys()): a file
  // that does not lex is reported, and must not take the report with it.
  turf::detail::runIsolated([&] {
    std::istringstream Buffer{std::string(Source)};
    LexerState Lexer(Buffer, "<unsafe-report>");
    ParserState P(Lexer);
    ParserScope Scope(P);
    SourceFilename = "<unsafe-report>";

    g_recoverActive = true;
    try {
      scanTokens(F, Libs);
    } catch (const RecoverUnwind &) {
    }
    g_recoverActive = false;

    for (const auto &D : DiagnosticEngine::getDiagnostics()) {
      if (D.IsWarning)
        continue;
      F.Error = "line " + std::to_string(D.Loc.Line) + ": " + D.Msg.substr(0, D.Msg.find('\n'));
      break;
    }
  });

  F.LinkLibraries.assign(Libs.begin(), Libs.end());
  return F;
}

bool buildReport(const PackageGraph &Graph, const std::string &StdlibDir,
                 Report &Out, std::string &Error) {
  Out = Report();
  std::vector<const PackageInfo *> Sorted;
  for (const auto &P : Graph.Packages)
    Sorted.push_back(&P);
  std::sort(Sorted.begin(), Sorted.end(),
            [](const PackageInfo *A, const PackageInfo *B) { return A->Name < B->Name; });
  for (const PackageInfo *P : Sorted) {
    PackageSurface Pkg;
    Pkg.Name = P->Name;
    Pkg.AllowUnsafe = P->AllowUnsafe;
    if (!scanPackage(P->SrcRoot, Pkg, Error))
      return false;
    Out.Packages.push_back(std::move(Pkg));
  }
  Out.Stdlib.Name = "std";
  Out.Stdlib.AllowUnsafe = true;
  if (!llvm::sys::fs::is_directory(StdlibDir)) {
    Error = "the standard library is not at " + StdlibDir;
    return false;
  }
  return scanPackage(StdlibDir, Out.Stdlib, Error);
}

std::string reportJson(const Report &R) {
  llvm::json::Object Pkgs;
  for (const auto &P : R.Packages)
    Pkgs[P.Name] = packageObject(P);
  return print(llvm::json::Object{{"format", 1},
                                  {"packages", std::move(Pkgs)},
                                  {"stdlib", packageObject(R.Stdlib)}});
}

std::string filesJson(const std::vector<FileSurface> &Files) {
  llvm::json::Array A;
  for (const auto &F : Files)
    A.push_back(fileObject(F));
  return print(llvm::json::Object{{"format", 1}, {"files", std::move(A)}});
}

} // namespace turf::unsafe_report
