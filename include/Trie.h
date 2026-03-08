#ifndef TRIE_H
#define TRIE_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Category tags for filtering suggestions by context
enum class IdentifierCategory {
  VARIABLE,     // User-defined variables in scope
  BUILTIN,      // Built-in functions (print, input, etc.)
  KEYWORD,      // Language keywords (if, while, etc.)
  TYPE          // Type names (int, string, etc.)
};

// TrieNode: represents one node in the prefix tree
struct TrieNode {
  std::unordered_map<char, std::unique_ptr<TrieNode>> Children;
  bool IsEndOfWord = false;
  IdentifierCategory Category;
  size_t ScopeLevel = 0;  // For variables: tracks which scope they belong to
  
  TrieNode() = default;
};

// Trie: efficient prefix-based identifier storage
// Enables O(k) insertions and O(k) lookups where k = key length
// Optimized for "did you mean" suggestions with scope-aware filtering
class Trie {
private:
  std::unique_ptr<TrieNode> Root;
  
  // Helper for collecting words with category filtering
  void CollectWordsHelper(TrieNode *Node, const std::string &Prefix,
                         std::vector<std::string> &Results,
                         IdentifierCategory FilterCategory,
                         size_t MaxScopeLevel) const;
  
  // Helper for collecting all words without category filtering
  void CollectAllWordsHelper(TrieNode *Node, const std::string &Prefix,
                            std::vector<std::string> &Results,
                            size_t MaxScopeLevel) const;

public:
  Trie() : Root(std::make_unique<TrieNode>()) {}
  
  // Insert an identifier into the trie
  void Insert(const std::string &Word, IdentifierCategory Category,
              size_t ScopeLevel = 0);
  
  // Remove an identifier from the trie (for scope exit)
  void Remove(const std::string &Word);
  
  // Check if a word exists in the trie
  bool Contains(const std::string &Word) const;
  
  // Get all identifiers matching a prefix
  std::vector<std::string> GetWordsWithPrefix(const std::string &Prefix) const;
  
  // Get all identifiers matching a prefix, filtered by category and scope
  // MaxScopeLevel: only return identifiers visible at this scope level
  std::vector<std::string> GetFilteredWords(
      const std::string &Prefix,
      IdentifierCategory FilterCategory,
      size_t MaxScopeLevel = SIZE_MAX) const;
  
  // Get all identifiers in the trie (for unfiltered suggestions)
  std::vector<std::string> GetAllWords() const;
  
  // Get all identifiers filtered by category and scope
  std::vector<std::string> GetAllFilteredWords(
      IdentifierCategory FilterCategory,
      size_t MaxScopeLevel = SIZE_MAX) const;
  
  // Clear all entries (useful for scope management)
  void Clear();
  
  // Get the number of unique identifiers stored
  size_t Size() const;
};

// SuggestionEngine: Coordinates tries and edit distance for smart suggestions
class SuggestionEngine {
private:
  Trie VariableTrie;    // Stores user-defined variables
  Trie BuiltinTrie;     // Stores built-in functions
  Trie KeywordTrie;     // Stores language keywords
  
  // Find best matches using edit distance, pre-filtered by scope/category
  std::vector<std::string> RankCandidates(
      const std::string &Typo,
      const std::vector<std::string> &Candidates,
      int MaxDistance = 2) const;

public:
  SuggestionEngine() = default;
  
  // Variable management (scope-aware)
  void AddVariable(const std::string &Name, size_t ScopeLevel);
  void RemoveVariable(const std::string &Name);
  
  // Builtin/keyword registration (one-time setup)
  void AddBuiltin(const std::string &Name);
  void AddKeyword(const std::string &Name);
  
  // Get suggestions for an unknown identifier
  // Searches variables in current scope, then builtins, then keywords
  std::vector<std::string> GetVariableSuggestions(const std::string &Typo,
                                                   size_t CurrentScopeLevel,
                                                   int MaxDistance = 2) const;
  
  // Get suggestions for unknown builtin
  std::vector<std::string> GetBuiltinSuggestions(const std::string &Typo,
                                                  int MaxDistance = 2) const;
  
  // Get suggestions for unknown keyword
  std::vector<std::string> GetKeywordSuggestions(const std::string &Typo,
                                                  int MaxDistance = 2) const;
  
  // Get all visible variables at a scope level
  std::vector<std::string> GetVisibleVariables(size_t ScopeLevel) const;
  
  // Clear all tries (for testing/reset)
  void Clear();
};

// Global suggestion engine instance
extern std::unique_ptr<SuggestionEngine> GlobalSuggestionEngine;

// Initialize the global suggestion engine with keywords and builtins
void InitializeSuggestionEngine();

#endif
