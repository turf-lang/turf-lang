#include "Lexer.h"
#include "Colors.h"
#include "SLMIntegration.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>

std::vector<std::string> SourceLines;
SourceLocation CurLoc = {1, 0};
int CurLine = 1;
int CurCol = 0;
static int LastChar = ' ';

// DiagnosticEngine : static storage
std::vector<DiagnosticEngine::Diag> DiagnosticEngine::Diagnostics;
std::set<int> DiagnosticEngine::ErrorLines;

// setjmp/longjmp recovery buffers
jmp_buf g_recoverJmp;
bool g_recoverActive = false;

// AI/SLM flag: only true when --smart is passed on the CLI.
bool g_AIEnabled = false;

// Set to true after the SLM handles the first error in a --smart pass.
// Any further errors are suppressed from SLM analysis because they are
// likely cascading side-effects of the first failure (ghost errors).
static bool g_slmFiredThisPass = false;

std::ifstream SourceFile;
double NumVal;
long long IntVal;
bool BoolVal;
std::string IdentifierStr;
std::string StringVal;

std::map<std::string, int> Keywords = {{"if", TOK_IF},
                                       {"then", TOK_THEN},
                                       {"else", TOK_ELSE},
                                       {"while", TOK_WHILE},
                                       {"int", TOK_TYPE_INT},
                                       {"float", TOK_TYPE_DOUBLE},
                                       {"double", TOK_TYPE_DOUBLE},
                                       {"bool", TOK_TYPE_BOOL},
                                       {"true", TOK_BOOL_LITERAL},
                                       {"false", TOK_BOOL_LITERAL},
                                       {"string", TOK_TYPE_STRING},
                                       {"void", TOK_TYPE_VOID},
                                       {"return", TOK_RETURN},
                                       {"fn", TOK_FN},
                                       {"break", TOK_BREAK},
                                       {"continue", TOK_CONTINUE},
                                       {"for", TOK_FOR},
                                       {"in", TOK_IN},
                                       {"step", TOK_STEP},
                                       {"elseif", TOK_ELSEIF}};

// Note: builtin function names (e.g. "print") are inserted here by
// RegisterBuiltins() at startup. See src/Builtins.cpp.

// Internal helper: physically print one diagnostic to stderr.
static void printDiag(const DiagnosticEngine::Diag &D) {
  bool isWarning = D.IsWarning || D.Msg.find("Warning:") != std::string::npos ||
                   D.Msg.find("warning:") != std::string::npos;
  std::string HeaderColor =
      isWarning ? Colors::BRIGHT_YELLOW : Colors::BRIGHT_RED;
  std::string HeaderText = isWarning ? "Warning" : "Oops! Something went wrong";

  std::cerr << Colors::BOLD << HeaderColor << HeaderText << " at line "
            << D.Loc.Line << ", column " << D.Loc.Col << ":" << Colors::RESET
            << "\n\n"
            << "  " << D.Msg << "\n\n";

  if (D.Loc.Line > 0 && D.Loc.Line <= static_cast<int>(SourceLines.size())) {
    const std::string &LineContent = SourceLines[D.Loc.Line - 1];

    std::cerr << Colors::BRIGHT_BLACK << "  " << D.Loc.Line << " | "
              << Colors::RESET << LineContent << "\n";

    std::string LineNumStr = std::to_string(D.Loc.Line);
    int CaretOffset = (D.Loc.Col > 0) ? D.Loc.Col - 1 : 0;
    std::cerr << Colors::BRIGHT_BLACK << "  "
              << std::string(LineNumStr.length(), ' ') << " | " << Colors::RESET
              << std::string(CaretOffset, ' ') << Colors::BOLD << HeaderColor
              << "^ Here!" << Colors::RESET << "\n\n";
  }
}

