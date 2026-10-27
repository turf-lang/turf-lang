/**
 * @file ParserStmt.cpp
 * @brief Control-flow and statement parsers: blocks, if/while/do-while/for, compare, let, return.
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

std::unique_ptr<ExprAST> ParseIfExpr() {
  SourceLocation KeywordLoc = Lex().CurLoc;
  getNextToken(); // eat 'if'

  std::unique_ptr<ExprAST> Cond = ParseExpression();
  if (!Cond)
    return nullptr;

  if (Parser().CurTok != '{') {
    LogErrorAt(Lex().CurLoc, ErrCode::MissingOpeningDelimiter, "Expected '{' after if-condition. Use 'give ... otherwise ...' for single-expression ternary.", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }

  auto Then = ParseBlock();
  if (!Then)
    return nullptr;

  std::vector<CondBranch> Branches;
  Branches.push_back({KeywordLoc, std::move(Cond), std::move(Then)});
  while (Parser().CurTok == TOK_ELSEIF) {
    SourceLocation ElseifLoc = Lex().CurLoc;
    getNextToken(); // eat 'elseif'

    std::unique_ptr<ExprAST> ElseifCond = ParseExpression();
    if (!ElseifCond)
      return nullptr;

    if (Parser().CurTok != '{') {
      LogErrorAt(ElseifLoc, ErrCode::MissingOpeningDelimiter,
                 "Expected '{' to open the elseif-block body."
                            "\n  Hint: 'elseif' blocks must always use braces: "
                            "elseif condition { ... }", SLM::ErrorCategory::CONTROL_FLOW);
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

  if (Parser().CurTok == TOK_ELSE) {
    ElseLoc = Lex().CurLoc;
    getNextToken(); // eat 'else'

    if (Parser().CurTok == '{') {
      ElseBody = ParseBlock();
    } else {
      ElseBody = ParseExpression();
    }
    if (!ElseBody)
      return nullptr;
  } else {
    if (Parser().CurTok == TOK_IDENTIFIER) {
      int thresh = std::max(1, (int)Lex().IdentifierStr.length() / 3);
      if (getDamerauLevenshteinDistance(Lex().IdentifierStr, "else") <= thresh ||
          getDamerauLevenshteinDistance(Lex().IdentifierStr, "elseif") <= thresh) {
        KeywordError(Lex().CurLoc, Lex().IdentifierStr).raise();
        return nullptr;
      }
    }
  }

  return std::make_unique<IfExprAST>(std::move(Branches), std::move(ElseBody),
                                     ElseLoc);
}





std::unique_ptr<ExprAST> ParseBlock() {
  ParseRecursionGuard RecursionGuard;
  if (RecursionGuard.tooDeep()) {
    LogErrorAt(Lex().CurLoc, ErrCode::NestingTooDeep,
               "Block nested too deeply to parse."
               "\n  Hint: check for unbalanced '{'.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

  if (Parser().CurTok != '{') {
    LogErrorAt(Lex().CurLoc, ErrCode::MissingOpeningDelimiter, "Expected '{' to open a block", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

  SourceLocation OpenBraceLoc = Lex().CurLoc;
  getNextToken();
  std::vector<std::unique_ptr<ExprAST>> Exprs;

  while (Parser().CurTok != '}' && Parser().CurTok != TOK_EOF) {
    if (Parser().CurTok == ';') {
      getNextToken();
      continue;
    }

    if (Parser().CurTok == TOK_MUTABLE || Parser().CurTok == TOK_IMMUTABLE) {
      auto Expr = ParseLetStatement(Parser().CurTok == TOK_IMMUTABLE);
      if (!Expr) return nullptr;
      Exprs.push_back(std::move(Expr));
      continue;
    }
    if (Parser().CurTok == TOK_EXPORT) {
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
    if (Parser().CurTok == TOK_EXTERN) {
      // LANG-004 item 3: `extern fn` may now appear inside a function body,
      // not just at file scope. Needs its own branch for the same reason
      // TOK_EXPORT does just above -- ParseExternDecl() already consumes
      // the declaration's trailing ';' itself, so falling through to the
      // generic ParseExpression() path below would hit its `if (CurTok !=
      // ';') error` check against whatever token comes AFTER the one
      // ParseExternDecl() already consumed, misreporting the failure at
      // the following statement instead of anywhere near the real
      // `extern fn` line.
      auto Expr = ParseExternDecl();
      if (!Expr) return nullptr;
      Exprs.push_back(std::move(Expr));
      continue;
    }
    if (Parser().CurTok == TOK_IF || Parser().CurTok == TOK_WHILE || Parser().CurTok == TOK_DO || Parser().CurTok == TOK_FOR ||
        Parser().CurTok == TOK_FN || Parser().CurTok == TOK_STRUCT || Parser().CurTok == TOK_ENUM || Parser().CurTok == TOK_MACHINE ||
        Parser().CurTok == TOK_COMPARE || Parser().CurTok == TOK_ERROR || Parser().CurTok == TOK_UNSAFE ||
        Parser().CurTok == TOK_PUB || Parser().CurTok == TOK_AT) {
      // TOK_AT: `@name(args)` before a nested `extern fn` (or `fn`). Like
      // `pub`, it ends in a declaration that has already consumed its own
      // ';' (an extern) or needs none (a fn), so no ';' is expected here.
      auto Expr = ParseExpression();
      if (!Expr) return nullptr;
      if (Parser().CurTok == ';') {
        LogErrorAt(Lex().CurLoc, ErrCode::UnexpectedToken, "Unexpected ';'", SLM::ErrorCategory::SYNTAX_ERROR);
        return nullptr;
      }
      Exprs.push_back(std::move(Expr));
      continue;
    }

    std::string PossibleKeyword = "";
    if (Parser().CurTok == TOK_IDENTIFIER) {
      PossibleKeyword = Lex().IdentifierStr;
    }

    auto Expr = ParseExpression();
    if (!Expr) return nullptr;
    if (Parser().CurTok != ';') {
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
      // The keyword hint above looks at the statement's first word, which is
      // any identifier within a typo's distance of a keyword (`io::printline(..)`
      // trips it for `if`/`do`), so it does not make "the ';' is missing" a
      // guess: a statement that really starts with a misspelled keyword is an
      // unresolved name once the ';' is in, and the sandbox rejects that.
      LogErrorAt(Lex().CurLoc, ErrCode::MissingSemicolon, "Expected ';' after statement." + Hint, SLM::ErrorCategory::SYNTAX_ERROR,
                 MissingSemicolonHints());
      return nullptr;
    }
    getNextToken(); // eat ';'
    Exprs.push_back(std::move(Expr));
  }

  if (Parser().CurTok != '}') {
    LogErrorAt(OpenBraceLoc, ErrCode::UnclosedDelimiter,
               "Missing closing '}' — every '{' must have a matching '}'."
               "\n  Hint: Add a '}' to close the block opened on line " +
                   std::to_string(OpenBraceLoc.Line) + ".", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }
  getNextToken();

  return std::make_unique<BlockExprAST>(std::move(Exprs));
}

std::unique_ptr<ExprAST> ParseWhileExpr() {
  SourceLocation KeywordLoc = Lex().CurLoc;
  getNextToken();

  std::unique_ptr<ExprAST> Cond = ParseExpression();
  if (!Cond) {
    return nullptr;
  }

  if (Parser().CurTok != '{') {
    LogErrorAt(
        KeywordLoc, ErrCode::MissingOpeningDelimiter,
        "Expected '{' to open the while-loop body."
        "\n  Hint: while loops must use braces: while condition { ... }", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }

  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  return std::make_unique<WhileExprAST>(KeywordLoc, std::move(Cond),
                                        std::move(Body));
}

// `unsafe { }` (issue #238) -- a thin wrapper exactly like ParseWhileExpr()
// above, minus the condition: consume the keyword, parse the body with the
// ordinary ParseBlock() (which already supports nested struct/fn/enum/
// machine declarations, not just statements -- see the block-decl branch
// further down in this function), wrap the result. Reachable from three
// call sites: ParsePrimary()'s generic dispatch (ParserCore.cpp), this
// file's own ParseBlock() statement-keyword list (nested/statement-level
// unsafe), and ParseFile()'s top-level loop (ParserModule.cpp), which
// calls this directly and then FLATTENS the result -- see that call site's
// own comment for why a top-level unsafe-wrapped struct/fn decl can't stay
// wrapped in a real nested AST node.
// The part of ParseUnsafeExpr() that runs AFTER 'unsafe' has already been
// consumed -- split out so ParseLeadingUnsafeModuleDecl() (ParserModule.cpp,
// LANG-002) can share it. That function has to consume 'unsafe' itself (this
// parser only ever has one token of lookahead, so telling `unsafe module;`
// apart from `unsafe { ... }` requires looking PAST 'unsafe' first) and,
// once it determines this ISN'T the `unsafe module;` capability pragma,
// needs to finish parsing an ordinary block from exactly this point without
// re-consuming 'unsafe' a second time.
std::unique_ptr<ExprAST> ParseUnsafeBlockBody(SourceLocation KeywordLoc) {
  if (Parser().CurTok != '{') {
    LogErrorAt(KeywordLoc, ErrCode::MissingOpeningDelimiter,
               "Expected '{' to open an unsafe block."
               "\n  Hint: unsafe blocks must use braces: unsafe { ... }", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  return std::make_unique<UnsafeBlockExprAST>(KeywordLoc, std::move(Body));
}

std::unique_ptr<ExprAST> ParseUnsafeExpr() {
  SourceLocation KeywordLoc = Lex().CurLoc;
  getNextToken(); // consume 'unsafe'
  return ParseUnsafeBlockBody(KeywordLoc);
}

// ParseDoWhileExpr - parses:
//   do { body } while cond;
// Parens around `cond` are optional, same as if/while/elseif -- only needed
// for grouping in complex conditions.
// Unlike while/if/for, the body always runs at least once: the condition is
// only checked after the first pass through the body.
std::unique_ptr<ExprAST> ParseDoWhileExpr() {
  SourceLocation KeywordLoc = Lex().CurLoc;
  getNextToken(); // eat 'do'

  if (Parser().CurTok != '{') {
    LogErrorAt(
        KeywordLoc, ErrCode::MissingOpeningDelimiter,
        "Expected '{' to open the do-block body."
        "\n  Hint: do-while loops must use braces: do { ... } while cond;", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }

  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  if (Parser().CurTok != TOK_WHILE) {
    LogErrorAt(Lex().CurLoc, ErrCode::ExpectedKeyword, "Expected 'while' after do-block\n  Hint: A do-block ends with its condition: `do { ... } while (cond);`.", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }
  getNextToken(); // eat 'while'

  // Parens around the condition are optional, same as if/while/elseif:
  // ParseExpression() already handles a leading '(' as grouping via
  // ParseParenExpr(), so no explicit '(' / ')' handling is needed here.
  std::unique_ptr<ExprAST> Cond = ParseExpression();
  if (!Cond)
    return nullptr;

  if (Parser().CurTok != ';') {
    LogErrorAt(Lex().CurLoc, ErrCode::MissingSemicolon, "Expected ';' after do-while loop.", SLM::ErrorCategory::CONTROL_FLOW, MissingSemicolonHints());
    return nullptr;
  }
  getNextToken(); // eat ';'

  return std::make_unique<DoWhileExprAST>(KeywordLoc, std::move(Body),
                                          std::move(Cond));
}

// ParseForExpr - parses three forms sharing a "for ... in <expr>" prefix:
//   for <ident> in <start>..<end> step <step_expr> { body }   (range-based for)
//   for <ident> in <iterable> { body }                        (for-each, single-var)
//   for (<index>, <value>) in <iterable> { body }              (for-each, paired)
// The prefix (loop variable(s) + 'in' + the first expression) is genuinely
// shared -- which continuation follows (TOK_RANGE vs '{') is only knowable
// after parsing that first expression, so all three forms live in one
// function rather than being split at TOK_FOR dispatch time.
std::unique_ptr<ExprAST> ParseForExpr() {
  SourceLocation ForLoc = Lex().CurLoc;
  getNextToken();

  std::string VarName;      // range-for's induction var, or for-each's value var
  std::string IndexVarName; // for-each paired form only; empty otherwise
  bool IsPairForm = false;

  if (Parser().CurTok == '(') {
    IsPairForm = true;
    getNextToken(); // eat '('

    if (Parser().CurTok != TOK_IDENTIFIER) {
      LogErrorAt(Lex().CurLoc, ErrCode::ExpectedName,
                 "Expected an index variable name after '(' in a for-each loop."
                         "\n  Hint: Correct syntax: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    IndexVarName = Lex().IdentifierStr;
    getNextToken();

    if (Parser().CurTok != ',') {
      LogErrorAt(Lex().CurLoc, ErrCode::MissingComma,
                 "Expected ',' between the index and value variable names."
                         "\n  Hint: Correct syntax: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    getNextToken(); // eat ','

    if (Parser().CurTok != TOK_IDENTIFIER) {
      LogErrorAt(Lex().CurLoc, ErrCode::ExpectedName,
                 "Expected a value variable name after ',' in a for-each loop."
                         "\n  Hint: Correct syntax: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    VarName = Lex().IdentifierStr;
    getNextToken();

    if (Parser().CurTok != ')') {
      LogErrorAt(Lex().CurLoc, ErrCode::UnclosedDelimiter,
                 "Expected ')' to close the '(index, value)' pair."
                         "\n  Hint: Correct syntax: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    getNextToken(); // eat ')'
  } else if (Parser().CurTok == TOK_IDENTIFIER) {
    VarName = Lex().IdentifierStr;
    getNextToken();
  } else {
    LogErrorAt(Lex().CurLoc, ErrCode::ExpectedName,
               "Expected a loop variable name (or '(index, value)') after 'for'."
                       "\n  Hint: for i in 0..10 step i += 1 { }"
                       "\n    or: for value in numbers { }"
                       "\n    or: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }

  if (Parser().CurTok != TOK_IN) {
    LogErrorAt(ForLoc, ErrCode::ExpectedKeyword,
               "Expected 'in' after the loop variable in a for-loop."
                       "\n  Hint: Correct syntax: for " +
                           VarName + " in 0..10 step " + VarName + "++ { }", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }
  getNextToken();

  std::unique_ptr<ExprAST> Start = ParseExpression();
  if (!Start)
    return nullptr;

  // Range-based for: `for i in 0..10 step i += 1 { }`
  if (Parser().CurTok == TOK_RANGE) {
    if (IsPairForm) {
      LogErrorAt(ForLoc, ErrCode::InvalidLoopForm,
                 "The '(index, value)' form isn't supported for a range-based for-loop."
                 "\n  Hint: Range-based for only binds a single scalar variable: "
                 "for i in 0..10 step i += 1 { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }

    getNextToken(); // eat '..'

    std::unique_ptr<ExprAST> End = ParseExpression();
    if (!End)
      return nullptr;

    // `step` is optional (LANG-004 item 1): omitting it means `step <var>++`,
    // synthesized here rather than left as a Codegen-level special case, so
    // every later pass (Resolver/TypeChecker/Codegen) sees the exact same
    // AST shape an explicit `step i++` would have produced.
    std::unique_ptr<ExprAST> Step;
    if (Parser().CurTok == TOK_STEP) {
      getNextToken();

      Step = ParseExpression();
      if (!Step)
        return nullptr;
    } else {
      Step = std::make_unique<PostfixExprAST>(
          ForLoc, '+', std::make_unique<VariableExprAST>(ForLoc, VarName));
    }

    if (Parser().CurTok != '{') {
      LogErrorAt(ForLoc, ErrCode::MissingOpeningDelimiter,
                 "Expected '{' to open the for-loop body."
                         "\n  Hint: Correct syntax: for " +
                             VarName + " in 0..10 step " + VarName + "++ { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }

    auto Body = ParseBlock();
    if (!Body)
      return nullptr;

    return std::make_unique<ForExprAST>(ForLoc, VarName, std::move(Start),
                                        std::move(End), std::move(Step),
                                        std::move(Body));
  }

  // For-each: `for value in numbers { }`  /  `for (index, value) in numbers { }`
  if (Parser().CurTok != '{') {
    LogErrorAt(ForLoc, ErrCode::ExpectedKeyword,
               "Expected '..' (range-based for) or '{' (for-each loop) here."
                       "\n  Hint: for i in 0..10 step i += 1 { }"
                       "\n    or: for value in numbers { }"
                       "\n    or: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }

  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  return std::make_unique<ForEachExprAST>(ForLoc, VarName, IndexVarName,
                                          std::move(Start), std::move(Body));
}

// ─── Patterns (LEC-003, FREEZE.md §6.17) ─────────────────────────────────────
//
//   pattern := '_'                                   wildcard
//            | Name [ '::' Name ] '{' fields '}'     struct pattern
//            | Name '::' Name '(' subs ')'           error-variant pattern (LEC-004)
//            | <any expression>                      value pattern
//   (nested only: a bare identifier is a BINDING)
//   fields  := [ field { ',' field } [ ',' ] ] [ '..' ]
//   field   := name                                  binds the field as `name`
//            | name ':' pattern
//   subs    := [ pattern { ',' pattern } [ ',' ] ] [ '..' ]   positional fields
//
// Nothing here needs lookahead beyond the current token, because a header's
// pattern is read by the ordinary expression parser first and only
// reinterpreted afterwards: `Point` parses as a variable and `geometry::Point`
// as a scope access, and it is the '{' that follows which says "this was a
// struct name". That is unambiguous -- struct brace-literal EXPRESSIONS were
// removed (C-3), so an expression is never followed by '{' in this position.
// ParserState::InPatternHeader keeps ParsePrimary() from mistaking that '{'
// for the removed literal.

namespace {

struct PatternHeaderScope {
  bool Saved;
  PatternHeaderScope() : Saved(Parser().InPatternHeader) { Parser().InPatternHeader = true; }
  ~PatternHeaderScope() { Parser().InPatternHeader = Saved; }
};

std::unique_ptr<ExprAST> ParsePattern(bool Nested);

// Parses `( sub, sub, .. )` after an error variant's name was read -- the
// `Type::Variant` in `matches IOError::Denied(path, _, ..):` (LEC-004,
// FREEZE.md §6.18). Sub-patterns are exactly what a struct pattern's fields
// take, and read positionally. CurTok is the '('.
std::unique_ptr<ExprAST> ParseErrorVariantPatternBody(std::unique_ptr<ExprAST> Head) {
  const SourceLocation Loc = static_cast<ScopeAccessExprAST *>(Head.get())->getLoc();
  getNextToken(); // eat '('

  std::vector<std::unique_ptr<ExprAST>> Subs;
  bool HasRest = false;
  SourceLocation RestLoc{0, 0};

  while (Parser().CurTok != ')') {
    if (Parser().CurTok == TOK_EOF) {
      LogErrorAt(Loc, ErrCode::UnclosedDelimiter, "Missing closing ')' for error pattern.",
                 SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }

    if (Parser().CurTok == TOK_RANGE) {
      HasRest = true;
      RestLoc = Lex().CurLoc;
      getNextToken(); // eat '..'
      if (Parser().CurTok == ',')
        getNextToken(); // a trailing comma after '..' is harmless
      if (Parser().CurTok != ')') {
        InvalidPatternError(Lex().CurLoc,
                            "'..' must be the last thing in an error pattern.",
                            "Move '..' to the end: `IOError::Denied(path, ..)`. "
                            "It stands for every field you did not list.")
            .raise();
        return nullptr;
      }
      break;
    }

    auto Sub = ParsePattern(/*Nested=*/true);
    if (!Sub)
      return nullptr;
    Subs.push_back(std::move(Sub));

    if (Parser().CurTok == ',') {
      getNextToken();
      continue;
    }
    if (Parser().CurTok != ')') {
      InvalidPatternError(Lex().CurLoc, "expected ',' or ')' after a field pattern.",
                          "Separate a variant's fields with commas: "
                          "`IOError::Denied(path, mode)`.")
          .raise();
      return nullptr;
    }
  }
  getNextToken(); // eat ')'

  return std::make_unique<ErrorVariantPatternExprAST>(
      Loc, std::move(Head), std::move(Subs), /*HasParens=*/true, HasRest, RestLoc);
}

