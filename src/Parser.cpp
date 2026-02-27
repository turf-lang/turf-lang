#include "Parser.h"
#include "AST.h"
#include "Builtins.h"
#include "Errors.h"
#include "Lexer.h"
#include <map>
#include <memory>

// Current state of the Parser
int CurTok; // The current token the parser is looking at
static std::map<int, int> BinopPrecedence; // Precedence table: '*' > '+'

// Reads the next token from the Lexer and updates CurTok
int getNextToken() { return CurTok = gettok(); }

void InitializePrecedence() {
  // Assignment
  BinopPrecedence['='] = 10;

  // Logical
  BinopPrecedence[TOK_OR] = 15;  // ||
  BinopPrecedence[TOK_AND] = 20; // &&

  // Equality
  BinopPrecedence[TOK_EQ] = 25;
  BinopPrecedence[TOK_NEQ] = 25;

  // Relational
  BinopPrecedence['<'] = 30;
  BinopPrecedence['>'] = 30;
  BinopPrecedence[TOK_LEQ] = 30;
  BinopPrecedence[TOK_GEQ] = 30;

  // Additive
  BinopPrecedence['+'] = 40;
  BinopPrecedence['-'] = 40;

  // Multiplicative
  BinopPrecedence['*'] = 50;
  BinopPrecedence['/'] = 50;
  BinopPrecedence['%'] = 50;

  // Exponentiation
  BinopPrecedence['^'] = 60;
}

// Returns the priority of the current operator. If it's not an operator (like a
// number), returns -1.
static int GetTokPrecedence() {
  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0)
    return -1;

  return TokPrec;
}

std::unique_ptr<ExprAST> ParseExpression();
std::unique_ptr<ExprAST> ParseUnary();
std::unique_ptr<ExprAST> ParseBlock();
std::unique_ptr<ExprAST> ParseBuiltinCall(); // generic handler for all builtins
std::unique_ptr<ExprAST> ParseWhileExpr();
std::unique_ptr<ExprAST> ParseVarDecl();
std::unique_ptr<ExprAST> ParseBoolExpr();
std::unique_ptr<ExprAST> ParseStringExpr();

static TurfType TokenToTurfType(int Tok) {
  switch (Tok) {
  case TOK_TYPE_INT:
    return TURF_INT;
  case TOK_TYPE_DOUBLE:
    return TURF_DOUBLE;
  case TOK_TYPE_BOOL:
    return TURF_BOOL;
  case TOK_TYPE_STRING:
    return TURF_STRING;
  default:
    SyntaxError(CurLoc, "Unknown type").raise();
    return TURF_VOID;
  }
}

// Called when CurTok is a Number.
static std::unique_ptr<ExprAST> ParseNumberExpr(bool IsInteger) {
  auto Result = IsInteger ? std::make_unique<NumberExprAST>(IntVal)
                          : std::make_unique<NumberExprAST>(NumVal);
  getNextToken(); // consume the number
  return Result;
}

std::unique_ptr<ExprAST> ParseBoolExpr() {
  SourceLocation KeywordLoc = CurLoc;
  std::string IdName = IdentifierStr;
  bool SavedBoolVal = BoolVal;
  getNextToken();

  if (CurTok == TOK_ASSIGN) {
    getNextToken();
    auto RHS = ParseExpression();
    if (!RHS) return nullptr;
    return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName, std::move(RHS));
  }

  return std::make_unique<BoolExprAST>(SavedBoolVal);
}

std::unique_ptr<ExprAST> ParseStringExpr() {
  auto Result = std::make_unique<StringExprAST>(StringVal);
  getNextToken();
  return Result;
}

// Called when CurTok is an Assignment or Reference
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;
  SourceLocation VarLoc = CurLoc;

  getNextToken();

  if (CurTok == TOK_ASSIGN) {
    getNextToken();

    auto RHS = ParseExpression();
    if (!RHS) {
      return nullptr;
    }

    // Variable Assignmnent
    return std::make_unique<AssignmentExprAST>(VarLoc, IdName,
                                               std::move(RHS));
  }

  // Variable Reference
  return std::make_unique<VariableExprAST>(VarLoc, IdName);
}

// Parse Parentheses
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken(); // eat '('
  auto V = ParseExpression();
  if (!V)
    return nullptr;

  if (CurTok != ')') {
    SyntaxError(CurLoc, "Expected ')'").raise();
    return nullptr;
  }
  getNextToken(); // eat ')'
  return V;
}

