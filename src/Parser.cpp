/**
 * @file Parser.cpp
 * @brief Recursive-descent parser for TurfLang.
 * @details Translates the stream of tokens provided by the Lexer into an
 *          Abstract Syntax Tree (AST). Includes precedence parsing for binary
 *          expressions and structured parsing for all language constructs.
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
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

static std::unordered_set<std::string> ImportedFiles;
// Current state of the Parser
int CurTok; // The current token the parser is looking at
static std::map<int, int> BinopPrecedence; // Precedence table: '*' > '+'
static std::vector<std::unique_ptr<ExprAST>> ParsedTopLevelASTs;

// Issue #1 fix: When parsing an if/while/for condition, we must NOT allow
// struct-brace-init syntax (Identifier '{' ...) because '{' is ambiguous
// with the block opener. This mirrors Rust's grammar restriction.
static bool g_AllowStructInit = true;
struct NoStructInitGuard {
  bool Previous;
  NoStructInitGuard() : Previous(g_AllowStructInit) { g_AllowStructInit = false; }
  ~NoStructInitGuard() { g_AllowStructInit = Previous; }
};

struct SymbolScopeGuard {
  bool Active;
  SymbolScopeGuard(bool ShouldEnter) : Active(ShouldEnter) {
    if (Active && GlobalSymbolTable) GlobalSymbolTable->EnterScope();
  }
  ~SymbolScopeGuard() {
    if (Active && GlobalSymbolTable) GlobalSymbolTable->ExitScope();
  }
};

// Reads the next token from the Lexer and updates CurTok
int getNextToken() { return CurTok = gettok(); }

void InitializePrecedence() {
  // Cast
  BinopPrecedence[TOK_TO] = 5;

  // Assignment
  BinopPrecedence['='] = 10;
  BinopPrecedence[TOK_ASSIGN] = 10;
  BinopPrecedence[TOK_PLUS_ASSIGN] = 10;
  BinopPrecedence[TOK_MINUS_ASSIGN] = 10;
  BinopPrecedence[TOK_MUL_ASSIGN] = 10;
  BinopPrecedence[TOK_DIV_ASSIGN] = 10;
  BinopPrecedence[TOK_MOD_ASSIGN] = 10;

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
  ImportedFiles.clear();
}

std::vector<std::unique_ptr<ExprAST>> SwapParsedTopLevelASTs(std::vector<std::unique_ptr<ExprAST>> New) {
  std::vector<std::unique_ptr<ExprAST>> Old = std::move(ParsedTopLevelASTs);
  ParsedTopLevelASTs = std::move(New);
  return Old;
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
  auto It = BinopPrecedence.find(CurTok);
  if (It == BinopPrecedence.end() || It->second <= 0)
    return -1;
  return It->second;
}

std::unique_ptr<ExprAST> ParseExpression();
std::unique_ptr<ExprAST> ParseUnary();
std::unique_ptr<ExprAST> ParseBlock();

std::unique_ptr<ExprAST> ParseWhileExpr();
std::unique_ptr<ExprAST> ParseForExpr();
std::unique_ptr<ExprAST> ParseLetStatement(bool IsImmutable);
std::unique_ptr<ExprAST> ParseVarDecl();
std::unique_ptr<ExprAST> ParseVarDeclBody(SourceLocation TypeLoc,
                                          TurfTypePtr Type);
std::unique_ptr<ExprAST> ParseBoolExpr();
std::unique_ptr<ExprAST> ParseStringExpr();
std::unique_ptr<ExprAST> ParseFuncDef(std::string IntrinsicName);
std::unique_ptr<ExprAST> ParseFile(const std::string &Path);
std::unique_ptr<ExprAST> ParseImport();
std::unique_ptr<ExprAST> ParseReturnExpr();
std::unique_ptr<ExprAST> ParseStructDecl();
std::unique_ptr<ExprAST> ParseExternDecl();
// NOTE: ParseListDecl removed — list<T> declarations go through
// ParseVarDeclBody
//       via the unified ParseType() path, same as int/double/bool/string.

// ParseType: recursively parse a type expression from the token stream.
// Handles primitives, void, generics like list<int>, and arrays like int[5].
// Trailing [N] brackets are consumed here, making array types fully
// first-class. Returns nullptr on failure.
static TurfTypePtr ParseType() {
  if (CurTok != TOK_IDENTIFIER && CurTok != TOK_TYPE_INT &&
      CurTok != TOK_TYPE_DOUBLE && CurTok != TOK_TYPE_BOOL &&
      CurTok != TOK_TYPE_STRING && CurTok != TOK_TYPE_VOID &&
      CurTok != TOK_PTR) {
    SyntaxError(CurLoc, "Expected a type name").raise();
    return nullptr;
  }

  std::string BaseName;
  if (CurTok == TOK_TYPE_INT)
    BaseName = "int";
  else if (CurTok == TOK_TYPE_DOUBLE)
    BaseName = "double";
  else if (CurTok == TOK_TYPE_BOOL)
    BaseName = "bool";
  else if (CurTok == TOK_TYPE_STRING)
    BaseName = "string";
  else if (CurTok == TOK_TYPE_VOID)
    BaseName = "void";
  else if (CurTok == TOK_PTR) {
    // `ptr` is a terminal type — no generics, no arrays (for now)
    getNextToken(); // consume 'ptr'
    return TurfPtr();
  }
  else
    BaseName = IdentifierStr; // user-defined

  getNextToken(); // consume the type name

  // Check for generic parameter: BaseName<T1, T2, ...>
  if (CurTok == '<') {
    getNextToken(); // consume '<'
    std::vector<TurfTypePtr> TypeArgs;
    while (true) {
      TurfTypePtr Arg = ParseType();
      if (!Arg)
        return nullptr;
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
    TurfTypePtr GenBase =
        std::make_shared<GenericType>(BaseName, std::move(TypeArgs));
    while (CurTok == '*') {
      getNextToken();
      GenBase = std::make_shared<TurfPointerType>(GenBase);
    }
    std::vector<int> GenArraySizes;
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
      GenArraySizes.push_back(Size);
    }
    for (auto it = GenArraySizes.rbegin(); it != GenArraySizes.rend(); ++it) {
      GenBase = makeArrayType(GenBase, *it);
    }
    return GenBase;
  }

  // Primitive / named type
  TurfTypePtr Base;
  if (BaseName == "int")
    Base = TurfInt();
  else if (BaseName == "double")
    Base = TurfDouble();
  else if (BaseName == "bool")
    Base = TurfBool();
  else if (BaseName == "string")
    Base = TurfString();
  else if (BaseName == "void")
    Base = TurfVoid();
  else if (BaseName == "List") {
    // "list" without <T> is an error — give a helpful message
    SyntaxError(CurLoc, "Expected '<' after 'list' — e.g. list<int>").raise();
    return nullptr;
  } else {
    Base = std::make_shared<NamedType>(BaseName);
  }

  while (CurTok == '*') {
    getNextToken();
    Base = std::make_shared<TurfPointerType>(Base);
  }

  // Recursively consume trailing array dimensions: int[5] or int[5][10]
  // Size may be omitted (int[]) to indicate a pointer-parameter (size = -1).
  std::vector<int> ArraySizes;
  while (CurTok == '[') {
    getNextToken(); // eat '['
    int Size = -1;
    if (CurTok == TOK_INT_LITERAL) {
      Size = static_cast<int>(IntVal);
      if (Size <= 0) {
        SyntaxError(CurLoc,
                    "Array size must be a positive integer (greater than 0)")
            .raise();
        return nullptr;
      }
      getNextToken(); // eat the literal
    }
    if (CurTok != ']') {
      SyntaxError(CurLoc, "Expected ']' to close array size — e.g. int[5] arr")
          .raise();
      return nullptr;
    }
    getNextToken(); // eat ']'
    ArraySizes.push_back(Size);
  }
  for (auto it = ArraySizes.rbegin(); it != ArraySizes.rend(); ++it) {
    Base = makeArrayType(Base, *it);
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
  bool SavedBoolVal = BoolVal;
  getNextToken();
  return std::make_unique<BoolExprAST>(SavedBoolVal);
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
                                                   MethodName, std::move(Args));
      } else {
        if (MethodName == "length") {
          Base = std::make_unique<MethodCallExprAST>(
              Loc, std::move(Base), "length",
              std::vector<std::unique_ptr<ExprAST>>{});
        } else {
          Base = std::make_unique<FieldAccessExprAST>(Loc, std::move(Base),
                                                      MethodName);
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

// Parse Parentheses
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken(); // eat '('
  auto V = ParseExpression();
  if (!V)
    return nullptr;

  if (CurTok != ')') {
    SyntaxError(CurLoc,
                "Missing closing ')' — every '(' must have a matching ')'")
        .raise();
    return nullptr;
  }
  getNextToken(); // eat ')'
  return V;
}
std::unique_ptr<ExprAST> ParseIfExpr() {
  SourceLocation KeywordLoc = CurLoc;
  getNextToken(); // eat 'if'

  std::unique_ptr<ExprAST> Cond;
  {
    NoStructInitGuard Guard;
    Cond = ParseExpression();
  }
  if (!Cond)
    return nullptr;

  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' after if-condition. Use 'give ... otherwise ...' for single-expression ternary.");
    return nullptr;
  }

  auto Then = ParseBlock();
  if (!Then)
    return nullptr;

  std::vector<CondBranch> Branches;
  Branches.push_back({KeywordLoc, std::move(Cond), std::move(Then)});
  while (CurTok == TOK_ELSEIF) {
    SourceLocation ElseifLoc = CurLoc;
    getNextToken(); // eat 'elseif'

    std::unique_ptr<ExprAST> ElseifCond;
    {
      NoStructInitGuard Guard;
      ElseifCond = ParseExpression();
    }
    if (!ElseifCond)
      return nullptr;

    if (CurTok != '{') {
      LogErrorAt(ElseifLoc, "Expected '{' to open the elseif-block body."
                            "\n  Hint: 'elseif' blocks must always use braces: "
                            "elseif condition { ... }");
      return nullptr;
    }
    auto ElseifBody = ParseBlock();
    if (!ElseifBody)
      return nullptr;

    Branches.push_back(
        {ElseifLoc, std::move(ElseifCond), std::move(ElseifBody)});
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
static std::unique_ptr<ExprAST> ParsePrimaryBase() {
  switch (CurTok) {
  default:

    LogErrorAt(CurLoc, "Unexpected token — expected a value, variable name, or "
                       "expression here."
                       "\n  Hint: Check for stray characters, a missing "
                       "operator, or an incomplete statement.");
    return nullptr;

  case TOK_SIZEOF: {
    SourceLocation Loc = CurLoc;
    getNextToken(); // eat sizeof
    if (CurTok != '(') {
      LogErrorAt(CurLoc, "Expected '(' after sizeof");
      return nullptr;
    }
    getNextToken(); // eat '('
    TurfTypePtr Ty = ParseType();
    if (!Ty)
      return nullptr;
    if (CurTok != ')') {
      LogErrorAt(CurLoc, "Expected ')' to close sizeof");
      return nullptr;
    }
    getNextToken(); // eat ')'
    return std::make_unique<SizeOfExprAST>(Loc, Ty);
  }

  case TOK_SELF:
  case TOK_IDENTIFIER: {
    // Snapshot the identifier, then advance so CurTok reflects what follows.
    std::string IdName = IdentifierStr;
    SourceLocation IdLoc = CurLoc;
    getNextToken(); // consume the identifier; CurTok now = next token

    // ── Struct brace-init: Identifier '{' field: expr, ... '}' ──────────────
    // Checked before the keyword-typo guard so `Point { ... }` is never
    // mistaken for a typo.
    if (g_AllowStructInit && CurTok == '{') {
      getNextToken(); // eat '{'
      std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> NamedFields;
      while (CurTok != '}' && CurTok != TOK_EOF) {
        if (CurTok != TOK_IDENTIFIER) {
          LogErrorAt(CurLoc, "Expected field name in struct initializer");
          return nullptr;
        }
        std::string FieldName = IdentifierStr;
        getNextToken(); // eat field name
        if (CurTok != TOK_COLON) {
          LogErrorAt(CurLoc,
                     "Expected ':' after field name '" + FieldName +
                         "' in struct initializer");
          return nullptr;
        }
        getNextToken(); // eat ':'
        auto Val = ParseExpression();
        if (!Val)
          return nullptr;
        NamedFields.emplace_back(FieldName, std::move(Val));
        if (CurTok == ',')
          getNextToken(); // eat ','
        else if (CurTok != '}')
          break;
      }
      if (CurTok != '}') {
        LogErrorAt(CurLoc,
                   "Expected '}' to close struct initializer for '" + IdName +
                       "'");
        return nullptr;
      }
      getNextToken(); // eat '}'
      return std::make_unique<StructInitExprAST>(IdLoc, IdName,
                                                 std::move(NamedFields));
    }

    bool CouldBeKeywordTypo = false;

    // Only flag a typo if this name is NOT already a known variable.
    bool IsKnownVariable = false;
    if (GlobalSymbolTable)
      IsKnownVariable = (GlobalSymbolTable->LookupSymbol(IdName) != nullptr);
    if (!IsKnownVariable)
      IsKnownVariable = (NamedValues.find(IdName) != NamedValues.end());

    // Do not flag a keyword typo if the token was actually TOK_SELF.
    if (!IsKnownVariable && IdName != "self" && IdName.length() >= 2) {
      CouldBeKeywordTypo = !SuggestionIndex::suggestKeywords(IdName, 1).empty();
    }

    if (CouldBeKeywordTypo) {
      // Allow: assignment, dot-access, subscript, function call
      if (CurTok != TOK_ASSIGN && CurTok != '.' && CurTok != '[' &&
          CurTok != '(') {
        KeywordError(IdLoc, IdName).raise();
        return nullptr;
      }
    }

    auto Base = std::make_unique<VariableExprAST>(IdLoc, IdName);
    return Base;
  }

  case TOK_NUMBER:
    return ParseNumberExpr(false);

  case TOK_INT_LITERAL:
    return ParseNumberExpr(true);

  case TOK_BOOL_LITERAL:
    return ParseBoolExpr();

  case TOK_STRING_LITERAL:
    return ParseStringExpr();
    
  case TOK_NULL:
    return ParseNullExpr();

  case '[': {
    SourceLocation Loc = CurLoc;
    getNextToken(); // eat '['
    std::vector<std::unique_ptr<ExprAST>> Elements;
    while (CurTok != ']' && CurTok != TOK_EOF) {
      if (auto Val = ParseExpression()) {
        Elements.push_back(std::move(Val));
      } else {
        return nullptr;
      }
      if (CurTok == ',') {
        getNextToken(); // eat ','
      } else if (CurTok != ']') {
        LogErrorAt(CurLoc, "Expected ',' or ']' in array literal");
        return nullptr;
      }
    }
    if (CurTok != ']') {
      LogErrorAt(CurLoc, "Expected ']' to close array literal");
      return nullptr;
    }
    getNextToken(); // eat ']'
    return std::make_unique<ArrayLiteralExprAST>(Loc, std::move(Elements));
  }

  case '(':
    return ParseParenExpr();

  case TOK_IF:
    return ParseIfExpr();

  case TOK_WHILE:
    return ParseWhileExpr();

  case TOK_FOR:
    return ParseForExpr();

  case TOK_ELSE: {
    SourceLocation KeywordLoc = CurLoc;
    std::string IdName = IdentifierStr;
    getNextToken();
    LogErrorAt(KeywordLoc,
               "Unexpected '" + IdName +
                   "' keyword here — expected a value or expression."
                   "\n  Hint: '" +
                   IdName +
                   "' is a control-flow keyword; it can't appear standalone in "
                   "an expression.");
    return nullptr;
  }

  case TOK_TYPE_INT:
  case TOK_TYPE_DOUBLE:
  case TOK_TYPE_STRING:
  case TOK_TYPE_BOOL:
    LogErrorAt(CurLoc, "Expected 'mutable' or 'immutable' to begin a declaration.");
    return nullptr;

  case TOK_TYPE_VOID:
    SyntaxError(CurLoc, "'void' may only be used as a function return type")
        .raise();
    return nullptr;

  case TOK_FN:
    return ParseFuncDef("");

  case TOK_AT: {
    SourceLocation AtLoc = CurLoc;
    getNextToken();
    if (CurTok != TOK_IDENTIFIER || IdentifierStr != "intrinsic") {
      LogErrorAt(AtLoc, "Expected 'intrinsic' after '@'");
      return nullptr;
    }
    getNextToken();
    if (CurTok != '(') {
      LogErrorAt(CurLoc, "Expected '(' after @intrinsic");
      return nullptr;
    }
    getNextToken();
    if (CurTok != TOK_STRING_LITERAL) {
      LogErrorAt(CurLoc, "Expected string literal for intrinsic name");
      return nullptr;
    }
    std::string IntrinsicName = StringVal;
    getNextToken();
    if (CurTok != ')') {
      LogErrorAt(CurLoc, "Expected ')' after intrinsic name");
      return nullptr;
    }
    getNextToken(); // eat ')'
    if (CurTok != TOK_FN) {
      LogErrorAt(CurLoc, "Expected 'fn' after @intrinsic decorator");
      return nullptr;
    }
    return ParseFuncDef(IntrinsicName);
  }

  case TOK_IMPORT:
    return ParseImport();

  case TOK_STRUCT:
    return ParseStructDecl();

  case TOK_EXTERN:
    return ParseExternDecl();


  case TOK_RETURN:
    return ParseReturnExpr();
  case TOK_BREAK: {
    SourceLocation KeywordLoc = CurLoc;
    getNextToken();
    return std::make_unique<BreakExprAST>(KeywordLoc);
  }

  case TOK_CONTINUE: {
    SourceLocation KeywordLoc = CurLoc;
    getNextToken();
    return std::make_unique<ContinueExprAST>(KeywordLoc);
  }
  }
}

static std::unique_ptr<ExprAST> ParsePrimary() {
  auto Base = ParsePrimaryBase();
  if (!Base)
    return nullptr;
  return ParsePostfixExpression(std::move(Base));
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

    if (BinOp == TOK_TO) {
      auto CastType = ParseType();
      if (!CastType)
        return nullptr;
      LHS = std::make_unique<CastExprAST>(OpLoc, CastType, std::move(LHS));
      continue;
    }

    // Parse the primary expression after the binary operator
    auto RHS = ParseUnary();
    if (!RHS)
      return nullptr;

    // LOOKAHEAD: Is the *next* operator even stronger?
    int NextPrec = GetTokPrecedence();
    bool isRightAssociative =
        (BinOp == TOK_ASSIGN || BinOp == TOK_PLUS_ASSIGN ||
         BinOp == TOK_MINUS_ASSIGN || BinOp == TOK_MUL_ASSIGN ||
         BinOp == TOK_DIV_ASSIGN || BinOp == TOK_MOD_ASSIGN);
    if (TokPrec < NextPrec || (isRightAssociative && TokPrec == NextPrec)) {
      // Recursively parse the high-priority part first
      RHS = ParseBinOpRHS(isRightAssociative ? TokPrec : TokPrec + 1,
                          std::move(RHS));
      if (!RHS)
        return nullptr;
    }

    // Merge LHS and RHS into a new node
    if (BinOp == TOK_ASSIGN) {
      LHS = std::make_unique<AssignmentExprAST>(OpLoc, std::move(LHS),
                                                std::move(RHS));
    } else if (BinOp == TOK_PLUS_ASSIGN || BinOp == TOK_MINUS_ASSIGN ||
               BinOp == TOK_MUL_ASSIGN || BinOp == TOK_DIV_ASSIGN ||
               BinOp == TOK_MOD_ASSIGN) {
      char Op = '+';
      if (BinOp == TOK_MINUS_ASSIGN)
        Op = '-';
      else if (BinOp == TOK_MUL_ASSIGN)
        Op = '*';
      else if (BinOp == TOK_DIV_ASSIGN)
        Op = '/';
      else if (BinOp == TOK_MOD_ASSIGN)
        Op = '%';

      LHS = std::make_unique<CompoundAssignmentExprAST>(OpLoc, Op, std::move(LHS),
                                                        std::move(RHS));
    } else {
      LHS = std::make_unique<BinaryExprAST>(OpLoc, BinOp, std::move(LHS),
                                            std::move(RHS));
    }
  }
}

/**
 * @brief Parses an expression from the current token stream.
 *
 * Uses operator precedence parsing to build the correct AST structure for
 * primary expressions combined with binary operators.
 *
 * @return std::unique_ptr<ExprAST> The fully constructed expression AST.
 */
