/**
 * @file Codegen.cpp
 * @brief LLVM IR Generation for TurfLang.
 * @details Traverses the parsed Abstract Syntax Tree (AST) to emit equivalent
 *          LLVM Intermediate Representation (IR). Handles type checking,
 *          variable scoping, control flow structures, and function
 * declarations.
 *
 * @author a2ys
 * @author AaravTandon24
 * @author Karan
 *
 * @copyright Copyright (c) 2026 TurfLang contributors. Licensed under the
 * Apache License.
 * @version 1.0.0
 * @date 2026-05-25
 */

#include "Codegen.h"
#include "AST.h"
#include "Algorithms.h"
#include "Builtins.h"
#include "CFG.h"
#include "Errors.h"
#include "Analysis.h"
#include "AliasAnalysis.h"
#include "Lexer.h"
#include "ValueRange.h"
#include "Lint.h"
#include "SymbolTable.h"
#include "CallGraph.h"
#include "Types.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <unordered_map>

using namespace llvm;

std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<IRBuilder<>> Builder;
std::unique_ptr<Module> TheModule;

std::map<std::string, VarInfo> NamedValues;

// Current enclosing function context (nullptr at top level)
TurfTypePtr CurrentFuncReturnType = TurfVoid();
llvm::Function *CurrentFunction = nullptr;

std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> LoopBlocks;

// Tracks full param types for user-defined functions.
static std::map<std::string, std::vector<TurfTypePtr>> FuncParamTypes;
static std::map<std::string, TurfTypePtr> FuncReturnTypes;

// Set of library names collected from 'extern from "libname" fn' declarations.
// Populated during codegen; consumed by main.cpp::linkObject().
std::set<std::string> g_RequiredLibs;

// Type cache for Monomorphization: maps type name -> LLVM type
static std::map<std::string, llvm::Type *> LLVMTypeCache;

// Forward declarations for list helpers (defined after Array Codegen)
/**
 * @brief Initializes the core LLVM context, module, and instruction builder.
 *
 * Must be called before any IR generation begins. Sets up the environment
 * necessary for LLVM to construct the in-memory representation of the program.
 */
void InitializeModule() {
  // Holds types and constants
  TheContext = std::make_unique<LLVMContext>();

  // Holds functions
  TheModule = std::make_unique<Module>("Turf Compiler", *TheContext);

  // Builder to insert instructions
  Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

/**
 * @brief Maps a frontend TurfType to its corresponding backend llvm::Type.
 *
 * Monomorphizes generic types like list<T> by generating and caching a unique
 * LLVM struct type for each specific instantiation.
 *
 * @param T The frontend TurfType pointer.
 * @return llvm::Type* The mapped LLVM type pointer.
 */
llvm::Type *getLLVMType(const TurfType *T) {
  if (!T)
    return llvm::Type::getDoubleTy(*TheContext);
  std::string Key = T->getMangledName();

  auto It = LLVMTypeCache.find(Key);
  if (It != LLVMTypeCache.end())
    return It->second;

  // Primitive named types
  if (T->isNamed("int"))
    return LLVMTypeCache[Key] = llvm::Type::getInt64Ty(*TheContext);
  if (T->isNamed("double"))
    return LLVMTypeCache[Key] = llvm::Type::getDoubleTy(*TheContext);
  if (T->isNamed("bool"))
    return LLVMTypeCache[Key] = llvm::Type::getInt1Ty(*TheContext);
  if (T->isNamed("string"))
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);
  if (T->isNamed("void"))
    return LLVMTypeCache[Key] = llvm::Type::getVoidTy(*TheContext);
  // `ptr` — opaque pointer (i8* in LLVM IR)
  if (T->isNamed("ptr") || dynamic_cast<const OpaquePointerType *>(T))
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);

  // Array types — return ArrayType for fixed size, opaque pointer for unsized.
  if (isArrayType(T)) {
    const auto *AT = dynamic_cast<const ::ArrayType *>(T);
    if (AT && AT->getSize() > 0)
      return LLVMTypeCache[Key] = llvm::ArrayType::get(getLLVMType(AT->getElementTypePtr().get()), AT->getSize());
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);
  }

  // PointerType — lowers to ptr
  if (dynamic_cast<const TurfPointerType *>(T))
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);

  // User-defined structs must be declared and cached prior to this call.
  // Missing structs fallback to an opaque type to prevent immediate crashes.
  if (auto *ST = dynamic_cast<const TurfStructType *>(T)) {
    // If we reach here, StructDecl codegen hasn't happened or it failed.
    // We'll return an opaque struct to avoid crashing.
    return LLVMTypeCache[Key] = llvm::StructType::create(*TheContext, Key);
  }

  // Type Erasure: Generic Type Parameters are compiled as void* 
  if (dynamic_cast<const TurfGenericParamType *>(T)) {
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);
  }

  // GenericType instantiations (e.g. List<int>) — look up the base struct.
  // The base struct (e.g. "List") was registered in LLVMTypeCache by
  // StructDeclExprAST::codegen(). Use that layout for the generic instance.
  if (auto *GT = dynamic_cast<const GenericType *>(T)) {
    auto BaseIt = LLVMTypeCache.find(GT->getBaseName());
    if (BaseIt != LLVMTypeCache.end())
      return LLVMTypeCache[Key] = BaseIt->second;
    // Base struct not yet seen — create a forward-declared opaque type
    return LLVMTypeCache[Key] = llvm::StructType::create(*TheContext, GT->getBaseName());
  }

  // User-defined enums — always stored as their underlying integer type (i64).
  if (dynamic_cast<const TurfEnumType *>(T)) {
    return LLVMTypeCache[Key] = llvm::Type::getInt64Ty(*TheContext);
  }

  // NamedType — may refer to a user-defined struct OR an enum registered in
  // GlobalSymbolTable. Check GlobalSymbolTable first so enums resolve to i64.
  if (dynamic_cast<const NamedType *>(T)) {
    // Try to resolve through GlobalSymbolTable first
    if (GlobalSymbolTable) {
      if (auto Registered = GlobalSymbolTable->LookupType(T->getName())) {
        if (dynamic_cast<const TurfEnumType *>(Registered.get()))
          return LLVMTypeCache[Key] = llvm::Type::getInt64Ty(*TheContext);
      }
    }
    auto CacheIt = LLVMTypeCache.find(Key);
    if (CacheIt != LLVMTypeCache.end())
      return CacheIt->second;
    // Not yet registered — create a forward-declared opaque struct so the
    // function type can be built; StructDeclExprAST::codegen will fill it in.
    return LLVMTypeCache[Key] = llvm::StructType::create(*TheContext, Key);
  }

  // Unknown / future types
  return LLVMTypeCache[Key] = llvm::Type::getDoubleTy(*TheContext);
}

// Recover a TurfTypePtr from an LLVM Value type.
// Pointers are ambiguous (could be string or list), so callers that need
// precise list-vs-string info should use NamedValues directly.
static TurfTypePtr getTurfTypeFromLLVM(llvm::Type *Ty) {
  if (Ty->isDoubleTy())
    return TurfDouble();
  if (Ty->isIntegerTy(64))
    return TurfInt();
  if (Ty->isIntegerTy(32))
    return TurfInt();
  if (Ty->isIntegerTy(1))
    return TurfBool();
  if (Ty->isPointerTy())
    return TurfPtr(); // All LLVM pointers map to ptr; callers refine via RefineLLVMType
  if (Ty->isVoidTy())
    return TurfVoid();
  return TurfDouble();
}

static int getTypeRank(const TurfType *T) {
  if (!T)
    return 0;
  if (T->isNamed("double"))
    return 3;
  if (T->isNamed("int"))
    return 2;
  if (T->isNamed("bool"))
    return 1;
  if (T->isNamed("string"))
    return 4;
  return 0;
}

static TurfTypePtr getCommonType(TurfTypePtr A, TurfTypePtr B) {
  return getTypeRank(A.get()) >= getTypeRank(B.get()) ? A : B;
}

static TurfTypePtr RefineLLVMType(TurfTypePtr Deduced, TurfTypePtr Expected,
                                  llvm::Type *LLVMTy) {
  // When the LLVM type is a pointer and the expected type is any pointer-like
  // Turf type, trust the expected type (it has more info than the raw LLVM type).
  if (LLVMTy->isPointerTy() && Expected &&
      (isArrayType(Expected.get()) ||
       dynamic_cast<const TurfPointerType *>(Expected.get()) ||
       dynamic_cast<const OpaquePointerType *>(Expected.get()) ||
       Expected->isNamed("string") ||
       Expected->isNamed("void*"))) {
    return Expected;
  }
  return Deduced;
}

// Returns true when T is any pointer-like Turf type:
// string, ptr (OpaquePointerType), T* (TurfPointerType), void*.
// All of these share the same LLVM representation (opaque pointer).
static bool isPointerLike(const TurfType *T) {
  if (!T) return false;
  return T->isNamed("string") || T->isNamed("ptr") || T->isNamed("void*") ||
         dynamic_cast<const OpaquePointerType *>(T) ||
         dynamic_cast<const TurfPointerType *>(T);
}


static const TurfType *getExprType(const ExprAST *E) {
  if (!E)
    return nullptr;
  if (auto *Num = dynamic_cast<const NumberExprAST *>(E)) {
    if (!Num->isInteger())
      return TurfDouble().get();
    return TurfInt().get();
  }
  if (auto *BoolE = dynamic_cast<const BoolExprAST *>(E)) {
    return TurfBool().get();
  }
  if (auto *Str = dynamic_cast<const StringExprAST *>(E)) {
    return TurfString().get();
  }
  if (auto *NullE = dynamic_cast<const NullExprAST *>(E)) {
    return TurfNull().get();
  }
  if (auto *Bin = dynamic_cast<const BinaryExprAST *>(E)) {
    int Op = Bin->getOp();
    if (Op == '<' || Op == '>' || Op == '!' || Op == '=' ||
        Op == TOK_EQ || Op == TOK_NEQ || Op == TOK_LEQ || Op == TOK_GEQ ||
        Op == TOK_AND || Op == TOK_OR)
      return TurfBool().get();
    // Bitwise operators return int
    if (Op == '&' || Op == '|' || Op == '^' ||
        Op == TOK_LSHIFT || Op == TOK_RSHIFT)
      return TurfInt().get();
    // Exponentiation returns double
    if (Op == TOK_POWER)
      return TurfDouble().get();
    const TurfType *LType = getExprType(Bin->getLHS());
    const TurfType *RType = getExprType(Bin->getRHS());
    if (!LType)
      return RType;
    if (!RType)
      return LType;
    if (LType->isNamed("double") || RType->isNamed("double"))
      return TurfDouble().get();
    return LType;
  }
  if (auto *Unary = dynamic_cast<const UnaryExprAST *>(E)) {
    return getExprType(Unary->getOperand());
  }
  if (auto *FuncCall = dynamic_cast<const FuncCallExprAST *>(E)) {
    if (auto *VarCallee =
            dynamic_cast<const VariableExprAST *>(FuncCall->getCallee())) {
      std::string CalleeName = VarCallee->getName();

      if (CalleeName == "typeof")
        return TurfString().get();
      if (CalleeName == "lengthof")
        return TurfInt().get();

      std::string MangledName =
          (CalleeName == "main") ? CalleeName : "turf_" + CalleeName;
      std::vector<const TurfType *> ArgTypes;
      for (const auto &Arg : FuncCall->getArgs()) {
        const TurfType *T = getExprType(Arg.get());
        if (!T)
          return nullptr;
        ArgTypes.push_back(T);
        if (CalleeName != "main")
          MangledName += "_" + T->getMangledName();
      }
      // Check FuncReturnTypes for user-declared extern fn first.
      // This covers both normal turf functions and extern fn declarations.
      auto It = FuncReturnTypes.find(MangledName);
      if (It != FuncReturnTypes.end())
        return It->second.get();
      // Also check by raw CalleeName (extern fns register under their C name)
      auto ItRaw = FuncReturnTypes.find(CalleeName);
      if (ItRaw != FuncReturnTypes.end())
        return ItRaw->second.get();

      for (const auto &[Name, ParamTypes] : FuncParamTypes) {
        std::string Prefix = "turf_" + CalleeName;
        if (Name == Prefix ||
            (Name.size() > Prefix.size() && Name[Prefix.size()] == '_' &&
             Name.compare(0, Prefix.size(), Prefix) == 0)) {
          if (ParamTypes.size() == ArgTypes.size()) {
            bool Compatible = true;
            for (size_t i = 0; i < ArgTypes.size(); ++i) {
              if (!typesCompatible(ArgTypes[i], ParamTypes[i].get())) {
                Compatible = false;
                break;
              }
            }
            if (Compatible) {
              // Same class of gap as the struct-method case just above:
              // a generic FREE function is type-erased to ONE compiled
              // version, registered here under its bare declared return
              // type (e.g. TurfGenericParamType("T") for `fn<T>
              // firstElem(arr: T[]): T`) -- returning it unsubstituted
              // meant `firstElem(intArray) to string` failed with "I
              // can't convert 'T' to a string!" even though
              // TypeChecker's OWN (separate) inference already correctly
              // resolves this call's type as int. Recover T -> int by
              // unifying each DECLARED parameter type against the
              // ACTUAL argument type at this call site (recursing
              // through pointer/array wrapping, e.g. T[] vs int[] ->
              // T=int), then substitute it into the declared return type.
              std::unordered_map<std::string, TurfTypePtr> Subst;
              std::function<void(const TurfTypePtr &, const TurfType *)> Unify =
                  [&](const TurfTypePtr &ParamTy, const TurfType *ArgTy) {
                    if (!ParamTy || !ArgTy) return;
                    if (auto *GP = dynamic_cast<const TurfGenericParamType *>(ParamTy.get())) {
                      if (!Subst.count(GP->getName())) {
                        // Subst stores an owning TurfTypePtr; ArgTypes are
                        // raw pointers into caches that outlive this call,
                        // so wrap without taking ownership of a temporary.
                        Subst.emplace(GP->getName(), TurfTypePtr(const_cast<TurfType *>(ArgTy), [](TurfType*){}));
                      }
                    } else if (auto *NT = dynamic_cast<const NamedType *>(ParamTy.get())) {
                      bool KnownElsewhere = GlobalSymbolTable &&
                                           GlobalSymbolTable->LookupType(NT->getName()) != nullptr;
                      if (!KnownElsewhere && !Subst.count(NT->getName()))
                        Subst.emplace(NT->getName(), TurfTypePtr(const_cast<TurfType *>(ArgTy), [](TurfType*){}));
                    } else if (auto *PPT = dynamic_cast<const TurfPointerType *>(ParamTy.get())) {
                      if (auto *APT = dynamic_cast<const TurfPointerType *>(ArgTy))
                        Unify(PPT->getPointee(), APT->getPointee().get());
                    } else if (isArrayType(ParamTy.get()) && isArrayType(ArgTy)) {
                      Unify(dynamic_cast<const ::ArrayType *>(ParamTy.get())->getElementTypePtr(),
                            getArrayElementType(ArgTy));
                    }
                  };
              for (size_t i = 0; i < ArgTypes.size(); ++i)
                Unify(ParamTypes[i], ArgTypes[i]);

              const TurfType *RetTy = FuncReturnTypes[Name].get();
              if (!Subst.empty() && RetTy) {
                if (auto *GP = dynamic_cast<const TurfGenericParamType *>(RetTy)) {
                  auto It2 = Subst.find(GP->getName());
                  if (It2 != Subst.end())
                    return It2->second.get();
                } else if (auto *NT = dynamic_cast<const NamedType *>(RetTy)) {
                  auto It2 = Subst.find(NT->getName());
                  if (It2 != Subst.end())
                    return It2->second.get();
                }
              }
              return RetTy;
            }
          }
        }
      }
      // Fallback for intrinsic path: malloc/realloc return ptr, free returns void
      if (CalleeName == "malloc" || CalleeName == "realloc")
        return TurfPtr().get();
      if (CalleeName == "free")
        return TurfVoid().get();
    }
    return nullptr;
  }
  if (auto *Cast = dynamic_cast<const CastExprAST *>(E)) {
    return Cast->getDestType();
  }
  if (auto *Var = dynamic_cast<const VariableExprAST *>(E)) {
    // A bare struct TYPE NAME used as a static-method-call qualifier
    // (`MathUtil` in `MathUtil.square(5)`, Var->IsStructName set by the
    // Resolver) is not a real variable/symbol at all, so none of the three
    // lookups below could ever find it -- this branch fell all the way
    // through to `return nullptr;` at the end of the function, and the
    // null propagated to a `report_fatal_error("could not determine type
    // of method call base... this is a compiler bug")` further down in
    // MethodCallExprAST::codegen(), crashing the WHOLE compiler process on
    // ANY static struct method call, correctly-typed arguments or not.
    // Confirmed: no test in the suite exercised this at all before now.
    if (Var->IsStructName && GlobalSymbolTable) {
      if (auto T = GlobalSymbolTable->LookupType(Var->getName()))
        return T.get();
    }
    if (Var->ResolvedSymbol && Var->ResolvedSymbol->Type)
      return Var->ResolvedSymbol->Type.get();
    auto It = NamedValues.find(Var->getName());
    if (It != NamedValues.end() && It->second.Type)
      return It->second.Type.get();
    if (GlobalSymbolTable) {
      Symbol *Sym = GlobalSymbolTable->LookupSymbol(Var->getName());
      if (Sym)
        return Sym->Type.get();
    }
  }
  if (auto *ArrAcc = dynamic_cast<const ArrayAccessExprAST *>(E)) {
    const TurfType *BaseTy = getExprType(ArrAcc->getBase());
    if (BaseTy) {
      if (BaseTy->isString())
        return TurfInt().get();
      const TurfType *ElemTy = nullptr;
      if (isArrayType(BaseTy)) {
        ElemTy = getArrayElementType(BaseTy);
      } else if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy)) {
        ElemTy = PtrTy->getPointee().get();
      }
      
      if (ElemTy) {
        // Issue #17 fix: resolve NamedType element types (e.g. struct User)
        // to TurfStructType so that arr[0].field codegen works correctly.
        if (GlobalSymbolTable) {
          if (const auto *NT = dynamic_cast<const NamedType *>(ElemTy)) {
            if (auto Resolved = GlobalSymbolTable->LookupType(NT->getName()))
              return Resolved.get();
          }
        }
        return ElemTy;
      }
    }
  }
  if (auto *MethodCall = dynamic_cast<const MethodCallExprAST *>(E)) {
    const TurfType *BaseTy = getExprType(MethodCall->getBase());

    if (!BaseTy) {
      // Could be a module call (e.g. mem.malloc)
      if (auto *VarBase = dynamic_cast<const VariableExprAST *>(MethodCall->getBase())) {
        std::string CalleeName = VarBase->getName() + "_" + MethodCall->getMethod();
        std::string MangledName = "turf_" + CalleeName;
        std::vector<const TurfType *> ArgTypes;
        for (const auto &Arg : MethodCall->getArgs()) {
          const TurfType *T = getExprType(Arg.get());
          if (!T) {
            return nullptr;
          }
          ArgTypes.push_back(T);
          MangledName += "_" + T->getMangledName();
        }
        auto It = FuncReturnTypes.find(MangledName);
        if (It != FuncReturnTypes.end()) {
          return It->second.get();
        }
          
        for (const auto &[Name, ParamTypes] : FuncParamTypes) {
          std::string Prefix = "turf_" + CalleeName;
          if (Name == Prefix ||
              (Name.size() > Prefix.size() && Name[Prefix.size()] == '_' &&
               Name.compare(0, Prefix.size(), Prefix) == 0)) {
            if (ParamTypes.size() == ArgTypes.size()) {
              bool Compatible = true;
              for (size_t i = 0; i < ArgTypes.size(); ++i) {
                if (!typesCompatible(ArgTypes[i], ParamTypes[i].get())) {
                  Compatible = false;
                  break;
                }
              }
              if (Compatible)
                return FuncReturnTypes[Name].get();
            }
          }
        }
      }
      return nullptr;
    }

    if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy))
      BaseTy = PtrTy->getPointee().get();

    // Resolve NamedType base (e.g. the pointee of a method's own `Counter*`
    // return type) to the real TurfStructType, mirroring what the sibling
    // FieldAccessExprAST case below already does. Without this, a chained
    // method call three levels deep (`c.increment().increment().increment()`,
    // where `increment(): Counter*` returns self for chaining) crashed with
    // "Internal error: could not determine type of method call base": the
    // middle level's BaseTy resolved fine (a bare VariableExprAST base), but
    // the outermost level's Base is itself a MethodCallExprAST whose type
    // came back as NamedType("Counter") (the unresolved pointee of the
    // previous call's Counter* return type) instead of TurfStructType --
    // which the `dynamic_cast<const TurfStructType *>(BaseTy)` check just
    // below silently failed to match, falling through to `return nullptr`
    // with no other fallback since the Base isn't a VariableExprAST either.
    if (auto *NT = dynamic_cast<const NamedType *>(BaseTy)) {
      if (GlobalSymbolTable) {
        if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
          BaseTy = R.get();
      }
    }

    if (MethodCall->getMethod() == "pop" || MethodCall->getMethod() == "peek" ||
        MethodCall->getMethod() == "dequeue") {
      // For a concrete generic instantiation like List<int>, return the
      // first type argument as the element type so callers know how to
      // un-erase the returned void* back to the concrete type.
      if (auto *GenTy = dynamic_cast<const GenericType *>(BaseTy)) {
        if (!GenTy->getTypeArgs().empty())
          return GenTy->getTypeArg(0);
      }
    }
    // Resolve a GenericType base (e.g. List<int>) to its underlying
    // TurfStructType, mirroring the NamedType unwrap just above -- without
    // this, the general lookup just below (which only matches
    // TurfStructType) never fired for ANY method call on a generic struct
    // instance, including 'push'. That gap used to be papered over by a
    // hardcoded `if (MethodName == "push") return BaseTy;` special case
    // (removed) that assumed EVERY method literally named "push", on ANY
    // struct, generic or not, returns its receiver's bare type -- which
    // happens to not matter for List<T>.push (declared 'void', and its
    // result is never used), but is flatly wrong for a user-defined struct
    // whose OWN "push" method has a real, different declared return type.
    // Confirmed via a `Builder.push(x: int): Builder*` fluent/chaining
    // method (returns self by pointer, like the already-covered
    // `Counter.increment(): Counter*` pattern): this hack made
    // getExprType() report the chained call's type as bare `Builder`
    // instead of `Builder*`, so MethodCallExprAST::codegen()'s self-pointer
    // logic (which branches on "is Base's type actually a pointer") took
    // the wrong path -- spilling the returned pointer into a fresh temp
    // alloca and passing THAT ALLOCA'S ADDRESS as `self` instead of the
    // pointer itself, an extra level of indirection. Only the FIRST call in
    // `b.push(1).push(2)` ever mutated the real struct; the second silently
    // wrote through to a throwaway temporary.
    // Capture the ORIGINAL GenericType's type arguments (e.g. `int` for
    // `Box<int>`) before BaseTy gets overwritten to the resolved
    // TurfStructType just below -- needed for the substitution a few lines
    // down. Copied into an owning vector so it stays valid regardless of
    // what BaseTy is reassigned to.
    std::vector<TurfTypePtr> GenericTypeArgs;
    if (auto *GT0 = dynamic_cast<const GenericType *>(BaseTy))
      GenericTypeArgs = GT0->getTypeArgs();

    if (auto *GT = dynamic_cast<const GenericType *>(BaseTy)) {
      if (GlobalSymbolTable) {
        if (auto R = GlobalSymbolTable->LookupType(GT->getBaseName()))
          BaseTy = R.get();
      }
    }
    if (BaseTy && dynamic_cast<const TurfStructType *>(BaseTy)) {
      std::string MethodName =
          BaseTy->getName() + "." + MethodCall->getMethod();
      std::string MangledName =
          "turf_" + MethodName + "_" + BaseTy->getName() + "_ptr";
      std::vector<const TurfType *> ArgTypes;
      for (const auto &Arg : MethodCall->getArgs()) {
        const TurfType *T = getExprType(Arg.get());
        if (!T)
          return nullptr;
        ArgTypes.push_back(T);
        MangledName += "_" + T->getMangledName();
      }

      // Recover this struct's own type-parameter substitution (T -> int,
      // from GenericTypeArgs captured above) by scanning its fields for a
      // NamedType/TurfGenericParamType name that isn't a known struct/enum
      // -- mirrors TypeChecker::visitMethodCall()'s identical Subst-
      // building code exactly (see that function's doc comment for why
      // GlobalSymbolTable's copy of a generic struct's fields is
      // permanently stuck as NamedType, never rewritten to
      // TurfGenericParamType).
      //
      // Without this, a method's FuncReturnTypes entry -- the function's
      // own DECLARED return type, e.g. TurfGenericParamType("T") for
      // `fn<T> Box.get(): T` -- was returned completely unsubstituted.
      // This used to be silently papered over for exactly three method
      // names (pop/peek/dequeue, handled above by a hardcoded special
      // case using GenTy->getTypeArg(0) directly), but ANY OTHER
      // generically-returning method fell through to here with zero
      // substitution. Confirmed via `fn<T> Box.get(): T` on a `Box<int>`:
      // `b.get() to string` failed with "I can't convert 'T' to a
      // string!" even though TypeChecker had already correctly inferred
      // `int` for the very same call -- Codegen's getExprType() is a
      // completely separate, independent type-inference pass that never
      // consults TypeChecker's results.
      std::unordered_map<std::string, TurfTypePtr> Subst;
      if (!GenericTypeArgs.empty()) {
        if (auto *STy2 = dynamic_cast<const TurfStructType *>(BaseTy)) {
          std::function<void(const TurfTypePtr &)> Scan = [&](const TurfTypePtr &FTy) {
            if (!FTy || Subst.size() >= GenericTypeArgs.size()) return;
            if (auto *GP = dynamic_cast<const TurfGenericParamType *>(FTy.get())) {
              if (!Subst.count(GP->getName()))
                Subst.emplace(GP->getName(), GenericTypeArgs[Subst.size()]);
            } else if (auto *NT = dynamic_cast<const NamedType *>(FTy.get())) {
              bool KnownElsewhere = GlobalSymbolTable &&
                                   GlobalSymbolTable->LookupType(NT->getName()) != nullptr;
              if (!KnownElsewhere && !Subst.count(NT->getName()))
                Subst.emplace(NT->getName(), GenericTypeArgs[Subst.size()]);
            } else if (auto *PT = dynamic_cast<const TurfPointerType *>(FTy.get())) {
              Scan(PT->getPointee());
            } else if (isArrayType(FTy.get())) {
              Scan(dynamic_cast<const ::ArrayType *>(FTy.get())->getElementTypePtr());
            }
          };
          for (const auto &F : STy2->getFields())
            Scan(F.Type);
        }
      }
      // Substitute a TurfGenericParamType/NamedType return type through
      // Subst -- a no-op for a non-generic struct (Subst stays empty) or a
      // return type that doesn't mention any of this struct's parameters.
      auto ApplySubst = [&](const TurfType *RetTy) -> const TurfType * {
        if (Subst.empty() || !RetTy)
          return RetTy;
        if (auto *GP = dynamic_cast<const TurfGenericParamType *>(RetTy)) {
          auto It2 = Subst.find(GP->getName());
          if (It2 != Subst.end())
            return It2->second.get();
        } else if (auto *NT = dynamic_cast<const NamedType *>(RetTy)) {
          auto It2 = Subst.find(NT->getName());
          if (It2 != Subst.end())
            return It2->second.get();
        }
        return RetTy;
      };

      auto It = FuncReturnTypes.find(MangledName);
      if (It != FuncReturnTypes.end())
        return ApplySubst(It->second.get());

      for (const auto &[Name, ParamTypes] : FuncParamTypes) {
        std::string Prefix = "turf_" + MethodName;
        std::string Suffix = "_" + MethodName;
        bool Match = false;
        if (Name == Prefix || (Name.size() > Prefix.size() && Name[Prefix.size()] == '_' &&
                               Name.compare(0, Prefix.size(), Prefix) == 0)) {
          Match = true;
        } else {
          size_t Pos = Name.find(Suffix);
          if (Pos != std::string::npos && (Pos + Suffix.size() == Name.size() || Name[Pos + Suffix.size()] == '_')) {
            Match = true;
          }
        }
        if (Match) {
          if (ParamTypes.size() == ArgTypes.size() + 1) {
            bool Compatible = true;
            for (size_t i = 0; i < ArgTypes.size(); ++i) {
              if (!typesCompatible(ArgTypes[i], ParamTypes[i + 1].get())) {
                Compatible = false;
                break;
              }
            }
            if (Compatible)
              return ApplySubst(FuncReturnTypes[Name].get());
          }
        }
      }
    }
  }
  if (auto *StructInit = dynamic_cast<const StructInitExprAST *>(E)) {
    if (GlobalSymbolTable) {
      if (auto T = GlobalSymbolTable->LookupType(StructInit->getName())) {
        return T.get();
      }
    }
  }
  if (auto *FieldAccess = dynamic_cast<const FieldAccessExprAST *>(E)) {
    const TurfType *BaseTy = getExprType(FieldAccess->getBase());
    if (!BaseTy) return nullptr;
    if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy))
      BaseTy = PtrTy->getPointee().get();
    // Resolve NamedType base (e.g. field type from a parent struct field)
    if (auto *NT = dynamic_cast<const NamedType *>(BaseTy)) {
      if (GlobalSymbolTable) {
        if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
          BaseTy = R.get();
      }
    }
    if (auto *STy = dynamic_cast<const TurfStructType *>(BaseTy)) {
      TurfTypePtr FieldTy = STy->getFieldType(FieldAccess->getFieldName());
      if (!FieldTy) return nullptr;
      // Resolve NamedType field types so chained access (a.b.c) works correctly.
      if (auto *NT = dynamic_cast<const NamedType *>(FieldTy.get())) {
        if (GlobalSymbolTable) {
          if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
            return R.get();
        }
      }
      return FieldTy.get();
    }
  }
  if (auto *ArrLit = dynamic_cast<const ArrayLiteralExprAST *>(E)) {
    return ArrLit->CachedType.get();
  }
  // Address-of: &expr produces a pointer to the operand's type.
  // We cache one TurfPointerType per call site inside a static map keyed by
  // the pointee's mangled name so the returned pointer stays valid for the
  // caller's lifetime without leaking.
  if (auto *AddrOf = dynamic_cast<const AddressOfExprAST *>(E)) {
    const TurfType *InnerTy = getExprType(AddrOf->getOperand());
    if (!InnerTy) return nullptr;
    static std::map<std::string, TurfTypePtr> AddrOfTypeCache;
    std::string Key = InnerTy->getMangledName();
    auto It = AddrOfTypeCache.find(Key);
    if (It == AddrOfTypeCache.end()) {
      // Build a TurfPointerType wrapping a shared_ptr to a copy of InnerTy.
      // We cannot hold a bare pointer because InnerTy may be a temporary.
      TurfTypePtr InnerCopy;
      // Attempt cheap reconstruction from known primitive names first.
      if (InnerTy->isNamed("int"))    InnerCopy = TurfInt();
      else if (InnerTy->isNamed("double")) InnerCopy = TurfDouble();
      else if (InnerTy->isNamed("bool"))   InnerCopy = TurfBool();
      else {
        // For structs / named types, wrap the raw pointer with a no-op deleter.
        InnerCopy = TurfTypePtr(const_cast<TurfType*>(InnerTy), [](TurfType*){});
      }
      AddrOfTypeCache[Key] = std::make_shared<TurfPointerType>(std::move(InnerCopy));
    }
    return AddrOfTypeCache[Key].get();
  }
  // EnumAccess: Direction::North → the type is Direction (TurfEnumType)
  if (auto *EA = dynamic_cast<const EnumAccessExprAST *>(E)) {
    if (GlobalSymbolTable) {
      if (auto Registered = GlobalSymbolTable->LookupType(EA->getEnumName()))
        return Registered.get();
    }
  }
  return nullptr;
}

