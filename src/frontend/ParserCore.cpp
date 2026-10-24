/**
 * @file ParserCore.cpp
 * @brief Token stream engine: precedence climbing, ParseType, and the primary-expression dispatcher.
 * @details Part of the recursive-descent parser for TurfLang. The parser is
 *          split by category, mirroring AST.h's ASTExpr/ASTStmt/ASTDecl/
 *          ASTAccess split (see docs/architecture/compiler_pipeline.md).
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

#include "frontend/Parser.h"
#include "frontend/AST.h"
#include "analysis/Algorithms.h"
#include "sema/Builtins.h"
#include "codegen/Codegen.h"
#include "diagnostics/Errors.h"
#include "frontend/Lexer.h"
#include "diagnostics/SuggestionIndex.h"
#include "resolve/SymbolTable.h"
#include "frontend/ParserInternal.h"
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

// Forward declaration: ParseBinOpRHS() (below) calls ParseUnary(), which is
// defined later in this file (ParserInternal.h holds the cross-file
// declarations; this one is Core-internal only).
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
  if (CurTok == '{')
    g_curlyDepth++;
  else if (CurTok == '}' && g_curlyDepth > 0)
    g_curlyDepth--;
  if (takeLookaheadToken(CurTok))
    return CurTok;
  return CurTok = gettok();
}

SLM::DiagnosticHints MissingSemicolonHints() {
  SLM::DiagnosticHints Hints;
  // Only an unambiguous missing ';': the next token begins a later line (the
  // classic "forgot it at the end of the line"), or closes the block. On the
  // same line anything else could be a missing operator or comma, and a ';'
  // there would validate as a different program than the one meant.
  const bool NextLine = PrevTokEndLine > 0 && CurLoc.Line > PrevTokEndLine;
  if (!NextLine && CurTok != '}')
    return Hints;
  if (PrevTokEndLine < 1 || PrevTokEndCol < 2)
    return Hints;
  // The source must show the previous token ending exactly where the lexer
  // says: text on the line up to that column, and not whitespace just before
  // it. A multi-line token (a string with a raw newline) ends on a line this
  // column does not describe, so it never passes.
  const std::string *Line =
      FixItSourceLine(SourceLocation{PrevTokEndLine, PrevTokEndCol, CurLoc.Filename});
  if (!Line || PrevTokEndCol > static_cast<int>(Line->size()) + 1 ||
      std::isspace(static_cast<unsigned char>((*Line)[PrevTokEndCol - 2])))
    return Hints;
  Hints.FixIts.push_back({PrevTokEndLine, PrevTokEndCol, PrevTokEndCol, ";"});
  Hints.FixReason = "Every statement has to end with ';', and this one runs "
                    "straight into what follows it, so the ';' is missing "
                    "right after its last token.";
  return Hints;
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

  // Cast: just tighter than comparison (31 > 30), so a cast is an operand of
  // a comparison and of `&&`/`||` -- `a < 3 && s to int > 5` is
  // `(a < 3) && ((s to int) > 5)` -- and still of assignment (`y = x to int;`
  // stores the cast value). It stays looser than the bitwise, shift and
  // arithmetic operators, so `1 + 2 to double` is `(1 + 2) to double`.
  BinopPrecedence[TOK_TO] = 31;

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

  // Exponentiation (`**`)
  BinopPrecedence[TOK_POWER] = 60;
}

void InitializeCompilerGlobals() {
  InitializeSymbolTable();
  InitializePrecedence();
}

// See Parser.h for the doc comment.
bool IsTopLevelBoundaryToken(int Tok) {
  return Tok == TOK_EOF || Tok == ';' || Tok == TOK_FN || Tok == TOK_STRUCT ||
         Tok == TOK_ENUM || Tok == TOK_MACHINE || Tok == TOK_EXTERN ||
         Tok == TOK_EXPORT || Tok == TOK_PUB ||
         Tok == TOK_OPERATOR || Tok == TOK_ERROR || Tok == TOK_IMPORT ||
         Tok == TOK_UNSAFE;
}

void SkipToNextTopLevelBoundary() {
  // Every call site is a top-level per-statement recovery loop (main.cpp's
  // pre-pass/main-pass, the query facade's entry-file loop in
  // AnalysisFacade.cpp), so this runs where nesting should be top level.
  // g_braceDepth can still read wrong there: a failed statement may have
  // opened a '{'/'('/'[' it never closed (e.g. `fn broken(: int {`: the '(' is
  // counted, then the parse fails before any ')'). Left as is, that permanent
  // +1 keeps the do-while below from ever seeing depth 0 again, so it would
  // swallow every remaining top-level declaration up to EOF.
  //
  // The reset is not to 0 but to the CURLY depth, which drops only the stray
  // `(`/`[` imbalance. For an error in a function's own header the two agree
  // (no `{` is open yet). For an error in the BODY (a lambda attempt,
  // `List<int>::default()`, `fn(` where an expression belongs) the body's `{`
  // is still open, and resetting to 0 would stop the skip at the next `;`
  // inside that body: the rest of the body would parse as top-level
  // statements and draw a false "Statements are not allowed outside of a
  // function" ahead of the real error. Keeping the `{` count resynchronises
  // on the function's own closing brace.
  //
  // When the failed construct's brackets are already balanced (e.g.
  // 'operator -(a: Vec2): Vec2 { ... }' rejected for being unary, right after
  // its parameter list), the reset is a no-op and the loop below walks
  // through the untouched '{'...'}' body.
  g_braceDepth = g_curlyDepth;

  if (CurTok == TOK_EOF) return;

  // If the failed construct already consumed everything through its own
  // natural boundary before rejecting the result (e.g. ParseLetStatement()
  // eats a declaration's trailing ';' as part of building the node, and
  // ParseExportDecl() then rejects the shape of that complete node), CurTok
  // is already on the START of the next legitimate declaration. Forcing a
  // token here would eat that declaration's leading keyword and desync a
  // statement that was never broken (`fn main(): int` would become a bare
  // call `main()` and draw a second, unrelated "Expected ';'").
  if (g_braceDepth == 0 && IsTopLevelBoundaryToken(CurTok)) return;

  // Otherwise the failed construct left unconsumed text: anywhere from a
  // single bad token (e.g. an unsupported operator symbol, rejected before
  // any '(') to a whole signature and body (e.g. the unary 'operator -'
  // above, with its ': Vec2 { return a; }' tail never parsed). Skip forward,
  // tracking nesting via g_braceDepth (kept by getNextToken()), until back at
  // depth 0 AND on a real boundary. At least one token is always consumed.
  //
  // A lexer error in the skipped text is reported and skipped too: this runs
  // inside a recovery point's catch block, and letting the error propagate
  // would escape that recovery point.
  do {
    try {
      getNextToken();
    } catch (const RecoverUnwind &) {
    }
  } while (CurTok != TOK_EOF &&
           (g_braceDepth > 0 || !IsTopLevelBoundaryToken(CurTok)));

  // Stopped at a top-level boundary, so by definition no `{` is open -- even
  // if the skipped text closed a `(` with a `}` and left the counters
  // disagreeing, which must not leak into the next declaration.
  g_braceDepth = 0;
  g_curlyDepth = 0;
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
bool g_AllowGuessType = false;
std::vector<std::string> g_SizeParamsInScope;

// `[N]` in a type, with N a size parameter in scope (`fn<T, N: size>`):
// consumes N and returns its name; otherwise consumes nothing and returns "".
static std::string TakeSizeParamDim() {
  if (CurTok != TOK_IDENTIFIER ||
      std::find(g_SizeParamsInScope.begin(), g_SizeParamsInScope.end(),
                IdentifierStr) == g_SizeParamsInScope.end())
    return "";
  std::string Name = IdentifierStr;
  getNextToken();
  return Name;
}

// Wraps Base in one array dimension: a number, `[]` (-1), or a size
// parameter's name.
static TurfTypePtr WrapArrayDim(TurfTypePtr Base, const std::pair<int, std::string> &Dim) {
  if (!Dim.second.empty())
    return std::make_shared<ArrayType>(std::move(Base), Dim.second);
  return makeArrayType(std::move(Base), Dim.first);
}

static TurfTypePtr ParseTrailingArrayDims(TurfTypePtr Base);

// LG-1: if the '<' at CurTok opens a list of type-shaped tokens that closes
// with its '>', the kind of the token after that '>' (0 otherwise), and in
// `SawTypeKeyword` whether the list names a built-in type (`int`, `string`,
// ...), which no operand can be. Only tokens that can appear in a type are
// read, at most a few dozen, and none is consumed (peekToken()).
static int TypeArgListFollower(bool *SawTypeKeyword = nullptr) {
  constexpr size_t MaxTokens = 64;
  int Depth = 1;
  bool Keyword = false;
  for (size_t I = 0; I < MaxTokens; ++I) {
    // An array size (`int[3]`) is the one place a number appears in a type.
    if (peekToken(I) == TOK_INT_LITERAL && (I == 0 || peekToken(I - 1) != '['))
      return 0;
    switch (peekToken(I)) {
    case '<': ++Depth; break;
    case '>': --Depth; break;
    case TOK_RSHIFT: Depth -= 2; break;
    case TOK_TYPE_INT: case TOK_TYPE_INT32: case TOK_TYPE_DOUBLE:
    case TOK_TYPE_BOOL: case TOK_TYPE_CHAR: case TOK_TYPE_STRING:
    case TOK_TYPE_VOID: case TOK_PTR:
      Keyword = true;
      break;
    case TOK_IDENTIFIER: case TOK_SCOPE: case ',': case '[': case ']':
    case TOK_INT_LITERAL: case '*': case '&': case '?': case '!': case TOK_FN:
    case '(': case ')': case TOK_COLON:
      break;
    default:
      return 0;
    }
    if (Depth == 0) {
      if (SawTypeKeyword)
        *SawTypeKeyword = Keyword;
      return peekToken(I + 1);
    }
    if (Depth < 0)
      return 0;
  }
  return 0;
}

// LG-1: does the '<' at CurTok open a type-argument list that closes with
// '>' immediately followed by '::' (`List<int>::default()`)? Anything else
// means the '<' is less-than. Committing is safe because `a < b > ::c` is a
// syntax error with '<' as less-than: '::' cannot begin an operand, so only
// programs rejected before change meaning.
static bool TypeArgsThenScopeAhead() {
  return TypeArgListFollower() == TOK_SCOPE;
}

// A type-argument list, `<T1, T2, ...>`, CurTok at its '<'. Shared by a
// generic type (`List<int>`) and the expression-position spellings
// `List<int>::default()` (LG-1) and `identity::<int>` (LG-2), so all three
// read type arguments the same way. False after reporting an error.
bool ParseTypeArgList(std::vector<TurfTypePtr> &TypeArgs) {
  getNextToken(); // consume '<'
  while (true) {
    TurfTypePtr Arg = ParseType();
    if (!Arg)
      return false;
    TypeArgs.push_back(std::move(Arg));
    if (CurTok == '>') {
      getNextToken(); // consume '>'
      break;
    }
    // The lexer's maximal munch (INV-LEX-1 -- never special-
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
      SyntaxError(CurLoc, ErrCode::MissingComma, "Expected '>' or ',' in generic type").raise();
      return false;
    }
    getNextToken(); // consume ','
  }
  return true;
}

TurfTypePtr ParseType() {
  // Recurses into itself directly (generic type args, `!T` error unions,
  // pointer types) without going back through ParseExpression(), so it needs
  // its OWN recursion guard: a deeply nested `list<list<list<...>>>` would
  // otherwise be unbounded. See ParseRecursionGuard's doc comment in Lexer.h.
  ParseRecursionGuard RecursionGuard;
  if (RecursionGuard.tooDeep()) {
    LogErrorAt(CurLoc, ErrCode::NestingTooDeep,
               "Type nested too deeply to parse."
               "\n  Hint: check for unbalanced '<', '[', or '!'.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

  // `!T` -- a fallible/error-union type
  // (docs/architecture/errors_and_recover.md): T on success, or any declared
  // `error` type value on failure. Unambiguous with the unary logical-not
  // operator ('!' desugars to `x != true` -- see ParseUnary()) since that's
  // parsed entirely inside EXPRESSION parsing; ParseType() is only ever
  // called from type positions (return types, parameter types, var-decl
  // annotations, struct/error-variant fields).
  if (CurTok == '!') {
    getNextToken(); // consume '!'
    TurfTypePtr SuccessType = ParseType();
    if (!SuccessType)
      return nullptr;
    return std::make_shared<TurfErrorUnionType>(std::move(SuccessType));
  }

  // `&T` -- a compiler-guaranteed non-null safe reference -- and `&?T` -- a
  // nullable, NullabilityPass-checked safe reference. Symmetric to the `!T` branch just above: its own
  // leading-token branch, ahead of the primitive/identifier dispatch below,
  // recursing into ParseType() for the pointee. Unambiguous with the binary
  // bitwise-AND operator ('&') and the unary address-of operator (also '&')
  // since both of those are parsed entirely inside EXPRESSION parsing --
  // ParseType() is a wholly separate function only ever called from type
  // positions. The one-token '?' lookahead right after '&' is similarly
  // unambiguous: '?' has no other meaning anywhere in this grammar.
  // `(&N)[2]` -- an array (or `(&N)[]` slice) of references. `&N[2]` is a
  // reference to an array, because `&` takes the whole type after it, so an
  // array of references needs the parentheses. A function type
  // needs them for the same reason: in `fn(int): int[2]` the `[2]` belongs
  // to the return type, so an array of function values is
  // `(fn(int): int)[2]`. Only those two may be parenthesised, and only to
  // put array dimensions after them: every other type takes `[N]` directly.
  if (CurTok == '(') {
    SourceLocation OpenLoc = CurLoc;
    getNextToken(); // consume '('
    if (CurTok != '&' && CurTok != TOK_FN) {
      SyntaxError(OpenLoc, ErrCode::ExpectedType,
                  "Parentheses in a type are only for an array of references "
                  "or of functions, e.g. (&Node)[4] or (fn(int): int)[2].",
                  "Write the type without the parentheses, e.g. `int[4]`.")
          .raise();
      return nullptr;
    }
    TurfTypePtr Elem = ParseType();
    if (!Elem)
      return nullptr;
    if (CurTok != ')') {
      SyntaxError(CurLoc, ErrCode::UnclosedDelimiter,
                  "Expected ')' to close the parenthesised type, "
                  "e.g. (&Node)[4] or (fn(int): int)[2].")
          .raise();
      return nullptr;
    }
    getNextToken(); // consume ')'
    if (CurTok != '[') {
      SyntaxError(OpenLoc, ErrCode::ExpectedType,
                  "Parentheses in a type are only for an array of references "
                  "or of functions; expected '[' after ')'.",
                  "A single one needs no parentheses: write `" +
                      Elem->getName() + "`.")
          .raise();
      return nullptr;
    }
    return ParseTrailingArrayDims(std::move(Elem));
  }

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
      SyntaxError(CurLoc, ErrCode::MissingOpeningDelimiter, "Expected '(' after 'fn' in a function type").raise();
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
      SyntaxError(CurLoc, ErrCode::UnclosedDelimiter, "Expected ')' to close a function type's parameter list").raise();
      return nullptr;
    }
    getNextToken(); // consume ')'
    if (CurTok != TOK_COLON) {
      SyntaxError(CurLoc, ErrCode::MissingColon, "Expected ':' before a function type's return type").raise();
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
    SyntaxError(CurLoc, ErrCode::ExpectedType, "Expected a type name").raise();
    return nullptr;
  }

  // A built-in type is never generic, so a '<' after one is the less-than
  // operator: `(s to int) < n` needs no parentheses, `s to int < n` parses
  // the same way.
  const bool IsBuiltinType = CurTok != TOK_IDENTIFIER;
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
    // `guess` is a terminal type-position placeholder (FREEZE.md §6.3)
    // -- never composed with generics/arrays/pointers, exactly like `ptr`
    // above. TypeChecker::visitVarDecl replaces it with the initializer's
    // real type.
    //
    // F-GUESS-3 (FREEZE.md §6.3): inference is for a local variable's
    // initializer only -- never a parameter, return type, struct field, error
    // payload or generic argument, none of which has an initializer. Allowed
    // there, it would surface later and differently per position (a struct
    // field would compile, a parameter would fail at the call). Only the
    // local-declaration parse (ParseLetStatement) sets the flag.
    if (!g_AllowGuessType) {
      LogErrorAt(CurLoc, ErrCode::MissingInitializer,
                 "'guess' needs an initializer to infer from, so it can only "
                 "declare a local variable."
                 "\n  Hint: a parameter, return type or struct field has no "
                 "initializer; write its type, e.g. `x: int`.",
                 SLM::ErrorCategory::DECLARATION);
      getNextToken(); // consume 'guess' so the caller's recovery starts after it
      return nullptr;
    }
    getNextToken(); // consume 'guess'
    return TurfGuess();
  }
  else
    BaseName = IdentifierStr; // user-defined

  getNextToken(); // consume the type name

  // Module-qualified type: Module::TypeName<Args> (e.g. list::List<int>).
  // BaseName parsed above was actually the module qualifier; re-bind it to
  // the real type name that follows '::', and carry the qualifier forward
  // into the NamedType/GenericType built below. The Resolver validates it
  // (Resolver::validateModuleQualifier(), reached from validateNamedType())
  // after its first pass has registered every module, which cannot be known
  // here while the program is still being parsed.
  std::string ModuleQualifier;
  if (CurTok == TOK_SCOPE) {
    getNextToken(); // eat '::'
    if (CurTok != TOK_IDENTIFIER) {
      SyntaxError(CurLoc, ErrCode::ExpectedType, "Expected a type name after '::'").raise();
      return nullptr;
    }
    // The qualifier as written may be an alias for a dependency module's
    // identity (ModuleKeyFor(), ParserModule.cpp).
    ModuleQualifier = ModuleKeyFor(BaseName);
    BaseName = IdentifierStr;
    getNextToken();
  }

  // Check for generic parameter: BaseName<T1, T2, ...>
  if (CurTok == '<' && !IsBuiltinType) {
    std::vector<TurfTypePtr> TypeArgs;
    if (!ParseTypeArgList(TypeArgs))
      return nullptr;
    // Arrays of generics are rare but allowed: parse trailing [N]
    TurfTypePtr GenBase =
        std::make_shared<GenericType>(BaseName, std::move(TypeArgs), ModuleQualifier);
    while (CurTok == '*') {
      getNextToken();
      GenBase = std::make_shared<TurfPointerType>(GenBase);
    }
    std::vector<std::pair<int, std::string>> GenArraySizes;
    while (CurTok == '[') {
      getNextToken();
      int Size = -1;
      std::string SizeName = TakeSizeParamDim();
      if (SizeName.empty() && CurTok == TOK_INT_LITERAL) {
        // A best-effort (saturating) reading of the literal: an array size
        // is a small positive int in practice, no NumberExprAST node exists
        // for a type-position literal, and the exact-value representability
        // checks apply to literals used as VALUES, not to this compile-time
        // dimension. See ParseNumberExpr (ParserLiteral.cpp) for the
        // exact-value path a value-position literal takes.
        Size = static_cast<int>(strtoll(NumLitClean.c_str(), nullptr, NumLitRadix));
        if (Size <= 0) {
          SyntaxError(CurLoc, ErrCode::InvalidArraySize, "Array size must be positive",
                    "An array's size counts its slots: write a positive whole number, e.g. int[5].").raise();
          return nullptr;
        }
        getNextToken();
      }
      if (CurTok != ']') {
        SyntaxError(CurLoc, ErrCode::UnclosedDelimiter, "Expected ']' to close array size").raise();
        return nullptr;
      }
      getNextToken();
      GenArraySizes.push_back({Size, SizeName});
    }
    for (auto it = GenArraySizes.rbegin(); it != GenArraySizes.rend(); ++it) {
      GenBase = WrapArrayDim(GenBase, *it);
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

  return ParseTrailingArrayDims(std::move(Base));
}

// The `[N]` / `[]` suffixes after an element type: int[5], int[5][10], and
// int[] (an unsized slice parameter, size -1). Returns Base unchanged when
// none follow.
static TurfTypePtr ParseTrailingArrayDims(TurfTypePtr Base) {
  std::vector<std::pair<int, std::string>> ArraySizes;
  while (CurTok == '[') {
    getNextToken(); // eat '['
    int Size = -1;
    std::string SizeName = TakeSizeParamDim();
    if (SizeName.empty() && CurTok == TOK_INT_LITERAL) {
      // See the identical comment on the other array-size site above (this
      // file, `T[N]` in a generic base type) for why the best-effort
      // NumLitClean/NumLitRadix reading is the right choice here too.
      Size = static_cast<int>(strtoll(NumLitClean.c_str(), nullptr, NumLitRadix));
      if (Size <= 0) {
        SyntaxError(CurLoc, ErrCode::InvalidArraySize,
                    "Array size must be a positive integer (greater than 0)",
                    "An array's size counts its slots: write a positive whole number, e.g. int[5].")
            .raise();
        return nullptr;
      }
      getNextToken(); // eat the literal
    }
    if (CurTok != ']') {
      if (CurTok == TOK_IDENTIFIER)
        SyntaxError(CurLoc, ErrCode::InvalidArraySize,
                    "'" + IdentifierStr + "' is not a size: an array's size is a "
                    "positive whole number or a size parameter.",
                    "Write a number, e.g. int[5]. A generic function can take "
                    "the size as a parameter: 'fn<T, " + IdentifierStr +
                        ": size> f(a: T[" + IdentifierStr + "])'.")
            .raise();
      else
        SyntaxError(CurLoc, ErrCode::UnclosedDelimiter, "Expected ']' to close array size — e.g. mut arr: int[5];")
            .raise();
      return nullptr;
    }
    getNextToken(); // eat ']'
    ArraySizes.push_back({Size, SizeName});
  }
  for (auto it = ArraySizes.rbegin(); it != ArraySizes.rend(); ++it) {
    Base = WrapArrayDim(Base, *it);
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
 * assumes it never sees one there (`mut x: void;` and `fn f(x: void): void {}`
 * would hang the compiler). Callers use this predicate to reject the mistake
 * at parse time with a normal diagnostic.
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
    SyntaxError(CurLoc, ErrCode::UnclosedDelimiter,
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

    LogErrorAt(CurLoc, ErrCode::UnexpectedToken,
               "Unexpected token — expected a value, variable name, or "
                       "expression here."
                       "\n  Hint: Check for stray characters, a missing "
                       "operator, or an incomplete statement.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;

  case TOK_SIZEOF: {
    SourceLocation Loc = CurLoc;
    getNextToken(); // eat sizeof
    if (CurTok != '(') {
      LogErrorAt(CurLoc, ErrCode::MissingOpeningDelimiter, "Expected '(' after sizeof", SLM::ErrorCategory::SYNTAX_ERROR);
      return nullptr;
    }
    getNextToken(); // eat '('
    TurfTypePtr Ty = ParseType();
    if (!Ty)
      return nullptr;
    if (CurTok != ')') {
      LogErrorAt(CurLoc, ErrCode::UnclosedDelimiter, "Expected ')' to close sizeof", SLM::ErrorCategory::SYNTAX_ERROR);
      return nullptr;
    }
    getNextToken(); // eat ')'
    return std::make_unique<SizeOfExprAST>(Loc, Ty);
  }

  case TOK_CALLER_LOCATION: {
    // Bare keyword, no arguments -- see CallerLocationExprAST's own doc
    // comment (ASTExpr.h) for why this is a dedicated node rather than
    // falling through to the identifier/VariableExprAST path TOK_SELF shares
    // below. Legality (must be lexically inside a function declared
    // '@caller_location') is TypeChecker's job (visitCallerLocation()), not
    // checked here.
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

    // Struct brace-init (`Identifier '{' ... '}'`) is not valid syntax: a
    // struct is built with `StructName::new(...)`/`StructName::default()`.
    // Best-effort, not authoritative: if `IdName` is a struct type already
    // registered by this point in the (top-to-bottom, live) parse, give a
    // specific hint instead of letting this fall through to a confusing parse
    // error a few tokens later. A forward-declared struct misses this check,
    // which is fine: the ordinary parse failure that follows is still a real,
    // if less specific, diagnostic.
    if (CurTok == '{' && GlobalSymbolTable && !ParsingPatternHeader) {
      // A struct declared earlier in THIS module is registered under its
      // identity (`M::IdName`), not its bare spelling.
      if (auto T = GlobalSymbolTable->LookupType(
              TypeIdentity::make(CurrentParseModule(), IdName))) {
        if (dynamic_cast<TurfStructType *>(T.get())) {
          LogErrorAt(IdLoc, ErrCode::RemovedSyntax,
                     "'" + IdName + " { ... }' is no longer valid syntax "
                             "for constructing a struct."
                             "\n  Hint: use '" + IdName + "::new(...)' (if '" +
                             IdName + "' declares a constructor) or '" + IdName +
                             "::default()' instead.", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
      }
    }

    // ── LG-1: Name<Args>::member ─────────────────────────────────────────────
    // `List<int>::default()`: the type arguments of a generic type, written
    // where the type is used, in its type-position spelling. Taken only when
    // the list closes with `>` immediately followed by `::`
    // (TypeArgsThenScopeAhead()); otherwise this '<' is less-than.
    TurfTypePtr LeftType;
    if (CurTok == '<' && !ParsingPatternHeader && TypeArgsThenScopeAhead()) {
      std::vector<TurfTypePtr> TypeArgs;
      if (!ParseTypeArgList(TypeArgs))
        return nullptr;
      LeftType = std::make_shared<GenericType>(IdName, std::move(TypeArgs));
    }

    // LG-7: `identity<int>(3)`, a generic function's type arguments without
    // the `::`. A list naming a built-in type cannot be operands of `<` and
    // `>`, so this is never a comparison; the fix writes the `::`.
    if (!LeftType && CurTok == '<' && !ParsingPatternHeader) {
      bool SawTypeKeyword = false;
      if (TypeArgListFollower(&SawTypeKeyword) == '(' && SawTypeKeyword) {
        SyntaxError(CurLoc, ErrCode::ExpectedKeyword,
                    "Type arguments of a generic function are written after "
                    "'::': '" + IdName + "<' starts a comparison.",
                    "Write `" + IdName + "::<...>(...)`, e.g. `" + IdName +
                        "::<int>(x)`, or leave the type arguments out and let "
                        "the arguments decide them.")
            .withInsertAfterTokenFixIt(IdLoc, IdName, "::")
            .withFixReason("A generic function's type arguments follow '::' "
                           "(`f::<int>(x)`); without it '<' is less-than.")
            .raise();
        return nullptr;
      }
    }

    // ── LG-2: name::<T> ──────────────────────────────────────────────────────
    // `identity::<int>(3)`, `immut g = identity::<int>;`: type arguments for a
    // generic function. `::` followed by `<` begins nothing else.
    if (!LeftType && CurTok == TOK_SCOPE && peekToken(0) == '<') {
      getNextToken(); // eat '::'
      auto Var = std::make_unique<VariableExprAST>(IdLoc, IdName);
      if (!ParseTypeArgList(Var->TypeArgs))
        return nullptr;
      return Var;
    }

    // ── Scope access: Left::Right ────────────────────────────────────────────
    // Purely syntax-directed: the parser doesn't know whether `IdName` names
    // an enum, a module, or something else -- ScopeAccessKind disambiguation
    // happens entirely in the Resolver (visitScopeAccess / visitFuncCall).
    // Whatever follows this node syntactically (a postfix '(' for a call, a
    // bare reference, etc.) is handled by the postfix-expression machinery.
    if (CurTok == TOK_SCOPE) {
      getNextToken(); // eat '::'
      if (LeftType) {
        if (CurTok != TOK_IDENTIFIER) {
          LogErrorAt(CurLoc, ErrCode::ExpectedName, "Expected a name after '::'", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
        auto Node = std::make_unique<ScopeAccessExprAST>(IdLoc, IdName, IdLoc,
                                                         IdentifierStr, CurLoc);
        getNextToken(); // eat the member name
        Node->LeftType = std::move(LeftType);
        return Node;
      }
      if (CurTok != TOK_IDENTIFIER) {
        LogErrorAt(CurLoc, ErrCode::ExpectedName, "Expected a name after '::'", SLM::ErrorCategory::DECLARATION);
        return nullptr;
      }
      std::string RightName = IdentifierStr;
      SourceLocation RightLoc = CurLoc;
      getNextToken(); // eat right-hand name

      // If `IdName` is this file's spelling of a dependency module, every
      // consumer below (the ScopeAccessExprAST, including the 3-part
      // `Module::Enum::Variant` chain's qualifier) must see its identity.
      // Only imported module spellings are ever aliased, so an enum or struct
      // name passes through unchanged.
      IdName = ModuleKeyFor(IdName);

      // Same hint as the bare form above, for the qualified
      // `Module::StructName { ... }` spelling -- looked up by the identity
      // the qualifier names (`IdName::RightName` IS that identity when
      // IdName is the owning module).
      if (CurTok == '{' && GlobalSymbolTable && !ParsingPatternHeader) {
        if (auto T = GlobalSymbolTable->LookupType(
                TypeIdentity::make(IdName, RightName))) {
          if (dynamic_cast<TurfStructType *>(T.get())) {
            LogErrorAt(IdLoc, ErrCode::RemovedSyntax,
                       "'" + IdName + "::" + RightName + " { ... }' is no "
                               "longer valid syntax for constructing a struct."
                               "\n  Hint: use '" + IdName + "::" + RightName +
                               "::new(...)' or '" + IdName + "::" + RightName +
                               "::default()' instead.", SLM::ErrorCategory::DECLARATION);
            return nullptr;
          }
        }
      }

      // LG-1, qualified: `list::List<int>::default()`. Built like the
      // three-name chain below (`Module::Name::member`), with the type
      // arguments on the middle name.
      if (CurTok == '<' && !ParsingPatternHeader && TypeArgsThenScopeAhead()) {
        std::vector<TurfTypePtr> TypeArgs;
        if (!ParseTypeArgList(TypeArgs))
          return nullptr;
        getNextToken(); // eat '::' (TypeArgsThenScopeAhead() saw it)
        if (CurTok != TOK_IDENTIFIER) {
          LogErrorAt(CurLoc, ErrCode::ExpectedName, "Expected a name after '::'", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
        auto Node = std::make_unique<ScopeAccessExprAST>(
            IdLoc, RightName, RightLoc, IdentifierStr, CurLoc,
            /*ModuleQualifier=*/IdName);
        getNextToken(); // eat the member name
        Node->LeftType =
            std::make_shared<GenericType>(RightName, std::move(TypeArgs), IdName);
        return Node;
      }

      // LG-2, qualified: `module::f::<T>`.
      if (CurTok == TOK_SCOPE && peekToken(0) == '<') {
        getNextToken(); // eat '::'
        auto Node = std::make_unique<ScopeAccessExprAST>(IdLoc, IdName, IdLoc,
                                                         RightName, RightLoc);
        if (!ParseTypeArgList(Node->TypeArgs))
          return nullptr;
        return Node;
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
          LogErrorAt(CurLoc, ErrCode::ExpectedName, "Expected a variant name after '::'", SLM::ErrorCategory::DECLARATION);
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
      // '{' is deliberately NOT one of these triggers: `if x < d2 { ... }`
      // would misfire on 'd2' (an ordinary variable one edit away from the
      // keyword 'do'). Unlike an adjacent identifier/number/string, '{' is
      // not unambiguous evidence of a typo: this language has no parens
      // around if/while/for/compare conditions, so "some expression
      // immediately followed by '{'" is also the ordinary shape of every
      // condition's last token right before its block. A typo'd keyword
      // directly followed by '{' (e.g. 'els { ... }') still gets a real,
      // if less specific, diagnostic from whatever this identifier fails to
      // parse as further down.
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
        LogErrorAt(CurLoc, ErrCode::MissingComma, "Expected ',' or ']' in array literal", SLM::ErrorCategory::ARRAY_ERROR);
        return nullptr;
      }
    }
    if (CurTok != ']') {
      LogErrorAt(CurLoc, ErrCode::UnclosedDelimiter, "Expected ']' to close array literal", SLM::ErrorCategory::ARRAY_ERROR);
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
    LogErrorAt(KeywordLoc, ErrCode::UnexpectedToken,
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
    // A primitive type name is only ever valid after `mut x:` / `->`; here it
    // usually means a generic type argument written in EXPRESSION position in
    // a shape LG-1/LG-2 do not read (`List<int>.f()`, `x < int`) -- say how
    // type arguments are written instead of leaving the user to guess from
    // "begin a declaration".
    LogErrorAt(CurLoc, ErrCode::ExpectedKeyword,
               "Expected 'mutable' or 'immutable' to begin a declaration."
               "\n  Hint: if this is a generic type argument in an expression, "
               "a generic struct's go before '::' (`List<int>::default()`) and "
               "a generic function's after it (`identity::<int>(x)`).",
               SLM::ErrorCategory::DECLARATION);
    return nullptr;

  case TOK_TYPE_VOID:
    SyntaxError(CurLoc, ErrCode::VoidTypeNotAllowed, "'void' may only be used as a function return type")
        .raise();
    return nullptr;

  case TOK_FN:
    return ParseFuncDef();

  case TOK_OPERATOR:
    return ParseOperatorDecl();

  case TOK_AT:
    // `@caller_location` and the general `@name(args)` annotations share one
    // entry point; see ParseAnnotatedDecl() in ParserDecl.cpp.
    return ParseAnnotatedDecl();

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

  case TOK_PUB:
    return ParseVisibilityPrefixed();


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
    // as `2 ** (3 ** 2)` == 512, as in other languages with a `**`
    // operator, not left-associative like `+`/`*`).
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
      // Opcode is the plain operator this desugars to, in the same encoding
      // BinaryExprAST uses (raw ASCII for single-char ops, the token enum for
      // multi-char ones) so TypeChecker/Codegen's per-operator switches can
      // be shared instead of needing a second encoding.
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
    LogErrorAt(CurLoc, ErrCode::NestingTooDeep,
               "Expression nested too deeply to parse."
               "\n  Hint: check for unbalanced '(', '[', or '{'.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

  auto LHS = ParseUnary();
  if (!LHS)
    return nullptr;

  // A declaration is a whole statement, never the left operand of anything:
  // `fn f(): int { return 0; } % 5` must not read the `%` as an operator and
  // pull the NEXT declaration in as its right-hand side (`import test %`
  // would hide `main` and print an error from inside lib/std). ParseImport()
  // is the entry point for imports; this covers the declarations
  // ParseExpression() builds.
  if (dynamic_cast<FuncDefExprAST *>(LHS.get()) ||
      dynamic_cast<StructDeclExprAST *>(LHS.get()) ||
      dynamic_cast<EnumDeclExprAST *>(LHS.get()) ||
      dynamic_cast<ErrorDeclExprAST *>(LHS.get()) ||
      dynamic_cast<MachineDeclExprAST *>(LHS.get()) ||
      dynamic_cast<ExternFuncDeclExprAST *>(LHS.get()) ||
      dynamic_cast<ModuleImportExprAST *>(LHS.get()))
    return LHS;

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
      LogErrorAt(CurLoc, ErrCode::ExpectedKeyword, "Expected 'otherwise' after 'give' expression in ternary", SLM::ErrorCategory::CONTROL_FLOW);
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
  // position as the give/otherwise ternary just above), since `recover`
  // attaches directly to a call expression, not to a sub-term of a larger
  // arithmetic expression.
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
        LogErrorAt(CurLoc, ErrCode::ExpectedName,
                   "Expected an identifier after 'recover as' to "
                           "bind the failing error value."
                           "\n  Hint: `recover as err run { ... }`.", SLM::ErrorCategory::CONTROL_FLOW);
        return nullptr;
      }
      std::string BoundName = IdentifierStr;
      getNextToken(); // consume the bound name

      if (CurTok != TOK_RUN) {
        LogErrorAt(CurLoc, ErrCode::ExpectedKeyword,
                   "Expected 'run' after 'recover as " + BoundName +
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

    LogErrorAt(CurLoc, ErrCode::ExpectedKeyword,
               "Expected 'produce', 'run', or 'as' after 'recover'."
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
  // `~~~x`, `---x`) without going back through ParseExpression(), so it
  // needs its OWN recursion guard: otherwise a long run of `!` is that many
  // C++ stack frames deep, an uncatchable stack overflow. See
  // ParseRecursionGuard's doc comment in Lexer.h.
  ParseRecursionGuard RecursionGuard;
  if (RecursionGuard.tooDeep()) {
    LogErrorAt(CurLoc, ErrCode::NestingTooDeep,
               "Expression nested too deeply to parse."
               "\n  Hint: check for a runaway chain of unary operators.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

  // `try <expr>` -- error propagation (FREEZE.md §6.18). A prefix
  // operator with `!`/`-`'s precedence, so `try a() + try b()` is
  // `(try a()) + (try b())` and `try x.f(1)` propagates the whole call.
  if (CurTok == TOK_TRY) {
    SourceLocation Loc = CurLoc;
    getNextToken(); // consume 'try'
    auto Operand = ParseUnary();
    if (!Operand)
      return nullptr;
    ParsingFuncUsesTry = true; // see FuncBodyTryScope (ParserInternal.h)
    return std::make_unique<TryExprAST>(Loc, std::move(Operand));
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
  // Prefix ++/-- build the same PostfixExprAST node the postfix form uses
  // (see its doc comment, ASTExpr.h) with IsPrefix=true. The operand is
  // parsed via ParsePrimary() -- the primary+postfix-chain parse
  // (ParsePostfixExpression(), ParserLiteral.cpp) -- not restricted to a bare
  // identifier, so `++arr[0]`/`++obj.field` parse exactly like `arr[0]++`,
  // returning the NEW value instead of the old one. A non-lvalue operand
  // (`++(a + b)`) is not a parse error: TypeChecker::visitPostfixExpr()
  // (TypeCheckerExpr.cpp) rejects it with InvalidAssignmentTargetError via the
  // isAssignableLValue() check assignment and compound assignment use.
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