std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParseUnary();
  if (!LHS)
    return nullptr;

  LHS = ParseBinOpRHS(0, std::move(LHS));
  if (!LHS)
    return nullptr;

  if (CurTok == TOK_GIVE) {
    SourceLocation OpLoc = CurLoc;
    getNextToken(); // consume 'give'
    auto ThenExpr = ParseExpression();
    if (!ThenExpr)
      return nullptr;
    if (CurTok != TOK_OTHERWISE) {
      LogErrorAt(CurLoc, "Expected 'otherwise' after 'give' expression in ternary");
      return nullptr;
    }
    SourceLocation ElseLoc = CurLoc;
    getNextToken(); // consume 'otherwise'
    auto ElseExpr = ParseExpression();
    if (!ElseExpr)
      return nullptr;

    std::vector<CondBranch> Branches;
    Branches.push_back({OpLoc, std::move(LHS), std::move(ThenExpr)});
    return std::make_unique<IfExprAST>(std::move(Branches), std::move(ElseExpr), ElseLoc);
  }

  return LHS;
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
    auto LHSVarBin = std::make_unique<VariableExprAST>(Loc, IdName);
    auto OneNum = std::make_unique<NumberExprAST>(1LL);
    auto RHS = std::make_unique<BinaryExprAST>(CurLoc, Op, std::move(LHSVarBin),
                                               std::move(OneNum));

    auto LHSVarAssign = std::make_unique<VariableExprAST>(Loc, IdName);
    return std::make_unique<AssignmentExprAST>(Loc, std::move(LHSVarAssign),
                                               std::move(RHS));
  }

  // Address-of: &expr — takes the address of an lvalue.
  // The lexer already returns '&' as ASCII for a single ampersand;
  // '&&' is tokenised as TOK_AND and never reaches here.
  if (CurTok == '&') {
    SourceLocation Loc = CurLoc;
    getNextToken(); // eat '&'
    auto Operand = ParseUnary();
    if (!Operand)
      return nullptr;
    return std::make_unique<AddressOfExprAST>(Loc, std::move(Operand));
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

    if (CurTok == TOK_MUTABLE || CurTok == TOK_IMMUTABLE) {
      auto Expr = ParseLetStatement(CurTok == TOK_IMMUTABLE);
      if (!Expr) return nullptr;
      Exprs.push_back(std::move(Expr));
      continue;
    }
    if (CurTok == TOK_IF || CurTok == TOK_WHILE || CurTok == TOK_FOR ||
        CurTok == TOK_FN || CurTok == TOK_STRUCT) {
      auto Expr = ParseExpression();
      if (!Expr) return nullptr;
      if (CurTok == ';') {
        LogErrorAt(CurLoc, "Unexpected ';'");
        return nullptr;
      }
      Exprs.push_back(std::move(Expr));
      continue;
    }

    std::string PossibleKeyword = "";
    if (CurTok == TOK_IDENTIFIER) {
      PossibleKeyword = IdentifierStr;
    }

    auto Expr = ParseExpression();
    if (!Expr) return nullptr;
    if (CurTok != ';') {
      std::string Hint = "";
      if (!PossibleKeyword.empty()) {
        static const std::vector<std::string> Keywords = {
            "if", "while", "for", "mutable", "immutable", "mut", "immut", 
            "return", "compare", "do", "fn", "struct"};
        for (const auto &KW : Keywords) {
          int MaxDist = KW.length() <= 3 ? 1 : 2;
          if (getDamerauLevenshteinDistance(PossibleKeyword, KW) <= MaxDist) {
            Hint = "\n  Hint: Did you mean '" + KW + "' instead of '" +
                   PossibleKeyword + "'?";
            break;
          }
        }
      }
      LogErrorAt(CurLoc, "Expected ';' after statement." + Hint);
      return nullptr;
    }
    getNextToken(); // eat ';'
    Exprs.push_back(std::move(Expr));
  }

  if (CurTok != '}') {
    LogErrorAt(OpenBraceLoc,
               "Missing closing '}' — every '{' must have a matching '}'."
               "\n  Hint: Check that all your blocks are properly closed.");
    return nullptr;
  }
  getNextToken();

  return std::make_unique<BlockExprAST>(std::move(Exprs));
}