static Value *CastToType(Value *Val, const TurfType *DestType,
                         const std::string &Name, SourceLocation Loc = {0, 0});

static Value *CastToType(Value *Val, TurfTypePtr DestType,
                         const std::string &Name, SourceLocation Loc = {0, 0}) {
  return CastToType(Val, DestType.get(), Name, Loc);
}

static Value *CastToType(Value *Val, const TurfType *DestType,
                         const std::string &Name, SourceLocation Loc) {
  Type *ValTy = Val->getType();

  if (isa<ConstantPointerNull>(Val)) {
    if (isPointerLike(DestType)) {
      return Builder->CreateBitCast(Val, getLLVMType(DestType), Name);
    }
  }

  // Normalize foreign integer widths (e.g. i32 from printf) to i64
  if (ValTy->isIntegerTy(32)) {
    Val = Builder->CreateSExt(Val, Type::getInt64Ty(*TheContext), "i32toi64");
    ValTy = Val->getType();
  }

  TurfTypePtr SrcType = getTurfTypeFromLLVM(ValTy);
  SrcType = RefineLLVMType(SrcType, TurfTypePtr(const_cast<TurfType*>(DestType), [](TurfType*){}), ValTy);
  if (SrcType->getName() == DestType->getName())
    return Val;

  // Auto-coerce void* to/from any pointer
  if (SrcType->getName() == "void*" &&
      dynamic_cast<const TurfPointerType *>(DestType))
    return Val;
  if (DestType->getName() == "void*" &&
      dynamic_cast<const TurfPointerType *>(SrcType.get()))
    return Val;

  // Type Erasure: cast to generic parameter (stored as void*)
  if (dynamic_cast<const TurfGenericParamType *>(DestType)) {
    Type *VoidPtrTy = PointerType::get(*TheContext, 0);
    if (ValTy->isPointerTy())
      return Val; // already a pointer
    if (ValTy->isDoubleTy())
      Val = Builder->CreateBitCast(Val, Type::getInt64Ty(*TheContext), "dbl_to_i64");
    if (Val->getType()->isIntegerTy())
      return Builder->CreateIntToPtr(Val, VoidPtrTy, "erased_ptr");
    return Builder->CreateBitCast(Val, VoidPtrTy, "erased_ptr");
  }

  // Type Erasure: cast from generic parameter (void*) back to actual type.
  //
  // Gated on `ValTy->isPointerTy()`, NOT on `dynamic_cast<TurfGenericParamType>
  // (SrcType.get())` (the condition this replaced) -- SrcType a few lines up
  // is always produced by getTurfTypeFromLLVM(ValTy), which only ever
  // returns TurfDouble/TurfInt/TurfBool/TurfPtr/TurfVoid: it can NEVER
  // itself construct a TurfGenericParamType, so the old condition could
  // never match and this whole branch was dead code. What actually
  // identifies "this pointer is an erased generic value" at this point is
  // that it's a raw pointer being cast to a genuinely numeric DestType --
  // by the time Codegen runs, TypeChecker's typesCompatible() type-erasure
  // fast path is the only reason a pointer-typed value would ever be
  // assignment-compatible with int/bool/double in the first place, so this
  // combination cannot arise from anything else. Confirmed via
  // `fn<T> identity(x: T): T` returning a bool or double: with the old dead
  // condition, `identity(true)` segfaulted (a bare i1 was stored into an
  // opaque-pointer-typed alloca with no conversion at all) and
  // `identity(3.5)` silently returned garbage -- both are fixed by this
  // branch actually running.
  if (ValTy->isPointerTy()) {
    if (DestType->isNamed("int") || DestType->isNamed("bool")) {
      Value *IntVal = Builder->CreatePtrToInt(Val, Type::getInt64Ty(*TheContext), "unerased_int");
      if (DestType->isNamed("bool"))
        return Builder->CreateICmpNE(IntVal, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), "unerased_bool");
      return IntVal;
    }
    if (DestType->isNamed("double")) {
      Value *IntVal = Builder->CreatePtrToInt(Val, Type::getInt64Ty(*TheContext), "unerased_int");
      return Builder->CreateBitCast(IntVal, Type::getDoubleTy(*TheContext), "unerased_dbl");
    }
    if (isPointerLike(DestType)) {
      return Val; // already pointer
    }

    // Implicit dereference: a pointer-to-struct value cast to the bare
    // struct type BY VALUE -- e.g. `return self;` from a struct method
    // declared to return the struct type itself rather than a pointer.
    // `self` is always typed as `StructName*` (see ParseFuncDef's implicit
    // self-parameter construction), and there is no explicit dereference
    // operator in this language, so this is the only way
    // `fn Node.get(): Node { return self; }` can ever be satisfied.
    // TypeChecker::visitReturn() allows this exact pattern through its
    // return-type check (a pointer whose pointee resolves to the declared
    // return struct type); this is where the actual load happens.
    {
      const TurfType *ResolvedDest = DestType;
      if (const auto *NT = dynamic_cast<const NamedType *>(DestType)) {
        if (GlobalSymbolTable) {
          if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
            ResolvedDest = R.get();
        }
      }
      if (dynamic_cast<const TurfStructType *>(ResolvedDest)) {
        return Builder->CreateLoad(getLLVMType(ResolvedDest), Val, "derefstruct");
      }
    }
  }

  // All pointer-like types (string, ptr, T*, void*) share the same LLVM
  // representation. Casts between them are always no-ops at the IR level.
  if (isPointerLike(SrcType.get()) && isPointerLike(DestType))
    return Val;

  // Struct-to-struct: if DestType is a struct (NamedType or TurfStructType) and
  // the LLVM types already match (e.g. inner struct init), pass through without
  // casting. Without this, CastToType would hit the string-error guard below.
  if (ValTy->isStructTy()) {
    if (dynamic_cast<const TurfStructType *>(DestType) ||
        dynamic_cast<const NamedType *>(DestType))
      return Val;
  }

  // Strings cannot be cast to/from non-pointer types implicitly
  if (SrcType->isNamed("string") || DestType->isNamed("string")) {
    TypeError(CurLoc, "Cannot cast between string and non-pointer types. SrcType: " + SrcType->getName() + ", DestType: " + DestType->getName() + ", Context Name: " + Name)
        .raise();
    return Val;
  }

  if (DestType->isNamed("double")) {
    if (SrcType->isNamed("int"))
      return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), Name);
    if (SrcType->isNamed("bool"))
      return Builder->CreateUIToFP(Val, Type::getDoubleTy(*TheContext), Name);
  }
  if (DestType->isNamed("int")) {
    if (SrcType->isNamed("double"))
      return Builder->CreateFPToSI(Val, Type::getInt64Ty(*TheContext), Name);
    if (SrcType->isNamed("bool"))
      return Builder->CreateZExt(Val, Type::getInt64Ty(*TheContext), Name);
  }
  if (DestType->isNamed("bool")) {
    if (SrcType->isNamed("double"))
      // UNE, not ONE: this cast means "is Val truthy (non-zero)?", i.e.
      // `Val != 0.0`. ONE ("ordered and not equal") is false whenever Val
      // is NaN, which would make NaN cast to `false` -- but NaN != 0.0 is
      // true, so NaN should cast to `true`, matching the same '!=' fix
      // applied to TOK_NEQ above (see its doc comment for the full story).
      return Builder->CreateFCmpUNE(
          Val, ConstantFP::get(*TheContext, APFloat(0.0)), Name);
    if (SrcType->isNamed("int"))
      return Builder->CreateICmpNE(
          Val, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), Name);
  }

  return Val;
}

// Turns a number to LLVM Number constant
Value *NumberExprAST::codegen() {
  if (isInteger())
    return ConstantInt::get(*TheContext, APInt(64, getIntVal(), true));

  // APFloat is how LLVM represents floating point numbers internally
  return ConstantFP::get(*TheContext, APFloat(getDoubleVal()));
}

Value *BoolExprAST::codegen() {
  return ConstantInt::get(Type::getInt1Ty(*TheContext), Val ? 1 : 0);
}

Value *StringExprAST::codegen() {
  // Create a global string constant
  return Builder->CreateGlobalString(Val, "str");
}

Value *NullExprAST::codegen() {
  // A null pointer of type i8*
  return ConstantPointerNull::get(PointerType::getUnqual(*TheContext));
}

static llvm::Constant *GetDefaultValue(const TurfType *T);

Value *DefaultInitExprAST::codegen() {
  return GetDefaultValue(Type.get());
}

