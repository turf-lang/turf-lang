#include "Parser.h"
#include "AST.h"
#include "Builtins.h"
#include "Codegen.h"
#include "Errors.h"
#include "Lexer.h"
#include "SymbolTable.h"
#include <map>
#include <memory>
#include <vector>

// Current state of the Parser
int CurTok; // The current token the parser is looking at
static std::map<int, int> BinopPrecedence; // Precedence table: '*' > '+'
static std::vector<std::unique_ptr<ExprAST>> ParsedTopLevelASTs;

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

void ClearParsedTopLevelASTs() {
  ParsedTopLevelASTs.clear();
}

void RecordParsedTopLevelAST(const ExprAST *AST) {
  if (AST) {
    ParsedTopLevelASTs.push_back(CloneExprAST(AST));
  }
}

std::unique_ptr<ExprAST> BuildProgramASTFromCache() {
  return std::make_unique<BlockExprAST>(CloneExprList(ParsedTopLevelASTs));
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
std::unique_ptr<ExprAST> ParseVarDeclBody(SourceLocation TypeLoc,
                                          TurfTypePtr Type);
std::unique_ptr<ExprAST> ParseCastExpr(TurfTypePtr DestType, SourceLocation Loc);
std::unique_ptr<ExprAST> ParseBoolExpr();
std::unique_ptr<ExprAST> ParseStringExpr();
std::unique_ptr<ExprAST> ParseFuncDef();
std::unique_ptr<ExprAST> ParseReturnExpr();
// NOTE: ParseListDecl removed — list<T> declarations go through ParseVarDeclBody
//       via the unified ParseType() path, same as int/double/bool/string.

// ParseType: recursively parse a type expression from the token stream.
// Handles primitives, void, generics like list<int>, and arrays like int[5].
// Trailing [N] brackets are consumed here, making array types fully first-class.
// Returns nullptr on failure.
static TurfTypePtr ParseType() {
  if (CurTok != TOK_IDENTIFIER && CurTok != TOK_TYPE_INT &&
      CurTok != TOK_TYPE_DOUBLE && CurTok != TOK_TYPE_BOOL &&
      CurTok != TOK_TYPE_STRING && CurTok != TOK_TYPE_VOID &&
      CurTok != TOK_TYPE_LIST) {
    SyntaxError(CurLoc, "Expected a type name").raise();
    return nullptr;
  }

  std::string BaseName;
  if      (CurTok == TOK_TYPE_INT)    BaseName = "int";
  else if (CurTok == TOK_TYPE_DOUBLE) BaseName = "double";
  else if (CurTok == TOK_TYPE_BOOL)   BaseName = "bool";
  else if (CurTok == TOK_TYPE_STRING) BaseName = "string";
  else if (CurTok == TOK_TYPE_VOID)   BaseName = "void";
  else if (CurTok == TOK_TYPE_LIST)   BaseName = "list";
  else                                BaseName = IdentifierStr; // user-defined

  getNextToken(); // consume the type name

  // Check for generic parameter: BaseName<T1, T2, ...>
  if (CurTok == '<') {
    getNextToken(); // consume '<'
    std::vector<TurfTypePtr> TypeArgs;
    while (true) {
      TurfTypePtr Arg = ParseType();
      if (!Arg) return nullptr;
      TypeArgs.push_back(std::move(Arg));
      if (CurTok == '>') {
        getNextToken(); // consume '>'
        break;
      }
      if (CurTok != ',') {
        SyntaxError(CurLoc, "Expected '>' or ',' in generic type").raise();
        return nullptr;
      }
      getNextToken(); // consume ','
    }
    // Arrays of generics are rare but allowed: parse trailing [N]
    TurfTypePtr GenBase = std::make_shared<GenericType>(BaseName, std::move(TypeArgs));
    while (CurTok == '[') {
      getNextToken();
      int Size = -1;
      if (CurTok == TOK_INT_LITERAL) {
        Size = static_cast<int>(IntVal);
        if (Size <= 0) {
          SyntaxError(CurLoc, "Array size must be positive").raise();
          return nullptr;
        }
        getNextToken();
      }
      if (CurTok != ']') {
        SyntaxError(CurLoc, "Expected ']' to close array size").raise();
        return nullptr;
      }
      getNextToken();
      GenBase = makeArrayType(GenBase, Size);
    }
    return GenBase;
  }

  // Primitive / named type
  TurfTypePtr Base;
  if      (BaseName == "int")    Base = TurfInt();
  else if (BaseName == "double") Base = TurfDouble();
  else if (BaseName == "bool")   Base = TurfBool();
  else if (BaseName == "string") Base = TurfString();
  else if (BaseName == "void")   Base = TurfVoid();
  else if (BaseName == "list") {
    // "list" without <T> is an error — give a helpful message
    SyntaxError(CurLoc, "Expected '<' after 'list' — e.g. list<int>").raise();
    return nullptr;
  }
  else Base = std::make_shared<NamedType>(BaseName); // user-defined struct etc.

  // Recursively consume trailing array dimensions: int[5] or int[5][10]
  // Size may be omitted (int[]) to indicate a pointer-parameter (size = -1).
  while (CurTok == '[') {
    getNextToken(); // eat '['
    int Size = -1;
    if (CurTok == TOK_INT_LITERAL) {
      Size = static_cast<int>(IntVal);
      if (Size <= 0) {
        SyntaxError(CurLoc, "Array size must be a positive integer (greater than 0)").raise();
        return nullptr;
      }
      getNextToken(); // eat the literal
    }
    if (CurTok != ']') {
      SyntaxError(CurLoc, "Expected ']' to close array size — e.g. int[5] arr").raise();
      return nullptr;
    }
    getNextToken(); // eat ']'
    Base = makeArrayType(Base, Size);
  }

  return Base;
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
    if (!RHS)
      return nullptr;
    return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName,
                                               std::move(RHS));
  }

  return std::make_unique<BoolExprAST>(SavedBoolVal);
}