std::unique_ptr<ExprAST> ParseWhileExpr() {
  SourceLocation KeywordLoc = CurLoc;
  getNextToken();

  std::unique_ptr<ExprAST> Cond;
  {
    NoStructInitGuard Guard;
    Cond = ParseExpression();
  }
  if (!Cond) {
    return nullptr;
  }

  if (CurTok != '{') {
    LogErrorAt(
        KeywordLoc,
        "Expected '{' to open the while-loop body."
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
                       "\n  Hint: Correct syntax: for " +
                           VarName + " in 0..10 step " + VarName + "++ { }");
    return nullptr;
  }
  getNextToken();

  // Suppress struct-init inside for range/step expressions to avoid
  // ambiguity between a trailing identifier and the block opener '{'
  std::unique_ptr<ExprAST> Start, End, Step;
  {
    NoStructInitGuard Guard;
    Start = ParseExpression();
  }
  if (!Start)
    return nullptr;
  if (CurTok != TOK_RANGE) {
    LogErrorAt(ForLoc,
               "Expected '..' between the start and end values of the range."
               "\n  Hint: Correct syntax: for " +
                   VarName + " in 0..10 step " + VarName + "++ { }");
    return nullptr;
  }
  getNextToken();

  {
    NoStructInitGuard Guard;
    End = ParseExpression();
  }
  if (!End)
    return nullptr;
  if (CurTok != TOK_STEP) {
    LogErrorAt(ForLoc, "Expected 'step' after the range end value."
                       "\n  Hint: Correct syntax: for " +
                           VarName + " in 0..10 step " + VarName + "++ { }");
    return nullptr;
  }
  getNextToken();

  {
    NoStructInitGuard Guard;
    Step = ParseExpression();
  }
  if (!Step)
    return nullptr;

  if (CurTok != '{') {
    LogErrorAt(ForLoc, "Expected '{' to open the for-loop body."
                       "\n  Hint: Correct syntax: for " +
                           VarName + " in 0..10 step " + VarName + "++ { }");
    return nullptr;
  }

  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  return std::make_unique<ForExprAST>(ForLoc, VarName, std::move(Start),
                                      std::move(End), std::move(Step),
                                      std::move(Body));
}