// Turns any expression into IR operation
static Value *EmitStringConcat(Value *L, Value *R) {
  Function *StrlenF = TheModule->getFunction("strlen");
  if (!StrlenF) {
    FunctionType *FT =
        FunctionType::get(Type::getInt64Ty(*TheContext),
                          {PointerType::getUnqual(*TheContext)}, false);
    StrlenF = Function::Create(FT, Function::ExternalLinkage, "strlen",
                               TheModule.get());
  }
  Function *StrcpyF = TheModule->getFunction("strcpy");
  if (!StrcpyF) {
    FunctionType *FT =
        FunctionType::get(PointerType::getUnqual(*TheContext),
                          {PointerType::getUnqual(*TheContext),
                           PointerType::getUnqual(*TheContext)}, false);
    StrcpyF = Function::Create(FT, Function::ExternalLinkage, "strcpy",
                               TheModule.get());
  }
  Function *StrcatF = TheModule->getFunction("strcat");
  if (!StrcatF) {
    FunctionType *FT =
        FunctionType::get(PointerType::getUnqual(*TheContext),
                          {PointerType::getUnqual(*TheContext),
                           PointerType::getUnqual(*TheContext)}, false);
    StrcatF = Function::Create(FT, Function::ExternalLinkage, "strcat",
                               TheModule.get());
  }
  Function *MallocF = TheModule->getFunction("malloc");
  if (!MallocF) {
    FunctionType *FT =
        FunctionType::get(PointerType::getUnqual(*TheContext),
                          {Type::getInt64Ty(*TheContext)}, false);
    MallocF = Function::Create(FT, Function::ExternalLinkage, "malloc",
                               TheModule.get());
  }

  Value *LenL = Builder->CreateCall(StrlenF, {L}, "lenL");
  Value *LenR = Builder->CreateCall(StrlenF, {R}, "lenR");
  Value *SumLen = Builder->CreateAdd(LenL, LenR, "sumlen");
  Value *TotalLen = Builder->CreateAdd(
      SumLen, ConstantInt::get(Type::getInt64Ty(*TheContext), 1), "totallen");

  // TURF-MEMLEAK: This malloc is intentionally not freed — Turf has no GC yet.
  // Every string concatenation allocates a fresh buffer on the heap and leaks it.
  // Future work: replace with an arena allocator or Boehm GC sweep.
  Value *Buf = Builder->CreateCall(MallocF, {TotalLen}, "strbuf");
  Builder->CreateCall(StrcpyF, {Buf, L});
  Builder->CreateCall(StrcatF, {Buf, R});
  return Buf;
}

static Value *EmitStrcmp(Value *L, Value *R) {
  Function *StrcmpF = TheModule->getFunction("strcmp");
  if (!StrcmpF) {
    FunctionType *FT =
        FunctionType::get(Type::getInt32Ty(*TheContext),
                          {PointerType::getUnqual(*TheContext),
                           PointerType::getUnqual(*TheContext)}, false);
    StrcmpF = Function::Create(FT, Function::ExternalLinkage, "strcmp",
                               TheModule.get());
  }
  return Builder->CreateCall(StrcmpF, {L, R}, "strcmptmp");
}

// EmitStrcmpRelational — emit a boolean comparison of two strings using strcmp.
// strcmp returns <0, 0, >0. We compare the result to 0 using the requested op.
// Called from BinaryExprAST::codegen() when both operands are string type.
static Value *EmitStrcmpRelational(Value *L, Value *R, int Op) {
  Value *Cmp  = EmitStrcmp(L, R);
  Value *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
  switch (Op) {
    case '<':     return Builder->CreateICmpSLT(Cmp, Zero, "strlt");
    case '>':     return Builder->CreateICmpSGT(Cmp, Zero, "strgt");
    case TOK_LEQ: return Builder->CreateICmpSLE(Cmp, Zero, "strle");
    case TOK_GEQ: return Builder->CreateICmpSGE(Cmp, Zero, "strge");
    default:      llvm_unreachable("EmitStrcmpRelational: invalid op");
  }
}

Value *BinaryExprAST::codegen() {
  // ── Short-circuit && / || ───────────────────────────────────────────────
  // These operators must NOT evaluate both sides eagerly.
  // We emit a branch: LHS first, then conditionally evaluate RHS.
  if (Op == TOK_AND) {
    Function *TheFunc = Builder->GetInsertBlock()->getParent();
    BasicBlock *RHSBlock  = BasicBlock::Create(*TheContext, "and.rhs",  TheFunc);
    BasicBlock *MergeBlock = BasicBlock::Create(*TheContext, "and.merge", TheFunc);

    Value *L = LHS->codegen();
    if (!L) return nullptr;
    L = CastToType(L, TurfBool(), "lhstobool");

    BasicBlock *LHSEnd = Builder->GetInsertBlock();
    Builder->CreateCondBr(L, RHSBlock, MergeBlock);

    // RHS block — only reached when LHS is true
    Builder->SetInsertPoint(RHSBlock);
    Value *R = RHS->codegen();
    if (!R) return nullptr;
    R = CastToType(R, TurfBool(), "rhstobool");
    BasicBlock *RHSEnd = Builder->GetInsertBlock();
    Builder->CreateBr(MergeBlock);

    // Merge — phi to pick the right bool value
    Builder->SetInsertPoint(MergeBlock);
    PHINode *Phi = Builder->CreatePHI(Type::getInt1Ty(*TheContext), 2, "andtmp");
    Phi->addIncoming(ConstantInt::getFalse(*TheContext), LHSEnd);
    Phi->addIncoming(R, RHSEnd);
    return Phi;
  }

  if (Op == TOK_OR) {
    Function *TheFunc = Builder->GetInsertBlock()->getParent();
    BasicBlock *RHSBlock  = BasicBlock::Create(*TheContext, "or.rhs",  TheFunc);
    BasicBlock *MergeBlock = BasicBlock::Create(*TheContext, "or.merge", TheFunc);

    Value *L = LHS->codegen();
    if (!L) return nullptr;
    L = CastToType(L, TurfBool(), "lhstobool");

    BasicBlock *LHSEnd = Builder->GetInsertBlock();
    Builder->CreateCondBr(L, MergeBlock, RHSBlock);

    // RHS block — only reached when LHS is false
    Builder->SetInsertPoint(RHSBlock);
    Value *R = RHS->codegen();
    if (!R) return nullptr;
    R = CastToType(R, TurfBool(), "rhstobool");
    BasicBlock *RHSEnd = Builder->GetInsertBlock();
    Builder->CreateBr(MergeBlock);

    // Merge — phi to pick the right bool value
    Builder->SetInsertPoint(MergeBlock);
    PHINode *Phi = Builder->CreatePHI(Type::getInt1Ty(*TheContext), 2, "ortmp");
    Phi->addIncoming(ConstantInt::getTrue(*TheContext), LHSEnd);
    Phi->addIncoming(R, RHSEnd);
    return Phi;
  }
  // ───────────────────────────────────────────────────────────────────────

  // Generate codes for the Left side and Right side
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();

  if (!L || !R)
    return nullptr;

  const TurfType *LHSTyAST = getExprType(LHS.get());
  const TurfType *RHSTyAST = getExprType(RHS.get());

  if (Op == '+') {
    if ((LHSTyAST && LHSTyAST->isString()) || (RHSTyAST && RHSTyAST->isString())) {
      if (LHSTyAST && LHSTyAST->isString() && RHSTyAST && RHSTyAST->isString()) {
        return EmitStringConcat(L, R);
      }
      return nullptr;
    }
  }

  // Type validation (void checks, compatibility, etc.) is handled by the TypeChecker.
  TurfTypePtr LTy = getTurfTypeFromLLVM(L->getType());
  TurfTypePtr RTy = getTurfTypeFromLLVM(R->getType());

  if (isa<ConstantPointerNull>(L) || isa<ConstantPointerNull>(R)) {
    if (Op == TOK_EQ || Op == TOK_NEQ) {
      if (isa<ConstantPointerNull>(L)) L = Builder->CreateBitCast(L, R->getType(), "lhscast");
      if (isa<ConstantPointerNull>(R)) R = Builder->CreateBitCast(R, L->getType(), "rhscast");
      return Op == TOK_EQ ? Builder->CreateICmpEQ(L, R, "cmptmp") : Builder->CreateICmpNE(L, R, "cmptmp");
    } else {
      llvm::report_fatal_error("Internal error: Invalid operation with null (caught by TypeChecker)");
      return nullptr;
    }
  }

  auto CastBoth = [&](TurfTypePtr Target) {
    L = CastToType(L, Target, "lhscast");
    R = CastToType(R, Target, "rhscast");
  };

  TurfTypePtr CommonType = getCommonType(LTy, RTy);
  TurfTypePtr NumericType =
      CommonType->isNamed("bool") ? TurfInt() : CommonType;

  // TypeChecker has already verified operator compatibility.

  // Create the instruction based on the operator
  switch (Op) {

    // Arithmetic
  case '+':
    CastBoth(NumericType);
    return NumericType->isNamed("double") ? Builder->CreateFAdd(L, R, "addtmp")
                                          : Builder->CreateAdd(L, R, "addtmp");

  case '-':
    CastBoth(NumericType);
    return NumericType->isNamed("double") ? Builder->CreateFSub(L, R, "subtmp")
                                          : Builder->CreateSub(L, R, "subtmp");

  case '*':
    CastBoth(NumericType);
    return NumericType->isNamed("double") ? Builder->CreateFMul(L, R, "multmp")
                                          : Builder->CreateMul(L, R, "multmp");

  case '/': {
    CastBoth(NumericType);

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *DivZeroBB =
        BasicBlock::Create(*TheContext, "divzero", TheFunction);
    BasicBlock *DivContBB =
        BasicBlock::Create(*TheContext, "divcont", TheFunction);

    Value *IsZero;
    if (NumericType->isNamed("double")) {
      IsZero = Builder->CreateFCmpOEQ(
          R, ConstantFP::get(*TheContext, APFloat(0.0)), "iszero");
    } else {
      IsZero = Builder->CreateICmpEQ(
          R, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), "iszero");
    }

    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);

    Builder->SetInsertPoint(DivZeroBB);
    CodegenIntrinsic("fail", {Builder->CreateGlobalString("Division by zero")}, Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    Builder->SetInsertPoint(DivContBB);

    return NumericType->isNamed("double") ? Builder->CreateFDiv(L, R, "divtmp")
                                          : Builder->CreateSDiv(L, R, "divtmp");
  }

  case '%': {
    CastBoth(NumericType);

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *DivZeroBB =
        BasicBlock::Create(*TheContext, "divzero", TheFunction);
    BasicBlock *DivContBB =
        BasicBlock::Create(*TheContext, "divcont", TheFunction);

    Value *IsZero;
    if (NumericType->isNamed("double")) {
      IsZero = Builder->CreateFCmpOEQ(
          R, ConstantFP::get(*TheContext, APFloat(0.0)), "iszero");
    } else {
      IsZero = Builder->CreateICmpEQ(
          R, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), "iszero");
    }

    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);

    Builder->SetInsertPoint(DivZeroBB);
    CodegenIntrinsic("fail", {Builder->CreateGlobalString("Modulo by zero")}, Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    Builder->SetInsertPoint(DivContBB);

    return NumericType->isNamed("double") ? Builder->CreateFRem(L, R, "modtmp")
                                          : Builder->CreateSRem(L, R, "modtmp");
  }

  // Comparison
  case '<': {
    // String ordering: byte-by-byte via strcmp (see EmitStrcmpRelational).
    if (LHSTyAST && LHSTyAST->isString() && RHSTyAST && RHSTyAST->isString())
      return EmitStrcmpRelational(L, R, '<');
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOLT(L, R, "cmptmp")
                                      : Builder->CreateICmpSLT(L, R, "cmptmp");
  }

  case '>': {
    // String ordering: byte-by-byte via strcmp.
    if (LHSTyAST && LHSTyAST->isString() && RHSTyAST && RHSTyAST->isString())
      return EmitStrcmpRelational(L, R, '>');
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOGT(L, R, "cmptmp")
                                      : Builder->CreateICmpSGT(L, R, "cmptmp");
  }

  case TOK_EQ: {
    if (LHSTyAST && LHSTyAST->isString() && RHSTyAST && RHSTyAST->isString()) {
      Value *Cmp = EmitStrcmp(L, R);
      Value *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
      return Builder->CreateICmpEQ(Cmp, Zero, "streq");
    }
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOEQ(L, R, "cmptmp")
                                      : Builder->CreateICmpEQ(L, R, "cmptmp");
  }

  case TOK_NEQ: {
    if (LHSTyAST && LHSTyAST->isString() && RHSTyAST && RHSTyAST->isString()) {
      Value *Cmp = EmitStrcmp(L, R);
      Value *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
      return Builder->CreateICmpNE(Cmp, Zero, "strne");
    }
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    // UNE ("unordered or not equal"), not ONE ("ordered and not equal"):
    // '!=' must be the logical negation of '==' for every input, including
    // NaN. '==' correctly uses OEQ below (ordered-equal is false for NaN,
    // matching NaN == NaN -> false). ONE requires BOTH operands to be
    // non-NaN AND unequal, so ONE(NaN, NaN) is *also* false -- meaning
    // `x != x` silently evaluated to false instead of true whenever x held
    // NaN, breaking the standard `x != x` isnan(x) idiom (found while
    // writing a Lint.cpp test that relied on it -- see
    // tests/02_resolver/warn_self_comparison_nan.tr).
    return CmpType->isNamed("double") ? Builder->CreateFCmpUNE(L, R, "cmptmp")
                                      : Builder->CreateICmpNE(L, R, "cmptmp");
  }

  case TOK_GEQ: {
    // String ordering: byte-by-byte via strcmp.
    if (LHSTyAST && LHSTyAST->isString() && RHSTyAST && RHSTyAST->isString())
      return EmitStrcmpRelational(L, R, TOK_GEQ);
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOGE(L, R, "cmptmp")
                                      : Builder->CreateICmpSGE(L, R, "cmptmp");
  }

  case TOK_LEQ: {
    // String ordering: byte-by-byte via strcmp.
    if (LHSTyAST && LHSTyAST->isString() && RHSTyAST && RHSTyAST->isString())
      return EmitStrcmpRelational(L, R, TOK_LEQ);
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOLE(L, R, "cmptmp")
                                      : Builder->CreateICmpSLE(L, R, "cmptmp");
  }

  // Logical operators
  case TOK_AND: {
    L = CastToType(L, TurfBool(), "lhstobool");
    R = CastToType(R, TurfBool(), "rhstobool");
    return Builder->CreateAnd(L, R, "andtmp");
  }

  case TOK_OR: {
    L = CastToType(L, TurfBool(), "lhstobool");
    R = CastToType(R, TurfBool(), "rhstobool");
    return Builder->CreateOr(L, R, "ortmp");
  }

  // Bitwise XOR (^ is now XOR, not power)
  case '^':
    // Bitwise XOR — both operands must be int (TypeChecker verified)
    return Builder->CreateXor(L, R, "xortmp");

  // Bitwise AND
  case '&':
    return Builder->CreateAnd(L, R, "bitandtmp");

  // Bitwise OR
  case '|':
    return Builder->CreateOr(L, R, "bitormp");

  // Bitwise left shift
  case TOK_LSHIFT:
    return Builder->CreateShl(L, R, "lshifttmp");

  // Bitwise right shift (arithmetic, preserves sign)
  case TOK_RSHIFT:
    return Builder->CreateAShr(L, R, "rshifttmp");

  // Exponentiation: ** (replaces the old ^ operator)
  case TOK_POWER: {
    CastBoth(TurfDouble());
#if LLVM_VERSION_MAJOR >= 19
    Function *PowFunc = llvm::Intrinsic::getOrInsertDeclaration(
      TheModule.get(), llvm::Intrinsic::pow,
      {Type::getDoubleTy(*TheContext)});
#else
    Function *PowFunc = llvm::Intrinsic::getDeclaration(
      TheModule.get(), llvm::Intrinsic::pow,
      {Type::getDoubleTy(*TheContext)});
#endif
    return Builder->CreateCall(PowFunc, {L, R}, "powtmp");
  }
  default:
    SemanticError(CurLoc, "Error: invalid binary operator");
    return nullptr;
  }
}

// This creates an alloca instruction in the entry block of a function
AllocaInst *CreateEntryBlockAlloca(Function *TheFunction,
                                   const std::string &VarName,
                                   const TurfType *Type) {
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(getLLVMType(Type), 0, VarName.c_str());
}

static llvm::Constant *GetDefaultValue(const TurfType *T) {
  if (!T)
    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*TheContext), 0.0);

  if (T->isNamed("int"))
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 0);
  if (T->isNamed("double"))
    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*TheContext), 0.0);
  if (T->isNamed("bool"))
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*TheContext), 0);
  if (T->isNamed("string"))
    return Builder->CreateGlobalStringPtr("");

  if (isArrayType(T) || dynamic_cast<const TurfPointerType *>(T) || dynamic_cast<const TurfGenericParamType *>(T)) {
    return llvm::ConstantPointerNull::get(llvm::PointerType::get(*TheContext, 0));
  }

  if (auto *ST = dynamic_cast<const TurfStructType *>(T)) {
    llvm::Type *LLVMTy = getLLVMType(T);
    auto *StructTy = llvm::dyn_cast<llvm::StructType>(LLVMTy);
    if (!StructTy)
      return llvm::UndefValue::get(LLVMTy);

    std::vector<llvm::Constant *> FieldVals;
    for (const auto &Field : ST->getFields()) {
      FieldVals.push_back(GetDefaultValue(Field.Type.get()));
    }
    return llvm::ConstantStruct::get(StructTy, FieldVals);
  }

  if (auto *GT = dynamic_cast<const GenericType *>(T)) {
    llvm::Type *LLVMTy = getLLVMType(T);
    return llvm::Constant::getNullValue(LLVMTy);
  }

  return llvm::Constant::getNullValue(getLLVMType(T));
}

Value *VarDeclExprAST::codegen() {
  // Validation (void type, keywords, duplicates, etc.) is handled by the
  // Resolver and TypeChecker passes before Codegen.


  // Check for unreachable declaration
  if (GlobalSymbolTable && GlobalSymbolTable->CurrentScopeHasEarlyExit()) {
    UnreachableCodeError(Loc, "declaration").raise();
    return nullptr;
  }

  // Check for duplicate declaration in current scope
  if (GlobalSymbolTable && GlobalSymbolTable->IsSymbolInCurrentScope(Name)) {
    Symbol *Prev = GlobalSymbolTable->LookupSymbolInCurrentScope(Name);
    if (Prev) {
      DuplicateDeclarationError(Loc, Name, Prev->DeclLoc).raise();
    } else {
      SyntaxError(Loc, "Variable already declared").raise();
    }
    return nullptr;
  }

  // Check for shadowing
  if (GlobalSymbolTable) {
    Symbol *Shadowed = GlobalSymbolTable->FindShadowedSymbol(Name);
    if (Shadowed) {
      ShadowingWarning(Loc, Name, Shadowed->DeclLoc).warn();
    }
  }

  Value *Init = nullptr;
  if (InitVal) {
    Init = InitVal->codegen();
    if (!Init)
      return nullptr;

    // ── Struct-init shortcut ───────────────────────────────────────────────
    // StructInitExprAST already allocated a fresh alloca, filled it
    // field-by-field, and returned a load of that alloca.  If we create
    // ANOTHER alloca and store the loaded struct value into it, LLVM's SROA
    // pass promotes each alloca independently and loses the alias between
    // `u`'s alloca and the pointer passed to `self` inside a method.
    // Fix: detect the load-from-structinit-alloca and reuse it directly.
    if (dynamic_cast<const StructInitExprAST *>(InitVal.get())) {
      if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(Init)) {
        if (auto *SrcAlloca = llvm::dyn_cast<AllocaInst>(LI->getPointerOperand())) {
          SrcAlloca->setName(Name);
          if (GlobalSymbolTable)
            GlobalSymbolTable->DeclareSymbol(Name, Type, Loc, SrcAlloca);
          NamedValues[Name] = {SrcAlloca, Type};
          return Init;
        }
      }
    }
    // ──────────────────────────────────────────────────────────────────────
  } else {
    Init = GetDefaultValue(Type.get());
  }

  Init = CastToType(Init, Type, "initcast", Loc);

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Name, Type.get());

  Builder->CreateStore(Init, Alloca);

  // Register in symbol table
  if (GlobalSymbolTable) {
    GlobalSymbolTable->DeclareSymbol(Name, Type, Loc, Alloca);
  }

  // Also keep in NamedValues for backward compatibility
  NamedValues[Name] = {Alloca, Type};
  return Init;
}

Value *VariableExprAST::codegen() {
  // Use the mangled name resolved by Resolver/TypeChecker (Issue #5)
  Symbol *Sym = nullptr;
  if (GlobalSymbolTable && ResolvedSymbol) {
    Sym = GlobalSymbolTable->LookupSymbol(ResolvedSymbol->Name);
  }

  if (Sym) {
    AllocaInst *A = Sym->Alloca;
    return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
  }

  // Fall back to old table for built-in compatibility
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end()) {
    AllocaInst *A = Iter->second.Alloca;
    return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
  }

  // Use-before-declaration error
  RankedSuggestions Suggestions;
  if (GlobalSymbolTable) {
    Suggestions = GlobalSymbolTable->GetRankedSuggestions(Name);
  } else {
    std::vector<std::string> VisibleNames;
    for (const auto &pair : NamedValues) VisibleNames.push_back(pair.first);
    Suggestions.Local = SuggestionIndex::suggestVisible(Name, VisibleNames);
  }
  UseBeforeDeclarationError(Loc, Name, Suggestions).raise();
  return nullptr;
}

