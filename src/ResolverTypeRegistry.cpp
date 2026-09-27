/**
 * @file ResolverTypeRegistry.cpp
 * @brief Struct/enum/user-error-type registry half of the Resolver's symbol
 *        table (Issue #2), split from the original monolithic
 *        ResolverSymbolTable.cpp -- see
 *        docs/architecture/compiler_pipeline.md.
 *
 * @author a2ys
 * @copyright Copyright (c) 2026 TurfLang contributors.  Apache License.
 */

#include "ResolverTypeRegistry.h"

#include <algorithm>
#include <map>

std::optional<ResolverSourceLocation>
ResolverTypeRegistry::registerStruct(const std::string &Name,
                                     ResolverSourceLocation Loc,
                                     const std::string &ModuleName) {
  auto It = RegisteredStructs_.find(Name);
  if (It != RegisteredStructs_.end())
    return It->second; // duplicate -- caller raises DuplicateTypeNameError
  RegisteredStructs_.emplace(Name, Loc);
  TypeOwningModule_.emplace(Name, ModuleName);
  return std::nullopt;
}

bool ResolverTypeRegistry::isKnownStruct(const std::string &Name) const {
  return RegisteredStructs_.count(Name) > 0;
}

std::vector<std::string> ResolverTypeRegistry::getAllStructNames() const {
  std::vector<std::string> Names;
  Names.reserve(RegisteredStructs_.size());
  for (const auto &Pair : RegisteredStructs_)
    Names.push_back(Pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

std::string ResolverTypeRegistry::getTypeOwningModule(const std::string &Name) const {
  auto It = TypeOwningModule_.find(Name);
  return It == TypeOwningModule_.end() ? "" : It->second;
}

std::vector<std::string>
ResolverTypeRegistry::getModuleTypeNames(const std::string &ModuleName) const {
  std::vector<std::string> Names;
  for (const auto &Pair : TypeOwningModule_)
    if (Pair.second == ModuleName)
      Names.push_back(Pair.first);
  std::sort(Names.begin(), Names.end());
  return Names;
}

std::optional<ResolverSourceLocation>
ResolverTypeRegistry::registerEnumType(const std::string &Name,
                                       ResolverSourceLocation Loc,
                                       std::shared_ptr<TurfEnumType> ET,
                                       const std::string &ModuleName) {
  auto LocIt = EnumDeclLocs_.find(Name);
  if (LocIt != EnumDeclLocs_.end())
    return LocIt->second; // duplicate -- caller raises DuplicateTypeNameError
  EnumDeclLocs_.emplace(Name, Loc);
  RegisteredEnums_[Name] = std::move(ET);
  TypeOwningModule_.emplace(Name, ModuleName);
  return std::nullopt;
}

std::shared_ptr<TurfEnumType> ResolverTypeRegistry::lookupEnumType(const std::string& Name) const {
  auto It = RegisteredEnums_.find(Name);
  if (It != RegisteredEnums_.end()) {
    return It->second;
  }
  return nullptr;
}

bool ResolverTypeRegistry::isEnumName(const std::string& Name) const {
  return RegisteredEnums_.count(Name) > 0;
}

std::vector<std::string> ResolverTypeRegistry::getAllEnumNames() const {
  std::vector<std::string> Names;
  Names.reserve(RegisteredEnums_.size());
  for (const auto& Pair : RegisteredEnums_) {
    Names.push_back(Pair.first);
  }
  std::sort(Names.begin(), Names.end());
  return Names;
}

std::optional<ResolverSourceLocation>
ResolverTypeRegistry::registerErrorType(const std::string &Name,
                                        ResolverSourceLocation Loc,
                                        std::shared_ptr<TurfUserErrorType> ET,
                                        const std::string &ModuleName) {
  auto LocIt = ErrorDeclLocs_.find(Name);
  if (LocIt != ErrorDeclLocs_.end())
    return LocIt->second; // duplicate -- caller raises DuplicateTypeNameError
  ErrorDeclLocs_.emplace(Name, Loc);
  RegisteredErrorTypes_[Name] = std::move(ET);
  TypeOwningModule_.emplace(Name, ModuleName);
  return std::nullopt;
}

std::shared_ptr<TurfUserErrorType> ResolverTypeRegistry::lookupErrorType(const std::string& Name) const {
  auto It = RegisteredErrorTypes_.find(Name);
  if (It != RegisteredErrorTypes_.end()) {
    return It->second;
  }
  return nullptr;
}

bool ResolverTypeRegistry::isErrorTypeName(const std::string& Name) const {
  return RegisteredErrorTypes_.count(Name) > 0;
}

std::vector<std::string> ResolverTypeRegistry::getAllErrorTypeNames() const {
  std::vector<std::string> Names;
  Names.reserve(RegisteredErrorTypes_.size());
  for (const auto& Pair : RegisteredErrorTypes_) {
    Names.push_back(Pair.first);
  }
  std::sort(Names.begin(), Names.end());
  return Names;
}

void ResolverTypeRegistry::dumpStructNames(std::ostream &OS) const {
  OS << "--- Registered Struct Names (Pass 1 Hoist) ---\n";
  if (RegisteredStructs_.empty()) {
    OS << "  (none)\n";
  } else {
    auto Names = getAllStructNames(); // sorted
    for (const auto &N : Names)
      OS << "  struct " << N << "\n";
  }
}

void ResolverTypeRegistry::dumpTypeOwnership(std::ostream &OS) const {
  OS << "\n--- Struct/Enum Type Ownership ---\n";
  if (TypeOwningModule_.empty()) {
    OS << "  (no struct/enum types registered)\n";
  } else {
    std::map<std::string, std::vector<std::string>> ByModule; // "" = main file
    for (const auto &Pair : TypeOwningModule_)
      ByModule[Pair.second].push_back(Pair.first);
    for (auto &Pair : ByModule)
      std::sort(Pair.second.begin(), Pair.second.end());
    for (const auto &Pair : ByModule) {
      OS << (Pair.first.empty() ? "(main file)" : "Module '" + Pair.first + "'")
         << " declares:\n";
      for (const auto &TypeName : Pair.second) {
        const char *Kind = RegisteredEnums_.count(TypeName) ? "enum" : "struct";
        OS << "    " << Kind << " " << TypeName << "\n";
      }
    }
  }
}