std::unique_ptr<ExprAST> ParseStringExpr() {
  auto Result = std::make_unique<StringExprAST>(StringVal);
  getNextToken();
  return Result;
}

// The actual body — takes an already-consumed identifier name and location.
std::unique_ptr<ExprAST>
ParseIdentifierExprFrom(const std::string &IdName, SourceLocation VarLoc) {
  // CurTok is already the token AFTER IdName (caller consumed it)
  // Array element access: arr[index] or arr[index] = value
  if (CurTok == '[') {
    getNextToken();
    auto Index = ParseExpression();
    if (!Index)
      return nullptr;
    if (CurTok != ']') {
      LogErrorAt(CurLoc, "Expected ']' to close the array index — did you forget to close the '[' before the index?");
      return nullptr;
    }
    getNextToken();

    // Check for assignment to array element: arr[i] = expr
    if (CurTok == TOK_ASSIGN || CurTok == TOK_PLUS_ASSIGN ||
        CurTok == TOK_MINUS_ASSIGN || CurTok == TOK_MUL_ASSIGN ||
        CurTok == TOK_DIV_ASSIGN || CurTok == TOK_MOD_ASSIGN) {
      int AssignOp = CurTok;
      getNextToken();
      auto RHS = ParseExpression();
      if (!RHS)
        return nullptr;

      if (AssignOp != TOK_ASSIGN) {
        char Op = '+';
        if (AssignOp == TOK_MINUS_ASSIGN)
          Op = '-';
        else if (AssignOp == TOK_MUL_ASSIGN)
          Op = '*';
        else if (AssignOp == TOK_DIV_ASSIGN)
          Op = '/';
        else if (AssignOp == TOK_MOD_ASSIGN)
          Op = '%';
        LogErrorAt(VarLoc, "Compound assignment operators are not supported on "
                           "array elements. Use arr[i] = arr[i] + value instead.");
        return nullptr;
      }

      return std::make_unique<ArrayAssignExprAST>(VarLoc, IdName,
                                                  std::move(Index),
                                                  std::move(RHS));
    }

    return std::make_unique<ArrayAccessExprAST>(VarLoc, IdName,
                                                std::move(Index));
  }

  // Dot-method dispatch: arr.length  /  list.push(v)  /  list.pop()  /  list.length
  if (CurTok == '.') {
    getNextToken(); // eat '.'
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected a method name after '.' (e.g. push, pop, length)");
      return nullptr;
    }
    std::string MethodName = IdentifierStr;
    SourceLocation MethodLoc = CurLoc;
    getNextToken(); // eat method name

    // push(val)
    if (MethodName == "push") {
      if (CurTok != '(') {
        LogErrorAt(MethodLoc, "Expected '(' after 'push' — e.g. nums.push(value)");
        return nullptr;
      }
      getNextToken(); // eat '('
      auto Arg = ParseExpression();
      if (!Arg)
        return nullptr;
      if (CurTok != ')') {
        LogErrorAt(CurLoc, "Expected ')' to close push() call");
        return nullptr;
      }
      getNextToken(); // eat ')'
      return std::make_unique<ListMethodCallExprAST>(VarLoc, IdName, "push",
                                                     std::move(Arg));
    }

    // pop()
    if (MethodName == "pop") {
      if (CurTok != '(') {
        LogErrorAt(MethodLoc, "Expected '(' after 'pop' — e.g. nums.pop()");
        return nullptr;
      }
      getNextToken(); // eat '('
      if (CurTok != ')') {
        LogErrorAt(CurLoc, "pop() takes no arguments");
        return nullptr;
      }
      getNextToken(); // eat ')'
      return std::make_unique<ListMethodCallExprAST>(VarLoc, IdName, "pop");
    }

    // length  (for both arrays and lists — codegen differentiates)
    if (MethodName == "length") {
      // Emit ArrayLengthExprAST for both arrays and lists.
      // ArrayLengthExprAST::codegen already handles the list case by checking
      // isListType on the VarInfo — no parser-time symbol table lookup needed.
      return std::make_unique<ArrayLengthExprAST>(VarLoc, IdName);
    }

    LogErrorAt(MethodLoc,
               "Unknown method '" + MethodName + "'. Supported methods: push, pop, length.");
    return nullptr;
  }

  if (CurTok == TOK_ASSIGN || CurTok == TOK_PLUS_ASSIGN ||
      CurTok == TOK_MINUS_ASSIGN || CurTok == TOK_MUL_ASSIGN ||
      CurTok == TOK_DIV_ASSIGN || CurTok == TOK_MOD_ASSIGN) {
    int AssignOp = CurTok;
    getNextToken();

    auto RHS = ParseExpression();
    if (!RHS) {
      return nullptr;
    }

    if (AssignOp != TOK_ASSIGN) {
      char Op = '+';
      if (AssignOp == TOK_MINUS_ASSIGN)
        Op = '-';
      else if (AssignOp == TOK_MUL_ASSIGN)
        Op = '*';
      else if (AssignOp == TOK_DIV_ASSIGN)
        Op = '/';
      else if (AssignOp == TOK_MOD_ASSIGN)
        Op = '%';

      auto LHSVar = std::make_unique<VariableExprAST>(VarLoc, IdName);
      RHS = std::make_unique<BinaryExprAST>(VarLoc, Op, std::move(LHSVar),
                                            std::move(RHS));
    }

    // Variable Assignmnent
    return std::make_unique<AssignmentExprAST>(VarLoc, IdName, std::move(RHS));
  }

  // Handle Postfix ++ and --
  if (CurTok == TOK_PLUS_PLUS || CurTok == TOK_MINUS_MINUS) {
    int OpType = CurTok;
    getNextToken();

    char Op = (OpType == TOK_PLUS_PLUS) ? '+' : '-';
    auto LHSVar = std::make_unique<VariableExprAST>(VarLoc, IdName);
    auto OneNum = std::make_unique<NumberExprAST>(1LL);
    auto RHS = std::make_unique<BinaryExprAST>(VarLoc, Op, std::move(LHSVar),
                                               std::move(OneNum));

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
      LogErrorAt(CurLoc, "Expected ')' to close the function call — check for a missing ')' or an extra argument");
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
    SyntaxError(CurLoc, "Missing closing ')' — every '(' must have a matching ')'").raise();
    return nullptr;
  }
  getNextToken(); // eat ')'
  return V;
}
std::unique_ptr<ExprAST> ParseIfExpr() {
  SourceLocation KeywordLoc = CurLoc;
  std::string IdName = IdentifierStr;
  getNextToken(); // eat 'if'

  if (CurTok == TOK_ASSIGN) {
    getNextToken();
    auto RHS = ParseExpression();
    if (!RHS)
      return nullptr;
    return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName,
                                               std::move(RHS));
  }

  auto Cond = ParseExpression();
  if (!Cond)
    return nullptr;

  std::unique_ptr<ExprAST> Then;
  bool IsBlockForm = false;

  if (CurTok == TOK_THEN) {
    getNextToken();
    Then = ParseExpression();
  } else if (CurTok == '{') {
    IsBlockForm = true;
    Then = ParseBlock();
  } else {
    if (CurTok == TOK_IDENTIFIER) {
      if (getDamerauLevenshteinDistance(IdentifierStr, "then") <= std::max(1, (int)IdentifierStr.length() / 3)) {
        KeywordError(CurLoc, IdentifierStr).raise();
        return nullptr;
      }
    }
    LogErrorAt(KeywordLoc, "Expected 'then' or '{' after the if-condition."
               "\n  Hint: Use 'if cond then expr else expr' or 'if cond { ... }'.");
    return nullptr;
  }

  if (!Then)
    return nullptr;

  if (!IsBlockForm) {
    if (CurTok == TOK_IDENTIFIER) {
      if (getDamerauLevenshteinDistance(IdentifierStr, "else") <= std::max(1, (int)IdentifierStr.length() / 3)) {
        KeywordError(CurLoc, IdentifierStr).raise();
        return nullptr;
      }
    }
    if (CurTok != TOK_ELSE) {
      LogErrorAt(CurLoc, "expected 'else'");
      return nullptr;
    }

    SourceLocation ElseLoc = CurLoc;
    getNextToken(); // eat 'else'

    auto Else = ParseExpression();
    if (!Else)
      return nullptr;

    std::vector<CondBranch> Branches;
    Branches.push_back({KeywordLoc, std::move(Cond), std::move(Then)});
    return std::make_unique<IfExprAST>(std::move(Branches), std::move(Else),
                                       ElseLoc);
  }

  std::vector<CondBranch> Branches;
  Branches.push_back({KeywordLoc, std::move(Cond), std::move(Then)});
  while (CurTok == TOK_ELSEIF) {
    SourceLocation ElseifLoc = CurLoc;
    getNextToken(); // eat 'elseif'

    auto ElseifCond = ParseExpression();
    if (!ElseifCond)
      return nullptr;

    if (CurTok != '{') {
      LogErrorAt(ElseifLoc, "Expected '{' to open the elseif-block body."
                 "\n  Hint: 'elseif' blocks must always use braces: elseif condition { ... }");
      return nullptr;
    }
    auto ElseifBody = ParseBlock();
    if (!ElseifBody)
      return nullptr;

    Branches.push_back({ElseifLoc, std::move(ElseifCond), std::move(ElseifBody)});
  }

  std::unique_ptr<ExprAST> ElseBody = nullptr;
  SourceLocation ElseLoc = {0, 0};

  if (CurTok == TOK_ELSE) {
    ElseLoc = CurLoc;
    getNextToken(); // eat 'else'

    if (CurTok == '{') {
      ElseBody = ParseBlock();
    } else {
      ElseBody = ParseExpression();
    }
    if (!ElseBody)
      return nullptr;
  } else {
    if (CurTok == TOK_IDENTIFIER) {
      int thresh = std::max(1, (int)IdentifierStr.length() / 3);
      if (getDamerauLevenshteinDistance(IdentifierStr, "else") <= thresh ||
          getDamerauLevenshteinDistance(IdentifierStr, "elseif") <= thresh) {
        KeywordError(CurLoc, IdentifierStr).raise();
        return nullptr;
      }
    }
  }

  return std::make_unique<IfExprAST>(std::move(Branches), std::move(ElseBody),
                                     ElseLoc);
}