std::unique_ptr<ExprAST> ParseLetStatement(bool IsImmutable) {
  SourceLocation Loc = CurLoc;
  getNextToken(); // eat mutable or immutable

  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected a variable name after 'mutable' or 'immutable'.");
    return nullptr;
  }

  std::string Name = IdentifierStr;
  SourceLocation NameLoc = CurLoc;
  getNextToken();

  if (CurTok != TOK_COLON) {
    LogErrorAt(CurLoc, "Expected ':' after variable name.");
    return nullptr;
  }
  getNextToken();

  TurfTypePtr Type = ParseType();
  if (!Type) return nullptr;

  if (CurTok != TOK_ASSIGN) {
    LogErrorAt(CurLoc, "Expected '=' after variable type.");
    return nullptr;
  }
  getNextToken();

  if (isArrayType(Type.get())) {
    const auto *AT = dynamic_cast<const ::ArrayType *>(Type.get());
    int ArraySize = AT ? AT->getSize() : -1;
    TurfTypePtr ElemType = AT ? AT->getElementTypePtr() : Type;

    std::vector<std::unique_ptr<ExprAST>> InitList;
    std::unique_ptr<ExprAST> InitExpr = nullptr;
    if (CurTok == '[') {
      getNextToken(); // eat '['

      if (CurTok != ']') {
        while (true) {
          auto Expr = ParseExpression();
          if (!Expr)
            return nullptr;
          InitList.push_back(std::move(Expr));

          if (CurTok == ']')
            break;
          if (CurTok != ',') {
            LogErrorAt(CurLoc, "Expected ',' or ']' in array initializer list");
            return nullptr;
          }
          getNextToken(); // eat ','
        }
      }
      getNextToken(); // eat ']'
    } else {
      InitExpr = ParseExpression();
      if (!InitExpr)
        return nullptr;
    }
    if (CurTok != ';') {
      LogErrorAt(CurLoc, "Expected ';' after statement.");
      return nullptr;
    }
    getNextToken(); // eat ';'
    return std::make_unique<ArrayDeclExprAST>(NameLoc, Name, ElemType, ArraySize, std::move(InitList), std::move(InitExpr), IsImmutable);
  }

  auto Init = ParseExpression();
  if (!Init) return nullptr;

  if (CurTok != ';') {
    LogErrorAt(CurLoc, "Expected ';' after statement.");
    return nullptr;
  }
  getNextToken(); // eat ';'
  return std::make_unique<VarDeclExprAST>(Loc, Name, Type, std::move(Init), IsImmutable);
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
      LogErrorAt(
          CurLoc,
          "Expected a variable name after the array type, e.g. int[5] arr");
      return nullptr;
    }

    std::string Name = IdentifierStr;
    SourceLocation NameLoc = CurLoc;
    getNextToken();

    // Optional initializer: int[3] primes = [2, 3, 5]
    std::vector<std::unique_ptr<ExprAST>> InitList;
    std::unique_ptr<ExprAST> InitExpr = nullptr;
    if (CurTok == TOK_ASSIGN) {
      getNextToken();
      InitExpr = ParseExpression();
      if (!InitExpr)
        return nullptr;
    }

    return std::make_unique<ArrayDeclExprAST>(NameLoc, Name, ElemType,
                                              ArraySize, std::move(InitList), std::move(InitExpr), false);
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

  std::unique_ptr<ExprAST> Init = nullptr;
  if (CurTok == TOK_ASSIGN) {
    getNextToken();
    Init = ParseExpression();
    if (!Init)
      return nullptr;
  }

  return std::make_unique<VarDeclExprAST>(NameLoc, Name, Type, std::move(Init), false);
}

