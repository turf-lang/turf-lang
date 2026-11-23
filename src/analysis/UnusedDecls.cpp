// AN-7: unused private functions, unused imports, unused `pub` functions in a
// binary's root file. See UnusedDecls.h.

#include "analysis/UnusedDecls.h"

#include "diagnostics/Errors.h"
#include "frontend/ASTChildWalker.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"
#include "resolve/ResolverSymbolBase.h"

#include <regex>
#include <set>
#include <string>
#include <unordered_set>

namespace {

// `geometry::Point.len` and `Point.len` are one method: the Resolver renames a
// module's method to its type identity, and a call's symbol may carry either.
std::string normalized(const std::string &Name) {
  static const std::regex Qualifier("\\b[A-Za-z_][A-Za-z0-9_]*::");
  return std::regex_replace(Name, Qualifier, "");
}

struct Def {
  const FuncDefExprAST *Decl;
  std::string File;
  bool InRoot;
};

// Every function declaration and every use, across the main file and every
// module body it imports.
class Collector : public ASTChildWalker {
public:
  std::vector<Def> Defs;
  std::unordered_set<const FuncDefExprAST *> UsedDecls;
  std::unordered_set<std::string> UsedNames;
  std::unordered_set<std::string> UsedMethodNames;
  // Modules a resolved use in the ROOT file came from (E179).
  std::unordered_set<std::string> RootModulesUsed;
  std::string File;
  bool InRoot = true;

  void use(const ResolverFunctionSymbol *S) {
    if (!S)
      return;
    if (S->Decl)
      UsedDecls.insert(S->Decl);
    UsedNames.insert(normalized(S->BaseName));
    if (InRoot && !S->SourceModule.empty())
      RootModulesUsed.insert(S->SourceModule);
  }

