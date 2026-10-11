#pragma once

// AnnotationRegistry.h -- which annotation names exist, and who checks them.
//
// An annotation name is "known" when some compiler pass has claimed it, and the
// claiming pass supplies the function that checks that name's arguments
// against the declaration it sits on. The Resolver runs these at every
// annotated declaration (ResolverDecl.cpp), so the front end never needs to
// understand any one annotation: it asks the registry. A pass starting to read
// a new annotation adds one line here and nothing else changes.

#include "AliasAnalysis.h"
#include "Annotation.h"
#include "FfiAnnotation.h"

#include <string>
#include <utility>
#include <vector>

struct AnnotationSpec {
  const char *Name;
  /// Returns what is wrong with `Ann` on a declaration with these parameters
  /// and return type (`IsExtern`: it is an `extern fn`), or "" if it is well
  /// formed. `All` is every annotation on the declaration, `Ann` among them,
  /// for a check that spans several (a fact stated twice).
  std::string (*Validate)(const Annotation &Ann,
                          const std::vector<std::pair<std::string, TurfTypePtr>> &Params,
                          const TurfTypePtr &ReturnType, bool IsExtern,
                          const std::vector<Annotation> &All);
};

inline const std::vector<AnnotationSpec> &KnownAnnotations() {
  static const std::vector<AnnotationSpec> Specs = {
      {OwnershipAnnotation::Name, OwnershipAnnotation::validate},
      {FfiAnnotation::Name, FfiAnnotation::validate},
  };
  return Specs;
}