// DiagnosticEngine : method implementations
void DiagnosticEngine::add(SourceLocation Loc, const std::string &Msg,
                           bool IsWarning) {
  // If there is already an error on this line, suppress everything else
  // (both additional errors and warnings) to avoid cascading noise.
  if (ErrorLines.count(Loc.Line))
    return;

  std::string finalMessage = Msg;

  // Only invoke the SLM when the user explicitly opted in via --smart.
  // This keeps the default compilation path fully deterministic with
  // zero model-loading overhead.
  if (g_AIEnabled) {
    if (g_slmFiredThisPass) {
      // The SLM already handled the root error for this compilation pass.
      // Any subsequent errors are likely cascading side-effects of that
      // first failure (ghost errors that would disappear once the root
      // cause is fixed). Presenting AI suggestions on them would be
      // misleading and waste inference time.
      finalMessage +=
          "\n\n  " + Colors::BRIGHT_YELLOW +
          "[Smart Assist] This error may be a cascading effect of the one "
          "reported above." +
          Colors::RESET + "\n  " + Colors::BRIGHT_CYAN +
          "Fix the first issue and run --smart again for a fresh analysis." +
          Colors::RESET;
    } else {
      SLM::AISuggestion ai = SLM::fetchSuggestion(Msg, Loc.Line);

      if (ai.valid && ai.confidence >= 0.8f && ai.fix_mode != "NONE") {
        // ── High-confidence AI suggestion: REPLACE the compiler message ──
        g_slmFiredThisPass = true;
        finalMessage = Colors::BRIGHT_MAGENTA + "AI Suggestion: " +
                       ai.suggested_comment + Colors::RESET;

        if (!ai.explanation.empty()) {
          finalMessage += "\n  " + Colors::BRIGHT_CYAN +
                          "Explanation: " + ai.explanation + Colors::RESET;
        }

        if (ai.fix_mode == "MODIFY" && !ai.code_patch.empty()) {
          finalMessage += "\n  " + Colors::BRIGHT_GREEN +
                          "Suggested fix:\n  " + ai.code_patch + Colors::RESET;
        }

        if (!ai.warning.empty()) {
          finalMessage += "\n  " + Colors::BRIGHT_YELLOW +
                          "Note: " + ai.warning + Colors::RESET;
        }
      }
      // else: confidence < 0.8 or fix_mode == NONE → silent fallback.
      // finalMessage stays as the original compiler Msg.
    }
  }

  Diagnostics.push_back({Loc, finalMessage, IsWarning});

  if (!IsWarning)
    ErrorLines.insert(Loc.Line);
}

bool DiagnosticEngine::hasErrors() { return !ErrorLines.empty(); }

bool DiagnosticEngine::hasErrorAt(int Line) {
  return ErrorLines.count(Line) != 0;
}

void DiagnosticEngine::flushAll() {
  // Sort by line, then column
  std::sort(Diagnostics.begin(), Diagnostics.end(),
            [](const Diag &A, const Diag &B) {
              return A.Loc.Line < B.Loc.Line ||
                     (A.Loc.Line == B.Loc.Line && A.Loc.Col < B.Loc.Col);
            });

  for (const auto &D : Diagnostics)
    printDiag(D);

  Diagnostics.clear();
}

void DiagnosticEngine::reset() {
  Diagnostics.clear();
  ErrorLines.clear();
  g_slmFiredThisPass = false; // Reset cascade guard for the next compilation pass
}

// LogErrorAt : now defers to DiagnosticEngine so diagnostics are collected
// and de-duplicated.  Warnings emitted from lint still use this path.
void LogErrorAt(SourceLocation Loc, const std::string &Msg) {
  bool isWarning = Msg.find("Warning:") != std::string::npos ||
                   Msg.find("warning:") != std::string::npos;
  DiagnosticEngine::add(Loc, Msg, isWarning);
}

void resetLexer() {
  LastChar = ' ';
  CurLine = 1;
  CurCol = 0;
  CurLoc = {1, 0};
}

