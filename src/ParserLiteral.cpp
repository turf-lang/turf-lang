/**
 * @file ParserLiteral.cpp
 * @brief Literal-expression parsers (number/bool/char/string/null) and postfix-expression parsing.
 * @details Part of the recursive-descent parser for TurfLang, split from the
 *          original monolithic Parser.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/architecture/compiler_pipeline.md).
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "Parser.h"
#include "AST.h"
#include "Algorithms.h"
#include "Builtins.h"
#include "Codegen.h"
#include "Errors.h"
#include "Lexer.h"
#include "SuggestionIndex.h"
#include "SymbolTable.h"
#include "ParserInternal.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>


// Called when CurTok is a Number.
std::unique_ptr<ExprAST> ParseNumberExpr(bool IsInteger) {
  auto Result = IsInteger ? std::make_unique<NumberExprAST>(IntVal)
                          : std::make_unique<NumberExprAST>(NumVal);
  getNextToken(); // consume the number
  return Result;
}

std::unique_ptr<ExprAST> ParseBoolExpr() {
  bool SavedBoolVal = BoolVal;
  getNextToken();
  return std::make_unique<BoolExprAST>(SavedBoolVal);
}

std::unique_ptr<ExprAST> ParseCharExpr() {
  auto Result = std::make_unique<CharExprAST>(CharVal);
  getNextToken();
  return Result;
}

std::unique_ptr<ExprAST> ParseStringExpr() {
  auto Result = std::make_unique<StringExprAST>(StringVal);
  getNextToken();
  return Result;
}

std::unique_ptr<ExprAST> ParseNullExpr() {
  auto Result = std::make_unique<NullExprAST>(CurLoc);
  getNextToken();
  return Result;
}

std::unique_ptr<ExprAST> ParsePostfixExpression(std::unique_ptr<ExprAST> Base) {
  while (true) {
    if (CurTok == '[') {
      SourceLocation Loc = CurLoc;
      getNextToken();
      auto Index = ParseExpression();
      if (!Index)
        return nullptr;
      if (CurTok != ']') {
        LogErrorAt(CurLoc, "Expected ']' to close the array index");
        return nullptr;
      }
      getNextToken();
      Base = std::make_unique<ArrayAccessExprAST>(Loc, std::move(Base),
                                                  std::move(Index));
    } else if (CurTok == '(') {
      SourceLocation Loc = CurLoc;
      getNextToken();
      std::vector<std::unique_ptr<ExprAST>> Args;
      while (CurTok != ')' && CurTok != TOK_EOF) {
        auto Arg = ParseExpression();
        if (!Arg)
          return nullptr;
        Args.push_back(std::move(Arg));
        if (CurTok == ',')
          getNextToken();
        else if (CurTok != ')')
          break;
      }
      if (CurTok != ')') {
        LogErrorAt(CurLoc, "Expected ')' to close the function call");
        return nullptr;
      }
      getNextToken();
      // Struct initialization now uses brace syntax: Point { x: 10, y: 20 }
      // Positional Point(10, 20) is no longer valid; treat as a regular call.
      Base = std::make_unique<FuncCallExprAST>(Loc, std::move(Base),
                                               std::move(Args));
    } else if (CurTok == '.') {
      SourceLocation Loc = CurLoc;
      getNextToken();
      if (CurTok != TOK_IDENTIFIER) {
        LogErrorAt(CurLoc, "Expected a method name after '.'");
        return nullptr;
      }
      std::string MethodName = IdentifierStr;
      SourceLocation MethodLoc = CurLoc;
      getNextToken();
      if (CurTok == '(') {
        getNextToken(); // eat '('
        std::vector<std::unique_ptr<ExprAST>> Args;
        if (CurTok != ')') {
          while (true) {
            if (auto Arg = ParseExpression()) {
              Args.push_back(std::move(Arg));
            } else {
              return nullptr;
            }
            if (CurTok == ')')
              break;
            if (CurTok != ',') {
              LogErrorAt(CurLoc, "Expected ')' or ',' in argument list");
              return nullptr;
            }
            getNextToken();
          }
        }
        getNextToken(); // eat ')'
        Base = std::make_unique<MethodCallExprAST>(Loc, std::move(Base),
                                                   MethodName, MethodLoc, std::move(Args));
      } else {
        if (MethodName == "length") {
          Base = std::make_unique<MethodCallExprAST>(
              Loc, std::move(Base), "length", MethodLoc,
              std::vector<std::unique_ptr<ExprAST>>{});
        } else {
          Base = std::make_unique<FieldAccessExprAST>(Loc, std::move(Base),
                                                      MethodName, MethodLoc);
        }
      }
    } else if (CurTok == TOK_PLUS_PLUS || CurTok == TOK_MINUS_MINUS) {
      int OpType = CurTok;
      SourceLocation OpLoc = CurLoc;
      getNextToken();
      char Op = (OpType == TOK_PLUS_PLUS) ? '+' : '-';
      Base = std::make_unique<PostfixExprAST>(OpLoc, Op, std::move(Base));
    } else {
      break;
    }
  }
  return Base;
}
