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

std::unique_ptr<ExprAST> ParseIfExpr() {
  SourceLocation KeywordLoc = CurLoc;
  getNextToken(); // eat 'if'

  std::unique_ptr<ExprAST> Cond = ParseExpression();
  if (!Cond)
    return nullptr;

  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' after if-condition. Use 'give ... otherwise ...' for single-expression ternary.", SLM::ErrorCategory::CONTROL_FLOW);
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





std::unique_ptr<ExprAST> ParseBlock() {
  ParseRecursionGuard RecursionGuard;
  if (RecursionGuard.tooDeep()) {
    LogErrorAt(CurLoc,
               "Block nested too deeply to parse."
               "\n  Hint: check for unbalanced '{'.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }

  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' to open a block", SLM::ErrorCategory::SYNTAX_ERROR);
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
    if (CurTok == TOK_EXTERN) {
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
    if (CurTok == TOK_IF || CurTok == TOK_WHILE || CurTok == TOK_DO || CurTok == TOK_FOR ||
        CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM || CurTok == TOK_MACHINE ||
        CurTok == TOK_COMPARE || CurTok == TOK_ERROR || CurTok == TOK_UNSAFE ||
        CurTok == TOK_PUB) {
      auto Expr = ParseExpression();
      if (!Expr) return nullptr;
      if (CurTok == ';') {
        LogErrorAt(CurLoc, "Unexpected ';'", SLM::ErrorCategory::SYNTAX_ERROR);
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
      LogErrorAt(CurLoc, "Expected ';' after statement." + Hint, SLM::ErrorCategory::SYNTAX_ERROR);
      return nullptr;
    }
    getNextToken(); // eat ';'
    Exprs.push_back(std::move(Expr));
  }

  if (CurTok != '}') {
    LogErrorAt(OpenBraceLoc,
               "Missing closing '}' — every '{' must have a matching '}'."
               "\n  Hint: Check that all your blocks are properly closed.", SLM::ErrorCategory::SYNTAX_ERROR);
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
  if (CurTok != '{') {
    LogErrorAt(KeywordLoc,
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
  SourceLocation KeywordLoc = CurLoc;
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
  SourceLocation KeywordLoc = CurLoc;
  getNextToken(); // eat 'do'

  if (CurTok != '{') {
    LogErrorAt(
        KeywordLoc,
        "Expected '{' to open the do-block body."
        "\n  Hint: do-while loops must use braces: do { ... } while cond;", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }

  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  if (CurTok != TOK_WHILE) {
    LogErrorAt(CurLoc, "Expected 'while' after do-block", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }
  getNextToken(); // eat 'while'

  // Parens around the condition are optional, same as if/while/elseif:
  // ParseExpression() already handles a leading '(' as grouping via
  // ParseParenExpr(), so no explicit '(' / ')' handling is needed here.
  std::unique_ptr<ExprAST> Cond = ParseExpression();
  if (!Cond)
    return nullptr;

  if (CurTok != ';') {
    LogErrorAt(CurLoc, "Expected ';' after do-while loop.", SLM::ErrorCategory::CONTROL_FLOW);
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
  SourceLocation ForLoc = CurLoc;
  getNextToken();

  std::string VarName;      // range-for's induction var, or for-each's value var
  std::string IndexVarName; // for-each paired form only; empty otherwise
  bool IsPairForm = false;

  if (CurTok == '(') {
    IsPairForm = true;
    getNextToken(); // eat '('

    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected an index variable name after '(' in a for-each loop."
                         "\n  Hint: Correct syntax: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    IndexVarName = IdentifierStr;
    getNextToken();

    if (CurTok != ',') {
      LogErrorAt(CurLoc, "Expected ',' between the index and value variable names."
                         "\n  Hint: Correct syntax: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    getNextToken(); // eat ','

    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected a value variable name after ',' in a for-each loop."
                         "\n  Hint: Correct syntax: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    VarName = IdentifierStr;
    getNextToken();

    if (CurTok != ')') {
      LogErrorAt(CurLoc, "Expected ')' to close the '(index, value)' pair."
                         "\n  Hint: Correct syntax: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    getNextToken(); // eat ')'
  } else if (CurTok == TOK_IDENTIFIER) {
    VarName = IdentifierStr;
    getNextToken();
  } else {
    LogErrorAt(CurLoc, "Expected a loop variable name (or '(index, value)') after 'for'."
                       "\n  Hint: for i in 0..10 step i += 1 { }"
                       "\n    or: for value in numbers { }"
                       "\n    or: for (index, value) in numbers { }", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }

  if (CurTok != TOK_IN) {
    LogErrorAt(ForLoc, "Expected 'in' after the loop variable in a for-loop."
                       "\n  Hint: Correct syntax: for " +
                           VarName + " in 0..10 step " + VarName + "++ { }", SLM::ErrorCategory::CONTROL_FLOW);
    return nullptr;
  }
  getNextToken();

  std::unique_ptr<ExprAST> Start = ParseExpression();
  if (!Start)
    return nullptr;

  // Range-based for: `for i in 0..10 step i += 1 { }`
  if (CurTok == TOK_RANGE) {
    if (IsPairForm) {
      LogErrorAt(ForLoc,
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
    if (CurTok == TOK_STEP) {
      getNextToken();

      Step = ParseExpression();
      if (!Step)
        return nullptr;
    } else {
      Step = std::make_unique<PostfixExprAST>(
          ForLoc, '+', std::make_unique<VariableExprAST>(ForLoc, VarName));
    }

    if (CurTok != '{') {
      LogErrorAt(ForLoc, "Expected '{' to open the for-loop body."
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
  if (CurTok != '{') {
    LogErrorAt(ForLoc, "Expected '..' (range-based for) or '{' (for-each loop) here."
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
      LogErrorAt(CurLoc, "Expected ')' to close the compare subject", SLM::ErrorCategory::CONTROL_FLOW);
      return nullptr;
    }
    getNextToken(); // eat ')'
  }

  // ── Opening brace ────────────────────────────────────────────────────────
  if (CurTok != '{') {
    LogErrorAt(CompareLoc, "Expected '{' to open the compare block.", SLM::ErrorCategory::CONTROL_FLOW);
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
        CompareError(CurLoc, "Duplicate 'otherwise' arm in compare.")
            .withRelated(OtherLoc, "first 'otherwise' arm here")
            .raise();
        return nullptr;
      }
      HasOther = true;
      OtherLoc = CurLoc;
      getNextToken(); // eat 'otherwise'

      if (CurTok != TOK_COLON) {
        LogErrorAt(CurLoc, "Expected ':' after 'otherwise'.", SLM::ErrorCategory::CONTROL_FLOW);
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
        if (CurTok == TOK_IF || CurTok == TOK_WHILE || CurTok == TOK_DO || CurTok == TOK_FOR ||
            CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM || CurTok == TOK_MACHINE ||
            CurTok == TOK_COMPARE || CurTok == TOK_ERROR || CurTok == TOK_PUB) {
          auto E = ParseExpression();
          if (!E) return nullptr;
          OtherStmts.push_back(std::move(E));
          continue;
        }
        auto E = ParseExpression();
        if (!E) return nullptr;
        if (CurTok != ';') {
          LogErrorAt(CurLoc, "Expected ';' after statement in 'otherwise' arm.", SLM::ErrorCategory::CONTROL_FLOW);
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
        CompareError(CurLoc, "Duplicate 'otherwise' arm in compare.")
            .withRelated(OtherLoc, "first 'otherwise' arm here")
            .raise();
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
                 "Expected 'matches' or 'otherwise' inside compare block.", SLM::ErrorCategory::CONTROL_FLOW);
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
          LogErrorAt(CurLoc, "Expected ')' to close matches pattern.", SLM::ErrorCategory::CONTROL_FLOW);
          return nullptr;
        }
        getNextToken(); // eat ')'
      }

      if (CurTok != TOK_COLON) {
        LogErrorAt(CurLoc, "Expected ':' after matches pattern.", SLM::ErrorCategory::CONTROL_FLOW);
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
      if (CurTok == TOK_IF || CurTok == TOK_WHILE || CurTok == TOK_DO || CurTok == TOK_FOR ||
          CurTok == TOK_FN || CurTok == TOK_STRUCT || CurTok == TOK_ENUM || CurTok == TOK_MACHINE ||
          CurTok == TOK_COMPARE || CurTok == TOK_ERROR || CurTok == TOK_PUB) {
        auto E = ParseExpression();
        if (!E) return nullptr;
        ArmStmts.push_back(std::move(E));
        continue;
      }
      auto E = ParseExpression();
      if (!E) return nullptr;
      if (CurTok != ';') {
        LogErrorAt(CurLoc, "Expected ';' after statement in matches arm body.", SLM::ErrorCategory::CONTROL_FLOW);
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
  SourceLocation Loc = CurLoc;
  getNextToken(); // eat mutable or immutable

  if (CurTok != TOK_IDENTIFIER) {
    if (RejectVisibilityKeywordAsName())
      return nullptr;
    LogErrorAt(CurLoc, "Expected a variable name after 'mutable' or 'immutable'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  std::string Name = IdentifierStr;
  SourceLocation NameLoc = CurLoc;
  getNextToken();

  // SEM-003 (§11.2, FREEZE.md §6.3): omitting the ': Type' annotation
  // entirely is defined as EXACTLY 'mut x: guess = 5;', not a separate
  // grammar rule -- both spellings must produce the identical AST (a
  // TurfGuess()-typed VarDeclExprAST) so TypeChecker::visitVarDecl never
  // needs to know which spelling was used.
  SourceLocation TypeLoc = CurLoc; // points at the type token itself, for a precise caret below
  TurfTypePtr Type;
  if (CurTok == TOK_COLON) {
    getNextToken();
    TypeLoc = CurLoc;
    Type = ParseType();
    if (!Type) return nullptr;
  } else {
    Type = TurfGuess();
  }

  // 'void' has no meaning as a variable's type (see isDisallowedVoidType's
  // doc comment for why this must be caught here rather than left to a
  // later pass -- it isn't a style nit, it's a compiler hang).
  if (isDisallowedVoidType(Type.get())) {
    LogErrorAt(TypeLoc,
               "'void' cannot be a variable's type."
               "\n  Hint: 'void' only makes sense as a function's return "
               "type, e.g. fn f(): void { ... }. Give '" +
                   Name + "' a real type instead, like int, string, or a "
                   "struct name.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  if (CurTok != TOK_ASSIGN) {
    if (isArrayType(Type.get())) {
      const auto *AT = dynamic_cast<const ::ArrayType *>(Type.get());
      int ArraySize = AT ? AT->getSize() : -1;
      TurfTypePtr ElemType = AT ? AT->getElementTypePtr() : Type;

      if (CurTok != ';') {
        LogErrorAt(CurLoc, "Expected ';' after array declaration.", SLM::ErrorCategory::DECLARATION);
        return nullptr;
      }
      getNextToken(); // eat ';'
      return std::make_unique<ArrayDeclExprAST>(NameLoc, Name, ElemType, ArraySize, std::vector<std::unique_ptr<ExprAST>>(), nullptr, IsImmutable);
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
    if (CurTok != ';') {
      LogErrorAt(CurLoc, "Expected ';' after variable declaration.", SLM::ErrorCategory::DECLARATION);
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
            LogErrorAt(CurLoc, "Expected ',' or ']' in array initializer list", SLM::ErrorCategory::ARRAY_ERROR);
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
      LogErrorAt(CurLoc, "Expected ';' after statement.", SLM::ErrorCategory::SYNTAX_ERROR);
      return nullptr;
    }
    getNextToken(); // eat ';'
    return std::make_unique<ArrayDeclExprAST>(NameLoc, Name, ElemType, ArraySize, std::move(InitList), std::move(InitExpr), IsImmutable);
  }

  auto Init = ParseExpression();
  if (!Init) return nullptr;

  if (CurTok != ';') {
    LogErrorAt(CurLoc, "Expected ';' after statement.", SLM::ErrorCategory::SYNTAX_ERROR);
    return nullptr;
  }
  getNextToken(); // eat ';'
  return std::make_unique<VarDeclExprAST>(Loc, Name, NameLoc, Type, std::move(Init), IsImmutable, true, TypeLoc);
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
  SourceLocation KeywordLoc = CurLoc;
  getNextToken(); // eat 'defer'

  auto Stmt = ParseExpression();
  if (!Stmt)
    return nullptr;
  return std::make_unique<DeferExprAST>(KeywordLoc, std::move(Stmt));
}