// "Primary" means the basic building blocks: numbers or parentheses.
static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  default:
    // Check if the current token belongs to a registered builtin (e.g. print).
    // This branch never needs to change when new builtins are added.
    if (FindBuiltinByToken(CurTok))
      return ParseBuiltinCall();

    LogErrorAt(CurLoc, "Unexpected token — expected a value, variable name, or expression here."
               "\n  Hint: Check for stray characters, a missing operator, or an incomplete statement.");
    return nullptr;

  case TOK_IDENTIFIER: {
    // Snapshot the identifier, then advance so CurTok reflects what follows.
    std::string IdName = IdentifierStr;
    SourceLocation IdLoc = CurLoc;
    getNextToken(); // consume the identifier; CurTok now = next token
    bool CouldBeKeywordTypo = false;

    // Only flag a typo if this name is NOT already a known variable.
    bool IsKnownVariable = false;
    if (GlobalSymbolTable)
      IsKnownVariable = (GlobalSymbolTable->LookupSymbol(IdName) != nullptr);
    if (!IsKnownVariable)
      IsKnownVariable = (NamedValues.find(IdName) != NamedValues.end());

    if (!IsKnownVariable && IdName.length() >= 2) {
      for (const auto &pair : Keywords) {
        int dist = getDamerauLevenshteinDistance(IdName, pair.first);
        int thresh = std::max(1, (int)IdName.length() / 3);
        if (dist <= thresh) { CouldBeKeywordTypo = true; break; }
      }
    }

    if (CouldBeKeywordTypo) {
      // Allow: assignment, dot-access, subscript, function call
      if (CurTok != TOK_ASSIGN && CurTok != '.' && CurTok != '[' && CurTok != '(') {
        KeywordError(IdLoc, IdName).raise();
        return nullptr;
      }
    }

    // Hand off to ParseIdentifierExpr with the already-consumed name.
    // We restore IdentifierStr so ParseIdentifierExpr can use it.
    // Note: ParseIdentifierExpr starts by reading IdentifierStr then calling
    // getNextToken(). Since we already consumed the token, we pass the name
    // and the already-advanced CurTok by calling the body directly.
    //
    // Simplest: just re-enter ParseIdentifierExpr but it will re-consume
    // IdentifierStr which is now stale. Instead, we duplicate the essential
    // logic here using IdName and the already-advanced CurTok.
    return ParseIdentifierExprFrom(IdName, IdLoc);
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
      if (!RHS)
        return nullptr;
      return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName,
                                                 std::move(RHS));
    }
    LogErrorAt(KeywordLoc, "Unexpected '" + IdName + "' keyword here — expected a value or expression."
               "\n  Hint: '" + IdName + "' is a control-flow keyword; it can't appear standalone in an expression.");
    return nullptr;
  }

  case TOK_TYPE_INT:
  case TOK_TYPE_DOUBLE:
  case TOK_TYPE_STRING:
  case TOK_TYPE_BOOL:
  case TOK_TYPE_LIST: {
    // ParseType() handles all type keywords uniformly, including list<T>.
    // If the next token is '(' it is a cast: int(x), double(x), string(x).
    // Otherwise it is a variable (or array) declaration.
    SourceLocation TypeLoc = CurLoc;
    TurfTypePtr DestType = ParseType(); // consumes the type keyword(s)
    if (!DestType) return nullptr;
    if (CurTok == '(')
      return ParseCastExpr(DestType, TypeLoc);
    return ParseVarDeclBody(TypeLoc, DestType);
  }

  case TOK_TYPE_VOID:
    SyntaxError(CurLoc, "'void' may only be used as a function return type")
        .raise();
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
      if (!RHS)
        return nullptr;
      return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName,
                                                 std::move(RHS));
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
      if (!RHS)
        return nullptr;
      return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName,
                                                 std::move(RHS));
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

    SourceLocation OpLoc = CurLoc;
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
    LHS = std::make_unique<BinaryExprAST>(CurLoc, BinOp, std::move(LHS),
                                          std::move(RHS));
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
    auto RHS = std::make_unique<BinaryExprAST>(CurLoc, Op, std::move(LHSVar),
                                               std::move(OneNum));

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
    LogErrorAt(CurLoc, "Expected '{' to open a block");
    return nullptr;
  }

  SourceLocation OpenBraceLoc = CurLoc;
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
    LogErrorAt(OpenBraceLoc, "Missing closing '}' — every '{' must have a matching '}'."
               "\n  Hint: Check that all your blocks are properly closed.");
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
  SourceLocation CallLoc = CurLoc; // Capture location BEFORE consuming token
  getNextToken(); // consume the builtin keyword

  if (CurTok != '(') {
    LogErrorAt(CallLoc, "Expected '(' after '" + Name + "' — builtins must be called with parentheses");
    return nullptr;
  }
  getNextToken(); // eat '('

  // ── Zero-argument guard ──────────────────────────────────────────────────
  // Fires before the loop so the silent ParseExpression() path is never hit.
  // Without this, a call like typeof() would reach ParsePrimary()'s default
  // branch and emit the inscrutable "unknown token when expecting an expression".
  if (Def->ArgCount > 0 && CurTok == ')') {
    std::string argWord = (Def->ArgCount == 1) ? "argument" : "arguments";
    LogErrorAt(CallLoc,
      "'" + Name + "' requires exactly " + std::to_string(Def->ArgCount) +
      " " + argWord + ", but 0 were given." +
      "\n  Hint: Pass an expression inside the parentheses, e.g. " + Name + "(expr)");
    return nullptr;
  }

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

  // ── Too-many-arguments guard ─────────────────────────────────────────────
  // After consuming all expected arguments, if we see ',' the user passed
  // more than the function accepts.
  if (CurTok == ',') {
    std::string argWord = (Def->ArgCount == 1) ? "argument" : "arguments";
    LogErrorAt(CurLoc,
      "'" + Name + "' takes exactly " + std::to_string(Def->ArgCount) +
      " " + argWord + ", but more were given." +
      "\n  Hint: Remove the extra argument(s) — only " +
      std::to_string(Def->ArgCount) + " " + argWord + " allowed.");
    return nullptr;
  }

  if (CurTok != ')') {
    LogErrorAt(CurLoc, "Expected ')' to close the call to '" + Name + "'");
    return nullptr;
  }
  getNextToken(); // eat ')'

  return std::make_unique<BuiltinCallExprAST>(CallLoc, Name, std::move(Args));
}

