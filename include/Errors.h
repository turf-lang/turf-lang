#ifndef ERRORS_H
#define ERRORS_H

#include "Algorithms.h"
#include "Builtins.h"
#include "Codegen.h"
#include "Lexer.h"
#include "Colors.h"
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
      : TurfError(Loc, Colors::BRIGHT_RED + "Oops! There's a tiny mistake here:\n" + Colors::RESET + "  " + Msg + "\n  " + Colors::BRIGHT_GREEN + "Hint: Check your spelling, punctuation like ';' or '}', and make sure everything is closed properly." + Colors::RESET) {}
};

// Keyword Error : Misspelled keywords
class KeywordError : public TurfError {
public:
  KeywordError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED + "Hmm, I don't know the word '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "'." + Colors::RESET) {
    
    std::vector<std::string> Candidates;
    for (const auto &pair : Keywords) {
      if (getLevenshteinDistance(Name, pair.first) <= 2) {
        Candidates.push_back(pair.first);
      }
    }

    if (!Candidates.empty()) {
      Message += "\n  " + Colors::BRIGHT_GREEN + "Did you mean: ";
      for (size_t i = 0; i < Candidates.size(); ++i) {
        Message += Colors::BRIGHT_YELLOW + "'" + Candidates[i] + "'" + Colors::BRIGHT_GREEN;
        if (i != Candidates.size() - 1)
          Message += ", ";
      }
      Message += "?" + Colors::RESET;
    } else {
        Message += "\n  " + Colors::BRIGHT_GREEN + "Hint: Check if you spelled the keyword correctly." + Colors::RESET;
    }
  }
};

// Arithmetic Error : Math issues
class ArithmeticError : public TurfError {
public:
  ArithmeticError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc, Colors::BRIGHT_RED + "Math trouble! " + Colors::RESET + Msg + "\n  " + Colors::BRIGHT_GREEN + "Hint: Are you trying to divide by zero? Or combining things that can't be added together like a number and a word?" + Colors::RESET) {}
};

// Reference Error : Variable lookup issues
class ReferenceError : public TurfError {
public:
  ReferenceError(SourceLocation Loc, const std::string &Name,
                 const std::map<std::string, VarInfo> &SymbolTable)
      : TurfError(Loc, "") {

    Message = Colors::BRIGHT_RED + "I can't find anything named '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "'." + Colors::RESET;

    bool foundCandidate = false;

    if (Name.length() > 2) {
      std::vector<std::string> Candidates;

      // Check variables in symbol table
      for (const auto &pair : SymbolTable) {
        const std::string &KnownVar = pair.first;
        if (KnownVar == Name)
          continue;

        if (getLevenshteinDistance(Name, KnownVar) <= 2) {
          Candidates.push_back(KnownVar);
        }
      }

      // Check built-in functions
      for (const auto &builtin : Builtins) {
        if (builtin.Name == Name)
          continue;

        if (getLevenshteinDistance(Name, builtin.Name) <= 2) {
          Candidates.push_back(builtin.Name);
        }
      }

      if (!Candidates.empty()) {
        foundCandidate = true;
        Message += "\n  " + Colors::BRIGHT_GREEN + "Did you mean: ";
        for (size_t i = 0; i < Candidates.size(); ++i) {
          Message += Colors::BRIGHT_YELLOW + "'" + Candidates[i] + "'" + Colors::BRIGHT_GREEN;
          if (i != Candidates.size() - 1)
            Message += ", ";
        }
        Message += "?" + Colors::RESET;
      }
    }
    
    if (!foundCandidate) {
        Message += "\n  " + Colors::BRIGHT_GREEN + "Hint: Did you forget to create this variable? You have to create it before you can use it, like this: `int " + Name + " = 5;`" + Colors::RESET;
    }
  }
};

// Semantic Error : Symbol binding issues
class UseBeforeDeclarationError : public TurfError {
public:
  UseBeforeDeclarationError(SourceLocation UseLoc, const std::string &Name, const std::vector<std::string> &KnownNames)
      : TurfError(UseLoc, "") {
      
    Message = Colors::BRIGHT_RED + "Hold on! You're trying to use '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "' before telling me what it is." + Colors::RESET;
    
    bool foundCandidate = false;
    
    if (Name.length() > 2) {
      std::vector<std::string> Candidates;
      
      // Check variables in the current scope chain
      for (const auto &KnownVar : KnownNames) {
        if (KnownVar == Name)
          continue;

        if (getLevenshteinDistance(Name, KnownVar) <= 2) {
          Candidates.push_back(KnownVar);
        }
      }
      
      // Check built-in functions
      for (const auto &builtin : Builtins) {
        if (builtin.Name == Name)
          continue;

        if (getLevenshteinDistance(Name, builtin.Name) <= 2) {
          Candidates.push_back(builtin.Name);
        }
      }

      if (!Candidates.empty()) {
        foundCandidate = true;
        Message += "\n  " + Colors::BRIGHT_GREEN + "Did you mean: ";
        for (size_t i = 0; i < Candidates.size(); ++i) {
          Message += Colors::BRIGHT_YELLOW + "'" + Candidates[i] + "'" + Colors::BRIGHT_GREEN;
          if (i != Candidates.size() - 1)
            Message += ", ";
        }
        Message += "?" + Colors::RESET;
      }
    }
    
    if (!foundCandidate) {
        Message += "\n  " + Colors::BRIGHT_GREEN + "Hint: Did you forget to create this variable? You have to create it before you can use it, like this: `int " + Name + " = 5;`" + Colors::RESET;
    }
  }
};

