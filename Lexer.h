#ifndef LEXER_H
#define LEXER_H

#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct SourceLocation {
  int Line;
  int Col;
};

extern std::ifstream SourceFile;
extern double NumVal;
extern long long IntVal;
extern bool BoolVal;
extern std::string IdentifierStr;
extern std::string StringVal;

extern std::vector<std::string> SourceLines;
extern SourceLocation CurLoc;
extern const std::map<std::string, int> Keywords;

enum Token {
  TOK_EOF = -1,
  TOK_NUMBER = -2,
  TOK_IDENTIFIER = -3, // Variable
  TOK_ASSIGN = -4,
  TOK_THEN = -5,
  TOK_ELSE = -6,
  TOK_IF = -7,
  TOK_PRINT = -8,
  TOK_WHILE = -9,
  TOK_EQ = -10,
  TOK_NEQ = -11,
  TOK_LEQ = -12,
  TOK_GEQ = -13,
  TOK_INT_LITERAL = -20,
  TOK_BOOL_LITERAL = -21,
  TOK_TYPE_INT = -22,
  TOK_TYPE_DOUBLE = -23,
  TOK_TYPE_BOOL = -24,
  TOK_STRING_LITERAL = -25,
  TOK_TYPE_STRING = -26,
  TOK_AND = -27,
  TOK_OR = -28
};

int gettok();
void LogErrorAt(SourceLocation Loc, const std::string &Msg);

#endif
