#include "Trie.h"
#include "Algorithms.h"
#include "Builtins.h"
#include "Lexer.h"
#include <algorithm>

std::unique_ptr<SuggestionEngine> GlobalSuggestionEngine;

void InitializeSuggestionEngine() {
  GlobalSuggestionEngine = std::make_unique<SuggestionEngine>();
  
  // Register all keywords
  for (const auto &pair : Keywords) {
    GlobalSuggestionEngine->AddKeyword(pair.first);
  }
  
  // Register all builtins (will be populated after RegisterBuiltins())
  for (const auto &builtin : Builtins) {
    GlobalSuggestionEngine->AddBuiltin(builtin.Name);
  }
}

// ============================================================================
// Trie Implementation
// ============================================================================

void Trie::Insert(const std::string &Word, IdentifierCategory Category,
                  size_t ScopeLevel) {
  TrieNode *Current = Root.get();
  
  for (char Ch : Word) {
    if (Current->Children.find(Ch) == Current->Children.end()) {
      Current->Children[Ch] = std::make_unique<TrieNode>();
    }
    Current = Current->Children[Ch].get();
  }
  
  Current->IsEndOfWord = true;
  Current->Category = Category;
  Current->ScopeLevel = ScopeLevel;
}

void Trie::Remove(const std::string &Word) {
  if (Word.empty())
    return;
  
  std::vector<TrieNode *> Path;
  TrieNode *Current = Root.get();
  Path.push_back(Current);
  
  // Navigate to the word's end node
  for (char Ch : Word) {
    auto It = Current->Children.find(Ch);
    if (It == Current->Children.end()) {
      return;  // Word doesn't exist
    }
    Current = It->second.get();
    Path.push_back(Current);
  }
  
  // If it's not marked as end of word, nothing to remove
  if (!Current->IsEndOfWord)
    return;
  
  Current->IsEndOfWord = false;
  
  // If this node has children, we can't remove it
  if (!Current->Children.empty())
    return;
  
  // Walk back and remove nodes that are no longer needed
  for (int i = static_cast<int>(Word.length()) - 1; i >= 0; --i) {
    TrieNode *Parent = Path[i];
    char Ch = Word[i];
    
    // Remove the child if it has no children and isn't end of another word
    TrieNode *Child = Parent->Children[Ch].get();
    if (Child->Children.empty() && !Child->IsEndOfWord) {
      Parent->Children.erase(Ch);
      
      // Continue removal upward if parent also becomes empty
      if (Parent->Children.empty() && !Parent->IsEndOfWord && i > 0) {
        continue;
      }
    }
    break;
  }
}

bool Trie::Contains(const std::string &Word) const {
  TrieNode *Current = Root.get();
  
  for (char Ch : Word) {
    auto It = Current->Children.find(Ch);
    if (It == Current->Children.end()) {
      return false;
    }
    Current = It->second.get();
  }
  
  return Current->IsEndOfWord;
}

void Trie::CollectWordsHelper(TrieNode *Node, const std::string &Prefix,
                              std::vector<std::string> &Results,
                              IdentifierCategory FilterCategory,
                              size_t MaxScopeLevel) const {
  if (!Node)
    return;
  
  if (Node->IsEndOfWord) {
    // Apply scope filter first
    if (Node->ScopeLevel <= MaxScopeLevel) {
      // For filtered searches, check category match
      // For unfiltered (GetAllWords), FilterCategory will be the node's own category
      if (FilterCategory == Node->Category) {
        Results.push_back(Prefix);
      }
    }
  }
  
  for (const auto &pair : Node->Children) {
    CollectWordsHelper(pair.second.get(), Prefix + pair.first, Results,
                      FilterCategory, MaxScopeLevel);
  }
}

// Helper for collecting all words without category filtering
void Trie::CollectAllWordsHelper(TrieNode *Node, const std::string &Prefix,
                                 std::vector<std::string> &Results,
                                 size_t MaxScopeLevel) const {
  if (!Node)
    return;
  
  if (Node->IsEndOfWord && Node->ScopeLevel <= MaxScopeLevel) {
    Results.push_back(Prefix);
  }
  
  for (const auto &pair : Node->Children) {
    CollectAllWordsHelper(pair.second.get(), Prefix + pair.first, Results,
                         MaxScopeLevel);
  }
}

std::vector<std::string> Trie::GetWordsWithPrefix(
    const std::string &Prefix) const {
  std::vector<std::string> Results;
  TrieNode *Current = Root.get();
  
  // Navigate to the prefix
  for (char Ch : Prefix) {
    auto It = Current->Children.find(Ch);
    if (It == Current->Children.end()) {
      return Results;  // Prefix not found
    }
    Current = It->second.get();
  }
  
  // Collect all words from this point (no category filter)
  CollectAllWordsHelper(Current, Prefix, Results, SIZE_MAX);
  return Results;
}

