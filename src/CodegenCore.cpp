/**
 * @file CodegenCore.cpp
 * @brief Shared LLVM setup, type mapping, casting, and cross-category codegen helpers.
 * @details Part of LLVM IR generation for TurfLang, split from the original
 *          monolithic Codegen.cpp by category (mirrors AST.h's
 *          ASTExpr/ASTStmt/ASTDecl/ASTAccess split -- see
 *          docs/architecture/compiler_pipeline.md).
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
#include "Types.h"
#include "CodegenInternal.h"
#include "Monomorphize.h" // Monomorphize::instantiatedDeclName() -- getLLVMType()'s GenericType branch, TYPE-002 S13b
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>
#include <unordered_map>

using namespace llvm;

std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<IRBuilder<>> Builder;
std::unique_ptr<Module> TheModule;

// TOOL-001 debug-info state (declared in Codegen.h -- see its own doc
// comment). g_EmitDebugInfo is the ONLY one of these four set from outside
// InitializeModule(): main.cpp sets it from the `-g` flag before calling
// InitializeModule(), which is what decides whether DBuilder/DebugCU get
// built at all.
bool g_EmitDebugInfo = false;
std::unique_ptr<DIBuilder> DBuilder;
DICompileUnit *DebugCU = nullptr;
DISubprogram *CurrentDebugScope = nullptr;

std::map<std::string, VarInfo> NamedValues;

// Current enclosing function context (nullptr at top level)
TurfTypePtr CurrentFuncReturnType = TurfVoid();
llvm::Function *CurrentFunction = nullptr;

// Tracks full param types for user-defined functions. Declared `extern` in
// CodegenInternal.h -- CodegenFunc.cpp (FuncDefExprAST/ExternFuncDeclExprAST)
// populates these; CodegenCore.cpp's own getExprType() also reads them.
std::map<std::string, std::vector<TurfTypePtr>> FuncParamTypes;
std::map<std::string, TurfTypePtr> FuncReturnTypes;

// Type cache for Monomorphization: maps type name -> LLVM type
std::map<std::string, llvm::Type *> LLVMTypeCache;

// MEM-002: see CodegenInternal.h's own doc comment. Reset to nullptr at the
// top of InitializeModule() below for the identical reason ResetArgvHelpers()
// exists -- a fresh compile must never carry a stale Value* left over from a
// previous TheModule (relevant for the compiler's own multi-file/REPL-like
// invocations, not a single ordinary `turf file.tr` run, but cheap to get
// right unconditionally).
llvm::Value *PendingGCAllocDescOverride = nullptr;

void InitializeModule() {
  // Holds types and constants
  TheContext = std::make_unique<LLVMContext>();

  // Holds functions
  TheModule = std::make_unique<Module>("Turf Compiler", *TheContext);

  // Builder to insert instructions
  Builder = std::make_unique<IRBuilder<>>(*TheContext);

  // See CodegenInternal.h's doc comment: CodegenFunc.cpp's main argv/argc
  // plumbing globals must not survive into a fresh TheModule.
  ResetArgvHelpers();
  PendingGCAllocDescOverride = nullptr;

  // TOOL-001: `-g` DWARF debug info. g_EmitDebugInfo is set by main.cpp
  // (from the `-g` flag) strictly BEFORE this function runs, so it's already
  // final by the time we get here. DBuilder/DebugCU are rebuilt fresh
  // alongside TheContext/TheModule above -- there is exactly one
  // InitializeModule() call per compiler invocation (a single `.tr` file
  // per process), so no stale-pointer-into-a-torn-down-Module hazard exists
  // here the way ResetArgvHelpers() guards against for ArgcStorageGV/
  // ArgvStorageGV.
  DBuilder.reset();
  DebugCU = nullptr;
  CurrentDebugScope = nullptr;
  if (g_EmitDebugInfo) {
    DBuilder = std::make_unique<DIBuilder>(*TheModule);

    // Both module flags are required for a DWARF-emitting backend to treat
    // this module's debug metadata as well-formed at all -- omitting either
    // one is silently accepted by LLVM (no verifier error) but produces an
    // object file with no .debug_* sections whatsoever, which is a much
    // harder failure to notice than a verifier error would be. Warning (not
    // Error) behavior matches every other frontend that sets these (clang
    // included): a conflicting flag from a differently-compiled object this
    // one might someday link against should warn, not hard-fail the link.
    TheModule->addModuleFlag(Module::Warning, "Debug Info Version",
                             DEBUG_METADATA_VERSION);
    TheModule->addModuleFlag(Module::Warning, "Dwarf Version", 4);

    // MainSourceFilename (Lexer.h) is set by main.cpp strictly before this
    // call (right after OpenSourceFile(), before InitializeModule()) -- see
    // that assignment's own comment for why it's kept distinct from the
    // transient, per-imported-module SourceFilename.
    SmallString<256> AbsPath(MainSourceFilename);
    // Failure here just means AbsPath stays relative (e.g. a filesystem
    // that can't report a current working directory) -- DW_AT_comp_dir
    // still gets SOME directory string either way, so there's no fallback
    // branch worth writing for an error this narrow.
    (void)sys::fs::make_absolute(AbsPath);
    StringRef Dir = sys::path::parent_path(AbsPath);
    StringRef Base = sys::path::filename(AbsPath);
    DIFile *MainFile = DBuilder->createFile(Base, Dir);

    // DW_LANG_C99: there is no assigned DWARF language code for TurfLang
    // (it isn't one of the languages DWARF standardizes), and every
    // DWARF-consuming debugger falls back to C-like expression evaluation
    // for an unrecognized/generic code anyway -- picking the closest
    // structurally-similar standardized code (like C's) is the same choice
    // most small/custom-language compilers make, rather than inventing a
    // vendor extension code nothing downstream would recognize.
    // isOptimized=false unconditionally, not from opts.OptLevel: main.cpp
    // sets g_EmitDebugInfo/calls InitializeModule() before it ever looks at
    // -O, and INV-OPT-1 (INVARIANTS.md) is exactly the structural fact that
    // Codegen never receives an opt level at all -- threading it in here
    // just for this one cosmetic DWARF producer flag would be a real,
    // if narrow, crack in that invariant for zero behavioral benefit (the
    // flag only affects how a debugger prints "was this optimized", it
    // changes no code generation).
    DebugCU = DBuilder->createCompileUnit(
        dwarf::DW_LANG_C99, MainFile, "turf", /*isOptimized=*/false, "",
        /*RV=*/0);
  }
}


// ─── `!T` / `error` runtime representation ───────────────────────────────────
// (docs/architecture/errors_and_recover.md)
//
// See getLLVMType()'s TurfErrorUnionType/TurfUserErrorType/AnyErrorType
// branches just below for the overall design: `!T` is one fixed struct type
// regardless of T (the success value is boxed into a 64-bit slot), and every
// `error`-type value (constructed or type-erased as AnyErrorType) is a
// pointer to a heap-allocated tagged payload.

/// `{ i1 isOk, i64 successBits, i8* errorPayload }` -- cached, built once.
static llvm::StructType *getFallibleValueType() {
  static llvm::StructType *Ty = nullptr;
  if (!Ty) {
    Ty = llvm::StructType::create(*TheContext, "FallibleValue");
    Ty->setBody({llvm::Type::getInt1Ty(*TheContext),
                llvm::Type::getInt64Ty(*TheContext),
                PointerType::get(*TheContext, 0)});
  }
  return Ty;
}


/// `{ i8* tag, i8* message, i8* payload }` -- cached, built once. An error
/// value's runtime representation, UNIFORM across every occurrence (a bare
/// `ErrorType::Variant` reference, an `ErrorType::Variant("msg")` and a
/// `ErrorType::Variant(path, "msg")` construction produce the exact same
/// SHAPE, differing only in what `message` and `payload` point to) --
/// deliberately not a discriminated union, since telling the cases apart at
/// runtime without a discriminant would mean reading an arbitrary number of
/// bytes past a bare tag string's own NUL terminator (STD-002). `payload`
/// (LEC-004, FREEZE.md §6.18) is null for a variant with no fields, else a
/// GC-allocated per-variant struct (getErrorPayloadStructType()) with its own
/// type descriptor, so the collector traces a `string` or collection held in
/// a payload exactly as it traces `message`. `errorPayload` in
/// FallibleValue above, and `err`'s own alloca (RecoverExprAST::codegen(),
/// this file, further down), both still just hold an OPAQUE i8*/ptr to one
/// of these -- neither needed any change, since LLVM pointers carry no
/// static type at that level.
static llvm::StructType *getErrorValueType() {
  static llvm::StructType *Ty = nullptr;
  if (!Ty) {
    Ty = llvm::StructType::create(*TheContext, "TurfErrorValue");
    Ty->setBody({PointerType::get(*TheContext, 0),
                PointerType::get(*TheContext, 0),
                PointerType::get(*TheContext, 0)});
  }
  return Ty;
}


/// The registered declaration of error type @p ErrorTypeName, or nullptr.
static const TurfUserErrorType *lookupErrorTypeDecl(const std::string &ErrorTypeName) {
  if (!GlobalSymbolTable)
    return nullptr;
  auto T = GlobalSymbolTable->LookupType(ErrorTypeName);
  return T ? dynamic_cast<const TurfUserErrorType *>(T.get()) : nullptr;
}


std::vector<TurfTypePtr> ErrorVariantFieldTypes(const std::string &ErrorTypeName,
                                                const std::string &VariantName) {
  std::vector<TurfTypePtr> Out;
  if (const auto *ET = lookupErrorTypeDecl(ErrorTypeName))
    if (const auto *Info = ET->getVariantInfo(VariantName))
      for (const auto &F : Info->Fields)
        Out.push_back(F.Type);
  return Out;
}


/// One named LLVM struct per error variant that has fields, holding those
/// fields by value in declaration order. NAMED so that EmitAllTurfTypeDescs()
/// (which emits a descriptor for every identified struct in the module)
/// covers it without any special case -- a payload is a GC block like any
/// struct instance, and its descriptor is what keeps a `string` field inside
/// it alive across a collection. Built lazily at first use, which is always
/// inside a function body: by then every struct a field can name is complete
/// (main.cpp builds all of them before any body), so the layout is never
/// computed against an opaque stub.
llvm::StructType *getErrorPayloadStructType(const std::string &ErrorTypeName,
                                            const std::string &VariantName) {
  static std::map<std::string, llvm::StructType *> Cache;
  const std::string Key = ErrorTypeName + "::" + VariantName;
  auto It = Cache.find(Key);
  if (It != Cache.end())
    return It->second;

  std::vector<TurfTypePtr> Fields = ErrorVariantFieldTypes(ErrorTypeName, VariantName);
  if (Fields.empty())
    return nullptr;

  std::vector<llvm::Type *> LLVMFields;
  for (const auto &F : Fields)
    LLVMFields.push_back(getLLVMType(F.get()));
  auto *Ty = llvm::StructType::create(*TheContext, LLVMFields,
                                      "errpayload." + ErrorTypeName + "." + VariantName);
  Cache[Key] = Ty;
  return Ty;
}


