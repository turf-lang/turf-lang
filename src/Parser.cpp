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
std::unique_ptr<ExprAST> ParseForExpr();
std::unique_ptr<ExprAST> ParseVarDecl();
std::unique_ptr<ExprAST> ParseVarDeclBody(SourceLocation TypeLoc, TurfType Type);
std::unique_ptr<ExprAST> ParseCastExpr(TurfType DestType, SourceLocation Loc);
std::unique_ptr<ExprAST> ParseBoolExpr();
std::unique_ptr<ExprAST> ParseStringExpr();
std::unique_ptr<ExprAST> ParseFuncDef();
std::unique_ptr<ExprAST> ParseReturnExpr();

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
  case TOK_TYPE_VOID:
    return TURF_VOID;
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

  if (CurTok == TOK_ASSIGN || CurTok == TOK_PLUS_ASSIGN || CurTok == TOK_MINUS_ASSIGN ||
      CurTok == TOK_MUL_ASSIGN || CurTok == TOK_DIV_ASSIGN || CurTok == TOK_MOD_ASSIGN) {
    int AssignOp = CurTok;
    getNextToken();

    auto RHS = ParseExpression();
    if (!RHS) {
      return nullptr;
    }

    if (AssignOp != TOK_ASSIGN) {
      char Op = '+';
      if (AssignOp == TOK_MINUS_ASSIGN) Op = '-';
      else if (AssignOp == TOK_MUL_ASSIGN) Op = '*';
      else if (AssignOp == TOK_DIV_ASSIGN) Op = '/';
      else if (AssignOp == TOK_MOD_ASSIGN) Op = '%';

      auto LHSVar = std::make_unique<VariableExprAST>(VarLoc, IdName);
      RHS = std::make_unique<BinaryExprAST>(Op, std::move(LHSVar), std::move(RHS));
    }

    // Variable Assignmnent
    return std::make_unique<AssignmentExprAST>(VarLoc, IdName,
                                               std::move(RHS));
  }

  // Handle Postfix ++ and --
  if (CurTok == TOK_PLUS_PLUS || CurTok == TOK_MINUS_MINUS) {
    int OpType = CurTok;
    getNextToken();

    char Op = (OpType == TOK_PLUS_PLUS) ? '+' : '-';
    auto LHSVar = std::make_unique<VariableExprAST>(VarLoc, IdName);
    auto OneNum = std::make_unique<NumberExprAST>(1LL);
    auto RHS = std::make_unique<BinaryExprAST>(Op, std::move(LHSVar), std::move(OneNum));

    return std::make_unique<AssignmentExprAST>(VarLoc, IdName, std::move(RHS));
  }

  // User-defined function call: name(...)
  if (CurTok == '(') {
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
      LogErrorAt(CurLoc, "Expected ')' in function call");
      return nullptr;
    }
    getNextToken();
    return std::make_unique<FuncCallExprAST>(VarLoc, IdName, std::move(Args));
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

  case TOK_FOR:
    return ParseForExpr();

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
  case TOK_TYPE_DOUBLE: {
    // if the next token is '(' this is a cast call, e.g. int(x)
    // otherwise it is a variable declaration, e.g. int x = ...
    TurfType DestType = (CurTok == TOK_TYPE_INT) ? TURF_INT : TURF_DOUBLE;
    SourceLocation TypeLoc = CurLoc;
    getNextToken(); // consume the type keyword
    if (CurTok == '(')
      return ParseCastExpr(DestType, TypeLoc);
    return ParseVarDeclBody(TypeLoc, DestType);
  }

  case TOK_TYPE_BOOL:
  case TOK_TYPE_STRING:
    return ParseVarDecl();

  case TOK_TYPE_VOID:
    SyntaxError(CurLoc, "'void' may only be used as a function return type").raise();
    return nullptr;

  case TOK_FN:
    return ParseFuncDef();

  case TOK_RETURN:
    return ParseReturnExpr();
  case TOK_BREAK: {
    SourceLocation KeywordLoc = CurLoc;
    std::string IdName = IdentifierStr;
    getNextToken();
    if (CurTok == TOK_ASSIGN) {
      getNextToken();
      auto RHS = ParseExpression();
      if (!RHS) return nullptr;
      return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName, std::move(RHS));
    }
    return std::make_unique<BreakExprAST>(KeywordLoc);
  }

  case TOK_CONTINUE: {
    SourceLocation KeywordLoc = CurLoc;
    std::string IdName = IdentifierStr;
    getNextToken();
    if (CurTok == TOK_ASSIGN) {
      getNextToken();
      auto RHS = ParseExpression();
      if (!RHS) return nullptr;
      return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName, std::move(RHS));
    }
    return std::make_unique<ContinueExprAST>(KeywordLoc);
  }
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
  if (CurTok == TOK_PLUS_PLUS || CurTok == TOK_MINUS_MINUS) {
    int OpType = CurTok;
    SourceLocation Loc = CurLoc;
    getNextToken();

    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(Loc, "Expected identifier after prefix ++ or --");
      return nullptr;
    }

    std::string IdName = IdentifierStr;
    getNextToken();

    char Op = (OpType == TOK_PLUS_PLUS) ? '+' : '-';
    auto LHSVar = std::make_unique<VariableExprAST>(Loc, IdName);
    auto OneNum = std::make_unique<NumberExprAST>(1LL);
    auto RHS = std::make_unique<BinaryExprAST>(Op, std::move(LHSVar), std::move(OneNum));

    return std::make_unique<AssignmentExprAST>(Loc, IdName, std::move(RHS));
  }

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