// ParseVarDecl - original entry point (type token is still in CurTok).
std::unique_ptr<ExprAST> ParseVarDecl() {
  SourceLocation TypeLoc = CurLoc;
  TurfTypePtr Type = ParseType(); // consumes the type token
  if (!Type)
    return nullptr;
  return ParseVarDeclBody(TypeLoc, Type);
}

// ParseStructDecl - called when CurTok == TOK_STRUCT.
std::unique_ptr<ExprAST> ParseStructDecl() {
  SourceLocation Loc = CurLoc;
  getNextToken(); // eat 'struct'
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected identifier after 'struct'");
    return nullptr;
  }
  std::string Name = IdentifierStr;
  getNextToken();

  std::vector<std::string> TypeParams;
  if (CurTok == '<') {
    getNextToken(); // eat '<'
    while (CurTok == TOK_IDENTIFIER) {
      TypeParams.push_back(IdentifierStr);
      getNextToken();
      if (CurTok == ',') {
        getNextToken();
      } else {
        break;
      }
    }
    if (CurTok != '>') {
      LogErrorAt(CurLoc, "Expected '>' after generic type parameters");
      return nullptr;
    }
    getNextToken(); // eat '>'
  }

  // Push scope to allow ParseType to resolve generic parameters
  SymbolScopeGuard Guard(!TypeParams.empty());
  if (GlobalSymbolTable && !TypeParams.empty()) {
    for (const auto &TP : TypeParams) {
      GlobalSymbolTable->RegisterType(TP, std::make_shared<TurfGenericParamType>(TP));
    }
  }

  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' after struct name");
    return nullptr;
  }
  getNextToken(); // eat '{'

  std::vector<StructField> Fields;
  while (CurTok != '}' && CurTok != TOK_EOF) {
    // New syntax: name: type
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected field name in struct definition");
      return nullptr;
    }
    std::string FieldName = IdentifierStr;
    getNextToken(); // eat field name

    if (CurTok != TOK_COLON) {
      LogErrorAt(CurLoc,
                 "Expected ':' after field name '" + FieldName +
                     "' in struct definition (syntax: name: type)");
      return nullptr;
    }
    getNextToken(); // eat ':'

    TurfTypePtr FieldType = ParseType();
    if (!FieldType)
      return nullptr;

    Fields.push_back({FieldName, std::move(FieldType)});

    if (CurTok == ',') {
      getNextToken();
    } else if (CurTok != '}') {
      LogErrorAt(CurLoc, "Expected ',' or '}' after field definition");
      return nullptr;
    }
  }

  if (CurTok != '}') {
    LogErrorAt(CurLoc, "Expected '}' to close struct definition");
    return nullptr;
  }
  getNextToken(); // eat '}'



  // Register the struct type. For generics, fields will use TurfGenericParamType which compiles to void*
  auto SType = std::make_shared<TurfStructType>(Name, Fields);
  if (GlobalSymbolTable) {
    GlobalSymbolTable->RegisterType(Name, SType);
  }

  return std::make_unique<StructDeclExprAST>(Loc, Name, Fields, TypeParams);
}

