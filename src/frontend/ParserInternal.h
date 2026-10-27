#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

// Private, implementation-only forward declarations shared across the 5
// ParserCore/Literal/Stmt/Decl/Module.cpp files (the split of the original
// monolithic Parser.cpp -- see docs/architecture/compiler_pipeline.md).
// NOT part of the compiler's public interface (that's Parser.h) -- these
// exist only so the mutually-recursive descent parser can call across the
// 5 translation units it's now spread across. Mirrors the forward-decl
// block that used to sit at the top of the single Parser.cpp file, plus a
// few functions that were `static` there but need external linkage now
// that a caller lives in a different file.

#include "frontend/AST.h"
#include "frontend/Parser.h"

#include <memory>

// Defined in ParserCore.cpp
TurfTypePtr ParseType();
// A type-argument list `<T1, T2, ...>`, CurTok at its '<' (ParserCore.cpp).
bool ParseTypeArgList(std::vector<TurfTypePtr> &TypeArgs);
bool isDisallowedVoidType(const TurfType *T);

// Defined in ParserLiteral.cpp
std::unique_ptr<ExprAST> ParseNumberExpr(bool IsInteger);
std::unique_ptr<ExprAST> ParseBoolExpr();
std::unique_ptr<ExprAST> ParseCharExpr();
std::unique_ptr<ExprAST> ParseStringExpr();
std::unique_ptr<ExprAST> ParseNullExpr();
std::unique_ptr<ExprAST> ParsePostfixExpression(std::unique_ptr<ExprAST> Base);

// Defined in ParserStmt.cpp
std::unique_ptr<ExprAST> ParseBlock();
std::unique_ptr<ExprAST> ParseIfExpr();
std::unique_ptr<ExprAST> ParseWhileExpr();
std::unique_ptr<ExprAST> ParseDoWhileExpr();
std::unique_ptr<ExprAST> ParseForExpr();
std::unique_ptr<ExprAST> ParseCompareExpr();
std::unique_ptr<ExprAST> ParseLetStatement(bool IsImmutable);
std::unique_ptr<ExprAST> ParseReturnExpr();
std::unique_ptr<ExprAST> ParseDeferStmt();
std::unique_ptr<ExprAST> ParseUnsafeExpr();
// The post-'unsafe' half of ParseUnsafeExpr() -- see its own doc comment
// (ParserStmt.cpp) for why this is split out (LANG-002's capability-pragma
// lookahead in ParserModule.cpp needs it too).
std::unique_ptr<ExprAST> ParseUnsafeBlockBody(SourceLocation KeywordLoc);

// Defined in ParserDecl.cpp
//
// Saves, clears and restores ParserState::FuncUsesTry (Parser.h) around one
// function body.
struct FuncBodyTryScope {
  bool Saved;
  FuncBodyTryScope() : Saved(Parser().FuncUsesTry) { Parser().FuncUsesTry = false; }
  bool used() const { return Parser().FuncUsesTry; }
  ~FuncBodyTryScope() { Parser().FuncUsesTry = Saved; }
};
std::unique_ptr<ExprAST> ParseStructDecl();
std::unique_ptr<ExprAST> ParseEnumDecl();
std::unique_ptr<ExprAST> ParseMachineDecl();
std::unique_ptr<ExprAST> ParseErrorDecl();
std::unique_ptr<ExprAST> ParseExternDecl();
std::unique_ptr<ExprAST> ParseExportDecl();
std::unique_ptr<ExprAST> ParseFuncDef(bool HasCallerLocation = false);
std::unique_ptr<ExprAST> ParseOperatorDecl(bool HasCallerLocation = false);
std::unique_ptr<ExprAST> ParseVisibilityPrefixed();
// Called when CurTok == TOK_AT: one or more `@name(args)` annotations (plus the
// `@caller_location` decorator), then an optional `pub`, then the `fn` or
// `extern fn` they describe.
std::unique_ptr<ExprAST> ParseAnnotatedDecl();
// LEC-002/LEC-003: true (after reporting) if CurTok is `pub`/`public` or a lone
// `_` where a name is expected -- a reserved word, not a missing identifier.
bool RejectReservedWordAsName();

// Defined in ParserModule.cpp
std::unique_ptr<ExprAST> ParseImport();

#endif
