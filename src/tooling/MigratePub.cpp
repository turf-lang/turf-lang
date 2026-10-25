/**
 * @file MigratePub.cpp
 * @brief `turf --migrate=pub` -- see MigratePub.h for what it does and why.
 */

#include "tooling/MigratePub.h"
#include "frontend/Lexer.h"
#include "frontend/Parser.h"

#include <algorithm>
#include <sstream>

namespace turf::migrate {

namespace {

struct Tok {
  int T;
  int Line;
  int Col;
};

/// Tokenizes @p Source with the real lexer (the same getNextToken() the
/// parser and the formatter use), on a lexer of its own, and saving and
/// restoring the parser state it touches so the tool is safe to run in-process
/// beside the rest of the compiler.
std::vector<Tok> tokenize(std::string_view Source) {
  std::string SavedSourceFilename = SourceFilename;
  int SavedCurTok = CurTok;

  std::istringstream Buffer{std::string(Source)};
  LexerState Lexer(Buffer, "<migrate>");
  LexerScope Scope(Lexer);
  SourceFilename = "<migrate>";
  resetLexer();
  CurTok = 0;
  getNextToken();

  std::vector<Tok> Toks;
  while (CurTok != TOK_EOF) {
    Toks.push_back({CurTok, Lexer.CurLoc.Line, Lexer.CurLoc.Col});
    getNextToken();
  }

  SourceFilename = SavedSourceFilename;
  CurTok = SavedCurTok;
  return Toks;
}

enum class Ctx {
  Other,     ///< a function body, enum/machine/error body, `matches` arm, ...
  Struct,    ///< a struct's own `{ }` -- fields live here
  UnsafeTop, ///< a top-level `unsafe { }` block, which the parser flattens
};

bool isDeclKeyword(int T) {
  return T == TOK_FN || T == TOK_STRUCT || T == TOK_ENUM || T == TOK_MACHINE ||
         T == TOK_ERROR || T == TOK_EXTERN;
}

/// The insertion points in @p Toks, in token order.
std::vector<Insertion> findInsertions(const std::vector<Tok> &Toks) {
  std::vector<Insertion> Out;
  std::vector<Ctx> Stack;
  int ParenDepth = 0;
  bool PendingStruct = false; // saw `struct`, its `{` not yet
  bool InExtern = false;      // between `extern` and its `;` -- its own `fn` is not a declaration

  // "Declaration level": not inside any function body, struct body or other
  // block -- except a top-level `unsafe { }`, which the parser flattens into
  // the file, so what is inside it is top level too.
  auto atDeclLevel = [&] {
    for (Ctx C : Stack)
      if (C != Ctx::UnsafeTop)
        return false;
    return ParenDepth == 0;
  };

  for (size_t I = 0; I < Toks.size(); ++I) {
    const int T = Toks[I].T;
    const int Prev = I ? Toks[I - 1].T : 0;
    const int Next = I + 1 < Toks.size() ? Toks[I + 1].T : 0;

    if (T == '{') {
      Ctx C = Ctx::Other;
      if (PendingStruct && atDeclLevel()) {
        C = Ctx::Struct;
        PendingStruct = false;
      } else if (Prev == TOK_UNSAFE && atDeclLevel()) {
        C = Ctx::UnsafeTop;
      }
      Stack.push_back(C);
      continue;
    }
    if (T == '}') {
      if (!Stack.empty())
        Stack.pop_back();
      ParenDepth = 0; // a stray '(' must not poison the rest of the file
      continue;
    }
    if (T == '(' || T == '[') {
      ++ParenDepth;
      continue;
    }
    if (T == ')' || T == ']') {
      ParenDepth = std::max(0, ParenDepth - 1);
      continue;
    }
    if (T == ';') {
      PendingStruct = false;
      InExtern = false;
      continue;
    }

    // A struct field: `name:` right after the body's `{` or a `,`. The
    // previous-token test also skips a field that already says `pub`
    // (its previous token is then TOK_PUB).
    if (!Stack.empty() && Stack.back() == Ctx::Struct) {
      if (T == TOK_IDENTIFIER && (Prev == '{' || Prev == ',') && Next == TOK_COLON)
        Out.push_back({Toks[I].Line, Toks[I].Col, "field"});
      continue;
    }

    if (!atDeclLevel() || !isDeclKeyword(T))
      continue;
    if (T == TOK_FN && Next == '(')
      continue; // a function TYPE (`fn(int): int`), not a declaration
    if (T == TOK_FN && InExtern)
      continue; // `extern fn f(...)` / `extern from "m" fn f(...)`: one declaration
    if (T == TOK_STRUCT)
      PendingStruct = true;
    if (T == TOK_EXTERN)
      InExtern = true;
    if (Prev != TOK_PUB)
      Out.push_back({Toks[I].Line, Toks[I].Col, ""});
  }
  return Out;
}

/// The declaration head at (@p Line, @p Col): the rest of that source line up
/// to the first `(`, `{`, `;`, `:` or `<`, trimmed. For the report only.
std::string headAt(const std::vector<std::string> &Lines, int Line, int Col) {
  if (Line < 1 || Line > static_cast<int>(Lines.size()))
    return "";
  const std::string &L = Lines[Line - 1];
  if (Col < 1 || Col > static_cast<int>(L.size()) + 1)
    return "";
  std::string Rest = L.substr(Col - 1);
  size_t End = Rest.find_first_of("({;:<");
  if (End != std::string::npos)
    Rest = Rest.substr(0, End);
  while (!Rest.empty() && (Rest.back() == ' ' || Rest.back() == '\t' || Rest.back() == '\r'))
    Rest.pop_back();
  return Rest;
}

} // namespace

MigrateResult migratePub(std::string_view Source) {
  MigrateResult Result;
  Result.Text = std::string(Source);

  std::vector<Insertion> Points = findInsertions(tokenize(Source));
  if (Points.empty())
    return Result;

  // Split into lines by '\n' only, so a '\r' stays attached to its line and a
  // CRLF file comes back CRLF.
  std::vector<std::string> Lines;
  {
    size_t Start = 0;
    while (true) {
      size_t Nl = Source.find('\n', Start);
      if (Nl == std::string_view::npos) {
        Lines.emplace_back(Source.substr(Start));
        break;
      }
      Lines.emplace_back(Source.substr(Start, Nl - Start));
      Start = Nl + 1;
    }
  }

  for (auto &P : Points)
    if (P.What.empty())
      P.What = headAt(Lines, P.Line, P.Col);
    else if (P.What == "field")
      P.What = "field " + headAt(Lines, P.Line, P.Col);

  // Insert right to left within each line so earlier columns stay valid.
  std::vector<Insertion> Sorted = Points;
  std::sort(Sorted.begin(), Sorted.end(), [](const Insertion &A, const Insertion &B) {
    return A.Line != B.Line ? A.Line < B.Line : A.Col > B.Col;
  });
  for (const auto &P : Sorted) {
    if (P.Line < 1 || P.Line > static_cast<int>(Lines.size()))
      continue;
    std::string &L = Lines[P.Line - 1];
    const size_t At = static_cast<size_t>(std::max(P.Col - 1, 0));
    if (At <= L.size())
      L.insert(At, "pub ");
  }

  std::string Text;
  for (size_t I = 0; I < Lines.size(); ++I) {
    Text += Lines[I];
    if (I + 1 < Lines.size())
      Text += '\n';
  }
  Result.Text = std::move(Text);
  Result.Insertions = std::move(Points);
  return Result;
}

} // namespace turf::migrate