// ParseExternDecl - called when CurTok == TOK_EXTERN.
// Parses: extern fn Name(paramName: paramType, ...): ReturnType;
std::unique_ptr<ExprAST> ParseExternDecl() {
  SourceLocation ExternLoc = CurLoc;
  getNextToken(); // eat 'extern'

  // Optional: 'from "libname"' annotation — specifies which library to link.
  // Syntax: extern from "ssl" fn SSL_new(ctx: ptr): ptr;
  // The library name is passed to the linker as -lssl.
  std::string LibName;
  if (CurTok == TOK_IDENTIFIER && IdentifierStr == "from") {
    getNextToken(); // eat 'from'
    if (CurTok != TOK_STRING_LITERAL) {
      LogErrorAt(CurLoc,
                 "Expected a library name string after 'from' "
                 "(e.g. extern from \"ssl\" fn SSL_new(ctx: ptr): ptr;).");
      // Panic-mode recovery: skip to ';' or EOF so the next statement parses.
      while (CurTok != ';' && CurTok != TOK_EOF) getNextToken();
      return nullptr;
    }
    LibName = StringVal;
    getNextToken(); // eat the library name string
  }

  // Expect 'fn'
  if (CurTok != TOK_FN) {
    LogErrorAt(ExternLoc,
               "Expected 'fn' after 'extern' — e.g. extern fn malloc(size: int): ptr;");
    return nullptr;
  }
  getNextToken(); // eat 'fn'

  // Expect function name
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected a function name after 'extern fn'.");
    return nullptr;
  }
  std::string Name = IdentifierStr;
  SourceLocation NameLoc = CurLoc;
  getNextToken(); // eat name

  // Expect '('
  if (CurTok != '(') {
    LogErrorAt(NameLoc,
               "Expected '(' after extern function name '" + Name + "'.");
    return nullptr;
  }
  getNextToken(); // eat '('

  // Parse parameter list: each param is: Identifier TOK_COLON ParseType()
  // Optionally terminated by '...' (variadic marker) as the final item.
  std::vector<std::pair<std::string, TurfTypePtr>> Params;
  bool IsVarArg = false;

  while (CurTok != ')' && CurTok != TOK_EOF) {
    // Check for '...' variadic marker — must be last in the list
    if (CurTok == TOK_ELLIPSIS) {
      IsVarArg = true;
      getNextToken(); // eat '...'
      break;          // nothing after '...' except ')'
    }

    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc,
                 "Expected a parameter name in extern function '" + Name + "'.");
      return nullptr;
    }
    std::string ParamName = IdentifierStr;
    getNextToken(); // eat param name

    if (CurTok != TOK_COLON) {
      LogErrorAt(CurLoc,
                 "Expected ':' after parameter name '" + ParamName +
                     "' in extern function '" + Name + "'.");
      return nullptr;
    }
    getNextToken(); // eat ':'

    TurfTypePtr ParamType = ParseType();
    if (!ParamType)
      return nullptr;

    Params.emplace_back(ParamName, std::move(ParamType));

    if (CurTok == ',') {
      getNextToken(); // eat ','
      // Allow '...' immediately after comma: printf(fmt: string, ...)
      if (CurTok == TOK_ELLIPSIS) {
        IsVarArg = true;
        getNextToken(); // eat '...'
        break;
      }
    } else if (CurTok != ')') {
      break;
    }
  }

  if (CurTok != ')') {
    LogErrorAt(ExternLoc,
               "Expected ')' to close parameter list of extern function '" +
                   Name + "'.");
    return nullptr;
  }
  getNextToken(); // eat ')'

  // Expect ':'
  if (CurTok != TOK_COLON) {
    LogErrorAt(CurLoc,
               "Expected ':' followed by return type after parameter list of "
               "extern function '" + Name + "'.");
    return nullptr;
  }
  getNextToken(); // eat ':'

  TurfTypePtr RetType = ParseType();
  if (!RetType) {
    LogErrorAt(CurLoc,
               "Expected return type after ':' in extern function '" + Name +
                   "'.");
    return nullptr;
  }

  // extern functions must NOT have a body
  if (CurTok == '{') {
    LogErrorAt(ExternLoc,
               "extern function declarations cannot have a body.");
    return nullptr;
  }

  // Expect ';'
  if (CurTok != ';') {
    LogErrorAt(CurLoc,
               "Expected ';' after extern function declaration '" + Name + "'.");
    return nullptr;
  }
  getNextToken(); // eat ';'

  return std::make_unique<ExternFuncDeclExprAST>(ExternLoc, Name,
                                                 std::move(Params),
                                                 std::move(RetType),
                                                 IsVarArg,
                                                 std::move(LibName));
}

