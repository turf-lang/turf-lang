// SL-1: the Fix Score (FixScore.h).
#include "diagnostics/FixScore.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace FixScore {

namespace {

bool identStart(char C) { return std::isalpha(static_cast<unsigned char>(C)) || C == '_'; }
bool identChar(char C) { return std::isalnum(static_cast<unsigned char>(C)) || C == '_'; }

// Words followed by `(` that are not calls.
const std::set<std::string> &notCallees() {
  static const std::set<std::string> S = {
      "if", "elseif", "while", "for", "compare", "matches", "return", "fn",
      "function", "func", "def", "fun", "to", "in", "step", "and", "or", "not",
      "unsafe", "defer",
      "give", "guess", "recover", "do", "struct", "enum", "sizeof"};
  return S;
}

bool isAssignment(const std::string &T) {
  return T == "=" || T == "+=" || T == "-=" || T == "*=" || T == "/=" || T == "%=";
}

struct Counts {
  int Calls = 0, Asserts = 0, Returns = 0, Assignments = 0;
};

// Whether `name(` at Toks[I] declares a function rather than calls one: it
// follows a function keyword (Turf's or another language's, which a broken
// program may use), its `)` is followed by a return type (`:` or `->`), or
// its `)` is followed by `{` and it starts the statement, alone or after one
// type word (`int main() {`). `if f(x) {` is a call: `if` is a keyword.
bool declares(const std::vector<std::string> &Toks, size_t I) {
  static const std::set<std::string> FnWords = {"fn", "function", "func", "def", "fun"};
  auto startsStatement = [&](size_t J) {
    return J == 0 || Toks[J - 1] == ";" || Toks[J - 1] == "{" || Toks[J - 1] == "}";
  };
  size_t K = I;
  while (K >= 2 && Toks[K - 1] == "." && identStart(Toks[K - 2][0]))
    K -= 2; // `fn Box.count(`: the declared name is `Box.count`
  if (K >= 1 && FnWords.count(Toks[K - 1]))
    return true;
  int Depth = 0;
  size_t Close = I + 1;
  for (; Close < Toks.size(); ++Close) {
    Depth += Toks[Close] == "(" ? 1 : Toks[Close] == ")" ? -1 : 0;
    if (Depth == 0)
      break;
  }
  if (Close + 1 >= Toks.size())
    return false;
  const std::string &Next = Toks[Close + 1];
  if (Next == ":" || Next == "->")
    return true;
  if (Next != "{")
    return false;
  return startsStatement(K) ||
         (K >= 1 && identStart(Toks[K - 1][0]) && !notCallees().count(Toks[K - 1]) &&
          startsStatement(K - 1));
}

Counts count(const std::vector<std::string> &Toks) {
  Counts C;
  for (size_t I = 0; I < Toks.size(); ++I) {
    const std::string &T = Toks[I];
    if (T == "return")
      ++C.Returns;
    else if (isAssignment(T))
      ++C.Assignments;
    else if (identStart(T[0]) && I + 1 < Toks.size() && Toks[I + 1] == "(" &&
             !notCallees().count(T) && !declares(Toks, I)) {
      ++C.Calls;
      if (T.rfind("assert", 0) == 0)
        ++C.Asserts;
    }
  }
  return C;
}

int editDistance(const std::vector<std::string> &A, const std::vector<std::string> &B) {
  std::vector<int> Prev(B.size() + 1), Cur(B.size() + 1);
  for (size_t J = 0; J <= B.size(); ++J)
    Prev[J] = static_cast<int>(J);
  for (size_t I = 1; I <= A.size(); ++I) {
    Cur[0] = static_cast<int>(I);
    for (size_t J = 1; J <= B.size(); ++J)
      Cur[J] = std::min({Prev[J] + 1, Cur[J - 1] + 1, Prev[J - 1] + (A[I - 1] != B[J - 1])});
    std::swap(Prev, Cur);
  }
  return Prev[B.size()];
}

std::string squeeze(const std::string &S) {
  std::string Out, Last;
  for (const auto &T : tokenize(S)) {
    // Words apart, a space before `{` and after `,` or `:`: one spelling
    // for the same declaration however it was spaced.
    if (!Out.empty() && ((identChar(Out.back()) && identChar(T[0])) || T == "{" ||
                         Last == "," || Last == ":"))
      Out += ' ';
    Out += T;
    Last = T;
  }
  return Out;
}

// A declaration a fix may change: a function, struct, enum or constant
// header, or a field line of a struct (`Field` = the line is in a struct body).
bool isDeclaration(const std::vector<std::string> &Toks, bool Field) {
  size_t I = 0;
  while (I < Toks.size() && (Toks[I] == "pub" || Toks[I] == "static"))
    ++I;
  if (I >= Toks.size())
    return false;
  const std::string &K = Toks[I];
  if (K == "fn" || K == "struct" || K == "enum")
    return true;
  if (K == "immut" && I > 0 && Toks[0] == "pub")
    return true;
  return Field && identStart(K[0]) && I + 1 < Toks.size() && Toks[I + 1] == ":";
}

std::string joinWords(const std::vector<std::string> &Parts) {
  std::string Out;
  for (size_t I = 0; I < Parts.size(); ++I) {
    if (I > 0)
      Out += I + 1 == Parts.size() ? " and " : ", ";
    Out += Parts[I];
  }
  return Out;
}

} // namespace

