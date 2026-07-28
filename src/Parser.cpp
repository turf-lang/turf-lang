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

// Tracks the chain of modules currently being parsed (not yet finished) so a
// circular `import` (A -> B -> ... -> A) can be reported as a real
// diagnostic instead of silently truncating. This is deliberately a SEPARATE
// structure from ImportedFiles above: ImportedFiles is a "fully parsed,
// cacheable" set (a module already finished parsing returns an empty body on
// a second import, by design -- see ParseFile()), while ImportStack is an
// "in progress right now" stack that must be empty of a path the moment that
// path's own ParseFile() call returns, cycle or not.
struct ImportStackFrame {
  std::string ModuleName;
  std::string ResolvedPath;
  SourceLocation ImportLoc; // location of the `import` statement that pulled this in
};
static std::vector<ImportStackFrame> ImportStack;

// Current state of the Parser
int CurTok; // The current token the parser is looking at
static std::map<int, int> BinopPrecedence; // Precedence table: '*' > '+'
static std::vector<std::unique_ptr<ExprAST>> ParsedTopLevelASTs;

struct SymbolScopeGuard {
  bool Active;
  SymbolScopeGuard(bool ShouldEnter) : Active(ShouldEnter) {
    if (Active && GlobalSymbolTable) GlobalSymbolTable->EnterScope();
  }
  ~SymbolScopeGuard() {
    if (Active && GlobalSymbolTable) GlobalSymbolTable->ExitScope();
  }
};

// Reads the next token from the Lexer and updates CurTok.
//
// Also maintains g_braceDepth (see Lexer.h/.cpp): consuming '{'/'('/'['
// (i.e. CurTok is one of those RIGHT BEFORE we advance past it) increments
// it, consuming '}'/')'/']' decrements it. This is the single choke point
// every token advance in the entire parser goes through, which is exactly
// why it's the right place for this: it gives SkipToNextTopLevelBoundary()
// an always-accurate nesting depth with zero per-callsite bookkeeping,
// regardless of how deeply a parse failure is nested.
int getNextToken() {
  if (CurTok == '{' || CurTok == '(' || CurTok == '[')
    g_braceDepth++;
  else if ((CurTok == '}' || CurTok == ')' || CurTok == ']') && g_braceDepth > 0)
    g_braceDepth--;
  return CurTok = gettok();
}

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

  // Bitwise OR
  BinopPrecedence['|'] = 33;

  // Bitwise XOR
  BinopPrecedence['^'] = 35;

  // Bitwise AND
  BinopPrecedence['&'] = 37;

  // Bitwise shifts
  BinopPrecedence[TOK_LSHIFT] = 38;
  BinopPrecedence[TOK_RSHIFT] = 38;

  // Additive
  BinopPrecedence['+'] = 40;
  BinopPrecedence['-'] = 40;

  // Multiplicative
  BinopPrecedence['*'] = 50;
  BinopPrecedence['/'] = 50;
  BinopPrecedence['%'] = 50;

  // Exponentiation (formerly ^, now **)
  BinopPrecedence[TOK_POWER] = 60;
}

