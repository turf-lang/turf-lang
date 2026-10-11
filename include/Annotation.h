#pragma once

// Annotation.h -- the front end's whole picture of `@name(args)` annotations.
//
// An annotation is written immediately before a function or `extern fn`
// declaration and says something ABOUT that declaration to one compiler pass:
//
//     @ownership(acquires)
//     pub extern fn malloc(size: int): void*;
//
// The grammar is deliberately meaning-free: the Parser records the name and the
// arguments and nothing else, and the Resolver rejects a name no pass has
// claimed (KnownAnnotations(), AnnotationRegistry.h). What an annotation MEANS, including
// whether its arguments make sense, is decided only by the one pass that
// consumes that name. This is the mechanism that lets the compiler know a fact
// about a function because the function's declaration says so, never because
// of its name.

#include "Lexer.h" // SourceLocation

#include <string>
#include <vector>

/// One argument of an annotation: `value` (positional, Key empty) or
/// `key: value`. A key is a word; a value is a word or a string literal. A
/// word is an identifier or a keyword, as spelled (`error: ...` is a valid
/// key). The grammar accepts nothing richer, so no pass has to evaluate an
/// expression here.
struct AnnotationArg {
  std::string Key;   ///< empty for a positional argument
  std::string Value; ///< the word as spelled, or the string literal's contents
  bool IsString = false;
  SourceLocation Loc;
};

struct Annotation {
  std::string Name; ///< without the '@'
  SourceLocation Loc; ///< the '@'
  std::vector<AnnotationArg> Args;

  /// The first argument written `Key: value`, or null.
  const AnnotationArg *findKeyed(const std::string &Key) const {
    for (const auto &A : Args)
      if (A.Key == Key)
        return &A;
    return nullptr;
  }

  /// True if a positional (keyless) argument spells `Word`.
  bool hasWord(const std::string &Word) const {
    for (const auto &A : Args)
      if (A.Key.empty() && !A.IsString && A.Value == Word)
        return true;
    return false;
  }
};

/// Mixin for every declaration an annotation may precede (a function or an
/// `extern fn`), beside VisibleDecl. Repeatable: one declaration may carry
/// several annotations, including several of the same name.
class AnnotatedDecl {
  std::vector<Annotation> Annotations_;

public:
  const std::vector<Annotation> &getAnnotations() const { return Annotations_; }
  void setAnnotations(std::vector<Annotation> A) { Annotations_ = std::move(A); }

  /// Every annotation spelled `Name`, in source order.
  std::vector<const Annotation *> getAnnotations(const std::string &Name) const {
    std::vector<const Annotation *> Out;
    for (const auto &A : Annotations_)
      if (A.Name == Name)
        Out.push_back(&A);
    return Out;
  }
};
