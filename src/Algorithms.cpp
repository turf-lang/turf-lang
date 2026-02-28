#include <algorithm>
#include <string>
#include <vector>

using namespace std;

// DP implementation of Damerau-Levenshtein Distance (Optimal String Alignment)
// Supports insertions, deletions, substitutions, and transpositions.
// Transpositions improve detection of common typos like "pritn" -> "print".
int getLevenshteinDistance(const string &s1, const string &s2) {
  size_t m = s1.length();
  size_t n = s2.length();

  if (m == 0)
    return n;

  if (n == 0)
    return m;

  vector<vector<int>> dp(m + 1, vector<int>(n + 1));

  for (size_t i = 0; i <= m; ++i)
    dp[i][0] = i;
  for (size_t j = 0; j <= n; ++j)
    dp[0][j] = j;

  for (size_t i = 1; i <= m; ++i) {
    for (size_t j = 1; j <= n; ++j) {
      int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
      
      dp[i][j] = min({
        dp[i - 1][j] + 1,           // deletion
        dp[i][j - 1] + 1,           // insertion
        dp[i - 1][j - 1] + cost     // substitution
      });

      // Check for transposition (swap of adjacent characters)
      if (i > 1 && j > 1 && 
          s1[i - 1] == s2[j - 2] && 
          s1[i - 2] == s2[j - 1]) {
        dp[i][j] = min(dp[i][j], dp[i - 2][j - 2] + 1);
      }
    }
  }

  return dp[m][n];
}