std::vector<std::string> tokenize(const std::string &Text) {
  std::vector<std::string> Out;
  const size_t N = Text.size();
  size_t I = 0;
  while (I < N) {
    const char C = Text[I];
    if (std::isspace(static_cast<unsigned char>(C))) {
      ++I;
    } else if (C == '/' && I + 1 < N && Text[I + 1] == '/') {
      while (I < N && Text[I] != '\n')
        ++I;
    } else if (C == '"' || C == '\'') {
      size_t J = I + 1;
      while (J < N && Text[J] != C && Text[J] != '\n')
        J += Text[J] == '\\' ? 2 : 1;
      J = std::min(N, J + (J < N && Text[J] == C ? 1 : 0));
      Out.push_back(Text.substr(I, J - I));
      I = J;
    } else if (identStart(C) || std::isdigit(static_cast<unsigned char>(C))) {
      size_t J = I + 1;
      const bool Num = std::isdigit(static_cast<unsigned char>(C));
      while (J < N && (identChar(Text[J]) || (Num && Text[J] == '.')))
        ++J;
      Out.push_back(Text.substr(I, J - I));
      I = J;
    } else {
      static const char *Two[] = {"::", "->", "==", "!=", "<=", ">=", "&&", "||",
                                  "+=", "-=", "*=", "/=", "%="};
      std::string T(1, C);
      for (const char *P : Two)
        if (I + 1 < N && Text[I] == P[0] && Text[I + 1] == P[1])
          T = P;
      Out.push_back(T);
      I += T.size();
    }
  }
  return Out;
}

void describeEdit(Facts &F, const std::vector<std::string> &Before,
                  const std::vector<std::string> &After,
                  const std::string &ErrorLine, int ErrorIndex) {
  std::string B, A;
  for (const auto &L : Before)
    B += L + "\n";
  for (const auto &L : After)
    A += L + "\n";
  const auto BT = tokenize(B), AT = tokenize(A);
  F.TokenDistance = editDistance(BT, AT);
  F.LinesTouched = static_cast<int>(std::max(Before.size(), After.size()));
  F.SpanTokens = std::max<int>(1, static_cast<int>(tokenize(ErrorLine).size()));
  F.Wide = F.TokenDistance > std::max(kWideFloor, static_cast<int>(kWideRatio * F.SpanTokens));

  const Counts Was = count(BT), Is = count(AT);
  F.Lost.clear();
  if (Is.Asserts < Was.Asserts)
    F.Lost.push_back("an assert");
  if (Is.Calls - Is.Asserts < Was.Calls - Was.Asserts)
    F.Lost.push_back("a call");
  if (Is.Returns < Was.Returns)
    F.Lost.push_back("a `return`");
  if (Is.Assignments < Was.Assignments)
    F.Lost.push_back("an assignment");

  // A declaration among the replaced lines that does not read the same among
  // the new ones. A replaced line is taken for a struct field when it has the
  // `name: type` shape with no `;`, `=` or `(` (a statement has one of them).
  std::multiset<std::string> Now;
  for (const auto &L : After)
    Now.insert(squeeze(L));
  F.SurfaceChanges.clear();
  for (size_t I = 0; I < Before.size(); ++I) {
    if (static_cast<int>(I) == ErrorIndex)
      continue;
    const std::string &L = Before[I];
    const auto T = tokenize(L);
    const bool Field = std::find(T.begin(), T.end(), ";") == T.end() &&
                       std::find(T.begin(), T.end(), "=") == T.end() &&
                       std::find(T.begin(), T.end(), "(") == T.end();
    if (!isDeclaration(T, Field))
      continue;
    auto It = Now.find(squeeze(L));
    if (It != Now.end()) {
      Now.erase(It);
      continue;
    }
    std::string Shown = squeeze(L);
    if (!Shown.empty() && Shown.back() == '{')
      Shown.pop_back();
    while (!Shown.empty() && (Shown.back() == ' ' || Shown.back() == ','))
      Shown.pop_back();
    F.SurfaceChanges.push_back(Shown);
  }
}

