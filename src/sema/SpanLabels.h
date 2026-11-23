#ifndef TURF_SPAN_LABELS_H
#define TURF_SPAN_LABELS_H

// DX-5 (TDP-2-028): the words and places of a labelled span, for the raise
// sites that know the types involved. Worded by
// docs/development/DIAGNOSTICS_STYLE.md: `this is an `int``,
// `` `s` is a `string` because of this ``.

#include "frontend/AST.h"
#include "sema/Types.h"

#include <cctype>
#include <string>

namespace SpanLabels {

// "an `int`", "a `string`": the type in backticks with its article. A type
// read with a vowel sound first takes "an"; `u...` (`uint8`, "you-int")
// takes "a".
inline std::string aTypeNamed(const std::string &Name) {
  const char C = Name.empty() ? 'x' : static_cast<char>(std::tolower(static_cast<unsigned char>(Name[0])));
  const bool An = C == 'a' || C == 'e' || C == 'i' || C == 'o';
  return std::string(An ? "an" : "a") + " `" + Name + "`";
}

inline std::string aType(const TurfType *T) {
  return aTypeNamed(T ? T->getName() : "unknown");
}
inline std::string aType(const TurfTypePtr &T) { return aType(T.get()); }

// The primary label for an expression of type `T`.
inline std::string thisIs(const TurfType *T) { return "this is " + aType(T); }

// Where `E` is written, start to end (ExprAST::getSpan()); `Fallback` for a
// node the parser did not read.
inline SourceLocation spanOf(const ExprAST *E, const SourceLocation &Fallback) {
  return E && E->hasSpan() ? E->getSpan() : Fallback;
}

} // namespace SpanLabels

#endif // TURF_SPAN_LABELS_H
