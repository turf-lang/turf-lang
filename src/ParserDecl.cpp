/**
 * @file ParserDecl.cpp
 * @brief Top-level declaration parsers: struct, enum, error, extern, export, fn, operator.
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
#include <cassert>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SymbolScopeGuard {
  bool Active;
  SymbolScopeGuard(bool ShouldEnter) : Active(ShouldEnter) {
    if (Active && GlobalSymbolTable) GlobalSymbolTable->EnterScope();
  }
  ~SymbolScopeGuard() {
    if (Active && GlobalSymbolTable) GlobalSymbolTable->ExitScope();
  }
};


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
  if (RejectReservedWordAsName())
    return nullptr;
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected identifier after 'struct'", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  std::string Name = IdentifierStr;
  SourceLocation NameLoc = CurLoc;
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
      LogErrorAt(CurLoc, "Expected '>' after generic type parameters", SLM::ErrorCategory::DECLARATION);
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
      LogErrorAt(CurLoc, "Expected '{' after struct name", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    getNextToken(); // eat '{'

    while (CurTok != '}' && CurTok != TOK_EOF) {
    // LEC-002: `pub x: int` / `public x: int`. A field is private unless it
    // says so, even inside a `pub struct` (FREEZE.md §6.15) -- that is what
    // lets `List<T>.data` be hidden while the struct itself is public.
    VisibilityMark FieldVis;
    if (CurTok == TOK_PUB) {
      FieldVis = {true, CurLoc};
      getNextToken(); // eat 'pub'/'public'
    }
    // New syntax: name: type
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected field name in struct definition", SLM::ErrorCategory::DECLARATION);
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
                       "struct -- rename one of them.", SLM::ErrorCategory::DECLARATION);
        return nullptr;
      }
    }

    if (CurTok != TOK_COLON) {
      LogErrorAt(CurLoc,
                 "Expected ':' after field name '" + FieldName +
                     "' in struct definition (syntax: name: type)", SLM::ErrorCategory::DECLARATION);
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
                     FieldName + "' a real type instead.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }

    Fields.push_back({FieldName, std::move(FieldType), FieldNameLoc, FieldTypeLoc, FieldVis});

    if (CurTok == ',') {
      getNextToken();
    } else if (CurTok != '}') {
      LogErrorAt(CurLoc, "Expected ',' or '}' after field definition", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
  }

  if (CurTok != '}') {
    LogErrorAt(CurLoc, "Expected '}' to close struct definition", SLM::ErrorCategory::DECLARATION);
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
  //
  // FDN-003: registered (and the node named) under the TYPE IDENTITY, not
  // the spelling -- `json`'s `Parser` is `json::Parser`, a main-file
  // `Parser` stays `Parser`, and the two no longer overwrite each other
  // here. See TypeIdentity (Types.h).
  std::string Identity = TypeIdentity::make(CurrentParseModule(), Name);
  auto SType = std::make_shared<TurfStructType>(Identity, Fields);
  if (GlobalSymbolTable) {
    GlobalSymbolTable->RegisterGlobalType(Identity, SType);
  }

  return std::make_unique<StructDeclExprAST>(Loc, Identity, NameLoc, Fields, TypeParams);
}

// ParseEnumDecl - called when CurTok == TOK_ENUM.
std::unique_ptr<ExprAST> ParseEnumDecl() {
  SourceLocation Loc = CurLoc;
  getNextToken(); // eat 'enum'
  
  if (RejectReservedWordAsName())
    return nullptr;
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected identifier after 'enum'", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  std::string Name = IdentifierStr;
  getNextToken();

  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' after enum name", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat '{'

  std::vector<std::string> Variants;
  while (CurTok != '}' && CurTok != TOK_EOF) {
    if (CurTok == TOK_PUB) {
      // A variant is public exactly when its enum is (FREEZE.md §6.15).
      MisplacedVisibilityError(CurLoc, MisplacedVisibilityError::Where::OnEnumVariant).raise();
      return nullptr;
    }
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected variant name in enum definition", SLM::ErrorCategory::DECLARATION);
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
                       "enum -- rename one of them.", SLM::ErrorCategory::DECLARATION);
        return nullptr;
      }
    }

    Variants.push_back(IdentifierStr);
    getNextToken(); // eat variant name

    if (CurTok == ',') {
      getNextToken();
    } else if (CurTok != '}') {
      LogErrorAt(CurLoc, "Expected ',' or '}' after variant name", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
  }

  if (CurTok != '}') {
    LogErrorAt(CurLoc, "Expected '}' to close enum definition", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat '}'

  // Register the enum type in GlobalSymbolTable so that codegen can look it up
  // via GlobalSymbolTable->LookupType(Name), mirroring how struct types are
  // registered during ParseStructDecl.
  // FDN-003: under the type identity -- see ParseStructDecl().
  std::string Identity = TypeIdentity::make(CurrentParseModule(), Name);
  auto EType = std::make_shared<TurfEnumType>(Identity, Variants, TurfInt());
  if (GlobalSymbolTable) {
    GlobalSymbolTable->RegisterGlobalType(Identity, EType);
  }

  return std::make_unique<EnumDeclExprAST>(Loc, Identity, Variants);
}

// ParseMachineDecl - called when CurTok == TOK_MACHINE.
// `machine Name { State -> State, ... }` -- see MachineDeclExprAST's doc
// comment (include/ASTDecl.h) for the overall design. States are declared
// implicitly by appearing on either side of an edge (no separate
// `{ State, State }` list) -- ordinal = first-appearance order, mirroring
// how enum variant order = ordinal today in ParseEnumDecl() above.
std::unique_ptr<ExprAST> ParseMachineDecl() {
  SourceLocation Loc = CurLoc;
  getNextToken(); // eat 'machine'

  if (RejectReservedWordAsName())
    return nullptr;
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected identifier after 'machine'", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  std::string Name = IdentifierStr;
  getNextToken();

  if (CurTok != '{') {
    LogErrorAt(CurLoc, "Expected '{' after machine name", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat '{'

  std::vector<std::string> States;
  std::vector<MachineEdge> Edges;
  std::map<std::string, std::set<std::string>> EdgeSet; // adjacency, also used for duplicate-edge detection

  while (CurTok != '}' && CurTok != TOK_EOF) {
    if (CurTok == TOK_PUB) {
      MisplacedVisibilityError(CurLoc, MisplacedVisibilityError::Where::OnMachineState).raise();
      return nullptr;
    }
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected state name in machine definition", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    std::string From = IdentifierStr;
    SourceLocation EdgeLoc = CurLoc;
    getNextToken(); // eat From

    if (CurTok != TOK_ARROW) {
      LogErrorAt(CurLoc, "Expected '->' after state name in machine definition", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    getNextToken(); // eat '->'

    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected target state name after '->' in machine definition", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    std::string To = IdentifierStr;
    getNextToken(); // eat To

    // Reject the same edge declared twice in one machine block. Mirrors
    // ParseEnumDecl()'s duplicate-variant check just above -- same
    // reasoning: harmless to the adjacency set either way (it just absorbs
    // the repeat), but almost certainly a copy-paste mistake worth flagging
    // at parse time rather than silently accepting it.
    if (EdgeSet[From].count(To)) {
      LogErrorAt(EdgeLoc,
                 "Machine '" + Name + "' already declares a transition from '" +
                     From + "' to '" + To + "'."
                     "\n  Hint: Remove the duplicate transition.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    EdgeSet[From].insert(To);
    Edges.push_back({From, To, EdgeLoc});

    // States are ordered by first appearance on either side of an edge --
    // linear scan, not std::find, to match this file's existing style for
    // small "have I seen this name yet" checks (see the duplicate-variant
    // loop in ParseEnumDecl() just above).
    for (const std::string &S : {From, To}) {
      bool Seen = false;
      for (const auto &Existing : States) {
        if (Existing == S) { Seen = true; break; }
      }
      if (!Seen) States.push_back(S);
    }

    if (CurTok == ',') {
      getNextToken();
    } else if (CurTok != '}') {
      LogErrorAt(CurLoc, "Expected ',' or '}' after transition", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
  }

  if (CurTok != '}') {
    LogErrorAt(CurLoc, "Expected '}' to close machine definition", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat '}'

  // Warn on any declared state with zero INCOMING edges -- see
  // UnreachableStateWarning's doc comment (include/ErrorsScope.h) for why
  // this is a warning, not an error, and its known false-positive on a
  // machine's legitimate start state. Computed here (not per-edge above)
  // since it needs the full edge set to know which states were ever a
  // target.
  {
    std::set<std::string> HasIncoming;
    for (const auto &[From, Tos] : EdgeSet)
      for (const auto &To : Tos)
        HasIncoming.insert(To);
    for (const std::string &S : States)
      if (!HasIncoming.count(S))
        UnreachableStateWarning(Loc, Name, S).warn();
  }

  // Register the machine type in GlobalSymbolTable so that a parse-time
  // lookup elsewhere in the same file already sees it -- mirrors
  // ParseEnumDecl()'s identical registration just above.
  // FDN-003: under the type identity -- see ParseStructDecl().
  std::string Identity = TypeIdentity::make(CurrentParseModule(), Name);
  auto MType = std::make_shared<TurfMachineType>(Identity, States, EdgeSet);
  if (GlobalSymbolTable) {
    GlobalSymbolTable->RegisterGlobalType(Identity, MType);
  }

  return std::make_unique<MachineDeclExprAST>(Loc, Identity, States, Edges);
}

// ParseErrorDecl - called when CurTok == TOK_ERROR
// (docs/architecture/errors_and_recover.md). Parses either:
//   error Name;                                          -- bare form, sugar
//                                                            for a single
//                                                            implicit
//                                                            DEFAULT variant
//   error Name { Variant(f: type, ...), Variant2, ... }   -- braced form
std::unique_ptr<ExprAST> ParseErrorDecl() {
  SourceLocation Loc = CurLoc;
  getNextToken(); // eat 'error'

  if (RejectReservedWordAsName())
    return nullptr;
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected identifier after 'error'", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  std::string Name = IdentifierStr;
  getNextToken();

  std::vector<std::string> Variants;
  bool IsBareForm = false;

  if (CurTok == ';') {
    // Bare form: `error GeneralError;` -- sugar for a single implicit
    // DEFAULT variant (docs/architecture/errors_and_recover.md). Unlike the
    // braced form (which ends in '}', same as struct/enum), this form's own
    // trailing ';' is consumed HERE, not by the caller -- every top-level/block
    // statement loop in this file treats TOK_ERROR like TOK_STRUCT/
    // TOK_ENUM (a construct that must NOT be followed by a ';'), so if this
    // branch left the ';' unconsumed those loops would reject it as
    // "Unexpected ';'". Same self-terminating convention as TOK_EXPORT's
    // branch in ParseBlock().
    IsBareForm = true;
    Variants.push_back("DEFAULT");
    getNextToken(); // eat ';'

    // `error Name;` already fully declares the type (an implicit DEFAULT
    // variant) -- a braced variant list can't also follow right after it.
    // Reject this explicitly rather than returning a valid bare decl and
    // leaving the stray '{ ... }' block for the top-level dispatch loop to
    // misread as unrelated statement garbage (it used to surface as a
    // cascade of unrelated "expected a value"/"expected ';'" errors instead
    // of naming the actual rule that was broken).
    if (CurTok == '{') {
      LogErrorAt(Loc,
                 "An 'error' declaration can't mix the bare ';' form with a "
                 "braced variant list."
                 "\n  Hint: use either 'error " + Name + ";' (bare, a single "
                 "implicit DEFAULT variant) or 'error " + Name +
                 " { Variant1, Variant2, ... }' (braced), not both.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
  } else if (CurTok == '{') {
    getNextToken(); // eat '{'

    // Variants are bare names only, exactly like ParseEnumDecl's loop -- no
    // payload fields (a deliberate scope decision, see
    // errors_and_recover.md §5.1; a variant with fields and a real
    // destructuring syntax to read it back safely is deferred rather than
    // half-built now).
    while (CurTok != '}' && CurTok != TOK_EOF) {
      if (CurTok == TOK_PUB) {
        MisplacedVisibilityError(CurLoc, MisplacedVisibilityError::Where::OnErrorVariant).raise();
        return nullptr;
      }
      if (CurTok != TOK_IDENTIFIER) {
        LogErrorAt(CurLoc, "Expected variant name in error type definition", SLM::ErrorCategory::DECLARATION);
        return nullptr;
      }

      // Reject a variant name reused from earlier in this same error type --
      // mirrors ParseEnumDecl's identical duplicate-variant check (see its
      // own doc comment for the exact bug this prevents: a later duplicate
      // silently orphaning the earlier one with no diagnostic anywhere).
      std::string VariantName = IdentifierStr;
      SourceLocation VariantLoc = CurLoc;
      for (const auto &Existing : Variants) {
        if (Existing == VariantName) {
          LogErrorAt(VariantLoc,
                     "Error type '" + Name + "' already has a variant named '" +
                         VariantName + "'."
                         "\n  Hint: Variant names must be unique within an "
                         "error type -- rename one of them.", SLM::ErrorCategory::DECLARATION);
          return nullptr;
        }
      }

      Variants.push_back(VariantName);
      getNextToken(); // eat variant name

      if (CurTok == ',') {
        getNextToken();
      } else if (CurTok != '}') {
        LogErrorAt(CurLoc, "Expected ',' or '}' after error variant", SLM::ErrorCategory::DECLARATION);
        return nullptr;
      }
    }

    if (CurTok != '}') {
      LogErrorAt(CurLoc, "Expected '}' to close error type definition", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    getNextToken(); // eat '}'
  } else {
    LogErrorAt(CurLoc, "Expected '{' or ';' after error type name '" + Name + "'"
               "\n  Hint: `error Name;` (bare, no variants) or "
               "`error Name { Variant1, Variant2, ... }`.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  // Register the error type in GlobalSymbolTable, mirroring how struct/enum
  // types are registered during ParseStructDecl/ParseEnumDecl.
  // FDN-003: under the type identity -- see ParseStructDecl().
  std::string Identity = TypeIdentity::make(CurrentParseModule(), Name);
  auto ErrType = std::make_shared<TurfUserErrorType>(Identity, Variants, IsBareForm);
  if (GlobalSymbolTable) {
    GlobalSymbolTable->RegisterGlobalType(Identity, ErrType);
  }

  return std::make_unique<ErrorDeclExprAST>(Loc, Identity, Variants, IsBareForm);
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
                 "(e.g. extern from \"ssl\" fn SSL_new(ctx: ptr): ptr;).", SLM::ErrorCategory::DECLARATION);
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
               "Expected 'fn' after 'extern' — e.g. extern fn malloc(size: int): ptr;", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat 'fn'

  // Expect function name
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(CurLoc, "Expected a function name after 'extern fn'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  std::string Name = IdentifierStr;
  SourceLocation NameLoc = CurLoc;
  getNextToken(); // eat name

  // Expect '('
  if (CurTok != '(') {
    LogErrorAt(NameLoc,
               "Expected '(' after extern function name '" + Name + "'.", SLM::ErrorCategory::DECLARATION);
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
                 "Expected a parameter name in extern function '" + Name + "'.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    std::string ParamName = IdentifierStr;
    getNextToken(); // eat param name

    if (CurTok != TOK_COLON) {
      LogErrorAt(CurLoc,
                 "Expected ':' after parameter name '" + ParamName +
                     "' in extern function '" + Name + "'.", SLM::ErrorCategory::DECLARATION);
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
                     "meant an opaque pointer).", SLM::ErrorCategory::DECLARATION);
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
                   Name + "'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat ')'

  // Expect ':'
  if (CurTok != TOK_COLON) {
    LogErrorAt(CurLoc,
               "Expected ':' followed by return type after parameter list of "
               "extern function '" + Name + "'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat ':'

  TurfTypePtr RetType = ParseType();
  if (!RetType) {
    LogErrorAt(CurLoc,
               "Expected return type after ':' in extern function '" + Name +
                   "'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  // extern functions must NOT have a body
  if (CurTok == '{') {
    LogErrorAt(ExternLoc,
               "extern function declarations cannot have a body.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  // Expect ';'
  if (CurTok != ';') {
    LogErrorAt(CurLoc,
               "Expected ';' after extern function declaration '" + Name + "'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat ';'

  return std::make_unique<ExternFuncDeclExprAST>(ExternLoc, Name, NameLoc,
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
// not implemented at all yet; the rule is stated for users in
// docs/reference/functions_and_modules.md's `export` section.
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
          CurTok != TOK_STRUCT && CurTok != TOK_ENUM && CurTok != TOK_MACHINE &&
          CurTok != TOK_AT &&
          CurTok != TOK_EXTERN && CurTok != TOK_EXPORT && CurTok != TOK_IMPORT &&
          CurTok != TOK_ERROR)
      getNextToken();
  };

  if (CurTok == TOK_MUTABLE) {
    LogErrorAt(ExportLoc,
               "'export mut' is not supported yet -- only 'export immut' "
               "(a compile-time constant) works right now."
               "\n  Hint: true mutable shared global state needs its own "
               "future feature (initialization ordering, real global-"
               "variable codegen). If you don't need to mutate it, use "
               "'export immut' instead.", SLM::ErrorCategory::DECLARATION);
    SkipRestOfBrokenStatement();
    return nullptr;
  }

  if (CurTok != TOK_IMMUTABLE) {
    LogErrorAt(ExportLoc,
               "Expected 'immut' or 'immutable' after 'export'."
               "\n  Hint: 'export' only works on variable declarations, "
               "e.g. export immut PI: double = 3.14159;", SLM::ErrorCategory::DECLARATION);
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
               "exported constant with no value has nothing to export.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  VarDecl->setExported(true);
  return Decl;
}

// ParseFuncDef - called when CurTok == TOK_FN.
// Parses: fn <rettype> <name>(<type> <param>, ...) { body }
// HasCallerLocation (STD-002): true when the '@caller_location' decorator
// (ParserCore.cpp's TOK_AT case) immediately preceded this 'fn' -- threaded
// through as a plain parameter rather than a global/thread-local flag so
// this function stays reentrant-safe the same way every other parameter
// here already is (matters for Sandbox.cpp's incremental reparse passes).
std::unique_ptr<ExprAST> ParseFuncDef(bool HasCallerLocation) {
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
      LogErrorAt(CurLoc, "Expected '>' after generic type parameters", SLM::ErrorCategory::DECLARATION);
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
  if (RejectReservedWordAsName())
    return nullptr;
  if (CurTok != TOK_IDENTIFIER) {
    LogErrorAt(FnLoc, "Expected a function name after 'fn'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  std::string FuncName = IdentifierStr;
  SourceLocation NameLoc = CurLoc;
  getNextToken();
  std::string StructName = "";
  bool IsStaticMethodDecl = false;

  if (CurTok == '.') {
    StructName = FuncName;
    getNextToken(); // eat '.'
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc, "Expected method name after '.'", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    // Repoint NameLoc at the METHOD name specifically -- the more useful
    // hover/rename target than the struct name captured above. See #230.
    NameLoc = CurLoc;
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
               "it (StructName::method(...) is unchanged).", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  // Expect '('
  if (CurTok != '(') {
    LogErrorAt(FnLoc, "Expected '(' after function name '" + FuncName + "'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat '('

  std::vector<ParamDecl> Params;

  // Parse parameter list: each param is: Identifier TOK_COLON ParseType()
  // Params separated by ','
  while (CurTok != ')' && CurTok != TOK_EOF) {
    if (CurTok == TOK_PUB) {
      MisplacedVisibilityError(CurLoc, MisplacedVisibilityError::Where::OnParameter).raise();
      return nullptr;
    }
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc,
                 "Expected a parameter name in the parameter list of '" +
                     FuncName + "'.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    std::string ParamName = IdentifierStr;
    SourceLocation ParamNameLoc = CurLoc;
    getNextToken(); // consume parameter name

    if (CurTok != TOK_COLON) {
      LogErrorAt(CurLoc,
                 "Expected ':' after parameter name '" + ParamName + "' in '" +
                     FuncName + "'.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    getNextToken(); // consume ':'

    SourceLocation ParamTypeLoc = CurLoc;
    TurfTypePtr ParamType = ParseType();
    if (!ParamType) {
      LogErrorAt(CurLoc,
                 "Expected a parameter type after ':' in the parameter list of '" +
                     FuncName + "'.", SLM::ErrorCategory::DECLARATION);
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
                     ParamName + "' a real type instead.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }

    Params.push_back({ParamType, ParamName, ParamNameLoc, ParamTypeLoc});

    if (CurTok == ',')
      getNextToken();
    else if (CurTok != ')')
      break;
  }

  // Expect ')'
  if (CurTok != ')') {
    LogErrorAt(
        FnLoc,
        "Expected ')' to close the parameter list of '" + FuncName + "'.", SLM::ErrorCategory::DECLARATION);
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
    LogErrorAt(CurLoc, "Expected ':' followed by return type after parameter list.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat ':'

  // Call ParseType() → save as ReturnType
  TurfTypePtr RetType = ParseType();
  if (!RetType) {
    LogErrorAt(CurLoc, "Expected return type.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  // If CurTok == ';': external declaration (forward decl)
  if (CurTok == ';') {
    getNextToken(); // eat ';'
    return std::make_unique<FuncDefExprAST>(FnLoc, FuncName, NameLoc, RetType, Params,
                                            nullptr, TypeParams,
                                            IsStaticMethodDecl, HasCallerLocation);
  }

  // Expect '{' to open function body
  if (CurTok != '{') {
    LogErrorAt(FnLoc, "Expected '{' to begin function body for '" + FuncName + "'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  auto Body = ParseBlock();
  if (!Body)
    return nullptr;



  return std::make_unique<FuncDefExprAST>(FnLoc, FuncName, NameLoc, RetType, Params,
                                          std::move(Body), TypeParams,
                                          IsStaticMethodDecl, HasCallerLocation);
}

// ParseOperatorDecl -- called when CurTok == TOK_OPERATOR.
//
// `operator +(a: string, b: string): string { ... }`. Deliberately does NOT
// share a body with ParseFuncDef(): operators are top-level-only (no '.'/'::'
// method-name handling), non-generic (no '<T>' block), and have no 'static'
// keyword -- so reusing ParseFuncDef() directly would mean threading a bunch
// of "but not for operators" conditionals through an already-long function.
// Instead this reuses ParseFuncDef()'s PARAM-LIST/RETURN-TYPE/BODY parsing
// shape verbatim (same three blocks, same error messages) and produces an
// ordinary FuncDefExprAST with a synthesized name (operatorFunctionName()) --
// every downstream pass (hoisting, Resolver, TypeChecker, Codegen mangling)
// therefore needs zero changes to handle the DECLARATION side of an operator
// at all; it's just a function with a funny name. See
// STRING_UFCS_OPERATOR_CHAR_RFC.md §4 and §7 step 3, and LANG-001
// (docs/development/changes/LANG-001.md) for the binary-set extension,
// unary support, and `[]`/`[]=` addition below.
std::unique_ptr<ExprAST> ParseOperatorDecl() {
  SourceLocation OpLoc = CurLoc;
  getNextToken(); // consume 'operator'

  // `operator[]` / `operator[]=` -- no single Lexer token to switch on (`[`
  // is also array-literal/array-access syntax elsewhere), so this is a
  // dedicated branch ahead of the ordinary spelling-token dispatch below.
  // Arity (2 for read, 3 for write) is validated after the parameter list,
  // same as every other operator -- see OperatorIndexGetName()'s doc
  // comment (ResolverSymbolBase.h) for why these two names aren't produced
  // by operatorFunctionName().
  bool IsIndexGet = false, IsIndexSet = false;
  int OpTok = 0;
  std::string FuncName;
  if (CurTok == '[') {
    getNextToken(); // eat '['
    if (CurTok != ']') {
      LogErrorAt(OpLoc, "Expected ']' to close 'operator['.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    getNextToken(); // eat ']'
    if (CurTok == TOK_ASSIGN) {
      getNextToken(); // eat '='
      IsIndexSet = true;
      FuncName = OperatorIndexSetName();
    } else {
      IsIndexGet = true;
      FuncName = OperatorIndexGetName();
    }
  } else if (isOverloadableBinaryOp(CurTok) || isOverloadableUnaryOp(CurTok)) {
    OpTok = CurTok;
    FuncName = operatorFunctionName(OpTok);
    getNextToken(); // consume the operator token
  } else {
    LogErrorAt(OpLoc,
               "Expected an overloadable operator after 'operator'."
               "\n  Hint: binary + - * / % ** & | ^ << >> == != < > <= >=, "
               "unary - ! ~, and [] / []= may be overloaded.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  // Expect '('
  if (CurTok != '(') {
    LogErrorAt(OpLoc, "Expected '(' after 'operator " + FuncName.substr(8) + "'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat '('

  std::vector<ParamDecl> Params;

  // Parse parameter list -- identical shape to ParseFuncDef()'s.
  while (CurTok != ')' && CurTok != TOK_EOF) {
    if (CurTok != TOK_IDENTIFIER) {
      LogErrorAt(CurLoc,
                 "Expected a parameter name in the parameter list of '" +
                     FuncName + "'.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    std::string ParamName = IdentifierStr;
    SourceLocation ParamNameLoc = CurLoc;
    getNextToken(); // consume parameter name

    if (CurTok != TOK_COLON) {
      LogErrorAt(CurLoc,
                 "Expected ':' after parameter name '" + ParamName + "' in '" +
                     FuncName + "'.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
    getNextToken(); // consume ':'

    SourceLocation ParamTypeLoc = CurLoc;
    TurfTypePtr ParamType = ParseType();
    if (!ParamType) {
      LogErrorAt(CurLoc,
                 "Expected a parameter type after ':' in the parameter list of '" +
                     FuncName + "'.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }

    if (isDisallowedVoidType(ParamType.get())) {
      LogErrorAt(ParamTypeLoc,
                 "'void' cannot be a parameter's type."
                 "\n  Hint: give parameter '" + ParamName + "' a real type.", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }

    Params.push_back({ParamType, ParamName, ParamNameLoc, ParamTypeLoc});

    if (CurTok == ',')
      getNextToken();
    else if (CurTok != ')')
      break;
  }

  if (CurTok != ')') {
    LogErrorAt(
        OpLoc,
        "Expected ')' to close the parameter list of '" + FuncName + "'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat ')'

  // Arity check -- LANG-001 generalizes this from "always exactly 2" to
  // depend on which operator was named:
  //   operator[]   (read)  -- exactly 2 (self, index)
  //   operator[]=  (write) -- exactly 3 (self, index, value)
  //   spelling-based, unary-capable only (! ~)      -- exactly 1
  //   spelling-based, binary-capable only            -- exactly 2
  //   spelling-based, BOTH unary and binary capable (-) -- 1 or 2; arity
  //     alone (not a separate name) is what distinguishes negation from
  //     subtraction, per operatorFunctionName()'s doc comment.
  if (IsIndexGet) {
    if (Params.size() != 2) {
      LogErrorAt(OpLoc, "'operator[]' must take exactly 2 parameters "
                        "(self, index), got " + std::to_string(Params.size()) + ".",
                SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
  } else if (IsIndexSet) {
    if (Params.size() != 3) {
      LogErrorAt(OpLoc, "'operator[]=' must take exactly 3 parameters "
                        "(self, index, value), got " + std::to_string(Params.size()) + ".",
                SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
  } else {
    bool CanUnary = isOverloadableUnaryOp(OpTok);
    bool CanBinary = isOverloadableBinaryOp(OpTok);
    std::string OpSpelling = FuncName.substr(8);
    if (Params.size() == 1) {
      if (!CanUnary) {
        LogErrorAt(OpLoc,
                   "'operator " + OpSpelling +
                       "' has no unary form -- it must take exactly 2 "
                       "parameters (a binary overload).", SLM::ErrorCategory::DECLARATION);
        return nullptr;
      }
    } else if (Params.size() == 2) {
      if (!CanBinary) {
        LogErrorAt(OpLoc,
                   "'operator " + OpSpelling +
                       "' has no binary form -- it must take exactly 1 "
                       "parameter (a unary overload).", SLM::ErrorCategory::DECLARATION);
        return nullptr;
      }
    } else {
      std::string Arities = CanUnary && CanBinary ? "1 or 2" : (CanUnary ? "1" : "2");
      LogErrorAt(OpLoc,
                 "'operator " + OpSpelling + "' must take " + Arities +
                     " parameter(s), got " + std::to_string(Params.size()) + ".", SLM::ErrorCategory::DECLARATION);
      return nullptr;
    }
  }

  // Expect ':' then return type -- identical shape to ParseFuncDef()'s.
  if (CurTok != TOK_COLON) {
    LogErrorAt(CurLoc, "Expected ':' followed by return type after parameter list.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  getNextToken(); // eat ':'

  TurfTypePtr RetType = ParseType();
  if (!RetType) {
    LogErrorAt(CurLoc, "Expected return type.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }

  if (CurTok == ';') {
    getNextToken(); // eat ';'
    return std::make_unique<FuncDefExprAST>(OpLoc, FuncName, OpLoc, RetType, Params,
                                            nullptr, std::vector<std::string>{},
                                            /*IsStaticMethodDecl=*/false);
  }

  if (CurTok != '{') {
    LogErrorAt(OpLoc, "Expected '{' to begin the body of 'operator " +
                          FuncName.substr(8) + "'.", SLM::ErrorCategory::DECLARATION);
    return nullptr;
  }
  auto Body = ParseBlock();
  if (!Body)
    return nullptr;

  return std::make_unique<FuncDefExprAST>(OpLoc, FuncName, OpLoc, RetType, Params,
                                          std::move(Body), std::vector<std::string>{},
                                          /*IsStaticMethodDecl=*/false);
}

