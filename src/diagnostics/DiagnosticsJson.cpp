/**
 * @file DiagnosticsJson.cpp
 * @brief DX-2: the JSON form of a diagnostic (DiagnosticsJson.h).
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 */

#include "diagnostics/DiagnosticsJson.h"
#include "diagnostics/ErrorCodes.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>

namespace DiagnosticsJson {

bool Enabled = false;

namespace {

// The text form colours parts of a message; the JSON form carries the words.
std::string stripAnsi(const std::string &S) {
  std::string Out;
  for (size_t I = 0; I < S.size(); ++I) {
    if (S[I] == '\x1b' && I + 1 < S.size() && S[I + 1] == '[') {
      I += 2;
      while (I < S.size() && !std::isalpha(static_cast<unsigned char>(S[I])))
        ++I;
      continue;
    }
    Out += S[I];
  }
  return Out;
}

std::string trim(const std::string &S) {
  size_t B = 0, E = S.size();
  while (B < E && std::isspace(static_cast<unsigned char>(S[B])))
    ++B;
  while (E > B && std::isspace(static_cast<unsigned char>(S[E - 1])))
    --E;
  return S.substr(B, E - B);
}

// A span: 1-based line and column, `end` exclusive. A location with no
// recorded end (most multi-token ones) is a span of its first character.
llvm::json::Object span(const SourceLocation &Loc) {
  int EndLine = Loc.Line, EndCol = Loc.Col + 1;
  if (Loc.EndLine > 0 && Loc.EndCol > 0 &&
      (Loc.EndLine > Loc.Line || Loc.EndCol > Loc.Col)) {
    EndLine = Loc.EndLine;
    EndCol = Loc.EndCol;
  }
  return llvm::json::Object{
      {"start", llvm::json::Object{{"line", Loc.Line}, {"column", Loc.Col}}},
      {"end", llvm::json::Object{{"line", EndLine}, {"column", EndCol}}}};
}

// A file as the compiler opened it, without `dir/..` steps
// (`build/../lib/std/x.tr` is `lib/std/x.tr` under the same root).
std::string cleanPath(const std::string &Path) {
  llvm::SmallString<256> P(Path);
  llvm::sys::path::remove_dots(P, /*remove_dot_dot=*/true);
  return P.str().str();
}

llvm::json::Value orNull(const std::string &S) {
  if (S.empty())
    return nullptr;
  return S;
}

} // namespace

std::string render(const DiagnosticEngine::Diag &D, bool IsWarning) {
  // A message is "Title!\n  text", or just text; a raw parser error may carry
  // its hint inside the message ("...\n  Hint: ...").
  std::string Msg = stripAnsi(D.Msg);
  std::string Hint = trim(stripAnsi(D.Hint));
  if (const size_t H = Msg.find("Hint:"); Hint.empty() && H != std::string::npos) {
    Hint = trim(Msg.substr(H + 5));
    Msg = Msg.substr(0, H);
  }
  std::string Title;
  Msg = trim(Msg);
  // "Warning: ..." repeats `severity`.
  if (Msg.rfind("Warning: ", 0) == 0)
    Msg = Msg.substr(9);
  if (const size_t NL = Msg.find('\n'); NL != std::string::npos) {
    const std::string First = trim(Msg.substr(0, NL));
    // "Nothing to infer!", or a lead-in such as "Oops! ...:".
    if (!First.empty() && (First.back() == '!' || First.back() == ':')) {
      Title = First;
      Msg = trim(Msg.substr(NL + 1));
    }
  }

  std::string Category;
  if (const ErrorCodeEntry *Row = ErrorCodes::lookupByCode(D.Code)) {
    Category = SLM::getCategoryName(Row->Category);
    for (char &C : Category)
      C = C == '_' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
  }

  llvm::json::Array Related;
  for (const auto &R : D.RelatedLocations)
    Related.push_back(llvm::json::Object{
        {"file", cleanPath(R.Loc.Filename.empty() ? D.Filename : R.Loc.Filename)},
        {"span", span(R.Loc)},
        {"message", stripAnsi(R.Description)}});

  llvm::json::Value Fix = nullptr;
  if (!D.Fix.empty()) {
    llvm::json::Array Edits;
    for (const auto &E : D.Fix.Edits)
      Edits.push_back(llvm::json::Object{
          {"file", cleanPath(D.Filename)},
          {"span",
           llvm::json::Object{
               {"start", llvm::json::Object{{"line", E.Line}, {"column", E.StartCol}}},
               {"end", llvm::json::Object{{"line", E.Line}, {"column", E.EndCol}}}}},
          {"replacement", E.Replacement}});
    Fix = llvm::json::Object{{"description", D.Fix.Description},
                             {"edits", std::move(Edits)}};
  }

  llvm::json::Object Obj{
      {"version", SchemaVersion},
      {"severity", IsWarning ? "warning" : "error"},
      {"code", orNull(D.Code)},
      {"category", orNull(Category)},
      {"title", orNull(Title)},
      {"message", Msg},
      {"hint", orNull(Hint)},
      {"file", cleanPath(D.Filename)},
      {"span", D.Loc.Line > 0 ? llvm::json::Value(span(D.Loc)) : llvm::json::Value(nullptr)},
      {"related", std::move(Related)},
      {"fix", std::move(Fix)},
      {"denied", D.Denied}};

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << llvm::json::Value(std::move(Obj));
  return OS.str();
}

} // namespace DiagnosticsJson
