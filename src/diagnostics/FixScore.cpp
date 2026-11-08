// SL-1: the Fix Score (FixScore.h).
#include "diagnostics/FixScore.h"
#include "diagnostics/ErrorCodes.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
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

std::string joinLines(const std::vector<std::string> &Lines) {
  std::string Out;
  for (const auto &L : Lines)
    Out += L + "\n";
  return Out;
}

// A declaration as describeProgram() compares them. `Owner` is the struct a
// field line belongs to ("" for anything else).
struct Decl {
  std::string Text, Owner;
  int Line = 0;
  bool Pub = false;
};

std::string shownDeclaration(const std::string &Line) {
  std::string Shown = squeeze(Line);
  if (!Shown.empty() && Shown.back() == '{')
    Shown.pop_back();
  while (!Shown.empty() && (Shown.back() == ' ' || Shown.back() == ','))
    Shown.pop_back();
  return Shown;
}

// The declarations of a file, line by line: function, struct, enum and
// constant headers, and the field lines of a struct's body. A line is taken
// for a field when it has the `name: type` shape with no `;`, `=` or `(` (a
// statement has one of them) and sits directly in a struct's braces.
std::vector<Decl> declarations(const std::vector<std::string> &Lines) {
  std::vector<Decl> Out;
  int Depth = 0;
  std::vector<std::pair<int, std::string>> Structs; // body depth, name
  for (size_t I = 0; I < Lines.size(); ++I) {
    const auto T = tokenize(Lines[I]);
    const bool InStruct = !Structs.empty() && Structs.back().first == Depth;
    const bool FieldShape = std::find(T.begin(), T.end(), ";") == T.end() &&
                            std::find(T.begin(), T.end(), "=") == T.end() &&
                            std::find(T.begin(), T.end(), "(") == T.end();
    if (isDeclaration(T, InStruct && FieldShape))
      Out.push_back({shownDeclaration(Lines[I]), InStruct ? Structs.back().second : "",
                     static_cast<int>(I) + 1, !T.empty() && T[0] == "pub"});
    // A struct header opens a body at its first `{`.
    std::string StructName;
    size_t K = 0;
    while (K < T.size() && T[K] == "pub")
      ++K;
    if (K + 1 < T.size() && T[K] == "struct")
      StructName = T[K + 1];
    bool Opened = false;
    for (const auto &Tok : T) {
      if (Tok == "{") {
        ++Depth;
        if (!StructName.empty() && !Opened)
          Structs.push_back({Depth, StructName});
        Opened = true;
      } else if (Tok == "}") {
        if (!Structs.empty() && Structs.back().first == Depth)
          Structs.pop_back();
        Depth = std::max(0, Depth - 1);
      }
    }
  }
  return Out;
}

// The top-level item each line belongs to: [first line, next item's first
// line), as the analysis bounds a function. An item starts at depth 0 with a
// line whose first character is not a blank or `}` and that is not a comment.
std::vector<std::pair<int, int>> topLevelItems(const std::vector<std::string> &Lines) {
  std::vector<int> Starts;
  int Depth = 0;
  for (size_t I = 0; I < Lines.size(); ++I) {
    const std::string &L = Lines[I];
    const auto T = tokenize(L);
    if (Depth == 0 && !T.empty() && !L.empty() && !std::isspace(static_cast<unsigned char>(L[0])) &&
        L[0] != '}')
      Starts.push_back(static_cast<int>(I) + 1);
    for (const auto &Tok : T)
      Depth = Tok == "{" ? Depth + 1 : Tok == "}" ? std::max(0, Depth - 1) : Depth;
  }
  std::vector<std::pair<int, int>> Out;
  for (size_t I = 0; I < Starts.size(); ++I)
    Out.push_back({Starts[I], I + 1 < Starts.size() ? Starts[I + 1]
                                                    : static_cast<int>(Lines.size()) + 1});
  return Out;
}

// What a program reaches for (capabilityDiff()).
struct Reach {
  int Unsafe = 0, Extern = 0, RawPointers = 0, MemCalls = 0;
  std::set<std::string> Imports; // the first word of each `import`
};

Reach reachOf(const std::vector<std::string> &Lines) {
  static const std::set<std::string> AfterPointer = {")", ",", ";", "=", ">",
                                                     "{", "]", "*"};
  Reach R;
  const auto T = tokenize(joinLines(Lines));
  for (size_t I = 0; I < T.size(); ++I) {
    const std::string &Tok = T[I];
    const std::string Prev = I > 0 ? T[I - 1] : "";
    const std::string Next = I + 1 < T.size() ? T[I + 1] : "";
    if (Tok == "unsafe")
      ++R.Unsafe;
    else if (Tok == "extern")
      ++R.Extern;
    else if (Tok == "import" && !Next.empty() && identStart(Next[0]))
      R.Imports.insert(Next);
    else if (Tok == "mem" && Next == "::")
      ++R.MemCalls;
    // `ptr` written as a type, or a type followed by `*` (`int*`, `Box<T>*`)
    // where a product would have an operand next.
    else if ((Tok == "ptr" && (Prev == ":" || Prev == "->" || Prev == "<")) ||
             (Tok == "*" && !Prev.empty() && (identChar(Prev.back()) || Prev == ">") &&
              (Next.empty() || AfterPointer.count(Next))))
      ++R.RawPointers;
  }
  return R;
}