/// Builds a global, private-linkage NUL-terminated C string constant and
/// returns a `Constant*` pointer to its first byte -- usable as a field
/// initializer inside another constant (e.g. `ConstantStruct::get()`),
/// which `Builder->CreateGlobalString()` elsewhere in this file is NOT (it
/// returns a `Value*`/`GlobalVariable*` suitable for ordinary runtime use,
/// but building the POINTER-typed, already-decayed constant a
/// ConstantStruct field needs takes one more step). Mirrors
/// CodegenStruct.cpp's `emitTurfTypeDesc()`/NameGV+NamePtr idiom exactly
/// (RT-001) -- the only other place in this codebase that already builds a
/// global constant struct holding a string pointer field.
static llvm::Constant *BuildGlobalCStringPtr(const std::string &S,
                                             const std::string &GVName) {
  llvm::Constant *StrConst =
      llvm::ConstantDataArray::getString(*TheContext, S, /*AddNull=*/true);
  auto *GV = new llvm::GlobalVariable(
      *TheModule, StrConst->getType(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, StrConst, GVName);
  llvm::Constant *ZeroIdx =
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(*TheContext), 0);
  llvm::Constant *Indices[] = {ZeroIdx, ZeroIdx};
  return llvm::ConstantExpr::getInBoundsGetElementPtr(StrConst->getType(), GV,
                                                       Indices);
}


/// Builds the BARE tag string (i8*, e.g. "MathError::DivisionByZero") for a
/// declared error variant -- no message, no struct wrapping. This is the
/// entire pre-STD-002 `BuildErrorTag()`, kept as its own helper (renamed)
/// because it is still exactly what an equality comparison needs
/// (GetErrorTagValue() below): comparing WHICH VARIANT occurred never needs
/// to look at a message, so this stays the cheap, direct, no-allocation
/// path it always was.
static llvm::Constant *BuildBareTagString(const std::string &ErrorTypeName,
                                          const std::string &VariantName) {
  return llvm::dyn_cast<llvm::Constant>(
      Builder->CreateGlobalString(ErrorTypeName + "::" + VariantName, "errtag"));
}

Value *BuildErrorVariantTag(const std::string &ErrorTypeName,
                            const std::string &VariantName) {
  return BuildBareTagString(ErrorTypeName, VariantName);
}


/// Declares (once) or looks up `libturfrt`'s `turf_rt_alloc` -- the typed,
/// COLLECTED GC-heap allocation entrypoint (`runtime/turfrt.h`), used to
/// heap-box a success value that doesn't fit in a FallibleValue's 64-bit
/// successBits slot (BoxToI64Slot()'s struct branch) and a constructed
/// error value and its payload (CodegenErrorVariantConstruct(), below). OVH-002
/// (docs/development/changes/OVH-002.md): both call sites used to
/// reach a bare `malloc` instead -- correct before MEM-002's collector
/// existed, a live use-after-free once it did, because a GC-heap pointer
/// (a `string` field inside the boxed value, the error value's own message
/// pointer) stored ONLY inside that manual-heap block was invisible to the
/// collector's mark phase (F-MEM-3: manual memory is never traced). Same
/// lookup-or-create pattern `getOrCreateStrcmpFn()`/the gc_alloc dispatch
/// in `CodegenFunc.cpp` already use for a bare libturfrt entrypoint.
Function *getOrCreateTurfRtAllocFn() {
  Function *AllocF = TheModule->getFunction("turf_rt_alloc");
  if (!AllocF) {
    PointerType *PtrTy = PointerType::get(*TheContext, 0);
    llvm::FunctionType *FT = llvm::FunctionType::get(
        PtrTy, {llvm::Type::getInt64Ty(*TheContext), PtrTy}, false);
    AllocF = Function::Create(FT, Function::ExternalLinkage, "turf_rt_alloc",
                              TheModule.get());
  }
  return AllocF;
}


/// Box an arbitrary Turf value into a 64-bit slot for storage in a
/// FallibleValue's successBits field -- same "reinterpret without loss"
/// trick CastToType() already uses to erase a generic type parameter into a
/// void* (bitcast for same-width types, [sz]ext for narrower ones,
/// ptrtoint for pointers), just targeting an i64 slot instead of a pointer.
///
/// A struct success type (e.g. `!TcpConnection`) doesn't fit inline -- found
/// live while converting net.tr's TcpConnection::connect() to `!T` (a
/// single-field `{fd: int}` struct is still a real LLVM struct, not a bare
/// i64, so returning it silently truncated to zero bits before this branch
/// existed). Heap-allocates space on the GC HEAP (OVH-002: not a bare
/// `malloc` -- see getOrCreateTurfRtAllocFn()'s own doc comment), stores
/// the struct there, and boxes the POINTER instead -- same "must outlive
/// the function" reasoning as an error tag string, just for the success
/// side.
static Value *BoxToI64Slot(Value *Val) {
  llvm::Type *Ty = Val->getType();
  llvm::Type *I64Ty = llvm::Type::getInt64Ty(*TheContext);
  if (Ty->isIntegerTy(64))
    return Val;
  if (Ty->isIntegerTy(32))
    return Builder->CreateSExt(Val, I64Ty, "box_i32");
  if (Ty->isIntegerTy(1))
    return Builder->CreateZExt(Val, I64Ty, "box_bool");
  if (Ty->isIntegerTy(8))
    return Builder->CreateZExt(Val, I64Ty, "box_char");
  if (Ty->isDoubleTy())
    return Builder->CreateBitCast(Val, I64Ty, "box_dbl");
  if (Ty->isPointerTy())
    return Builder->CreatePtrToInt(Val, I64Ty, "box_ptr");
  if (Ty->isVoidTy())
    // void success values reach here with no meaningful bits to store --
    // callers never read successBits back for a void T anyway.
    return llvm::ConstantInt::get(I64Ty, 0);
  // Anything else (structs, fixed-size arrays by value): too wide for the
  // inline slot, so heap-box it instead, on the GC heap, with a real
  // descriptor -- a NAMED struct type's own descriptor (the SAME one
  // EmitAllTurfTypeDescs() emits for it, referenced before that late pass
  // runs, OVH-002) if `Ty` is one, else the general per-element-type
  // descriptor (getOrCreateElementTurfTypeDesc(), already handles an
  // anonymous type like a fixed-size array). Primitive `T` never reaches
  // this branch (every scalar case above returns first), so only
  // aggregates ever need this.
  Value *Size = ConstantInt::get(I64Ty, TheModule->getDataLayout().getTypeAllocSize(Ty));
  Value *DescArg;
  if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty))
    DescArg = getOrForwardDeclareTurfTypeDescGlobal(ST);
  else
    DescArg = getOrCreateElementTurfTypeDesc(Ty);
  Value *HeapSlot = Builder->CreateCall(getOrCreateTurfRtAllocFn(), {Size, DescArg}, "box_slot");
  Builder->CreateStore(Val, HeapSlot);
  return Builder->CreatePtrToInt(HeapSlot, I64Ty, "box_ptr_wide");
}


/// FDN-001 (DEC-5): codegen for the `heap(expr): &T` bare intrinsic --
/// see this function's own declaration (CodegenInternal.h) for the full
/// contract. Shares BoxToI64Slot()'s aggregate-boxing size/descriptor logic
/// above verbatim (both need "sizeof(T) bytes on the GC heap, with T's real
/// descriptor"); the only difference is the return value, a real pointer
/// rather than a ptrtoint'd i64 slot, since heap()'s whole point is handing
/// back a genuine &T, not smuggling an oversized value through a fixed-width
/// scalar slot.
Value *HeapAllocateValue(Value *Val) {
  llvm::Type *Ty = Val->getType();
  llvm::Type *I64Ty = llvm::Type::getInt64Ty(*TheContext);
  Value *Size = ConstantInt::get(
      I64Ty, TheModule->getDataLayout().getTypeAllocSize(Ty));
  Value *DescArg;
  if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty))
    DescArg = getOrForwardDeclareTurfTypeDescGlobal(ST);
  else
    DescArg = getOrCreateElementTurfTypeDesc(Ty);
  Value *HeapSlot =
      Builder->CreateCall(getOrCreateTurfRtAllocFn(), {Size, DescArg}, "heap_slot");
  Builder->CreateStore(Val, HeapSlot);
  return HeapSlot;
}


/// Reverse of BoxToI64Slot(): reinterpret a FallibleValue's successBits back
/// to the real LLVM representation of Turf type `T`.
Value *UnboxFromI64Slot(Value *Slot, const TurfType *T) {
  llvm::Type *TargetTy = getLLVMType(T);
  if (TargetTy->isIntegerTy(64))
    return Slot;
  if (TargetTy->isIntegerTy(32))
    return Builder->CreateTrunc(Slot, llvm::Type::getInt32Ty(*TheContext), "unbox_i32");
  if (TargetTy->isIntegerTy(1))
    return Builder->CreateTrunc(Slot, llvm::Type::getInt1Ty(*TheContext), "unbox_bool");
  if (TargetTy->isIntegerTy(8))
    return Builder->CreateTrunc(Slot, llvm::Type::getInt8Ty(*TheContext), "unbox_char");
  if (TargetTy->isDoubleTy())
    return Builder->CreateBitCast(Slot, llvm::Type::getDoubleTy(*TheContext), "unbox_dbl");
  if (TargetTy->isPointerTy())
    return Builder->CreateIntToPtr(Slot, TargetTy, "unbox_ptr");
  if (TargetTy->isVoidTy())
    return Slot; // caller never uses this value
  // Struct/array: the slot holds a heap pointer (see BoxToI64Slot()), load
  // the real value back through it.
  Value *HeapSlot = Builder->CreateIntToPtr(Slot, PointerType::get(*TheContext, 0), "unbox_slot");
  return Builder->CreateLoad(TargetTy, HeapSlot, "unbox_wide");
}


/// Declares (once) or looks up the C `strcmp` used to compare two error
/// tag strings (see BuildErrorTag()/GetErrorTagValue()).
Function *getOrCreateStrcmpFn() {
  Function *StrcmpF = TheModule->getFunction("strcmp");
  if (!StrcmpF) {
    Type *I8Ptr = PointerType::get(*TheContext, 0);
    llvm::FunctionType *FT = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(*TheContext), {I8Ptr, I8Ptr}, false);
    StrcmpF = Function::Create(FT, Function::ExternalLinkage, "strcmp",
                               TheModule.get());
  }
  return StrcmpF;
}


/// Builds the runtime value of a BARE `ErrorType::Variant` reference
/// (docs/architecture/errors_and_recover.md), e.g. `return
/// MathError::DivisionByZero;` -- a pointer to a global constant
/// `TurfErrorValue{tag, message="", payload=null}` (STD-002: see
/// getErrorValueType()'s own doc comment for why this is the UNIFORM shape
/// every error value has, message-carrying or not). No heap allocation is
/// needed here specifically -- everything is compile-time-known -- unlike
/// ErrorVariantConstruct's own Codegen branch (FuncCallExprAST::codegen(),
/// CodegenFunc.cpp), which GC-heap-allocates (OVH-002) because its message
/// and payload are, in general, runtime-computed values. Only a variant with
/// NO fields reaches here: a bare reference to one that has fields is
/// rejected by the Resolver (it has no payload to fill in), except as an
/// `==`/`!=` operand, which never builds a value (GetErrorTagValue()).
Value *BuildErrorTag(const std::string &ErrorTypeName,
                            const std::string &VariantName) {
  llvm::StructType *EVTy = getErrorValueType();
  llvm::Constant *TagPtr =
      BuildBareTagString(ErrorTypeName, VariantName);
  llvm::Constant *MsgPtr = BuildGlobalCStringPtr("", "errmsg_empty");
  llvm::Constant *NoPayload =
      llvm::ConstantPointerNull::get(PointerType::get(*TheContext, 0));
  llvm::Constant *EVConst =
      llvm::ConstantStruct::get(EVTy, {TagPtr, MsgPtr, NoPayload});
  auto *GV = new llvm::GlobalVariable(
      *TheModule, EVTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      EVConst, "errval");
  return GV;
}