// ParseVisibilityPrefixed - called when CurTok == TOK_PUB (`pub`/`public`).
//
// The ONE place a declaration's visibility mark is set, so "pub on something
// that is not a declaration" cannot be parsed by accident anywhere else:
// every declaration form is reached through here or not at all (struct fields
// are the one exception -- they are not expressions and ParseStructDecl()
// reads their `pub` itself). The token after `pub` is checked BEFORE it is
// parsed, so a misplacement is reported at the `pub` with a hint naming the
// fix, not as a confusing failure somewhere inside the wrong construct.
//
// `pub extern fn` is accepted: an extern fn is a named top-level function and
// the freeze's rule is "every named declaration" (LEC-002.md, Unresolved
// question 1).
std::unique_ptr<ExprAST> ParseVisibilityPrefixed() {
  using Where = MisplacedVisibilityError::Where;
  VisibilityMark Mark{true, CurLoc};
  getNextToken(); // eat 'pub'/'public'

  std::unique_ptr<ExprAST> Decl;
  switch (CurTok) {
  case TOK_FN:
    Decl = ParseFuncDef();
    break;
  case TOK_STRUCT:
    Decl = ParseStructDecl();
    break;
  case TOK_ENUM:
    Decl = ParseEnumDecl();
    break;
  case TOK_MACHINE:
    Decl = ParseMachineDecl();
    break;
  case TOK_ERROR:
    Decl = ParseErrorDecl();
    break;
  case TOK_EXTERN:
    Decl = ParseExternDecl();
    break;
  case TOK_EXPORT:
    MisplacedVisibilityError(Mark.Loc, Where::WithExport).raise();
    return nullptr;
  case TOK_OPERATOR:
    MisplacedVisibilityError(Mark.Loc, Where::OnOperator).raise();
    return nullptr;
  case TOK_IMPORT:
    MisplacedVisibilityError(Mark.Loc, Where::OnImport).raise();
    return nullptr;
  case TOK_MUTABLE:
  case TOK_IMMUTABLE:
    MisplacedVisibilityError(Mark.Loc, Where::OnVariable).raise();
    return nullptr;
  case TOK_UNSAFE:
    MisplacedVisibilityError(Mark.Loc, Where::OnUnsafeBlock).raise();
    return nullptr;
  case TOK_AT:
    MisplacedVisibilityError(Mark.Loc, Where::BeforeDecorator).raise();
    return nullptr;
  default:
    MisplacedVisibilityError(Mark.Loc, Where::NotFollowedByDeclaration).raise();
    return nullptr;
  }
  if (!Decl)
    return nullptr;

  // Every declaration node this switch can produce derives from VisibleDecl
  // (ASTDecl.h). A failed cast would mean a declaration form was added above
  // without giving it the mixin -- fail loudly rather than drop the `pub`.
  auto *Visible = dynamic_cast<VisibleDecl *>(Decl.get());
  assert(Visible && "declaration parsed under 'pub' cannot carry a visibility");
  if (Visible)
    Visible->setVisibility(Mark);
  return Decl;
}

// RejectReservedWordAsName - call where a NAME is expected and the token is
// not an identifier. `pub`/`public` became reserved words in LEC-002 and a
// lone `_` in LEC-003, so `mut pub: int` and `mut _: int` used to be valid
// (or at least lexable) programs and are now rejected ones; the bare
// "Expected a variable name" would leave the writer guessing why, so this
// names the cause and suggests a rename. Returns true if it reported.
bool RejectReservedWordAsName() {
  if (CurTok == TOK_WILDCARD) {
    // `_` alone is the wildcard pattern. `_name` is an ordinary identifier, and
    // is the spelling the unused-variable hint already recommends.
    InvalidPatternError(
        CurLoc,
        "'_' is the wildcard pattern, so it can't be used as a name.",
        "Pick a real name, or prefix one with an underscore ('_unused') to "
        "say a variable is deliberately unused. A bare '_' only means "
        "\"match anything\" in a `compare` pattern.")
        .raise();
    return true;
  }
  if (CurTok != TOK_PUB)
    return false;
  MisplacedVisibilityError(CurLoc, MisplacedVisibilityError::Where::UsedAsName).raise();
  return true;
}