Value *VariableExprAST::codegenLValue() {
  Symbol *Sym = nullptr;
  if (GlobalSymbolTable && ResolvedSymbol)
    Sym = GlobalSymbolTable->LookupSymbol(ResolvedSymbol->Name);
  if (Sym)
    return Sym->Alloca;

  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end())
    return Iter->second.Alloca;

  RankedSuggestions Suggestions;
  if (GlobalSymbolTable) {
    Suggestions = GlobalSymbolTable->GetRankedSuggestions(Name);
  } else {
    std::vector<std::string> VisibleNames;
    for (const auto &pair : NamedValues) VisibleNames.push_back(pair.first);
    Suggestions.Local = SuggestionIndex::suggestVisible(Name, VisibleNames);
  }
  UseBeforeDeclarationError(Loc, Name, Suggestions).raise();
  return nullptr;
}

Value *AssignmentExprAST::codegen() {
  // Generate code for the RHS first
  Value *Val = RHS->codegen();
  if (!Val)
    return nullptr;

    // Void value check is handled by TypeChecker.

  // Determine the LHS type without evaluating it (avoid double-evaluation side effects)
  const TurfType *LHSTy = getExprType(LHS.get());
  if (!LHSTy)
    return nullptr;

  // Cast directly against LHSTy (the raw-pointer CastToType overload) rather
  // than round-tripping it through getLLVMType()/getTurfTypeFromLLVM() to
  // rebuild a TurfTypePtr. That roundtrip used to be here and is lossy for
  // anything getTurfTypeFromLLVM() doesn't specifically recognize (double,
  // int64/32, i1, pointer, void) -- notably STRUCTS, which aren't mentioned
  // in that function's own doc comment at all and silently fell through to
  // its catch-all `return TurfDouble();`. For a same-struct-type assignment
  // this happened to stay harmless only by coincidence: both the source and
  // destination sides degraded to "double" symmetrically, so CastToType's
  // getName()-equality fast path still passed the value through unchanged.
  // But it was fragile (relying on typesCompatible() upstream to guarantee
  // the two sides could never degrade *asymmetrically*) and actively
  // misleading to read (a struct assignment's "VarType" claiming to be
  // "double"). Using LHSTy directly is both a correctness hardening and a
  // simplification -- no roundtrip needed at all.
  Value *CastVal = CastToType(Val, LHSTy, "assigncast", Loc);

  // Generate the Store instruction using LValue
  Value *Ptr = LHS->codegenLValue();
  if (!Ptr)
    return nullptr;
  Builder->CreateStore(CastVal, Ptr);

  // Assignment expressions usually return the value assigned (allows x = y = 5)
  return CastVal;
}

Value *CompoundAssignmentExprAST::codegen() {
  Value *Val = RHS->codegen();
  if (!Val) return nullptr;

  Value *Ptr = LHS->codegenLValue();
  if (!Ptr) return nullptr;

  const TurfType *LHSTy = getExprType(LHS.get());
  if (!LHSTy) {
    llvm::report_fatal_error("Internal error: Unknown LHS type for CompoundAssignment");
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(LHSTy);
  Value *LHSVal = Builder->CreateLoad(LLVMTy, Ptr, "compassign.load");

  TurfTypePtr VarType = getTurfTypeFromLLVM(LLVMTy);
  Value *CastRHS = CastToType(Val, VarType, "compassign.cast", Loc);

  Value *Result = nullptr;
  bool isDouble = LHSTy->isNamed("double");

  switch (Opcode) {
  case '+':
    Result = isDouble ? Builder->CreateFAdd(LHSVal, CastRHS, "addtmp")
                      : Builder->CreateAdd(LHSVal, CastRHS, "addtmp");
    break;
  case '-':
    Result = isDouble ? Builder->CreateFSub(LHSVal, CastRHS, "subtmp")
                      : Builder->CreateSub(LHSVal, CastRHS, "subtmp");
    break;
  case '*':
    Result = isDouble ? Builder->CreateFMul(LHSVal, CastRHS, "multmp")
                      : Builder->CreateMul(LHSVal, CastRHS, "multmp");
    break;
  case '/': {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *DivZeroBB = BasicBlock::Create(*TheContext, "divzero", TheFunction);
    BasicBlock *DivContBB = BasicBlock::Create(*TheContext, "divcont", TheFunction);

    Value *IsZero = isDouble
                        ? Builder->CreateFCmpOEQ(CastRHS, ConstantFP::get(*TheContext, APFloat(0.0)))
                        : Builder->CreateICmpEQ(CastRHS, ConstantInt::get(Type::getInt64Ty(*TheContext), 0));

    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);

    Builder->SetInsertPoint(DivZeroBB);
    CodegenIntrinsic("fail", {Builder->CreateGlobalString("Division by zero in compound assignment")}, Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    Builder->SetInsertPoint(DivContBB);
    Result = isDouble ? Builder->CreateFDiv(LHSVal, CastRHS, "divtmp")
                      : Builder->CreateSDiv(LHSVal, CastRHS, "divtmp");
    break;
  }
  case '%': {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *DivZeroBB = BasicBlock::Create(*TheContext, "modzero", TheFunction);
    BasicBlock *DivContBB = BasicBlock::Create(*TheContext, "modcont", TheFunction);

    Value *IsZero = isDouble
                        ? Builder->CreateFCmpOEQ(CastRHS, ConstantFP::get(*TheContext, APFloat(0.0)))
                        : Builder->CreateICmpEQ(CastRHS, ConstantInt::get(Type::getInt64Ty(*TheContext), 0));

    Builder->CreateCondBr(IsZero, DivZeroBB, DivContBB);

    Builder->SetInsertPoint(DivZeroBB);
    CodegenIntrinsic("fail", {Builder->CreateGlobalString("Modulo by zero in compound assignment")}, Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    Builder->SetInsertPoint(DivContBB);
    Result = isDouble ? Builder->CreateFRem(LHSVal, CastRHS, "modtmp")
                      : Builder->CreateSRem(LHSVal, CastRHS, "modtmp");
    break;
  }
  default:
    llvm::report_fatal_error("Unknown compound assignment operator");
  }

  Builder->CreateStore(Result, Ptr);
  return Result; // Compound assignment returns the new value
}

Value *PostfixExprAST::codegen() {
  Value *Ptr = Operand->codegenLValue();
  if (!Ptr) return nullptr;

  const TurfType *OpTy = getExprType(Operand.get());
  if (!OpTy) {
    llvm::report_fatal_error("Internal error: Unknown operand type for PostfixExpr");
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(OpTy);
  Value *OldVal = Builder->CreateLoad(LLVMTy, Ptr, "postfix.load");

  bool isDouble = OpTy->isNamed("double");
  Value *One = isDouble ? static_cast<Value *>(ConstantFP::get(*TheContext, APFloat(1.0)))
                        : static_cast<Value *>(ConstantInt::get(*TheContext, APInt(64, 1, true)));

  Value *NewVal = nullptr;
  if (Opcode == '+') {
    NewVal = isDouble ? Builder->CreateFAdd(OldVal, One, "inctmp")
                      : Builder->CreateAdd(OldVal, One, "inctmp");
  } else {
    NewVal = isDouble ? Builder->CreateFSub(OldVal, One, "dectmp")
                      : Builder->CreateSub(OldVal, One, "dectmp");
  }

  Builder->CreateStore(NewVal, Ptr);
  return OldVal; // Postfix returns the old value
}

Value *IfExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  bool IsBlockForm =
      dynamic_cast<BlockExprAST *>(Branches.front().Body.get()) != nullptr;
  bool HasElse = (ElseBody != nullptr);

  // Block-form if/elseif/else: statement, no PHI node needed.
  if (IsBlockForm) {
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

    for (size_t i = 0; i < Branches.size(); ++i) {
      Value *CondV = Branches[i].Cond->codegen();
      if (!CondV)
        return nullptr;
      CondV = CastToType(CondV, TurfBool(), "ifcond");

      BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
      BasicBlock *NextBB = BasicBlock::Create(*TheContext, "else");

      Builder->CreateCondBr(CondV, ThenBB, NextBB);

      // Emit then body
      Builder->SetInsertPoint(ThenBB);
      Branches[i].Body->codegen();
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);

      // NextBB becomes the insert point for the next condition or the
      // else/merge
      TheFunction->insert(TheFunction->end(), NextBB);
      Builder->SetInsertPoint(NextBB);
    }

    // We're now in the final "else" block
    if (HasElse) {
      ElseBody->codegen();
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);
    } else {
      // No else: just branch to merge
      Builder->CreateBr(MergeBB);
    }

    TheFunction->insert(TheFunction->end(), MergeBB);
    Builder->SetInsertPoint(MergeBB);
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

  // Ternary form: if cond then expr else expr
  Value *CondV = Branches[0].Cond->codegen();
  if (!CondV)
    return nullptr;
  CondV = CastToType(CondV, TurfBool(), "ifcond");

  BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
  BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

  Builder->CreateCondBr(CondV, ThenBB, ElseBB);
  Builder->SetInsertPoint(ThenBB);

  Value *ThenV = Branches[0].Body->codegen();
  if (!ThenV)
    return nullptr;
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateBr(MergeBB);
  ThenBB = Builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), ElseBB);
  Builder->SetInsertPoint(ElseBB);
  Value *ElseV = ElseBody->codegen();
  if (!ElseV)
    return nullptr;
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateBr(MergeBB);
  ElseBB = Builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);

  TurfTypePtr ThenType = getTurfTypeFromLLVM(ThenV->getType());
  TurfTypePtr ElseType = getTurfTypeFromLLVM(ElseV->getType());
  TurfTypePtr MergeType = getCommonType(ThenType, ElseType);

  ThenV = CastToType(ThenV, MergeType, "thencast");
  ElseV = CastToType(ElseV, MergeType, "elsecast");

  PHINode *PN = Builder->CreatePHI(ThenV->getType(), 2, "iftmp");
  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}

Value *UnaryExprAST::codegen() {
  Value *OperandV = Operand->codegen();
  if (!OperandV)
    return nullptr;

  TurfTypePtr OperandType = getTurfTypeFromLLVM(OperandV->getType());
  if (OperandType->isNamed("bool"))
    OperandV = CastToType(OperandV, TurfInt(), "boolneg");

  OperandType = getTurfTypeFromLLVM(OperandV->getType());

  switch (Opcode) {
  case '-':
    if (OperandType->isNamed("double"))
      return Builder->CreateFNeg(OperandV);
    if (OperandType->isNamed("int"))
      return Builder->CreateNeg(OperandV);
    llvm::report_fatal_error("Internal error: Unknown unary operand type");
    return nullptr;
  case '~':
    // Bitwise NOT: ~x == x XOR -1 (all bits set)
    if (!OperandType->isNamed("int")) {
      return ConstantInt::get(*TheContext, APInt(64, 0, true));
    }
    return Builder->CreateNot(OperandV, "bitnottmp");
  default:
    llvm::report_fatal_error("Internal error: Unknown unary operator");
    return nullptr;
  }
}

// Address-of: &expr — returns the lvalue address (alloca ptr) of the operand.
// The operand must be addressable (variable, field, or array element).
Value *AddressOfExprAST::codegen() {
  Value *LVal = Operand->codegenLValue();
  if (!LVal) {
    LogErrorAt(Loc, "Cannot take the address of a non-addressable expression."
                    "\n  Hint: Only variables, struct fields, and array elements"
                    " are addressable.");
    return nullptr;
  }
  return LVal;
}

Value *BlockExprAST::codegen() {
  // Enter new scope
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  Value *LastVal = nullptr;
  for (auto &Expr : Expressions) {
    LastVal = Expr->codegen();
  }

  // Exit scope
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitScope();
  }

  // An empty block is valid - it just produces no meaningful value.
  // Return a dummy zero so callers (e.g. IfExprAST::codegen) don't
  // mistake it for a codegen failure and bail out mid-IR-construction.
  if (!LastVal)
    LastVal = Constant::getNullValue(Type::getInt64Ty(*TheContext));

  return LastVal;
}

Value *SizeOfExprAST::codegen() {
  llvm::Type *LLVMTy = getLLVMType(TypeArg.get());
  if (!LLVMTy)
    return nullptr;
  uint64_t AllocSize = TheModule->getDataLayout().getTypeAllocSize(LLVMTy);
  return ConstantInt::get(Type::getInt64Ty(*TheContext), AllocSize);
}

