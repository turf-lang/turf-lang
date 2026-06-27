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
static llvm::Function *GetOrDeclareMalloc();
static llvm::Function *GetOrDeclareRealloc();
static void EmitListBoundsCheck(llvm::Value *, llvm::Value *,
                                const std::string &, SourceLocation);

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
  std::string Key = T->getName();

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

  // Array types — the alloca is created per-site; return opaque pointer here.
  if (isArrayType(T))
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);

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

// Check whether two types are implicitly compatible.
// Numerics (int, double, bool) are mutually compatible; string and void are
// not. All pointer-like types (string, ptr, T*, void*) are mutually compatible
// since they share the same LLVM representation.
static bool isTypeCompatible(const TurfType *From, const TurfType *To) {
  if (!From || !To)
    return false;
  if (From->getName() == To->getName())
    return true;

  // Type Erasure: Generic Type Parameters are compatible with any type
  if (dynamic_cast<const TurfGenericParamType *>(From) ||
      dynamic_cast<const TurfGenericParamType *>(To))
    return true;

  // All pointer-like types are mutually compatible (same LLVM ptr representation)
  if (isPointerLike(From) && isPointerLike(To))
    return true;

  // null is compatible with any reference type (string, pointer)
  if (From->getName() == "null") {
    if (isPointerLike(To))
      return true;
    return false;
  }

  if (From->isVoid() || To->isVoid())
    return false;
  if (From->isNamed("string") || To->isNamed("string"))
    return false;
  // Remaining numerics: int, double, bool — all mutually convertible
  return true;
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
    char Op = Bin->getOp();
    if (Op == '<' || Op == '>' || Op == '!' || Op == '=')
      return TurfBool().get();
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
            (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')) {
          if (ParamTypes.size() == ArgTypes.size()) {
            bool Compatible = true;
            for (size_t i = 0; i < ArgTypes.size(); ++i) {
              if (!isTypeCompatible(ArgTypes[i], ParamTypes[i].get())) {
                Compatible = false;
                break;
              }
            }
            if (Compatible)
              return FuncReturnTypes[Name].get();
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
    if (GlobalSymbolTable) {
      Symbol *Sym = GlobalSymbolTable->LookupSymbol(Var->getName());
      if (Sym)
        return Sym->Type.get();
    }
    auto It = NamedValues.find(Var->getName());
    if (It != NamedValues.end())
      return It->second.Type.get();
  }
  if (auto *ArrAcc = dynamic_cast<const ArrayAccessExprAST *>(E)) {
    const TurfType *BaseTy = getExprType(ArrAcc->getBase());
    if (BaseTy && isArrayType(BaseTy))
      return getArrayElementType(BaseTy);
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
              (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')) {
            if (ParamTypes.size() == ArgTypes.size()) {
              bool Compatible = true;
              for (size_t i = 0; i < ArgTypes.size(); ++i) {
                if (!isTypeCompatible(ArgTypes[i], ParamTypes[i].get())) {
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
    if (MethodCall->getMethod() == "push") {
      return BaseTy;
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
      auto It = FuncReturnTypes.find(MangledName);
      if (It != FuncReturnTypes.end())
        return It->second.get();

      for (const auto &[Name, ParamTypes] : FuncParamTypes) {
        std::string Prefix = "turf_" + MethodName;
        std::string Suffix = "_" + MethodName;
        bool Match = false;
        if (Name == Prefix || (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')) {
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
              if (!isTypeCompatible(ArgTypes[i], ParamTypes[i + 1].get())) {
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
    if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy))
      BaseTy = PtrTy->getPointee().get();

    if (auto *STy = dynamic_cast<const TurfStructType *>(BaseTy)) {
      // Return a raw pointer of the underlying TurfTypePtr to match return
      // signature. But we can't easily get the raw pointer from TurfTypePtr
      // without storing it or retrieving it. Actually we can:
      return STy->getFieldType(FieldAccess->getFieldName()).get();
    }
  }
  return nullptr;
}

static Value *CastToType(Value *Val, TurfTypePtr DestType,
                         const std::string &Name, SourceLocation Loc = {0, 0}) {
  Type *ValTy = Val->getType();

  if (isa<ConstantPointerNull>(Val)) {
    if (isPointerLike(DestType.get())) {
      return Builder->CreateBitCast(Val, getLLVMType(DestType.get()), Name);
    }
  }

  // Normalize foreign integer widths (e.g. i32 from printf) to i64
  if (ValTy->isIntegerTy(32)) {
    Val = Builder->CreateSExt(Val, Type::getInt64Ty(*TheContext), "i32toi64");
    ValTy = Val->getType();
  }

  TurfTypePtr SrcType = getTurfTypeFromLLVM(ValTy);
  SrcType = RefineLLVMType(SrcType, DestType, ValTy);
  if (SrcType->getName() == DestType->getName())
    return Val;

  // Auto-coerce void* to/from any pointer
  if (SrcType->getName() == "void*" &&
      dynamic_cast<const TurfPointerType *>(DestType.get()))
    return Val;
  if (DestType->getName() == "void*" &&
      dynamic_cast<const TurfPointerType *>(SrcType.get()))
    return Val;

  // Type Erasure: cast to generic parameter (stored as void*)
  if (dynamic_cast<const TurfGenericParamType *>(DestType.get())) {
    Type *VoidPtrTy = PointerType::get(*TheContext, 0);
    if (ValTy->isPointerTy())
      return Val; // already a pointer
    if (ValTy->isDoubleTy())
      Val = Builder->CreateBitCast(Val, Type::getInt64Ty(*TheContext), "dbl_to_i64");
    if (Val->getType()->isIntegerTy())
      return Builder->CreateIntToPtr(Val, VoidPtrTy, "erased_ptr");
    return Builder->CreateBitCast(Val, VoidPtrTy, "erased_ptr");
  }

  // Type Erasure: cast from generic parameter (void*) back to actual type
  if (dynamic_cast<const TurfGenericParamType *>(SrcType.get())) {
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
    if (isPointerLike(DestType.get())) {
      return Val; // already pointer
    }
  }

  // All pointer-like types (string, ptr, T*, void*) share the same LLVM
  // representation. Casts between them are always no-ops at the IR level.
  if (isPointerLike(SrcType.get()) && isPointerLike(DestType.get()))
    return Val;

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
      return Builder->CreateFCmpONE(
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

// Turns any expression into IR operation
Value *BinaryExprAST::codegen() {
  // Generate codes for the Left side and Right side
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();

  if (!L || !R)
    return nullptr;

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
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOLT(L, R, "cmptmp")
                                      : Builder->CreateICmpSLT(L, R, "cmptmp");
  }

  case '>': {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOGT(L, R, "cmptmp")
                                      : Builder->CreateICmpSGT(L, R, "cmptmp");
  }

  case TOK_EQ: {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOEQ(L, R, "cmptmp")
                                      : Builder->CreateICmpEQ(L, R, "cmptmp");
  }

  case TOK_NEQ: {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpONE(L, R, "cmptmp")
                                      : Builder->CreateICmpNE(L, R, "cmptmp");
  }

  case TOK_GEQ: {
    TurfTypePtr CmpType = CommonType->isNamed("bool") ? TurfInt() : CommonType;
    CastBoth(CmpType);
    return CmpType->isNamed("double") ? Builder->CreateFCmpOGE(L, R, "cmptmp")
                                      : Builder->CreateICmpSGE(L, R, "cmptmp");
  }

  case TOK_LEQ: {
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

  // Power
  case '^': {
    CastBoth(TurfDouble());
    Function *PowFunc = llvm::Intrinsic::getOrInsertDeclaration(
      TheModule.get(), llvm::Intrinsic::pow,
      {Type::getDoubleTy(*TheContext)});
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

    // Initialization type checks are handled by the TypeChecker.

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

  // We check LHS type by performing a dummy load through codegen
  Value *LHSVal = LHS->codegen();
  if (!LHSVal)
    return nullptr;
  TurfTypePtr VarType = getTurfTypeFromLLVM(LHSVal->getType());

  TurfTypePtr RHSType = getTurfTypeFromLLVM(Val->getType());
  RHSType = RefineLLVMType(RHSType, VarType, Val->getType());

  Value *CastVal = CastToType(Val, VarType, "assigncast", Loc);

  // Generate the Store instruction using LValue
  Value *Ptr = LHS->codegenLValue();
  if (!Ptr)
    return nullptr;
  Builder->CreateStore(CastVal, Ptr);

  // Assignment expressions usually return the value assigned (allows x = y = 5)
  return CastVal;
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
  default:
    llvm::report_fatal_error("Internal error: Unknown unary operator");
    return nullptr;
  }
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
    else
      SrcType = getTurfTypeFromLLVM(Val->getType());
  } else {
    SrcType = getTurfTypeFromLLVM(Val->getType());
  }
  const std::string SrcName = ExprTy ? ExprTy->getName() : SrcType->getName();
  const std::string DstName = DestType->getName();

  // Identity - no-op
  if (SrcName == DstName)
    return Val;

  // numeric → numeric
  if (SrcName == "int" && DstName == "double")
    return Builder->CreateSIToFP(Val, Type::getDoubleTy(*TheContext), "itod");

  if (SrcName == "double" && DstName == "int")
    return Builder->CreateFPToSI(Val, Type::getInt64Ty(*TheContext), "dtoi");

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

    Function *PutsF = TheModule->getFunction("puts");
    if (!PutsF) {
      FunctionType *FT =
          FunctionType::get(Type::getInt32Ty(*TheContext), {I8Ptr}, false);
      PutsF = Function::Create(FT, Function::ExternalLinkage, "puts",
                               TheModule.get());
    }
    Value *ErrMsg = Builder->CreateGlobalString(
        "Runtime Error: Invalid string to int conversion.", "errmsg");
    Builder->CreateCall(PutsF, {ErrMsg});

    Function *ExitF = TheModule->getFunction("exit");
    if (!ExitF) {
      FunctionType *FT = FunctionType::get(
          Type::getVoidTy(*TheContext), {Type::getInt32Ty(*TheContext)}, false);
      ExitF = Function::Create(FT, Function::ExternalLinkage, "exit",
                               TheModule.get());
    }
    Builder->CreateCall(ExitF,
                        {ConstantInt::get(Type::getInt32Ty(*TheContext), 1)});
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

  // int → string : snprintf(buf, 32, "%lld", val)
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
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    AllocaInst *Buf = TmpB.CreateAlloca(Type::getInt8Ty(*TheContext), BufSize,
                                        "int_to_str_buf");
    Value *Fmt = Builder->CreateGlobalString("%lld", "intfmt");
    Builder->CreateCall(SnprintfF, {Buf, BufSize, Fmt, Val}, "snprintf_int");
    return Buf;
  }

  // double → string : snprintf(buf, 32, "%g", val)
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
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                     TheFunction->getEntryBlock().begin());
    Value *BufSize = ConstantInt::get(Type::getInt64Ty(*TheContext), 32);
    AllocaInst *Buf = TmpB.CreateAlloca(Type::getInt8Ty(*TheContext), BufSize,
                                        "dbl_to_str_buf");
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

  // Unsupported → string
  if (DstName == "string") {
    StringConversionError(Loc, SrcName.c_str()).raise();
    return nullptr;
  }

  // int -> pointer : inttoptr
  if (SrcName == "int" && dynamic_cast<const TurfPointerType*>(DestType.get())) {
    return Builder->CreateIntToPtr(Val, getLLVMType(DestType.get()), "inttoptr");
  }

  // pointer -> pointer : bitcast
  if ((ExprTy && dynamic_cast<const TurfPointerType*>(ExprTy)) && dynamic_cast<const TurfPointerType*>(DestType.get())) {
    return Builder->CreateBitCast(Val, getLLVMType(DestType.get()), "ptrcast");
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
    SyntaxError(Loc, "'continue' used outside of a loop").raise();
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
    FSym.IsIntrinsic = !IntrinsicName.empty();
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

  if (!IntrinsicName.empty()) {
    std::vector<Value *> ArgVals;
    for (auto &Arg : TheFunc->args()) {
      ArgVals.push_back(&Arg);
    }
    Value *IntrinsicRet = CodegenIntrinsic(IntrinsicName, ArgVals, Loc);
    if (!IntrinsicRet) {
      // CodegenIntrinsic will raise its own error
      TheFunc->eraseFromParent();
      return nullptr;
    }
    if (ReturnType->isVoid()) {
      Builder->CreateRetVoid();
    } else {
      Builder->CreateRet(IntrinsicRet);
    }

    // Restore caller context
    NamedValues = SavedNamedValues;
    CurrentFuncReturnType = SavedReturnType;
    CurrentFunction = SavedFunction;
    if (SavedInsertBlock) {
      Builder->SetInsertPoint(SavedInsertBlock, SavedInsertPoint);
    } else {
      Builder->ClearInsertionPoint();
    }
    return TheFunc;
  }

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

    // For array parameters, normalise the registered type to size=-1 so that
    // ArrayAccessExprAST uses the pointer-load GEP path (not the two-index
    // alloca-GEP path that assumes the alloca IS the array).
    TurfTypePtr RegisteredType = ParamTurfType;
    if (isArrayType(ParamTurfType.get())) {
      const TurfType *ElemT = getArrayElementType(ParamTurfType.get());
      // Rebuild the type with the element type shared_ptr
      RegisteredType =
          makeArrayType(dynamic_cast<const ::ArrayType *>(ParamTurfType.get())
                            ->getElementTypePtr(),
                        -1); // -1 = pointer parameter
    }

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
    auto FuncCFG = CFGBuilder.buildCFG(Name, ReturnType, Body.get());

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
    FSym.IsIntrinsic = false;
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

    // Array params: the function expects a pointer to the array data.
    // We must pass the raw AllocaInst of the caller's array, not a loaded
    // value. TypeChecker has already verified the caller is passing an array
    // variable with a compatible element type.
    if (isArrayType(DeclaredType.get())) {
      if (auto *VE = dynamic_cast<VariableExprAST *>(Args[Idx].get())) {
        auto It = NamedValues.find(VE->getName());
        assert(It != NamedValues.end() && "Array argument not found in NamedValues");
        V = It->second.Alloca;
      } else {
        llvm::report_fatal_error(
            "Internal error: array argument must be a variable. "
            "This should have been caught by the TypeChecker.");
        return nullptr;
      }
    } else {
      // Standard scalar type casting
      TurfTypePtr ExpectedType = getTurfTypeFromLLVM(Arg.getType());

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
static llvm::Function *GetOrDeclareMalloc() {
  if (auto *F = TheModule->getFunction("malloc"))
    return F;
  auto *FT =
      llvm::FunctionType::get(llvm::PointerType::get(*TheContext, 0),
                              {llvm::Type::getInt64Ty(*TheContext)}, false);
  return llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "malloc",
                                TheModule.get());
}
static llvm::Function *GetOrDeclareRealloc() {
  if (auto *F = TheModule->getFunction("realloc"))
    return F;
  auto *FT = llvm::FunctionType::get(llvm::PointerType::get(*TheContext, 0),
                                     {llvm::PointerType::get(*TheContext, 0),
                                      llvm::Type::getInt64Ty(*TheContext)},
                                     false);
  return llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "realloc",
                                TheModule.get());
}

// Helper: emit a runtime bounds check for list access.
static void EmitListBoundsCheck(Value *IndexVal, Value *LenVal,
                                const std::string &ListName,
                                SourceLocation Loc) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
  Value *TooSmall = Builder->CreateICmpSLT(IndexVal, Zero, "idx_neg");
  Value *TooBig = Builder->CreateICmpSGE(IndexVal, LenVal, "idx_big");
  Value *OOB = Builder->CreateOr(TooSmall, TooBig, "oob");

  BasicBlock *OobBB = BasicBlock::Create(*TheContext, "list_oob", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "list_ok");
  Builder->CreateCondBr(OOB, OobBB, ContBB);

  Builder->SetInsertPoint(OobBB);
  CodegenIntrinsic("fail", {Builder->CreateGlobalString(
      "List index out of bounds for '" + ListName + "'")}, Loc);

  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateUnreachable();

  TheFunction->insert(TheFunction->end(), ContBB);
  Builder->SetInsertPoint(ContBB);
}

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
  if (!InitList.empty()) {
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
        ArrayBoundsError(Loc, "array", Idx, ArraySize).raise();
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
  Value *Ptr = codegenLValue();
  if (!Ptr)
    return nullptr;
  const TurfType *BaseTy = getExprType(Base.get());
  const TurfType *ElemTyT = nullptr;
  if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseTy))
    ElemTyT = PtrTy->getPointee().get();
  else
    ElemTyT = getArrayElementType(BaseTy);
  return Builder->CreateLoad(getLLVMType(ElemTyT), Ptr, "arrelem");
}

Value *ArrayAssignExprAST::codegen() {
  VarInfo *VI = nullptr;
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end())
    VI = &Iter->second;



  if (!VI || !isArrayType(VI->Type.get())) {
    llvm::report_fatal_error(llvm::StringRef("Internal error: '" + Name + "' is not an array"));
    return nullptr;
  }

  int ArraySize = dynamic_cast<const ::ArrayType *>(VI->Type.get())->getSize();
  const TurfType *ElemType = getArrayElementType(VI->Type.get());

  // Evaluate the index
  Value *IndexVal = Index->codegen();
  if (!IndexVal)
    return nullptr;

  // Index must be integer
  // Index type check is handled by TypeChecker.
  IndexVal = CastToType(IndexVal, TurfInt(), "arridx", Loc);

  // Compile-time bounds check for constant indices (local arrays only)
  if (ArraySize > 0) {
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal)) {
      long long Idx = CI->getSExtValue();
      if (Idx < 0 || Idx >= ArraySize) {
        ArrayBoundsError(Loc, Name, Idx, ArraySize).raise();
        return nullptr;
      }
    } else {
      EmitRuntimeBoundsCheck(IndexVal, ArraySize, Name, Loc);
    }
  }

  // Evaluate the RHS value
  Value *RHSVal = RHS->codegen();
  if (!RHSVal)
    return nullptr;

  // RHS void check and type compatibility are handled by TypeChecker.

  // Cast RHS to the element type before storing
  TurfTypePtr ElemTypePtr = getTurfTypeFromLLVM(getLLVMType(ElemType));
  RHSVal = CastToType(RHSVal, ElemTypePtr, "arrstore", Loc);

  // GEP + store
  // Two cases (same as ArrayAccessExprAST):
  //  A) Local array (ArraySize > 0): two-index GEP into [N x T] alloca.
  //  B) Pointer parameter (ArraySize == -1): load pointer, single-index GEP.
  Type *ElemTy = getLLVMType(ElemType);
  Value *ElemPtr;
  if (ArraySize > 0) {
    llvm::ArrayType *ArrTy = llvm::ArrayType::get(ElemTy, ArraySize);
    Value *Indices[] = {ConstantInt::get(Type::getInt64Ty(*TheContext), 0),
                        IndexVal};
    ElemPtr = Builder->CreateInBoundsGEP(ArrTy, VI->Alloca, Indices, "arrelem");
  } else {
    Type *PtrTy = PointerType::get(*TheContext, 0);
    Value *ArrPtr = Builder->CreateLoad(PtrTy, VI->Alloca, "arrptr");
    ElemPtr = Builder->CreateGEP(ElemTy, ArrPtr, IndexVal, "arrelem");
  }

  Builder->CreateStore(RHSVal, ElemPtr);
  return RHSVal;
}

Value *ArrayLengthExprAST::codegen() {
  VarInfo *VI = nullptr;
  auto Iter = NamedValues.find(Name);
  if (Iter != NamedValues.end())
    VI = &Iter->second;



  if (!VI || !isArrayType(VI->Type.get())) {
    llvm::report_fatal_error(
        llvm::StringRef("Internal error: '" + Name + "' is not an array. '.length' can only be used on arrays."));
    return nullptr;
  }

  return ConstantInt::get(
      Type::getInt64Ty(*TheContext),
      dynamic_cast<const ::ArrayType *>(VI->Type.get())->getSize());
}

// NOTE: ListDeclExprAST has been removed. List declarations are now
// represented as VarDeclExprAST with a GenericType("list", [elemType]).
// The VarDeclExprAST::codegen handles list initialization via the type system.

Value *MethodCallExprAST::codegen() {
  if (auto *Var = dynamic_cast<VariableExprAST *>(Base.get())) {
    const std::string &ModuleName = Var->getName();

    // Module method call — Resolver + TypeChecker have already validated name,
    // arity, and types.  Codegen blindly reads ResolvedCallTarget.
    bool IsModule = GlobalSymbolTable && GlobalSymbolTable->HasModule(ModuleName);
    if (IsModule) {
      // Codegen all arguments.
      std::vector<Value *> ArgVals;
      for (auto &Arg : Args) {
        Value *V = Arg->codegen();
        if (!V) return nullptr;
        ArgVals.push_back(V);
      }

      // ResolvedCallTarget is guaranteed set by the Resolver/TypeChecker.
      assert(ResolvedCallTarget &&
             "MethodCallExprAST: ResolvedCallTarget is null — TypeChecker bug");
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

    // Arity is guaranteed by the TypeChecker — if it's wrong here, it's a bug.
    assert(CalleeF->arg_size() == Args.size() + 1 &&
           "Struct method arity mismatch after TypeChecker — compiler bug");

    std::vector<Value *> ArgVals;
    Value *SelfVal = Base->codegenLValue();
    if (!SelfVal) {
      // If Base is an rvalue (like User(10)), we need a temporary alloca to
      // pass by reference
      Value *Val = Base->codegen();
      if (!Val)
        return nullptr;
      SelfVal = Builder->CreateAlloca(Val->getType(), nullptr, "tmp.self");
      Builder->CreateStore(Val, SelfVal);
    } else if (dynamic_cast<const TurfPointerType *>(getExprType(Base.get()))) {
      SelfVal = Builder->CreateLoad(llvm::PointerType::get(*TheContext, 0),
                                    SelfVal, "deref");
    }
    if (!SelfVal)
      return nullptr;
    ArgVals.push_back(SelfVal);

    unsigned Idx = 0;
    for (auto ArgIt = std::next(CalleeF->arg_begin());
         ArgIt != CalleeF->arg_end(); ++ArgIt, ++Idx) {
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

    return Builder->CreateCall(CalleeF, ArgVals, "methodcalltmp");
  }

  // TypeChecker should have rejected a method call on an unsupported type.
  llvm::report_fatal_error(
      "Internal error: method call on unsupported type. This is a compiler bug.");
  return nullptr;
}

Value *StructDeclExprAST::codegen() {
  std::vector<Type *> LLVMFieldTypes;
  for (const auto &Field : Fields) {
    Type *LLVMType = getLLVMType(Field.Type.get());
    if (!LLVMType) {
      LogErrorAt(Loc, "Unknown type for field '" + Field.Name + "'");
      return nullptr;
    }
    LLVMFieldTypes.push_back(LLVMType);
  }

  StructType *ST = StructType::create(*TheContext, LLVMFieldTypes, Name);
  if (!ST) {
    LogErrorAt(Loc, "Failed to create LLVM StructType for '" + Name + "'");
    return nullptr;
  }

  LLVMTypeCache[Name] = ST;

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
    if (!MatchedExpr) {
      // TypeChecker should have caught this — treat as internal error.
      LogErrorAt(Loc, "Internal error: missing field '" + DeclF.Name +
                          "' in codegen for struct '" + Name + "'");
      return nullptr;
    }

    Value *V = const_cast<ExprAST *>(MatchedExpr)->codegen();
    if (!V)
      return nullptr;
    EvaluatedArgs.push_back(V);

    TurfTypePtr ActTy;
    if (auto *VE = dynamic_cast<const VariableExprAST *>(MatchedExpr)) {
      auto It = NamedValues.find(VE->getName());
      if (It != NamedValues.end())
        ActTy = It->second.Type;
    }
    if (!ActTy)
      ActTy = getTurfTypeFromLLVM(V->getType());
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
            if (!isTypeCompatible(ArgTypes[i].get(), ParamTypes[i + 1].get())) {
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
  if (!BaseLVal)
    return nullptr;

  const TurfType *BaseType = getExprType(Base.get());

  if (auto *PtrTy = dynamic_cast<const TurfPointerType *>(BaseType)) {
    BaseType = PtrTy->getPointee().get();
    BaseLVal = Builder->CreateLoad(llvm::PointerType::get(*TheContext, 0),
                                   BaseLVal, "deref");
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