// The words between backticks in a diagnostic's message.
std::set<std::string> quotedWords(const std::string &Message) {
  std::set<std::string> Out;
  size_t At = 0;
  while ((At = Message.find('`', At)) != std::string::npos) {
    const size_t Close = Message.find('`', At + 1);
    if (Close == std::string::npos)
      break;
    for (const auto &T : tokenize(Message.substr(At + 1, Close - At - 1)))
      if (identStart(T[0]))
        Out.insert(T);
    At = Close + 1;
  }
  return Out;
}

std::set<std::string> words(const std::vector<std::string> &Lines) {
  std::set<std::string> Out;
  for (const auto &T : tokenize(joinLines(Lines)))
    if (identStart(T[0]))
      Out.insert(T);
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
                  const std::string &ErrorLine) {
  const auto BT = tokenize(joinLines(Before)), AT = tokenize(joinLines(After));
  F.TokenDistance = editDistance(BT, AT);
  F.LinesTouched = static_cast<int>(std::max(Before.size(), After.size()));
  F.SpanTokens = std::max<int>(1, static_cast<int>(tokenize(ErrorLine).size()));
  F.Wide = F.TokenDistance > std::max(kWideFloor, static_cast<int>(kWideRatio * F.SpanTokens));
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

std::vector<std::string> capabilityDiff(const std::vector<std::string> &Before,
                                        const std::vector<std::string> &After) {
  static const char *System[] = {"os", "fs", "http", "io"};
  const Reach Was = reachOf(Before), Is = reachOf(After);
  std::vector<std::string> Out;
  if (Is.Unsafe > Was.Unsafe)
    Out.push_back("it adds `unsafe`");
  if (Is.Extern > Was.Extern)
    Out.push_back("it adds an `extern` declaration");
  for (const char *M : System)
    if (Is.Imports.count(M) && !Was.Imports.count(M))
      Out.push_back(std::string("it imports `") + M + "`");
  if (Is.RawPointers > Was.RawPointers)
    Out.push_back("it adds a raw pointer");
  if (Is.MemCalls > Was.MemCalls)
    Out.push_back("it adds a `mem::` call");
  return Out;
}

void describeProgram(Facts &F, const Program &P) {
  const int NB = static_cast<int>(P.Before.size());
  const int NA = static_cast<int>(P.After.size());
  const int Replaced = P.End - P.Start + 1;
  const int Written = NA - NB + Replaced;
  if (P.Start < 1 || P.End < P.Start || P.End > NB || Written < 0)
    return;
  const std::vector<std::string> SpanB(P.Before.begin() + (P.Start - 1),
                                       P.Before.begin() + P.End);
  const std::vector<std::string> SpanA(P.After.begin() + (P.Start - 1),
                                       P.After.begin() + (P.Start - 1 + Written));
  describeEdit(F, SpanB, SpanA,
               P.ErrorLine >= 1 && P.ErrorLine <= NB ? P.Before[P.ErrorLine - 1]
                                                     : std::string());

  // Deletion, over the whole file.
  const Counts Was = count(tokenize(joinLines(P.Before)));
  const Counts Is = count(tokenize(joinLines(P.After)));
  F.Lost.clear();
  if (Is.Asserts < Was.Asserts)
    F.Lost.push_back("an assert");
  if (Is.Calls - Is.Asserts < Was.Calls - Was.Asserts)
    F.Lost.push_back("a call");
  if (Is.Returns < Was.Returns)
    F.Lost.push_back("a `return`");
  if (Is.Assignments < Was.Assignments)
    F.Lost.push_back("an assignment");

  // API surface: a declaration that does not read the same after the fix, and
  // a `pub` item or a field of an existing struct the fix adds. The
  // declaration on the error's line may change; what it became is not an
  // addition either.
  const auto DB = declarations(P.Before), DA = declarations(P.After);
  auto key = [](const Decl &D) { return D.Owner + "\x1f" + D.Text; };
  std::multiset<std::string> Now, Had;
  for (const auto &D : DA)
    Now.insert(key(D));
  std::set<std::string> Structs;
  for (const auto &D : DB) {
    Had.insert(key(D));
    if (D.Owner.empty()) {
      const auto T = tokenize(D.Text);
      for (size_t I = 0; I + 1 < T.size(); ++I)
        if (T[I] == "struct")
          Structs.insert(T[I + 1]);
    }
  }
  F.SurfaceChanges.clear();
  bool ErrorDeclChanged = false;
  for (const auto &D : DB) {
    auto It = Now.find(key(D));
    if (It != Now.end()) {
      Now.erase(It);
      continue;
    }
    if (D.Line == P.ErrorLine)
      ErrorDeclChanged = true;
    else
      F.SurfaceChanges.push_back(D.Text);
  }
  std::vector<Decl> Added;
  // The error's line as the fix wrote it (same offset into the written
  // lines): a declaration there is the error's own, whatever it was before
  // (`export immut` -> `pub immut`).
  const int ErrorLineAfter = P.ErrorLine >= P.Start && P.ErrorLine <= P.End &&
                                     P.ErrorLine - P.Start < Written
                                 ? P.ErrorLine
                                 : -1;
  for (const auto &D : DA) {
    auto It = Had.find(key(D));
    if (It != Had.end())
      Had.erase(It);
    else if (D.Line != ErrorLineAfter)
      Added.push_back(D);
  }
  if (ErrorDeclChanged && !Added.empty()) {
    auto Nearest = std::min_element(Added.begin(), Added.end(), [&](const Decl &A, const Decl &B) {
      return std::abs(A.Line - P.ErrorLine) < std::abs(B.Line - P.ErrorLine);
    });
    Added.erase(Nearest);
  }
  F.SurfaceAdded.clear();
  for (const auto &D : Added) {
    if (!D.Owner.empty() && Structs.count(D.Owner))
      F.SurfaceAdded.push_back("it adds the field `" + D.Text + "` to `" + D.Owner + "`");
    else if (D.Owner.empty() && D.Pub)
      F.SurfaceAdded.push_back("it adds the public declaration `" + D.Text + "`");
  }

  F.Capabilities = capabilityDiff(P.Before, P.After);

  // The warning delta. In the text the fix wrote: SL-1's rule.
  const int FirstNew = P.Start, LastNew = P.Start + Written - 1;
  std::vector<std::string> HadHere;
  for (const auto &D : P.BeforeDiags)
    if (!D.Error && D.Line >= P.Start && D.Line <= P.End)
      HadHere.push_back(D.Code);
  std::vector<Warning> AddedHere;
  for (const auto &D : P.AfterDiags) {
    if (D.Error || D.Line < FirstNew || D.Line > LastNew)
      continue;
    auto It = std::find(HadHere.begin(), HadHere.end(), D.Code);
    if (It != HadHere.end())
      HadHere.erase(It);
    else
      AddedHere.push_back({D.Line - FirstNew, D.Col, D.Code});
  }
  F.NewWarnings = warningsInChangedText(SpanB, SpanA, AddedHere);

  // Elsewhere: a warning the original did not have on the same line (lines
  // after the fix are moved back by what it added). The original's analysis
  // skipped every function with an error in it, and every flow check after a
  // syntax error, so a warning there counts only when it names a word the fix
  // removed or added.
  bool SyntaxError = false;
  std::vector<int> ErrorLines;
  for (const auto &D : P.BeforeDiags)
    if (D.Error) {
      ErrorLines.push_back(D.Line);
      const ErrorCodeEntry *Row = ErrorCodes::lookupByCode(D.Code);
      SyntaxError |= Row && Row->Category == SLM::ErrorCategory::SYNTAX_ERROR;
    }
  const auto Items = topLevelItems(P.Before);
  auto analysedInFull = [&](int Line) {
    if (SyntaxError)
      return false;
    for (const auto &[From, To] : Items)
      if (Line >= From && Line < To)
        return std::none_of(ErrorLines.begin(), ErrorLines.end(),
                            [&](int E) { return E >= From && E < To; });
    return true;
  };
  std::set<std::string> Touched;
  {
    const auto WB = words(SpanB), WA = words(SpanA);
    std::set_symmetric_difference(WB.begin(), WB.end(), WA.begin(), WA.end(),
                                  std::inserter(Touched, Touched.end()));
  }
  std::multiset<std::pair<int, std::string>> HadElsewhere;
  for (const auto &D : P.BeforeDiags)
    if (!D.Error)
      HadElsewhere.insert({D.Line, D.Code});
  const int Shift = Written - Replaced;
  for (const auto &D : P.AfterDiags) {
    if (D.Error || (D.Line >= FirstNew && D.Line <= LastNew))
      continue;
    const int Was = D.Line < P.Start ? D.Line : D.Line - Shift;
    auto It = HadElsewhere.find({Was, D.Code});
    if (It != HadElsewhere.end()) {
      HadElsewhere.erase(It);
      continue;
    }
    bool Named = false;
    for (const auto &W : quotedWords(D.Message))
      Named |= Touched.count(W) > 0;
    if (Named || analysedInFull(Was))
      F.NewWarnings.push_back((D.Code.empty() ? std::string("a warning") : D.Code) +
                              " (line " + std::to_string(D.Line) + ")");
  }
}

std::vector<std::string> flags(const Facts &F) {
  std::vector<std::string> Out;
  if (!F.Lost.empty())
    Out.push_back("it removes " + joinWords(F.Lost));
  for (const auto &S : F.SurfaceChanges)
    Out.push_back("it changes the declaration `" + S + "`");
  Out.insert(Out.end(), F.SurfaceAdded.begin(), F.SurfaceAdded.end());
  Out.insert(Out.end(), F.Capabilities.begin(), F.Capabilities.end());
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