std::unique_ptr<ExprAST> ParseIfExpr() {
  SourceLocation KeywordLoc = CurLoc;
  std::string IdName = IdentifierStr;
  getNextToken(); // Eating the if expression

  if (CurTok == TOK_ASSIGN) {
    getNextToken();
    auto RHS = ParseExpression();
    if (!RHS) return nullptr;
    return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName, std::move(RHS));
  }

  auto Cond = ParseExpression();
  if (!Cond)
    return nullptr;

  std::unique_ptr<ExprAST> Then;
  std::unique_ptr<ExprAST> Else;

  if (CurTok == TOK_THEN) {
    getNextToken();
    Then = ParseExpression();
  } else if (CurTok == '{') {
    Then = ParseBlock();
  } else {
    // Check if the current token is a typo for 'then'
    if (CurTok == TOK_IDENTIFIER) {
      if (getLevenshteinDistance(IdentifierStr, "then") <= 2) {
        KeywordError(CurLoc, IdentifierStr).raise();
        return nullptr;
      }
    }
    LogErrorAt(CurLoc, "Expected 'then' or '{' after if condition");
    return nullptr;
  }

  if (!Then)
    return nullptr;

  if (CurTok != TOK_ELSE) {
    if (CurTok == TOK_IDENTIFIER) {
      if (getLevenshteinDistance(IdentifierStr, "else") <= 2) {
        KeywordError(CurLoc, IdentifierStr).raise();
        return nullptr;
      }
    }
    LogErrorAt(CurLoc, "expected 'else'");
    return nullptr;
  }
  getNextToken();

  if (CurTok == '{') {
    Else = ParseBlock();
  } else {
    Else = ParseExpression();
  }

  if (!Else)
    return nullptr;

  return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then),
                                     std::move(Else));
}

// "Primary" means the basic building blocks: numbers or parentheses.
static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  default:
    // Check if the current token belongs to a registered builtin (e.g. print).
    // This branch never needs to change when new builtins are added.
    if (FindBuiltinByToken(CurTok))
      return ParseBuiltinCall();

    LogErrorAt(CurLoc, "unknown token when expecting an expression");
    return nullptr;

  case TOK_IDENTIFIER: {
    // Check for keyword typos before standard identifier parsing
    std::string IdName = IdentifierStr;
    bool CouldBeKeywordTypo = false;
    
    // Calculate distance to keywords only if its length is > 2 or the matched keyword length is similar
    if (IdName.length() >= 2) {
      for (const auto &pair : Keywords) {
        int dist = getLevenshteinDistance(IdName, pair.first);
        // Distance 1 is always good. Distance 2 is good only for longer words to avoid 'x' -> 'if'
        if (dist <= 1 || (dist == 2 && IdName.length() > 3 && pair.first.length() > 3)) {
          CouldBeKeywordTypo = true;
          break;
        }
      }
    }

    if (CouldBeKeywordTypo) {
      if (CurTok != TOK_ASSIGN) {
        KeywordError(CurLoc, IdName).raise();
        return nullptr;
      }
    }

    // Hand off to existing logic
    return ParseIdentifierExpr();
  }

  case TOK_NUMBER:
    return ParseNumberExpr(false);

  case TOK_INT_LITERAL:
    return ParseNumberExpr(true);

  case TOK_BOOL_LITERAL:
    return ParseBoolExpr();

  case TOK_STRING_LITERAL:
    return ParseStringExpr();

  case '(':
    return ParseParenExpr();

  case TOK_IF:
    return ParseIfExpr();

  case TOK_WHILE:
    return ParseWhileExpr();

  case TOK_THEN:
  case TOK_ELSE: {
    SourceLocation KeywordLoc = CurLoc;
    std::string IdName = IdentifierStr;
    getNextToken();
    if (CurTok == TOK_ASSIGN) {
      getNextToken();
      auto RHS = ParseExpression();
      if (!RHS) return nullptr;
      return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName, std::move(RHS));
    }
    LogErrorAt(KeywordLoc, "unknown token when expecting an expression");
    return nullptr;
  }

  case TOK_TYPE_INT:
  case TOK_TYPE_DOUBLE:
  case TOK_TYPE_BOOL:
  case TOK_TYPE_STRING:
    return ParseVarDecl();
  }
}

// This function handles the "Right Hand Side" of an expression.
// ExprPrec: The precedence of the operator strictly to our left.
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
  while (true) {
    // Look at the next operator
    int TokPrec = GetTokPrecedence();

    // If this is a low-precedence operator (or not an operator at all), then we
    // are done with the current block. Return what we have. Example: If we are
    // parsing "4 * 5 + 1", and we just built "4 * 5", the next op is "+". Since
    // "+" (20) < "*" (40), we stop and return.
    if (TokPrec < ExprPrec)
      return LHS;

    int BinOp = CurTok;
    getNextToken(); // consume binop

    // Parse the primary expression after the binary operator
    auto RHS = ParseUnary();
    if (!RHS)
      return nullptr;

    // LOOKAHEAD: Is the *next* operator even stronger?
    // Example: "a + b * c"
    // We are at "+". We parsed "b". Next op is "*". Since "*" is stronger than
    // "+", we let "*" steal "b" as its LHS.
    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      // Recursively parse the high-priority part first
      RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
      if (!RHS)
        return nullptr;
    }

    // Merge LHS and RHS into a new node
    LHS =
        std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
  }
}