// ParseFuncDef - called when CurTok == TOK_FN.
// Parses: fn <rettype> <name>(<type> <param>, ...) { body }
std::unique_ptr<ExprAST> ParseFuncDef(std::string IntrinsicName) {
  SourceLocation FnLoc = CurLoc;
  getNextToken(); // consume TOK_FN

  std::vector<std::string> TypeParams;
  if (CurTok == '<') {
    getNextToken(); // eat '<'
    while (CurTok == TOK_IDENTIFIER) {
      TypeParams.push_back(IdentifierStr);
      getNextToken();
      if (CurTok == ',') {
        getNextToken();
      } else {
        break;
      }
    }
    if (CurTok != '>') {
      LogErrorAt(CurLoc, "Expected '>' after generic type parameters");
      return nullptr;
    }
    getNextToken(); // eat '>'
  }

  // Push scope to allow ParseType to resolve generic parameters
  SymbolScopeGuard Guard(!TypeParams.empty());
  if (GlobalSymbolTable && !TypeParams.empty()) {
    for (const auto &TP : TypeParams) {
      GlobalSymbolTable->RegisterType(TP, std::make_shared<TurfGenericParamType>(TP));
    }
  }

  // Expect TOK_IDENTIFIER → save as FuncName
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(FnLoc, "Expected a function name after 'fn'.");
    return nullptr;
  }
  std::string FuncName = IdentifierStr;
  getNextToken();
  std::string StructName = "";

  if (CurTok == '.') {
    StructName = FuncName;
    getNextToken(); // eat '.'
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected method name after '.'");
      return nullptr;
    }
    FuncName = StructName + "." + IdentifierStr;
    getNextToken();
  }

  // Expect '('
  if (CurTok != '(') {
    LogErrorAt(FnLoc, "Expected '(' after function name '" + FuncName + "'.");
    return nullptr;
  }
  getNextToken(); // eat '('

  std::vector<ParamDecl> Params;
  if (!StructName.empty()) {
    TurfTypePtr StructType = nullptr;
    if (GlobalSymbolTable) {
      StructType = GlobalSymbolTable->LookupType(StructName);
    }
    if (!StructType) {
      LogErrorAt(FnLoc, "Unknown struct type '" + StructName +
                            "' in method declaration");
      return nullptr;
    }
    Params.push_back({std::make_shared<TurfPointerType>(StructType), "self"});
  }

  // Parse parameter list: each param is: Identifier TOK_COLON ParseType()
  // Params separated by ','
  while (CurTok != ')' && CurTok != TOK_EOF) {
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc,
                 "Expected a parameter name in the parameter list of '" +
                     FuncName + "'.");
      return nullptr;
    }
    std::string ParamName = IdentifierStr;
    getNextToken(); // consume parameter name

    if (CurTok != TOK_COLON) {
      LogErrorAt(CurLoc,
                 "Expected ':' after parameter name '" + ParamName + "' in '" +
                     FuncName + "'.");
      return nullptr;
    }
    getNextToken(); // consume ':'

    TurfTypePtr ParamType = ParseType();
    if (!ParamType) {
      LogErrorAt(CurLoc,
                 "Expected a parameter type after ':' in the parameter list of '" +
                     FuncName + "'.");
      return nullptr;
    }

    Params.push_back({ParamType, ParamName});

    if (CurTok == ',')
      getNextToken();
    else if (CurTok != ')')
      break;
  }

  // Expect ')'
  if (CurTok != ')') {
    LogErrorAt(
        FnLoc,
        "Expected ')' to close the parameter list of '" + FuncName + "'.");
    return nullptr;
  }
  getNextToken(); // eat ')'

  // Expect TOK_COLON
  if (CurTok != TOK_COLON) {
    LogErrorAt(CurLoc, "Expected ':' followed by return type after parameter list.");
    return nullptr;
  }
  getNextToken(); // eat ':'

  // Call ParseType() → save as ReturnType
  TurfTypePtr RetType = ParseType();
  if (!RetType) {
    LogErrorAt(CurLoc, "Expected return type.");
    return nullptr;
  }

  // If CurTok == ';': external declaration (forward decl)
  if (CurTok == ';') {
    getNextToken(); // eat ';'
    return std::make_unique<FuncDefExprAST>(FnLoc, FuncName, RetType, Params,
                                            nullptr, IntrinsicName, TypeParams);
  }

  // If we are @intrinsic, we might not have a body or semicolon
  if (!IntrinsicName.empty() && CurTok != '{') {
    return std::make_unique<FuncDefExprAST>(FnLoc, FuncName, RetType, Params,
                                            nullptr, IntrinsicName, TypeParams);
  }

  // Expect '{' to open function body
  if (CurTok != '{') {
    LogErrorAt(FnLoc, "Expected '{' to begin function body for '" + FuncName + "'.");
    return nullptr;
  }
  auto Body = ParseBlock();
  if (!Body)
    return nullptr;



  return std::make_unique<FuncDefExprAST>(FnLoc, FuncName, RetType, Params,
                                          std::move(Body), "", TypeParams);
}

