/**
 * @file ParserCore.cpp
 * @brief Token stream engine: precedence climbing, ParseType, and the primary-expression dispatcher.
 * @details Part of the recursive-descent parser for TurfLang, split from the
 *          original monolithic Parser.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/planning/COMPILER_MODULE_BREAKDOWN.md).
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

// Current state of the Parser
int CurTok; // The current token the parser is looking at
static std::map<int, int> BinopPrecedence; // Precedence table: '*' > '+'

// Forward declaration: ParseBinOpRHS() (below) calls ParseUnary(), but
// ParseUnary() itself isn't defined until later in this same file -- this
// mirrors the forward-declaration block that used to sit at the top of the
// single monolithic Parser.cpp before the split (see ParserInternal.h for
// the cross-file equivalents; this one is Core-internal only).
std::unique_ptr<ExprAST> ParseUnary();

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
  // Assignment (incl. all compound-assignment forms)
  BinopPrecedence['='] = 10;
  BinopPrecedence[TOK_ASSIGN] = 10;
  BinopPrecedence[TOK_PLUS_ASSIGN] = 10;
  BinopPrecedence[TOK_MINUS_ASSIGN] = 10;
  BinopPrecedence[TOK_MUL_ASSIGN] = 10;
  BinopPrecedence[TOK_DIV_ASSIGN] = 10;
  BinopPrecedence[TOK_MOD_ASSIGN] = 10;
  BinopPrecedence[TOK_AND_ASSIGN] = 10;
  BinopPrecedence[TOK_OR_ASSIGN] = 10;
  BinopPrecedence[TOK_XOR_ASSIGN] = 10;
  BinopPrecedence[TOK_LSHIFT_ASSIGN] = 10;
  BinopPrecedence[TOK_RSHIFT_ASSIGN] = 10;
  BinopPrecedence[TOK_POWER_ASSIGN] = 10;

  // Cast. Must bind TIGHTER than assignment (12 > 10) so `y = x to int;`
  // parses as `y = (x to int)`, not `(y = x) to int` -- the latter used to
  // silently discard the cast entirely (the assignment's own value, already
  // computed and stored, just got cast-and-thrown-away), which was live in
  // lib/std (`self.data = mem::malloc(n) to T*;` in list.tr/stack.tr/queue.tr/
  // map.tr/mem.tr) and only "worked" by accident because raw `ptr` already
  // implicitly coerces to any `T*` (see typesCompatible()'s Rule 2) -- the
  // explicit cast was dead weight, not the thing actually making it compile.
  // Still looser than every other real binary operator (15+) so
  // `1 + 2 to double` keeps meaning `(1 + 2) to double`, unchanged.
  BinopPrecedence[TOK_TO] = 12;

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

// Returns the priority of the current operator. If it's not an operator (like a
// number), returns -1.
static int GetTokPrecedence() {
  auto It = BinopPrecedence.find(CurTok);
  if (It == BinopPrecedence.end() || It->second <= 0)
    return -1;
  return It->second;
}

// list<T> declarations have no dedicated parse function: ParseType() already
// produces a GenericType("list", [elemType]) for any `Name<...>` spelling,
// so `list<int> xs = ...;` flows through the exact same declaration path as
// `int x = ...;` -- ParseLetStatement(), the only variable-declaration
// entry point this parser actually calls.

// ParseType: recursively parse a type expression from the token stream.
// Handles primitives, void, generics like list<int>, and arrays like int[5].
// Trailing [N] brackets are consumed here, making array types fully
// first-class. Returns nullptr on failure.
TurfTypePtr ParseType() {
  // `!T` -- a fallible/error-union type (docs/planning/Errors.md): T on
  // success, or any declared `error` type value on failure. Unambiguous with
  // the unary logical-not operator ('!' desugars to `x != true` -- see
  // ParseUnary()) since that's parsed entirely inside EXPRESSION parsing;
  // ParseType() is a wholly separate function only ever called from type
  // positions (return types, parameter types, var-decl annotations, struct/
  // error-variant fields), which previously rejected a leading '!' outright.
  if (CurTok == '!') {
    getNextToken(); // consume '!'
    TurfTypePtr SuccessType = ParseType();
    if (!SuccessType)
      return nullptr;
    return std::make_shared<TurfErrorUnionType>(std::move(SuccessType));
  }

  // Function-value type: fn(T1, T2, ...): RetType -- e.g. the `f` parameter
  // in `fn apply(f: fn(int): int, x: int): int`. Handled as its own branch,
  // before the primitive/identifier dispatch below, since 'fn' is a keyword
  // token (TOK_FN) here, not a TOK_IDENTIFIER -- this can never collide with
  // a top-level `fn name(...)` declaration, which is parsed by
  // ParseFuncDef() at the statement level and never calls ParseType() to
  // consume its own leading 'fn'.
  if (CurTok == TOK_FN) {
    getNextToken(); // consume 'fn'
    if (CurTok != '(') {
      SyntaxError(CurLoc, "Expected '(' after 'fn' in a function type").raise();
      return nullptr;
    }
    getNextToken(); // consume '('
    std::vector<TurfTypePtr> ParamTypes;
    if (CurTok != ')') {
      while (true) {
        TurfTypePtr ParamTy = ParseType();
        if (!ParamTy)
          return nullptr;
        ParamTypes.push_back(std::move(ParamTy));
        if (CurTok == ',') {
          getNextToken();
          continue;
        }
        break;
      }
    }
    if (CurTok != ')') {
      SyntaxError(CurLoc, "Expected ')' to close a function type's parameter list").raise();
      return nullptr;
    }
    getNextToken(); // consume ')'
    if (CurTok != TOK_COLON) {
      SyntaxError(CurLoc, "Expected ':' before a function type's return type").raise();
      return nullptr;
    }
    getNextToken(); // consume ':'
    TurfTypePtr RetType = ParseType();
    if (!RetType)
      return nullptr;
    return std::make_shared<FuncType>(std::move(RetType), std::move(ParamTypes));
  }

  if (CurTok != TOK_IDENTIFIER && CurTok != TOK_TYPE_INT &&
      CurTok != TOK_TYPE_INT32 &&
      CurTok != TOK_TYPE_DOUBLE && CurTok != TOK_TYPE_BOOL &&
      CurTok != TOK_TYPE_CHAR &&
      CurTok != TOK_TYPE_STRING && CurTok != TOK_TYPE_VOID &&
      CurTok != TOK_PTR) {
    SyntaxError(CurLoc, "Expected a type name").raise();
    return nullptr;
  }

  std::string BaseName;
  if (CurTok == TOK_TYPE_INT)
    BaseName = "int";
  else if (CurTok == TOK_TYPE_INT32)
    BaseName = "int32";
  else if (CurTok == TOK_TYPE_DOUBLE)
    BaseName = "double";
  else if (CurTok == TOK_TYPE_BOOL)
    BaseName = "bool";
  else if (CurTok == TOK_TYPE_CHAR)
    BaseName = "char";
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
  else if (BaseName == "int32")
    Base = TurfInt32();
  else if (BaseName == "double")
    Base = TurfDouble();
  else if (BaseName == "bool")
    Base = TurfBool();
  else if (BaseName == "char")
    Base = TurfChar();
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
bool isDisallowedVoidType(const TurfType *T) {
  if (!T)
    return false;
  if (T->isVoid())
    return true;
  if (isArrayType(T))
    return isDisallowedVoidType(getArrayElementType(T));
  return false;
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

  case TOK_CHAR_LITERAL:
    return ParseCharExpr();

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

  case TOK_DO:
    return ParseDoWhileExpr();

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
  case TOK_TYPE_INT32:
  case TOK_TYPE_DOUBLE:
  case TOK_TYPE_STRING:
  case TOK_TYPE_BOOL:
  case TOK_TYPE_CHAR:
    LogErrorAt(CurLoc, "Expected 'mutable' or 'immutable' to begin a declaration.");
    return nullptr;

  case TOK_TYPE_VOID:
    SyntaxError(CurLoc, "'void' may only be used as a function return type")
        .raise();
    return nullptr;

  case TOK_FN:
    return ParseFuncDef();

  case TOK_OPERATOR:
    return ParseOperatorDecl();

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

  case TOK_ERROR:
    return ParseErrorDecl();

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
    // Right-associative operators: all assignment forms (so `a = b = c`
    // groups as `a = (b = c)`) and exponentiation (so `2 ** 3 ** 2` groups
    // as `2 ** (3 ** 2)` == 512, matching every other language that has a
    // `**` operator -- this used to be missing, silently making `**`
    // left-associative like `+`/`*` instead (`(2**3)**2` == 64), with no
    // test ever exercising a chained exponent to catch it.
    bool isRightAssociative =
        (BinOp == TOK_ASSIGN || BinOp == TOK_PLUS_ASSIGN ||
         BinOp == TOK_MINUS_ASSIGN || BinOp == TOK_MUL_ASSIGN ||
         BinOp == TOK_DIV_ASSIGN || BinOp == TOK_MOD_ASSIGN ||
         BinOp == TOK_AND_ASSIGN || BinOp == TOK_OR_ASSIGN ||
         BinOp == TOK_XOR_ASSIGN || BinOp == TOK_LSHIFT_ASSIGN ||
         BinOp == TOK_RSHIFT_ASSIGN || BinOp == TOK_POWER_ASSIGN ||
         BinOp == TOK_POWER);
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
               BinOp == TOK_MOD_ASSIGN || BinOp == TOK_AND_ASSIGN ||
               BinOp == TOK_OR_ASSIGN || BinOp == TOK_XOR_ASSIGN ||
               BinOp == TOK_LSHIFT_ASSIGN || BinOp == TOK_RSHIFT_ASSIGN ||
               BinOp == TOK_POWER_ASSIGN) {
      // Opcode is the plain operator this desugars to -- reusing the exact
      // same encoding BinaryExprAST uses (raw ASCII for single-char ops,
      // the token enum for multi-char ones) so TypeChecker/Codegen's
      // existing per-operator switches can be shared/mirrored directly
      // instead of inventing a second parallel encoding.
      int Op = '+';
      if (BinOp == TOK_MINUS_ASSIGN)
        Op = '-';
      else if (BinOp == TOK_MUL_ASSIGN)
        Op = '*';
      else if (BinOp == TOK_DIV_ASSIGN)
        Op = '/';
      else if (BinOp == TOK_MOD_ASSIGN)
        Op = '%';
      else if (BinOp == TOK_AND_ASSIGN)
        Op = '&';
      else if (BinOp == TOK_OR_ASSIGN)
        Op = '|';
      else if (BinOp == TOK_XOR_ASSIGN)
        Op = '^';
      else if (BinOp == TOK_LSHIFT_ASSIGN)
        Op = TOK_LSHIFT;
      else if (BinOp == TOK_RSHIFT_ASSIGN)
        Op = TOK_RSHIFT;
      else if (BinOp == TOK_POWER_ASSIGN)
        Op = TOK_POWER;

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

  // `<expr> recover ...` -- handles the failure case of a `!T`-typed call
  // expression at the call site (docs/planning/Errors.md). Bound to the
  // whole expression parsed so far (same "loosest binding" position as the
  // give/otherwise ternary just above), since every example in Errors.md
  // attaches `recover` directly to a call expression, not to some inner
  // sub-term of a larger arithmetic expression.
  if (CurTok == TOK_RECOVER) {
    SourceLocation RecoverLoc = CurLoc;
    getNextToken(); // consume 'recover'

    if (CurTok == TOK_PRODUCE) {
      getNextToken(); // consume 'produce'
      auto ProduceExpr = ParseExpression();
      if (!ProduceExpr)
        return nullptr;
      return std::make_unique<RecoverExprAST>(
          RecoverLoc, std::move(LHS), RecoverKind::Produce,
          std::move(ProduceExpr), "", nullptr);
    }

    if (CurTok == TOK_RUN) {
      getNextToken(); // consume 'run'
      auto Block = ParseBlock();
      if (!Block)
        return nullptr;
      return std::make_unique<RecoverExprAST>(RecoverLoc, std::move(LHS),
                                              RecoverKind::Run, nullptr, "",
                                              std::move(Block));
    }

    if (CurTok == TOK_AS) {
      getNextToken(); // consume 'as'
      if (CurTok != TOK_IDENTIFIER) {
        LogErrorAt(CurLoc, "Expected an identifier after 'recover as' to "
                           "bind the failing error value."
                           "\n  Hint: `recover as err run { ... }`.");
        return nullptr;
      }
      std::string BoundName = IdentifierStr;
      getNextToken(); // consume the bound name

      if (CurTok != TOK_RUN) {
        LogErrorAt(CurLoc, "Expected 'run' after 'recover as " + BoundName +
                               "'."
                               "\n  Hint: `recover as " +
                               BoundName + " run { ... }`.");
        return nullptr;
      }
      getNextToken(); // consume 'run'

      auto Block = ParseBlock();
      if (!Block)
        return nullptr;
      return std::make_unique<RecoverExprAST>(RecoverLoc, std::move(LHS),
                                              RecoverKind::AsRun, nullptr,
                                              BoundName, std::move(Block));
    }

    LogErrorAt(CurLoc, "Expected 'produce', 'run', or 'as' after 'recover'."
                       "\n  Hint: `recover produce value`, "
                       "`recover run { ... }`, or "
                       "`recover as err run { ... }`.");
    return nullptr;
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
    auto NotExpr = std::make_unique<BinaryExprAST>(
        Loc, TOK_NEQ,
        std::move(Operand),
        std::make_unique<BoolExprAST>(true)
    );
    // Marks this node as compiler-synthesized, not literal user source --
    // see BinaryExprAST::IsDesugaredNot's doc comment (AST.h) for why
    // Lint.cpp needs to tell the two apart.
    NotExpr->IsDesugaredNot = true;
    return NotExpr;
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

  // If the current token is not an operator that handles unary (like '-' or
  // '+'), then it must be a primary expression.
  if (CurTok != '-' && CurTok != '+')
    return ParsePrimary();

  int Opc = CurTok;
  SourceLocation Loc = CurLoc;
  getNextToken();

  if (auto Operand = ParseUnary())
    return std::make_unique<UnaryExprAST>(Loc, Opc, std::move(Operand));

  return nullptr;
}
