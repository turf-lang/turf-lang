/**
 * @file ResolverCall.cpp
 * @brief Call & member/module access resolution: function calls, method calls, field access, recover, scope access.
 * @details Part of the Resolver name-resolution pass (Issue #18), split from
 *          the original monolithic Resolver.cpp by category (mirrors
 *          AST.h's ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/architecture/compiler_pipeline.md).
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "Resolver.h"
#include "SymbolTable.h"

#include <algorithm> // std::find

#include "AST.h"
#include "Builtins.h"        // LookupBuiltin — for skipping bare intrinsics in visitFuncCall
#include "Errors.h"          // ReferenceError, DuplicateDeclarationError
#include "ErrorsFlow.h"      // SemanticError
#include "ResolverSymbolTable.h"
#include "ResolverInternal.h"
#include "SuggestionIndex.h" // SuggestionIndex::suggestVisible
#include "Types.h"           // RankedSuggestions

#include <memory>
#include <unordered_set>


void Resolver::visitFuncCall(FuncCallExprAST &Node) {
  // Resolve argument expressions first so any nested variable lookups happen
  // before we inspect the callee.
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());

  Node.ResolvedCandidates.clear();

  const auto *VarCallee = dynamic_cast<const VariableExprAST *>(Node.getCallee());
  // `Module::func(args)` -- a ScopeAccessExprAST callee explicitly naming
  // its module, as opposed to VarCallee's bare/self-reference case below.
  // Handled here, specially and narrowly, rather than via the generic
  // visitScopeAccess() dispatch -- exactly mirroring how VarCallee is
  // special-cased instead of going through visitVariable(). A ScopeAccessExprAST
  // that ISN'T in call position (e.g. used as a bare value) never reaches
  // this branch at all; it falls through to visitChild() below and is
  // handled by visitScopeAccess() instead, which reports it as invalid
  // (no first-class functions).
  auto *ScopeCallee = dynamic_cast<ScopeAccessExprAST *>(
      const_cast<ExprAST *>(Node.getCallee()));
  if (!VarCallee && !ScopeCallee) {
    visitChild(Node.getCallee());
    return;
  }

  std::string CalleeName;
  std::vector<const ResolverFunctionSymbol *> CurrentFileMatches;
  std::vector<ResolverSymbolTable::ModuleFunctionMatch> ModuleMatches;

  if (VarCallee) {
    CalleeName = VarCallee->getName();

    // `main` is compiled directly as the real C-ABI entry point (see
    // CodegenFunc.cpp's argv/argc plumbing) -- its LLVM FunctionType is
    // forced to (argc: i32, argv: ptr) regardless of Turf's always-empty
    // Params list. A Turf-level call `main()` supplies zero arguments,
    // which would desync from that 2-arg signature and previously reached
    // Codegen's `EvaluatedArgs[Idx]` indexing with an out-of-bounds Idx
    // (main is otherwise an ordinary, calls-just-fine function symbol, so
    // nothing else in the Resolver would have caught this). Reject it here
    // with a clear diagnostic instead of letting it crash the compiler.
    if (CalleeName == "main") {
      SemanticError(Node.getLoc(),
                     "'main' cannot be called directly -- it is the "
                     "program's entry point, not an ordinary function.")
          .raise();
      return;
    }

    // First-class functions: `f(3)` where `f` is a local variable/parameter
    // of fn(...):... type (holding a function VALUE), not the name of a
    // real top-level `fn`. Must be checked before the ordinary function-
    // overload lookup below -- a local variable and a top-level `fn` can
    // never share a name in the same scope anyway (SymTable_.declareVariable
    // enforces one flat name per scope), so this can never misfire against a
    // genuine direct call. When it matches, resolve the callee exactly like
    // an ordinary variable reference (ResolvedSymbol, not ResolvedCandidates/
    // ResolvedCallTarget) -- Codegen detects this shape from the callee's
    // own resolved symbol type and emits an indirect call instead of the
    // usual mangled-name lookup.
    if (auto VarSym = SymTable_.lookupVariable(CalleeName)) {
      if (VarSym->Type && VarSym->Type->isFunc()) {
        const_cast<VariableExprAST *>(VarCallee)->ResolvedSymbol = VarSym;
        return;
      }
    }

    CurrentFileMatches = SymTable_.lookupCurrentFileFunctions(CalleeName);

    // A bare (unqualified) call may only resolve to a function declared in
    // the SAME module currently being walked -- i.e. genuine self-
    // reference, like math.tr's abs() calling its own fabs() bare, or
    // io.tr's print() calling its own printf() bare. It must NOT resolve
    // to some OTHER transitively-imported module's function just because
    // that module happens to be loaded somewhere in the program.
    //
    // This used to be an unconditional, unscoped SymTable_.findInModules(...)
    // search across EVERY loaded module, with no check that the CURRENT
    // file itself imported (let alone flat-imported) anything at all --
    // confirmed live: bare `printline("x");` compiled successfully with
    // ZERO import statements anywhere in user code, because
    // lib/std/prelude.tr always `import io;`-ed for every compile and this
    // lookup didn't care who actually asked for that visibility.
    // prelude.tr is gone now (see its removal), but the underlying
    // promiscuous lookup was the real bug, not just prelude.tr's existence
    // -- fixed here by scoping the search to currentModuleName() (empty =
    // main file, which has no "self" module to search) instead of every
    // module ever loaded.
    const std::string &SelfModule = currentModuleName();
    if (!SelfModule.empty()) {
      for (const auto &Match : SymTable_.findInModules(CalleeName))
        if (Match.ModuleName == SelfModule)
          ModuleMatches.push_back(Match);
    }

    // Also allow a bare call to resolve via an explicit on-demand import
    // (`import module::CalleeName;` / `module::[..., CalleeName, ...]` /
    // `module::[*]`) made by THIS same context -- currentModuleName() is
    // exactly the right key (empty = main file), matching whatever
    // registerOnDemandImports() registered things under during hoisting.
    if (const auto *Flat = SymTable_.lookupFlatFunctionImport(SelfModule, CalleeName)) {
      for (const auto &Match : SymTable_.findInModules(CalleeName))
        if (Match.ModuleName == Flat->SourceModule)
          ModuleMatches.push_back(Match);
    }
  } else {
    // ScopeCallee: an explicit `Left::Right(args)` call.
    CalleeName = ScopeCallee->getRightName();
    const std::string &LeftName = ScopeCallee->getLeftName();

    if (SymTable_.isEnumName(LeftName)) {
      // e.g. `Color::Red(...)` -- a real enum, but variants aren't callable.
      ScopeCallee->ResolvedKind = ScopeAccessKind::EnumVariant;
      TypeError(Node.getLoc(), "'" + LeftName + "::" + CalleeName +
                                   "' is an enum variant, not a function -- "
                                   "it can't be called.",
                "Drop the '(...)' -- '" + LeftName + "::" + CalleeName +
                    "' is already a complete value on its own.",
                "Not callable!")
          .raise();
      return;
    }

    if (SymTable_.isErrorTypeName(LeftName)) {
      // e.g. `MathError::DivisionByZero(...)` -- a real error type, but
      // variants are bare, non-callable names, exactly like a plain `enum`
      // variant just above (docs/architecture/errors_and_recover.md -- payload
      // fields on a variant are deferred, see errors_and_recover.md §5.1).
      ScopeCallee->ResolvedKind = ScopeAccessKind::ErrorVariantRef;
      TypeError(Node.getLoc(), "'" + LeftName + "::" + CalleeName +
                                   "' is an error variant, not a function -- "
                                   "it can't be called.",
                "Drop the '(...)' -- '" + LeftName + "::" + CalleeName +
                    "' is already a complete value on its own.",
                "Not callable!")
          .raise();
      return;
    }

    if (SymTable_.isKnownStruct(LeftName)) {
      // `StructName::new(...)` / `StructName::default()` -- construction,
      // reserved and dispatched BEFORE the general static-method lookup
      // below (so a struct can never declare an ordinary static/instance
      // method literally named `new` or `default` that would otherwise
      // shadow this -- Resolver::visitFuncDef already rejects declaring
      // `default` at all and declaring `new` as `static`, so the only
      // `new`/`default` internal names that can ever exist are the ones
      // this dispatch expects).
      //
      // Unlike an ordinary static method call (existence-only, never
      // bare-visibility-gated -- see 2.7.1/2.8.1), CONSTRUCTING a struct is
      // treated the same as naming its TYPE: bare (unqualified)
      // construction respects the same isTypeVisibleUnqualified() gate a
      // `mut r: Rect = ...` type annotation already goes through
      // (validateNamedType(), visitVarDecl) -- this mirrors exactly what
      // the now-removed visitStructInit() enforced for the old brace-
      // literal form, so `Rect::new(...)`/`Rect::default()` still requires
      // `Rect` to be visible from this file/module, not merely to exist
      // somewhere in the program.
      if (CalleeName == "new" || CalleeName == "default") {
        if (ScopeCallee->getModuleQualifier().empty()) {
          if (!SymTable_.isTypeVisibleUnqualified(currentModuleName(), LeftName)) {
            UnqualifiedTypeNotVisibleError(Node.getLoc(), LeftName,
                                           SymTable_.getTypeOwningModule(LeftName))
                .raise();
            return;
          }
        } else if (!validateModuleQualifier(ScopeCallee->getModuleQualifier(),
                                            LeftName, Node.getLoc())) {
          return;
        }
      }

      if (CalleeName == "default") {
        ScopeCallee->ResolvedKind = ScopeAccessKind::DefaultConstruct;
        const int DefaultArgCount = static_cast<int>(Node.getArgs().size());
        if (DefaultArgCount != 0) {
          DefaultConstructorArityError(Node.getLoc(), LeftName, DefaultArgCount).raise();
        }
        return;
      }

      if (CalleeName == "new") {
        ScopeCallee->ResolvedKind = ScopeAccessKind::ConstructorCall;
        const int NewArgCount = static_cast<int>(Node.getArgs().size());
        const std::string OwningModule = SymTable_.getTypeOwningModule(LeftName);
        const std::string CtorKey = LeftName + ".new";

        std::vector<const ResolverFunctionSymbol *> RawCurrentFileMatches;
        std::vector<ResolverSymbolTable::ModuleFunctionMatch> RawModuleMatches;
        if (OwningModule.empty()) {
          RawCurrentFileMatches = SymTable_.lookupCurrentFileFunctions(CtorKey);
        } else {
          for (const auto &Match : SymTable_.findInModules(CtorKey))
            if (Match.ModuleName == OwningModule)
              RawModuleMatches.push_back(Match);
        }

        if (RawCurrentFileMatches.empty() && RawModuleMatches.empty()) {
          NoConstructorDefinedError(Node.getLoc(), LeftName).raise();
          return;
        }

        // Build a self-stripped copy of each overload (dropping Params[0],
        // always self -- 'new' can never be declared 'static', enforced by
        // visitFuncDef above) with ReturnType overridden to the struct's
        // own NamedType (its real declared return type is always 'void':
        // it mutates self in place, but the CALL EXPRESSION `StructName::
        // new(...)` itself evaluates to the constructed value). Pushing
        // these into ResolvedCandidates lets EVERY downstream consumer --
        // the shared arity/signature code just below, and TypeChecker's
        // own overload-narrowing in visitFuncCall -- treat this exactly
        // like an ordinary resolved call, entirely unmodified. Codegen
        // reconstructs the TRUE (self-included) mangled name from this
        // stripped candidate instead of needing the original preserved
        // anywhere (see FuncCallExprAST::codegen()).
        auto stripSelf = [&](const ResolverFunctionSymbol &Sym) {
          auto Copy = std::make_shared<ResolverFunctionSymbol>(Sym);
          if (!Copy->Params.empty())
            Copy->Params.erase(Copy->Params.begin());
          Copy->ReturnType = std::make_shared<NamedType>(LeftName);
          return Copy;
        };

        std::vector<std::shared_ptr<ResolverFunctionSymbol>> StrippedCandidates;
        for (const auto *Sym : RawCurrentFileMatches)
          StrippedCandidates.push_back(stripSelf(*Sym));
        for (const auto &Match : RawModuleMatches)
          StrippedCandidates.push_back(stripSelf(*Match.Symbol));

        bool AnyArityMatch = false;
        for (const auto &Cand : StrippedCandidates) {
          if (Cand->arity() == NewArgCount ||
              (Cand->isVarArg() && NewArgCount >= Cand->arity())) {
            AnyArityMatch = true;
            break;
          }
        }

        if (!AnyArityMatch) {
          std::vector<std::string> AllSignatures;
          for (const auto &Cand : StrippedCandidates)
            AllSignatures.push_back(Cand->signature());
          auto Err = WrongArityError(Node.getLoc(), LeftName + "::new",
                                     NewArgCount, AllSignatures);
          for (const auto *Sym : RawCurrentFileMatches)
            Err.withRelated(toSourceLocation(Sym->DeclLoc), "candidate");
          for (const auto &Match : RawModuleMatches)
            Err.withRelated(toSourceLocation(Match.Symbol->DeclLoc), "candidate");
          Err.raise();
          return;
        }

        for (const auto &Cand : StrippedCandidates) {
          if (Cand->arity() == NewArgCount ||
              (Cand->isVarArg() && NewArgCount >= Cand->arity()))
            Node.ResolvedCandidates.push_back(Cand);
        }
        if (Node.ResolvedCandidates.size() == 1)
          Node.ResolvedCallTarget = Node.ResolvedCandidates[0];
        return;
      }

      // `StructName::method(args)` -- a true STATIC method call: no
      // instance involved at all (unlike `instance.method(args)`, handled
      // entirely separately by visitMethodCall/MethodCallExprAST). A
      // static method is registered under the exact same "StructName.method"
      // internal name an INSTANCE method would be (see ParseFuncDef()'s
      // `::`-declaration branch), just without a `self` parameter -- so the
      // lookup here is byte-identical to the ModuleFunctionRef branch below,
      // just scoped to the struct's OWNING module (main file "" or a real
      // module name) instead of a qualifier that names a module directly.
      // Qualified access is never gated by bare-type visibility (2.7.1 in
      // the architecture doc) -- isKnownStruct() alone (existence) is the
      // only requirement, exactly like a qualified type annotation.
      ScopeCallee->ResolvedKind = ScopeAccessKind::StaticMethodRef;
      // `Module::StructName::method(...)` -- the parser reinterprets a
      // second consecutive '::' the same way it does for
      // Module::EnumName::Variant (see ParsePrimaryBase()'s TOK_SCOPE
      // branch), landing the leading module name in ModuleQualifier rather
      // than LeftName/RightName. Without this check, a WRONG qualifier
      // (`mem::Vec2::zero()` when Vec2 actually lives in a different
      // module) was silently ignored entirely -- LeftName/RightName alone
      // determine the lookup below, and ModuleQualifier was never
      // consulted, so any qualifier (right or wrong) produced the same
      // result. Confirmed live before this check existed.
      if (!validateModuleQualifier(ScopeCallee->getModuleQualifier(), LeftName,
                                   Node.getLoc()))
        return;
      const std::string OwningModule = SymTable_.getTypeOwningModule(LeftName);
      const std::string MethodKey = LeftName + "." + CalleeName;
      if (OwningModule.empty()) {
        CurrentFileMatches = SymTable_.lookupCurrentFileFunctions(MethodKey);
      } else {
        for (const auto &Match : SymTable_.findInModules(MethodKey))
          if (Match.ModuleName == OwningModule)
            ModuleMatches.push_back(Match);
      }
    } else if (!SymTable_.hasModule(LeftName)) {
      // Not an enum, struct, or module -- but if it's a real, declared
      // variable (an instance), give the specific "you meant '.'" error
      // instead of the generic "unknown qualifier" one. `p::get()` where
      // `p: SomeStruct` is a genuinely common mistake to make right after
      // learning static methods exist, and "isn't a known enum, struct,
      // or module" doesn't actually tell the user what they did wrong.
      if (SymTable_.lookupVariable(LeftName)) {
        ScopeOperatorOnInstanceError(Node.getLoc(), LeftName, CalleeName).raise();
        return;
      }
      RankedSuggestions QualifierSuggestions;
      auto QualifierSugg = SuggestionIndex::suggestVisible(
          LeftName, unknownQualifierSuggestionCandidates());
      for (const auto &S : QualifierSugg)
        QualifierSuggestions.Local.push_back(S + "::" + CalleeName);
      // ScopeCallee->getLeftLoc(), not Node.getLoc() -- the latter is the
      // whole call expression's location (lands on the call's '(' for a
      // `nosuchmodule::foo()`-shaped call), not the unresolved qualifier
      // token itself.
      UnknownScopeQualifierError(ScopeCallee->getLeftLoc(), LeftName,
                                 QualifierSuggestions)
          .raise();
      return;
    } else {
      ScopeCallee->ResolvedKind = ScopeAccessKind::ModuleFunctionRef;
      // A 3-part `ModuleQualifier::LeftName::RightName` chain only has a
      // defined meaning when LeftName is an enum or struct (Module owns
      // it -- validated above/below); modules aren't nested inside other
      // modules via '::' at all, so a qualifier here is always meaningless.
      // Without this check it was silently IGNORED entirely -- LeftName
      // alone determined the lookup, so `mem::io::printline(...)` compiled
      // identically to `io::printline(...)` regardless of what the leading
      // qualifier said. Confirmed live before this check existed.
      if (!ScopeCallee->getModuleQualifier().empty()) {
        TypeError(Node.getLoc(), "'" + ScopeCallee->getModuleQualifier() +
                                     "::" + LeftName +
                                     "' -- modules can't be qualified by "
                                     "another module; just write '" +
                                     LeftName + "::" + CalleeName + "(...)'.",
                  "Modules aren't nested inside each other -- drop the '" +
                      ScopeCallee->getModuleQualifier() + "::' prefix.",
                  "Invalid module qualifier!")
            .raise();
        return;
      }
      // Explicit qualification means the search is scoped to EXACTLY the
      // named module -- no self-reference concept applies here, unlike the
      // bare-call case above.
      for (const auto &Match : SymTable_.findInModules(CalleeName))
        if (Match.ModuleName == LeftName)
          ModuleMatches.push_back(Match);
    }
  }

  if (CurrentFileMatches.empty() && ModuleMatches.empty()) {
    // An explicit `Module::func(...)` call where Module IS real but doesn't
    // have `func` needs its own, much more specific error than the bare-call
    // path below -- that path's hints ("import the module that defines it")
    // are actively confusing once the user has ALREADY written the
    // qualifier. ModuleMethodNotFoundError existed in Errors.h for exactly
    // this shape (written for the old dot-based module-call syntax) but was
    // never actually wired up anywhere -- confirmed by grep, dead code until
    // now.
    if (ScopeCallee && ScopeCallee->ResolvedKind == ScopeAccessKind::ModuleFunctionRef) {
      const std::string &LeftName = ScopeCallee->getLeftName();
      RankedSuggestions Suggestions;
      Suggestions.Local = SuggestionIndex::suggestVisible(
          CalleeName, SymTable_.getModuleFunctionNames(LeftName));
      ModuleMethodNotFoundError(Node.getLoc(), LeftName, CalleeName, Suggestions).raise();
      return;
    }

    // Same idea as ModuleMethodNotFoundError above, for the struct case:
    // `StructName::method(...)` where StructName is real but has no
    // (static OR instance -- either would show up here, since both share
    // the "StructName.method" registration name) method by that name.
    // Reuses StructMethodNotFoundError, the exact class the INSTANCE
    // (`instance.method()`) not-found path already raises, for one
    // consistent message regardless of which call form the user wrote.
    if (ScopeCallee && ScopeCallee->ResolvedKind == ScopeAccessKind::StaticMethodRef) {
      const std::string &LeftName = ScopeCallee->getLeftName();
      const std::string Prefix = LeftName + ".";
      std::vector<std::string> AllNames = SymTable_.getAllCurrentFileFunctionNames();
      std::vector<std::string> ModuleNames = SymTable_.getAllModuleFunctionNames();
      AllNames.insert(AllNames.end(), ModuleNames.begin(), ModuleNames.end());
      std::vector<std::string> BareMethodNames;
      for (const auto &N : AllNames)
        if (N.size() > Prefix.size() && N.compare(0, Prefix.size(), Prefix) == 0)
          BareMethodNames.push_back(N.substr(Prefix.size()));
      RankedSuggestions Suggestions;
      Suggestions.Local = SuggestionIndex::suggestVisible(CalleeName, BareMethodNames);
      StructMethodNotFoundError(Node.getLoc(), LeftName, CalleeName, Suggestions).raise();
      return;
    }

    // Before emitting an error, check whether the callee is a true language-level
    // bare intrinsic — one that is callable WITHOUT a module qualifier AND has no
    // module home in the standard library.
    //
    // As of now these are the bare intrinsics:
    //   - typeof(x)             — compile-time type name, no module
    //   - gc_alloc(n)           — MEM-001, typed-allocation surface for
    //   - gc_alloc_zeroed(n)      turf_rt_alloc/_zeroed/turf_rt_realloc
    //   - gc_realloc(p, n)        (include/Intrinsics.h category 1)
    //
    // Everything else comes from lib/std:
    //   - print / printline / input  → io.tr   (call as io.printline(...))
    //   - malloc / realloc / free    → mem.tr   (call as mem.malloc(...))
    //   - abs / power / sqrt / …     → math.tr  (call as math.abs(...))
    //   - assert / fail / assert_eq  → test.tr  (call as test::assert(...))
    //   - length (was lengthof(x))   → strings.tr (call as strings::length(x),
    //     part of the prelude -- see STRING_UFCS_OPERATOR_CHAR_RFC.md §7)
    //
    // A module-qualified call (io.printline(...)) never reaches this
    // function at all -- it's a MethodCallExprAST, handled by
    // visitMethodCall instead. This function only ever sees bare,
    // unqualified names, so no stdlib function name belongs in the
    // BareIntrinsics set below no matter how "primitive" it feels.
    // assert/fail/lengthof used to be listed here too, back when they were
    // compiler intrinsics; they're now ordinary, real Turf functions in
    // lib/std, resolved through the exact same module-function path as
    // everything else in this comment.
    static const std::unordered_set<std::string> BareIntrinsics = {
        "typeof", "gc_alloc", "gc_alloc_zeroed", "gc_realloc",
    };
    if (BareIntrinsics.count(CalleeName)) {
      // True bare intrinsic — skip error, TypeChecker validates arity/types.
      return;
    }

    RankedSuggestions Suggestions;

    auto CurrentNames = SuggestionIndex::suggestVisible(
        CalleeName, SymTable_.getAllCurrentFileFunctionNames());
    for (const auto &SuggestedName : CurrentNames) {
      for (const auto *Sym : SymTable_.lookupCurrentFileFunctions(SuggestedName)) {
        Suggestions.Local.push_back(Sym->signature());
      }
    }

    // Exact-name matches in OTHER modules (checked before the fuzzy pass
    // below, which deliberately skips exact matches -- see
    // SuggestionIndex::suggestFromCandidates' `Candidate == Query` guard,
    // since it exists for typo-correction, not this case). This is now the
    // single most common way to land in this branch: a real, correctly-
    // spelled function that exists in some OTHER imported module, called
    // bare with no qualifier -- e.g. `printline(x)` when `io.printline(x)`
    // was meant. Surface it as a real suggestion instead of the generic
    // "double-check the spelling" fallback, which is actively wrong here
    // (the spelling IS correct; the qualifier is just missing).
    for (const auto &Match : SymTable_.findInModules(CalleeName))
      Suggestions.Module[Match.ModuleName].push_back(Match.Symbol->signature());

    auto ModuleNames = SuggestionIndex::suggestVisible(
        CalleeName, SymTable_.getAllModuleFunctionNames());
    for (const auto &SuggestedName : ModuleNames) {
      for (const auto &Match : SymTable_.findInModules(SuggestedName)) {
        Suggestions.Module[Match.ModuleName].push_back(Match.Symbol->signature());
      }
    }

    // Issue #8 fix: also search intrinsic/builtin names (assert, fail,
    // typeof) so typos like `asert()` produce a proper suggestion.
    auto BuiltinMatches = SuggestionIndex::suggestBuiltins(CalleeName);
    for (const auto &B : BuiltinMatches)
      Suggestions.Local.push_back(B + "(...)");

    UnknownFunctionError(Node.getLoc(), CalleeName, Suggestions).raise();
    return;
  }

  // ── Arity check ───────────────────────────────────────────────────────────
  // The symbol table already knows every overload's arity.  Before handing
  // the candidates to the TypeChecker, verify that at least one overload
  // accepts the number of arguments supplied at this call site.
  //
  // This is the authoritative arity check — no hardcoded maps anywhere.
  // WrongArityError fires here; Codegen never sees an arity mismatch.
  const int ArgCount = static_cast<int>(Node.getArgs().size());
  std::vector<std::string> AllSignatures;

  for (const auto *Sym : CurrentFileMatches)
    AllSignatures.push_back(Sym->signature());
  for (const auto &Match : ModuleMatches)
    AllSignatures.push_back(Match.Symbol->signature());

  bool AnyArityMatch = false;
  for (const auto *Sym : CurrentFileMatches) {
    // Vararg functions accept any number of args >= their declared param count
    if (Sym->arity() == ArgCount || (Sym->isVarArg() && ArgCount >= Sym->arity()))
    { AnyArityMatch = true; break; }
  }
  if (!AnyArityMatch) {
    for (const auto &Match : ModuleMatches) {
      if (Match.Symbol->arity() == ArgCount ||
          (Match.Symbol->isVarArg() && ArgCount >= Match.Symbol->arity()))
      { AnyArityMatch = true; break; }
    }
  }

  if (!AnyArityMatch) {
    auto Err = WrongArityError(Node.getLoc(), CalleeName, ArgCount, AllSignatures);
    for (const auto *Sym : CurrentFileMatches)
      Err.withRelated(toSourceLocation(Sym->DeclLoc), "candidate");
    for (const auto &Match : ModuleMatches)
      Err.withRelated(toSourceLocation(Match.Symbol->DeclLoc), "candidate");
    Err.raise();
    return;
  }

  // Populate ResolvedCandidates with only the arity-matching overloads.
  // TypeChecker::visitFuncCall then narrows further by type compatibility.
  for (const auto *Sym : CurrentFileMatches) {
    if (Sym->arity() == ArgCount || (Sym->isVarArg() && ArgCount >= Sym->arity()))
      Node.ResolvedCandidates.push_back(
          std::make_shared<ResolverFunctionSymbol>(*Sym));
  }
  for (const auto &Match : ModuleMatches) {
    if (Match.Symbol->arity() == ArgCount ||
        (Match.Symbol->isVarArg() && ArgCount >= Match.Symbol->arity()))
      Node.ResolvedCandidates.push_back(
          std::make_shared<ResolverFunctionSymbol>(*Match.Symbol));
  }

  // ── Set ResolvedCallTarget when unambiguous ───────────────────────────────
  // If exactly one overload matches the arity, the Resolver can commit to it
  // immediately.  Codegen reads ResolvedCallTarget blindly (Issue #9) — this
  // ensures it's always set for the common single-overload case, even when
  // the TypeChecker hasn't run.
  //
  // Multiple arity-matching candidates means the function has type-overloads
  // (e.g. add(int,int) and add(double,double)).  In that case TypeChecker
  // narrows to one via type compatibility and sets ResolvedCallTarget itself.
  if (Node.ResolvedCandidates.size() == 1) {
    Node.ResolvedCallTarget = Node.ResolvedCandidates[0];
  }
}


void Resolver::visitMethodCall(MethodCallExprAST &Node) {
  // Resolve argument expressions FIRST (mirrors visitFuncCall's own
  // ordering) so a genuinely broken argument (e.g. an undeclared variable)
  // is still reported even when the base itself turns out to be rejected
  // below -- errors accumulate across the whole Resolver pass rather than
  // stopping at the first one, so there's no reason to skip resolving the
  // arguments just because the call as a whole won't be allowed to proceed.
  for (const auto &Arg : Node.getArgs())
    visitChild(Arg.get());

  // 'new'/'default' are reserved for construction (StructName::new(...)/
  // StructName::default()) and can never be invoked via '.', on ANY base --
  // checked unconditionally, up front, so a real instance is rejected here
  // too (a bare struct TYPE NAME base is separately, already caught by the
  // isKnownStruct() branch below via DotOperatorOnNonInstanceError, for
  // every method name, not just these two -- this check exists specifically
  // for the gap that WOULD otherwise remain: a real instance calling
  // `.new(...)` would otherwise resolve completely normally, since `new` is
  // registered as an ordinary '.'-declared instance method internally).
  if (Node.getMethod() == "new" || Node.getMethod() == "default") {
    ConstructorCalledOnInstanceError(Node.getLoc(), Node.getMethod()).raise();
    return;
  }

  // IMPORTANT — variable shadowing: a locally-declared variable of the same
  // spelling as a module or struct type ALWAYS takes priority, exactly like
  // ordinary lexical scoping everywhere else in the language. This is not a
  // hypothetical: `io` used to be auto-imported into EVERY Turf program via
  // lib/std/prelude.tr, so a variable literally named `io` used to always be
  // misread as the io module — `mut io: Thing = ...; io.show();` raised
  // "Unknown function 'io.show'" instead of calling Thing's struct method,
  // because the module check ran before any check for an actual declared
  // variable of that name.
  auto *BaseVar = dynamic_cast<VariableExprAST *>(
      const_cast<ExprAST *>(Node.getBase()));
  if (BaseVar) {
    auto ExistingVar = SymTable_.lookupVariable(BaseVar->getName());
    if (ExistingVar) {
      // Base is a declared variable — resolve it directly (equivalent to
      // what visitVariable() would do, without re-running its lookup).
      BaseVar->ResolvedSymbol = ExistingVar;
      return;
    }
    if (SymTable_.hasModule(BaseVar->getName())) {
      // Base is a module qualifier used with '.' -- not allowed. '.' is
      // instance-member-access only; a module-qualified call must use
      // '::' (io::printline(...), not io.printline(...)). This used to
      // silently succeed via VariableExprAST::ResolvedModuleName, a legacy
      // resolution path threaded all the way through TypeChecker/Codegen
      // that made '.' a working, undocumented alias for '::' here --
      // confirmed live before this check existed. That field (and its
      // downstream TypeChecker.cpp/Codegen.cpp readers) are left in place
      // purely as defense-in-depth; they can no longer be reached in a
      // real compile now that this branch always raises and returns
      // instead of ever setting it.
      DotOperatorOnNonInstanceError(Node.getLoc(), BaseVar->getName(),
                                    "module", Node.getMethod())
          .raise();
      return;
    }
    if (SymTable_.isKnownStruct(BaseVar->getName())) {
      // Base is a bare struct TYPE name used with '.' -- same rule as the
      // module case just above: only '::' can invoke a static method
      // (StructName::method(...)), never '.'. This used to silently
      // succeed for any method whose arity happened to match with no
      // `self` (i.e. any `::`-declared static method, or coincidentally
      // any '.'-declared instance method called with one fewer argument
      // than it actually needs) via VariableExprAST::IsStructName, a
      // legacy path originally added only to turn a compiler CRASH into a
      // clean arity error -- never intended to make '.' a real, working
      // way to call a static method, but it accidentally became exactly
      // that once true static methods (fn StructName::method(...), no
      // self at all) were introduced. Confirmed live before this check
      // existed. IsStructName is left in place (TypeChecker.cpp/
      // Codegen.cpp still read it) as defense-in-depth only, same
      // rationale as ResolvedModuleName above.
      DotOperatorOnNonInstanceError(Node.getLoc(), BaseVar->getName(),
                                    "struct type", Node.getMethod())
          .raise();
      return;
    }
    // Base is neither a declared variable, a module, nor a struct name —
    // resolve normally so visitVariable() raises the proper ReferenceError.
    visitChild(Node.getBase());
    return;
  }
  // Base is a complex expression — resolve normally.
  visitChild(Node.getBase());
}


void Resolver::visitFieldAccess(FieldAccessExprAST &Node) {
  visitChild(Node.getBase());
}


// `<expr> recover ...` (docs/architecture/errors_and_recover.md). Resolves the
// wrapped `!T` expression first (populating its callee's ResolvedCandidates/
// ResolvedCallTarget, needed below), then dispatches on which of the three
// forms this is.
void Resolver::visitRecoverExpr(RecoverExprAST &Node) {
  visitChild(const_cast<ExprAST *>(Node.getWrapped()));

  if (Node.getKind() == RecoverKind::Produce) {
    visitChild(const_cast<ExprAST *>(Node.getProduceExpr()));
    return;
  }

  // Run / AsRun: the block gets its own scope, mirroring if/while/function
  // bodies -- needed so AsRun's bound name doesn't leak past the block.
  SymTable_.enterScope();

  if (Node.getKind() == RecoverKind::AsRun) {
    // Bare-error restriction (docs/architecture/errors_and_recover.md):
    // `recover as err run` is rejected when every error type this call can
    // actually fail with (per the callee's PossibleErrorTypeNames, computed by
    // CollectReturnedErrorTypeNames -- see its own doc comment on why it's
    // unfiltered) is bare-form (`error Name;`, sugar for one informationless
    // DEFAULT variant, nothing to discriminate on). If the callee is
    // ambiguous (overloaded, no unique ResolvedCallTarget) or has no
    // apparent error-shaped return at all, conservatively allow it --
    // erring toward under-flagging, same rationale as MutatesSelfField's
    // doc comment for hard-error-gating checks.
    if (const auto *Call = dynamic_cast<const FuncCallExprAST *>(Node.getWrapped())) {
      if (const auto *Target = Call->getResolvedCallTarget()) {
        std::vector<std::string> RealErrorTypes;
        for (const auto &Name : Target->PossibleErrorTypeNames)
          if (SymTable_.isErrorTypeName(Name))
            RealErrorTypes.push_back(Name);

        if (!RealErrorTypes.empty()) {
          bool AllBare = true;
          for (const auto &Name : RealErrorTypes) {
            auto ET = SymTable_.lookupErrorType(Name);
            if (ET && !ET->isBareForm()) {
              AllBare = false;
              break;
            }
          }
          if (AllBare) {
            BareErrorAsBindingError(Node.getLoc(), Node.getBoundName()).raise();
          }
        }
      }
    }

    SourceLocation Loc = Node.getLoc();
    SymTable_.declareVariable(Node.getBoundName(), TurfAnyError(),
                              {Loc.Line, Loc.Col});
  }

  visitChild(const_cast<ExprAST *>(Node.getBlock()));
  SymTable_.exitScope();
}


// Handles a `Left::Right` node reached as an ordinary expression -- i.e.
// NOT as the direct callee of a FuncCallExprAST (that case is handled
// specially and more narrowly by visitFuncCall itself, mirroring how a bare
// VariableExprAST callee is special-cased there too, so this function never
// even sees it). What's left for this function to disambiguate:
//   - EnumName::Variant used as a value (e.g. `mut c: Color = Color::Red;`)
//     -- the original, only-ever-supported meaning of this node.
//   - Module::something referenced but NOT called -- this language has no
//     first-class functions, so there is no valid non-call meaning of a
//     module-qualified name; reported as a clean error instead of reaching
//     Codegen and crashing (FuncCallExprAST::codegen() already has a
//     "First-class functions are not supported yet" internal error for
//     exactly this shape once it gets that far un-rejected).
void Resolver::visitScopeAccess(ScopeAccessExprAST &Node) {
  const std::string &LeftName = Node.getLeftName();
  const std::string &RightName = Node.getRightName();

  if (SymTable_.isEnumName(LeftName)) {
    Node.ResolvedKind = ScopeAccessKind::EnumVariant;
    // Module::EnumName::Variant -- the parser already reinterpreted the
    // module-qualified enum name into ModuleQualifier/LeftName by the time
    // this runs; validate it the same way a qualified type/struct-literal
    // is (Resolver::validateModuleQualifier()).
    if (!validateModuleQualifier(Node.getModuleQualifier(), LeftName, Node.getLoc()))
      return;
    if (RightName != "COUNT" && RightName != "VALUES" && RightName != "NAMES") {
      auto ET = SymTable_.lookupEnumType(LeftName);
      if (ET && !ET->hasVariant(RightName)) {
        RankedSuggestions Suggestions;
        Suggestions.Local = SuggestionIndex::suggestVisible(RightName, ET->getVariants());
        NoSuchVariantError(Node.getRightLoc(), "enum", LeftName, RightName, Suggestions)
            .raise();
      }
    }
    return;
  }

  if (SymTable_.isErrorTypeName(LeftName)) {
    // Bare `ErrorType::Variant` reference
    // (docs/architecture/errors_and_recover.md) -- e.g. `return
    // MathError::DivisionByZero;` or `err == MathError:: DivisionByZero`.
    // Mirrors the EnumVariant branch above exactly: a variant is always a bare
    // name, never callable (payload fields are deferred, see
    // errors_and_recover.md §5.1).
    Node.ResolvedKind = ScopeAccessKind::ErrorVariantRef;
    auto ET = SymTable_.lookupErrorType(LeftName);
    if (!ET)
      return; // Should not happen -- isErrorTypeName() just confirmed it.

    if (!ET->hasVariant(RightName)) {
      RankedSuggestions Suggestions;
      Suggestions.Local = SuggestionIndex::suggestVisible(RightName, ET->getVariantNames());
      NoSuchVariantError(Node.getRightLoc(), "error", LeftName, RightName, Suggestions)
          .raise();
    }
    return;
  }

  if (SymTable_.hasModule(LeftName)) {
    // `module::CONST` -- a qualified reference to an `export immut`
    // constant. Checked BEFORE the "functions aren't first-class" fallback
    // just below, since a constant genuinely IS usable as a value (unlike a
    // bare `module::function`).
    if (const auto *Const = SymTable_.findModuleConstant(LeftName, RightName)) {
      Node.ResolvedKind = ScopeAccessKind::ModuleConstantRef;
      Node.ResolvedConstant = Const;
      return;
    }
    // A module name IS real, but referencing `module::function` without
    // calling it has no meaning in this language yet.
    TypeError(Node.getLoc(), "'" + LeftName + "::" + RightName +
                                 "' can't be used as a value -- functions "
                                 "aren't first-class yet, only calls like '" +
                                 LeftName + "::" + RightName + "(...)' work.",
              "Call it directly at the point you need its result, or wrap "
              "it in your own function if you need to pass it around.",
              "Not a value!")
        .raise();
    return;
  }

  if (SymTable_.isKnownStruct(LeftName)) {
    // A struct name IS real, but referencing `StructName::method` without
    // calling it has no meaning either -- same "no first-class functions"
    // rule as the module case just above, worded for the static-method form.
    Node.ResolvedKind = ScopeAccessKind::StaticMethodRef;
    TypeError(Node.getLoc(), "'" + LeftName + "::" + RightName +
                                 "' can't be used as a value -- functions "
                                 "aren't first-class yet, only calls like '" +
                                 LeftName + "::" + RightName + "(...)' work.",
              "Call it directly at the point you need its result, or wrap "
              "it in your own function if you need to pass it around.",
              "Not a value!")
        .raise();
    return;
  }

  // Same "you meant '.'" specific-error upgrade as visitFuncCall's
  // equivalent fallback -- see its comment for why this is worth
  // distinguishing from the generic "unknown qualifier" case below.
  if (SymTable_.lookupVariable(LeftName)) {
    ScopeOperatorOnInstanceError(Node.getLoc(), LeftName, RightName).raise();
    return;
  }

  RankedSuggestions Suggestions;
  auto QualifierSugg = SuggestionIndex::suggestVisible(
      LeftName, unknownQualifierSuggestionCandidates());
  for (const auto &S : QualifierSugg)
    Suggestions.Local.push_back(S + "::" + RightName);
  UnknownScopeQualifierError(Node.getLoc(), LeftName, Suggestions).raise();
}