std::unique_ptr<ExprAST> ParseFile(const std::string &Path) {
  if (ImportedFiles.count(Path)) {
    return std::make_unique<BlockExprAST>(
        std::vector<std::unique_ptr<ExprAST>>{});
  }

  std::ifstream ImportFile(Path);
  if (!ImportFile.is_open()) {
    return nullptr;
  }

  ImportedFiles.insert(Path);

  // Save current lexer state
  struct LexerStateGuard {
    std::ifstream *SavedSourceFile;
    int SavedLastChar, SavedCurTok, SavedCurLine, SavedCurCol;
    SourceLocation SavedCurLoc;
    std::string SavedFilename, SavedIdentifierStr, SavedStringVal;
    double SavedNumVal; long long SavedIntVal; bool SavedBoolVal;

    LexerStateGuard() {
      SavedSourceFile = SourceFile;
      SavedLastChar = GetLastChar();
      SavedCurTok = CurTok;
      SavedCurLoc = CurLoc;
      SavedCurLine = CurLine;
      SavedCurCol = CurCol;
      SavedFilename = SourceFilename;
      SavedNumVal = NumVal;
      SavedIntVal = IntVal;
      SavedBoolVal = BoolVal;
      SavedIdentifierStr = IdentifierStr;
      SavedStringVal = StringVal;
    }
    ~LexerStateGuard() {
      SourceFile = SavedSourceFile;
      SetLastChar(SavedLastChar);
      CurTok = SavedCurTok;
      CurLoc = SavedCurLoc;
      CurLine = SavedCurLine;
      CurCol = SavedCurCol;
      SourceFilename = SavedFilename;
      NumVal = SavedNumVal;
      IntVal = SavedIntVal;
      BoolVal = SavedBoolVal;
      IdentifierStr = SavedIdentifierStr;
      StringVal = SavedStringVal;
    }
  } LGuard;

  // Set new lexer state
  SourceFile = &ImportFile;
  SourceFilename = Path;
  CurLine = 1;
  CurCol = 0;
  CurLoc = {1, 1};
  SetLastChar(' ');
  getNextToken();

  std::vector<std::unique_ptr<ExprAST>> Exprs;

  // Parse the file
  while (true) {
    if (CurTok == TOK_EOF)
      break;
    if (CurTok == ';') {
      getNextToken();
      continue;
    }

    if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_AT ||
        CurTok == TOK_EXTERN) {
      auto Expr = ParseExpression();
      if (Expr) {
        if (CurTok == ';') {
          LogErrorAt(CurLoc, "Unexpected ';'");
          return nullptr;
        }
        Exprs.push_back(std::move(Expr));
      } else {
        getNextToken(); // skip for error recovery
      }
      continue;
    }

    if (CurTok == TOK_IMPORT) {
      auto Expr = ParseExpression();
      if (Expr) {
        if (CurTok != ';') {
          LogErrorAt(CurLoc, "Expected ';' after statement.");
          return nullptr;
        }
        getNextToken(); // eat ';'
        Exprs.push_back(std::move(Expr));
      } else {
        getNextToken(); // skip for error recovery
      }
      continue;
    }

    // Everything else: must end with ';'
    auto Expr = ParseExpression();
    if (Expr) {
      if (CurTok != ';') {
        LogErrorAt(CurLoc, "Expected ';' after statement.");
        return nullptr;
      }
      getNextToken(); // eat ';'
      Exprs.push_back(std::move(Expr));
    } else {
      getNextToken(); // skip for error recovery
    }
  }

  return std::make_unique<BlockExprAST>(std::move(Exprs));
}

std::unique_ptr<ExprAST> ParseImport() {
  SourceLocation Loc = CurLoc;
  getNextToken(); // eat import
  std::string ModuleName;
  if (CurTok == TOK_IDENTIFIER) {
    ModuleName = IdentifierStr;
  } else {
    LogErrorAt(CurLoc, "Expected module name after 'import'");
    return nullptr;
  }
  getNextToken();

  llvm::SmallString<128> StdlibPath(CompilerExecutablePath);
  llvm::sys::path::remove_filename(StdlibPath);
  llvm::sys::path::append(StdlibPath, "..", "lib", "std", ModuleName + ".tr");

  // Search order:
  //   1. Compiler stdlib (<binary>/../lib/std/<module>.tr)
  //   2. Same directory as the source file being compiled
  //   3. Current working directory
  auto Block = ParseFile(StdlibPath.str().str());
  if (!Block) {
    // Issue #19 fix: resolve relative to the source file's directory so that
    // `turf path/to/file.tr` can find sibling modules without a cd first.
    if (!SourceFilename.empty()) {
      llvm::SmallString<256> SrcRelPath(SourceFilename);
      llvm::sys::path::remove_filename(SrcRelPath);
      llvm::sys::path::append(SrcRelPath, ModuleName + ".tr");
      Block = ParseFile(SrcRelPath.str().str());
    }
  }
  if (!Block) {
    std::string CwdPath = ModuleName + ".tr";
    Block = ParseFile(CwdPath);
    if (!Block) {
      ModuleNotFoundError(Loc, ModuleName).raise();
      return nullptr;
    }
  }

  return std::make_unique<ModuleImportExprAST>(ModuleName, std::move(Block));
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
// list<T> declarations go through ParseVarDeclBody with a GenericType from
// ParseType(). This function is kept as a named reference point but is no
// longer called.
