#include "diagnostics/LintLevels.h"

#include "diagnostics/ErrorCodes.h"

#include <cctype>
#include <map>

namespace LintLevels {

namespace {
std::map<std::string, Level> Explicit;
int PinnedMinor = -1; // -1: no pin
} // namespace

const char *name(Level L) {
  switch (L) {
  case Level::Allow:
    return "allow";
  case Level::Warn:
    return "warn";
  case Level::Deny:
    return "deny";
  }
  return "warn";
}

bool setLevel(const std::string &Code, Level L, std::string &Err) {
  std::string Upper = Code;
  for (char &C : Upper)
    C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));

  bool Shaped = Upper.size() == 4 && Upper[0] == 'E';
  for (size_t I = 1; Shaped && I < Upper.size(); ++I)
    Shaped = std::isdigit(static_cast<unsigned char>(Upper[I])) != 0;
  if (!Shaped) {
    Err = "'" + Code + "' is not a diagnostic code (a code is E and three "
          "digits, such as E163)";
    return false;
  }

  const ErrorCodeEntry *Entry = ErrorCodes::lookupByCode(Upper);
  if (!Entry) {
    Err = "unknown diagnostic code '" + Upper + "'";
    return false;
  }
  if (!ErrorCodes::isWarningCode(*Entry)) {
    // `deny` of an error is what it already is, so it is accepted and recorded
    // as nothing; lowering one is the mistake.
    if (L == Level::Deny)
      return true;
    Err = Upper + " is an error, not a warning: an error's level is always "
          "deny, so it cannot be set to " + name(L);
    return false;
  }
  Explicit[Upper] = L;
  return true;
}

bool setLintSet(const std::string &Spec, int CompilerMinor, std::string &Err) {
  const size_t Dot = Spec.find('.');
  bool Shaped = Dot != std::string::npos && Dot > 0 && Dot + 1 < Spec.size();
  for (size_t I = 0; Shaped && I < Spec.size(); ++I)
    Shaped = I == Dot || std::isdigit(static_cast<unsigned char>(Spec[I]));
  if (!Shaped) {
    Err = "'" + Spec + "' is not a lint set (write 2.N, such as 2.1)";
    return false;
  }
  if (Spec.size() > 9) { // a number this long names no release
    Err = "'" + Spec + "' is not a lint set this compiler has";
    return false;
  }
  const int Major = std::stoi(Spec.substr(0, Dot));
  const int Minor = std::stoi(Spec.substr(Dot + 1));
  if (Major != 2) {
    Err = "lint set '" + Spec + "' does not exist: only 2.N lint sets are "
          "defined";
    return false;
  }
  if (Minor > CompilerMinor) {
    Err = "lint set '" + Spec + "' is newer than this compiler (2." +
          std::to_string(CompilerMinor) + "): upgrade the compiler, or pin an "
          "older set";
    return false;
  }
  PinnedMinor = Minor;
  return true;
}

Level levelOfSince(const std::string &Code, int SinceMinor) {
  const auto It = Explicit.find(Code);
  if (It != Explicit.end())
    return It->second;
  if (PinnedMinor >= 0 && SinceMinor > PinnedMinor)
    return Level::Allow;
  return Level::Warn;
}

Level levelOf(const std::string &Code) {
  const ErrorCodeEntry *Entry = ErrorCodes::lookupByCode(Code);
  return levelOfSince(Code, Entry ? Entry->SinceMinor : 0);
}

bool anySet() { return !Explicit.empty() || PinnedMinor >= 0; }

void reset() {
  Explicit.clear();
  PinnedMinor = -1;
}

} // namespace LintLevels