Value *CastExprAST::codegen() {
  const TurfType *ExprTy = getExprType(Operand.get());
  
  Value *Val = Operand->codegen();
  if (!Val)
    return nullptr;
    
  TurfTypePtr SrcType;
  if (ExprTy) {
    // We only need the type info, so we can clone it or cast it if needed
    // But since CastExprAST only uses dynamic_cast and getName, we can just use a trick
    // to keep it compatible with TurfTypePtr.
    if (auto *Ptr = dynamic_cast<const TurfPointerType*>(ExprTy))
      SrcType = std::make_shared<TurfPointerType>(Ptr->getPointee());
    else if (ExprTy->isNamed("void*"))
      SrcType = std::make_shared<TurfPointerType>(TurfVoid());
    else if (ExprTy->isEnum()) {
      // Preserve the enum type pointer directly so isEnum() checks work below.
      SrcType = GlobalSymbolTable->LookupType(ExprTy->getName());
      if (!SrcType) SrcType = getTurfTypeFromLLVM(Val->getType());
    }
    else
      SrcType = getTurfTypeFromLLVM(Val->getType());
  } else {
    SrcType = getTurfTypeFromLLVM(Val->getType());
  }
  const std::string SrcName = ExprTy ? ExprTy->getName() : SrcType->getName();

  // Resolve DestType: NamedType may point to a registered enum/struct — look it
  // up at codegen time so isEnum() and isStruct() return the correct value.
  TurfTypePtr ResolvedDest = DestType;
  if (dynamic_cast<const NamedType *>(DestType.get()) && GlobalSymbolTable) {
    if (auto Registered = GlobalSymbolTable->LookupType(DestType->getName()))
      ResolvedDest = Registered;
  }
  const std::string DstName = ResolvedDest->getName();

  // Identity - no-op
  if (SrcName == DstName)
    return Val;

  // numeric → numeric
  if (SrcName == "int" && DstName == "double")
    return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), "itod");

  if (SrcName == "double" && DstName == "int")
    return Builder->CreateFPToSI(Val, Type::getInt64Ty(*TheContext), "dtoi");

  // Resolve both SrcType and DestType through GlobalSymbolTable so that
  // enum types (stored as NamedType at parse time) are correctly identified.
  // All primitive type names ("int", "double", etc.) return null from
  // LookupType and are handled by the existing branches.
  TurfTypePtr SrcEnum, DstEnum;
  if (GlobalSymbolTable) {
    if (auto R = GlobalSymbolTable->LookupType(SrcName))
      if (dynamic_cast<const TurfEnumType *>(R.get())) SrcEnum = R;
    if (auto R = GlobalSymbolTable->LookupType(DstName))
      if (dynamic_cast<const TurfEnumType *>(R.get())) DstEnum = R;
  }

  // enum → int : enums are stored as i64 — just zero-extend
  if (SrcEnum && DstName == "int")
    return Builder->CreateIntCast(Val, Type::getInt64Ty(*TheContext), false, "enum_to_int");

  // int → enum : reinterpret the integer as the enum's underlying type
  if (SrcName == "int" && DstEnum) {
    const TurfEnumType *ET = dynamic_cast<const TurfEnumType *>(DstEnum.get());
    if (ET) {
      llvm::Type *LLVMTy = getLLVMType(ET->getUnderlyingType());
      return Builder->CreateIntCast(Val, LLVMTy, false, "int_to_enum");
    }
  }

  // string → int  : call strtoll(str, &endptr, 10)
  if (SrcName == "string" && DstName == "int") {
    Function *StrtollF = TheModule->getFunction("strtoll");
    Type *I8Ptr = PointerType::get(*TheContext, 0);
    if (!StrtollF) {
      Type *I8PtrPtr = PointerType::get(I8Ptr->getContext(), 0);
      FunctionType *FT =
          FunctionType::get(Type::getInt64Ty(*TheContext),
                            {I8Ptr, I8PtrPtr, Type::getInt32Ty(*TheContext)},
                            /*isVarArg=*/false);
      StrtollF = Function::Create(FT, Function::ExternalLinkage, "strtoll",
                                  TheModule.get());
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    AllocaInst *EndPtrAlloca = TmpB.CreateAlloca(I8Ptr, 0, "endptr");

    Value *Base = ConstantInt::get(Type::getInt32Ty(*TheContext), 10);
    Value *Res =
        Builder->CreateCall(StrtollF, {Val, EndPtrAlloca, Base}, "strtoll_res");

    Value *EndPtrVal = Builder->CreateLoad(I8Ptr, EndPtrAlloca, "endptr_val");
    Value *IsSame = Builder->CreateICmpEQ(EndPtrVal, Val, "is_same_ptr");

    BasicBlock *ErrorBB =
        BasicBlock::Create(*TheContext, "cast_error", TheFunction);
    BasicBlock *ContBB = BasicBlock::Create(*TheContext, "cast_cont");

    Builder->CreateCondBr(IsSame, ErrorBB, ContBB);

    Builder->SetInsertPoint(ErrorBB);

    CodegenIntrinsic("fail", {Builder->CreateGlobalString(
        "Runtime Error: Invalid string to int conversion.")}, Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    TheFunction->insert(TheFunction->end(), ContBB);
    Builder->SetInsertPoint(ContBB);

    return Res;
  }

  // string → double : call strtod(str, nullptr)
  if (SrcName == "string" && DstName == "double") {
    Function *StrtodF = TheModule->getFunction("strtod");
    if (!StrtodF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      Type *I8PtrPtr = PointerType::get(I8Ptr->getContext(), 0);
      FunctionType *FT =
          FunctionType::get(Type::getDoubleTy(*TheContext), {I8Ptr, I8PtrPtr},
                            /*isVarArg=*/false);
      StrtodF = Function::Create(FT, Function::ExternalLinkage, "strtod",
                                 TheModule.get());
    }
    Value *NullPtr = ConstantPointerNull::get(
        PointerType::get(PointerType::get(*TheContext, 0)->getContext(), 0));
    return Builder->CreateCall(StrtodF, {Val, NullPtr}, "strtod_res");
  }

  // int → string : snprintf into a heap-allocated buffer.
  //
  // BUG FIX (was: TmpB.CreateAlloca):
  //   The previous implementation used alloca which places the buffer on the
  //   current function's stack frame. When this value is returned from a
  //   function (e.g. `fn make_label(n: int): string { return n to string; }`)
  //   the caller receives a pointer into a dead stack frame — silent UB.
  //   We now use malloc(32) so the buffer lives on the heap and survives the
  //   function return. The caller is responsible for freeing (no GC yet).
  if (SrcName == "int" && DstName == "string") {
    Function *SnprintfF = TheModule->getFunction("snprintf");
    if (!SnprintfF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT = FunctionType::get(
          Type::getInt32Ty(*TheContext),
          {I8Ptr, Type::getInt64Ty(*TheContext), I8Ptr}, /*isVarArg=*/true);
      SnprintfF = Function::Create(FT, Function::ExternalLinkage, "snprintf",
                                   TheModule.get());
    }
    Function *MallocF = TheModule->getFunction("malloc");
    if (!MallocF) {
      FunctionType *FT =
          FunctionType::get(PointerType::get(*TheContext, 0),
                            {Type::getInt64Ty(*TheContext)}, false);
      MallocF = Function::Create(FT, Function::ExternalLinkage, "malloc",
                                 TheModule.get());
    }
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    Value *Buf = Builder->CreateCall(MallocF, {BufSize}, "int_to_str_buf");
    Value *Fmt = Builder->CreateGlobalString("%lld", "intfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_int");
    return Buf;
  }

  // double → string : snprintf into a heap-allocated buffer.
  //
  // BUG FIX (was: TmpB.CreateAlloca):
  //   Same root cause as int → string above. Stack alloca returns dangling
  //   pointer when the result escapes the function. Fixed with malloc(32).
  if (SrcName == "double" && DstName == "string") {
    Function *SnprintfF = TheModule->getFunction("snprintf");
    if (!SnprintfF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT = FunctionType::get(
          Type::getInt32Ty(*TheContext),
          {I8Ptr, Type::getInt64Ty(*TheContext), I8Ptr}, /*isVarArg=*/true);
      SnprintfF = Function::Create(FT, Function::ExternalLinkage, "snprintf",
                                   TheModule.get());
    }
    Function *MallocF = TheModule->getFunction("malloc");
    if (!MallocF) {
      FunctionType *FT =
          FunctionType::get(PointerType::get(*TheContext, 0),
                            {Type::getInt64Ty(*TheContext)}, false);
      MallocF = Function::Create(FT, Function::ExternalLinkage, "malloc",
                                 TheModule.get());
    }
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    Value *Buf = Builder->CreateCall(MallocF, {BufSize}, "dbl_to_str_buf");
    Value *Fmt = Builder->CreateGlobalString("%g", "dblfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_dbl");
    return Buf;
  }

  // bool → string : select(val, "true", "false")
  if (SrcName == "bool" && DstName == "string") {
    Value *TrueStr = Builder->CreateGlobalString("true", "str_true");
    Value *FalseStr = Builder->CreateGlobalString("false", "str_false");
    return Builder->CreateSelect(Val, TrueStr, FalseStr, "bool_to_str");
  }

  // enum -> string : switch on the integer value, each case returns the name
  // IR layout:
  //   [current block]: switch val, default=defaultBB, [case i -> caseBB_i]
  //   defaultBB:  store "<unknown enum>", br mergeBB
  //   caseBB_0:   store "North",   br mergeBB
  //   caseBB_1:   store "East",    br mergeBB
  //   ...
  //   mergeBB:    phi [str from each case]
  if (SrcEnum && DstName == "string") {
    const TurfEnumType *ET = dynamic_cast<const TurfEnumType *>(SrcEnum.get());
    if (!ET) return nullptr;

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    llvm::Type *UnderlyingLLVMTy = getLLVMType(ET->getUnderlyingType());

    // Destination blocks
    BasicBlock *DefaultBB = BasicBlock::Create(*TheContext, "enum_str_def", TheFunction);
    BasicBlock *MergeBB   = BasicBlock::Create(*TheContext, "enum_str_merge", TheFunction);

    // Emit switch in current block
    SwitchInst *SI = Builder->CreateSwitch(Val, DefaultBB, ET->numVariants());

    // Default block: unknown enum value
    Builder->SetInsertPoint(DefaultBB);
    Value *UnknownStr = Builder->CreateGlobalString("<unknown enum>", "str_unk");
    Builder->CreateBr(MergeBB);

    // PHI in merge block — add default incoming first
    Builder->SetInsertPoint(MergeBB);
    PHINode *PN = Builder->CreatePHI(PointerType::getUnqual(*TheContext),
                                     ET->numVariants() + 1, "enum_str_phi");
    PN->addIncoming(UnknownStr, DefaultBB);

    // Per-variant case blocks
    for (const auto& V : ET->getVariants()) {
      BasicBlock *CaseBB = BasicBlock::Create(*TheContext, "enum_case_" + V, TheFunction);
      ConstantInt *CaseVal = cast<ConstantInt>(
          ConstantInt::get(UnderlyingLLVMTy, ET->getVariantValue(V)));
      SI->addCase(CaseVal, CaseBB);

      Builder->SetInsertPoint(CaseBB);
      Value *VariantStr = Builder->CreateGlobalString(V, "str_" + V);
      Builder->CreateBr(MergeBB);
      PN->addIncoming(VariantStr, CaseBB);
    }

    Builder->SetInsertPoint(MergeBB);
    return PN;
  }

  // string -> enum : strcmp chain starting from the current block
  // IR layout:
  //   [current block]:  cmp "North", condBr matchBB_0, nextBB_0
  //   matchBB_0:        br mergeBB  (value = 0)
  //   nextBB_0:         cmp "East",  condBr matchBB_1, nextBB_1
  //   ...
  //   nextBB_last:      br failBB
  //   failBB:           call fail()
  //   mergeBB:          phi [values from each match]
  if (SrcName == "string" && DstEnum) {
    const TurfEnumType *ET = dynamic_cast<const TurfEnumType *>(DstEnum.get());
    if (!ET) return nullptr;

    Function *StrcmpF = TheModule->getFunction("strcmp");
    if (!StrcmpF) {
      Type *I8Ptr = PointerType::get(*TheContext, 0);
      FunctionType *FT = FunctionType::get(Type::getInt32Ty(*TheContext),
                                           {I8Ptr, I8Ptr}, false);
      StrcmpF = Function::Create(FT, Function::ExternalLinkage, "strcmp",
                                 TheModule.get());
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    llvm::Type *UnderlyingLLVMTy = getLLVMType(ET->getUnderlyingType());

    // KEY: save the entry block BEFORE moving the insert point anywhere.
    BasicBlock *EntryBB = Builder->GetInsertBlock();

    // Create terminal blocks (appended to function, filled below)
    BasicBlock *FailBB  = BasicBlock::Create(*TheContext, "enum_parse_fail",  TheFunction);
    BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "enum_parse_merge", TheFunction);

    // Build PHI in MergeBB
    Builder->SetInsertPoint(MergeBB);
    PHINode *PN = Builder->CreatePHI(UnderlyingLLVMTy, ET->numVariants(), "enum_parse_phi");

    // Build fail block
    Builder->SetInsertPoint(FailBB);
    CodegenIntrinsic("fail", {Builder->CreateGlobalString(
        "Runtime Error: Invalid string to enum conversion.")}, Loc);
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateUnreachable();

    // Build the strcmp chain directly from EntryBB (saved before any SetInsertPoint calls)
    BasicBlock *CurrentBB = EntryBB;
    for (const auto& V : ET->getVariants()) {
      BasicBlock *MatchBB = BasicBlock::Create(*TheContext, "enum_parse_match_" + V, TheFunction, FailBB);
      BasicBlock *NextBB  = BasicBlock::Create(*TheContext, "enum_parse_next_"  + V, TheFunction, FailBB);

      Builder->SetInsertPoint(CurrentBB);
      Value *VariantStr = Builder->CreateGlobalString(V, "cmpstr_" + V);
      Value *CmpRes  = Builder->CreateCall(StrcmpF, {Val, VariantStr}, "strcmp_" + V);
      Value *IsMatch = Builder->CreateICmpEQ(
          CmpRes, ConstantInt::get(Type::getInt32Ty(*TheContext), 0), "match_" + V);
      Builder->CreateCondBr(IsMatch, MatchBB, NextBB);

      Builder->SetInsertPoint(MatchBB);
      ConstantInt *CaseVal = cast<ConstantInt>(
          ConstantInt::get(UnderlyingLLVMTy, ET->getVariantValue(V)));
      Builder->CreateBr(MergeBB);
      PN->addIncoming(CaseVal, MatchBB);

      CurrentBB = NextBB;
    }

    // Last "next" block falls to fail
    Builder->SetInsertPoint(CurrentBB);
    Builder->CreateBr(FailBB);

    Builder->SetInsertPoint(MergeBB);
    return PN;
  }

    // (moved unsupported string cast down)

  // int -> pointer : inttoptr
  if (SrcName == "int" && dynamic_cast<const TurfPointerType*>(DestType.get())) {
    return Builder->CreateIntToPtr(Val, getLLVMType(DestType.get()), "inttoptr");
  }

  // pointer -> pointer : bitcast
  bool isSrcPtr = (ExprTy && dynamic_cast<const TurfPointerType*>(ExprTy)) || SrcName == "ptr" || SrcName == "string";
  bool isDstPtr = dynamic_cast<const TurfPointerType*>(DestType.get()) || DstName == "ptr" || DstName == "string";
  
  if (isSrcPtr && isDstPtr) {
    return Builder->CreateBitCast(Val, getLLVMType(DestType.get()), "ptrcast");
  }

  // Unsupported → string
  if (DstName == "string") {
    StringConversionError(Loc, SrcName.c_str()).raise();
    return nullptr;
  }

  // Everything else is unsupported
  TypeError(Loc, std::string("Cannot explicitly cast '") + SrcName + "' to '" +
                     DstName + "'")
      .raise();
  return nullptr;
}

Value *WhileExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  BasicBlock *LoopCondBB =
      BasicBlock::Create(*TheContext, "loopcond", TheFunction);
  BasicBlock *LoopBodyBB = BasicBlock::Create(*TheContext, "loopbody");
  BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "afterloop");

  Builder->CreateBr(LoopCondBB);
  Builder->SetInsertPoint(LoopCondBB);

  Value *CondV = Cond->codegen();
  if (!CondV)
    return nullptr;

  CondV = CastToType(CondV, TurfBool(), "loopcond");

  // Conditional Branch: if true -> Body, else -> After
  Builder->CreateCondBr(CondV, LoopBodyBB, AfterBB);

  // Loop Body Block
  TheFunction->insert(TheFunction->end(), LoopBodyBB);
  Builder->SetInsertPoint(LoopBodyBB);

  LoopBlocks.push_back({LoopCondBB, AfterBB});

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    return nullptr;
  }

  LoopBlocks.pop_back();

  // Jump back to the condition to loop again
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(LoopCondBB);
  }

  // After Loop Block
  TheFunction->insert(TheFunction->end(), AfterBB);
  Builder->SetInsertPoint(AfterBB);

  // While loops always return 0.0
  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}

// ForExprAST::codegen : desugars to init + while(cond) { body; step }
Value *ForExprAST::codegen() {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  //  1. Init: declare loop variable with start value.
  //     Evaluate start/end in the outer scope first — they cannot reference
  //     the loop variable (it doesn't exist yet).
  Value *StartV = Start->codegen();
  if (!StartV)
    return nullptr;

  // Determine the loop variable type from the start value
  TurfTypePtr VarType =
      StartV->getType()->isDoubleTy() ? TurfDouble() : TurfInt();

  StartV = CastToType(StartV, VarType, "forstart");

  AllocaInst *Alloca =
      CreateEntryBlockAlloca(TheFunction, VarName, VarType.get());
  Builder->CreateStore(StartV, Alloca);

  // Push a new scope in GlobalSymbolTable so the loop variable doesn't leak
  // out into the surrounding scope after the loop ends.
  // Previously, DeclareSymbol was called without EnterScope, causing the loop
  // variable to persist in the caller's scope — now fixed.
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  // Register loop variable in symbol table and NamedValues.
  // Save old NamedValues binding for backward-compatible restore after loop.
  auto OldBinding = NamedValues.find(VarName);
  VarInfo SavedBinding;
  bool HadOldBinding = false;
  if (OldBinding != NamedValues.end()) {
    SavedBinding = OldBinding->second;
    HadOldBinding = true;
  }
  NamedValues[VarName] = {Alloca, VarType};

  if (GlobalSymbolTable) {
    GlobalSymbolTable->DeclareSymbol(VarName, VarType, Loc, Alloca);
  }

  //  2. Evaluate end value
  Value *EndV = End->codegen();
  if (!EndV) {
    if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
    return nullptr;
  }
  EndV = CastToType(EndV, VarType, "forend");

  // Store end value so it doesn't get re-evaluated each iteration
  AllocaInst *EndAlloca =
      CreateEntryBlockAlloca(TheFunction, VarName + ".end", VarType.get());
  Builder->CreateStore(EndV, EndAlloca);

  //  3. Determine loop direction: start < end → ascending, else descending
  Value *Ascending;
  if (VarType->isNamed("int")) {
    Ascending = Builder->CreateICmpSLT(StartV, EndV, "ascending");
  } else {
    Ascending = Builder->CreateFCmpOLT(StartV, EndV, "ascending");
  }

  // Store direction flag
  AllocaInst *AscAlloca =
      CreateEntryBlockAlloca(TheFunction, VarName + ".asc", TurfBool().get());
  Builder->CreateStore(Ascending, AscAlloca);

  //  4. Create basic blocks
  BasicBlock *LoopCondBB =
      BasicBlock::Create(*TheContext, "for.cond", TheFunction);
  BasicBlock *LoopBodyBB = BasicBlock::Create(*TheContext, "for.body");
  BasicBlock *LoopStepBB = BasicBlock::Create(*TheContext, "for.step");
  BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "for.end");

  Builder->CreateBr(LoopCondBB);

  //  5. Loop condition
  Builder->SetInsertPoint(LoopCondBB);
  Value *CurVar =
      Builder->CreateLoad(getLLVMType(VarType.get()), Alloca, VarName);
  Value *EndVal = Builder->CreateLoad(getLLVMType(VarType.get()), EndAlloca,
                                      VarName + ".end");
  Value *AscFlag =
      Builder->CreateLoad(Type::getInt1Ty(*TheContext), AscAlloca, "asc");

  // If ascending: cond = (i < end), else: cond = (i > end)
  Value *CondAsc, *CondDesc;
  if (VarType->isNamed("int")) {
    CondAsc = Builder->CreateICmpSLT(CurVar, EndVal, "lt");
    CondDesc = Builder->CreateICmpSGT(CurVar, EndVal, "gt");
  } else {
    CondAsc = Builder->CreateFCmpOLT(CurVar, EndVal, "lt");
    CondDesc = Builder->CreateFCmpOGT(CurVar, EndVal, "gt");
  }

  Value *CondV = Builder->CreateSelect(AscFlag, CondAsc, CondDesc, "forcond");
  Builder->CreateCondBr(CondV, LoopBodyBB, AfterBB);

  //  6. Loop body
  TheFunction->insert(TheFunction->end(), LoopBodyBB);
  Builder->SetInsertPoint(LoopBodyBB);

  // Push {StepBB, AfterBB} so 'continue' jumps to step, 'break' jumps to after
  LoopBlocks.push_back({LoopStepBB, AfterBB});

  if (!Body->codegen()) {
    LoopBlocks.pop_back();
    if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
    return nullptr;
  }

  LoopBlocks.pop_back();

  // Fall through to step block
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(LoopStepBB);
  }

  // 7. Loop step
  TheFunction->insert(TheFunction->end(), LoopStepBB);
  Builder->SetInsertPoint(LoopStepBB);

  if (!Step->codegen()) {
    if (GlobalSymbolTable) GlobalSymbolTable->ExitScope();
    return nullptr;
  }

  // Branch back to condition
  if (!Builder->GetInsertBlock()->getTerminator()) {
    Builder->CreateBr(LoopCondBB);
  }

  // 8. After loop
  TheFunction->insert(TheFunction->end(), AfterBB);
  Builder->SetInsertPoint(AfterBB);

  // Exit the for-loop scope — loop variable is no longer visible.
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitScope();
  }

  // Restore old NamedValues binding for backward-compatibility.
  if (HadOldBinding) {
    NamedValues[VarName] = SavedBinding;
  } else {
    NamedValues.erase(VarName);
  }

  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}

// ─── CompareExprAST::codegen ─────────────────────────────────────────────────
//
// Two code-generation paths:
//
//  Integer / enum subjects  (SubjectIsString == false)
//  ─────────────────────────
//  Uses LLVM SwitchInst.  Each arm adds one or more case constants; the
//  backend promotes this to a jump table when the value range is dense enough.
//  The default target is either the 'otherwise' body block or the merge block.
//
//  String subjects  (SubjectIsString == true)
//  ────────────────────────────────────────────
//  Falls back to a strcmp chain (SwitchInst only works for integers).
//  For each arm we call strcmp(subject, pattern) == 0.  Multiple patterns in
//  one stacked arm are OR-ed: if any pattern matches we jump to the body.
//
//  Both paths guarantee no fall-through: every body always branches to MergeBB.
//
Value *CompareExprAST::codegen() {
  // Codegen the subject exactly once
  Value *SubjectV = Subject->codegen();
  if (!SubjectV)
    return nullptr;

  if (Arms.empty() && !OtherBody) {
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "compare.end");

  // ─── String subject: strcmp chain ────────────────────────────────────────
  if (SubjectIsString) {
    // Look up (or declare) strcmp
    FunctionType *StrcmpTy = FunctionType::get(
        Type::getInt32Ty(*TheContext),
        {PointerType::get(*TheContext, 0), PointerType::get(*TheContext, 0)},
        /*isVarArg=*/false);
    FunctionCallee StrcmpFn =
        TheModule->getOrInsertFunction("strcmp", StrcmpTy);

    // Build: otherwise body block (or merge if absent)
    BasicBlock *DefaultBB = OtherBody
                                ? BasicBlock::Create(*TheContext, "compare.otherwise")
                                : MergeBB;

    for (size_t i = 0; i < Arms.size(); ++i) {
      const MatchesArm &Arm = Arms[i];
      BasicBlock *ArmBodyBB =
          BasicBlock::Create(*TheContext, "compare.arm." + std::to_string(i), TheFunction);
      BasicBlock *NoMatchBB =
          BasicBlock::Create(*TheContext, "compare.nomatch." + std::to_string(i));

      // For each pattern in this arm: strcmp(subj, pat) == 0 → jump to body
      BasicBlock *CheckBB = Builder->GetInsertBlock();
      (void)CheckBB;
      Value *AnyMatch = ConstantInt::getFalse(*TheContext);

      for (size_t j = 0; j < Arm.Values.size(); ++j) {
        Value *PatV = Arm.Values[j]->codegen();
        if (!PatV)
          return nullptr;
        Value *Cmp = Builder->CreateCall(
            StrcmpFn, {SubjectV, PatV}, "strcmp.res." + std::to_string(j));
        Value *IsZero = Builder->CreateICmpEQ(
            Cmp, ConstantInt::get(Type::getInt32Ty(*TheContext), 0),
            "strcmp.eq." + std::to_string(j));

        if (j == 0) {
          AnyMatch = IsZero;
        } else {
          // Emit short-circuit OR:
          //   if AnyMatch already true → don't evaluate more strcmp
          //   For simplicity we use a phi-free OR here (non-short-circuit)
          //   This is correct because patterns are literals with no side effects.
          AnyMatch = Builder->CreateOr(AnyMatch, IsZero, "or.pat");
        }
      }

      Builder->CreateCondBr(AnyMatch, ArmBodyBB, NoMatchBB);

      // Arm body
      Builder->SetInsertPoint(ArmBodyBB);
      if (!Arm.Body->codegen())
        return nullptr;
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);

      // Next arm or default
      TheFunction->insert(TheFunction->end(), NoMatchBB);
      Builder->SetInsertPoint(NoMatchBB);
    }

    // Fall through to otherwise or merge
    if (OtherBody) {
      // The last NoMatchBB needs to branch to DefaultBB
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(DefaultBB);

      TheFunction->insert(TheFunction->end(), DefaultBB);
      Builder->SetInsertPoint(DefaultBB);
      if (!OtherBody->codegen())
        return nullptr;
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);
    } else {
      // No otherwise: current block (NoMatchBB or original) branches to merge
      if (!Builder->GetInsertBlock()->getTerminator())
        Builder->CreateBr(MergeBB);
    }

    TheFunction->insert(TheFunction->end(), MergeBB);
    Builder->SetInsertPoint(MergeBB);
    return Constant::getNullValue(Type::getInt64Ty(*TheContext));
  }

  // ─── Integer / enum subject: SwitchInst ──────────────────────────────────
  //
  // Cast subject to i64 (the natural integer type in TurfLang) so SwitchInst
  // cases are always the same width regardless of whether the subject was
  // declared as bool or a named enum.
  Value *IntSubjectV = CastToType(SubjectV, TurfInt(), "switch.subj");

  // Default block: either the 'otherwise' body or the merge block.
  BasicBlock *DefaultBB = OtherBody
                              ? BasicBlock::Create(*TheContext, "compare.otherwise")
                              : MergeBB;

  SwitchInst *SI = Builder->CreateSwitch(IntSubjectV, DefaultBB,
                                         static_cast<unsigned>(Arms.size()));

  // One basic block per arm (shared by all stacked patterns in that arm).
  for (size_t i = 0; i < Arms.size(); ++i) {
    const MatchesArm &Arm = Arms[i];
    BasicBlock *ArmBodyBB =
        BasicBlock::Create(*TheContext, "compare.arm." + std::to_string(i),
                           TheFunction);

    // Add all pattern constants as cases targeting this block.
    for (const auto &PatExpr : Arm.Values) {
      // Constant-fold the pattern.  Patterns must be integer literals or
      // constant-foldable expressions (TypeChecker already enforced int type).
      Value *PatV = PatExpr->codegen();
      if (!PatV)
        return nullptr;

      auto *PatConst = dyn_cast<ConstantInt>(PatV);
      if (!PatConst) {
        // Cast to i64 so the switch width matches
        PatV = CastToType(PatV, TurfInt(), "patcast");
        PatConst = dyn_cast<ConstantInt>(PatV);
      }
      if (!PatConst) {
        SemanticError(Arm.Loc,
                      "matches pattern must be a compile-time integer constant.")
            .raise();
        return nullptr;
      }
      // Normalise to i64 width
      APInt CaseVal = PatConst->getValue().sextOrTrunc(64);
      SI->addCase(
          llvm::cast<ConstantInt>(
              ConstantInt::get(Type::getInt64Ty(*TheContext), CaseVal)),
          ArmBodyBB);
    }

    // Emit arm body
    Builder->SetInsertPoint(ArmBodyBB);
    if (!Arm.Body->codegen())
      return nullptr;
    // Guarantee no fall-through: always branch to merge
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateBr(MergeBB);
  }

  // Emit otherwise body (the switch's default target)
  if (OtherBody) {
    TheFunction->insert(TheFunction->end(), DefaultBB);
    Builder->SetInsertPoint(DefaultBB);
    if (!OtherBody->codegen())
      return nullptr;
    if (!Builder->GetInsertBlock()->getTerminator())
      Builder->CreateBr(MergeBB);
  }

  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}

