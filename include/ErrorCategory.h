#ifndef ERROR_CATEGORY_H
#define ERROR_CATEGORY_H

#include <string>
#include <unordered_map>

namespace SLM {

enum class ErrorCategory {
  TYPE_MISMATCH,
  UNDECLARED_VAR,
  CONTROL_FLOW,
  SYNTAX_ERROR,
  ARITHMETIC,
  STRING_CONVERSION,
  ARRAY_ERROR,
  DECLARATION,
  FUNCTION_SIGNATURE,
  GENERAL
};

// Deterministic classifier: pattern-matches the (ANSI-stripped) error message
// to determine which language construct caused the error.
inline ErrorCategory classifyError(const std::string &msg) {
  // Order matters: more specific patterns first.

  // Array errors
  if (msg.find("Array index") != std::string::npos ||
      msg.find("Array size") != std::string::npos ||
      msg.find("Array element") != std::string::npos ||
      msg.find("array") != std::string::npos)
    return ErrorCategory::ARRAY_ERROR;

  // Type mismatch
  if (msg.find("Type mismatch") != std::string::npos ||
      msg.find("Wrong return type") != std::string::npos)
    return ErrorCategory::TYPE_MISMATCH;

  // Undeclared / use-before-declaration
  if (msg.find("can't find anything named") != std::string::npos ||
      msg.find("before telling me what it is") != std::string::npos)
    return ErrorCategory::UNDECLARED_VAR;

  // Control flow
  if (msg.find("Control flow error") != std::string::npos ||
      msg.find("promises to give back a result") != std::string::npos ||
      msg.find("will never happen") != std::string::npos ||
      msg.find("impossible to reach") != std::string::npos)
    return ErrorCategory::CONTROL_FLOW;

  // String conversion / lengthof
  if (msg.find("can't convert") != std::string::npos ||
      msg.find("lengthof only works") != std::string::npos)
    return ErrorCategory::STRING_CONVERSION;

  // Arithmetic
  if (msg.find("Math trouble") != std::string::npos ||
      msg.find("with zero") != std::string::npos)
    return ErrorCategory::ARITHMETIC;

  // Syntax / keyword
  if (msg.find("tiny mistake") != std::string::npos ||
      msg.find("don't know the word") != std::string::npos)
    return ErrorCategory::SYNTAX_ERROR;

  // Declaration issues
  if (msg.find("already created a variable") != std::string::npos ||
      msg.find("hides an older one") != std::string::npos)
    return ErrorCategory::DECLARATION;

  // Void value misuse
  if (msg.find("void value") != std::string::npos)
    return ErrorCategory::FUNCTION_SIGNATURE;

  return ErrorCategory::GENERAL;
}

// Human-readable category name (also used in the prompt).
inline const char *getCategoryName(ErrorCategory cat) {
  switch (cat) {
  case ErrorCategory::TYPE_MISMATCH:      return "TYPE_MISMATCH";
  case ErrorCategory::UNDECLARED_VAR:     return "UNDECLARED_VARIABLE";
  case ErrorCategory::CONTROL_FLOW:       return "CONTROL_FLOW";
  case ErrorCategory::SYNTAX_ERROR:       return "SYNTAX_ERROR";
  case ErrorCategory::ARITHMETIC:         return "ARITHMETIC";
  case ErrorCategory::STRING_CONVERSION:  return "STRING_CONVERSION";
  case ErrorCategory::ARRAY_ERROR:        return "ARRAY_ERROR";
  case ErrorCategory::DECLARATION:        return "DECLARATION";
  case ErrorCategory::FUNCTION_SIGNATURE: return "FUNCTION_SIGNATURE";
  case ErrorCategory::GENERAL:            return "GENERAL";
  }
  return "GENERAL";
}

// Returns the path to the external documentation file for a given category.
// Paths are relative to CWD (same convention as the model path).
inline std::string getCategoryDocPath(ErrorCategory cat) {
  static const std::unordered_map<ErrorCategory, std::string> paths = {
      {ErrorCategory::TYPE_MISMATCH,      "docs/slm/type_mismatch.txt"},
      {ErrorCategory::UNDECLARED_VAR,     "docs/slm/undeclared_var.txt"},
      {ErrorCategory::CONTROL_FLOW,       "docs/slm/control_flow.txt"},
      {ErrorCategory::SYNTAX_ERROR,       "docs/slm/syntax_error.txt"},
      {ErrorCategory::ARITHMETIC,         "docs/slm/arithmetic.txt"},
      {ErrorCategory::STRING_CONVERSION,  "docs/slm/string_conversion.txt"},
      {ErrorCategory::ARRAY_ERROR,        "docs/slm/array_error.txt"},
      {ErrorCategory::DECLARATION,        "docs/slm/declaration.txt"},
      {ErrorCategory::FUNCTION_SIGNATURE, "docs/slm/function_signature.txt"},
      {ErrorCategory::GENERAL,            "docs/slm/general.txt"},
  };

  auto it = paths.find(cat);
  if (it != paths.end())
    return it->second;
  return "docs/slm/general.txt";
}

} // namespace SLM

#endif // ERROR_CATEGORY_H
