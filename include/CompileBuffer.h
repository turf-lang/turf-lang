/**
 * @file CompileBuffer.h
 * @brief A single, reusable "parse + resolve + type-check this buffer,
 *        best-effort, no codegen" library entry point for tooling.
 * @details Ties together several pieces that used to be private
 *          implementation details of main.cpp's own pipeline, each
 *          independently required and each a plausible source of subtle
 *          bugs for a new embedder (an LSP, a REPL, a playground) trying to
 *          reassemble them by hand -- see issue #233:
 *            - InitializeCompilerGlobals() (issue #231)
 *            - Per-statement parse recovery (issue #228)
 *            - In-memory buffer parsing, no temp file (issue #232)
 *            - Uncapped, full diagnostic collection for the buffer
 *          main.cpp's own pipeline does NOT call this -- its pre-pass/main-
 *          pass duality exists specifically to support real codegen's
 *          BasicBlock-ordering needs (see main.cpp's own comments on why),
 *          which this function deliberately has no reason to replicate
 *          since it never runs Codegen at all.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#ifndef COMPILE_BUFFER_H
#define COMPILE_BUFFER_H

#include "Lexer.h" // SourceLocation, DiagnosticEngine::Diag
#include "AST.h"

#include <memory>
#include <string>
#include <vector>

// Result of a best-effort parse+resolve+typecheck pass over an in-memory
// buffer.
struct CompileBufferResult {
  // The parsed program: one top-level statement per declaration that
  // parsed successfully. Always non-null, even when some statement(s)
  // failed to parse -- those are simply omitted (per-statement recovery,
  // issue #228), not the whole result. An LSP wanting hover/go-to-def/
  // completions for the rest of a file with one isolated syntax error gets
  // a real AST for everything else, not nullptr.
  std::unique_ptr<ExprAST> AST;

  // Every diagnostic recorded while parsing, resolving, and type-checking
  // this buffer, in the order recorded (not pre-sorted -- sort by
  // (Loc.Line, Loc.Col) yourself for presentation order, mirroring what
  // DiagnosticEngine::flushAll() does internally). Collected WITHOUT the
  // CLI's one-error-per-line cap (see DiagnosticEngine::
  // setCollectAllDiagnostics()'s doc comment) -- a second genuine PARSE
  // error on one physical line is here too, not silently dropped, thanks to
  // per-statement recovery (issue #228).
  //
  // Scope limit (issue #246, narrowed but not eliminated): resolveAll()/
  // checkAll() are called with RecoverPerDeclaration=true, so a resolver
  // or type error in one top-level declaration no longer prevents every
  // OTHER top-level declaration in the buffer from being resolved/checked
  // and having its own errors surfaced -- two independent undefined-
  // variable errors in two DIFFERENT functions are both in this list.
  // Still limited WITHIN a single top-level declaration's own body,
  // though: Pass 2's per-node recovery guards one whole top-level node at
  // a time (a whole function body, a whole struct decl, ...), not each
  // individual statement inside it, so the first resolver/type error
  // inside ONE function still stops resolution of the REST of that same
  // function's body -- a second independent error 3 lines further down in
  // the SAME function won't be in this list. Narrowing that further would
  // need per-STATEMENT (not per-declaration) resolver/type-checker
  // recovery, a bigger change to the Resolver/TypeChecker classes'
  // internal walk logic than this parameter attempts.
  std::vector<DiagnosticEngine::Diag> Diagnostics;

  // True only if parsing, resolving, AND type-checking all completed with
  // zero diagnostics. False does not mean AST is unusable -- see AST's own
  // doc comment above; check Diagnostics for what's wrong and where.
  bool Clean = false;
};

// Parses, resolves, and type-checks an in-memory buffer -- best-effort, no
// codegen. Safe to call repeatedly in the same process: every call saves
// and restores the global lexer/parser state it touches (SourceFilename/
// SourceLines/the parsed-top-level-AST cache/the module-import cache -- see
// issues #231/#232 and the stale-ImportedFiles-cache fix these build on),
// so back-to-back calls -- an LSP re-analyzing a live edit buffer on every
// keystroke is exactly the intended use -- never see stale state left over
// from a previous call, and never disturb any OTHER compile that happens
// to be mid-flight in the same process (e.g. Sandbox.cpp's own passes).
//
// Calls InitializeCompilerGlobals() internally every time (idempotent, see
// Parser.h) -- an embedder does not need to call it separately first.
//
// `Source` is the buffer's current text, read directly from memory -- never
// written to a temp file. `AsIfPath` is the filesystem path this buffer
// conceptually represents: NOT read from disk (Source's text is
// authoritative regardless of what's actually on disk at that path, if
// anything), but still used to (a) resolve relative `import`s the same way
// a real file's own path would (ParseModuleImportByName()'s search order,
// src/ParserModule.cpp), and (b) tag this buffer's own diagnostics
// correctly in the returned Diagnostics list (each Diag's Filename field) as
// distinct from any imported module's.
//
// A syntax error inside an IMPORTED module (not the buffer itself) still
// terminates the whole process via std::exit() rather than being reported
// as an ordinary diagnostic -- a deliberate, pre-existing tradeoff
// documented on ParseFile()'s RecoveryGuard (src/ParserModule.cpp) to avoid
// a longjmp-past-destructors heap corruption crash. This is a real,
// currently-unresolved limitation for a host process embedding this
// function directly (rather than shelling out to a subprocess per call) --
// tracked separately, not fixed by this function.
CompileBufferResult CompileBufferBestEffort(const std::string &Source,
                                             const std::string &AsIfPath);

// Returns every unqualified name visible in scope at (Line, Col) -- see
// issue #244 (no position-indexed scope-query API existed for completions/
// autocomplete tooling). Parses and resolves `Source` exactly like
// CompileBufferBestEffort() (per-statement recovery, no codegen), but
// instead of returning the whole AST, hooks Resolver's
// ScopeSnapshotCallback (Resolver.h) to find the LATEST AST node visited
// (in Pass-2 walk order) whose own start position is still at-or-before
// (Line, Col), and returns the names visible at exactly that point.
//
// This is a heuristic, not a precise lexical-scope query: "latest node in
// walk order" tracks source order for the common top-to-bottom case, but
// isn't a span-containment test (a node's END position isn't reliably
// known for every node kind yet -- see issue #243), and a handful of node
// kinds (NumberExprAST/BoolExprAST/CharExprAST/StringExprAST/BlockExprAST)
// carry no location at all and are silently skipped when picking the
// "latest" node. Good enough for "what's visible around line L" today;
// not a substitute for a real per-token span index if one is added later.
//
// Returns an empty vector if (Line, Col) is before any resolvable node
// (e.g. inside/before the very first statement) or if nothing in the
// buffer parsed far enough to reach Pass 2 at all.
std::vector<std::string> GetVisibleNamesAtPosition(const std::string &Source,
                                                    const std::string &AsIfPath,
                                                    int Line, int Col);

#endif
