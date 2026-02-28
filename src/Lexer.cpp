#include "Lexer.h"

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>

std::vector<std::string> SourceLines;
SourceLocation CurLoc = {1, 0};
int CurLine = 1;
int CurCol = 0;
static int LastChar = ' ';

std::ifstream SourceFile;
double NumVal;
long long IntVal;
bool BoolVal;
std::string IdentifierStr;
std::string StringVal;

std::map<std::string, int> Keywords = {
    {"if", TOK_IF},          {"then", TOK_THEN},
    {"else", TOK_ELSE},      {"while", TOK_WHILE},
    {"int", TOK_TYPE_INT},   {"float", TOK_TYPE_DOUBLE},
    {"double", TOK_TYPE_DOUBLE},
    {"bool", TOK_TYPE_BOOL}, {"true", TOK_BOOL_LITERAL},
    {"false", TOK_BOOL_LITERAL},
    {"string", TOK_TYPE_STRING},
    {"void", TOK_TYPE_VOID},
    {"return", TOK_RETURN},
    {"fn", TOK_FN},
    {"break", TOK_BREAK},
    {"continue", TOK_CONTINUE}};

// Note: builtin function names (e.g. "print") are inserted here by
// RegisterBuiltins() at startup. See src/Builtins.cpp.

void LogErrorAt(SourceLocation Loc, const std::string &Msg) {
  std::cerr << "Error at " << Loc.Line << ":" << Loc.Col << ": " << Msg << "\n";

  if (Loc.Line > 0 && Loc.Line <= SourceLines.size()) {
    std::string LineContent = SourceLines[Loc.Line - 1];
    std::cerr << "  " << Loc.Line << " | " << LineContent << "\n";

    // Calculate indentation for the caret
    std::string LineNumStr = std::to_string(Loc.Line);
    std::cerr << "  " << std::string(LineNumStr.length(), ' ') << " | "
              << std::string(Loc.Col - 1, ' ') << "^" << "\n";
  }
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

  // Numbers: [0-9.]+
  if (isdigit(LastChar) || LastChar == '.') {
    std::string NumStr;
    do {
      NumStr += LastChar;
      LastChar = SourceFile.get();
      CurCol++;
    } while (isdigit(LastChar) || LastChar == '.');

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