// ParseForExpr - parses:
//   for <ident> in <start>..<end> step <step_expr> { body }
std::unique_ptr<ExprAST> ParseForExpr() {
  SourceLocation ForLoc = CurLoc;
  getNextToken();

  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected identifier after 'for'");
    return nullptr;
  }
  std::string VarName = IdentifierStr;
  getNextToken();

  if (CurTok != TOK_IN) {
    LogErrorAt(CurLoc, "Expected 'in' after loop variable");
    return nullptr;
  }
  getNextToken();

  auto Start = ParseExpression();
  if (!Start)
    return nullptr;
  if (CurTok != TOK_RANGE) {
    LogErrorAt(CurLoc, "Expected '..' in for loop range");
    return nullptr;
  }
  getNextToken();

  auto End = ParseExpression();
  if (!End)
    return nullptr;
  if (CurTok != TOK_STEP) {
    LogErrorAt(CurLoc, "Expected 'step' after range");
    return nullptr;
  }
  getNextToken();

  auto Step = ParseExpression();
  if (!Step)
    return nullptr;

  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' after step expression");
    return nullptr;
  }

  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  return std::make_unique<ForExprAST>(ForLoc, VarName, std::move(Start),
                                      std::move(End), std::move(Step),
                                      std::move(Body));
}

// ParseVarDeclBody - called after the type keyword has already been consumed.
// CurTok must already be the identifier (variable name) when this is called.
std::unique_ptr<ExprAST> ParseVarDeclBody(SourceLocation TypeLoc, TurfType Type) {
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

// ParseVarDecl - original entry point (type token is still in CurTok).
std::unique_ptr<ExprAST> ParseVarDecl() {
  SourceLocation TypeLoc = CurLoc;
  TurfType Type = TokenToTurfType(CurTok);
  getNextToken(); // consume the type keyword
  return ParseVarDeclBody(TypeLoc, Type);
}

// ParseCastExpr - called after the type keyword has been consumed and
// CurTok == '('. Parses int(expr) or double(expr).
std::unique_ptr<ExprAST> ParseCastExpr(TurfType DestType, SourceLocation Loc) {
  getNextToken(); // eat '('

  auto Operand = ParseExpression();
  if (!Operand)
    return nullptr;

  if (CurTok != ')') {
    LogErrorAt(CurLoc, "Expected ')' after cast expression");
    return nullptr;
  }
  getNextToken(); // eat ')'

  return std::make_unique<CastExprAST>(Loc, DestType, std::move(Operand));
}

// ParseFuncDef - called when CurTok == TOK_FN.
// Parses: fn <rettype> <name>(<type> <param>, ...) { body }
std::unique_ptr<ExprAST> ParseFuncDef() {
  SourceLocation FnLoc = CurLoc;
  getNextToken();

  if (CurTok != TOK_TYPE_INT && CurTok != TOK_TYPE_DOUBLE &&
      CurTok != TOK_TYPE_BOOL && CurTok != TOK_TYPE_STRING &&
      CurTok != TOK_TYPE_VOID) {
    LogErrorAt(CurLoc, "Expected return type after 'fn'");
    return nullptr;
  }
  TurfType RetType = TokenToTurfType(CurTok);
  getNextToken();

  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected function name after return type");
    return nullptr;
  }
  std::string FuncName = IdentifierStr;
  getNextToken();

  if (CurTok != '(') {
    LogErrorAt(CurLoc, "Expected '(' after function name");
    return nullptr;
  }
  getNextToken();

  std::vector<ParamDecl> Params;
  while (CurTok != ')' && CurTok != TOK_EOF) {
    // Parse each parameter: <type> <name>
    if (CurTok != TOK_TYPE_INT && CurTok != TOK_TYPE_DOUBLE &&
        CurTok != TOK_TYPE_BOOL && CurTok != TOK_TYPE_STRING) {
      LogErrorAt(CurLoc, "Expected parameter type in function parameter list");
      return nullptr;
    }
    TurfType ParamType = TokenToTurfType(CurTok);
    getNextToken();

    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected parameter name after type");
      return nullptr;
    }
    std::string ParamName = IdentifierStr;
    getNextToken();

    Params.push_back({ParamType, ParamName});

    if (CurTok == ',')
      getNextToken();
    else if (CurTok != ')')
      break;
  }

  if (CurTok != ')') {
    LogErrorAt(CurLoc, "Expected ')' after parameter list");
    return nullptr;
  }
  getNextToken();

  // Parse body block
  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' for function body");
    return nullptr;
  }
  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  return std::make_unique<FuncDefExprAST>(FnLoc, FuncName, RetType,
                                          std::move(Params), std::move(Body));
}

// ParseReturnExpr - called when CurTok == TOK_RETURN.
// Parses: return;   or   return <expr>;
std::unique_ptr<ExprAST> ParseReturnExpr() {
  SourceLocation RetLoc = CurLoc;
  getNextToken();

  if (CurTok == ';' || CurTok == '}' || CurTok == TOK_EOF) {
    return std::make_unique<ReturnExprAST>(RetLoc, nullptr);
  }
  auto Val = ParseExpression();
  if (!Val)
    return nullptr;
  return std::make_unique<ReturnExprAST>(RetLoc, std::move(Val));
}
