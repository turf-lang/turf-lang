#include "Monomorphize.h"

namespace Monomorphize {

static const char *kindName(InstantiationKind K) {
  switch (K) {
  case InstantiationKind::FreeFunction: return "fn";
  case InstantiationKind::Method:       return "method";
  case InstantiationKind::Struct:       return "struct";
  }
  return "?";
}

void InstantiationCollector::report(std::ostream &OS) const {
  const auto &All = Cache_.all();
  OS << "-- monomorphization instantiation census (" << All.size()
     << " distinct request" << (All.size() == 1 ? "" : "s") << ") --\n";
  for (const auto &Req : All) {
    OS << "  [" << kindName(Req.Kind) << "] " << Req.DeclKey << "<";
    for (size_t i = 0; i < Req.TypeArgs.size(); ++i) {
      if (i) OS << ", ";
      OS << (Req.TypeArgs[i] ? Req.TypeArgs[i]->getName() : "?");
    }
    OS << ">  (" << Req.Site.Filename << ":" << Req.Site.Line << ")\n";
  }
}

} // namespace Monomorphize
