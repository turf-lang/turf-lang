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
// Mutable so RegisterBuiltins() can insert builtin names at startup.
extern std::map<std::string, int> Keywords;

enum Token {
  TOK_EOF = -1,
  TOK_NUMBER = -2,
  TOK_IDENTIFIER = -3, // Variable
  TOK_ASSIGN = -4,
  TOK_THEN = -5,
  TOK_ELSE = -6,
  TOK_IF = -7,
  TOK_WHILE = -8,
  TOK_EQ = -9,
  TOK_NEQ = -10,
  TOK_LEQ = -11,
  TOK_GEQ = -12,
  TOK_INT_LITERAL = -13,
  TOK_BOOL_LITERAL = -14,
  TOK_TYPE_INT = -15,
  TOK_TYPE_DOUBLE = -16,
  TOK_TYPE_BOOL = -17,
  TOK_STRING_LITERAL = -18,
  TOK_TYPE_STRING = -19,
  TOK_AND = -20,
  TOK_OR = -21,
  TOK_CONV_INT = -22,   // for int(...)
  TOK_CONV_DOUBLE = -23,   // for double(...)
  TOK_TYPE_VOID = -24,
  TOK_RETURN = -25,
  TOK_FN = -26,
  TOK_BREAK = -27,
  TOK_CONTINUE = -28,
  TOK_PLUS_ASSIGN = -29,
  TOK_MINUS_ASSIGN = -30,
  TOK_MUL_ASSIGN = -31,
  TOK_DIV_ASSIGN = -32,
  TOK_MOD_ASSIGN = -33,
  TOK_PLUS_PLUS = -34,
  TOK_MINUS_MINUS = -35,
  TOK_FOR = -36,
  TOK_IN = -37,
  TOK_STEP = -38,
  TOK_RANGE = -39,
};

int gettok();
void resetLexer();
void LogErrorAt(SourceLocation Loc, const std::string &Msg);

#endif
