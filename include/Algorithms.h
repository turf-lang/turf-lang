#ifndef DAMERAU_LEVENSHTEIN_DISTANCE_H
#define DAMERAU_LEVENSHTEIN_DISTANCE_H

#include <string>

// Computes Damerau-Levenshtein distance (Optimal String Alignment variant).
// Supports insertions, deletions, substitutions, and adjacent transpositions.
// Used for ranking identifier/keyword typo suggestions.
int getLevenshteinDistance(const std::string &s1, const std::string &s2);

#endif