// Entry point for parsing
std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParseUnary();
  if (!LHS)
    return nullptr;

  return ParseBinOpRHS(0, std::move(LHS));
}

// Parse API
std::unique_ptr<ExprAST> Parse() {
  if (CurTok == 0)
    getNextToken();

  return ParseExpression();
}

std::unique_ptr<ExprAST> ParseUnary() {
  // If the current token is not an operator that handles unary (like '-'),
  // then it must be a primary expression.
  if (CurTok != '-')
    return ParsePrimary();

  int Opc = CurTok;
  getNextToken();

  if (auto Operand = ParseUnary())
    return std::make_unique<UnaryExprAST>(Opc, std::move(Operand));

  return nullptr;
}

std::unique_ptr<ExprAST> ParseBlock() {
  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{'");
    return nullptr;
  }

  getNextToken();
  std::vector<std::unique_ptr<ExprAST>> Exprs;

  while (CurTok != '}' && CurTok != TOK_EOF) {
    if (CurTok == ';') {
      getNextToken();
      continue;
    }

    auto Expr = ParseExpression();
    if (!Expr)
      return nullptr;
    Exprs.push_back(std::move(Expr));
  }

  if (CurTok != '}') {
    LogErrorAt(CurLoc, "Expected '}'");
    return nullptr;
  }
  getNextToken();

  return std::make_unique<BlockExprAST>(std::move(Exprs));
}

// ParseBuiltinCall -  called when CurTok is the token of a registered builtin.
// Parses:  builtinName ( arg1, arg2, ... )
// The number of expected arguments is taken from the BuiltinDef, so this
// function never needs to change when a new builtin is added.
std::unique_ptr<ExprAST> ParseBuiltinCall() {
  const BuiltinDef *Def = FindBuiltinByToken(CurTok);
  std::string Name = Def->Name;
  getNextToken(); // consume the builtin keyword
  
  if (CurTok != '(') {
    LogErrorAt(CurLoc, "Expected '(' after '" + Name + "'");
    return nullptr;
  }
  getNextToken(); // eat '('

  std::vector<std::unique_ptr<ExprAST>> Args;
  for (int i = 0; i < Def->ArgCount; ++i) {
    auto Arg = ParseExpression();
    if (!Arg)
      return nullptr;
    Args.push_back(std::move(Arg));

    // Expect a comma between arguments (but not after the last one)
    if (i < Def->ArgCount - 1) {
      if (CurTok != ',') {
        LogErrorAt(CurLoc, "Expected ',' between arguments to '" + Name + "'");
        return nullptr;
      }
      getNextToken(); // eat ','
    }
  }

  if (CurTok != ')') {
    LogErrorAt(CurLoc, "Expected ')' after argument to '" + Name + "'");
    return nullptr;
  }
  getNextToken(); // eat ')'

  return std::make_unique<BuiltinCallExprAST>(Name, std::move(Args));
}

std::unique_ptr<ExprAST> ParseWhileExpr() {
  SourceLocation KeywordLoc = CurLoc;
  std::string IdName = IdentifierStr;
  getNextToken();

  if (CurTok == TOK_ASSIGN) {
    getNextToken();
    auto RHS = ParseExpression();
    if (!RHS) return nullptr;
    return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName, std::move(RHS));
  }

  auto Cond = ParseExpression();
  if (!Cond) {
    return nullptr;
  }

  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' after while condition");
    return nullptr;
  }

  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  return std::make_unique<WhileExprAST>(std::move(Cond), std::move(Body));
}

std::unique_ptr<ExprAST> ParseVarDecl() {
  SourceLocation TypeLoc = CurLoc;
  TurfType Type = TokenToTurfType(CurTok);
  getNextToken();

  bool IsKeyword = Keywords.find(IdentifierStr) != Keywords.end() && Keywords.at(IdentifierStr) == CurTok;
  if (CurTok != TOK_IDENTIFIER && !IsKeyword) {
    LogErrorAt(CurLoc, "Expected identifier after type");
    return nullptr;
  }

  std::string Name = IdentifierStr;
  SourceLocation NameLoc = CurLoc;
  getNextToken();

  if (CurTok != TOK_ASSIGN) {
    LogErrorAt(CurLoc, "Expected '=' after variable name");
    return nullptr;
  }

  getNextToken();
  auto Init = ParseExpression();
  if (!Init)
    return nullptr;

  return std::make_unique<VarDeclExprAST>(NameLoc, Name, Type,
                                          std::move(Init));
}
