/**
 * @file Algorithms.cpp
 * @brief Algorithmic utilities for string distance calculation.
 * @details Implements optimal string alignment algorithms, including
 * Damerau-Levenshtein Distance. These functions are useful for finding closest
 * matching strings such as correcting typos in variable names or keywords.
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

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

/**
 * @brief Computes the Optimal String Alignment (restricted Damerau-Levenshtein)
 * Distance.
 *
 * DP implementation supporting insertions, deletions, substitutions, and
 * adjacent transpositions. Transpositions improve detection of common typos
 * like "pritn" -> "print". Note that this does not handle multiple
 * transpositions of the same substring optimally, but it is generally
 * sufficient for simple typo correction.
 *
 * @param s1 The first string to compare.
 * @param s2 The second string to compare.
 * @return int The minimum number of edit operations required to transform s1
 * into s2.
 */
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

/**
 * @brief Computes the true Damerau-Levenshtein Distance between two strings.
 *
 * This algorithm calculates the distance allowing for insertions, deletions,
 * substitutions, and transpositions of two adjacent characters. Unlike the
 * restricted version, this correctly computes the edit distance when multiple
 * transpositions might be required.
 *
 * @param s1 The first string to compare.
 * @param s2 The second string to compare.
 * @return int The true Damerau-Levenshtein distance between s1 and s2.
 */
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