int gettok() {
  while (isspace(LastChar)) {
    // Handle newlines to track line numbers
    if (LastChar == '\n') {
      CurLine++;
      CurCol = 0;
    }

    LastChar = SourceFile.get();

    // Increment column for every character read
    if (LastChar != EOF) {
      CurCol++;
    }
  }

  // Snapshot the location before parsing the token body
  CurLoc = {CurLine, CurCol};

  // Identifiers of the types: id = [a-zA-Z][a-zA-Z0-9_]*
  if (isalpha(LastChar)) {
    IdentifierStr = LastChar;

    while (isalnum(LastChar = SourceFile.get()) || LastChar == '_') {
      IdentifierStr += LastChar;
      CurCol++;
    }

    // Check for keywords
    auto It = Keywords.find(IdentifierStr);
    if (It != Keywords.end()) {
      if (It->second == TOK_BOOL_LITERAL)
        BoolVal = (IdentifierStr == "true");
      return It->second;
    }

    return TOK_IDENTIFIER;
  }

  // String literals: "..."
  if (LastChar == '"') {
    StringVal = "";
    LastChar = SourceFile.get();
    CurCol++;

    while (LastChar != '"' && LastChar != EOF) {
      if (LastChar == '\\') {
        // Handle escape sequences
        LastChar = SourceFile.get();
        CurCol++;
        if (LastChar == 'n') {
          StringVal += '\n';
        } else if (LastChar == 't') {
          StringVal += '\t';
        } else if (LastChar == '\\') {
          StringVal += '\\';
        } else if (LastChar == '"') {
          StringVal += '"';
        } else {
          StringVal += LastChar;
        }
      } else {
        StringVal += LastChar;
      }
      LastChar = SourceFile.get();
      CurCol++;
    }

    if (LastChar == EOF) {
      LogErrorAt(CurLoc, "Unterminated string literal");
      return TOK_EOF;
    }

    // Consume closing "
    LastChar = SourceFile.get();
    CurCol++;

    return TOK_STRING_LITERAL;
  }

  // Range operator: .. (must come before number handler so start..end works)
  if (LastChar == '.' && SourceFile.peek() == '.') {
    LastChar = SourceFile.get();
    CurCol++;
    LastChar = SourceFile.get();
    CurCol++;
    return TOK_RANGE;
  }

  // Numbers: [0-9.]+ (a leading '.' only starts a number if followed by a
  // digit)
  if (isdigit(LastChar) || (LastChar == '.' && isdigit(SourceFile.peek()))) {
    std::string NumStr;
    do {
      NumStr += LastChar;
      LastChar = SourceFile.get();
      CurCol++;
    } while (isdigit(LastChar) ||
             (LastChar == '.' && SourceFile.peek() != '.'));

    bool IsFloat = NumStr.find('.') != std::string::npos;
    if (IsFloat) {
      NumVal = strtod(NumStr.c_str(), 0);
      return TOK_NUMBER;
    }

    IntVal = strtoll(NumStr.c_str(), nullptr, 10);
    return TOK_INT_LITERAL;
  }

  if (LastChar == '=') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_EQ;
    }

    return TOK_ASSIGN;
  }

  if (LastChar == '!') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_NEQ;
    }

    return '!';
  }

  if (LastChar == '>') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_GEQ;
    }

    return '>';
  }

  if (LastChar == '<') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_LEQ;
    }

    return '<';
  }

  if (LastChar == '&') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '&') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_AND;
    }

    return '&';
  }

  if (LastChar == '|') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '|') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_OR;
    }

    return '|';
  }

  if (LastChar == '+') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '+') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_PLUS_PLUS;
    }

    if (LastChar == '=') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_PLUS_ASSIGN;
    }

    return '+';
  }

  if (LastChar == '-') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '-') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_MINUS_MINUS;
    }

    if (LastChar == '=') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_MINUS_ASSIGN;
    }

    return '-';
  }

  if (LastChar == '*') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_MUL_ASSIGN;
    }

    return '*';
  }

  if (LastChar == '%') {
    LastChar = SourceFile.get();
    CurCol++;

    if (LastChar == '=') {
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_MOD_ASSIGN;
    }

    return '%';
  }

  if (LastChar == '/') {
    if (SourceFile.peek() == '/') {
      do {
        LastChar = SourceFile.get();
        CurCol++;
      } while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

      if (LastChar != EOF)
        return gettok();
    } else if (SourceFile.peek() == '=') {
      LastChar = SourceFile.get();
      CurCol++;
      LastChar = SourceFile.get();
      CurCol++;
      return TOK_DIV_ASSIGN;
    }
  }

  if (LastChar == EOF)
    return TOK_EOF;

  // Handle ASCII characters
  int ThisChar = LastChar;
  LastChar = SourceFile.get();
  CurCol++;

  return ThisChar;
}