void ClearParsedTopLevelASTs() {
  ParsedTopLevelASTs.clear();
  ImportedFiles.clear();
  // ImportStack should always be empty already (the RAII guard in ParseFile()
  // pops every frame it pushes, cycle or not) -- cleared defensively anyway,
  // mirroring ImportedFiles' own reset-between-compiles behavior above.
  ImportStack.clear();
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
std::unique_ptr<ExprAST> ParseEnumDecl();
std::unique_ptr<ExprAST> ParseBoolExpr();
std::unique_ptr<ExprAST> ParseStringExpr();
std::unique_ptr<ExprAST> ParseFuncDef();
std::unique_ptr<ExprAST> ParseImport();
std::unique_ptr<ExprAST> ParseReturnExpr();
std::unique_ptr<ExprAST> ParseStructDecl();
std::unique_ptr<ExprAST> ParseExternDecl();
std::unique_ptr<ExprAST> ParseExportDecl();
std::unique_ptr<ExprAST> ParseCompareExpr();
// list<T> declarations have no dedicated parse function: ParseType() already
// produces a GenericType("list", [elemType]) for any `Name<...>` spelling,
// so `list<int> xs = ...;` flows through the exact same declaration path as
// `int x = ...;` -- ParseLetStatement(), the only variable-declaration
// entry point this parser actually calls.

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

  // Module-qualified type: Module::TypeName<Args> (e.g. list::List<int>).
  // BaseName parsed above was actually the module qualifier; re-bind it to
  // the real type name that follows '::', and carry the qualifier forward
  // into the NamedType/GenericType built below so the Resolver can
  // actually validate it (Resolver::validateModuleQualifier(), called from
  // validateNamedType()) once module registrations are known -- that can't
  // happen here at parse time, since module registration runs at Codegen
  // time, strictly after the entire program is parsed.
  std::string ModuleQualifier;
  if (CurTok == TOK_SCOPE) {
    getNextToken(); // eat '::'
    if (CurTok != TOK_IDENTIFIER) {
      SyntaxError(CurLoc, "Expected a type name after '::'").raise();
      return nullptr;
    }
    ModuleQualifier = BaseName;
    BaseName = IdentifierStr;
    getNextToken();
  }

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
        std::make_shared<GenericType>(BaseName, std::move(TypeArgs), ModuleQualifier);
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
    Base = std::make_shared<NamedType>(BaseName, ModuleQualifier);
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

/**
 * @brief True if @p T is bare 'void', or an array whose (possibly nested)
 *        element type is bare 'void'.
 *
 * 'void' only has a valid meaning as a function's return type -- there is no
 * LLVM representation for a void-typed local variable, struct field, array
 * element, or parameter, and code further down the pipeline (definite-
 * assignment analysis, GetDefaultValue(), LLVM struct-body construction, ...)
 * assumes it never sees one there. Concretely, `mut x: void;` and
 * `fn f(x: void): void {}` were both observed to hang the compiler
 * indefinitely rather than fail cleanly -- this predicate exists so callers
 * can catch the mistake at parse time instead, with a normal diagnostic.
 *
 * Deliberately does NOT match 'void*' (a TurfPointerType wrapping VoidType):
 * TurfPointerType::isVoid() is never overridden and always returns false, so
 * the FFI opaque-pointer idiom used throughout lib/std/mem.tr keeps working.
 * Also does not match a function's return type -- callers must only invoke
 * this on types parsed for a variable/field/parameter position, never on the
 * return-type ParseType() call.
 */
static bool isDisallowedVoidType(const TurfType *T) {
  if (!T)
    return false;
  if (T->isVoid())
    return true;
  if (isArrayType(T))
    return isDisallowedVoidType(getArrayElementType(T));
  return false;
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

  std::unique_ptr<ExprAST> Cond = ParseExpression();
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

    std::unique_ptr<ExprAST> ElseifCond = ParseExpression();
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

    // ── Legacy struct brace-init is gone: `Identifier '{' ... '}'` no longer
    // constructs anything -- see `StructName::new(...)`/`StructName::default()`
    // instead. This branch used to build a StructInitExprAST here (removed
    // entirely, along with the g_AllowStructInit flag it needed to avoid
    // colliding with every block-taking construct's own '{'). Best-effort,
    // not authoritative: if `IdName` is a struct type already registered by
    // this point in the (top-to-bottom, live) parse, give a specific hint
    // instead of letting this fall through to a generic/confusing parse
    // error a few tokens later -- a forward-declared struct just won't hit
    // this check, which is fine, since the ordinary parse failure that
    // follows is still a real, if less specific, diagnostic.
    if (CurTok == '{' && GlobalSymbolTable) {
      if (auto T = GlobalSymbolTable->LookupType(IdName)) {
        if (dynamic_cast<TurfStructType *>(T.get())) {
          LogErrorAt(IdLoc, "'" + IdName + " { ... }' is no longer valid syntax "
                             "for constructing a struct."
                             "\n  Hint: use '" + IdName + "::new(...)' (if '" +
                             IdName + "' declares a constructor) or '" + IdName +
                             "::default()' instead.");
          return nullptr;
        }
      }
    }

    // ── Scope access: Left::Right ────────────────────────────────────────────
    // Purely syntax-directed, exactly like before this was generalized: the
    // parser doesn't know (and doesn't need to know) whether `IdName` names
    // an enum, a module, or something else -- ScopeAccessKind disambiguation
    // happens entirely in the Resolver (visitScopeAccess / visitFuncCall).
    // Whatever follows this node syntactically (a postfix '(' for a call, a
    // bare reference, etc.) is handled by the existing, unmodified postfix-
    // expression machinery -- `Module::func(args)` already round-trips
    // through it with zero new parsing logic needed here.
    if (CurTok == TOK_SCOPE) {
      getNextToken(); // eat '::'
      if (CurTok != TOK_IDENTIFIER) {
        LogErrorAt(CurLoc, "Expected a name after '::'");
        return nullptr;
      }
      std::string RightName = IdentifierStr;
      SourceLocation RightLoc = CurLoc;
      getNextToken(); // eat right-hand name

      // Same removed-syntax hint as the bare form above, for the qualified
      // `Module::StructName { ... }` spelling -- RightName is checked
      // (structs/enums stay flat and module-blind, see 2.6 in
      // modules_and_scope_resolution.md, so a bare LookupType is correct
      // here too even though IdName is the module, not the struct).
      if (CurTok == '{' && GlobalSymbolTable) {
        if (auto T = GlobalSymbolTable->LookupType(RightName)) {
          if (dynamic_cast<TurfStructType *>(T.get())) {
            LogErrorAt(IdLoc, "'" + IdName + "::" + RightName + " { ... }' is no "
                               "longer valid syntax for constructing a struct."
                               "\n  Hint: use '" + IdName + "::" + RightName +
                               "::new(...)' or '" + IdName + "::" + RightName +
                               "::default()' instead.");
            return nullptr;
          }
        }
      }

      // A THIRD name chained on: `IdName::RightName` just parsed was
      // actually `Module::EnumName`, not the enum-and-variant pair itself
      // -- reinterpret it as such and parse the real variant name that
      // follows this second '::'. `Module::func::whatever` (chaining past
      // a module-function-ref) or any other shape is nonsensical and left
      // to the Resolver to reject once it knows IdName isn't an enum's
      // module-qualifying prefix at all.
      if (CurTok == TOK_SCOPE) {
        getNextToken(); // eat second '::'
        if (CurTok != TOK_IDENTIFIER) {
          LogErrorAt(CurLoc, "Expected a variant name after '::'");
          return nullptr;
        }
        std::string VariantName = IdentifierStr;
        SourceLocation VariantLoc = CurLoc;
        getNextToken();
        return std::make_unique<ScopeAccessExprAST>(IdLoc, RightName, RightLoc,
                                                     VariantName, VariantLoc,
                                                     /*ModuleQualifier=*/IdName);
      }

      return std::make_unique<ScopeAccessExprAST>(IdLoc, IdName, IdLoc,
                                                   RightName, RightLoc);
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
      // Heuristic: If this is a keyword typo (like 'iff x'), the next token is
      // usually an identifier, number, string, or brace — things that cannot
      // directly follow a valid variable expression without an operator.
      if (CurTok == TOK_IDENTIFIER || CurTok == TOK_NUMBER || 
          CurTok == TOK_INT_LITERAL || CurTok == TOK_STRING_LITERAL || 
          CurTok == '{') {
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

  case TOK_COMPARE:
    return ParseCompareExpr();

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
    return ParseFuncDef();

  case TOK_AT: {
    // The @intrinsic("name") decorator (used to bind print/printline/input
    // to compiler-native codegen) has been removed -- those functions are
    // now ordinary Turf code delegating to `extern fn` FFI bindings, see
    // lib/std/io.tr. No decorator syntax is currently supported at all.
    LogErrorAt(CurLoc,
               "'@' is not valid here.\n  Hint: Turf has no decorator syntax "
               "right now.");
    return nullptr;
  }

  case TOK_IMPORT:
    return ParseImport();

  case TOK_STRUCT:
    return ParseStructDecl();

  case TOK_ENUM:
    return ParseEnumDecl();

  case TOK_EXTERN:
    return ParseExternDecl();

  case TOK_EXPORT:
    return ParseExportDecl();


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
  if (CurTok == '!') {
    SourceLocation Loc = CurLoc;
    getNextToken(); // consume '!'
    auto Operand = ParseUnary();
    if (!Operand)
      return nullptr;
    // Desugar: !x -> x != true
    return std::make_unique<BinaryExprAST>(
        Loc, TOK_NEQ,
        std::move(Operand),
        std::make_unique<BoolExprAST>(true)
    );
  }

  // Bitwise NOT: ~expr
  if (CurTok == '~') {
    SourceLocation Loc = CurLoc;
    getNextToken(); // consume '~'
    auto Operand = ParseUnary();
    if (!Operand)
      return nullptr;
    return std::make_unique<UnaryExprAST>(Loc, '~', std::move(Operand));
  }
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
  SourceLocation Loc = CurLoc;
  getNextToken();

  if (auto Operand = ParseUnary())
    return std::make_unique<UnaryExprAST>(Loc, Opc, std::move(Operand));

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
    if (CurTok == TOK_EXPORT) {
      // `export` grammatically parses in any statement position (the
      // top-level-only rule is enforced semantically by the Resolver, via
      // FunctionNestingDepth_ -- see ExportOutsideTopLevelError), so a block
      // must still be able to parse one syntactically, even though it will
      // always be rejected downstream here. Needs its own branch (not the
      // generic ParseExpression() fallback below) for the same reason
      // TOK_MUTABLE/TOK_IMMUTABLE do: ParseExportDecl() -> ParseLetStatement()
      // already consumes the statement's trailing ';' itself, so the
      // fallback's OWN `if (CurTok != ';') error` check would immediately
      // misfire against the token that follows (already past the ';').
      auto Expr = ParseExportDecl();
      if (!Expr) return nullptr;
      Exprs.push_back(std::move(Expr));
      continue;
    }
    if (CurTok == TOK_IF || CurTok == TOK_WHILE || CurTok == TOK_FOR ||
        CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM || CurTok == TOK_COMPARE) {
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
            "return", "compare", "do", "fn", "struct", "enum"};
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

  std::unique_ptr<ExprAST> Cond = ParseExpression();
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

  std::unique_ptr<ExprAST> Start, End, Step;
  Start = ParseExpression();
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

  End = ParseExpression();
  if (!End)
    return nullptr;
  if (CurTok != TOK_STEP) {
    LogErrorAt(ForLoc, "Expected 'step' after the range end value."
                       "\n  Hint: Correct syntax: for " +
                           VarName + " in 0..10 step " + VarName + "++ { }");
    return nullptr;
  }
  getNextToken();

  Step = ParseExpression();
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

// ParseCompareExpr — parses:
//   compare [( ] subject [ )] {
//       matches [( ] val [ )] :
//       matches [( ] val [ )] :     ← stacking (all share one body + done)
//           body statements
//       done
//
//       otherwise:
//           body statements
//   }
//
// Rules enforced here (parse-time errors):
//   • Missing 'done' after arm body → error
//   • Body statements between stacked matches headers → error
//   • 'otherwise' before any remaining matches arm → error
//   • Duplicate 'otherwise' → error
//   • Empty compare{} → CompareWarning (not an error)
std::unique_ptr<ExprAST> ParseCompareExpr() {
  SourceLocation CompareLoc = CurLoc;
  getNextToken(); // eat 'compare'

  // ── Parse subject ────────────────────────────────────────────────────────
  // Optional parentheses around the subject (like if/while).
  bool HasParen = (CurTok == '(');
  if (HasParen)
    getNextToken(); // eat '('

  std::unique_ptr<ExprAST> Subject = ParseExpression();
  if (!Subject)
    return nullptr;

  if (HasParen) {
    if (CurTok != ')') {
      LogErrorAt(CurLoc, "Expected ')' to close the compare subject");
      return nullptr;
    }
    getNextToken(); // eat ')'
  }

  // ── Opening brace ────────────────────────────────────────────────────────
  if (CurTok != '{') {
    LogErrorAt(CompareLoc, "Expected '{' to open the compare block.");
    return nullptr;
  }
  getNextToken(); // eat '{'

  // ── Arm parsing loop ──────────────────────────────────────────────────────
  std::vector<MatchesArm> Arms;
  std::unique_ptr<ExprAST> OtherBody = nullptr;
  SourceLocation OtherLoc = {0, 0};
  bool HasOther = false;

  while (CurTok != '}' && CurTok != TOK_EOF) {
    // Skip stray semicolons between arms
    if (CurTok == ';') {
      getNextToken();
      continue;
    }

    // ── 'otherwise' arm (optional, must be last) ─────────────────────────
    if (CurTok == TOK_OTHERWISE) {
      if (HasOther) {
        CompareError(CurLoc, "Duplicate 'otherwise' arm in compare.").raise();
        return nullptr;
      }
      HasOther = true;
      OtherLoc = CurLoc;
      getNextToken(); // eat 'otherwise'

      if (CurTok != TOK_COLON) {
        LogErrorAt(CurLoc, "Expected ':' after 'otherwise'.");
        return nullptr;
      }
      getNextToken(); // eat ':'

      // Parse the body of the otherwise arm as a sequence of statements
      // terminated by '}' (the closing brace of the compare block). Also
      // terminate on TOK_OTHERWISE/TOK_MATCHES: those tokens can only validly
      // appear here as the *next arm's* header (a duplicate 'otherwise', or a
      // 'matches' arm wrongly placed after 'otherwise'), never as the start of
      // a real statement. Without this check they used to fall through to
      // ParseExpression() -> ParsePrimaryBase()'s generic "Unexpected token"
      // fallback, masking the specific CompareError diagnostics below (which
      // were unreachable dead code for this exact input shape) behind a vague
      // message pointing at the wrong problem.
      std::vector<std::unique_ptr<ExprAST>> OtherStmts;
      while (CurTok != '}' && CurTok != TOK_EOF &&
             CurTok != TOK_OTHERWISE && CurTok != TOK_MATCHES) {
        if (CurTok == ';') { getNextToken(); continue; }
        if (CurTok == TOK_MUTABLE || CurTok == TOK_IMMUTABLE) {
          auto E = ParseLetStatement(CurTok == TOK_IMMUTABLE);
          if (!E) return nullptr;
          OtherStmts.push_back(std::move(E));
          continue;
        }
        if (CurTok == TOK_EXPORT) {
          // See ParseBlock()'s matching TOK_EXPORT branch for why this
          // needs its own case instead of falling through to the generic
          // ParseExpression() path below.
          auto E = ParseExportDecl();
          if (!E) return nullptr;
          OtherStmts.push_back(std::move(E));
          continue;
        }
        if (CurTok == TOK_IF || CurTok == TOK_WHILE || CurTok == TOK_FOR ||
            CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM || CurTok == TOK_COMPARE) {
          auto E = ParseExpression();
          if (!E) return nullptr;
          OtherStmts.push_back(std::move(E));
          continue;
        }
        auto E = ParseExpression();
        if (!E) return nullptr;
        if (CurTok != ';') {
          LogErrorAt(CurLoc, "Expected ';' after statement in 'otherwise' arm.");
          return nullptr;
        }
        getNextToken(); // eat ';'
        OtherStmts.push_back(std::move(E));
      }
      OtherBody = std::make_unique<BlockExprAST>(std::move(OtherStmts));

      // After 'otherwise', only '}' is valid (it must be the last arm).
      // Distinguish a duplicate 'otherwise' from a 'matches'/other arm placed
      // after it -- both stop the body loop above on the same condition, but
      // deserve their own specific message rather than collapsing to one.
      if (CurTok == TOK_OTHERWISE) {
        CompareError(CurLoc, "Duplicate 'otherwise' arm in compare.").raise();
        return nullptr;
      }
      if (CurTok != '}') {
        // There's something after otherwise — likely another matches arm
        CompareError(CurLoc,
                     "'otherwise' must be the last arm in compare.")
            .raise();
        return nullptr;
      }
      break; // 'otherwise' consumed; exit arm loop
    }

    // ── 'matches' arm ────────────────────────────────────────────────────
    if (CurTok != TOK_MATCHES) {
      // Unexpected token inside compare block — could be a keyword typo
      LogErrorAt(CurLoc,
                 "Expected 'matches' or 'otherwise' inside compare block.");
      return nullptr;
    }

    if (HasOther) {
      CompareError(CurLoc,
                   "'otherwise' must be the last arm in compare.")
          .raise();
      return nullptr;
    }

    SourceLocation ArmLoc = CurLoc;
    std::vector<std::unique_ptr<ExprAST>> Patterns;

    // Collect all stacked 'matches val:' headers
    while (CurTok == TOK_MATCHES) {
      getNextToken(); // eat 'matches'

      // Optional parens around the pattern value
      bool PatternParen = (CurTok == '(');
      if (PatternParen)
        getNextToken(); // eat '('

      auto Val = ParseExpression();
      if (!Val) return nullptr;

      if (PatternParen) {
        if (CurTok != ')') {
          LogErrorAt(CurLoc, "Expected ')' to close matches pattern.");
          return nullptr;
        }
        getNextToken(); // eat ')'
      }

      if (CurTok != TOK_COLON) {
        LogErrorAt(CurLoc, "Expected ':' after matches pattern.");
        return nullptr;
      }
      getNextToken(); // eat ':'

      Patterns.push_back(std::move(Val));

      // If the very next non-semicolon token is not another 'matches' or 'done'
      // or 'otherwise' or '}', the user has put a body statement between
      // stacked headers — that is an error.
      while (CurTok == ';') getNextToken(); // skip stray semicolons

      if (CurTok != TOK_MATCHES && CurTok != TOK_DONE &&
          CurTok != TOK_OTHERWISE && CurTok != '}') {
        // Body appears before all matches headers are consumed — error only
        // when there are additional matches after the current one i.e. the
        // outer while would re-enter. We detect this by checking the next token.
        // Actually: if CurTok != TOK_MATCHES here, we're about to parse the
        // body, so this is fine (we just exit the inner while). No error.
        break; // exit inner while — next is body
      }

      if (CurTok == TOK_DONE || CurTok == TOK_OTHERWISE || CurTok == '}') {
        // Header has no body — error: missing body / done
        CompareError(ArmLoc,
                     "Expected 'done' after matches arm body — "
                     "arm has no body.")
            .raise();
        return nullptr;
      }
      // CurTok == TOK_MATCHES → continue stacking
    }

    // ── Parse arm body statements until 'done' ────────────────────────────
    std::vector<std::unique_ptr<ExprAST>> ArmStmts;
    bool GotDone = false;
    while (CurTok != TOK_DONE && CurTok != '}' && CurTok != TOK_EOF) {
      if (CurTok == ';') { getNextToken(); continue; }
      // Detect premature start of next arm (missing done)
      if (CurTok == TOK_MATCHES || CurTok == TOK_OTHERWISE) {
        CompareError(CurLoc,
                     "Expected 'done' after matches arm body.")
            .raise();
        return nullptr;
      }
      if (CurTok == TOK_MUTABLE || CurTok == TOK_IMMUTABLE) {
        auto E = ParseLetStatement(CurTok == TOK_IMMUTABLE);
        if (!E) return nullptr;
        ArmStmts.push_back(std::move(E));
        continue;
      }
      if (CurTok == TOK_EXPORT) {
        // See ParseBlock()'s matching TOK_EXPORT branch for why this needs
        // its own case instead of falling through to the generic
        // ParseExpression() path below.
        auto E = ParseExportDecl();
        if (!E) return nullptr;
        ArmStmts.push_back(std::move(E));
        continue;
      }
      if (CurTok == TOK_IF || CurTok == TOK_WHILE || CurTok == TOK_FOR ||
          CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM || CurTok == TOK_COMPARE) {
        auto E = ParseExpression();
        if (!E) return nullptr;
        ArmStmts.push_back(std::move(E));
        continue;
      }
      auto E = ParseExpression();
      if (!E) return nullptr;
      if (CurTok != ';') {
        LogErrorAt(CurLoc, "Expected ';' after statement in matches arm body.");
        return nullptr;
      }
      getNextToken(); // eat ';'
      ArmStmts.push_back(std::move(E));
    }

    if (CurTok == TOK_DONE) {
      GotDone = true;
      getNextToken(); // eat 'done'
    } else {
      // Reached '}' or EOF without 'done'
      CompareError(ArmLoc, "Expected 'done' after matches arm body.").raise();
      return nullptr;
    }
    (void)GotDone;

    MatchesArm Arm;
    Arm.Loc = ArmLoc;
    Arm.Values = std::move(Patterns);
    Arm.Body = std::make_unique<BlockExprAST>(std::move(ArmStmts));
    Arms.push_back(std::move(Arm));
  }

  // ── Closing brace ────────────────────────────────────────────────────────
  if (CurTok != '}') {
    LogErrorAt(CompareLoc,
               "Missing closing '}' for compare block.");
    return nullptr;
  }
  getNextToken(); // eat '}'

  // Warn on empty compare{}
  if (Arms.empty() && !OtherBody) {
    CompareWarning(CompareLoc).emit();
  }

  return std::make_unique<CompareExprAST>(
      CompareLoc, std::move(Subject), std::move(Arms), std::move(OtherBody),
      OtherLoc);
}

static std::unique_ptr<ExprAST> SynthesizeDefault(SourceLocation Loc, TurfTypePtr Type) {
  if (Type->isInt()) {
    return std::make_unique<NumberExprAST>(0LL);
  } else if (Type->isDouble()) {
    return std::make_unique<NumberExprAST>(0.0);
  } else if (Type->isBool()) {
    return std::make_unique<BoolExprAST>(false);
  } else if (Type->isString()) {
    return std::make_unique<StringExprAST>("");
  } else if (Type->isPointer() || Type->isNamed("ptr") || Type->isNamed("void*")) {
    return std::make_unique<NullExprAST>(Loc);
  } else {
    return std::make_unique<DefaultInitExprAST>(Loc, Type);
  }
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

  SourceLocation TypeLoc = CurLoc; // points at the type token itself, for a precise caret below
  TurfTypePtr Type = ParseType();
  if (!Type) return nullptr;

  // 'void' has no meaning as a variable's type (see isDisallowedVoidType's
  // doc comment for why this must be caught here rather than left to a
  // later pass -- it isn't a style nit, it's a compiler hang).
  if (isDisallowedVoidType(Type.get())) {
    LogErrorAt(TypeLoc,
               "'void' cannot be a variable's type."
               "\n  Hint: 'void' only makes sense as a function's return "
               "type, e.g. fn f(): void { ... }. Give '" +
                   Name + "' a real type instead, like int, string, or a "
                   "struct name.");
    return nullptr;
  }

  if (CurTok != TOK_ASSIGN) {
    if (isArrayType(Type.get())) {
      const auto *AT = dynamic_cast<const ::ArrayType *>(Type.get());
      int ArraySize = AT ? AT->getSize() : -1;
      TurfTypePtr ElemType = AT ? AT->getElementTypePtr() : Type;

      if (CurTok != ';') {
        LogErrorAt(CurLoc, "Expected ';' after array declaration.");
        return nullptr;
      }
      getNextToken(); // eat ';'
      return std::make_unique<ArrayDeclExprAST>(NameLoc, Name, ElemType, ArraySize, std::vector<std::unique_ptr<ExprAST>>(), nullptr, IsImmutable);
    }

    std::unique_ptr<ExprAST> Init = nullptr;
    if (!Type->isInt() && !Type->isDouble() && !Type->isBool() && !Type->isString() &&
        !Type->isPointer() && !Type->isNamed("ptr") && !Type->isNamed("void*") && !Type->isNull()) {
      Init = SynthesizeDefault(Loc, Type);
    }
    if (CurTok != ';') {
      LogErrorAt(CurLoc, "Expected ';' after variable declaration.");
      return nullptr;
    }
    getNextToken(); // eat ';'
    return std::make_unique<VarDeclExprAST>(Loc, Name, Type, std::move(Init), IsImmutable, false);
  }
  getNextToken(); // eat '='

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
  return std::make_unique<VarDeclExprAST>(Loc, Name, Type, std::move(Init), IsImmutable, true);
}

// NOTE: ParseVarDecl()/ParseVarDeclBody() (a "bare `int x = 5;` with no
// mutable/immutable keyword" declaration path) used to live here and have
// since been removed: they had zero callers anywhere in the codebase --
// ParsePrimaryBase()'s TOK_TYPE_INT/TOK_TYPE_DOUBLE/TOK_TYPE_STRING/
// TOK_TYPE_BOOL cases (see above) already redirect that exact syntax to a
// "Expected 'mutable' or 'immutable' to begin a declaration." error instead
// of ever reaching these functions. ParseLetStatement() is the only variable-
// declaration entry point actually reachable from the grammar.

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

  // Push scope to allow ParseType to resolve generic parameters WHILE
  // parsing the struct's own field list -- but only for that; the struct's
  // own final registration below must happen in the OUTER (persistent)
  // scope, once Guard has already popped this temporary one. See the
  // comment at the registration call below for why.
  std::vector<StructField> Fields;
  {
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

    while (CurTok != '}' && CurTok != TOK_EOF) {
    // New syntax: name: type
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected field name in struct definition");
      return nullptr;
    }
    std::string FieldName = IdentifierStr;
    SourceLocation FieldNameLoc = CurLoc;
    getNextToken(); // eat field name

    // Reject a field name reused from earlier in this same struct. Without
    // this, TurfStructType::getFieldIndex()'s linear search silently binds
    // every access to the FIRST match, so the later duplicate's type is
    // permanently unreachable -- a typo like accidentally repeating a field
    // name previously compiled with zero diagnostics.
    for (const auto &Existing : Fields) {
      if (Existing.Name == FieldName) {
        LogErrorAt(FieldNameLoc,
                   "Struct '" + Name + "' already has a field named '" +
                       FieldName + "'."
                       "\n  Hint: Field names must be unique within a "
                       "struct -- rename one of them.");
        return nullptr;
      }
    }

    if (CurTok != TOK_COLON) {
      LogErrorAt(CurLoc,
                 "Expected ':' after field name '" + FieldName +
                     "' in struct definition (syntax: name: type)");
      return nullptr;
    }
    getNextToken(); // eat ':'

    SourceLocation FieldTypeLoc = CurLoc;
    TurfTypePtr FieldType = ParseType();
    if (!FieldType)
      return nullptr;

    // See isDisallowedVoidType()'s doc comment: 'void' as a field's type is
    // not caught by anything downstream and silently produces a struct LLVM
    // can't actually lay out correctly.
    if (isDisallowedVoidType(FieldType.get())) {
      LogErrorAt(FieldTypeLoc,
                 "'void' cannot be a struct field's type."
                 "\n  Hint: 'void' only makes sense as a function's return "
                 "type. Give field '" +
                     FieldName + "' a real type instead.");
      return nullptr;
    }

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
  } // end of Guard's scope -- see the comment above its declaration.

  // Register the struct type in the OUTER (persistent) scope, now that
  // Guard has already popped the temporary generic-type-parameter scope.
  // For generics, fields use TurfGenericParamType which compiles to void*.
  //
  // This registration used to happen INSIDE Guard's scope (before it was
  // moved out into its own block above) -- meaning it got POPPED along with
  // the temporary generic-params scope the instant this function returned,
  // so ANY generic struct's own name became permanently unlookupable via
  // GlobalSymbolTable->LookupType() immediately after being parsed.
  // Confirmed via `struct Box<T> { val: T* }` followed later by
  // `Box { val: null }` in the same file: Codegen's
  // StructInitExprAST::codegen() failed with "Unknown struct type 'Box'"
  // even though the struct had just been declared. Non-generic structs
  // (empty TypeParams, Guard's SymbolScopeGuard is a no-op) were never
  // affected by this.
  auto SType = std::make_shared<TurfStructType>(Name, Fields);
  if (GlobalSymbolTable) {
    GlobalSymbolTable->RegisterType(Name, SType);
  }

  return std::make_unique<StructDeclExprAST>(Loc, Name, Fields, TypeParams);
}

// ParseEnumDecl - called when CurTok == TOK_ENUM.
std::unique_ptr<ExprAST> ParseEnumDecl() {
  SourceLocation Loc = CurLoc;
  getNextToken(); // eat 'enum'
  
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected identifier after 'enum'");
    return nullptr;
  }
  std::string Name = IdentifierStr;
  getNextToken();

  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' after enum name");
    return nullptr;
  }
  getNextToken(); // eat '{'

  std::vector<std::string> Variants;
  while (CurTok != '}' && CurTok != TOK_EOF) {
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected variant name in enum definition");
      return nullptr;
    }

    // Reject a variant name reused from earlier in this same enum. Mirrors
    // the identical check ParseStructDecl already does for field names
    // (see its own doc comment just above the equivalent loop there).
    // Without this, a duplicate variant silently compiled: the underlying
    // integer values are still assigned positionally (first "Red" = 0,
    // second "Red" = 2 in `enum Color { Red, Green, Red }`), but every
    // NAMED reference to "Red" (`Color::Red`, a `matches Color::Red:` arm,
    // ...) always resolves to the LAST match, permanently orphaning the
    // first one -- it becomes a real, distinct enum value with no name that
    // can ever construct or match it again. Confirmed via
    // `enum Color { Red, Green, Red }`: `Color::Red to int` returns 2, not
    // 0, with zero diagnostic anywhere.
    std::string VariantName = IdentifierStr;
    SourceLocation VariantLoc = CurLoc;
    for (const auto &Existing : Variants) {
      if (Existing == VariantName) {
        LogErrorAt(VariantLoc,
                   "Enum '" + Name + "' already has a variant named '" +
                       VariantName + "'."
                       "\n  Hint: Variant names must be unique within an "
                       "enum -- rename one of them.");
        return nullptr;
      }
    }

    Variants.push_back(IdentifierStr);
    getNextToken(); // eat variant name

    if (CurTok == ',') {
      getNextToken();
    } else if (CurTok != '}') {
      LogErrorAt(CurLoc, "Expected ',' or '}' after variant name");
      return nullptr;
    }
  }

  if (CurTok != '}') {
    LogErrorAt(CurLoc, "Expected '}' to close enum definition");
    return nullptr;
  }
  getNextToken(); // eat '}'

  // Register the enum type in GlobalSymbolTable so that codegen can look it up
  // via GlobalSymbolTable->LookupType(Name), mirroring how struct types are
  // registered during ParseStructDecl.
  auto EType = std::make_shared<TurfEnumType>(Name, Variants, TurfInt());
  if (GlobalSymbolTable) {
    GlobalSymbolTable->RegisterType(Name, EType);
  }

  return std::make_unique<EnumDeclExprAST>(Loc, Name, Variants);
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

    SourceLocation ParamTypeLoc = CurLoc;
    TurfTypePtr ParamType = ParseType();
    if (!ParamType)
      return nullptr;

    // See isDisallowedVoidType()'s doc comment above ParseType(). Note this
    // does NOT reject 'void*' (TurfPointerType wrapping VoidType) -- that's
    // the legitimate opaque-pointer idiom extern fns use constantly, e.g.
    // lib/std/mem.tr's `extern fn free(ptr: void*): void;`.
    if (isDisallowedVoidType(ParamType.get())) {
      LogErrorAt(ParamTypeLoc,
                 "'void' cannot be a parameter's type."
                 "\n  Hint: 'void' only makes sense as a function's return "
                 "type. Give parameter '" +
                     ParamName + "' a real type instead (use 'void*' if you "
                     "meant an opaque pointer).");
      return nullptr;
    }

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

// ParseExportDecl - called when CurTok == TOK_EXPORT.
// Parses: export immut <name>: <type> = <literal>;
//
// Grammar-wise this accepts `export` anywhere ParsePrimaryBase() is reached
// (the same shared dispatch point `fn`/`struct`/`import`/etc. all go
// through) -- it does NOT check whether it's actually at true top level.
// That's deliberate: enforcing "top level only" here would mean duplicating
// whatever notion of "am I inside a function" the parser has at every call
// site, when the Resolver already tracks that precisely (FunctionNestingDepth_)
// and can raise a far more specific error (ExportOutsideTopLevelError)
// pointing at the actual problem, matching how other semantic-only
// constraints in this language are enforced post-parse rather than baked
// into the grammar.
//
// `export mut` (a genuinely mutable, shared global) is deliberately
// rejected right here at parse time rather than deferred -- true mutable
// global state (initialization ordering, real LLVM global-variable
// codegen) is a meaningfully bigger feature than a named constant and is
// not implemented at all yet; see docs/planning/ROADMAP_v1.0.0.md.
std::unique_ptr<ExprAST> ParseExportDecl() {
  SourceLocation ExportLoc = CurLoc;
  getNextToken(); // eat 'export'

  // Both error paths below fire BEFORE any '{' could possibly be open (right
  // after the 'export' keyword), so the CALLER's SkipToNextTopLevelBoundary()
  // (main.cpp) -- which only makes ONE token of forward progress when
  // g_braceDepth is already 0 -- doesn't skip far enough past a whole broken
  // declaration like `export mut X: int = 5;` on its own. Left entirely to
  // that, recovery lands mid-statement (e.g. on the `X` identifier), which
  // the top-level dispatch then misreads as a bare expression statement,
  // producing a confusing SECOND "Statements are not allowed outside of a
  // function" error stacked on top of the real one.
  //
  // Skip up TO (but deliberately not PAST) the next statement boundary
  // ourselves -- stopping one token short of what the caller's own
  // always-advances-at-least-once SkipToNextTopLevelBoundary() will do right
  // after we return nullptr. Consuming the boundary token here too would
  // double-skip (the caller's forced extra step would then land one token
  // PAST the real boundary, e.g. eating the next `fn` and desyncing the
  // parse of the statement that follows).
  auto SkipRestOfBrokenStatement = [&]() {
    while (CurTok != ';' && CurTok != TOK_EOF && CurTok != TOK_FN &&
          CurTok != TOK_STRUCT && CurTok != TOK_ENUM && CurTok != TOK_AT &&
          CurTok != TOK_EXTERN && CurTok != TOK_EXPORT && CurTok != TOK_IMPORT)
      getNextToken();
  };

  if (CurTok == TOK_MUTABLE) {
    LogErrorAt(ExportLoc,
               "'export mut' is not supported yet -- only 'export immut' "
               "(a compile-time constant) works right now."
               "\n  Hint: true mutable shared global state needs its own "
               "future feature (initialization ordering, real global-"
               "variable codegen). If you don't need to mutate it, use "
               "'export immut' instead.");
    SkipRestOfBrokenStatement();
    return nullptr;
  }

  if (CurTok != TOK_IMMUTABLE) {
    LogErrorAt(ExportLoc,
               "Expected 'immut' or 'immutable' after 'export'."
               "\n  Hint: 'export' only works on variable declarations, "
               "e.g. export immut PI: double = 3.14159;");
    SkipRestOfBrokenStatement();
    return nullptr;
  }

  auto Decl = ParseLetStatement(/*IsImmutable=*/true);
  if (!Decl)
    return nullptr;

  auto *VarDecl = dynamic_cast<VarDeclExprAST *>(Decl.get());
  if (!VarDecl) {
    // ParseLetStatement() can also return an ArrayDeclExprAST for an array
    // type with no initializer (`export immut arr: int[5];`) -- exported
    // constants must have a real initializer value to export in the first
    // place, so that shape is rejected here rather than silently accepted
    // and then failing some other way downstream.
    LogErrorAt(ExportLoc,
               "'export immut' requires an explicit initializer value."
               "\n  Hint: e.g. export immut MAX_RETRIES: int = 3; -- an "
               "exported constant with no value has nothing to export.");
    return nullptr;
  }

  VarDecl->setExported(true);
  return Decl;
}

// ParseFuncDef - called when CurTok == TOK_FN.
// Parses: fn <rettype> <name>(<type> <param>, ...) { body }
std::unique_ptr<ExprAST> ParseFuncDef() {
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
  bool IsStaticMethodDecl = false;

  if (CurTok == '.') {
    StructName = FuncName;
    getNextToken(); // eat '.'
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected method name after '.'");
      return nullptr;
    }
    FuncName = StructName + "." + IdentifierStr;
    getNextToken();
  } else if (CurTok == TOK_SCOPE) {
    // `fn StructName::method(...)` used to declare a static method (no
    // implicit `self`). That form is gone -- ALL method declarations use
    // '.' now, static or not, distinguished by a trailing 'static' keyword
    // instead: `fn StructName.method(...) static: RetType { ... }`. '::' is
    // reserved for call/reference syntax only (StructName::method(...),
    // StructName::new(...), StructName::default()), never a declaration.
    LogErrorAt(CurLoc,
               "'fn " + FuncName + "::method(...)' is no longer valid syntax "
               "for declaring a static method."
               "\n  Hint: declare it as 'fn " + FuncName +
               ".method(...) static: ReturnType { ... }' instead -- '.' is "
               "used for every method declaration now (instance or static); "
               "'static' marks the no-self case. '::' is only for calling "
               "it (StructName::method(...) is unchanged).");
    return nullptr;
  }

  // Expect '('
  if (CurTok != '(') {
    LogErrorAt(FnLoc, "Expected '(' after function name '" + FuncName + "'.");
    return nullptr;
  }
  getNextToken(); // eat '('

  std::vector<ParamDecl> Params;

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
    SourceLocation ParamNameLoc = CurLoc;
    getNextToken(); // consume parameter name

    if (CurTok != TOK_COLON) {
      LogErrorAt(CurLoc,
                 "Expected ':' after parameter name '" + ParamName + "' in '" +
                     FuncName + "'.");
      return nullptr;
    }
    getNextToken(); // consume ':'

    SourceLocation ParamTypeLoc = CurLoc;
    TurfTypePtr ParamType = ParseType();
    if (!ParamType) {
      LogErrorAt(CurLoc,
                 "Expected a parameter type after ':' in the parameter list of '" +
                     FuncName + "'.");
      return nullptr;
    }

    // See isDisallowedVoidType()'s doc comment above ParseType(): a
    // void-typed parameter was observed to hang the compiler indefinitely
    // rather than fail cleanly.
    if (isDisallowedVoidType(ParamType.get())) {
      LogErrorAt(ParamTypeLoc,
                 "'void' cannot be a parameter's type."
                 "\n  Hint: 'void' only makes sense as a function's return "
                 "type. Give parameter '" +
                     ParamName + "' a real type instead.");
      return nullptr;
    }

    Params.push_back({ParamType, ParamName, ParamNameLoc});

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

  // Optional 'static' keyword: `fn StructName.method(...) static: RetType`.
  // Self-injection is decided HERE, after the parameter list is already
  // parsed (rather than before it, as this used to work when '::' picked
  // static-ness up front) -- 'static' is only known once we've read past
  // ')', so the self parameter (when applicable) is prepended to the
  // already-parsed Params vector instead of being pushed first.
  // Semantic legality (static only on a struct-bound method; 'new' can
  // never be static; 'default' can never be declared at all) is checked by
  // the Resolver (visitFuncDef), not here -- see ExportMutNotSupportedError
  // for the precedent this project already follows: a syntactically valid
  // but semantically-restricted combination is rejected downstream, with a
  // real diagnostic class, rather than in the parser.
  IsStaticMethodDecl = (CurTok == TOK_STATIC);
  if (IsStaticMethodDecl)
    getNextToken(); // eat 'static'

  if (!StructName.empty() && !IsStaticMethodDecl) {
    // Build 'self' as a pointer to a lazy NamedType(StructName) placeholder
    // rather than eagerly resolving StructName via GlobalSymbolTable here.
    //
    // This intentionally mirrors how every OTHER struct-type reference in
    // this parser works (see ParseType()'s `Base = std::make_shared<NamedType>(BaseName);`
    // fallback for any non-primitive name) instead of special-casing this
    // one spot to require the struct to already be registered.
    //
    // Why this matters: GlobalSymbolTable is populated by ParseStructDecl()
    // as it is encountered during this single top-to-bottom parse -- there is
    // no separate pre-pass that hoists struct names ahead of the parser the
    // way the Resolver later hoists them ahead of name resolution. An eager
    // lookup here previously meant `fn User.grow(): void { ... }` failed to
    // even PARSE ("Unknown struct type 'User' in method declaration") if it
    // appeared textually before `struct User { ... }` in the same file --
    // contradicting the language's forward-declaration story (plain function
    // calls, and struct fields referencing later-declared structs, both work
    // fine) for no reason other than this one spot being stricter than it
    // needs to be.
    //
    // This is safe: TypeChecker::visitFieldAccess/visitMethodCall and
    // Codegen's FieldAccessExprAST/MethodCallExprAST already unwrap a
    // TurfPointerType's pointee and, if it's a NamedType, resolve it via
    // GlobalSymbolTable->LookupType() at every `self.field` / `self.method()`
    // use site inside the method body -- that resolve-on-use path is exactly
    // what runs for every other NamedType field/parameter already, so 'self'
    // is not gaining or losing any capability here, just becoming consistent.
    //
    // Note this does mean a genuinely misspelled struct name in a method
    // declaration (`fn Usre.grow()`) is no longer caught here either -- but
    // that validation was never systematic to begin with (an ordinary
    // function parameter of an unknown struct type, e.g. `fn f(x: NoSuch)`,
    // already silently compiles with zero diagnostics today). Catching
    // unknown NamedType names belongs in the Resolver, which already has the
    // registered-struct-name machinery (isKnownStruct()) for exactly this,
    // currently applied only to struct fields.
    auto SelfPointee = std::make_shared<NamedType>(StructName);
    Params.insert(Params.begin(),
                  {std::make_shared<TurfPointerType>(SelfPointee), "self"});
  }

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
                                            nullptr, TypeParams,
                                            IsStaticMethodDecl);
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
                                          std::move(Body), TypeParams,
                                          IsStaticMethodDecl);
}

