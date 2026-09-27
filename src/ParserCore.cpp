/**
 * @file ParserCore.cpp
 * @brief Token stream engine: precedence climbing, ParseType, and the primary-expression dispatcher.
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

void InitializeCompilerGlobals() {
  InitializeSymbolTable();
  InitializePrecedence();
}

// See Parser.h for the doc comment -- moved here (non-static) from main.cpp
// so Sandbox.cpp's own recovery passes can share this logic; see #228/#233.
bool IsTopLevelBoundaryToken(int Tok) {
  return Tok == TOK_EOF || Tok == ';' || Tok == TOK_FN || Tok == TOK_STRUCT ||
         Tok == TOK_ENUM || Tok == TOK_MACHINE || Tok == TOK_EXTERN ||
         Tok == TOK_EXPORT ||
         Tok == TOK_OPERATOR || Tok == TOK_ERROR || Tok == TOK_IMPORT ||
         Tok == TOK_UNSAFE;
}

void SkipToNextTopLevelBoundary() {
  // Every call site of this function is a TOP-LEVEL per-statement recovery
  // loop (main.cpp's pre-pass/main-pass, Sandbox.cpp's two incremental
  // parse passes, CompileBufferBestEffort()) -- i.e. this is only ever
  // invoked at a point that logically SHOULD be top-level depth 0, even
  // when the ACTUAL g_braceDepth reading is wrong. It reads wrong whenever
  // the just-failed statement opened a '{'/'('/'[' it never got to close
  // before the parse bailed out (e.g. `fn broken(: int {` -- the '(' is
  // incremented, then the parse fails immediately on the next token,
  // before any ')' is ever seen) -- confirmed live: without this reset,
  // that permanent +1 offset makes the do-while below never see depth
  // return to 0 again until EOF, silently swallowing every remaining
  // top-level declaration in the file as "still skipping," not just the
  // one broken statement (issue #245).
  //
  // Resetting to 0 here is safe for the OTHER, already-correct case too
  // (a genuinely nested, not-yet-parsed tail that WILL close naturally,
  // e.g. 'operator -(a: Vec2): Vec2 { ... }' rejected for being unary,
  // with its ': Vec2 { return a; }' tail never parsed): by the time that
  // kind of failure reaches this function, the failed construct's own
  // brackets are already balanced (the arity check happens right after
  // the already-fully-matched parameter list), so g_braceDepth is already
  // 0 -- this reset is a no-op there, and the do-while below still
  // correctly walks forward through the real, still-untouched '{'...'}'
  // body by tracking depth incrementally from this point on, exactly as
  // before.
  g_braceDepth = 0;

  if (CurTok == TOK_EOF) return;

  // If the construct that just failed already consumed everything through
  // its own natural boundary before rejecting the result (e.g.
  // ParseLetStatement() eats a declaration's trailing ';' as part of
  // building the node; ParseExportDecl() then rejects the shape of that
  // already-complete node), CurTok is already sitting on the START of the
  // next legitimate declaration. Forcing an extra token here -- the old,
  // unconditional behavior below -- would eat that next declaration's own
  // leading keyword, corrupting the parse of a statement that was never
  // broken in the first place (confirmed: this desynced 'fn main(): int'
  // into a bogus bare call 'main()' after fail_export_array_no_initializer's
  // rejection, producing a second, unrelated "Expected ';'" error on top of
  // the real one).
  if (g_braceDepth == 0 && IsTopLevelBoundaryToken(CurTok)) return;

  // Otherwise there's genuine leftover garbage from the failed construct --
  // anywhere from a single bad token (e.g. an unsupported operator symbol,
  // rejected before any '(' was even reached) to an entire un-consumed
  // signature-and-body (e.g. 'operator -(a: Vec2): Vec2 { ... }' rejected
  // for being unary, with its ': Vec2 { return a; }' tail never parsed).
  // Skip forward, tracking nesting via g_braceDepth (maintained globally by
  // getNextToken(), see its own doc comment), until back at depth 0 AND
  // sitting on a real boundary -- always making at least one token of
  // forward progress, since we already know from the guard above that we
  // are NOT currently sitting on one.
  do {
    getNextToken();
  } while (CurTok != TOK_EOF &&
           (g_braceDepth > 0 || !IsTopLevelBoundaryToken(CurTok)));
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
  // Recurses into itself directly (generic type args, `!T` error unions,
  // pointer types) without ever going back through ParseExpression() --
  // needs its OWN recursion guard rather than relying on ParseExpression's
  // (found via code review: a deeply nested generic type,
  // `list<list<list<...>>>`, recurses this function directly and was
  // completely unbounded even after the ParseExpression()/ParseBlock()
  // guards existed). See ParseRecursionGuard's doc comment in Lexer.h.
  ParseRecursionGuard RecursionGuard;
  if (RecursionGuard.tooDeep()) {
    LogErrorAt(CurLoc,
               "Type nested too deeply to parse."
               "\n  Hint: check for unbalanced '<', '[', or '!'.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

  // `!T` -- a fallible/error-union type
  // (docs/architecture/errors_and_recover.md): T on success, or any declared
  // `error` type value on failure. Unambiguous with the unary logical-not
  // operator ('!' desugars to `x != true` -- see ParseUnary()) since that's
  // parsed entirely inside EXPRESSION parsing; ParseType() is a wholly
  // separate function only ever called from type positions (return types,
  // parameter types, var-decl annotations, struct/error-variant fields),
  // which previously rejected a leading '!' outright.
  if (CurTok == '!') {
    getNextToken(); // consume '!'
    TurfTypePtr SuccessType = ParseType();
    if (!SuccessType)
      return nullptr;
    return std::make_shared<TurfErrorUnionType>(std::move(SuccessType));
  }

  // `&T` -- a compiler-guaranteed non-null safe reference (issue #238) --
  // and `&?T` -- a nullable, NullabilityPass-checked safe reference
  // (LANG-002). Symmetric to the `!T` branch just above: its own
  // leading-token branch, ahead of the primitive/identifier dispatch below,
  // recursing into ParseType() for the pointee. Unambiguous with the binary
  // bitwise-AND operator ('&') and the unary address-of operator (also '&')
  // since both of those are parsed entirely inside EXPRESSION parsing --
  // ParseType() is a wholly separate function only ever called from type
  // positions. The one-token '?' lookahead right after '&' is similarly
  // unambiguous: '?' has no other meaning anywhere in this grammar.
  if (CurTok == '&') {
    getNextToken(); // consume '&'
    bool IsNullable = false;
    if (CurTok == '?') {
      IsNullable = true;
      getNextToken(); // consume '?'
    }
    TurfTypePtr Pointee = ParseType();
    if (!Pointee)
      return nullptr;
    if (IsNullable)
      return std::make_shared<TurfNullableReferenceType>(std::move(Pointee));
    return std::make_shared<TurfReferenceType>(std::move(Pointee));
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
      CurTok != TOK_PTR && CurTok != TOK_GUESS) {
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
  else if (CurTok == TOK_GUESS) {
    // `guess` is a terminal type-position placeholder (SEM-003, §11.2) —
    // never composed with generics/arrays/pointers, exactly like `ptr`
    // above. TypeChecker::visitVarDecl replaces it with the initializer's
    // real type; nothing between here and there ever inspects it beyond
    // the isGuess() checks ParseLetStatement/Resolver/TypeChecker add.
    getNextToken(); // consume 'guess'
    return TurfGuess();
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
      // OVH-009: the lexer's maximal munch (INV-LEX-1 -- never special-
      // cased there) fuses adjacent '>'-family characters into one token
      // whenever a nested generic argument list closes with no space
      // (`List<List<int>>` -> TOK_RSHIFT) or the close is immediately
      // followed by '=' (`List<T>=`/`Map<K, List<V>>=` -> TOK_GEQ/
      // TOK_RSHIFT_ASSIGN). The PARSER splits one '>' off such a token
      // here by replacing CurTok with whatever is left over -- crucially,
      // WITHOUT calling getNextToken(), which would ask the lexer for a
      // brand new token and silently skip the leftover instead of handing
      // it to whichever level of nesting (or the ordinary '=' this type
      // annotation is followed by) actually owns it next. `break`ing with
      // the replaced CurTok, rather than looping again, mirrors the real
      // '>' branch just above: this level's own list is closed either way,
      // real or split.
      if (CurTok == TOK_RSHIFT) {        // '>>' -> '>' then '>'
        CurTok = '>';
        break;
      }
      if (CurTok == TOK_RSHIFT_ASSIGN) { // '>>=' -> '>' then '>='
        CurTok = TOK_GEQ;
        break;
      }
      if (CurTok == TOK_GEQ) {           // '>=' -> '>' then '=' -- reached
                                          // directly (`List<T>=`) or as the
                                          // second split of a '>>=' above
        CurTok = TOK_ASSIGN;
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
        // A best-effort (saturating) reading of the literal, same as this
        // whole file did before SEM-001 -- an array size is always a small
        // positive int in practice, and NumLitClean/NumLitRadix (not the
        // exact llvm::APSInt NumberExprAST itself carries, since no
        // NumberExprAST node exists for a type-position literal like this
        // one at all) is all that's needed here; representability of an
        // array SIZE is unrelated to SEM-001/002's literal-representability
        // work, which is about literals used as VALUES, not this compile-
        // time-only dimension. See ParseNumberExpr's own comment
        // (ParserLiteral.cpp) for the exact-value path a real
        // value-position literal takes instead.
        Size = static_cast<int>(strtoll(NumLitClean.c_str(), nullptr, NumLitRadix));
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
  else {
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
      // See the identical comment on the other array-size site above (this
      // file, `T[N]` in a generic base type) for why the best-effort
      // NumLitClean/NumLitRadix reading is the right choice here too.
      Size = static_cast<int>(strtoll(NumLitClean.c_str(), nullptr, NumLitRadix));
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
                       "operator, or an incomplete statement.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;

  case TOK_SIZEOF: {
    SourceLocation Loc = CurLoc;
    getNextToken(); // eat sizeof
    if (CurTok != '(') {
      LogErrorAt(CurLoc, "Expected '(' after sizeof", SLM::ErrorCategory::SYNTAX_ERROR);
      return nullptr;
    }
    getNextToken(); // eat '('
    TurfTypePtr Ty = ParseType();
    if (!Ty)
      return nullptr;
    if (CurTok != ')') {
      LogErrorAt(CurLoc, "Expected ')' to close sizeof", SLM::ErrorCategory::SYNTAX_ERROR);
      return nullptr;
    }
    getNextToken(); // eat ')'
    return std::make_unique<SizeOfExprAST>(Loc, Ty);
  }

  case TOK_CALLER_LOCATION: {
    // Bare keyword, no arguments -- see CallerLocationExprAST's own doc
    // comment (ASTExpr.h) for why this is a dedicated node rather than
    // falling through to the ordinary identifier/VariableExprAST path
    // TOK_SELF shares below. Legality (must be lexically inside a function
    // declared '@caller_location') is entirely TypeChecker's job
    // (visitCallerLocation()), not checked here -- mirrors how ParseType()'s
    // '&'/'!' branches and this file's TOK_UNSAFE case never validate
    // context either.
    SourceLocation Loc = CurLoc;
    getNextToken(); // eat 'caller_location'
    return std::make_unique<CallerLocationExprAST>(Loc);
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
      // FDN-003: a struct declared earlier in THIS module is registered
      // under its identity (`M::IdName`), not its bare spelling.
      if (auto T = GlobalSymbolTable->LookupType(
              TypeIdentity::make(CurrentParseModule(), IdName))) {
        if (dynamic_cast<TurfStructType *>(T.get())) {
          LogErrorAt(IdLoc, "'" + IdName + " { ... }' is no longer valid syntax "
                             "for constructing a struct."
                             "\n  Hint: use '" + IdName + "::new(...)' (if '" +
                             IdName + "' declares a constructor) or '" + IdName +
                             "::default()' instead.", SLM::ErrorCategory::DECLARATION);
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
        LogErrorAt(CurLoc, "Expected a name after '::'", SLM::ErrorCategory::DECLARATION);
        return nullptr;
      }
      std::string RightName = IdentifierStr;
      SourceLocation RightLoc = CurLoc;
      getNextToken(); // eat right-hand name

      // Same removed-syntax hint as the bare form above, for the qualified
      // `Module::StructName { ... }` spelling -- looked up by the identity
      // the qualifier names (FDN-003: `IdName::RightName` IS that identity
      // when IdName is the owning module).
      if (CurTok == '{' && GlobalSymbolTable) {
        if (auto T = GlobalSymbolTable->LookupType(
                TypeIdentity::make(IdName, RightName))) {
          if (dynamic_cast<TurfStructType *>(T.get())) {
            LogErrorAt(IdLoc, "'" + IdName + "::" + RightName + " { ... }' is no "
                               "longer valid syntax for constructing a struct."
                               "\n  Hint: use '" + IdName + "::" + RightName +
                               "::new(...)' or '" + IdName + "::" + RightName +
                               "::default()' instead.", SLM::ErrorCategory::DECLARATION);
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
          LogErrorAt(CurLoc, "Expected a variant name after '::'", SLM::ErrorCategory::DECLARATION);
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
      // usually an identifier, number, or string — things that cannot
      // directly follow a valid variable expression without an operator.
      //
      // '{' is deliberately NOT one of these triggers (it used to be, until
      // this false-positive was found: `if x < d2 { ... }` misfired here on
      // 'd2' -- a perfectly ordinary variable one edit away from the
      // keyword 'do' -- because it's immediately followed by the if-block's
      // own opening brace). Unlike an adjacent identifier/number/string,
      // '{' is NOT unambiguous evidence of a typo: this language has no
      // parens around if/while/for/compare conditions, so "some expression
      // immediately followed by '{'" is also the ordinary shape of every
      // condition's last token right before its block -- indistinguishable
      // from a real keyword typo using only the next token. No test in this
      // suite exercises a typo'd keyword whose own very next token is '{'
      // with nothing in between (e.g. a hypothetical 'els { ... }'); such a
      // case still gets a real diagnostic, just a less specific one, from
      // whatever this identifier fails to parse as further down.
      if (CurTok == TOK_IDENTIFIER || CurTok == TOK_NUMBER ||
          CurTok == TOK_INT_LITERAL || CurTok == TOK_STRING_LITERAL) {
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
        LogErrorAt(CurLoc, "Expected ',' or ']' in array literal", SLM::ErrorCategory::ARRAY_ERROR);
        return nullptr;
      }
    }
    if (CurTok != ']') {
      LogErrorAt(CurLoc, "Expected ']' to close array literal", SLM::ErrorCategory::ARRAY_ERROR);
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

  case TOK_UNSAFE:
    return ParseUnsafeExpr();

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
                   "an expression.", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }

  case TOK_TYPE_INT:
  case TOK_TYPE_INT32:
  case TOK_TYPE_DOUBLE:
  case TOK_TYPE_STRING:
  case TOK_TYPE_BOOL:
  case TOK_TYPE_CHAR:
    LogErrorAt(CurLoc, "Expected 'mutable' or 'immutable' to begin a declaration.", SLM::ErrorCategory::DECLARATION);
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
    // lib/std/io.tr. STD-002 (ARCHITECTURE_OVERHAUL.md §8/§15.6(a)) adds
    // back exactly ONE decorator, `@caller_location fn ...` -- not a
    // general decorator syntax; any other `@word` still falls through to
    // the rejection below.
    SourceLocation AtLoc = CurLoc;
    getNextToken(); // eat '@'
    if (CurTok == TOK_CALLER_LOCATION) {
      getNextToken(); // eat 'caller_location'
      if (CurTok != TOK_FN) {
        LogErrorAt(CurLoc,
                   "Expected 'fn' after '@caller_location'."
                   "\n  Hint: '@caller_location' only decorates a function "
                   "declaration, e.g. '@caller_location\\nfn foo(...): T "
                   "{ ... }'.", SLM::ErrorCategory::SYNTAX_ERROR);
        return nullptr;
      }
      return ParseFuncDef(/*HasCallerLocation=*/true);
    }
    LogErrorAt(AtLoc,
               "'@' is not valid here.\n  Hint: the only supported decorator "
               "is '@caller_location', immediately before a function "
               "declaration.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

  case TOK_IMPORT:
    return ParseImport();

  case TOK_STRUCT:
    return ParseStructDecl();

  case TOK_ENUM:
    return ParseEnumDecl();

  case TOK_MACHINE:
    return ParseMachineDecl();

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

  case TOK_DEFER:
    return ParseDeferStmt();
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
  ParseRecursionGuard RecursionGuard;
  if (RecursionGuard.tooDeep()) {
    LogErrorAt(CurLoc,
               "Expression nested too deeply to parse."
               "\n  Hint: check for unbalanced '(', '[', or '{'.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

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
      LogErrorAt(CurLoc, "Expected 'otherwise' after 'give' expression in ternary", SLM::ErrorCategory::CONTROL_FLOW);
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
  // expression at the call site (docs/architecture/errors_and_recover.md).
  // Bound to the whole expression parsed so far (same "loosest binding"
  // position as the give/otherwise ternary just above), since every example in
  // errors_and_recover.md attaches `recover` directly to a call expression,
  // not to some inner sub-term of a larger arithmetic expression.
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
                           "\n  Hint: `recover as err run { ... }`.", SLM::ErrorCategory::CONTROL_FLOW);
        return nullptr;
      }
      std::string BoundName = IdentifierStr;
      getNextToken(); // consume the bound name

      if (CurTok != TOK_RUN) {
        LogErrorAt(CurLoc, "Expected 'run' after 'recover as " + BoundName +
                               "'."
                               "\n  Hint: `recover as " +
                               BoundName + " run { ... }`.", SLM::ErrorCategory::CONTROL_FLOW);
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
                       "`recover as err run { ... }`.", SLM::ErrorCategory::CONTROL_FLOW);
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
  // Recurses into itself directly for chained prefix operators (`!!!!x`,
  // `~~~x`, `---x`) without ever going back through ParseExpression() --
  // needs its OWN recursion guard rather than relying on ParseExpression's
  // (found via code review: `!` repeated ~100000 times recurses this
  // function that many C++ stack frames deep, completely unbounded even
  // after the ParseExpression()/ParseBlock() guards existed -- exactly the
  // uncatchable-stack-overflow failure class those guards were meant to
  // close). See ParseRecursionGuard's doc comment in Lexer.h.
  ParseRecursionGuard RecursionGuard;
  if (RecursionGuard.tooDeep()) {
    LogErrorAt(CurLoc,
               "Expression nested too deeply to parse."
               "\n  Hint: check for a runaway chain of unary operators.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

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
  // Prefix ++/-- -- LANG-001 follow-up: used to require a BARE
  // TOK_IDENTIFIER and desugar straight to `x = x + 1`/`x = x - 1`
  // (AssignmentExprAST(BinaryExprAST)), which made `++arr[0]`/`++obj.field`
  // a parse error even though the equivalent POSTFIX form (`arr[0]++`)
  // already worked -- postfix parses its operand through the general
  // primary+postfix-chain path (ParsePostfixExpression(), ParserLiteral.cpp)
  // and was never restricted to a bare identifier. Unified onto the exact
  // same PostfixExprAST node postfix already uses (see its own doc comment,
  // ASTExpr.h) with IsPrefix=true: the operand is parsed via ParsePrimary()
  // -- the identical primary+postfix-chain parse a bare `arr[0]` or
  // `obj.field` standing alone would get -- so `++arr[0]` now parses (and,
  // for a builtin array, codegens) exactly like `arr[0]++` does, just
  // returning the NEW value instead of the old one. A non-lvalue operand
  // (`++(a + b)`) is no longer a PARSE error the way "expected identifier"
  // was; it becomes a clean InvalidAssignmentTargetError at TypeCheck time
  // instead, via the same isAssignableLValue() check assignment/compound
  // assignment already use (TypeChecker::visitPostfixExpr(),
  // TypeCheckerExpr.cpp) -- postfix already had this exposure for a
  // non-lvalue operand with no dedicated check at all before this follow-up;
  // both directions get the real diagnostic now.
  if (CurTok == TOK_PLUS_PLUS || CurTok == TOK_MINUS_MINUS) {
    int OpType = CurTok;
    SourceLocation Loc = CurLoc;
    getNextToken();

    auto Operand = ParsePrimary();
    if (!Operand)
      return nullptr;

    char Op = (OpType == TOK_PLUS_PLUS) ? '+' : '-';
    return std::make_unique<PostfixExprAST>(Loc, Op, std::move(Operand),
                                            /*IsPrefix=*/true);
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