/// `ErrorType::Variant(fields..., [message])` -- the construction of an error
/// value: STD-002's message and LEC-004's payload (FREEZE.md §6.18). Unlike
/// BuildErrorTag() (fully compile-time-known, a global constant suffices), the
/// message and the fields are in general RUNTIME values, so this heap-allocates
/// on the GC HEAP (OVH-002: was a bare `malloc` before MEM-002's collector
/// existed to trace into it -- `message` is a GC-heap `string` pointer, and a
/// `TurfErrorValue` reachable only through manual memory made it invisible to
/// the collector's mark phase, F-MEM-3) -- same "the value must outlive the
/// function that returns it" reasoning BoxToI64Slot() already uses for an
/// oversized success value, just for the error side.
///
/// Two blocks for a variant with fields: the payload struct first, then the
/// `TurfErrorValue` pointing at it. Each has the descriptor
/// `EmitAllTurfTypeDescs()` emits for its (named) LLVM struct type, referenced
/// here before that late pass runs (getOrForwardDeclareTurfTypeDescGlobal(),
/// OVH-002), so a `string` or `List<T>` inside the payload is traced exactly
/// like the message. Nothing needs the payload's pointer kept anywhere but a
/// register between the two allocations: the collector scans registers
/// conservatively, so the second allocation cannot free the first.
/// Called from FuncCallExprAST::codegen() (CodegenFunc.cpp), which evaluates
/// the arguments and passes the resulting Value*s in.
Value *CodegenErrorVariantConstruct(const std::string &ErrorTypeName,
                                    const std::string &VariantName,
                                    const std::vector<Value *> &FieldVals,
                                    Value *MessageVal) {
  llvm::DataLayout DL = TheModule->getDataLayout();
  PointerType *PtrTy = PointerType::get(*TheContext, 0);

  Value *PayloadPtr = ConstantPointerNull::get(PtrTy);
  if (!FieldVals.empty()) {
    llvm::StructType *PTy = getErrorPayloadStructType(ErrorTypeName, VariantName);
    assert(PTy && PTy->getNumElements() == FieldVals.size() &&
           "LEC-004: constructor arguments must match the variant's fields");
    Value *PDesc = getOrForwardDeclareTurfTypeDescGlobal(PTy);
    PayloadPtr = Builder->CreateCall(
        getOrCreateTurfRtAllocFn(),
        {ConstantInt::get(Type::getInt64Ty(*TheContext),
                          DL.getTypeAllocSize(PTy).getFixedValue()),
         PDesc},
        "errpayload");
    for (size_t I = 0; I < FieldVals.size(); ++I)
      Builder->CreateStore(FieldVals[I],
                           Builder->CreateStructGEP(PTy, PayloadPtr, I, "payload.field"));
  }

  llvm::StructType *EVTy = getErrorValueType();
  uint64_t Size = DL.getTypeAllocSize(EVTy).getFixedValue();
  Value *DescArg = getOrForwardDeclareTurfTypeDescGlobal(EVTy);
  Value *HeapSlot = Builder->CreateCall(
      getOrCreateTurfRtAllocFn(),
      {ConstantInt::get(Type::getInt64Ty(*TheContext), Size), DescArg},
      "errval_msg");
  Value *TagFieldPtr = Builder->CreateStructGEP(EVTy, HeapSlot, 0, "tag");
  Builder->CreateStore(BuildBareTagString(ErrorTypeName, VariantName), TagFieldPtr);
  Value *MsgFieldPtr = Builder->CreateStructGEP(EVTy, HeapSlot, 1, "message");
  Builder->CreateStore(MessageVal ? MessageVal : BuildGlobalCStringPtr("", "errmsg_empty"),
                       MsgFieldPtr);
  Value *PayloadFieldPtr = Builder->CreateStructGEP(EVTy, HeapSlot, 2, "payload");
  Builder->CreateStore(PayloadPtr, PayloadFieldPtr);
  return HeapSlot;
}


/// Reads field @p FieldIdx (0 = tag, 1 = message, 2 = payload) out of a
/// runtime `TurfErrorValue*` value -- shared by GetErrorTagValue() (field 0,
/// for `==`/`!=`), FieldAccessExprAST::codegen()'s `err.message` special case
/// (field 1, CodegenStruct.cpp) and an error-variant pattern (fields 0 and
/// 2, CodegenPattern.cpp).
Value *LoadErrorValueField(Value *ErrValPtr, unsigned FieldIdx, const char *Name) {
  llvm::StructType *EVTy = getErrorValueType();
  Value *FieldPtr = Builder->CreateStructGEP(EVTy, ErrValPtr, FieldIdx, Name);
  return Builder->CreateLoad(PointerType::get(*TheContext, 0), FieldPtr,
                             std::string(Name) + ".val");
}


/// STD-002: see this function's own declaration (CodegenInternal.h) for the
/// full forward-or-stamp contract.
Value *BuildCallerLocationValue(const SourceLocation &StampLoc) {
  auto It = NamedValues.find("caller_location");
  if (It != NamedValues.end())
    return Builder->CreateLoad(getLLVMType(TurfString().get()), It->second.Alloca,
                               "loc_fwd");
  std::string LocStr = (StampLoc.Filename.empty() ? "<unknown>" : StampLoc.Filename) +
                        ":" + std::to_string(StampLoc.Line);
  return Builder->CreateGlobalString(LocStr, "caller_loc");
}


/// True if `E` is an error-tag-shaped operand for `==`/`!=`
/// (docs/architecture/errors_and_recover.md): either a bare
/// `ErrorType::Variant` reference, or a variable already known (via its
/// Resolver-set ResolvedSymbol, not TypeChecker's type map -- see
/// IsErrorConstructionExpr's doc comment for why) to hold an error tag -- `err`
/// inside `recover as err run { ... }` being the only way such a variable is
/// ever created today.
bool IsErrorTagExpr(const ExprAST *E) {
  if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(E))
    return SA->ResolvedKind == ScopeAccessKind::ErrorVariantRef;
  // STD-002: `ErrorType::Variant("msg")` -- a FuncCallExprAST wrapping the
  // same ScopeAccessExprAST callee shape, mirroring how ConstructorCall/
  // DefaultConstruct are ALSO FuncCallExprAST-wrapped (§2.8.1a of
  // modules_and_scope_resolution.md). `err == SomeError::Variant("msg")`
  // is unusual (comparing against a fresh construction rather than a bare
  // reference) but not illegal -- handled here rather than left to crash.
  if (auto *FC = dynamic_cast<const FuncCallExprAST *>(E)) {
    if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(FC->getCallee()))
      return SA->ResolvedKind == ScopeAccessKind::ErrorVariantConstruct;
  }
  if (auto *Var = dynamic_cast<const VariableExprAST *>(E)) {
    if (const auto *Sym = Var->getResolvedSymbol())
      return Sym->Type && (Sym->Type->isAnyError() || Sym->Type->isUserError());
  }
  return false;
}


/// Returns the BARE TAG string (i8*, field 0 of TurfErrorValue) for an
/// error-tag-shaped `==`/`!=` operand -- comparing WHICH VARIANT occurred
/// never needs to look at a message (STD-002: see getErrorValueType()'s
/// own doc comment). A bare `ErrorType::Variant` reference or an
/// `ErrorType::Variant("msg")` construction both have a compile-time-known
/// tag, so it's built directly via BuildBareTagString() rather than
/// evaluating the whole construction (and, for the "msg" form, the message
/// expression itself, which may have side effects -- comparing `err ==
/// SomeError::Variant("msg")` must not ALSO re-run whatever produced
/// "msg"). Everything else (`err`, or any other AnyErrorType/
/// TurfUserErrorType-typed expression) codegens normally to a
/// `TurfErrorValue*` now, so its tag needs an actual field read.
Value *GetErrorTagValue(const ExprAST *E) {
  if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(E)) {
    if (SA->ResolvedKind == ScopeAccessKind::ErrorVariantRef)
      return BuildBareTagString(SA->getLeftName(), SA->getRightName());
  }
  if (auto *FC = dynamic_cast<const FuncCallExprAST *>(E)) {
    if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(FC->getCallee())) {
      if (SA->ResolvedKind == ScopeAccessKind::ErrorVariantConstruct)
        return BuildBareTagString(SA->getLeftName(), SA->getRightName());
    }
  }
  Value *ErrVal = const_cast<ExprAST *>(E)->codegen();
  if (!ErrVal)
    return nullptr;
  return LoadErrorValueField(ErrVal, /*FieldIdx=*/0, "tag");
}


/// True if `E` is a value-producing `ErrorType::Variant` shape
/// (docs/architecture/errors_and_recover.md) -- either the bare reference
/// or a `ErrorType::Variant(fields..., "msg")` construction -- mirrors
/// Resolver's CollectReturnedErrorTypeNames' identical structural
/// recognition, used here to decide whether a `return`ed value is an error
/// tag or a genuine success value, since Codegen can't rely on
/// TypeChecker's inferred types (this compiler resolves/typechecks/
/// codegens function bodies in a pre-pass that runs its own full pipeline
/// on its own AST instance -- see main.cpp's "Pre-pass sub-pass 2b"
/// comment -- so only structural info set directly on the AST node by the
/// Resolver, like ResolvedKind, is safe to depend on here).
bool IsErrorConstructionExpr(const ExprAST *E) {
  if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(E))
    return SA->ResolvedKind == ScopeAccessKind::ErrorVariantRef;
  if (auto *FC = dynamic_cast<const FuncCallExprAST *>(E)) {
    if (auto *SA = dynamic_cast<const ScopeAccessExprAST *>(FC->getCallee()))
      return SA->ResolvedKind == ScopeAccessKind::ErrorVariantConstruct;
  }
  return false;
}


