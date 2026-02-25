#include <algorithm>
#include <string>
#include <vector>

using namespace std;

// Standard DP implementation of Levenshtein Distance
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
      dp[i][j] =
          min({dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + cost});
    }
  }

  return dp[m][n];
}
