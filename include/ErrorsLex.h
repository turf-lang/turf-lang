#ifndef ERRORS_LEX_H
#define ERRORS_LEX_H

#include "Errors.h"

// Syntax Error : Parser issues
class SyntaxError : public TurfError {
public:
  SyntaxError(SourceLocation Loc, const std::string &Msg)
      : TurfError(Loc, Colors::BRIGHT_RED +
                           "Oops! There's a tiny mistake here:\n" +
                           Colors::RESET + "  " + Msg,
                  "Check your spelling, punctuation like ';' or "
                  "'}', and make sure everything is closed properly.") {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;
  }
};
// Keyword Error : Misspelled keywords
class KeywordError : public TurfError {
public:
  KeywordError(SourceLocation Loc, const std::string &Name)
      : TurfError(Loc, Colors::BRIGHT_RED + "Hmm, I don't know the word '" +
                           Colors::CYAN + Name + Colors::BRIGHT_RED + "'." +
                           Colors::RESET) {
    Category_ = SLM::ErrorCategory::SYNTAX_ERROR;

    std::vector<std::string> Candidates = SuggestionIndex::suggestKeywords(Name);

    if (!Candidates.empty()) {
      std::string sug = "Did you mean: ";
      for (size_t i = 0; i < Candidates.size(); ++i) {
        sug += Colors::BRIGHT_YELLOW + "'" + Candidates[i] + "'" +
                   Colors::BRIGHT_GREEN;
        if (i != Candidates.size() - 1)
          sug += ", ";
      }
      sug += "?";
      Hint_ = sug;
    } else {
      Hint_ = "Check if you spelled the keyword correctly.";
    }
  }
};

#endif