std::vector<std::string> Trie::GetFilteredWords(
    const std::string &Prefix,
    IdentifierCategory FilterCategory,
    size_t MaxScopeLevel) const {
  std::vector<std::string> Results;
  TrieNode *Current = Root.get();
  
  // Navigate to the prefix
  for (char Ch : Prefix) {
    auto It = Current->Children.find(Ch);
    if (It == Current->Children.end()) {
      return Results;
    }
    Current = It->second.get();
  }
  
  // Collect filtered words
  CollectWordsHelper(Current, Prefix, Results, FilterCategory, MaxScopeLevel);
  return Results;
}

std::vector<std::string> Trie::GetAllWords() const {
  std::vector<std::string> Results;
  CollectAllWordsHelper(Root.get(), "", Results, SIZE_MAX);
  return Results;
}

std::vector<std::string> Trie::GetAllFilteredWords(
    IdentifierCategory FilterCategory,
    size_t MaxScopeLevel) const {
  std::vector<std::string> Results;
  CollectWordsHelper(Root.get(), "", Results, FilterCategory, MaxScopeLevel);
  return Results;
}

void Trie::Clear() {
  Root = std::make_unique<TrieNode>();
}

size_t Trie::Size() const {
  size_t Count = 0;
  std::vector<std::string> Words = GetAllWords();
  return Words.size();
}

// ============================================================================
// SuggestionEngine Implementation
// ============================================================================

void SuggestionEngine::AddVariable(const std::string &Name, size_t ScopeLevel) {
  VariableTrie.Insert(Name, IdentifierCategory::VARIABLE, ScopeLevel);
}

void SuggestionEngine::RemoveVariable(const std::string &Name) {
  VariableTrie.Remove(Name);
}

void SuggestionEngine::AddBuiltin(const std::string &Name) {
  BuiltinTrie.Insert(Name, IdentifierCategory::BUILTIN, 0);
}

void SuggestionEngine::AddKeyword(const std::string &Name) {
  KeywordTrie.Insert(Name, IdentifierCategory::KEYWORD, 0);
}

std::vector<std::string> SuggestionEngine::RankCandidates(
    const std::string &Typo,
    const std::vector<std::string> &Candidates,
    int MaxDistance) const {
  
  // Compute edit distance for each candidate
  std::vector<std::pair<std::string, int>> Scored;
  
  for (const auto &Candidate : Candidates) {
    int Distance = getLevenshteinDistance(Typo, Candidate);
    if (Distance <= MaxDistance) {
      Scored.push_back({Candidate, Distance});
    }
  }
  
  // Sort by distance (ascending), then alphabetically
  std::sort(Scored.begin(), Scored.end(),
           [](const auto &a, const auto &b) {
             if (a.second != b.second)
               return a.second < b.second;
             return a.first < b.first;
           });
  
  // Extract just the names
  std::vector<std::string> Results;
  for (const auto &pair : Scored) {
    Results.push_back(pair.first);
  }
  
  return Results;
}

std::vector<std::string> SuggestionEngine::GetVariableSuggestions(
    const std::string &Typo,
    size_t CurrentScopeLevel,
    int MaxDistance) const {
  
  // Get all visible variables at current scope
  std::vector<std::string> Candidates = 
      VariableTrie.GetAllFilteredWords(IdentifierCategory::VARIABLE,
                                       CurrentScopeLevel);
  
  // Add builtins to candidates (they're always visible)
  std::vector<std::string> BuiltinCandidates = BuiltinTrie.GetAllWords();
  Candidates.insert(Candidates.end(), BuiltinCandidates.begin(),
                   BuiltinCandidates.end());
  
  return RankCandidates(Typo, Candidates, MaxDistance);
}

std::vector<std::string> SuggestionEngine::GetBuiltinSuggestions(
    const std::string &Typo,
    int MaxDistance) const {
  
  std::vector<std::string> Candidates = BuiltinTrie.GetAllWords();
  return RankCandidates(Typo, Candidates, MaxDistance);
}

std::vector<std::string> SuggestionEngine::GetKeywordSuggestions(
    const std::string &Typo,
    int MaxDistance) const {
  
  std::vector<std::string> Candidates = KeywordTrie.GetAllWords();
  return RankCandidates(Typo, Candidates, MaxDistance);
}

std::vector<std::string> SuggestionEngine::GetVisibleVariables(
    size_t ScopeLevel) const {
  return VariableTrie.GetAllFilteredWords(IdentifierCategory::VARIABLE,
                                         ScopeLevel);
}

void SuggestionEngine::Clear() {
  VariableTrie.Clear();
  BuiltinTrie.Clear();
  KeywordTrie.Clear();
}