Value *BreakExprAST::codegen() {
  if (LoopBlocks.empty()) {
    ControlFlowError(Loc, "'break' used outside of a loop").raise();
    return nullptr;
  }

  Builder->CreateBr(LoopBlocks.back().second);

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *DeadBB =
      BasicBlock::Create(*TheContext, "after_break", TheFunction);
  Builder->SetInsertPoint(DeadBB);

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}

Value *ContinueExprAST::codegen() {
  if (LoopBlocks.empty()) {
    ControlFlowError(Loc, "'continue' used outside of a loop").raise();
    return nullptr;
  }

  Builder->CreateBr(LoopBlocks.back().first);

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *DeadBB =
      BasicBlock::Create(*TheContext, "after_continue", TheFunction);
  Builder->SetInsertPoint(DeadBB);

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}

// FuncDefExprAST::codegen
// Two-pass strategy:
//   Pass 1 (prototype): if no LLVM Function yet, create it (no body).
//   Pass 2 (body):      if function has no body yet, fill it in.
// Calling codegen() twice is what makes forward calls work: main.cpp's
// pre-pass creates prototypes, then the normal loop fills bodies.
Value *FuncDefExprAST::codegen() {
  std::string MangledName = Name;
  std::string Prefix = "";
  if (GlobalSymbolTable && GlobalSymbolTable->IsInModule()) {
    Prefix = GlobalSymbolTable->GetCurrentModule() + "_";
    MangledName = Prefix + Name;
  }

  if (Name != "main") {
    MangledName = "turf_" + Prefix + Name;
    for (const auto &P : Params) {
      MangledName += "_" + P.Type->getMangledName();
    }
  }

  // Build/look up the LLVM prototype
  Function *TheFunc = TheModule->getFunction(MangledName);

  if (!TheFunc) {
    // Save param types and return type
    std::vector<TurfTypePtr> PTypes;
    for (const auto &P : Params)
      PTypes.push_back(P.Type);
    FuncParamTypes[MangledName] = PTypes;
    FuncReturnTypes[MangledName] = ReturnType;

    // Build the LLVM FunctionType
    std::vector<Type *> ParamTypes;
    for (const auto &P : Params)
      ParamTypes.push_back(getLLVMType(P.Type.get()));

    FunctionType *FT =
        FunctionType::get(getLLVMType(ReturnType.get()), ParamTypes,
                          /*isVarArg=*/false);
    auto Linkage = (Name == "main") ? Function::ExternalLinkage
                                    : Function::InternalLinkage;
    TheFunc = Function::Create(FT, Linkage, MangledName, TheModule.get());

    // Name the arguments
    unsigned Idx = 0;
    for (auto &Arg : TheFunc->args())
      Arg.setName(Params[Idx++].Name);
  } else {
    // If the function already has a body, this is a duplicate definition
    if (!TheFunc->empty()) {
      SemanticError(Loc, "Duplicate function definition: '" + Name + "'")
          .raise();
      return nullptr;
    }
  }

  // If this is a prototype-only pass (pre-pass sub-pass 3a), stop here.
  // The body will be compiled in sub-pass 3b once all prototypes exist.
  if (g_protoOnlyPass) {
    return TheFunc;
  }

  // Store full param types for call-site checking
  FuncParamTypes[MangledName] = {};
  for (const auto &P : Params)
    FuncParamTypes[MangledName].push_back(P.Type);

  // Phase 2: also register this function in the SymbolTable's module registry.
  // This populates the hashmap used by MethodCallExprAST to do namespace-aware
  // overload resolution without string-prefix scanning.
  if (GlobalSymbolTable && Name != "main") {
    FunctionSymbol FSym;
    FSym.BaseName    = Name; // user-visible name, e.g. "printline"
    FSym.MangledName = MangledName; // e.g. "turf_io_printline_int"
    FSym.Arity       = static_cast<int>(Params.size());
    FSym.ReturnType  = ReturnType;
    for (const auto &P : Params)
      FSym.ParamTypes.push_back(P.Type);

    // Determine which module this function belongs to.
    // IsInModule() is true when we're currently inside a ModuleImportExprAST
    // codegen block (i.e. compiling a library like io.tr or math.tr).
    std::string ModuleName =
        (GlobalSymbolTable->IsInModule())
            ? GlobalSymbolTable->GetCurrentModule()
            : "main";
    GlobalSymbolTable->RegisterModuleFunction(ModuleName, FSym);
  }

  // Fill in the function body
  BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunc);

  // Save caller context
  auto SavedNamedValues = NamedValues;
  auto SavedReturnType = CurrentFuncReturnType;
  auto SavedFunction = CurrentFunction;
  auto *SavedInsertBlock = Builder->GetInsertBlock();
  auto SavedInsertPoint = Builder->GetInsertPoint();

  // Switch to the new function
  Builder->SetInsertPoint(BB);
  NamedValues.clear();
  CurrentFuncReturnType = ReturnType;
  CurrentFunction = TheFunc;

  // Enter a new scope for the function body
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterScope();
  }

  // Alloca each parameter and add to NamedValues and GlobalSymbolTable
  std::vector<SymbolID> ParamIDs;
  for (auto &Arg : TheFunc->args()) {
    TurfTypePtr ParamTurfType = TurfInt(); // fallback
    for (const auto &P : Params)
      if (P.Name == Arg.getName())
        ParamTurfType = P.Type;

    AllocaInst *Alloca = CreateEntryBlockAlloca(
        TheFunc, std::string(Arg.getName()), ParamTurfType.get());
    Builder->CreateStore(&Arg, Alloca);

    TurfTypePtr RegisteredType = ParamTurfType;

    NamedValues[std::string(Arg.getName())] = {Alloca, RegisteredType};

    // Add parameter to symbol table
    if (GlobalSymbolTable) {
      SourceLocation ParamLoc = Loc;
      SymbolID id = GlobalSymbolTable->DeclareSymbol(std::string(Arg.getName()),
                                       RegisteredType, ParamLoc, Alloca);
      ParamIDs.push_back(id);
    }
  }

  // Lint the function body before codegen
  LintExpression(Body.get());

  // Codegen the body
  Body->codegen();

  // Collect call edges from the AST for this function (safer than scanning CFGs)
  if (Body) {
    std::function<void(const ExprAST*)> scanCalls = [&](const ExprAST *N) {
      if (!N) return;
      if (auto FC = dynamic_cast<const FuncCallExprAST*>(N)) {
        if (auto CalleeVar = dynamic_cast<const VariableExprAST*>(FC->getCallee())) {
          std::string callee = CalleeVar->getName();
          AddCallEdge(Name, callee);
        }
      }
      if (auto B = dynamic_cast<const BinaryExprAST*>(N)) { scanCalls(B->getLHS()); scanCalls(B->getRHS()); }
      else if (auto U = dynamic_cast<const UnaryExprAST*>(N)) scanCalls(U->getOperand());
      else if (auto As = dynamic_cast<const AssignmentExprAST*>(N)) { scanCalls(As->getLHS()); scanCalls(As->getRHS()); }
      else if (auto Vd = dynamic_cast<const VarDeclExprAST*>(N)) scanCalls(Vd->getInitializer());
      else if (auto Bl = dynamic_cast<const BlockExprAST*>(N)) for (const auto &e : Bl->getExpressions()) scanCalls(e.get());
      else if (auto If = dynamic_cast<const IfExprAST*>(N)) { for (const auto &Br : If->getBranches()) { scanCalls(Br.Cond.get()); scanCalls(Br.Body.get()); } scanCalls(If->getElse()); }
    };
    scanCalls(Body.get());
  }

  // Build and analyze CFG for flow diagnostics
  if (Body) {
    CFGBuilder CFGBuilder;
    auto FuncCFG = CFGBuilder.buildCFG(Name, ReturnType, Body.get(), Loc);

    // Store CFG for later use (push before analyses so it survives raises)
    GlobalCFGs.push_back(std::move(FuncCFG));

    // Run flow analysis and report diagnostics (will raise error for missing
    // returns in non-void)
    CFG *CfgPtr = GlobalCFGs.back().get();
    CfgPtr->reportFlowDiagnostics();

    // Run definite-assignment analysis (parameters are considered assigned)
    DefiniteAssignmentPass DAP;
    DAP.runOnCFG(*CfgPtr, ParamIDs);

    // Run value-range (constant) propagation
    ValueRangePass VRP;
    VRP.runOnCFG(*CfgPtr, ParamIDs);

    // Run alias/lifetime analysis (detect use-after-free, double-free)
    AliasAnalysisPass AAP;
    AAP.runOnCFG(*CfgPtr, ParamIDs);
  }

  // Emit a return if the block has no terminator yet.
  // IMPORTANT: ReturnExprAST::codegen() leaves the builder pointing at a
  // fresh dead "after_ret" block (no predecessors, no terminator). We must
  // erase those dead blocks instead of treating them as missing-return paths.
  BasicBlock *CurBB = Builder->GetInsertBlock();
  if (!CurBB->getTerminator()) {
    bool IsDead = (CurBB != BB) && llvm::pred_empty(CurBB);
    if (IsDead) {
      // Dead block left behind by a return statement — erase it cleanly.
      CurBB->eraseFromParent();
    } else if (ReturnType->isVoid()) {
      Builder->CreateRetVoid();
    } else {
      // CFG already reported the error, but we still need to generate valid IR
      Builder->CreateRet(Constant::getNullValue(getLLVMType(ReturnType.get())));
    }
  }

  verifyFunction(*TheFunc);

  // Exit function scope
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitScope();
  }

  // Restore caller context
  NamedValues = SavedNamedValues;
  CurrentFuncReturnType = SavedReturnType;
  CurrentFunction = SavedFunction;
  if (SavedInsertBlock)
    Builder->SetInsertPoint(SavedInsertBlock, SavedInsertPoint);

  // Function definitions are statements; they do not produce a value.
  return nullptr;
}

// ExternFuncDeclExprAST::codegen
// Emits an LLVM ExternalLinkage prototype (no body block).
// Mirrors what `declare i64 @strlen(ptr)` looks like in LLVM IR.
Value *ExternFuncDeclExprAST::codegen() {
  // If a declaration with this name already exists in the module (e.g. because
  // the user declared it twice, or a builtin already emitted it), reuse it.
  if (Function *Existing = TheModule->getFunction(Name_)) {
    return Existing;
  }

  // Build the LLVM parameter type list.
  std::vector<Type *> ParamLLVMTypes;
  std::vector<TurfTypePtr> ParamTurfTypes;
  for (const auto &[PName, PType] : Params_) {
    ParamLLVMTypes.push_back(getLLVMType(PType.get()));
    ParamTurfTypes.push_back(PType);
  }

  // Build the LLVM function type.
  Type *RetLLVMType = getLLVMType(ReturnType_.get());
  FunctionType *FT =
      FunctionType::get(RetLLVMType, ParamLLVMTypes, /*isVarArg=*/IsVarArg_);

  // Create with ExternalLinkage — no body block is created.
  Function *F =
      Function::Create(FT, Function::ExternalLinkage, Name_, TheModule.get());

  // Name the parameters for IR readability.
  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(Params_[Idx++].first);

  // Register in the type caches so call sites can look up param/return types.
  FuncParamTypes[Name_] = ParamTurfTypes;
  FuncReturnTypes[Name_] = ReturnType_;

  // Register in the GlobalSymbolTable so FuncCallExprAST can find this
  // function by its user-visible name (without mangling).
  if (GlobalSymbolTable) {
    FunctionSymbol FSym;
    FSym.BaseName    = Name_;
    FSym.MangledName = Name_; // extern fns use their C name directly
    FSym.Arity       = static_cast<int>(Params_.size());
    FSym.ReturnType  = ReturnType_;
    FSym.IsVarArg    = IsVarArg_;  // propagate variadic flag to the symbol
    for (const auto &[PName, PType] : Params_)
      FSym.ParamTypes.push_back(PType);
    GlobalSymbolTable->RegisterModuleFunction("main", FSym);
  }

  // If this extern fn specifies a library (extern from "ssl" fn ...),
  // record it so the linker can append -lssl automatically.
  if (!LibName_.empty())
    g_RequiredLibs.insert(LibName_);

  return F;
}


// ReturnExprAST::codegen
Value *ReturnExprAST::codegen() {
  if (CurrentFunction == nullptr) {
    ControlFlowError(Loc, "'return' used outside of a function").raise();
    return nullptr;
  }

  // Mark scope as having early exit for unreachable code detection
  if (GlobalSymbolTable) {
    GlobalSymbolTable->MarkEarlyExit();
  }

  if (!Val) {
    // empty return; TypeChecker has verified this is a void function.
    Builder->CreateRetVoid();
  } else {
    // return <expr>; TypeChecker has verified the expression type.
    Value *RetVal = Val->codegen();
    if (!RetVal)
      return nullptr;

    // Type validation and compatibility are handled by the TypeChecker.

    RetVal = CastToType(RetVal, CurrentFuncReturnType, "retcast", Loc);
    Builder->CreateRet(RetVal);
  }

  // After a terminator, create a fresh unreachable block so the IR builder
  // has a valid insert point for any subsequent statements in the same block.
  BasicBlock *DeadBB =
      BasicBlock::Create(*TheContext, "after_ret", CurrentFunction);
  Builder->SetInsertPoint(DeadBB);

  // Return a dummy null so the AST node has a non-null value
  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}

Value *FuncCallExprAST::codegen() {
  auto *Var = dynamic_cast<VariableExprAST *>(Callee.get());
  if (!Var) {
    llvm::report_fatal_error("Internal error: First-class functions are not supported yet.");
    return nullptr;
  }
  std::string CalleeName = Var->getName();

  // For malloc/realloc/free: if the user declared an extern fn with that name,
  // let it flow through the normal symbol-table path below. Only use the
  // builtin path when no user declaration exists.
  bool UserDeclaredExtern = (CalleeName == "malloc" || CalleeName == "realloc" ||
                              CalleeName == "free") &&
                             TheModule->getFunction(CalleeName) != nullptr;

  if (!UserDeclaredExtern &&
      (CalleeName == "typeof" || CalleeName == "lengthof" ||
       CalleeName == "assert" || CalleeName == "fail" ||
       CalleeName == "malloc" || CalleeName == "realloc" ||
       CalleeName == "free")) {


    std::vector<Value *> ArgVals;
    for (auto &Arg : Args) {
      Value *V = Arg->codegen();
      if (!V)
        return nullptr;
      ArgVals.push_back(V);
    }
    
    // Phase 4 Note 14: typeof should return the compile-time type of the expression,
    // avoiding the loss of generic type information in LLVM IR.
    if (CalleeName == "typeof") {
      std::string TypeNameStr = "unknown";
      if (const TurfType *ExprTy = getExprType(Args[0].get())) {
        TypeNameStr = turfTypeName(ExprTy);
      } else {
        // Fallback to LLVM type if AST type resolution failed
        Type *Ty = ArgVals[0]->getType();
        if (Ty->isDoubleTy()) {
          TypeNameStr = "double";
        } else if (Ty->isIntegerTy(64)) {
          TypeNameStr = "int";
        } else if (Ty->isIntegerTy(1)) {
          TypeNameStr = "bool";
        } else if (Ty->isPointerTy()) {
          TypeNameStr = "string";
        } else if (Ty->isVoidTy()) {
          TypeNameStr = "void";
        }
      }
      return Builder->CreateGlobalString(TypeNameStr, "typename");
    }

    return CodegenIntrinsic(CalleeName, ArgVals, Loc);
  }

  std::vector<Value *> EvaluatedArgs;
  for (auto &Arg : Args) {
    Value *V = Arg->codegen();
    if (!V)
      return nullptr;
    EvaluatedArgs.push_back(V);
  }

  // ── Issue #9: Codegen trusts the Resolver ────────────────────────────────
  // ResolvedCallTarget was set by Resolver::visitFuncCall (mandatory pass).
  // Codegen reads the pre-computed mangled name blindly — no scanning, no
  // FuncParamTypes lookups, no arity checks.  Those are the Resolver's job.
  //
  // typeof / lengthof are handled above (language-level bare intrinsics that
  // never pass through the Resolver's function registry).
  std::string MangledName;
  if (CalleeName == "main") {
    MangledName = "main";
  } else if (ResolvedCallTarget) {
    MangledName = ResolvedCallTarget->mangledName();
  } else {
    // ResolvedCallTarget is null: this can happen for extern fns whose
    // raw C name is used directly.  Fall back to the user-visible name.
    MangledName = CalleeName;
  }

  Function *CalleeF = TheModule->getFunction(MangledName);
  if (!CalleeF) {
    // Fallback: extern fn declarations register under the raw C name, not the
    // mangled name (e.g. `strlen`, not `turf_strlen_string`).  Try the user-
    // visible callee name before declaring an internal compiler error.
    CalleeF = TheModule->getFunction(CalleeName);
  }
  if (!CalleeF) {
    // This path should be unreachable if the Resolver ran correctly.
    // If hit, it's an internal compiler bug — not a user error.
    SemanticError(Loc, "Internal error: LLVM function '" + MangledName +
                           "' not found after successful name resolution. "
                           "This is a compiler bug.")
        .raise();
    return nullptr;
  }

  std::vector<Value *> ArgVals;
  unsigned Idx = 0;

  // Look up full param type info (needed for generic struct params).
  // Try mangled name first; fall back to raw callee name for extern fns.
  auto ParamTypeIt = FuncParamTypes.find(MangledName);
  if (ParamTypeIt == FuncParamTypes.end())
    ParamTypeIt = FuncParamTypes.find(CalleeName);

  for (auto &Arg : CalleeF->args()) {
    Value *V = EvaluatedArgs[Idx];

    // Determine declared TurfTypePtr for this param
    TurfTypePtr DeclaredType;
    if (ParamTypeIt != FuncParamTypes.end() && Idx < ParamTypeIt->second.size())
      DeclaredType = ParamTypeIt->second[Idx];
    else
      DeclaredType = getTurfTypeFromLLVM(Arg.getType());

    // Unsized array params: the function expects a pointer to the array data.
    // We must pass the raw AllocaInst of the caller's array, not a loaded
    // value. TypeChecker has already verified the caller is passing an array
    // variable with a compatible element type.
    // Note: Fixed-size arrays (Size > 0) are passed by value, so V remains the
    // evaluated argument (the loaded array value).
    if (isArrayType(DeclaredType.get())) {
      const auto *ArrTy = dynamic_cast<const ::ArrayType *>(DeclaredType.get());
      if (ArrTy && ArrTy->getSize() == -1) {
        if (auto *VE = dynamic_cast<VariableExprAST *>(Args[Idx].get())) {
          V = VE->codegenLValue();
          assert(V && "Array argument LValue generation failed");
        } else {
          llvm::report_fatal_error(
              "Internal error: array argument must be a variable. "
              "This should have been caught by the TypeChecker.");
          return nullptr;
        }
      }
    } else {
      // Standard scalar type casting.
      //
      // Always prefer the already-computed, accurate DeclaredType over
      // re-deriving a type from the callee's raw LLVM parameter type.
      //
      // Getting this wrong for the concrete (non-generic) case is not
      // hypothetical: a struct-typed parameter's LLVM type is an LLVM
      // struct (not a primitive getTurfTypeFromLLVM() recognizes), so it
      // silently fell through to that function's `return TurfDouble();`
      // catch-all. The resulting CastToType() call "happened" to still pass
      // the struct value through unchanged (both sides of its exact-match
      // fast path degraded to "double" symmetrically) -- but only by
      // accident, and it skipped right past CastToType()'s own explicit,
      // purpose-built struct-to-struct passthrough check ("if DestType is a
      // struct ... pass through without casting") instead of actually
      // exercising it. Using DeclaredType directly routes through that
      // intended path.
      //
      // This previously special-cased away from DeclaredType whenever it
      // WAS a TurfGenericParamType (the T in `fn<T> foo(x: T)`), substituting
      // getTurfTypeFromLLVM(Arg.getType()) (== TurfPtr()) instead, on the
      // theory that a type-erased parameter has no single "real" type to
      // use. That reasoning no longer applies now that Resolver::
      // substituteTypeParams() (see Resolver.cpp) mutates the shared AST
      // node so DeclaredType really is a genuine, meaningful
      // TurfGenericParamType instance here -- and CastToType() has a
      // purpose-built erasure branch keyed on exactly that dynamic_cast
      // (`if (dynamic_cast<const TurfGenericParamType *>(DestType))`,
      // further down in this file). Substituting TurfPtr() instead skipped
      // right past that branch the same way the struct case above did:
      // TurfPtr() doesn't match any of CastToType()'s numeric-conversion
      // branches either, so the scalar argument value (e.g. a bare `i1` for
      // `identity(true)`) was returned completely unconverted and stored
      // into an opaque-pointer-typed slot with no cast at all -- confirmed
      // to segfault for a bool argument and silently corrupt a double
      // argument. Using DeclaredType directly here fixes both.
      TurfTypePtr ExpectedType = DeclaredType;

      // Type-erasure un-erase: if the value is a pointer (erased generic T)
      // and the expected type is numeric, convert via PtrToInt/bitcast.
      if (V->getType()->isPointerTy() &&
          (ExpectedType->isNamed("int") || ExpectedType->isNamed("bool") ||
           ExpectedType->isNamed("double"))) {
        Value *IntVal = Builder->CreatePtrToInt(V, Type::getInt64Ty(*TheContext), "unerase_arg");
        if (ExpectedType->isNamed("bool"))
          V = Builder->CreateICmpNE(IntVal, ConstantInt::get(Type::getInt64Ty(*TheContext), 0), "unerase_bool");
        else if (ExpectedType->isNamed("double"))
          V = Builder->CreateBitCast(IntVal, Type::getDoubleTy(*TheContext), "unerase_dbl");
        else
          V = IntVal;
      }

      V = CastToType(V, ExpectedType, "argcast", Loc);
    }

    ArgVals.push_back(V);
    Idx++;
  }

  // For variadic functions, append any extra arguments beyond the declared
  // parameter count. These are passed raw per the C vararg ABI — no casting.
  if (CalleeF->isVarArg()) {
    for (size_t i = Idx; i < EvaluatedArgs.size(); ++i)
      ArgVals.push_back(EvaluatedArgs[i]);
  }

  // void functions must not be given a name (LLVM requirement)
  if (CalleeF->getReturnType()->isVoidTy())
    return Builder->CreateCall(CalleeF, ArgVals);

  return Builder->CreateCall(CalleeF, ArgVals, "calltmp");
}