/// Wraps a raw success value (or, if IsError, an error payload i8*) into a
/// `!T` FallibleValue for a `return` inside a `!T`-declared function.
/// `RawVal` may be null when the function's success type is void (a bare
/// `return;` inside a `!void` function still needs to produce a real
/// FallibleValue -- the function's actual LLVM return type is never void
/// once its declared type is `!void`).
Value *WrapFallibleValue(Value *RawVal, bool IsError) {
  llvm::StructType *FVTy = getFallibleValueType();
  llvm::Type *I1Ty = llvm::Type::getInt1Ty(*TheContext);
  llvm::Type *I64Ty = llvm::Type::getInt64Ty(*TheContext);
  Value *Result = llvm::UndefValue::get(FVTy);
  Value *IsOk = ConstantInt::get(I1Ty, IsError ? 0 : 1);
  Result = Builder->CreateInsertValue(Result, IsOk, 0, "fv.isok");
  Value *SuccessBits = (!IsError && RawVal) ? BoxToI64Slot(RawVal)
                                            : ConstantInt::get(I64Ty, 0);
  Result = Builder->CreateInsertValue(Result, SuccessBits, 1, "fv.bits");
  Value *ErrPtr = IsError ? RawVal
                          : ConstantPointerNull::get(PointerType::get(*TheContext, 0));
  Result = Builder->CreateInsertValue(Result, ErrPtr, 2, "fv.errptr");
  return Result;
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
  if (T->isNamed("int32"))
    return LLVMTypeCache[Key] = llvm::Type::getInt32Ty(*TheContext);
  if (T->isNamed("double"))
    return LLVMTypeCache[Key] = llvm::Type::getDoubleTy(*TheContext);
  if (T->isNamed("bool"))
    return LLVMTypeCache[Key] = llvm::Type::getInt1Ty(*TheContext);
  if (T->isNamed("char"))
    return LLVMTypeCache[Key] = llvm::Type::getInt8Ty(*TheContext);
  if (T->isNamed("string"))
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);
  if (T->isNamed("void"))
    return LLVMTypeCache[Key] = llvm::Type::getVoidTy(*TheContext);
  // `ptr` — opaque pointer (i8* in LLVM IR)
  if (T->isNamed("ptr") || dynamic_cast<const OpaquePointerType *>(T))
    return LLVMTypeCache[Key] = llvm::PointerType::get(*TheContext, 0);

  // First-class function value (fn(T): R) — functions are already pointer-
  // shaped values under LLVM's opaque-pointer model; a variable/parameter of
  // this type just stores that pointer, same representation as `ptr`/T*.
  if (T->isFunc())
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
    return LLVMTypeCache[Key] = llvm::StructType::create(
               *TheContext, TypeIdentity::llvmName(Key));
  }

  // TYPE-002 (S14a): erasure is gone -- INV-GEN-1 (INVARIANTS.md): after
  // the monomorphizer runs, no TurfGenericParamType may reach Codegen.
  // Every occurrence in the whole program is either inside a generic
  // TEMPLATE (excluded from Codegen entirely by main.cpp's sub-pass 3a/3b
  // template-skip guard, widened this session to cover every generic
  // declaration, struct-bound or not) or has already been substituted to a
  // concrete type by the monomorphizer (Monomorphize.cpp) before its clone
  // ever reaches here. Debug-build assertion, never removed (the plan's
  // own "Completion criterion 1"); a release build that somehow still hits
  // this falls through to the function's own "unknown/future types"
  // catch-all at the bottom (getDoubleTy) rather than crash outright.
  assert(!dynamic_cast<const TurfGenericParamType *>(T) &&
        "INV-GEN-1 violated: a TurfGenericParamType reached getLLVMType() "
        "-- an un-monomorphized generic template reached Codegen. This is "
        "a compiler bug.");

  // GenericType instantiations (e.g. List<int>) — TYPE-002 (S13b): look up
  // the SPECIFIC per-instantiation struct, registered in GlobalSymbolTable/
  // LLVMTypeCache under its own mangled name by
  // Monomorphize::drainStructRequests() (S13a) and codegenned for real by
  // the ordinary, unmodified StructDeclExprAST::codegen() once its clone
  // reaches main.cpp's sub-pass 2c. This REPLACES the erasure-era version
  // of this branch, which collapsed every instantiation of the same base
  // struct onto ONE shared layout (correct only because Type Erasure made
  // every T/K/V a uniform 8-byte slot) -- see the change record's *Removed
  // assumptions* for why that collapse is gone and what it takes with it
  // (the >8-byte `Invalid bitcast` ICE).
  if (auto *GT = dynamic_cast<const GenericType *>(T)) {
    std::string MangledName =
        Monomorphize::instantiatedDeclName(GT->getBaseName(), GT->getTypeArgs());
    auto InstIt = LLVMTypeCache.find(MangledName);
    if (InstIt != LLVMTypeCache.end())
      return LLVMTypeCache[Key] = InstIt->second;
    // Not yet seen under its own mangled name -- create a forward-declared
    // opaque stub, EXACTLY like StructDeclExprAST::codegen()'s own Step 1
    // does for a self-referential field (issue #176), and like this
    // branch's OWN prior version did for the shared base name. Critically,
    // register it under the MANGLED name too, not just the outer `Key` --
    // StructDeclExprAST::codegen() looks up LLVMTypeCache[Name] by that
    // SAME mangled name for a monomorphized clone (its own Step 1, Name is
    // already the clone's mangled instantiatedDeclName() -- see
    // FuncDefExprAST's identical rename-before-resolve contract,
    // Monomorphize.cpp), so caching only under `Key` here would leave that
    // lookup missing the stub, creating a SECOND, disconnected StructType
    // and leaving every field that already resolved to this orphaned stub
    // permanently opaque -- the exact "GEP into unsized type" crash the
    // prior, base-name version of this comment already described, just for
    // a different key. Reaching this fallback for an instantiation that
    // will NEVER be codegenned (not merely "not yet") would mean some
    // GenericType occurrence's instantiation request was never recorded/
    // drained -- TypeChecker's recording sites (visitVarDecl, visitMethodCall,
    // visitFieldAccess) are meant to make that unreachable; if it happens
    // anyway, this stub at least isolates the damage to ONE instantiation's
    // own (permanently opaque) type instead of corrupting a DIFFERENT
    // instantiation's real layout the way sharing the base name used to.
    llvm::StructType *Stub =
        llvm::StructType::create(*TheContext, TypeIdentity::llvmName(MangledName));
    LLVMTypeCache[MangledName] = Stub;
    return LLVMTypeCache[Key] = Stub;
  }

  // User-defined enums — always stored as their underlying integer type (i64).
  if (dynamic_cast<const TurfEnumType *>(T)) {
    return LLVMTypeCache[Key] = llvm::Type::getInt64Ty(*TheContext);
  }

  // User-declared `error` type value (docs/architecture/errors_and_recover.md)
  // -- a global string constant identifying (error type, variant), e.g.
  // "MathError::DivisionByZero" (see BuildErrorTag()) -- variants are bare
  // names with no payload, so this tag string IS the whole value, an i8*
  // like any other string. AnyErrorType (the type of `err` inside
  // `recover as err run { ... }`) is the SAME tag string with its concrete
  // error type erased, so it gets the identical representation.
  if (T->isUserError() || T->isAnyError()) {
    return LLVMTypeCache[Key] = PointerType::get(*TheContext, 0);
  }

  // `!T` (docs/architecture/errors_and_recover.md) -- uniformly `{ i1 isOk, i64
  // successBits, i8* errorPayload }` regardless of what T is: the success value
  // is boxed into the i64 slot (see BoxToI64Slot()/UnboxFromI64Slot()) exactly
  // like this compiler already boxes a generic type parameter into a void*
  // (CastToType()'s "Type Erasure" branches) -- same idea, fixed-width slot
  // instead of a pointer. This keeps ONE concrete LLVM type for every `!T`
  // no matter what T is, so a function's LLVM return type/a variable's LLVM
  // type never needs to vary per success type.
  if (T->isErrorUnion()) {
    return LLVMTypeCache[Key] = getFallibleValueType();
  }

  // NamedType — may refer to a user-defined struct, enum, or error type
  // registered in GlobalSymbolTable. Check GlobalSymbolTable first so
  // enums/error types resolve to their real representation instead of
  // falling through to the opaque-struct-forward-decl path below (which is
  // only correct for structs).
  if (dynamic_cast<const NamedType *>(T)) {
    // Try to resolve through GlobalSymbolTable first
    if (GlobalSymbolTable) {
      if (auto Registered = GlobalSymbolTable->LookupType(T->getName())) {
        if (dynamic_cast<const TurfEnumType *>(Registered.get()))
          return LLVMTypeCache[Key] = llvm::Type::getInt64Ty(*TheContext);
        if (Registered->isUserError())
          return LLVMTypeCache[Key] = PointerType::get(*TheContext, 0);
      }
    }
    auto CacheIt = LLVMTypeCache.find(Key);
    if (CacheIt != LLVMTypeCache.end())
      return CacheIt->second;
    // Not yet registered — create a forward-declared opaque struct so the
    // function type can be built; StructDeclExprAST::codegen will fill it in.
    return LLVMTypeCache[Key] = llvm::StructType::create(
               *TheContext, TypeIdentity::llvmName(Key));
  }

  // Unknown / future types
  return LLVMTypeCache[Key] = llvm::Type::getDoubleTy(*TheContext);
}


// Recover a TurfTypePtr from an LLVM Value type.
// Pointers are ambiguous (could be string or list), so callers that need
// precise list-vs-string info should use NamedValues directly.
TurfTypePtr getTurfTypeFromLLVM(llvm::Type *Ty) {
  if (Ty->isDoubleTy())
    return TurfDouble();
  if (Ty->isIntegerTy(64))
    return TurfInt();
  if (Ty->isIntegerTy(32))
    return TurfInt();
  if (Ty->isIntegerTy(1))
    return TurfBool();
  // Must come before the final double fallback: an i8 char value would
  // otherwise be silently misidentified as double, which breaks
  // CastExprAST's SrcName==DstName identity fast path for any char-involving
  // cast or comparison (see STRING_UFCS_OPERATOR_CHAR_RFC.md §3 build notes).
  if (Ty->isIntegerTy(8))
    return TurfChar();
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
  if (T->isNamed("char"))
    return 5;
  if (T->isNamed("string"))
    return 4;
  return 0;
}


TurfTypePtr getCommonType(TurfTypePtr A, TurfTypePtr B) {
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
         dynamic_cast<const TurfPointerType *>(T) || T->isFunc();
}



