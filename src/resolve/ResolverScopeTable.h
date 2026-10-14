#ifndef RESOLVER_SCOPE_TABLE_H
#define RESOLVER_SCOPE_TABLE_H

#include "resolve/ResolverSymbolBase.h" // VariableSymbol, ResolverSourceLocation

#include <memory>
#include <ostream>
#include <string>
#include <vector>

// ─── ResolverScope ───────────────────────────────────────────────────────────

/**
 * @brief A single lexical scope in the Resolver's scope stack.
 *
 * Uses a pointer-to-parent chain so scopes can be queried independently of
 * the full stack (useful for the TypeChecker which may clone a scope chain).
 */
struct ResolverScope {
  size_t Depth;   ///< 0 = global, increments per nested block
  ResolverScope *Parent = nullptr; ///< Outer scope, or nullptr at global level

  /// Variable name → owned VariableSymbol
  std::unordered_map<std::string, std::shared_ptr<VariableSymbol>> Symbols;

  explicit ResolverScope(size_t Depth, ResolverScope *Parent = nullptr)
      : Depth(Depth), Parent(Parent) {}

  /// Look up a name in this scope only (no parent traversal).
  std::shared_ptr<VariableSymbol> lookupLocal(const std::string &Name) const {
    auto It = Symbols.find(Name);
    return It != Symbols.end() ? It->second : nullptr;
  }

  /// True if @p Name is declared directly in this scope.
  bool has(const std::string &Name) const {
    return Symbols.count(Name) > 0;
  }
};

// ─── ResolverScopeTable ──────────────────────────────────────────────────────

/**
 * @brief Lexical-scope half of the Resolver's symbol table (split out of the
 * original monolithic ResolverSymbolTable -- see
 * docs/architecture/compiler_pipeline.md). Manages a stack of
 * ResolverScope objects; owns no module/type-registry state at all.
 *
 * Error handling: lookup failures return nullptr (not exceptions) so that the
 * Resolver can accumulate multiple errors per compilation run.  enterScope /
 * exitScope are infallible (no error on over-pop; production code tracks this
 * separately).
 */
class ResolverScopeTable {
public:
  ResolverScopeTable();

  /// Push a new nested scope.
  void enterScope();

  /// Pop the current scope.  Variables declared in it become invisible.
  void exitScope();

  /// Reset the scope stack to the specified depth. Used during error recovery.
  void resetToDepth(size_t TargetDepth);

  /// Current nesting depth (0 = global).
  size_t currentDepth() const { return Depth_; }

  /**
   * @brief Declare a new variable in the current scope.
   *
   * Returns a shared_ptr to the new VariableSymbol so the Resolver can attach
   * it to AST nodes (Issue #5).  Throws std::runtime_error if the name is
   * already declared in the *current* scope (shadowing outer scopes is allowed).
   */
  std::shared_ptr<VariableSymbol> declareVariable(
      const std::string &Name, TurfTypePtr Type,
      ResolverSourceLocation Loc = {}, bool IsImmutable = false,
      bool IsParameter = false);

  /**
   * @brief Look up a variable by name, searching from the innermost scope out.
   * @return Shared pointer to the symbol, or nullptr if not found.
   */
  std::shared_ptr<VariableSymbol> lookupVariable(const std::string &Name) const;

  /**
   * @brief True if @p Name is declared in the current (innermost) scope only.
   */
  bool isDeclaredInCurrentScope(const std::string &Name) const;

  /**
   * @brief Collect every variable name visible from the current scope.
   *
   * Walks the scope stack from innermost to outermost and deduplicates names
   * so that a shadowed outer variable is only listed once.  The result is
   * suitable as the candidate list passed to SuggestionIndex::suggestVisible
   * when emitting an "Unknown Variable" diagnostic.
   */
  std::vector<std::string> getAllVisibleNames() const;

  /// Emits the "Variable Scopes" section of ResolverSymbolTable::dump().
  void dumpScopes(std::ostream &OS) const;

private:
  // Stack of scope objects.  We own them via unique_ptr; the parent pointer
  // inside ResolverScope is a raw (non-owning) back-reference.
  std::vector<std::unique_ptr<ResolverScope>> ScopeStack_;
  size_t Depth_ = 0;
};

#endif // RESOLVER_SCOPE_TABLE_H