std::unique_ptr<ExprAST> ParseWhileExpr() {
  SourceLocation KeywordLoc = CurLoc;
  std::string IdName = IdentifierStr;
  getNextToken();

  if (CurTok == TOK_ASSIGN) {
    getNextToken();
    auto RHS = ParseExpression();
    if (!RHS)
      return nullptr;
    return std::make_unique<AssignmentExprAST>(KeywordLoc, IdName,
                                               std::move(RHS));
  }

  auto Cond = ParseExpression();
  if (!Cond) {
    return nullptr;
  }

  if (CurTok != '{') {
    LogErrorAt(KeywordLoc, "Expected '{' to open the while-loop body."
               "\n  Hint: while loops must use braces: while condition { ... }");
    return nullptr;
  }

  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  return std::make_unique<WhileExprAST>(KeywordLoc, std::move(Cond),
                                        std::move(Body));
}

// ParseForExpr - parses:
//   for <ident> in <start>..<end> step <step_expr> { body }
std::unique_ptr<ExprAST> ParseForExpr() {
  SourceLocation ForLoc = CurLoc;
  getNextToken();

  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected a loop variable name after 'for'."
               "\n  Hint: Correct syntax: for i in 0..10 step i++ { }");
    return nullptr;
  }
  std::string VarName = IdentifierStr;
  getNextToken();

  if (CurTok != TOK_IN) {
    LogErrorAt(ForLoc, "Expected 'in' after the loop variable in a for-loop."
               "\n  Hint: Correct syntax: for " + VarName + " in 0..10 step " + VarName + "++ { }");
    return nullptr;
  }
  getNextToken();

  auto Start = ParseExpression();
  if (!Start)
    return nullptr;
  if (CurTok != TOK_RANGE) {
    LogErrorAt(ForLoc, "Expected '..' between the start and end values of the range."
               "\n  Hint: Correct syntax: for " + VarName + " in 0..10 step " + VarName + "++ { }");
    return nullptr;
  }
  getNextToken();

  auto End = ParseExpression();
  if (!End)
    return nullptr;
  if (CurTok != TOK_STEP) {
    LogErrorAt(ForLoc, "Expected 'step' after the range end value."
               "\n  Hint: Correct syntax: for " + VarName + " in 0..10 step " + VarName + "++ { }");
    return nullptr;
  }
  getNextToken();

  auto Step = ParseExpression();
  if (!Step)
    return nullptr;

  if (CurTok != '{') {
    LogErrorAt(ForLoc, "Expected '{' to open the for-loop body."
               "\n  Hint: Correct syntax: for " + VarName + " in 0..10 step " + VarName + "++ { }");
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
// ParseType() now produces ArrayType for array types (e.g. int[5]), so we
// simply check isArrayType() here instead of intercepting '[' ourselves.
std::unique_ptr<ExprAST> ParseVarDeclBody(SourceLocation TypeLoc,
                                          TurfTypePtr Type) {
  // If ParseType() already consumed [N] and built an ArrayType, handle it here.
  if (isArrayType(Type.get())) {
    const auto *AT = dynamic_cast<const ::ArrayType *>(Type.get());
    int ArraySize = AT ? AT->getSize() : -1;
    TurfTypePtr ElemType = AT ? AT->getElementTypePtr() : Type;

    bool IsKeyword = Keywords.find(IdentifierStr) != Keywords.end() &&
                     Keywords.at(IdentifierStr) == CurTok;
    if (CurTok != TOK_IDENTIFIER && !IsKeyword) {
      LogErrorAt(CurLoc, "Expected a variable name after the array type, e.g. int[5] arr");
      return nullptr;
    }

    std::string Name = IdentifierStr;
    SourceLocation NameLoc = CurLoc;
    getNextToken();

    // Optional initializer: int[3] primes = [2, 3, 5]
    std::vector<std::unique_ptr<ExprAST>> InitList;
    if (CurTok == TOK_ASSIGN) {
      getNextToken();

      if (CurTok != '[') {
        LogErrorAt(CurLoc, "Expected '[' to begin the array initializer — e.g. int[3] arr = [1, 2, 3]");
        return nullptr;
      }
      getNextToken();

      while (CurTok != ']' && CurTok != TOK_EOF) {
        auto Elem = ParseExpression();
        if (!Elem)
          return nullptr;
        InitList.push_back(std::move(Elem));
        if (CurTok == ',')
          getNextToken();
        else if (CurTok != ']')
          break;
      }

      if (CurTok != ']') {
        LogErrorAt(CurLoc, "Expected ']' to close the array initializer — check for a missing ']' or a stray element");
        return nullptr;
      }
      getNextToken();
    }

    return std::make_unique<ArrayDeclExprAST>(NameLoc, Name, ElemType, ArraySize,
                                              std::move(InitList));
  }

  // Standard variable declaration: int x = ...
  bool IsKeyword = Keywords.find(IdentifierStr) != Keywords.end() &&
                   Keywords.at(IdentifierStr) == CurTok;
  if (CurTok != TOK_IDENTIFIER && !IsKeyword) {
    LogErrorAt(CurLoc, "Expected a variable name after the type keyword."
               "\n  Hint: Variable declarations look like: int x = 5");
    return nullptr;
  }

  std::string Name = IdentifierStr;
  SourceLocation NameLoc = CurLoc;
  getNextToken();

  // list<T> declarations need no initializer — codegen zero-inits via malloc.
  // e.g. `list<int> nums` is valid without `= ...`
  if (isListType(Type.get())) {
    auto Init = std::make_unique<NumberExprAST>(0LL); // dummy, ignored by codegen
    return std::make_unique<VarDeclExprAST>(NameLoc, Name, Type, std::move(Init));
  }

  if (CurTok != TOK_ASSIGN) {
    LogErrorAt(NameLoc, "Expected '=' after variable name '" + Name + "'."
               "\n  Hint: Variables must be initialised: int " + Name + " = <value>");
    return nullptr;
  }

  getNextToken();
  auto Init = ParseExpression();
  if (!Init)
    return nullptr;

  return std::make_unique<VarDeclExprAST>(NameLoc, Name, Type, std::move(Init));
}

// ParseVarDecl - original entry point (type token is still in CurTok).
std::unique_ptr<ExprAST> ParseVarDecl() {
  SourceLocation TypeLoc = CurLoc;
  TurfTypePtr Type = ParseType(); // consumes the type token
  if (!Type) return nullptr;
  return ParseVarDeclBody(TypeLoc, Type);
}

// ParseCastExpr - called after the type keyword has been consumed and
// CurTok == '('. Parses int(expr) or double(expr).
std::unique_ptr<ExprAST> ParseCastExpr(TurfTypePtr DestType, SourceLocation Loc) {
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

  // Parse return type using ParseType() to support generics in the future
  TurfTypePtr RetType = ParseType(); // consumes the return type token
  if (!RetType) return nullptr;

  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(FnLoc, "Expected a function name after the return type."
               "\n  Hint: Function syntax: fn int myFunc(int x) { return x }");
    return nullptr;
  }
  std::string FuncName = IdentifierStr;
  getNextToken();

  if (CurTok != '(') {
    LogErrorAt(FnLoc, "Expected '(' after function name '" + FuncName + "'."
               "\n  Hint: Function syntax: fn int " + FuncName + "(int x) { return x }");
    return nullptr;
  }
  getNextToken();

  std::vector<ParamDecl> Params;
  while (CurTok != ')' && CurTok != TOK_EOF) {
    // Parse each parameter: <type> <name>
    // Parse each parameter: <type> <name>  (supports list<T> params too)
    // All parameter types (including list<T>) are parsed uniformly by ParseType()
    TurfTypePtr ParamType = ParseType();
    if (!ParamType) {
      LogErrorAt(CurLoc, "Expected a parameter type (int, double, bool, string, list<T>) in the parameter list of '" + FuncName + "'."
                 "\n  Hint: Each parameter must have a type: fn int " + FuncName + "(int x, string y) { ... }");
      return nullptr;
    }

    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected a parameter name after the type in '" + FuncName + "'."
                 "\n  Hint: Each parameter needs both a type and a name: fn int " + FuncName + "(int x) { ... }");
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
    LogErrorAt(FnLoc, "Expected ')' to close the parameter list of '" + FuncName + "'."
               "\n  Hint: Check for a missing ')' or a malformed parameter.");
    return nullptr;
  }
  getNextToken();

  // Parse body block
  if (CurTok != '{') {
    LogErrorAt(FnLoc, "Expected '{' to open the body of function '" + FuncName + "'."
               "\n  Hint: Function syntax: fn int " + FuncName + "(...) { ... }");
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

// ParseListDecl is now replaced by the unified type dispatch in ParsePrimary.
// list<T> declarations go through ParseVarDeclBody with a GenericType from ParseType().
// This function is kept as a named reference point but is no longer called.