const TurfType *getExprType(const ExprAST *E) {
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
    auto *VarCallee = dynamic_cast<const VariableExprAST *>(FuncCall->getCallee());
    auto *ScopeCallee = dynamic_cast<const ScopeAccessExprAST *>(FuncCall->getCallee());
    if (VarCallee || ScopeCallee) {
      std::string CalleeName = VarCallee ? VarCallee->getName() : ScopeCallee->getRightName();

      if (CalleeName == "typeof")
        return TurfString().get();

      // MEM-001: gc_alloc/gc_alloc_zeroed/gc_realloc always evaluate to a
      // raw `void*`, exactly like typeof always evaluates to `string` above
      // -- short-circuit here for the same reason (these are bare
      // intrinsics with no ResolverFunctionSymbol/FuncReturnTypes entry, so
      // falling through to the mangled-name reconstruction below would just
      // fail to find one and return nullptr instead of the real answer).
      if (CalleeName == "gc_alloc" || CalleeName == "gc_alloc_zeroed" ||
          CalleeName == "gc_realloc") {
        static const TurfPointerType VoidPtrTy(TurfVoid());
        return &VoidPtrTy;
      }

      // FDN-001 (DEC-5): heap(expr) → &T, where T is the OPERAND's own
      // type -- data-dependent, unlike every fixed-signature bare intrinsic
      // above. Mirrors AddressOfExprAST's own caching approach further down
      // in this function for the identical reason: the returned &T must
      // stay valid for the caller's lifetime without leaking a fresh
      // TurfReferenceType per call, and without assuming ownership over a
      // pointee type this function doesn't itself own (InnerTy here is
      // already whatever a NESTED getExprType() call returned -- itself
      // either a function-local `static const`/`TurfXxx().get()` singleton
      // or a long-lived GlobalSymbolTable/ResolvedSymbol entry, per every
      // branch in this function -- so aliasing it with a no-op deleter is
      // safe: nothing here ever outlives what InnerTy already points at).
      if (CalleeName == "heap") {
        const TurfType *InnerTy = getExprType(FuncCall->getArgs()[0].get());
        if (!InnerTy)
          return nullptr;
        static std::map<std::string, TurfTypePtr> HeapRefTypeCache;
        std::string Key = InnerTy->getMangledName();
        auto It = HeapRefTypeCache.find(Key);
        if (It == HeapRefTypeCache.end()) {
          TurfTypePtr InnerCopy;
          if (InnerTy->isNamed("int"))         InnerCopy = TurfInt();
          else if (InnerTy->isNamed("double"))  InnerCopy = TurfDouble();
          else if (InnerTy->isNamed("bool"))    InnerCopy = TurfBool();
          else
            InnerCopy = TurfTypePtr(const_cast<TurfType *>(InnerTy), [](TurfType *) {});
          HeapRefTypeCache[Key] =
              std::make_shared<TurfReferenceType>(std::move(InnerCopy));
        }
        return HeapRefTypeCache[Key].get();
      }

      // MEM-002: gc_is_managed(p) always evaluates to `bool`, for the
      // identical short-circuit reason as gc_alloc's own branch above.
      // gc_collect() returns void, which needs no branch here at all --
      // getExprType() is only ever asked about a VALUE-producing
      // expression, and a bare `gc_collect();` statement is never one.
      if (CalleeName == "gc_is_managed") {
        static const BoolType BoolTy;
        return &BoolTy;
      }

      // `StructName::new(...)` / `StructName::default()` -- both evaluate to
      // the constructed struct, looked up the same way the removed
      // StructInitExprAST branch (further below) used to for the old
      // brace-literal form.
      if (ScopeCallee &&
          (ScopeCallee->ResolvedKind == ScopeAccessKind::ConstructorCall ||
           ScopeCallee->ResolvedKind == ScopeAccessKind::DefaultConstruct)) {
        if (GlobalSymbolTable) {
          if (auto T = GlobalSymbolTable->LookupType(ScopeCallee->getLeftName()))
            return T.get();
        }
        return nullptr;
      }

      // Mangled names are built from the CALL SITE's actual argument types
      // (not the function's own declared param types) so that a generic
      // function's type-erased instantiation resolves correctly below (see
      // the Unify()-based substitution further down) -- so we can't just
      // take ResolvedCallTarget->mangledName() wholesale, that reflects the
      // declared signature, not this call site's. What WAS missing here
      // entirely is the MODULE PREFIX: a module function actually mangles
      // as turf_<module>_<name>_<paramtypes>, and this manual reconstruction
      // never accounted for that at all -- for a call reached via a
      // ScopeAccessExprAST callee (Module::func(...)) or a bare self-
      // reference/on-demand-imported module call, it always missed
      // FuncReturnTypes and fell through to `return nullptr`. Invisible for
      // int-returning functions (an int compare doesn't care), but a real,
      // silent wrong-answer bug for a string-returning one: e.g.
      // `strings::trim(s) == "hello"` needs an accurate LHS type to choose
      // strcmp over a raw pointer comparison, and a null type here meant it
      // silently chose the wrong one. Recovered from ResolvedCallTarget's
      // own SourceModule (Issue #9: trust the Resolver for WHERE this
      // function lives, even though its declared types aren't usable here).
      std::string ModulePrefix;
      if (const auto *Target = FuncCall->getResolvedCallTarget())
        if (!Target->SourceModule.empty())
          ModulePrefix = Target->SourceModule + "_";

      std::string MangledName =
          (CalleeName == "main") ? CalleeName : "turf_" + ModulePrefix + CalleeName;
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
        std::string Prefix = "turf_" + ModulePrefix + CalleeName;
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
              // Same class of gap as the struct-method case just above
              // (getExprType() is a completely separate, independent
              // type-inference pass from TypeChecker/the Monomorphizer --
              // it never consults either one's results): the exact-mangled-
              // name lookups above only hit `FuncReturnTypes` when this
              // function's `getMangledName()` reconstruction of the call
              // matches how the Monomorphizer actually named the clone it
              // generated. When it doesn't -- or when nothing here has
              // registered a per-instantiation entry at all -- this falls
              // through to the generic TEMPLATE's own declared signature
              // (still registered under its bare name, e.g.
              // TurfGenericParamType("T") for `fn<T> firstElem(arr: T[]):
              // T`), which is unsubstituted. Returning it as-is meant
              // `firstElem(intArray) to string` failed with "I can't
              // convert 'T' to a string!" even though TypeChecker had
              // already correctly resolved this call's type as int.
              // Recover T -> int by unifying each DECLARED parameter type
              // against the ACTUAL argument type at this call site
              // (recursing through pointer/array wrapping, e.g. T[] vs
              // int[] -> T=int), then substitute it into the declared
              // return type.
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
    // An `export immut` constant (no ResolvedSymbol -- see its field's doc
    // comment in AST.h).
    if (Var->ResolvedConstant && Var->ResolvedConstant->Type)
      return Var->ResolvedConstant->Type.get();
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
    // LANG-001: `operator[]` -- checked before the array/pointer/string
    // paths below for the identical reason TypeChecker::visitArrayAccess()
    // checks it first: a struct base is never ALSO a real array/pointer/
    // string, so there is no ambiguity between the two.
    if (ArrAcc->ResolvedIndexGetTarget && ArrAcc->ResolvedIndexGetTarget->ReturnType) {
      const TurfType *RetTy = ArrAcc->ResolvedIndexGetTarget->ReturnType.get();
      if (GlobalSymbolTable) {
        if (const auto *NT = dynamic_cast<const NamedType *>(RetTy)) {
          if (auto Resolved = GlobalSymbolTable->LookupType(NT->getName()))
            return Resolved.get();
        }
      }
      return RetTy;
    }
    const TurfType *BaseTy = getExprType(ArrAcc->getBase());
    if (BaseTy) {
      // FDN-004: mirrors TypeChecker::visitArrayAccess()'s `char` result for
      // `s[i]` -- Codegen's getExprType() is a separate, independent
      // type-inference pass (see this function's other cross-references)
      // and must agree or callers relying on it (e.g. a further `.method()`
      // UFCS dispatch on `s[i]`) silently get the wrong answer.
      if (BaseTy->isString())
        return TurfChar().get();
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

      // OVH-003: `ResolvedCallTarget` (TypeCheckerCall.cpp's visitMethodCall(),
      // set for every successfully-resolved BOUND struct method call) already
      // names the exact candidate this specific call resolved to -- reuse it
      // directly instead of falling through to the MangledName-based lookup
      // below, whose Prefix/Suffix scan matches on `MethodName` alone
      // (`BaseTy->getName()` has already been collapsed to the shared
      // generic TEMPLATE name a few lines up, e.g. "List", never the concrete
      // per-instantiation name "List_gen1_At64"). That ambiguity is silent
      // and load-bearing-wrong the moment two DIFFERENT instantiations of
      // the same generic method coexist: confirmed live with `List<At63>`
      // and `List<At64>` (structs differing by exactly one field) both in
      // scope -- `l64.get(0)` resolved its return type via WHICHEVER
      // instantiation's entry the (unordered_map-iteration-order-dependent)
      // scan happened to hit first, silently reusing At63's struct layout
      // for an At64 value and reading `.tail` at the wrong byte offset (an
      // out-of-bounds/misinterpreted-field read -- `strlen()` on garbage,
      // SIGSEGV). `ResolvedCallTarget->ReturnType` is still the TEMPLATE's
      // own un-substituted declared type (e.g. literal `T`) -- TypeChecker
      // sets `ResolvedCallTarget` before its own substitution step runs --
      // so `ApplySubst` above (built from THIS call's own concrete
      // `GenericTypeArgs`, captured earlier from the un-collapsed BaseTy)
      // still does the real work; this just removes the ambiguous NAME
      // re-derivation neither TypeChecker's own resolution nor this call's
      // `Subst` map ever needed in the first place.
      if (const auto *RCT = MethodCall->getResolvedCallTarget()) {
        if (RCT->ReturnType)
          return ApplySubst(RCT->ReturnType.get());
      }

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
    // Resolve a GenericType base (e.g. List<Entry>) to its underlying
    // TurfStructType, mirroring the identical MethodCallExprAST handling
    // above (see its extensive doc comment for the full rationale). Without
    // this, `someList.data` (a FIELD access on a GENERIC struct instance)
    // never matched the `dynamic_cast<TurfStructType>` check just below at
    // all -- GenericType doesn't inherit from TurfStructType -- so this
    // whole branch fell through to `return nullptr` for ANY field access on
    // a generic struct, including the publicly-exposed `data`/`len`/`cap`
    // fields List<T>/Stack<T>/Queue<T> all have. Confirmed live: `someList.
    // data[0].value` (index into a `List<Entry>`, then read a field off the
    // result) crashed the WHOLE COMPILER with "Internal error: Could not
    // determine type of array/list base" in ArrayAccessExprAST::
    // codegenLValue() -- getExprType() on `someList.data` (the array being
    // indexed) silently returned null here, and that function has no
    // graceful fallback for a null base type.
    std::vector<TurfTypePtr> GenericTypeArgs;
    if (auto *GT0 = dynamic_cast<const GenericType *>(BaseTy))
      GenericTypeArgs = GT0->getTypeArgs();
    if (auto *GT = dynamic_cast<const GenericType *>(BaseTy)) {
      if (GlobalSymbolTable) {
        if (auto R = GlobalSymbolTable->LookupType(GT->getBaseName()))
          BaseTy = R.get();
      }
    }
    if (auto *STy = dynamic_cast<const TurfStructType *>(BaseTy)) {
      TurfTypePtr FieldTy = STy->getFieldType(FieldAccess->getFieldName());
      if (!FieldTy) return nullptr;

      // Substitute this struct's own generic type parameter(s) into the
      // field's declared type (e.g. T -> Entry for a List<Entry> instance,
      // turning the field's stuck-as-declared `T*` into `Entry*`). Fixing
      // the GenericType-base gap above alone isn't enough on its own: even
      // once `.data`'s declared type (`T*`) is found at all, it's still the
      // UNSUBSTITUTED generic placeholder -- indexing into it would report
      // each element's type as bare `T`, and `T` isn't a real struct, so a
      // further `.value` field access on THAT would fail too. Mirrors the
      // identical scan-the-fields Subst-recovery technique the
      // MethodCallExprAST branch above already uses for method return
      // types (GlobalSymbolTable's copy of a generic struct's fields is
      // permanently stuck as NamedType/TurfGenericParamType, never
      // rewritten per-instantiation -- see that branch's own doc comment).
      if (!GenericTypeArgs.empty()) {
        std::unordered_map<std::string, TurfTypePtr> Subst;
        std::function<void(const TurfTypePtr &)> Scan = [&](const TurfTypePtr &FTy) {
          if (!FTy || Subst.size() >= GenericTypeArgs.size()) return;
          if (auto *GP = dynamic_cast<const TurfGenericParamType *>(FTy.get())) {
            if (!Subst.count(GP->getName()))
              Subst.emplace(GP->getName(), GenericTypeArgs[Subst.size()]);
          } else if (auto *NT2 = dynamic_cast<const NamedType *>(FTy.get())) {
            bool KnownElsewhere = GlobalSymbolTable &&
                                 GlobalSymbolTable->LookupType(NT2->getName()) != nullptr;
            if (!KnownElsewhere && !Subst.count(NT2->getName()))
              Subst.emplace(NT2->getName(), GenericTypeArgs[Subst.size()]);
          } else if (auto *PT = dynamic_cast<const TurfPointerType *>(FTy.get())) {
            Scan(PT->getPointee());
          } else if (isArrayType(FTy.get())) {
            Scan(dynamic_cast<const ::ArrayType *>(FTy.get())->getElementTypePtr());
          } else if (auto *NestedGT = dynamic_cast<const GenericType *>(FTy.get())) {
            // OVH-007: a field whose OWN declared type is ANOTHER generic
            // instantiation at the same type parameter (`struct Box<T> {
            // inner: Pair<T, T> }`) was invisible to this scan -- none of
            // the branches above match a bare GenericType, so `b.inner`'s
            // own T never got recovered here, even though
            // TypeCheckerCall.cpp's visitFieldAccess/visitMethodCall
            // already recurse into this exact shape (their own "NestedGT"
            // branch, TYPE-002 S15/D4). Mirror it here.
            for (const auto &NestedArg : NestedGT->getTypeArgs())
              Scan(NestedArg);
          }
        };
        for (const auto &F : STy->getFields())
          Scan(F.Type);

        if (!Subst.empty()) {
          // Recursively substitute FieldTy through Subst, rebuilding
          // wrapper (pointer) types as needed. A freshly-constructed
          // wrapper type is cached statically, keyed by its mangled name,
          // so the raw pointer this function ultimately returns stays
          // valid for the caller's lifetime without leaking or dangling --
          // the exact same pattern AddressOfExprAST's case below already
          // uses for the same reason (a locally-constructed TurfTypePtr
          // would otherwise be destroyed the moment this function returns,
          // leaving `.get()` a dangling pointer).
          static std::map<std::string, TurfTypePtr> FieldSubstCache;
          std::function<TurfTypePtr(const TurfTypePtr &)> Apply =
              [&](const TurfTypePtr &FTy) -> TurfTypePtr {
            if (!FTy) return FTy;
            if (auto *GP = dynamic_cast<const TurfGenericParamType *>(FTy.get())) {
              auto It = Subst.find(GP->getName());
              if (It != Subst.end()) return It->second;
            } else if (auto *NT2 = dynamic_cast<const NamedType *>(FTy.get())) {
              auto It = Subst.find(NT2->getName());
              if (It != Subst.end()) return It->second;
            } else if (auto *PT = dynamic_cast<const TurfPointerType *>(FTy.get())) {
              TurfTypePtr NewPointee = Apply(PT->getPointee());
              if (NewPointee.get() != PT->getPointee().get()) {
                std::string Key = "fieldsubst_ptr_" + NewPointee->getMangledName();
                auto CacheIt = FieldSubstCache.find(Key);
                if (CacheIt != FieldSubstCache.end())
                  return CacheIt->second;
                auto NewPtr = std::make_shared<TurfPointerType>(NewPointee);
                FieldSubstCache[Key] = NewPtr;
                return NewPtr;
              }
            } else if (auto *NestedGT = dynamic_cast<const GenericType *>(FTy.get())) {
              // OVH-007: the rebuild side of the NestedGT scan above -- a
              // field declared as `Pair<T, T>` must come back as
              // `Pair<int, int>` here, not the literal, still-open
              // `Pair<T, T>` -- so codegenLValue()'s own mangled-clone
              // lookup (Monomorphize::instantiatedDeclName()) resolves to
              // the REAL registered clone instead of a name nothing ever
              // requested. Same cache-and-key idiom as the pointer branch
              // just above, since this can construct a fresh GenericType.
              std::vector<TurfTypePtr> NewArgs;
              bool Changed = false;
              for (const auto &Arg : NestedGT->getTypeArgs()) {
                TurfTypePtr NewArg = Apply(Arg);
                Changed |= (NewArg.get() != Arg.get());
                NewArgs.push_back(std::move(NewArg));
              }
              if (Changed) {
                auto NewGT = NestedGT->withTypeArgs(NewArgs);
                std::string Key = "fieldsubst_gen_" + NewGT->getMangledName();
                auto CacheIt = FieldSubstCache.find(Key);
                if (CacheIt != FieldSubstCache.end())
                  return CacheIt->second;
                FieldSubstCache[Key] = NewGT;
                return NewGT;
              }
            }
            return FTy;
          };
          FieldTy = Apply(FieldTy);
        }
      }

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
  // ScopeAccess as an enum variant: Direction::North → the type is Direction
  // (TurfEnumType). A module-function-ref ScopeAccess (io::printline) falls
  // through to nullptr below since "io" was never registered as a TYPE --
  // it has no standalone type of its own, same as visitScopeAccess() in
  // TypeChecker.cpp.
  if (auto *EA = dynamic_cast<const ScopeAccessExprAST *>(E)) {
    // module::CONST -- an `export immut` constant reference (Node.
    // ResolvedConstant, set by Resolver::visitScopeAccess).
    if (EA->ResolvedKind == ScopeAccessKind::ModuleConstantRef &&
        EA->ResolvedConstant)
      return EA->ResolvedConstant->Type.get();
    if (GlobalSymbolTable) {
      if (auto Registered = GlobalSymbolTable->LookupType(EA->getLeftName()))
        return Registered.get();
    }
  }
  return nullptr;
}


// SEM-002 (docs/planning/ARCHITECTURE_OVERHAUL.md §8, FREEZE.md §6.2 row 7):
// "Runtime, signed integer: trap (panic)" -- LLVM's llvm.s{add,sub,mul}.
// with.overflow intrinsics already compute both the wrapped result and an
// i1 overflow flag in one instruction, so this is the same
// compute-then-branch-to-panic shape the pre-existing division-by-zero
// check (BinaryExprAST::codegen(), CodegenExpr.cpp) already used -- just
// with the overflow flag coming from the intrinsic instead of a separate
// icmp. Shared (not inlined at each call site) because THREE independent
// codegen paths need identical arithmetic-overflow behavior and don't
// share a common codegen() call: BinaryExprAST (CodegenExpr.cpp),
// CompoundAssignmentExprAST, and PostfixExprAST (both CodegenVar.cpp) --
// compound assignment and `x++`/`x--` deliberately do NOT desugar to a
// literal `x = x + 1`-shaped re-walk of BinaryExprAST at the Codegen layer
// (see CodegenVar.cpp's own top-of-file framing), so each already
// duplicates the divide-by-zero check independently; this duplicates the
// same way rather than inventing new shared plumbing beyond this function.
Value *EmitCheckedIntArith(char Op, Value *L, Value *R, SourceLocation Loc) {
  llvm::Intrinsic::ID ID;
  const char *Name;
  switch (Op) {
  case '+': ID = llvm::Intrinsic::sadd_with_overflow; Name = "addtmp"; break;
  case '-': ID = llvm::Intrinsic::ssub_with_overflow; Name = "subtmp"; break;
  case '*': ID = llvm::Intrinsic::smul_with_overflow; Name = "multmp"; break;
  default:
    llvm::report_fatal_error(
        "Internal error: EmitCheckedIntArith called with non-arithmetic operator");
    return nullptr;
  }
#if LLVM_VERSION_MAJOR >= 19
  Function *IntrinsicF = llvm::Intrinsic::getOrInsertDeclaration(
      TheModule.get(), ID, {Type::getInt64Ty(*TheContext)});
#else
  Function *IntrinsicF = llvm::Intrinsic::getDeclaration(
      TheModule.get(), ID, {Type::getInt64Ty(*TheContext)});
#endif
  Value *CallRes = Builder->CreateCall(IntrinsicF, {L, R}, Name);
  Value *Result = Builder->CreateExtractValue(CallRes, 0, std::string(Name) + ".val");
  Value *Overflowed = Builder->CreateExtractValue(CallRes, 1, std::string(Name) + ".ov");

  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  BasicBlock *OverflowBB = BasicBlock::Create(*TheContext, "int.overflow", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "int.overflow.cont", TheFunction);
  Builder->CreateCondBr(Overflowed, OverflowBB, ContBB);

  Builder->SetInsertPoint(OverflowBB);
  CodegenPanic(Builder->CreateGlobalString("Integer overflow"), Loc);
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateUnreachable();

  Builder->SetInsertPoint(ContBB);
  return Result;
}

// Negation overflows for exactly one input: negating INT64_MIN, whose
// magnitude (2^63) has no positive int64 representation -- the same
// "one remaining case" shape as EmitSignedDivOverflowGuard() below, just
// for unary '-' instead of '/'.
Value *EmitCheckedIntNegate(Value *V, SourceLocation Loc) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  Value *IsIntMin = Builder->CreateICmpEQ(
      V, ConstantInt::get(Type::getInt64Ty(*TheContext),
                          static_cast<uint64_t>(INT64_MIN), true),
      "isintmin");

  BasicBlock *OverflowBB = BasicBlock::Create(*TheContext, "neg.overflow", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "neg.cont", TheFunction);
  Builder->CreateCondBr(IsIntMin, OverflowBB, ContBB);

  Builder->SetInsertPoint(OverflowBB);
  CodegenPanic(Builder->CreateGlobalString("Integer overflow"), Loc);
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateUnreachable();

  Builder->SetInsertPoint(ContBB);
  return Builder->CreateNeg(V, "negtmp");
}

// '<<' has no LLVM "with.overflow" intrinsic (unlike +/-/*), so overflow is
// defined directly: the shift amount must be in [0, 64), AND shifting the
// result back right by the same amount (arithmetic, sign-preserving) must
// recover the original value -- if any set bit was pushed out past the
// sign bit, the recovered value differs from the input. The shift-amount
// range is checked FIRST, in its own branch, so the actual shl/ashr below
// never executes with an out-of-range amount at all (LLVM's shl/ashr are
// poison for a shift amount >= the operand's bit width, so computing them
// speculatively and only checking afterward would itself be the exact kind
// of UB this group exists to remove).
Value *EmitCheckedShl(Value *L, Value *R, SourceLocation Loc) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  Value *Zero64 = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
  Value *SixtyFour = ConstantInt::get(Type::getInt64Ty(*TheContext), 64);
  Value *AmtBad = Builder->CreateOr(
      Builder->CreateICmpSLT(R, Zero64, "shiftamt.neg"),
      Builder->CreateICmpSGE(R, SixtyFour, "shiftamt.toobig"), "shiftamt.bad");

  BasicBlock *OverflowBB = BasicBlock::Create(*TheContext, "shl.overflow", TheFunction);
  BasicBlock *ShiftOkBB = BasicBlock::Create(*TheContext, "shl.amtok", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "shl.cont", TheFunction);
  Builder->CreateCondBr(AmtBad, OverflowBB, ShiftOkBB);

  Builder->SetInsertPoint(ShiftOkBB);
  Value *ShlResult = Builder->CreateShl(L, R, "lshifttmp");
  Value *Recovered = Builder->CreateAShr(ShlResult, R, "lshiftcheck");
  Value *LostBits = Builder->CreateICmpNE(Recovered, L, "lshift.lost");
  Builder->CreateCondBr(LostBits, OverflowBB, ContBB);

  Builder->SetInsertPoint(OverflowBB);
  CodegenPanic(Builder->CreateGlobalString("Integer overflow"), Loc);
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateUnreachable();

  // ShlResult dominates ContBB: the only edge into ContBB is from ShiftOkBB,
  // after ShlResult was already computed -- no PHI needed.
  Builder->SetInsertPoint(ContBB);
  return ShlResult;
}

// The one signed-division overflow case (ARCHITECTURE_OVERHAUL.md §10.2):
// INT64_MIN / -1 has no representable int64 result (the mathematical
// result, 2^63, overflows by exactly one past INT64_MAX). LLVM's `sdiv` is
// poison for this input (mirrors hardware `idiv`, which traps on it) --
// must be caught explicitly, same shape as the pre-existing divide-by-zero
// check this always runs alongside. Call this AFTER the zero check (so the
// insert point is already the "R != 0" continuation block) and BEFORE
// emitting the actual CreateSDiv/CreateSRem.
void EmitSignedDivOverflowGuard(Value *L, Value *R, SourceLocation Loc) {
  Function *TheFunction = Builder->GetInsertBlock()->getParent();
  Value *IsIntMin = Builder->CreateICmpEQ(
      L, ConstantInt::get(Type::getInt64Ty(*TheContext),
                          static_cast<uint64_t>(INT64_MIN), true),
      "isintmin");
  Value *IsNegOne = Builder->CreateICmpEQ(
      R, ConstantInt::get(Type::getInt64Ty(*TheContext),
                          static_cast<uint64_t>(-1), true),
      "isnegone");
  Value *WillOverflow = Builder->CreateAnd(IsIntMin, IsNegOne, "sdiv.overflow.check");

  BasicBlock *OverflowBB = BasicBlock::Create(*TheContext, "sdiv.overflow", TheFunction);
  BasicBlock *ContBB = BasicBlock::Create(*TheContext, "sdiv.overflow.cont", TheFunction);
  Builder->CreateCondBr(WillOverflow, OverflowBB, ContBB);

  Builder->SetInsertPoint(OverflowBB);
  CodegenPanic(Builder->CreateGlobalString("Integer overflow"), Loc);
  if (!Builder->GetInsertBlock()->getTerminator())
    Builder->CreateUnreachable();

  Builder->SetInsertPoint(ContBB);
}


Value *CastToType(Value *Val, TurfTypePtr DestType,
                         const std::string &Name, SourceLocation Loc) {
  return CastToType(Val, DestType.get(), Name, Loc);
}


Value *CastToType(Value *Val, const TurfType *DestType,
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

  // TYPE-002 (S14a): erasure is gone -- INV-GEN-1: DestType can no longer
  // legitimately be a TurfGenericParamType here (every occurrence either
  // lives inside a generic template Codegen never reaches, or was already
  // substituted to a concrete type by the monomorphizer before this clone's
  // body was codegenned). The old "cast TO generic parameter" branch
  // (IntToPtr/bitcast into a void* slot) and the numeric half of "cast FROM
  // generic parameter" (PtrToInt/bitcast back to int/bool/double) are both
  // deleted outright, not left dead -- see the change record's *Removed
  // assumptions* for the completion-criterion verification this rested on.
  assert(!dynamic_cast<const TurfGenericParamType *>(DestType) &&
        "INV-GEN-1 violated: CastToType() received a TurfGenericParamType "
        "DestType -- an un-monomorphized generic template reached Codegen. "
        "This is a compiler bug.");

  if (ValTy->isPointerTy()) {
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
      // TYPE-002 (S13b): a GenericType destination (e.g. `fn Holder.get():
      // List<Big> { return self.items; }`, self's own field returned by
      // value) resolves to its OWN per-instantiation TurfStructType, not
      // the shared base -- mirrors FieldAccessExprAST's identical fix
      // (CodegenStruct.cpp), needed so getLLVMType(ResolvedDest) below
      // loads the CORRECT (possibly >8-byte) shape, not the erasure-era
      // shared one.
      if (const auto *GT = dynamic_cast<const GenericType *>(DestType)) {
        if (GlobalSymbolTable) {
          std::string MangledName =
              Monomorphize::instantiatedDeclName(GT->getBaseName(), GT->getTypeArgs());
          if (auto R = GlobalSymbolTable->LookupType(MangledName))
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

  // Struct-to-struct: if DestType is a struct (NamedType, TurfStructType, or
  // -- TYPE-002 S13b -- a GenericType instantiation, e.g. a >8-byte generic
  // struct value now genuinely arriving here as a real ValTy->isStructTy()
  // aggregate instead of an erased pointer) and the LLVM types already
  // match (e.g. inner struct init), pass through without casting. Without
  // this, CastToType would hit the string-error guard below.
  if (ValTy->isStructTy()) {
    if (dynamic_cast<const TurfStructType *>(DestType) ||
        dynamic_cast<const NamedType *>(DestType) ||
        dynamic_cast<const GenericType *>(DestType))
      return Val;
  }

  // Strings cannot be cast to/from non-pointer types implicitly
  if (SrcType->isNamed("string") || DestType->isNamed("string")) {
    TypeError(CurLoc, ErrCode::InvalidCast, "Cannot cast between string and non-pointer types. SrcType: " + SrcType->getName() + ", DestType: " + DestType->getName() + ", Context Name: " + Name)
        .raise();
    return Val;
  }

  // int32 as a DestType: note SrcType can never legitimately be "int32"
  // here -- CastToType()'s very first step (top of this function)
  // unconditionally sign-extends ANY genuinely-i32-typed value to i64
  // before SrcType is even derived, so by this point an int32-origin value
  // has already become a plain "int". This branch only exists for casting
  // an ordinary int/double/bool value DOWN INTO int32 (e.g. two int32
  // operands in BinaryExprAST::codegen()'s CastBoth(), where NumericType
  // ends up being int32 itself and both operands -- already force-widened
  // to i64 by the step above -- need to come back down to i32).
  if (DestType->isNamed("int32")) {
    if (SrcType->isNamed("int"))
      return Builder->CreateTrunc(Val, Type::getInt32Ty(*TheContext), Name);
    if (SrcType->isNamed("double"))
      return Builder->CreateFPToSI(Val, Type::getInt32Ty(*TheContext), Name);
    if (SrcType->isNamed("bool"))
      return Builder->CreateZExt(Val, Type::getInt32Ty(*TheContext), Name);
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
    // FDN-004: a `char`-typed value used where Codegen needs a plain `int`
    // (e.g. ArrayAccessExprAST forcing its own index to `int` before the
    // GEP, regardless of what the TypeChecker inferred the index
    // expression's type to be) -- zero-extends, matching `char -> int`'s
    // documented semantics (docs/reference/variables_and_types.md
    // "char") and CastExprAST::codegen()'s identical explicit-`to` case.
    // Before FDN-004 no expression could ever produce a `char`-typed
    // array/string index at all, so this path was unreachable; it is now
    // hit by anything as ordinary as `freq[s[i]]` (a byte-histogram over a
    // string). Confirmed live: without this, the index silently stayed an
    // un-widened i8 all the way into the bounds check and the GEP, and
    // `-Wverify` on LLVM caught it outright ("Both operands to ICmp
    // instruction are not of the same type! ... icmp slt i8 ..., i64 0").
    if (SrcType->isNamed("char"))
      return Builder->CreateZExt(Val, Type::getInt64Ty(*TheContext), Name);
  }
  // FDN-004: the narrowing mirror of the widening case just above -- Codegen
  // occasionally needs to force a value down to a struct/array field's
  // declared `char` element type (e.g. ArrayLiteralExprAST/ArrayDeclExprAST
  // casting each initializer element to ElementType). Truncates, matching
  // `int -> char`'s documented semantics.
  if (DestType->isNamed("char")) {
    if (SrcType->isNamed("int"))
      return Builder->CreateTrunc(Val, Type::getInt8Ty(*TheContext), Name);
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


// This creates an alloca instruction in the entry block of a function
AllocaInst *CreateEntryBlockAlloca(Function *TheFunction,
                                   const std::string &VarName,
                                   const TurfType *Type) {
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                   TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(getLLVMType(Type), 0, VarName.c_str());
}


llvm::Constant *GetDefaultValue(const TurfType *T) {
  if (!T)
    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*TheContext), 0.0);

  if (T->isNamed("int"))
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*TheContext), 0);
  if (T->isNamed("int32"))
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*TheContext), 0);
  if (T->isNamed("double"))
    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*TheContext), 0.0);
  if (T->isNamed("bool"))
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*TheContext), 0);
  if (T->isNamed("char"))
    return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*TheContext), 0);
  if (T->isNamed("string"))
    return Builder->CreateGlobalStringPtr("");

  if (isArrayType(T) || dynamic_cast<const TurfPointerType *>(T) ||
      dynamic_cast<const TurfGenericParamType *>(T) || T->isFunc()) {
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


// Emit the literal value of an `export immut` constant directly at a
// reference site -- exported constants have no LLVM storage of their own
// (see ResolverConstantSymbol's doc comment in ResolverSymbolTable.h), so
// every use (bare or module-qualified) inlines this instead of loading from
// an alloca/global. Mirrors NumberExprAST/BoolExprAST/StringExprAST's own
// codegen() bodies exactly -- this is just those three dispatched by the
// constant's stored Type instead of by AST node class.
Value *CodegenConstantValue(const ResolverConstantSymbol &C) {
  if (C.Type && C.Type->isDouble())
    return ConstantFP::get(*TheContext, APFloat(C.DoubleVal));
  if (C.Type && C.Type->isBool())
    return ConstantInt::get(Type::getInt1Ty(*TheContext), C.BoolVal ? 1 : 0);
  if (C.Type && C.Type->isString())
    return Builder->CreateGlobalString(C.StringVal, "str");
  // C.IntVal is C.'s exact decimal string (SEM-001) -- see
  // ResolverConstantSymbol::IntVal's own doc comment (ResolverSymbolBase.h)
  // for why it's a string rather than a plain llvm::APSInt at the Resolver
  // layer. Same getBitsNeeded()-then-extOrTrunc(64) pattern
  // NumberExprAST::codegen() (CodegenExpr.cpp) and ParseNumberExpr
  // (ParserLiteral.cpp) both already use to turn an exact, possibly-wider-
  // than-64-bit value into the fixed 64-bit `int` LLVM constant this
  // returns.
  StringRef Digits = C.IntVal;
  bool Negative = Digits.starts_with("-");
  StringRef Magnitude = Negative ? Digits.drop_front() : Digits;
  unsigned Bits = std::max(64u, APInt::getBitsNeeded(Magnitude, 10) + 1);
  APInt Value(Bits, Magnitude, 10);
  if (Negative)
    Value = -Value;
  return ConstantInt::get(*TheContext, Value.sextOrTrunc(64));
}


// ─── TOOL-001: `-g` debug info helpers ───────────────────────────────────────
// (docs/planning/ARCHITECTURE_OVERHAUL.md §8 TOOL-001, docs/architecture/
// compiler_pipeline.md). See CodegenInternal.h for each function's own doc
// comment -- kept there (not here) since that's the header every other
// Codegen*.cpp file actually reads to learn these exist.

static std::map<std::string, DIFile *> DebugFileCache;

DIFile *getOrCreateDebugFile(const std::string &Filename) {
  if (!DBuilder)
    return nullptr;
  if (Filename.empty())
    return DebugCU ? DebugCU->getFile() : nullptr;
  auto It = DebugFileCache.find(Filename);
  if (It != DebugFileCache.end())
    return It->second;
  SmallString<256> AbsPath(Filename);
  (void)sys::fs::make_absolute(AbsPath);
  StringRef Dir = sys::path::parent_path(AbsPath);
  StringRef Base = sys::path::filename(AbsPath);
  DIFile *F = DBuilder->createFile(Base, Dir);
  return DebugFileCache[Filename] = F;
}


static std::map<std::string, DIType *> DebugTypeCache;

DIType *getDebugType(const TurfType *T) {
  if (!DBuilder)
    return nullptr;
  if (!T)
    return DBuilder->createUnspecifiedType("void");

  std::string Key = T->getMangledName();
  auto CacheIt = DebugTypeCache.find(Key);
  if (CacheIt != DebugTypeCache.end())
    return CacheIt->second;

  // Primitives -- sizes/encodings match getLLVMType()'s own primitive
  // branches exactly (int -> i64, int32 -> i32, double -> double, bool ->
  // i1, char -> i8) so a debugger's "sizeof this local" matches the real
  // storage getLLVMType() actually allocated for it.
  if (T->isNamed("int"))
    return DebugTypeCache[Key] =
              DBuilder->createBasicType("int", 64, dwarf::DW_ATE_signed);
  if (T->isNamed("int32"))
    return DebugTypeCache[Key] =
              DBuilder->createBasicType("int32", 32, dwarf::DW_ATE_signed);
  if (T->isNamed("double"))
    return DebugTypeCache[Key] =
              DBuilder->createBasicType("double", 64, dwarf::DW_ATE_float);
  if (T->isNamed("bool"))
    // LLVM stores `bool` as i1, but DWARF has no 1-bit integer -- 8 bits
    // (the smallest DWARF/gdb actually prints sanely) is what every C
    // frontend does for `_Bool`/`bool` too.
    return DebugTypeCache[Key] =
              DBuilder->createBasicType("bool", 8, dwarf::DW_ATE_boolean);
  if (T->isNamed("char"))
    return DebugTypeCache[Key] = DBuilder->createBasicType(
              "char", 8, dwarf::DW_ATE_unsigned_char);
  if (T->isNamed("void"))
    return DebugTypeCache[Key] = nullptr; // DWARF: no DIType at all means void

  // A struct/enum referenced BY NAME (a local's declared `Point`, a
  // `List<int>` instantiation) is only a NamedType/GenericType here; the
  // layout lives on the registered TurfStructType/TurfEnumType. Without this
  // every such local fell through to the "unspecified type" catch-all at the
  // bottom -- visible by name in a debugger, but `print p` showed nothing
  // (the TOOL-001 gap FDN-003 recorded). A generic instantiation's struct is
  // registered under its monomorphized name, the same lookup getLLVMType()
  // does.
  if (GlobalSymbolTable) {
    TurfTypePtr Registered;
    if (const auto *NT = dynamic_cast<const NamedType *>(T))
      Registered = GlobalSymbolTable->LookupType(NT->getName());
    else if (const auto *GT = dynamic_cast<const GenericType *>(T))
      Registered = GlobalSymbolTable->LookupType(
          Monomorphize::instantiatedDeclName(GT->getBaseName(), GT->getTypeArgs()));
    if (Registered && Registered.get() != T &&
        (dynamic_cast<const TurfStructType *>(Registered.get()) ||
         dynamic_cast<const TurfEnumType *>(Registered.get())))
      return DebugTypeCache[Key] = getDebugType(Registered.get());
  }

  // `string` (FREEZE.md §6.8: "stays a NUL-terminated byte pointer") --
  // pointer to char, exactly what it actually is at the LLVM level.
  if (T->isNamed("string")) {
    DIType *CharTy =
        DBuilder->createBasicType("char", 8, dwarf::DW_ATE_unsigned_char);
    return DebugTypeCache[Key] =
              DBuilder->createPointerType(CharTy, 64, 64, std::nullopt, "string");
  }

  // Fixed-size arrays (`T[N]`) -- getLLVMType() only gives a real
  // llvm::ArrayType for a KNOWN positive size; an unsized `T[]` parameter
  // is already an opaque pointer at the LLVM level (§27 D3: no escape
  // analysis exists for it yet), so its debug type is just "pointer to T",
  // same as any other opaque-pointer-shaped Turf type below.
  if (isArrayType(T)) {
    const auto *AT = dynamic_cast<const ::ArrayType *>(T);
    if (AT && AT->getSize() > 0) {
      DIType *ElemTy = getDebugType(AT->getElementTypePtr().get());
      llvm::Type *ElemLLVMTy = getLLVMType(AT->getElementTypePtr().get());
      uint64_t ElemBits =
          TheModule->getDataLayout().getTypeSizeInBits(ElemLLVMTy).getFixedValue();
      llvm::Metadata *Subscript =
          DBuilder->getOrCreateSubrange(0, AT->getSize());
      DINodeArray Subscripts =
          DBuilder->getOrCreateArray({Subscript});
      return DebugTypeCache[Key] = DBuilder->createArrayType(
                ElemBits * AT->getSize(), 0, ElemTy, Subscripts);
    }
  }

  // Pointer-shaped types (`T*`, `&T`, `&?T`, `ptr`, an unsized `T[]`) --
  // all share the same opaque-pointer LLVM representation (getLLVMType()'s
  // own comment on this), so they share the same debug-type shape too: a
  // real DIDerivedType pointer to the pointee's own debug type when the
  // pointee is known, or an unspecified "ptr" type when it isn't (`ptr`
  // itself has no pointee at all).
  if (const auto *PT = dynamic_cast<const TurfPointerType *>(T)) {
    DIType *PointeeTy = getDebugType(PT->getPointee().get());
    return DebugTypeCache[Key] = DBuilder->createPointerType(PointeeTy, 64);
  }
  if (T->isNamed("ptr") || dynamic_cast<const OpaquePointerType *>(T))
    return DebugTypeCache[Key] = DBuilder->createUnspecifiedType("ptr");

  // Enums -- always stored as their underlying integer type (getLLVMType()'s
  // own comment), so their debug type is a real DW_TAG_enumeration_type
  // over that underlying type rather than a plain typedef -- lets a
  // debugger print a variant NAME instead of a bare ordinal.
  if (const auto *ET = dynamic_cast<const TurfEnumType *>(T)) {
    DIType *UnderlyingTy = getDebugType(ET->getUnderlyingType());
    std::vector<Metadata *> Enumerators;
    for (const auto &Variant : ET->getVariants())
      Enumerators.push_back(
          DBuilder->createEnumerator(Variant, ET->getVariantValue(Variant)));
    DINodeArray Elements = DBuilder->getOrCreateArray(Enumerators);
    DIFile *File = DebugCU ? DebugCU->getFile() : nullptr;
    return DebugTypeCache[Key] = DBuilder->createEnumerationType(
              DebugCU, TypeIdentity::llvmName(ET->getName()), File, 0, 64, 0,
              Elements, UnderlyingTy);
  }

  // Structs -- built lazily here (not at struct-declaration Codegen time)
  // from the already-registered TurfStructType + the already-`setBody()`'d
  // llvm::StructType's real DataLayout offsets, deliberately mirroring
  // getLLVMType()'s own "register the stub in the cache BEFORE resolving
  // fields" idiom (CodegenStruct.cpp's own long comment on issue #176) to
  // break a pointer-mediated struct cycle (`struct Node { next: Node* }`)
  // the exact same way: a cycle can only be reached through a pointer field
  // now (INV-LAYOUT-1/TYPE-001 already rejects a genuine by-value cycle
  // before Codegen ever runs), and a pointer field only needs ITS pointee's
  // debug type once this composite's stub is already sitting in
  // DebugTypeCache -- so the recursive getDebugType(Pointee) call a few
  // lines above (the TurfPointerType branch) finds the stub instead of
  // recursing back into this same struct forever.
  if (const auto *ST = dynamic_cast<const TurfStructType *>(T)) {
    llvm::Type *LLVMTy = getLLVMType(T);
    auto *LLVMStructTy = dyn_cast<llvm::StructType>(LLVMTy);
    DIFile *File = DebugCU ? DebugCU->getFile() : nullptr;
    // FDN-003: `M.Name` for a module-owned type, the same spelling as its
    // LLVM struct and GC descriptor (TypeIdentity::llvmName()).
    const std::string DebugName = TypeIdentity::llvmName(ST->getName());
    DICompositeType *Stub = DBuilder->createReplaceableCompositeType(
        dwarf::DW_TAG_structure_type, DebugName, DebugCU, File, 0);
    DebugTypeCache[Key] = Stub;

    if (!LLVMStructTy || LLVMStructTy->isOpaque()) {
      // Struct decl hasn't codegenned yet (a forward reference Codegen
      // itself hasn't resolved either) -- leave the forward declaration in
      // place. main.cpp's DBuilder->finalize() call (before verifyModule())
      // resolves it if it's still a dangling temporary by the time the
      // whole module is done; a debugger sees an incomplete-type
      // declaration for this struct in that case, same as it would for a
      // genuinely-opaque C struct.
      return Stub;
    }

    const llvm::StructLayout *Layout =
        TheModule->getDataLayout().getStructLayout(LLVMStructTy);
    std::vector<Metadata *> Members;
    const auto &Fields = ST->getFields();
    for (unsigned i = 0; i < Fields.size() && i < LLVMStructTy->getNumElements();
        ++i) {
      DIType *FieldTy = getDebugType(Fields[i].Type.get());
      llvm::Type *FieldLLVMTy = LLVMStructTy->getElementType(i);
      uint64_t FieldBits =
          TheModule->getDataLayout().getTypeSizeInBits(FieldLLVMTy).getFixedValue();
      uint64_t OffsetBits = Layout->getElementOffsetInBits(i).getFixedValue();
      Members.push_back(DBuilder->createMemberType(
          DebugCU, Fields[i].Name, File, 0, FieldBits, 0, OffsetBits,
          DINode::FlagZero, FieldTy));
    }
    DINodeArray Elements = DBuilder->getOrCreateArray(Members);
    uint64_t TotalBits = Layout->getSizeInBits().getFixedValue();
    DICompositeType *Full = DBuilder->createStructType(
        DebugCU, DebugName, File, 0, TotalBits, 0, DINode::FlagZero,
        nullptr, Elements);
    DBuilder->replaceTemporary(llvm::TempDICompositeType(Stub), Full);
    return DebugTypeCache[Key] = Full;
  }

  // Everything else (an uninstantiated/erased generic parameter, a
  // first-class function value, an `error`/`!T` value, ...) -- all
  // pointer- or i64-erased at the LLVM level with no single "real" Turf
  // type a debugger could usefully name; TOOL-001's own completion
  // criteria (a breakpoint, a backtrace, printing an ordinary local) don't
  // need more than "some plausibly-named thing" for these.
  return DebugTypeCache[Key] =
             DBuilder->createUnspecifiedType(TypeIdentity::llvmName(T->getName()));
}


SourceLocation GetNodeDebugLoc(const ExprAST *E) {
  if (!E)
    return SourceLocation{};

#define TRY_LOC(NodeType)                                                    \
  if (auto *N = dynamic_cast<const NodeType *>(E))                          \
    return N->getLoc();

  TRY_LOC(NumberExprAST)
  TRY_LOC(BinaryExprAST)
  TRY_LOC(AssignmentExprAST)
  TRY_LOC(CompoundAssignmentExprAST)
  TRY_LOC(PostfixExprAST)
  TRY_LOC(VariableExprAST)
  TRY_LOC(UnaryExprAST)
  TRY_LOC(AddressOfExprAST)
  TRY_LOC(SizeOfExprAST)
  TRY_LOC(CompareExprAST)
  TRY_LOC(NullExprAST)
  TRY_LOC(CastExprAST)
  TRY_LOC(FuncCallExprAST)
  TRY_LOC(ScopeAccessExprAST)
  TRY_LOC(FieldAccessExprAST)
  TRY_LOC(ArrayLiteralExprAST)
  TRY_LOC(ArrayAccessExprAST)
  TRY_LOC(MethodCallExprAST)
  TRY_LOC(IfExprAST)
  TRY_LOC(UnsafeBlockExprAST)
  TRY_LOC(WhileExprAST)
  TRY_LOC(DoWhileExprAST)
  TRY_LOC(ForExprAST)
  TRY_LOC(ForEachExprAST)
  TRY_LOC(BreakExprAST)
  TRY_LOC(ContinueExprAST)
  TRY_LOC(ReturnExprAST)
  TRY_LOC(RecoverExprAST)
  TRY_LOC(TryExprAST)
  TRY_LOC(ModuleImportExprAST)
  TRY_LOC(VarDeclExprAST)
  TRY_LOC(FuncDefExprAST)
  TRY_LOC(ExternFuncDeclExprAST)
  TRY_LOC(EnumDeclExprAST)
  TRY_LOC(MachineDeclExprAST)
  TRY_LOC(ErrorDeclExprAST)
  TRY_LOC(ArrayDeclExprAST)
  TRY_LOC(StructDeclExprAST)

#undef TRY_LOC

  // BlockExprAST (a container, no location of its own), BoolExprAST,
  // CharExprAST, StringExprAST (bare literals -- never a useful breakpoint
  // target on their own; they're always reached as part of a larger
  // statement whose OWN location is already set by the time these codegen)
  // and anything not listed above all fall through here.
  return SourceLocation{};
}


void SetDebugLoc(const SourceLocation &Loc) {
  if (!g_EmitDebugInfo || !CurrentDebugScope || Loc.Line <= 0)
    return;
  Builder->SetCurrentDebugLocation(
      DILocation::get(*TheContext, Loc.Line, Loc.Col > 0 ? Loc.Col : 1,
                      CurrentDebugScope));
}
