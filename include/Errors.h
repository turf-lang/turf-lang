#ifndef ERRORS_H
#define ERRORS_H

#include "Algorithms.h"
#include "Codegen.h"
#include "Lexer.h"
#include "llvm/IR/Instructions.h"
#include <iostream>
#include <map>
#include <string>
#include <vector>

struct SourceLocation;

// Base Class
class TurfError {
protected:
  SourceLocation Loc;
  std::string Message;

public:
  TurfError(SourceLocation Loc, const std::string &Msg)
      : Loc(Loc), Message(Msg) {}

  virtual ~TurfError() = default;

  // The main function to display the error and exit
  void raise() const {
    LogErrorAt(Loc, Message);
    std::exit(1); // All errors currently stop compilation
  }
};

// Syntax Error : Parser issues
class SyntaxError : public TurfError {
public:
  SyntaxError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc, "Syntax Error: " + Msg) {}
};

// Keyword Error : Misspelled keywords
class KeywordError : public TurfError {
public:
  KeywordError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, "unknown keyword '" + Name + "'") {
    
    std::vector<std::string> Candidates;
    for (const auto &pair : Keywords) {
      if (getLevenshteinDistance(Name, pair.first) <= 2) {
        Candidates.push_back(pair.first);
      }
    }

    if (!Candidates.empty()) {
      Message += ". Maybe you meant: ";
      for (size_t i = 0; i < Candidates.size(); ++i) {
        Message += "'" + Candidates[i] + "'";
        if (i != Candidates.size() - 1)
          Message += ", ";
      }
      Message += "?";
    }
  }
};

// Arithmetic Error : Math issues
class ArithmeticError : public TurfError {
public:
  ArithmeticError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc, "Arithmetic Error: " + Msg) {}
};

// Reference Error : Variable lookup issues
class ReferenceError : public TurfError {
public:
  ReferenceError(SourceLocation Loc, const std::string &Name,
                 const std::map<std::string, VarInfo> &SymbolTable)
      : TurfError(Loc, "") {

    Message = "Unknown variable name: '" + Name + "'";

    if (Name.length() > 2) {
      std::vector<std::string> Candidates;

      for (const auto &pair : SymbolTable) {
        const std::string &KnownVar = pair.first;
        if (KnownVar == Name)
          continue;

        if (getLevenshteinDistance(Name, KnownVar) <= 2) {
          Candidates.push_back(KnownVar);
        }
      }

      if (!Candidates.empty()) {
        Message += ". Maybe you meant: ";
        for (size_t i = 0; i < Candidates.size(); ++i) {
          Message += "'" + Candidates[i] + "'";
          if (i != Candidates.size() - 1)
            Message += ", ";
        }
        Message += "?";
      }
    }
  }
};

#endif