// Builds the reportable chain for a detected import cycle: the sub-sequence
// of ImportStack starting at the frame that re-enters Path (the module
// currently being closed back on) through to the top of the stack (whoever's
// `import ModuleName;` triggered this specific ParseFile() call). The caller
// appends ModuleName itself to close the loop when rendering.
static std::vector<ImportChainEntry>
BuildCircularImportChain(const std::vector<ImportStackFrame> &Stack,
                         const std::string &Path) {
  std::vector<ImportChainEntry> Chain;
  bool Started = false;
  for (const auto &Frame : Stack) {
    if (Frame.ResolvedPath == Path)
      Started = true;
    if (Started)
      Chain.push_back({Frame.ModuleName, Frame.ImportLoc});
  }
  return Chain;
}

std::unique_ptr<ExprAST> ParseFile(const std::string &Path,
                                   const std::string &ModuleName,
                                   SourceLocation ImportLoc) {
  // Circular-import check FIRST, before the ImportedFiles cache check below:
  // a module still being parsed (on ImportStack) is a genuine cycle, not a
  // cache hit. Checking ImportedFiles first would be wrong here -- that set
  // isn't populated for the CURRENT file until after this check anyway, but
  // conceptually the two checks answer different questions ("is this a
  // cycle" vs "did we already fully parse this") and cycle detection must
  // win when both could apply.
  for (const auto &Frame : ImportStack) {
    if (Frame.ResolvedPath == Path) {
      CircularImportError(ImportLoc, ModuleName,
                          BuildCircularImportChain(ImportStack, Path))
          .raise();
      return nullptr;
    }
  }

  if (ImportedFiles.count(Path)) {
    return std::make_unique<BlockExprAST>(
        std::vector<std::unique_ptr<ExprAST>>{});
  }

  std::ifstream ImportFile(Path);
  if (!ImportFile.is_open()) {
    return nullptr;
  }

  ImportedFiles.insert(Path);

  // Push this module onto the in-progress stack for the duration of parsing
  // its body; popped via RAII (including on every early-return path below,
  // e.g. the "nested ParseExpression already reported a real error" bailouts
  // already documented further down in this function) so a missed pop can
  // never silently poison every subsequent import in the same compile.
  struct ImportStackGuard {
    ImportStackGuard(std::string MN, std::string RP, SourceLocation Loc) {
      ImportStack.push_back({std::move(MN), std::move(RP), Loc});
    }
    ~ImportStackGuard() { ImportStack.pop_back(); }
  } IGuard(ModuleName, Path, ImportLoc);

  // Save current lexer state
  struct LexerStateGuard {
    std::ifstream *SavedSourceFile;
    int SavedLastChar, SavedCurTok, SavedCurLine, SavedCurCol;
    SourceLocation SavedCurLoc;
    std::string SavedFilename, SavedIdentifierStr, SavedStringVal;
    double SavedNumVal; long long SavedIntVal; bool SavedBoolVal;
    // SourceLines is what printDiag()'s caret snippet is snapshotted from
    // (see DiagnosticEngine::add() in Lexer.cpp) -- without saving/restoring
    // it here alongside SourceFilename, it never got populated for the
    // imported file at all, so any diagnostic raised while parsing an import
    // would snapshot the IMPORTING file's line text (whatever line happened
    // to share the same line number) instead of the module's real line.
    std::vector<std::string> SavedSourceLines;

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
      SavedSourceLines = SourceLines;
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
      SourceLines = SavedSourceLines;
    }
  } LGuard;

  // Set new lexer state
  SourceFile = &ImportFile;
  SourceFilename = Path;
  CurLine = 1;
  CurCol = 0;
  CurLoc = {1, 1};
  SetLastChar(' ');

  // Populate SourceLines for THIS file so any diagnostic raised while
  // parsing it snapshots the module's own line text, not the importer's
  // (see LexerStateGuard's doc comment above). Mirrors the read loop
  // main.cpp uses to populate SourceLines for the top-level input file.
  SourceLines.clear();
  {
    std::ifstream LineReader(Path);
    std::string Line;
    while (std::getline(LineReader, Line))
      SourceLines.push_back(Line);
  }
  // Also seed the persistent cross-pass cache -- see AllSourceLines' doc
  // comment in Lexer.h. Unlike SourceLines, this entry survives past the
  // LexerStateGuard's restore below, so Resolver/TypeChecker/Codegen
  // diagnostics about content inside THIS module can still find their line.
  AllSourceLines[Path] = SourceLines;

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

    if (CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM || CurTok == TOK_AT ||
        CurTok == TOK_EXTERN || CurTok == TOK_EXPORT) {
      auto Expr = ParseExpression();
      if (Expr) {
        if (CurTok == ';') {
          LogErrorAt(CurLoc, "Unexpected ';'");
          return nullptr;
        }
        Exprs.push_back(std::move(Expr));
      } else {
        // A nested ParseExpression() call already reported a real error
        // (via LogErrorAt) and returned nullptr -- bail out of this file
        // immediately rather than "skip one token and keep trying" (this
        // used to just call getNextToken() and continue the loop). Unlike
        // the top-level program's own parse loop, which deliberately keeps
        // going after an error to surface multiple problems in one compile,
        // there is no equivalent multi-error-collection UX for an imported
        // module -- and limping forward token-by-token through an already-
        // malformed declaration is actively dangerous here: confirmed to
        // eventually run the underlying ifstream's read position past EOF
        // repeatedly, which crashes the ENTIRE compiler with an unhandled
        // `std::__ios_failure` (not just this import failing cleanly) once
        // enough repeated `.get()` calls on an already-exhausted stream
        // trip libstdc++'s exception state. Reproduced via
        // `extern fn f(ptr: void*): void;` inside an imported module --
        // `ptr` collides with the `ptr` type keyword, which is itself a
        // separate, real stdlib bug (lib/std/mem.tr's own extern
        // declarations use `ptr` as a parameter name) -- but no single
        // syntax error inside an imported module should ever be able to
        // crash the compiler, regardless of what triggers it.
        return nullptr;
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
        // See the comment on the identical change above.
        return nullptr;
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
      // See the comment on the identical change above.
      return nullptr;
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

  // On-demand qualified forms: import Path::Symbol; / import Path::[a, b];
  // / import Path::[*]; -- each pulls SPECIFIC symbols into the importing
  // file's own flat (unqualified) namespace, instead of the bare form's
  // qualified-access-only behavior (see ImportMode's doc comment). The
  // module file itself is still always fully parsed below regardless of
  // mode -- even a single targeted symbol import needs the whole file
  // searched, since there's no lazy/partial parsing.
  ImportMode Mode = ImportMode::Bare;
  std::vector<ImportedSymbolRef> RequestedSymbols;
  if (CurTok == TOK_SCOPE) {
    getNextToken(); // eat '::'
    if (CurTok == '[') {
      getNextToken(); // eat '['
      if (CurTok == '*') {
        getNextToken(); // eat '*'
        if (CurTok != ']') {
          LogErrorAt(CurLoc, "Expected ']' after '*' in import list");
          return nullptr;
        }
        getNextToken(); // eat ']'
        Mode = ImportMode::Wildcard;
      } else {
        Mode = ImportMode::SymbolList;
        while (CurTok != ']' && CurTok != TOK_EOF) {
          if (CurTok != TOK_IDENTIFIER) {
            LogErrorAt(CurLoc, "Expected a symbol name in import list");
            return nullptr;
          }
          RequestedSymbols.push_back({IdentifierStr, CurLoc});
          getNextToken();
          if (CurTok == ',') {
            getNextToken();
          } else {
            break;
          }
        }
        if (CurTok != ']') {
          LogErrorAt(CurLoc, "Expected ']' to close import list");
          return nullptr;
        }
        getNextToken(); // eat ']'
        if (RequestedSymbols.empty()) {
          LogErrorAt(CurLoc, "Expected at least one symbol name in import list");
          return nullptr;
        }
      }
    } else if (CurTok == TOK_IDENTIFIER) {
      Mode = ImportMode::Symbol;
      RequestedSymbols.push_back({IdentifierStr, CurLoc});
      getNextToken();
    } else {
      LogErrorAt(CurLoc,
                 "Expected a symbol name or '[' after '::' in import");
      return nullptr;
    }
  }

  llvm::SmallString<128> StdlibPath(CompilerExecutablePath);
  llvm::sys::path::remove_filename(StdlibPath);
  llvm::sys::path::append(StdlibPath, "..", "lib", "std", ModuleName + ".tr");

  // Search order:
  //   1. Compiler stdlib (<binary>/../lib/std/<module>.tr)
  //   2. Same directory as the source file being compiled
  //   3. Current working directory
  //
  // A ParseFile() call can return nullptr for two very different reasons:
  // the file simply doesn't exist at that path (fine, keep trying the next
  // one), or the file DOES exist but has a genuine syntax error inside it
  // (LogErrorAt already recorded a real diagnostic -- trying the NEXT
  // fallback path at that point would be wrong: it can only ever produce a
  // confusing, unrelated "module not found" error stacked on top of the
  // real one, since the module WAS found, just broken). Distinguish the two
  // by snapshotting the diagnostic count before each attempt and checking
  // whether it grew.
  // ResolvedPath tracks whichever of the 3 candidates actually got parsed --
  // it's the canonical identity of "this physical module file", used by
  // both circular-import detection (Parser-level) and per-file import
  // scoping (Resolver-level) so neither has to re-derive or guess a path
  // string independently of the other.
  std::string ResolvedPath = StdlibPath.str().str();
  size_t DiagCountBefore = DiagnosticEngine::getDiagnosticCount();
  auto Block = ParseFile(ResolvedPath, ModuleName, Loc);
  bool FoundButBroken = !Block && DiagnosticEngine::getDiagnosticCount() > DiagCountBefore;
  if (!Block && !FoundButBroken) {
    // Issue #19 fix: resolve relative to the source file's directory so that
    // `turf path/to/file.tr` can find sibling modules without a cd first.
    if (!SourceFilename.empty()) {
      llvm::SmallString<256> SrcRelPath(SourceFilename);
      llvm::sys::path::remove_filename(SrcRelPath);
      llvm::sys::path::append(SrcRelPath, ModuleName + ".tr");
      ResolvedPath = SrcRelPath.str().str();
      DiagCountBefore = DiagnosticEngine::getDiagnosticCount();
      Block = ParseFile(ResolvedPath, ModuleName, Loc);
      FoundButBroken = !Block && DiagnosticEngine::getDiagnosticCount() > DiagCountBefore;
    }
  }
  if (!Block && !FoundButBroken) {
    ResolvedPath = ModuleName + ".tr";
    DiagCountBefore = DiagnosticEngine::getDiagnosticCount();
    Block = ParseFile(ResolvedPath, ModuleName, Loc);
    FoundButBroken = !Block && DiagnosticEngine::getDiagnosticCount() > DiagCountBefore;
    if (!Block && !FoundButBroken) {
      ModuleNotFoundError(Loc, ModuleName).raise();
      return nullptr;
    }
  }

  // The module WAS found at some path but failed to parse -- a real
  // diagnostic was already recorded there (see FoundButBroken's doc comment
  // above); don't construct a ModuleImportExprAST wrapping a null Block.
  if (!Block)
    return nullptr;

  return std::make_unique<ModuleImportExprAST>(Loc, ModuleName, ResolvedPath,
                                                std::move(Block), Mode,
                                                std::move(RequestedSymbols));
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