// Ensure malloc / realloc / free are declared in the module.


// Array Codegen
Value *ArrayDeclExprAST::codegen() {
  // Validation (void type, keywords, duplicates, bounds, etc.) is handled by the
  // Resolver and TypeChecker passes before Codegen.


  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  Type *ElemTy = getLLVMType(ElementType.get());
  llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, Size);

  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  AllocaInst *Alloca = TmpB.CreateAlloca(ArrTy, nullptr, Name);

  // Zero-initialize the entire array (memset to 0)
  // Get total size in bytes
  auto &DL = TheModule->getDataLayout();
  uint64_t TotalBytes = DL.getTypeAllocSize(ArrTy);
  Builder->CreateMemSet(
      Alloca, ConstantInt::get(Type::getInt8Ty(*TheContext), 0),
      ConstantInt::get(Type::getInt64Ty(*TheContext), TotalBytes),
      Alloca->getAlign());

  // If we have an initializer list, store each element
  if (InitExpr) {
    Value *RHSVal = InitExpr->codegen();
    if (!RHSVal)
      return nullptr;
    // Cast to the array type to ensure it's exact (though the TypeChecker
    // should have already validated this).
    TurfTypePtr ArrType = makeArrayType(ElementType, Size);
    RHSVal = CastToType(RHSVal, ArrType, "arrayinit", Loc);
    Builder->CreateStore(RHSVal, Alloca);
  } else if (!InitList.empty()) {
    for (int i = 0; i < static_cast<int>(InitList.size()); ++i) {
      Value *ElemVal = InitList[i]->codegen();
      if (!ElemVal)
        return nullptr;

      // Element type compatibility and void checks are handled by the TypeChecker.

      ElemVal = CastToType(ElemVal, ElementType, "arrayinit", Loc);

      // GEP into array[i] and store
      Value *Indices[] = {ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                          ConstantInt::get(Type::getInt64Ty(*TheContext), i)};
      Value *ElemPtr =
          Builder->CreateInBoundsGEP(ArrTy, Alloca, Indices, "arrelem");
      Builder->CreateStore(ElemVal, ElemPtr);
    }
  }

  // Register in symbol table — array type already carries the size
  TurfTypePtr ArrType = makeArrayType(ElementType, Size);
  if (GlobalSymbolTable) {
    GlobalSymbolTable->DeclareSymbol(Name, ArrType, Loc, Alloca);
  }

  NamedValues[Name] = {Alloca, ArrType};

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}

Value *ArrayLiteralExprAST::codegen() {
  if (Elements.empty()) {
    llvm::report_fatal_error("Internal error: empty array literal codegen");
    return nullptr;
  }

  const TurfType *ElemType = getExprType(Elements[0].get());
  Type *LLVMElemTy = getLLVMType(ElemType);
  llvm::ArrayType *ArrTy = llvm::ArrayType::get(LLVMElemTy, Elements.size());

  Value *ArrVal = llvm::UndefValue::get(ArrTy);

  for (size_t i = 0; i < Elements.size(); ++i) {
    Value *ElemVal = Elements[i]->codegen();
    if (!ElemVal)
      return nullptr;
    
    ElemVal = CastToType(ElemVal, ElemType, "arrlitelem", Loc);
    ArrVal = Builder->CreateInsertValue(ArrVal, ElemVal, i, "arrlitinsert");
  }

  return ArrVal;
}

// Helper function to emit runtime bounds check
static void EmitRuntimeBoundsCheck(Value *IndexVal, int ArraySize,
                                   const std::string &ArrayName,
                                   SourceLocation Loc) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();

  // Check: index < 0 || index >= size
  Value *SizeVal = ConstantInt::get(Type::getInt64Ty(*TheContext), ArraySize);
  Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);

  Value *TooSmall = Builder->CreateICmpSLT(IndexVal, Zero, "idx_neg");
  Value *TooBig = Builder->CreateICmpSGE(IndexVal, SizeVal, "idx_toobig");
  Value *OutOfBounds = Builder->CreateOr(TooSmall, TooBig, "oob");

  BasicBlock *OobBB = BasicBlock::Create(*TheContext, "arr_oob", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "arr_ok");

  Builder->CreateCondBr(OutOfBounds, OobBB, ContBB);

  // Out-of-bounds block: call fail()
  Builder->SetInsertPoint(OobBB);
  CodegenIntrinsic("fail", {Builder->CreateGlobalString(
      "Array index out of bounds for '" + ArrayName + "'")}, Loc);

  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateUnreachable();

  // Continue block
  TheFunction->insert(TheFunction->end(), ContBB);
  Builder->SetInsertPoint(ContBB);
}

Value *ArrayAccessExprAST::codegenLValue() {
  const TurfType *BaseTy = getExprType(Base.get());
  if (!BaseTy) {
    llvm::report_fatal_error("Internal error: Could not determine type of array/list base");
    return nullptr;
  }

  if (BaseTy->isString()) {
    Value *StrVal = Base->codegen();
    if (!StrVal)
      return nullptr;

    Value *IndexVal = Index->codegen();
    if (!IndexVal)
      return nullptr;
    IndexVal = CastToType(IndexVal, TurfInt(), "stridx", Loc);

    return Builder->CreateGEP(Type::getInt8Ty(*TheContext), StrVal, IndexVal,
                              "charptr");
  }

  if (auto *PtrTyTurf = dynamic_cast<const TurfPointerType *>(BaseTy)) {
    Value *PtrVal = Base->codegen();
    if (!PtrVal)
      return nullptr;

    Value *IndexVal = Index->codegen();
    if (!IndexVal)
      return nullptr;
    IndexVal = CastToType(IndexVal, TurfInt(), "ptridx", Loc);

    Type *ElemTy = getLLVMType(PtrTyTurf->getPointee().get());
    return Builder->CreateGEP(ElemTy, PtrVal, IndexVal, "ptrelemptr");
  }

  if (!isArrayType(BaseTy)) {
    llvm::report_fatal_error("Internal error: Base is not an array or pointer");
    return nullptr;
  }

  Value *BaseLVal = Base->codegenLValue();
  if (!BaseLVal)
    return nullptr;

  int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
  const TurfType *ElemType = getArrayElementType(BaseTy);

  Value *IndexVal = Index->codegen();
  if (!IndexVal)
    return nullptr;
  // Index type check is handled by TypeChecker.
  IndexVal = CastToType(IndexVal, TurfInt(), "arridx", Loc);

  if (ArraySize > 0) {
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      long long Idx = CI->getSExtValue();
      if (Idx < 0 || Idx >= ArraySize) {
        ArrayBoundsError Err(Loc, "array", Idx, ArraySize);
        if (auto *BaseVar = dynamic_cast<const VariableExprAST *>(Base.get())) {
          if (GlobalSymbolTable) {
            if (Symbol *S = GlobalSymbolTable->LookupSymbol(BaseVar->getName()))
              Err.withRelated(S->DeclLoc, "array declared here");
          }
        }
        Err.raise();
        return nullptr;
      }
    } else {
      EmitRuntimeBoundsCheck(IndexVal, ArraySize, "array", Loc);
    }
  }

  Type *ElemTy = getLLVMType(ElemType);
  if (ArraySize > 0) {
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, ArraySize);
    Value *Indices[] = {ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                        IndexVal};
    return Builder->CreateInBoundsGEP(ArrTy, BaseLVal, Indices, "arrelemptr");
  } else {
    Type *PtrTy = PointerType::get(*TheContext, 0);
    Value *ArrPtr = Builder->CreateLoad(PtrTy, BaseLVal, "arrptr");
    return Builder->CreateGEP(ElemTy, ArrPtr, IndexVal, "arrelemptr");
  }
}

Value *ArrayAccessExprAST::codegen() {
  const TurfType *BaseTy = getExprType(Base.get());
  if (BaseTy && BaseTy->isString()) {
    Value *StrVal = Base->codegen();
    if (!StrVal)
      return nullptr;

    Value *IndexVal = Index->codegen();
    if (!IndexVal)
      return nullptr;
    IndexVal = CastToType(IndexVal, TurfInt(), "stridx", Loc);

    Value *CharPtr = Builder->CreateGEP(Type::getInt8Ty(*TheContext), StrVal,
                                        IndexVal, "charptr");
    Value *CharVal = Builder->CreateLoad(Type::getInt8Ty(*TheContext), CharPtr,
                                         "charval");
    return Builder->CreateZExt(CharVal, Type::getInt64Ty(*TheContext),
                               "charint");
  }

  // First try to codegen the base as an L-value (e.g., a variable or another pointer-like access).
  Value *Ptr = codegenLValue();
  if (Ptr) {
    BaseTy = getExprType(Base.get());
    const TurfType *ElemTyT = nullptr;
    if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy))
      ElemTyT = PtrTy->getPointee().get();
    else
      ElemTyT = getArrayElementType(BaseTy);
    return Builder->CreateLoad(getLLVMType(ElemTyT), Ptr, "arrelem");
  }

  // If the base cannot be treated as an L-value (e.g., it is an R-value array returned from a function call),
  // we codegen it as an R-value.
  Value *BaseVal = Base->codegen();
  if (!BaseVal)
    return nullptr;

  BaseTy = getExprType(Base.get());
  if (!isArrayType(BaseTy)) {
    llvm::report_fatal_error("Internal error: Base is not an array");
    return nullptr;
  }

  int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
  const TurfType *ElemType = getArrayElementType(BaseTy);

  Value *IndexVal = Index->codegen();
  if (!IndexVal)
    return nullptr;

  IndexVal = CastToType(IndexVal, TurfInt(), "arridx", Loc);

  if (ArraySize > 0) {
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      long long Idx = CI->getSExtValue();
      if (Idx < 0 || Idx >= ArraySize) {
        ArrayBoundsError Err(Loc, "array", Idx, ArraySize);
        if (auto *BaseVar = dynamic_cast<const VariableExprAST *>(Base.get())) {
          if (GlobalSymbolTable) {
            if (Symbol *S = GlobalSymbolTable->LookupSymbol(BaseVar->getName()))
              Err.withRelated(S->DeclLoc, "array declared here");
          }
        }
        Err.raise();
        return nullptr;
      }
      return Builder->CreateExtractValue(BaseVal, CI->getZExtValue(), "arrextract");
    } else {
      // Dynamic index on an R-value array. We must dump it into an alloca to GEP into it.
      Function *TheFunction = Builder->GetInsertBlock()->getParent();
      AllocaInst *TempAlloca = CreateEntryBlockAlloca(TheFunction, "tmparr", BaseTy);
      Builder->CreateStore(BaseVal, TempAlloca);

      EmitRuntimeBoundsCheck(IndexVal, ArraySize, "array", Loc);

      Type *ElemTy = getLLVMType(ElemType);
      llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, ArraySize);
      Value *Indices[] = {ConstantInt::get(Type::getInt64Ty(*TheContext), 0), IndexVal};
      Value *ElemPtr = Builder->CreateInBoundsGEP(ArrTy, TempAlloca, Indices, "arrelemptr");
      return Builder->CreateLoad(ElemTy, ElemPtr, "arrelem");
    }
  }

  llvm::report_fatal_error("Internal error: Could not codegen ArrayAccessExprAST");
  return nullptr;
}

// NOTE: ListDeclExprAST has been removed. List declarations are now
// represented as VarDeclExprAST with a GenericType("list", [elemType]).
// The VarDeclExprAST::codegen handles list initialization via the type system.

Value *MethodCallExprAST::codegen() {
  if (auto *Var = dynamic_cast<VariableExprAST *>(Base.get())) {
    // Trust the Resolver's ResolvedModuleName verdict -- set only when the
    // base name is a genuine module qualifier AND is not shadowed by a
    // locally-declared variable of the same name (see
    // Resolver::visitMethodCall) -- rather than independently re-deriving
    // "is this a module call" from the raw variable name here.
    //
    // The raw-name check (GlobalSymbolTable->HasModule(Var->getName())) used
    // to live here instead, and it doesn't know about local variables at
    // all: a variable that happens to share a name with an imported module
    // (concretely, `io` -- auto-imported into EVERY Turf program via
    // lib/std/prelude.tr) would take this branch even though TypeChecker had
    // already set ResolvedCallTarget to the STRUCT method instead. That
    // mismatch was a real, reproduced compiler crash, not just a wrong
    // diagnostic: a few lines below, `ArgVals[Idx++]` walked past the end of
    // an empty ArgVals (the module path assumed zero explicit args) against
    // CalleeF's real argument count (the struct method's self parameter),
    // an out-of-bounds std::vector access that aborted the process.
    const std::string &ModuleName = Var->ResolvedModuleName;
    bool IsModule = !ModuleName.empty();
    if (IsModule) {
      // Codegen all arguments.
      std::vector<Value *> ArgVals;
      for (auto &Arg : Args) {
        Value *V = Arg->codegen();
        if (!V) return nullptr;
        ArgVals.push_back(V);
      }

      // ResolvedCallTarget is set by the TypeChecker during overload resolution.
      // If it's null the TypeChecker already emitted a type-mismatch error; just
      // propagate nullptr so Codegen can bail out cleanly.
      if (!ResolvedCallTarget) {
        LogErrorAt(Loc, "No matching overload found for '" + ModuleName + "." +
                            Method + "' (see type-mismatch error above)");
        return nullptr;
      }
      std::string MangledName = ResolvedCallTarget->mangledName();

      llvm::Function *CalleeF = TheModule->getFunction(MangledName);
      // If the function isn't in the LLVM module, that's a compiler bug —
      // the prototype pre-pass should have emitted it.
      if (!CalleeF) {
        llvm::report_fatal_error(
            llvm::StringRef("Internal error: LLVM function '" + MangledName +
            "' not found after successful type-checking. This is a compiler bug."));
        return nullptr;
      }

      // Cast arguments to match callee signature (numeric promotions only).
      std::vector<Value *> CastVals;
      unsigned Idx = 0;
      for (auto &FArg : CalleeF->args()) {
        Value *V = ArgVals[Idx++];
        llvm::Type *ExpTy = FArg.getType();
        if (V->getType() != ExpTy) {
          if (ExpTy->isDoubleTy() && V->getType()->isIntegerTy(64))
            V = Builder->CreateSIToFP(V, ExpTy, "argcast");
          else if (ExpTy->isDoubleTy() && V->getType()->isIntegerTy(1))
            V = Builder->CreateUIToFP(V, ExpTy, "argcast");
          else if (ExpTy->isIntegerTy(64) && V->getType()->isDoubleTy())
            V = Builder->CreateFPToSI(V, ExpTy, "argcast");
          else if (ExpTy->isIntegerTy(64) && V->getType()->isIntegerTy(1))
            V = Builder->CreateZExt(V, ExpTy, "argcast");
          else if (ExpTy->isIntegerTy(1) && V->getType()->isIntegerTy(64))
            V = Builder->CreateICmpNE(
                    V, ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                    "argcast");
          // pointer <-> integer / pointer passthrough handled by LLVM
        }
        CastVals.push_back(V);
      }

      // void functions must not be given a name (LLVM requirement) -- an
      // instruction that produces no value can't have a name bound to it.
      // Verified: this was previously unconditional, producing technically
      // invalid IR for every void-returning module call (e.g.
      // io.printline(...)) that `verifyModule()` (see main.cpp) now catches
      // -- LLVM's optimizer/backend evidently tolerated it silently before,
      // but it was never valid.
      if (CalleeF->getReturnType()->isVoidTy())
        return Builder->CreateCall(CalleeF, CastVals);
      return Builder->CreateCall(CalleeF, CastVals, "modcalltmp");
    }
  }

  // ── Struct / pointer method call ─────────────────────────────────────────
  // Base is a struct instance or pointer-to-struct.  ResolvedCallTarget is set
  // by the TypeChecker.  We just emit the call.
  const TurfType *BaseTy = getExprType(Base.get());
  if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy))
    BaseTy = PtrTy->getPointee().get();

  if (!BaseTy) {
    // TypeChecker should have caught this — reaching here is a compiler bug.
    llvm::report_fatal_error(
        "Internal error: could not determine type of method call base. "
        "This is a compiler bug.");
    return nullptr;
  }

  if (Method == "length") {
    if (isArrayType(BaseTy)) {
      int ArraySize = dynamic_cast<const ::ArrayType *>(BaseTy)->getSize();
      return ConstantInt::get(Type::getInt64Ty(*TheContext), ArraySize);
    }
    // For non-array types (e.g. list<T>), fall through to struct method dispatch.
  }

  std::string StructNameStr;
  // Resolve NamedType → TurfStructType, same as FieldAccessExprAST::codegen().
  // Without this, `expr.method()` where expr has type NamedType("T") will not
  // find the struct name and falls through to the fatal error below.
  if (auto *NT = dynamic_cast<const NamedType *>(BaseTy)) {
    if (GlobalSymbolTable)
      if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
        BaseTy = R.get();
  }
  if (auto *StructTy = dynamic_cast<const TurfStructType *>(BaseTy)) {
    StructNameStr = StructTy->getName();
  } else if (auto *GenTy = dynamic_cast<const GenericType *>(BaseTy)) {
    StructNameStr = GenTy->getBaseName();
  }

  if (!StructNameStr.empty()) {
    std::vector<Value *> EvaluatedArgs;
    for (auto &Arg : Args) {
      Value *V = Arg->codegen();
      if (!V)
        return nullptr;
      EvaluatedArgs.push_back(V);
    }

    std::string MangledName;
    if (ResolvedCallTarget) {
      MangledName = ResolvedCallTarget->mangledName();
    }

    Function *CalleeF = nullptr;
    if (!MangledName.empty()) {
      CalleeF = TheModule->getFunction(MangledName);
    }

    if (!CalleeF) {
      return nullptr;
    }

    bool IsStaticMethod = false;
    if (auto *Var = dynamic_cast<const VariableExprAST*>(Base.get())) {
      IsStaticMethod = Var->IsStructName;
    }

    // Arity is guaranteed by the TypeChecker — if it's wrong here, it's a bug.
    int ExpectedArity = IsStaticMethod ? Args.size() : (Args.size() + 1);
    assert(CalleeF->arg_size() == ExpectedArity &&
           "Struct method arity mismatch after TypeChecker — compiler bug");

    std::vector<Value *> ArgVals;
    if (!IsStaticMethod) {
      Value *SelfVal = Base->codegenLValue();
      if (!SelfVal) {
        // If Base is an rvalue (like User(10)), we need a temporary alloca to
        // pass by reference -- UNLESS Base's r-value IS ALREADY a pointer
        // (e.g. a chained call to a method that itself returns `self` as
        // Counter*, like `c.increment().increment()`). In that case the
        // pointer value itself is already what `self` should receive:
        // wrapping it in another alloca and passing THAT alloca's address
        // introduces a spurious extra level of indirection (self would
        // become Counter** instead of Counter*), so the callee's
        // `self.val = ...` writes end up mutating the temporary alloca
        // (which merely holds a copy of the real pointer) instead of the
        // real struct. Confirmed via a chained `.increment()` test: without
        // this check, only the first call in a chain actually mutated the
        // original struct -- every subsequent chained call silently wrote
        // to a throwaway temporary instead.
        Value *Val = Base->codegen();
        if (!Val)
          return nullptr;
        if (dynamic_cast<const TurfPointerType *>(getExprType(Base.get()))) {
          SelfVal = Val;
        } else {
          SelfVal = Builder->CreateAlloca(Val->getType(), nullptr, "tmp.self");
          Builder->CreateStore(Val, SelfVal);
        }
      } else if (dynamic_cast<const TurfPointerType *>(getExprType(Base.get()))) {
        SelfVal = Builder->CreateLoad(llvm::PointerType::get(*TheContext, 0),
                                      SelfVal, "deref");
      }
      if (!SelfVal)
        return nullptr;
      ArgVals.push_back(SelfVal);
    }

    unsigned Idx = 0;
    auto ArgIt = IsStaticMethod ? CalleeF->arg_begin() : std::next(CalleeF->arg_begin());
    for (; ArgIt != CalleeF->arg_end(); ++ArgIt, ++Idx) {
      Value *V = EvaluatedArgs[Idx];
      Type *ExpectedTy = ArgIt->getType();
      if (V->getType() != ExpectedTy) {
        if (ExpectedTy->isPointerTy() && V->getType()->isIntegerTy())
          V = Builder->CreateIntToPtr(V, ExpectedTy, "argcast_ptr");
        else if (ExpectedTy->isIntegerTy() && V->getType()->isPointerTy())
          V = Builder->CreatePtrToInt(V, ExpectedTy, "argcast_int");
        else if (ExpectedTy->isDoubleTy() && V->getType()->isPointerTy()) {
          V = Builder->CreatePtrToInt(V, Type::getInt64Ty(*TheContext));
          V = Builder->CreateBitCast(V, ExpectedTy, "argcast_dbl");
        }
        else
          V = Builder->CreateBitCast(V, ExpectedTy, "argcast_bit");
      }
      ArgVals.push_back(V);
    }

    // void functions must not be given a name (LLVM requirement) -- see the
    // matching fix/comment on the module-call path above for the full story.
    if (CalleeF->getReturnType()->isVoidTy())
      return Builder->CreateCall(CalleeF, ArgVals);
    return Builder->CreateCall(CalleeF, ArgVals, "methodcalltmp");
  }

  // TypeChecker should have rejected a method call on an unsupported type.
  std::cerr << "BaseTy: " << (BaseTy ? BaseTy->getName() : "null") << "\n";
  llvm::report_fatal_error(
      "Internal error: method call on unsupported type. This is a compiler bug.");
  return nullptr;
}

