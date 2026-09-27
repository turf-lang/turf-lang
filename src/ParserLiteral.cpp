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
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Error.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>


/**
 * @brief Builds the literal's EXACT value from the lexer's own scan
 * (NumLitSpelling/NumLitClean/NumLitRadix, Lexer.h) -- SEM-001
 * (docs/planning/ARCHITECTURE_OVERHAUL.md §8). The lexer deliberately stays
 * LLVM-free (plain strings, no llvm::APSInt/APFloat -- see those globals'
 * own doc comment in Lexer.h), so this is the one place that actually
 * constructs the bignum value; lexical well-formedness (radix, separators,
 * exponent shape) was already fully validated in the lexer, so nothing
 * here needs to raise a diagnostic of its own.
 */
std::unique_ptr<ExprAST> ParseNumberExpr(bool IsInteger) {
  SourceLocation Loc = CurLoc;
  std::string Spelling = NumLitSpelling;
  std::unique_ptr<ExprAST> Result;

  if (IsInteger) {
    // NumLitClean holds just the radix digits (no prefix, no '_'). Bit
    // width: llvm::APInt::getBitsNeeded() plus a round-trip safety check --
    // construct at the reported width, and if the result's OWN sign bit
    // ends up set (meaning this plain, never-negative-in-source magnitude
    // would misread as negative), redo one bit wider. This is defensive
    // rather than trusting getBitsNeeded()'s exact signed-vs-unsigned
    // rounding convention, and cheap (runs once per literal token, not
    // once per digit). A literal is never negative here regardless -- `-5`
    // parses as UnaryExprAST('-', NumberExprAST(5)), unchanged by SEM-001
    // (see this function's own doc comment: folding a literal unary minus
    // into the literal itself, per §10.2's `-9223372036854775808` example,
    // is explicitly deferred to whichever group actually enforces
    // representability against a destination type).
    unsigned Bits =
        std::max(64u, llvm::APInt::getBitsNeeded(NumLitClean, NumLitRadix));
    llvm::APInt Magnitude(Bits, NumLitClean, NumLitRadix);
    if (Magnitude.isNegative()) {
      Bits += 1;
      Magnitude = llvm::APInt(Bits, NumLitClean, NumLitRadix);
    }
    llvm::APSInt Exact(std::move(Magnitude), /*isUnsigned=*/false);
    Result = std::make_unique<NumberExprAST>(std::move(Exact), Spelling, Loc);
  } else {
    llvm::APFloat Value(llvm::APFloat::IEEEdouble());
    bool Overflowed = false;
    auto StatusOrErr =
        Value.convertFromString(NumLitClean, llvm::APFloat::rmNearestTiesToEven);
    if (StatusOrErr) {
      Overflowed = (*StatusOrErr & llvm::APFloat::opOverflow) != 0;
    } else {
      // The lexer already validated this is a syntactically well-formed
      // decimal float (digits, at most one '.', a well-formed optional
      // exponent) -- convertFromString() failing here would mean the
      // lexer's own grammar and LLVM's disagree about what's parseable,
      // i.e. a compiler bug, not a user mistake. Consume the error (rather
      // than let it propagate as an unhandled llvm::Error, which aborts
      // the process) and fall back to +0.0 for this one literal instead of
      // crashing the compiler over source that followed this language's
      // own documented grammar.
      llvm::consumeError(StatusOrErr.takeError());
    }
    Result = std::make_unique<NumberExprAST>(std::move(Value), Overflowed,
                                             Spelling, Loc);
  }

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
        LogErrorAt(CurLoc, "Expected ']' to close the array index", SLM::ErrorCategory::SYNTAX_ERROR);
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
        LogErrorAt(CurLoc, "Expected ')' to close the function call", SLM::ErrorCategory::SYNTAX_ERROR);
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
        LogErrorAt(CurLoc, "Expected a method name after '.'", SLM::ErrorCategory::SYNTAX_ERROR);
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
              LogErrorAt(CurLoc, "Expected ')' or ',' in argument list", SLM::ErrorCategory::SYNTAX_ERROR);
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
