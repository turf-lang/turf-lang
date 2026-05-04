#include <algorithm>
#include <string>
#include <unordered_map>
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
          dp[i - 1][j] + 1,       // deletion
          dp[i][j - 1] + 1,       // insertion
          dp[i - 1][j - 1] + cost // substitution
      });

      // Check for transposition (swap of adjacent characters)
      if (i > 1 && j > 1 && s1[i - 1] == s2[j - 2] && s1[i - 2] == s2[j - 1]) {
        dp[i][j] = min(dp[i][j], dp[i - 2][j - 2] + 1);
      }
    }
  }

  return dp[m][n];
}

int getDamerauLevenshteinDistance(const string &s1, const string &s2) {
  size_t m = s1.length(), n = s2.length();
  if (m == 0)
    return n;
  if (n == 0)
    return m;

  // Map each character to its last seen position
  unordered_map<char, int> last;

  vector<vector<int>> dp(m + 2, vector<int>(n + 2, 0));
  int maxDist = m + n;
  dp[0][0] = maxDist;
  for (size_t i = 0; i <= m; ++i) {
    dp[i + 1][0] = maxDist;
    dp[i + 1][1] = i;
  }
  for (size_t j = 0; j <= n; ++j) {
    dp[0][j + 1] = maxDist;
    dp[1][j + 1] = j;
  }

  for (size_t i = 1; i <= m; ++i) {
    int l = 0; // last matching column
    for (size_t j = 1; j <= n; ++j) {
      int i1 = last.count(s2[j - 1]) ? last[s2[j - 1]] : 0;
      int j1 = l;
      int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
      if (!cost)
        l = j;
      dp[i + 1][j + 1] =
          min({dp[i][j] + cost, dp[i + 1][j] + 1, dp[i][j + 1] + 1,
               dp[i1][j1] + (int)(i - i1 - 1) + 1 + (int)(j - j1 - 1)});
    }
    last[s1[i - 1]] = i;
  }
  return dp[m + 1][n + 1];
}