Value *EnumDeclExprAST::codegen() {
  return ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
}

Value *EnumAccessExprAST::codegen() {
  auto EnumTy = GlobalSymbolTable->LookupType(EnumName);
  auto *ET = dynamic_cast<const TurfEnumType *>(EnumTy.get());
  if (!ET) {
    LogErrorAt(Loc, "Internal error: enum type not found in codegen");
    return nullptr;
  }
  
  if (Variant == "COUNT") {
    return ConstantInt::get(Type::getInt64Ty(*TheContext), ET->numVariants());
  }
  
  if (Variant == "VALUES") {
    llvm::Type *UnderlyingLLVMTy = getLLVMType(ET->getUnderlyingType());
    std::vector<Constant*> ArrayVals;
    for (const auto& V : ET->getVariants()) {
      ArrayVals.push_back(ConstantInt::get(UnderlyingLLVMTy, ET->getVariantValue(V)));
    }
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(UnderlyingLLVMTy, ArrayVals.size());
    Constant *ArrConst = ConstantArray::get(ArrTy, ArrayVals);
    GlobalVariable *GV = new GlobalVariable(*TheModule, ArrTy, true,
                                            GlobalValue::PrivateLinkage, ArrConst, EnumName + ".values");
    Constant *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
    Constant *Indices[] = {Zero, Zero};
    return ConstantExpr::getInBoundsGetElementPtr(ArrTy, GV, Indices);
  }
  
  if (Variant == "NAMES") {
    std::vector<Constant*> ArrayVals;
    llvm::Type *StringTy = llvm::PointerType::getUnqual(*TheContext);
    for (const auto& V : ET->getVariants()) {
      Constant *StrConstant = llvm::ConstantDataArray::getString(*TheContext, V, true);
      GlobalVariable *StrGV = new GlobalVariable(*TheModule, StrConstant->getType(), true,
                                                 GlobalValue::PrivateLinkage, StrConstant, ".str");
      Constant *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
      Constant *Indices[] = {Zero, Zero};
      ArrayVals.push_back(ConstantExpr::getInBoundsGetElementPtr(StrConstant->getType(), StrGV, Indices));
    }
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(StringTy, ArrayVals.size());
    Constant *ArrConst = ConstantArray::get(ArrTy, ArrayVals);
    GlobalVariable *GV = new GlobalVariable(*TheModule, ArrTy, true,
                                            GlobalValue::PrivateLinkage, ArrConst, EnumName + ".names");
    Constant *Zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
    Constant *Indices[] = {Zero, Zero};
    return ConstantExpr::getInBoundsGetElementPtr(ArrTy, GV, Indices);
  }

  llvm::Type *LLVMTy = getLLVMType(ET->getUnderlyingType());
  return ConstantInt::get(LLVMTy, ET->getVariantValue(Variant));
}

Value *StructDeclExprAST::codegen() {
  // ── Fix for issue #176: Two-step struct creation ──────────────────────────
  // Step 1: Create (or reuse) an opaque StructType and register it in
  // LLVMTypeCache *before* evaluating any field types.  This ensures that a
  // self-referential field like `next: Node*` can call getLLVMType("Node")
  // and find the opaque stub in the cache rather than triggering infinite
  // recursion or creating a duplicate struct (which LLVM would rename to
  // "Node.0", breaking all previously-built call/alloca sites).
  //
  // NOTE: this function must never be called more than once for the same
  // struct (see main.cpp's pre-pass sub-pass 2c, which now runs every
  // struct's codegen exactly once, before any function prototype or body).
  // A second call would find the cached type no longer opaque (setBody()
  // already ran below) and fall through to creating a brand-new, distinct
  // llvm::StructType -- silently corrupting LLVMTypeCache for every
  // already-built reference to the original.
  StructType *ST = nullptr;
  auto CacheIt = LLVMTypeCache.find(Name);
  if (CacheIt != LLVMTypeCache.end()) {
    // An opaque forward-declaration was already inserted (e.g., because a
    // NamedType("Node") appeared in another struct or function signature
    // before this decl ran).  Reuse it so the single LLVM type identity is
    // preserved everywhere it was referenced.
    if (auto *Existing = dyn_cast<StructType>(CacheIt->second)) {
      if (Existing->isOpaque())
        ST = Existing;
    }
  }
  if (!ST) {
    // Brand-new opaque struct — no body yet.
    ST = StructType::create(*TheContext, Name);
    if (!ST) {
      LogErrorAt(Loc, "Failed to create LLVM StructType for '" + Name + "'");
      return nullptr;
    }
  }

  // Register immediately so that any recursive getLLVMType call (e.g., for a
  // Node* field) finds the opaque stub instead of recursing back here.
  LLVMTypeCache[Name] = ST;

  // Step 2: Evaluate all field LLVM types now that the stub is in the cache.
  // Self-referential pointer fields (TurfPointerType) lower to opaque ptr
  // unconditionally, so they never need to re-enter this codegen path.
  // Direct struct-valued fields (unusual but possible for non-recursive types)
  // will find the opaque stub above and use it.
  std::vector<Type *> LLVMFieldTypes;
  for (const auto &Field : Fields) {
    Type *LLVMType = getLLVMType(Field.Type.get());
    if (!LLVMType) {
      LogErrorAt(Loc, "Unknown type for field '" + Field.Name + "'");
      return nullptr;
    }
    LLVMFieldTypes.push_back(LLVMType);
  }

  // Step 3: Fill in the body now that all field types are resolved.
  ST->setBody(LLVMFieldTypes);

  return Constant::getNullValue(Type::getInt64Ty(*TheContext));
}

Value *StructInitExprAST::codegen() {
  TurfTypePtr T;
  if (GlobalSymbolTable)
    T = GlobalSymbolTable->LookupType(Name);
  auto *SType = dynamic_cast<TurfStructType *>(T.get());
  if (!SType) {
    LogErrorAt(Loc, "Unknown struct type '" + Name + "'");
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(T.get());
  StructType *LLVMStructType = dyn_cast<StructType>(LLVMTy);
  if (!LLVMStructType) {
    LogErrorAt(Loc, "LLVM StructType '" + Name + "' not found");
    return nullptr;
  }

  // Reorder NamedFields to match the struct definition's field order.
  // TypeChecker already validated completeness and no unknown fields.
  const auto &DeclFields = SType->getFields();
  std::vector<Value *> EvaluatedArgs;
  std::vector<TurfTypePtr> ArgTypes;
  EvaluatedArgs.reserve(DeclFields.size());
  ArgTypes.reserve(DeclFields.size());

  for (const auto &DeclF : DeclFields) {
    // Find the matching named field supplied by the caller.
    const ExprAST *MatchedExpr = nullptr;
    for (const auto &[FieldName, Val] : NamedFields) {
      if (FieldName == DeclF.Name) {
        MatchedExpr = Val.get();
        break;
      }
    }
    Value *V = nullptr;
    if (MatchedExpr) {
      V = const_cast<ExprAST *>(MatchedExpr)->codegen();
      if (!V)
        return nullptr;
    } else {
      V = GetDefaultValue(DeclF.Type.get());
    }
    EvaluatedArgs.push_back(V);

    TurfTypePtr ActTy = DeclF.Type;
    if (MatchedExpr) {
      if (auto *VE = dynamic_cast<const VariableExprAST *>(MatchedExpr)) {
        if (VE->ResolvedSymbol)
          ActTy = VE->ResolvedSymbol->Type;
        else if (GlobalSymbolTable) {
          Symbol *Sym = GlobalSymbolTable->LookupSymbol(VE->getName());
          if (Sym) ActTy = Sym->Type;
        }
      }
      if (!ActTy)
        ActTy = getTurfTypeFromLLVM(V->getType());
    }
    ArgTypes.push_back(ActTy);
  }

  // Try to find a user-defined constructor function (init method).
  std::string InitName = Name + ".init";
  std::string MangledName = "turf_" + InitName + "_" + Name + "_ptr";
  for (const auto &AT : ArgTypes) {
    MangledName += "_" + AT->getMangledName();
  }

  Function *InitF = TheModule->getFunction(MangledName);
  if (!InitF) {
    for (const auto &[FName, ParamTypes] : FuncParamTypes) {
      std::string Prefix = "turf_" + InitName;
      std::string Suffix = "_" + InitName;
      bool Match = false;
      if (FName == Prefix ||
          (FName.size() > Prefix.size() && FName[Prefix.size()] == '_' &&
           FName.substr(0, Prefix.size()) == Prefix)) {
        Match = true;
      } else {
        size_t Pos = FName.find(Suffix);
        if (Pos != std::string::npos &&
            (Pos + Suffix.size() == FName.size() ||
             FName[Pos + Suffix.size()] == '_')) {
          Match = true;
        }
      }
      if (Match) {
        if (ParamTypes.size() == ArgTypes.size() + 1) {
          bool Compatible = true;
          for (size_t i = 0; i < ArgTypes.size(); ++i) {
            if (!typesCompatible(ArgTypes[i].get(), ParamTypes[i + 1].get())) {
              Compatible = false;
              break;
            }
          }
          if (Compatible) {
            InitF = TheModule->getFunction(FName);
            break;
          }
        }
      }
    }
  }

  if (InitF) {
    if (InitF->arg_size() != EvaluatedArgs.size() + 1) {
      LogErrorAt(Loc, "Constructor for '" + Name + "' expects " +
                          std::to_string(InitF->arg_size() - 1) +
                          " arguments, but got " +
                          std::to_string(EvaluatedArgs.size()));
      return nullptr;
    }

    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    AllocaInst *Alloca =
        TmpB.CreateAlloca(LLVMStructType, nullptr, Name + "_init");

    std::vector<Value *> ArgVals;
    ArgVals.push_back(Alloca); // Implicit self pointer

    unsigned Idx = 0;
    for (auto ArgIt = std::next(InitF->arg_begin()); ArgIt != InitF->arg_end();
         ++ArgIt, ++Idx) {
      Value *V = EvaluatedArgs[Idx];
      Type *ExpectedTy = ArgIt->getType();
      if (V->getType() != ExpectedTy) {
        if (ExpectedTy->isPointerTy() && V->getType()->isIntegerTy())
          V = Builder->CreateIntToPtr(V, ExpectedTy, "argcast_ptr");
        else if (ExpectedTy->isIntegerTy() && V->getType()->isPointerTy())
          V = Builder->CreatePtrToInt(V, ExpectedTy, "argcast_int");
        else if (ExpectedTy->isDoubleTy() && V->getType()->isPointerTy()) {
          V = Builder->CreatePtrToInt(V, Type::getInt64Ty(*TheContext));
          V = Builder->CreateBitCast(V, ExpectedTy, "argcast_dbl");
        } else
          V = Builder->CreateBitCast(V, ExpectedTy, "argcast_bit");
      }
      ArgVals.push_back(V);
    }

    Builder->CreateCall(InitF, ArgVals);
    return Builder->CreateLoad(LLVMStructType, Alloca, "structval");
  }

  // Fallback to default struct initialization (no user constructor).
  if (EvaluatedArgs.empty()) {
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    AllocaInst *Alloca =
        TmpB.CreateAlloca(LLVMStructType, nullptr, Name + "_init");

    llvm::Constant *DefaultVal = GetDefaultValue(T.get());
    Builder->CreateStore(DefaultVal, Alloca);

    return Builder->CreateLoad(LLVMStructType, Alloca, "structval");
  }

  // Direct field-by-field initialization using definition order.
  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  AllocaInst *Alloca =
      TmpB.CreateAlloca(LLVMStructType, nullptr, Name + "_init");

  for (size_t i = 0; i < EvaluatedArgs.size(); ++i) {
    Value *ArgVal = EvaluatedArgs[i];

    TurfTypePtr FieldType = DeclFields[i].Type;
    TurfTypePtr ArgTurfType = getTurfTypeFromLLVM(ArgVal->getType());
    ArgTurfType = RefineLLVMType(ArgTurfType, FieldType, ArgVal->getType());

    // Type mismatch validation is handled by the TypeChecker.

    ArgVal = CastToType(ArgVal, FieldType, "fieldcast", Loc);

    Value *FieldPtr =
        Builder->CreateStructGEP(LLVMStructType, Alloca, i, "fieldptr");
    Builder->CreateStore(ArgVal, FieldPtr);
  }

  return Builder->CreateLoad(LLVMStructType, Alloca, "structval");
}

Value *FieldAccessExprAST::codegenLValue() {
  Value *BaseLVal = Base->codegenLValue();
  if (!BaseLVal) {
    // Base is not itself addressable -- e.g. a function-call result
    // (`makeBox().val`), a method-call result, or any other r-value-only
    // expression. Without this fallback, this used to silently return
    // nullptr with NO diagnostic anywhere: every caller up the chain
    // (VarDeclExprAST::codegen(), MethodCallExprAST arg evaluation, etc.)
    // treats a null Value* as "an error was already reported, propagate
    // silently" -- so `io.printline(makeBox().val)` used to compile clean
    // and simply never execute, and `mut p: int = makeBox().val;` used to
    // leave `p` completely unregistered, surfacing only as a confusing
    // "undeclared variable" error at the NEXT use of `p`, nowhere near the
    // real cause.
    //
    // Mirrors the identical fallback ArrayAccessExprAST::codegen() and
    // MethodCallExprAST::codegen() already use for the same r-value-base
    // scenario: evaluate the base as an r-value, spill it into a fresh
    // stack slot, and GEP into that instead. This is correct uniformly for
    // both a direct struct-valued base (spill the struct value; the
    // TurfStructType branch below GEPs into it directly) and a pointer-
    // typed base (spill the pointer value; the TurfPointerType branch below
    // already expects BaseLVal to be "address of the pointer" and loads
    // through it once, exactly as it does for an ordinary addressable
    // pointer variable).
    Value *BaseVal = Base->codegen();
    if (!BaseVal)
      return nullptr;
    BaseLVal = Builder->CreateAlloca(BaseVal->getType(), nullptr, "tmp.fieldbase");
    Builder->CreateStore(BaseVal, BaseLVal);
  }

  const TurfType *BaseType = getExprType(Base.get());

  if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseType)) {
    BaseType = PtrTy->getPointee().get();
    BaseLVal = Builder->CreateLoad(llvm::PointerType::get(*TheContext, 0),
                                   BaseLVal, "deref");
  }
  // Resolve NamedType base to TurfStructType for chained field access.
  if (auto *NT = dynamic_cast<const NamedType *>(BaseType)) {
    if (GlobalSymbolTable) {
      if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
        BaseType = R.get();
    }
  }
  // Resolve a GenericType base (e.g. a `Box<int>`-typed variable) to its
  // underlying TurfStructType via the base name. Under this compiler's
  // type-erasure model there is exactly one registered TurfStructType per
  // generic struct declaration (see typesCompatible()'s "Struct-literal /
  // generic-struct compatibility" comment in Types.cpp for why a single
  // layout is correct for every instantiation) -- GlobalSymbolTable only
  // ever has "Box" registered, never "Box<int>" specifically.
  if (auto *GT = dynamic_cast<const GenericType *>(BaseType)) {
    if (GlobalSymbolTable) {
      if (auto R = GlobalSymbolTable->LookupType(GT->getBaseName()))
        BaseType = R.get();
    }
  }
  auto *SType = dynamic_cast<const TurfStructType *>(BaseType);
  if (!SType) {
    LogErrorAt(Loc, "Cannot access field on non-struct type");
    return nullptr;
  }

  int FieldIdx = SType->getFieldIndex(FieldName);
  if (FieldIdx < 0) {
    LogErrorAt(Loc, "Struct '" + SType->getName() + "' has no field named '" +
                        FieldName + "'");
    return nullptr;
  }

  Type *LLVMTy = getLLVMType(BaseType);
  StructType *LLVMStructType = dyn_cast<StructType>(LLVMTy);

  return Builder->CreateStructGEP(LLVMStructType, BaseLVal, FieldIdx,
                                  "fieldptr");
}

Value *FieldAccessExprAST::codegen() {
  Value *Ptr = codegenLValue();
  if (!Ptr)
    return nullptr;

  const TurfType *BaseType = getExprType(Base.get());
  if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseType)) {
    BaseType = PtrTy->getPointee().get();
  }

  // Resolve NamedType base to TurfStructType, mirroring what codegenLValue()
  // already does. Without this, `p: Node*` has pointee NamedType("Node") —
  // not TurfStructType — and the cast below would fail.
  if (auto *NT = dynamic_cast<const NamedType *>(BaseType)) {
    if (GlobalSymbolTable)
      if (auto R = GlobalSymbolTable->LookupType(NT->getName()))
        BaseType = R.get();
  }
  // Resolve a GenericType base (e.g. `Box<int>`) the same way -- see the
  // matching comment in codegenLValue() just above.
  if (auto *GT = dynamic_cast<const GenericType *>(BaseType)) {
    if (GlobalSymbolTable)
      if (auto R = GlobalSymbolTable->LookupType(GT->getBaseName()))
        BaseType = R.get();
  }

  auto *SType = dynamic_cast<const TurfStructType *>(BaseType);
  if (!SType) {
    LogErrorAt(Loc, "Cannot load field on non-struct type");
    return nullptr;
  }
  TurfTypePtr FieldType = SType->getFieldType(FieldName);

  return Builder->CreateLoad(getLLVMType(FieldType.get()), Ptr,
                             FieldName.c_str());
}

Value *ModuleImportExprAST::codegen() {
  if (GlobalSymbolTable) {
    GlobalSymbolTable->EnterModule(ModuleName);
  }
  Value *Res = Body->codegen();
  if (GlobalSymbolTable) {
    GlobalSymbolTable->ExitModule();
  }
  return Res;
}