// Parses `{ field, field: pattern, .. }` after the struct's name was read.
// CurTok is the '{'.
std::unique_ptr<ExprAST> ParseStructPatternBody(SourceLocation Loc,
                                                TurfTypePtr TypeSpelling) {
  getNextToken(); // eat '{'

  std::vector<FieldPattern> Fields;
  bool HasRest = false;
  SourceLocation RestLoc{0, 0};

  while (Parser().CurTok != '}') {
    if (Parser().CurTok == TOK_EOF) {
      LogErrorAt(Loc, ErrCode::UnclosedDelimiter, "Missing closing '}' for struct pattern.",
                 SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }

    if (Parser().CurTok == TOK_RANGE) {
      HasRest = true;
      RestLoc = Lex().CurLoc;
      getNextToken(); // eat '..'
      if (Parser().CurTok == ',')
        getNextToken(); // a trailing comma after '..' is harmless
      if (Parser().CurTok != '}') {
        InvalidPatternError(Lex().CurLoc,
                            "'..' must be the last thing in a struct pattern.",
                            "Move '..' to the end: `Point { x, .. }`. It "
                            "stands for every field you did not list.")
            .raise();
        return nullptr;
      }
      break;
    }

    if (Parser().CurTok != TOK_IDENTIFIER) {
      InvalidPatternError(
          Lex().CurLoc, "expected a field name in a struct pattern.",
          "List the struct's fields by name: `Point { x, y }` binds both, "
          "`Point { x: 0, y }` tests x and binds y, and `..` ignores the rest.")
          .raise();
      return nullptr;
    }

    FieldPattern F;
    F.Name = Lex().IdentifierStr;
    F.Loc = Lex().CurLoc;
    getNextToken(); // eat the field name

    if (Parser().CurTok == TOK_COLON) {
      getNextToken(); // eat ':'
      F.Sub = ParsePattern(/*Nested=*/true);
      if (!F.Sub)
        return nullptr;
    } else {
      F.IsShorthand = true;
      F.Sub = std::make_unique<BindingPatternExprAST>(F.Loc, F.Name);
    }
    Fields.push_back(std::move(F));

    if (Parser().CurTok == ',') {
      getNextToken();
      continue;
    }
    if (Parser().CurTok != '}') {
      InvalidPatternError(Lex().CurLoc,
                          "expected ',' or '}' after a field pattern.",
                          "Separate fields with commas: `Point { x, y }`.")
          .raise();
      return nullptr;
    }
  }
  getNextToken(); // eat '}'

  return std::make_unique<StructPatternExprAST>(Loc, std::move(TypeSpelling),
                                                std::move(Fields), HasRest,
                                                RestLoc);
}

// Nested == true inside a struct pattern's field list, where a bare
// identifier BINDS. At the top of a `matches` header it stays an ordinary
// value expression (equality against a variable), which is what keeps every
// pre-LEC-003 program meaning what it meant.
std::unique_ptr<ExprAST> ParsePattern(bool Nested) {
  if (Parser().CurTok == TOK_WILDCARD) {
    SourceLocation Loc = Lex().CurLoc;
    getNextToken(); // eat '_'
    return std::make_unique<WildcardPatternExprAST>(Loc);
  }

  std::unique_ptr<ExprAST> E;
  {
    PatternHeaderScope Guard;
    // Arms ParsePostfixExpression() to stop at the '(' of `Type::Variant(`.
    // Cleared again here, not only by the callee: a parse that fails before
    // reaching a primary must not leave it armed for the next expression.
    Parser().PatternHeadCallPending = true;
    E = ParseExpression();
    Parser().PatternHeadCallPending = false;
  }
  if (!E)
    return nullptr;

  // `Type::Variant(...)`: an error-variant pattern. Whether `Type` really is
  // an error type is the Resolver's to say (the parser never knows); when it
  // is not, the Resolver turns this back into the call it always was.
  if (Parser().CurTok == '(' && dynamic_cast<ScopeAccessExprAST *>(E.get()))
    return ParseErrorVariantPatternBody(std::move(E));

  if (Parser().CurTok == '{') {
    // `Name {` or `module::Name {`: a struct pattern. Recover the spelling
    // from the node ParseExpression() built for it.
    std::string Module, Name;
    SourceLocation Loc{0, 0};
    if (auto *V = dynamic_cast<VariableExprAST *>(E.get())) {
      Name = V->getName();
      Loc = V->getLoc();
    } else if (auto *S = dynamic_cast<ScopeAccessExprAST *>(E.get());
               S && S->getModuleQualifier().empty()) {
      Module = S->getLeftName();
      Name = S->getRightName();
      Loc = S->getLoc();
    } else {
      InvalidPatternError(
          Lex().CurLoc, "a '{' after a value is not a pattern.",
          "A struct pattern names the struct first: `Point { x, y }` or "
          "`geometry::Point { x, y }`.")
          .raise();
      return nullptr;
    }
    return ParseStructPatternBody(
        Loc, std::make_shared<NamedType>(std::move(Name), std::move(Module)));
  }

  if (Nested) {
    if (auto *V = dynamic_cast<VariableExprAST *>(E.get()))
      return std::make_unique<BindingPatternExprAST>(V->getLoc(), V->getName());
  }
  return E;
}

} // namespace

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
// A header's `val` may also be a destructuring pattern (see "Patterns"
// above) and may end in `if <guard>` just before its ':'.
//
// Rules enforced here (parse-time errors):
//   • Missing 'done' after arm body → error
//   • Body statements between stacked matches headers → error
//   • 'otherwise' before any remaining matches arm → error
//   • Duplicate 'otherwise' → error
//   • Empty compare{} → CompareWarning (not an error)
std::unique_ptr<ExprAST> ParseCompareExpr() {
  SourceLocation CompareLoc = Lex().CurLoc;
  getNextToken(); // eat 'compare'

  // ── Parse subject ────────────────────────────────────────────────────────
  // Optional parentheses around the subject (like if/while).
  bool HasParen = (Parser().CurTok == '(');
  if (HasParen)
    getNextToken(); // eat '('

  std::unique_ptr<ExprAST> Subject = ParseExpression();
  if (!Subject)
    return nullptr;

  if (HasParen) {
    if (Parser().CurTok != ')') {
      LogErrorAt(Lex().CurLoc, ErrCode::UnclosedDelimiter, "Expected ')' to close the compare subject", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    getNextToken(); // eat ')'
  }

  // ── Opening brace ────────────────────────────────────────────────────────
  if (Parser().CurTok != '{') {
    LogErrorAt(CompareLoc, ErrCode::MissingOpeningDelimiter, "Expected '{' to open the compare block.", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }
  getNextToken(); // eat '{'

  // ── Arm parsing loop ──────────────────────────────────────────────────────
  std::vector<MatchesArm> Arms;
  std::unique_ptr<ExprAST> OtherBody = nullptr;
  SourceLocation OtherLoc = {0, 0};
  bool HasOther = false;

  while (Parser().CurTok != '}' && Parser().CurTok != TOK_EOF) {
    // Skip stray semicolons between arms
    if (Parser().CurTok == ';') {
      getNextToken();
      continue;
    }

    // ── 'otherwise' arm (optional, must be last) ─────────────────────────
    if (Parser().CurTok == TOK_OTHERWISE) {
      if (HasOther) {
        CompareError(Lex().CurLoc, "Duplicate 'otherwise' arm in compare.")
            .withRelated(OtherLoc, "first 'otherwise' arm here")
            .raise();
        return nullptr;
      }
      HasOther = true;
      OtherLoc = Lex().CurLoc;
      getNextToken(); // eat 'otherwise'

      if (Parser().CurTok != TOK_COLON) {
        LogErrorAt(Lex().CurLoc, ErrCode::MissingColon, "Expected ':' after 'otherwise'.", SLM::ErrorCategory::CONTROL_FLOW);
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
      while (Parser().CurTok != '}' && Parser().CurTok != TOK_EOF &&
             Parser().CurTok != TOK_OTHERWISE && Parser().CurTok != TOK_MATCHES) {
        if (Parser().CurTok == ';') { getNextToken(); continue; }
        if (Parser().CurTok == TOK_MUTABLE || Parser().CurTok == TOK_IMMUTABLE) {
          auto E = ParseLetStatement(Parser().CurTok == TOK_IMMUTABLE);
          if (!E) return nullptr;
          OtherStmts.push_back(std::move(E));
          continue;
        }
        if (Parser().CurTok == TOK_EXPORT) {
          // See ParseBlock()'s matching TOK_EXPORT branch for why this
          // needs its own case instead of falling through to the generic
          // ParseExpression() path below.
          auto E = ParseExportDecl();
          if (!E) return nullptr;
          OtherStmts.push_back(std::move(E));
          continue;
        }
        if (Parser().CurTok == TOK_IF || Parser().CurTok == TOK_WHILE || Parser().CurTok == TOK_DO || Parser().CurTok == TOK_FOR ||
            Parser().CurTok == TOK_FN || Parser().CurTok == TOK_STRUCT || Parser().CurTok == TOK_ENUM || Parser().CurTok == TOK_MACHINE ||
            Parser().CurTok == TOK_COMPARE || Parser().CurTok == TOK_ERROR || Parser().CurTok == TOK_PUB) {
          auto E = ParseExpression();
          if (!E) return nullptr;
          OtherStmts.push_back(std::move(E));
          continue;
        }
        auto E = ParseExpression();
        if (!E) return nullptr;
        if (Parser().CurTok != ';') {
          LogErrorAt(Lex().CurLoc, ErrCode::MissingSemicolon, "Expected ';' after statement in 'otherwise' arm.", SLM::ErrorCategory::CONTROL_FLOW, MissingSemicolonHints());
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
      if (Parser().CurTok == TOK_OTHERWISE) {
        CompareError(Lex().CurLoc, "Duplicate 'otherwise' arm in compare.")
            .withRelated(OtherLoc, "first 'otherwise' arm here")
            .raise();
        return nullptr;
      }
      if (Parser().CurTok != '}') {
        // There's something after otherwise — likely another matches arm
        CompareError(Lex().CurLoc,
                     "'otherwise' must be the last arm in compare.")
            .raise();
        return nullptr;
      }
      break; // 'otherwise' consumed; exit arm loop
    }

    // ── 'matches' arm ────────────────────────────────────────────────────
    if (Parser().CurTok != TOK_MATCHES) {
      // Unexpected token inside compare block — could be a keyword typo
      LogErrorAt(Lex().CurLoc, ErrCode::ExpectedKeyword,
                 "Expected 'matches' or 'otherwise' inside compare block.", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }

    if (HasOther) {
      CompareError(Lex().CurLoc,
                   "'otherwise' must be the last arm in compare.")
          .raise();
      return nullptr;
    }

    SourceLocation ArmLoc = Lex().CurLoc;
    std::vector<std::unique_ptr<ExprAST>> Patterns;
    std::vector<std::unique_ptr<ExprAST>> Guards; // parallel to Patterns; nullptr = none

    // Collect all stacked 'matches val:' headers
    while (Parser().CurTok == TOK_MATCHES) {
      getNextToken(); // eat 'matches'

      // Optional parens around a plain value pattern. A destructuring
      // pattern is never parenthesised: its own '{' ... '}' already delimits.
      std::unique_ptr<ExprAST> Val;
      if (Parser().CurTok == '(') {
        getNextToken(); // eat '('
        Val = ParseExpression();
        if (!Val) return nullptr;
        if (Parser().CurTok != ')') {
          LogErrorAt(Lex().CurLoc, ErrCode::UnclosedDelimiter, "Expected ')' to close matches pattern.", SLM::ErrorCategory::CONTROL_FLOW);
          return nullptr;
        }
        getNextToken(); // eat ')'
      } else {
        Val = ParsePattern(/*Nested=*/false);
        if (!Val) return nullptr;
      }

      // Optional guard: `matches <pattern> if <condition>:`. It belongs to
      // THIS header (FREEZE.md §6.17), so `matches A if g: matches B:` runs
      // the shared body for (A and g) or B.
      std::unique_ptr<ExprAST> Guard;
      if (Parser().CurTok == TOK_IF) {
        getNextToken(); // eat 'if'
        Guard = ParseExpression();
        if (!Guard) return nullptr;
      }

      if (Parser().CurTok != TOK_COLON) {
        LogErrorAt(Lex().CurLoc, ErrCode::MissingColon, "Expected ':' after matches pattern.", SLM::ErrorCategory::CONTROL_FLOW);
        return nullptr;
      }
      getNextToken(); // eat ':'

      Patterns.push_back(std::move(Val));
      Guards.push_back(std::move(Guard));

      // If the very next non-semicolon token is not another 'matches' or 'done'
      // or 'otherwise' or '}', the user has put a body statement between
      // stacked headers — that is an error.
      while (Parser().CurTok == ';') getNextToken(); // skip stray semicolons

      if (Parser().CurTok != TOK_MATCHES && Parser().CurTok != TOK_DONE &&
          Parser().CurTok != TOK_OTHERWISE && Parser().CurTok != '}') {
        // Body appears before all matches headers are consumed — error only
        // when there are additional matches after the current one i.e. the
        // outer while would re-enter. We detect this by checking the next token.
        // Actually: if CurTok != TOK_MATCHES here, we're about to parse the
        // body, so this is fine (we just exit the inner while). No error.
        break; // exit inner while — next is body
      }

      if (Parser().CurTok == TOK_DONE || Parser().CurTok == TOK_OTHERWISE || Parser().CurTok == '}') {
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
    while (Parser().CurTok != TOK_DONE && Parser().CurTok != '}' && Parser().CurTok != TOK_EOF) {
      if (Parser().CurTok == ';') { getNextToken(); continue; }
      // Detect premature start of next arm (missing done)
      if (Parser().CurTok == TOK_MATCHES || Parser().CurTok == TOK_OTHERWISE) {
        CompareError(Lex().CurLoc,
                     "Expected 'done' after matches arm body.")
            .raise();
        return nullptr;
      }
      if (Parser().CurTok == TOK_MUTABLE || Parser().CurTok == TOK_IMMUTABLE) {
        auto E = ParseLetStatement(Parser().CurTok == TOK_IMMUTABLE);
        if (!E) return nullptr;
        ArmStmts.push_back(std::move(E));
        continue;
      }
      if (Parser().CurTok == TOK_EXPORT) {
        // See ParseBlock()'s matching TOK_EXPORT branch for why this needs
        // its own case instead of falling through to the generic
        // ParseExpression() path below.
        auto E = ParseExportDecl();
        if (!E) return nullptr;
        ArmStmts.push_back(std::move(E));
        continue;
      }
      if (Parser().CurTok == TOK_IF || Parser().CurTok == TOK_WHILE || Parser().CurTok == TOK_DO || Parser().CurTok == TOK_FOR ||
          Parser().CurTok == TOK_FN || Parser().CurTok == TOK_STRUCT || Parser().CurTok == TOK_ENUM || Parser().CurTok == TOK_MACHINE ||
          Parser().CurTok == TOK_COMPARE || Parser().CurTok == TOK_ERROR || Parser().CurTok == TOK_PUB) {
        auto E = ParseExpression();
        if (!E) return nullptr;
        ArmStmts.push_back(std::move(E));
        continue;
      }
      auto E = ParseExpression();
      if (!E) return nullptr;
      if (Parser().CurTok != ';') {
        LogErrorAt(Lex().CurLoc, ErrCode::MissingSemicolon, "Expected ';' after statement in matches arm body.", SLM::ErrorCategory::CONTROL_FLOW, MissingSemicolonHints());
        return nullptr;
      }
      getNextToken(); // eat ';'
      ArmStmts.push_back(std::move(E));
    }

    if (Parser().CurTok == TOK_DONE) {
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
    Arm.Guards = std::move(Guards);
    Arm.Body = std::make_unique<BlockExprAST>(std::move(ArmStmts));
    Arms.push_back(std::move(Arm));
  }

  // ── Closing brace ────────────────────────────────────────────────────────
  if (Parser().CurTok != '}') {
    LogErrorAt(CompareLoc, ErrCode::UnclosedDelimiter,
               "Missing closing '}' for compare block.", SLM::ErrorCategory::CONTROL_FLOW);
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

std::unique_ptr<ExprAST> ParseLetStatement(bool IsImmutable) {
  SourceLocation Loc = Lex().CurLoc;
  getNextToken(); // eat mutable or immutable

  if (Parser().CurTok != TOK_IDENTIFIER) {
    if (RejectReservedWordAsName())
      return nullptr;
    LogErrorAt(Lex().CurLoc, ErrCode::ExpectedName, "Expected a variable name after 'mutable' or 'immutable'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  std::string Name = Lex().IdentifierStr;
  SourceLocation NameLoc = Lex().CurLoc;
  getNextToken();

  // SEM-003 (§11.2, FREEZE.md §6.3): omitting the ': Type' annotation
  // entirely is defined as EXACTLY 'mut x: guess = 5;', not a separate
  // grammar rule -- both spellings must produce the identical AST (a
  // TurfGuess()-typed VarDeclExprAST) so TypeChecker::visitVarDecl never
  // needs to know which spelling was used.
  SourceLocation TypeLoc = Lex().CurLoc; // points at the type token itself, for a precise caret below
  TurfTypePtr Type;
  if (Parser().CurTok == TOK_COLON) {
    getNextToken();
    TypeLoc = Lex().CurLoc;
    // `guess` is a terminal type token, so setting the flag only when the type
    // STARTS with it keeps it out of any generic argument or pointee.
    const bool SavedAllowGuess = Parser().AllowGuessType;
    Parser().AllowGuessType = (Parser().CurTok == TOK_GUESS);
    Type = ParseType();
    Parser().AllowGuessType = SavedAllowGuess;
    if (!Type) return nullptr;
  } else {
    Type = TurfGuess();
  }

  // 'void' has no meaning as a variable's type (see isDisallowedVoidType's
  // doc comment for why this must be caught here rather than left to a
  // later pass -- it isn't a style nit, it's a compiler hang).
  if (isDisallowedVoidType(Type.get())) {
    LogErrorAt(TypeLoc, ErrCode::VoidTypeNotAllowed,
               "'void' cannot be a variable's type."
               "\n  Hint: 'void' only makes sense as a function's return "
               "type, e.g. fn f(): void { ... }. Give '" +
                   Name + "' a real type instead, like int, string, or a "
                   "struct name.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  if (Parser().CurTok != TOK_ASSIGN) {
    if (isArrayType(Type.get())) {
      const auto *AT = dynamic_cast<const ::ArrayType *>(Type.get());
      int ArraySize = AT ? AT->getSize() : -1;
      TurfTypePtr ElemType = AT ? AT->getElementTypePtr() : Type;

      if (Parser().CurTok != ';') {
        LogErrorAt(Lex().CurLoc, ErrCode::MissingSemicolon, "Expected ';' after array declaration.", SLM::ErrorCategory::DECLARATION, MissingSemicolonHints());
        return nullptr;
      }
      getNextToken(); // eat ';'
      auto Decl = std::make_unique<ArrayDeclExprAST>(NameLoc, Name, ElemType, ArraySize, std::vector<std::unique_ptr<ExprAST>>(), nullptr, IsImmutable);
      if (AT && AT->hasSymbolicSize())
        Decl->SizeParam = AT->getSizeParam();
      return Decl;
    }

    // SEM-004 (F-INIT-1, FREEZE.md §6.4): no synthesized default for ANY
    // type, uniformly. A declaration without an initializer leaves the
    // variable statically unassigned; DefiniteAssignmentPass (Analysis.cpp)
    // is what catches a read before it's assigned, for every type, not just
    // the primitives Mechanism A already covered. `guess` still has nothing
    // to infer from here regardless -- TypeChecker::visitVarDecl raises that
    // diagnostic ("'guess' needs an initializer to infer from.", SEM-003
    // §11.6) against this same null-Init declaration.
    std::unique_ptr<ExprAST> Init = nullptr;
    if (Parser().CurTok != ';') {
      LogErrorAt(Lex().CurLoc, ErrCode::MissingSemicolon, "Expected ';' after variable declaration.", SLM::ErrorCategory::DECLARATION, MissingSemicolonHints());
      return nullptr;
    }
    getNextToken(); // eat ';'
    return std::make_unique<VarDeclExprAST>(Loc, Name, NameLoc, Type, std::move(Init), IsImmutable, false, TypeLoc);
  }
  getNextToken(); // eat '='

  if (isArrayType(Type.get())) {
    const auto *AT = dynamic_cast<const ::ArrayType *>(Type.get());
    int ArraySize = AT ? AT->getSize() : -1;
    TurfTypePtr ElemType = AT ? AT->getElementTypePtr() : Type;

    std::vector<std::unique_ptr<ExprAST>> InitList;
    std::unique_ptr<ExprAST> InitExpr = nullptr;
    if (Parser().CurTok == '[') {
      getNextToken(); // eat '['

      if (Parser().CurTok != ']') {
        while (true) {
          auto Expr = ParseExpression();
          if (!Expr)
            return nullptr;
          InitList.push_back(std::move(Expr));

          if (Parser().CurTok == ']')
            break;
          if (Parser().CurTok != ',') {
            LogErrorAt(Lex().CurLoc, ErrCode::MissingComma, "Expected ',' or ']' in array initializer list", SLM::ErrorCategory::ARRAY_ERROR);
            return nullptr;
          }
          getNextToken(); // eat ','
        }
      }
      getNextToken(); // eat ']'

      // `mut nums: int[] = [10, 20, 30];` -- the literal gives the array its
      // size: this IS `int[3]`, an owned array with value semantics, exactly
      // as if the 3 had been written (and the type `guess` infers from the
      // same literal). `int[]` stays a view only when initialized from an
      // existing array (`= a`, `= a[1..]`), which has storage to share; a
      // literal has none until this declaration gives it some. Only the
      // outermost size is inferred -- an inner `[]` is still E140.
      if (ArraySize == -1) {
        if (InitList.empty()) {
          LogErrorAt(NameLoc, ErrCode::InvalidArraySize,
                     "An empty literal can't give '" + Name +
                         "' a size -- an array needs at least one element.\n  Hint: Give '" + Name + "' a size or a first element: `mut " + Name + ": int[3] = [0, 0, 0];` or `[1]`.",
                     SLM::ErrorCategory::ARRAY_ERROR);
          return nullptr;
        }
        ArraySize = static_cast<int>(InitList.size());
      }
    } else {
      InitExpr = ParseExpression();
      if (!InitExpr)
        return nullptr;
    }
    if (Parser().CurTok != ';') {
      LogErrorAt(Lex().CurLoc, ErrCode::MissingSemicolon, "Expected ';' after statement.", SLM::ErrorCategory::SYNTAX_ERROR, MissingSemicolonHints());
      return nullptr;
    }
    getNextToken(); // eat ';'
    auto Decl = std::make_unique<ArrayDeclExprAST>(NameLoc, Name, ElemType, ArraySize, std::move(InitList), std::move(InitExpr), IsImmutable);
    if (AT && AT->hasSymbolicSize())
      Decl->SizeParam = AT->getSizeParam();
    return Decl;
  }

  auto Init = ParseExpression();
  if (!Init) return nullptr;

  if (Parser().CurTok != ';') {
    LogErrorAt(Lex().CurLoc, ErrCode::MissingSemicolon, "Expected ';' after statement.", SLM::ErrorCategory::SYNTAX_ERROR, MissingSemicolonHints());
    return nullptr;
  }
  getNextToken(); // eat ';'
  return std::make_unique<VarDeclExprAST>(Loc, Name, NameLoc, Type, std::move(Init), IsImmutable, true, TypeLoc);
}



// ParseReturnExpr - called when CurTok == TOK_RETURN.
// Parses: return;   or   return <expr>;
std::unique_ptr<ExprAST> ParseReturnExpr() {
  SourceLocation RetLoc = Lex().CurLoc;
  getNextToken();

  if (Parser().CurTok == ';' || Parser().CurTok == '}' || Parser().CurTok == TOK_EOF) {
    return std::make_unique<ReturnExprAST>(RetLoc, nullptr);
  }
  auto Val = ParseExpression();
  if (!Val)
    return nullptr;
  return std::make_unique<ReturnExprAST>(RetLoc, std::move(Val));
}

// ParseDeferStmt - called when CurTok == TOK_DEFER.
// Parses: defer <statement>
// Dispatched from ParsePrimaryBase() at the same level as
// TOK_RETURN/TOK_BREAK/TOK_CONTINUE (LANG-003, ARCHITECTURE_OVERHAUL.md §8)
// -- deliberately NOT its own statement-loop special case in ParseBlock(),
// unlike TOK_MUTABLE/TOK_EXPORT/TOK_EXTERN just above. Those need their own
// branch because they consume their own trailing ';' internally; `defer`
// doesn't need to (its wrapped statement is a single ParseExpression() call,
// same shape return/break/continue already have), so it can ride the SAME
// generic ParseExpression()-then-';' fallback every ordinary statement uses.
// That, in turn, is what makes a stray top-level `defer ...;` get rejected
// by main.cpp's existing "statements are not allowed outside of a function"
// check with zero extra plumbing -- the same protection return/break/
// continue already get there.
std::unique_ptr<ExprAST> ParseDeferStmt() {
  SourceLocation KeywordLoc = Lex().CurLoc;
  getNextToken(); // eat 'defer'

  auto Stmt = ParseExpression();
  if (!Stmt)
    return nullptr;
  return std::make_unique<DeferExprAST>(KeywordLoc, std::move(Stmt));
}