std::vector<std::string> warningsInChangedText(const std::vector<std::string> &Before,
                                               const std::vector<std::string> &After,
                                               const std::vector<Warning> &Warnings) {
  std::vector<std::string> Out;
  for (const auto &W : Warnings) {
    if (W.Index < 0 || W.Index >= static_cast<int>(After.size()))
      continue;
    // Lines are paired by position when the fix keeps the line count;
    // otherwise every new line counts as written.
    if (Before.size() == After.size()) {
      const std::string &O = Before[W.Index], &N = After[W.Index];
      size_t Pre = 0;
      while (Pre < O.size() && Pre < N.size() && O[Pre] == N[Pre])
        ++Pre;
      size_t Suf = 0;
      while (Suf < O.size() - Pre && Suf < N.size() - Pre &&
             O[O.size() - 1 - Suf] == N[N.size() - 1 - Suf])
        ++Suf;
      const int From = static_cast<int>(Pre) + 1;
      const int To = std::max(From, static_cast<int>(N.size() - Suf));
      if (O == N || W.Col < From || W.Col > To)
        continue;
    }
    Out.push_back(W.Code.empty() ? "a warning" : W.Code);
  }
  return Out;
}

std::vector<std::string> flags(const Facts &F) {
  std::vector<std::string> Out;
  if (!F.Lost.empty())
    Out.push_back("it removes " + joinWords(F.Lost));
  for (const auto &S : F.SurfaceChanges)
    Out.push_back("it changes the declaration `" + S + "`");
  if (!F.Capability.empty())
    Out.push_back(F.Capability);
  if (!F.NewWarnings.empty())
    Out.push_back(std::string("it adds warning") + (F.NewWarnings.size() > 1 ? "s " : " ") +
                  joinWords(F.NewWarnings));
  if (F.Wide)
    Out.push_back("it changes " + std::to_string(F.TokenDistance) +
                  " tokens, more than the error's line holds");
  if (F.Candidates > 1)
    Out.push_back(std::to_string(F.Candidates) + " different fixes pass these checks");
  return Out;
}

Tier tier(const Facts &F) {
  if (!F.Valid)
    return Tier::Rejected;
  if (!flags(F).empty())
    return Tier::Plausible;
  return F.From == Provenance::Rule ? Tier::Proven : Tier::Verified;
}

const char *tierName(Tier T) {
  switch (T) {
  case Tier::Proven:
    return "proven";
  case Tier::Verified:
    return "verified";
  case Tier::Plausible:
    return "plausible";
  case Tier::Rejected:
    return "rejected";
  }
  return "rejected";
}

std::string flagLine(const Facts &F) {
  const auto Fl = flags(F);
  if (Fl.empty())
    return "";
  std::string Out = "Check before applying: ";
  for (size_t I = 0; I < Fl.size(); ++I)
    Out += (I ? "; " : "") + Fl[I];
  return Out + ".";
}

} // namespace FixScore