  void visitFuncDef(FuncDefExprAST &Node) override {
    Defs.push_back({&Node, File, InRoot});
    ASTChildWalker::visitFuncDef(Node);
  }
  void visitFuncCall(FuncCallExprAST &Node) override {
    for (const auto &C : Node.getResolvedCandidates())
      use(C.get());
    use(Node.getResolvedCallTarget());
    ASTChildWalker::visitFuncCall(Node);
  }
  void visitMethodCall(MethodCallExprAST &Node) override {
    use(Node.getResolvedCallTarget());
    // Inside a generic body (`a.get()` with `a: T`) the call is resolved only
    // in each instantiation, which this walk does not see: any method of
    // that name counts as used.
    if (!Node.getResolvedCallTarget())
      UsedMethodNames.insert(Node.getMethod());
    ASTChildWalker::visitMethodCall(Node);
  }
  void visitVariable(VariableExprAST &Node) override {
    use(Node.ResolvedFunctionRef.get());
  }
  void visitModuleImport(ModuleImportExprAST &Node) override {
    const std::string SavedFile = File;
    const bool SavedRoot = InRoot;
    File = Node.getResolvedPath();
    InRoot = false;
    if (const auto *Body = dynamic_cast<const BlockExprAST *>(Node.getBody()))
      for (const auto &E : Body->getExpressions())
        visitChild(E.get());
    File = SavedFile;
    InRoot = SavedRoot;
  }
};

bool isTest(const FuncDefExprAST &F) {
  for (const auto &A : F.getAnnotations())
    if (A.Name == "test")
      return true;
  return false;
}

// The names a module declares at its top level (types, functions, constants):
// what an `import m::[*]` or `import m::Name` can bring into a file.
std::set<std::string> declaredNames(const ModuleImportExprAST &Import) {
  std::set<std::string> Out;
  const auto *Body = dynamic_cast<const BlockExprAST *>(Import.getBody());
  if (!Body)
    return Out;
  for (const auto &E : Body->getExpressions()) {
    std::string N;
    if (const auto *F = dynamic_cast<const FuncDefExprAST *>(E.get()))
      N = F->getName();
    else if (const auto *S = dynamic_cast<const StructDeclExprAST *>(E.get()))
      N = S->getName();
    else if (const auto *En = dynamic_cast<const EnumDeclExprAST *>(E.get()))
      N = En->getName();
    else if (const auto *V = dynamic_cast<const VarDeclExprAST *>(E.get()))
      N = V->getName();
    N = normalized(N);
    if (const size_t Dot = N.find('.'); Dot != std::string::npos)
      N = N.substr(0, Dot);
    if (!N.empty())
      Out.insert(N);
  }
  return Out;
}

// The root file's code with comments and string/char literals blanked, one
// entry per line, so a name inside a comment or a string is not a use.
std::vector<std::string> codeLines(const std::vector<std::string> &Lines) {
  std::vector<std::string> Out;
  bool InBlock = false;
  for (const std::string &L : Lines) {
    std::string C;
    for (size_t I = 0; I < L.size(); ++I) {
      if (InBlock) {
        if (L.compare(I, 2, "*/") == 0) {
          InBlock = false;
          ++I;
        }
        C += ' ';
        continue;
      }
      if (L.compare(I, 2, "//") == 0)
        break;
      if (L.compare(I, 2, "/*") == 0) {
        InBlock = true;
        ++I;
        C += "  ";
        continue;
      }
      if (L[I] == '"' || L[I] == '\'') {
        const char Q = L[I];
        C += ' ';
        for (++I; I < L.size() && L[I] != Q; ++I) {
          if (L[I] == '\\')
            ++I;
          C += ' ';
        }
        C += ' ';
        continue;
      }
      C += L[I];
    }
    Out.push_back(C);
  }
  return Out;
}

bool mentions(const std::vector<std::string> &Code, size_t SkipLine, const std::string &Name,
              bool Qualified) {
  const std::regex Re(Qualified ? "\\b" + Name + "::" : "\\b" + Name + "\\b");
  for (size_t I = 0; I < Code.size(); ++I)
    if (I + 1 != SkipLine && std::regex_search(Code[I], Re))
      return true;
  return false;
}

void reportUnusedImports(const std::vector<const ExprAST *> &Program,
                         const Collector &C) {
  auto Lines = AllSourceLines.find(MainSourceFilename);
  if (Lines == AllSourceLines.end())
    return;
  const std::vector<std::string> Code = codeLines(Lines->second);
  for (const ExprAST *E : Program) {
    const auto *Import = dynamic_cast<const ModuleImportExprAST *>(E);
    if (!Import || IsLibStdWarningLocation(Import->getLoc().Filename) ||
        (!Import->getLoc().Filename.empty() && Import->getLoc().Filename != MainSourceFilename))
      continue;
    const SourceLocation &At = Import->getLoc();
    if (At.Line < 1 || At.Line > static_cast<int>(Lines->second.size()))
      continue;
    const std::string &M = Import->getModuleName();
    // The prelude is imported into every file; naming it again is harmless.
    if (IsPreludeModuleName(M))
      continue;
    // `geometry/util` (a package module's identity) is written `util::`.
    std::string Bare = M;
    if (const size_t Slash = Bare.find_last_of('/'); Slash != std::string::npos)
      Bare = Bare.substr(Slash + 1);
    bool Used = C.RootModulesUsed.count(M) || C.RootModulesUsed.count(Bare) ||
                mentions(Code, At.Line, Bare, /*Qualified=*/true);
    if (!Used) {
      std::set<std::string> Names;
      if (Import->getMode() == ImportMode::Wildcard || Import->getMode() == ImportMode::Bare)
        Names = declaredNames(*Import);
      for (const auto &R : Import->getRequestedSymbols())
        Names.insert(R.Name);
      for (const auto &N : Names)
        if (mentions(Code, At.Line, N, /*Qualified=*/false)) {
          Used = true;
          break;
        }
    }
    if (Used)
      continue;
    // The whole statement on its line, and nothing else there: the Fix
    // empties the line.
    const std::string &Line = Lines->second[At.Line - 1];
    const size_t First = Line.find_first_not_of(" \t");
    const size_t Semi = Line.find(';');
    UnusedImportWarning W(At, M);
    if (First != std::string::npos && Line.compare(First, 7, "import ") == 0 &&
        Semi != std::string::npos && Line.find_first_not_of(" \t\r", Semi + 1) == std::string::npos)
      W.withFixIt(At.Line, 1, static_cast<int>(Line.size()) + 1, "")
          .withFixDescription("remove 'import " + M + "'");
    W.emit();
  }
}

} // namespace

namespace UnusedDecls {

void report(const std::vector<const ExprAST *> &Program, bool IsBinaryRoot) {
  Collector C;
  C.File = MainSourceFilename;
  for (const ExprAST *E : Program)
    C.visitChild(E);

  for (const Def &D : C.Defs) {
    const FuncDefExprAST &F = *D.Decl;
    const std::string Name = F.getName();
    const std::string Norm = normalized(Name);
    const size_t Dot = Norm.rfind('.');
    if (Name == "main" || Name.rfind("operator", 0) == 0 || isTest(F) ||
        IsLibStdWarningLocation(D.File) || C.UsedDecls.count(&F) || C.UsedNames.count(Norm) ||
        (Dot != std::string::npos && C.UsedMethodNames.count(Norm.substr(Dot + 1))))
      continue;
    if (!F.isPublic())
      UnusedPrivateFunctionWarning(F.getNameLoc().Line ? F.getNameLoc() : F.getLoc(),
                                   normalized(F.getSourceName()))
          .emit();
    else if (D.InRoot && IsBinaryRoot)
      UnusedPubInBinaryWarning(F.getNameLoc().Line ? F.getNameLoc() : F.getLoc(),
                               normalized(F.getSourceName()))
          .emit();
  }
  reportUnusedImports(Program, C);
}

} // namespace UnusedDecls
