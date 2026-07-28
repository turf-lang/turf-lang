#ifndef SUGGESTION_INDEX_H
#define SUGGESTION_INDEX_H

#include "Lexer.h"
#include "Builtins.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace SuggestionIndex {

namespace detail {

struct TrieNode {
  bool Terminal = false;
  std::map<char, std::unique_ptr<TrieNode>> Children;
};

class Trie {
public:
  void insert(const std::string &Word) {
    TrieNode *Node = &Root;
    for (char Ch : Word) {
      auto &Child = Node->Children[Ch];
      if (!Child) {
        Child = std::make_unique<TrieNode>();
      }
      Node = Child.get();
    }
    Node->Terminal = true;
  }

  std::vector<std::pair<std::string, int>> search(const std::string &Query,
                                                  int MaxDistance) const {
    std::vector<std::pair<std::string, int>> Results;
    std::vector<int> InitialRow(Query.size() + 1);
    for (size_t I = 0; I <= Query.size(); ++I) {
      InitialRow[I] = static_cast<int>(I);
    }

    for (const auto &Child : Root.Children) {
      std::string Prefix(1, Child.first);
      searchRecursive(Child.second.get(), Prefix, Child.first, Query,
                      InitialRow, MaxDistance, Results);
    }

    return Results;
  }

private:
  TrieNode Root;

  void searchRecursive(const TrieNode *Node, const std::string &Prefix,
                       char CurrentChar, const std::string &Query,
                       const std::vector<int> &PreviousRow, int MaxDistance,
                       std::vector<std::pair<std::string, int>> &Results) const {
    std::vector<int> CurrentRow(Query.size() + 1);
    CurrentRow[0] = PreviousRow[0] + 1;

    for (size_t I = 1; I <= Query.size(); ++I) {
      int InsertCost = CurrentRow[I - 1] + 1;
      int DeleteCost = PreviousRow[I] + 1;
      int ReplaceCost = PreviousRow[I - 1] + (Query[I - 1] == CurrentChar ? 0 : 1);
      CurrentRow[I] = std::min({InsertCost, DeleteCost, ReplaceCost});
    }

    if (Node->Terminal && CurrentRow.back() <= MaxDistance) {
      Results.emplace_back(Prefix, CurrentRow.back());
    }

    int BestInRow = *std::min_element(CurrentRow.begin(), CurrentRow.end());
    if (BestInRow > MaxDistance) {
      return;
    }

    for (const auto &Child : Node->Children) {
      searchRecursive(Child.second.get(), Prefix + Child.first, Child.first,
                      Query, CurrentRow, MaxDistance, Results);
    }
  }
};

inline int getThreshold(const std::string &Query) {
  size_t N = Query.size();
  if (N <= 4) return 1;
  if (N <= 7) return 2;
  return static_cast<int>(std::max<size_t>(3, N / 3));
}

inline std::vector<std::string> buildKeywordCandidates() {
  // 'static' is deliberately excluded from fuzzy keyword-typo suggestions:
  // it sits at edit-distance 2 from extremely common identifiers like
  // 'state' (well within getThreshold()'s distance-2 allowance for a
  // 5-letter query) -- confirmed live via `fn f(state: T): ...` starting to
  // raise a false "did you mean 'static'?" KeywordError the moment 'static'
  // was added as a keyword, even though 'state' is a perfectly ordinary
  // parameter name with no typo at all. Unlike every other keyword here,
  // 'static' is only ever meaningful in one narrow grammatical position
  // (right after a struct method's parameter list, before the return-type
  // ':') -- a genuine typo of it there still surfaces as an ordinary
  // "expected ':'" parse error, so the false-positive cost of suggesting it
  // as a typo elsewhere far outweighs the rare genuine-typo benefit.
  static const std::unordered_set<std::string> ExcludedFromTypoSuggestions = {
      "static"};
  std::vector<std::string> Candidates;
  Candidates.reserve(Keywords.size());
  for (const auto &Pair : Keywords) {
    if (ExcludedFromTypoSuggestions.count(Pair.first))
      continue;
    Candidates.push_back(Pair.first);
  }
  return Candidates;
}

inline std::vector<std::string> builtinCandidates() {
  return GetBuiltinNames();
}

inline std::vector<std::string> suggestFromCandidates(
    const std::string &Query, const std::vector<std::string> &Candidates,
    size_t MaxResults = 3) {
  if (Query.empty() || Candidates.empty() || MaxResults == 0)
    return {};

  const int Threshold = getThreshold(Query);
  Trie TrieIndex;
  std::unordered_set<std::string> Seen;

  for (const auto &Candidate : Candidates) {
    if (Candidate.empty() || Candidate == Query)
      continue;

    int LengthDelta = static_cast<int>(Candidate.size()) - static_cast<int>(Query.size());
    if (LengthDelta < 0)
      LengthDelta = -LengthDelta;
    if (LengthDelta > Threshold)
      continue;

    if (!Seen.insert(Candidate).second)
      continue;

    TrieIndex.insert(Candidate);
  }

  std::vector<std::pair<std::string, int>> Matches = TrieIndex.search(Query, Threshold);
  std::sort(Matches.begin(), Matches.end(), [](const auto &A, const auto &B) {
    if (A.second != B.second)
      return A.second < B.second;
    return A.first < B.first;
  });

  std::vector<std::string> Suggestions;
  std::unordered_set<std::string> Added;
  for (const auto &Match : Matches) {
    if (!Added.insert(Match.first).second)
      continue;

    Suggestions.push_back(Match.first);
    if (Suggestions.size() == MaxResults)
      break;
  }

  return Suggestions;
}

inline std::vector<std::string> suggestKeywords(const std::string &Query,
                                                size_t MaxResults = 3) {
  return suggestFromCandidates(Query, buildKeywordCandidates(), MaxResults);
}

inline std::vector<std::string> suggestBuiltins(const std::string &Query,
                                                size_t MaxResults = 3) {
  return suggestFromCandidates(Query, builtinCandidates(), MaxResults);
}

inline std::vector<std::string> suggestVisible(const std::string &Query,
                                               const std::vector<std::string> &VisibleNames,
                                               size_t MaxResults = 3) {
  return suggestFromCandidates(Query, VisibleNames, MaxResults);
}

} // namespace detail

inline std::vector<std::string> suggestKeywords(const std::string &Query,
                                                size_t MaxResults = 3) {
  return detail::suggestKeywords(Query, MaxResults);
}

inline std::vector<std::string> suggestBuiltins(const std::string &Query,
                                                size_t MaxResults = 3) {
  return detail::suggestBuiltins(Query, MaxResults);
}

inline std::vector<std::string> suggestVisible(const std::string &Query,
                                               const std::vector<std::string> &VisibleNames,
                                               size_t MaxResults = 3) {
  return detail::suggestVisible(Query, VisibleNames, MaxResults);
}

} // namespace SuggestionIndex

#endif