class DuplicateDeclarationError : public TurfError {
public:
  DuplicateDeclarationError(SourceLocation NewLoc, const std::string &Name,
                            SourceLocation PrevLoc)
      : TurfError(NewLoc, Colors::BRIGHT_RED + "Wait a minute! You already created a variable named '" + Colors::CYAN + Name + Colors::BRIGHT_RED + "'." + Colors::RESET) {
    Message += "\n  " + Colors::BRIGHT_BLUE + "Note: You first created it at line " + 
               std::to_string(PrevLoc.Line) + ", column " + 
               std::to_string(PrevLoc.Col) + Colors::RESET;
    Message += "\n  " + Colors::BRIGHT_GREEN + "Hint: If you want to change its value, just do `" + Name + " = new_value;` without the type (like 'int' or 'string'). If you want a new variable, give it a different name!" + Colors::RESET;
  }
};

class ShadowingWarning : public TurfError {
public:
  ShadowingWarning(SourceLocation NewLoc, const std::string &Name,
                   SourceLocation PrevLoc)
      : TurfError(NewLoc, Colors::BRIGHT_YELLOW + "Just so you know, you're creating a new '" + Colors::CYAN + Name + Colors::BRIGHT_YELLOW + "' that hides an older one!" + Colors::RESET) {
    Message = Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET + Message;
    Message += "\n  " + Colors::BRIGHT_BLUE + "Note: The older one is at line " + 
               std::to_string(PrevLoc.Line) + ", column " + 
               std::to_string(PrevLoc.Col) + Colors::RESET;
    Message += "\n  " + Colors::BRIGHT_GREEN + "Hint: It's better to give this new variable a different name to avoid confusion later." + Colors::RESET;
  }
  
  // Warnings don't exit
  void warn() const {
    LogErrorAt(Loc, Message);
  }
};

class UnreachableCodeError : public TurfError {
public:
  UnreachableCodeError(SourceLocation Loc, const std::string &What)
      : TurfError(Loc, Colors::BRIGHT_RED + "This " + What + " will never happen because you told me to return before it!" + Colors::RESET + "\n  " + Colors::BRIGHT_GREEN + "Hint: Move this code before the 'return' statement if you want it to run." + Colors::RESET) {}
};

// Control Flow Errors
class MissingReturnError : public TurfError {
public:
  MissingReturnError(const std::string &FunctionName)
      : TurfError(SourceLocation{0, 0}, 
                  Colors::BRIGHT_RED + "The function '" + Colors::CYAN + FunctionName + Colors::BRIGHT_RED + "' promises to give back a result, but sometimes it doesn't!" + Colors::RESET + "\n  " + Colors::BRIGHT_GREEN + "Hint: Make sure every path in your function (like inside 'if' and 'else') has a 'return' statement." + Colors::RESET) {}
};

class UnreachableBlockWarning : public TurfError {
public:
  UnreachableBlockWarning(const std::string &FunctionName, 
                          const std::string &BlockName)
      : TurfError(SourceLocation{0, 0},
                  Colors::BRIGHT_YELLOW + "The code in '" + BlockName + "' inside '" + FunctionName + "' can never be reached!" + Colors::RESET) {
    Message = Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET + Message + "\n  " + Colors::BRIGHT_GREEN + "Hint: Check your 'if' conditions. Are they impossible to be true, or did you 'return' before this block?" + Colors::RESET;
  }
  
  void warn() const {
    std::cerr << Message << "\n";
  }
};

class DeadBranchWarning : public TurfError {
public:
  DeadBranchWarning(SourceLocation Loc, const std::string &FunctionName)
      : TurfError(Loc, Colors::BRIGHT_YELLOW + "There's a part of '" + FunctionName + "' that will never be executed!" + Colors::RESET) {
    Message = Colors::BOLD + Colors::BRIGHT_YELLOW + "Warning: " + Colors::RESET + Message + "\n  " + Colors::BRIGHT_GREEN + "Hint: Check if you have code after a 'return', 'break', or 'continue' that can't be reached." + Colors::RESET;
  }
  
  void warn() const {
    LogErrorAt(Loc, Message);
  }
};

class StatementAfterTerminatorError : public TurfError {
public:
  StatementAfterTerminatorError(SourceLocation Loc, const std::string &Terminator)
      : TurfError(Loc, Colors::BRIGHT_RED + "This code comes after a " + Terminator + ", so it's impossible to reach it!" + Colors::RESET + "\n  " + Colors::BRIGHT_GREEN + "Hint: Delete this code or move it before the '" + Terminator + "' statement." + Colors::RESET) {}
};

#endif
