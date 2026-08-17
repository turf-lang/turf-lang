#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

// Private, implementation-only forward declarations shared across the 5
// ParserCore/Literal/Stmt/Decl/Module.cpp files (the split of the original
// monolithic Parser.cpp -- see docs/planning/COMPILER_MODULE_BREAKDOWN.md).
// NOT part of the compiler's public interface (that's Parser.h) -- these
// exist only so the mutually-recursive descent parser can call across the
// 5 translation units it's now spread across. Mirrors the forward-decl
// block that used to sit at the top of the single Parser.cpp file, plus a
// few functions that were `static` there but need external linkage now
// that a caller lives in a different file.

#include "AST.h"

#include <memory>

// Defined in ParserCore.cpp
TurfTypePtr ParseType();
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

// Defined in ParserDecl.cpp
std::unique_ptr<ExprAST> ParseStructDecl();
std::unique_ptr<ExprAST> ParseEnumDecl();
std::unique_ptr<ExprAST> ParseMachineDecl();
std::unique_ptr<ExprAST> ParseErrorDecl();
std::unique_ptr<ExprAST> ParseExternDecl();
std::unique_ptr<ExprAST> ParseExportDecl();
std::unique_ptr<ExprAST> ParseFuncDef();
std::unique_ptr<ExprAST> ParseOperatorDecl();

// Defined in ParserModule.cpp
std::unique_ptr<ExprAST> ParseImport();

#endif
