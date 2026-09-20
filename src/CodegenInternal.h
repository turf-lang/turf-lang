#ifndef CODEGEN_INTERNAL_H
#define CODEGEN_INTERNAL_H

// Private, implementation-only forward declarations shared across the 7
// CodegenCore/Expr/Var/ControlFlow/Func/Array/Struct.cpp files (the split of
// the original monolithic Codegen.cpp -- see
// docs/architecture/compiler_pipeline.md). NOT part of the compiler's
// public interface (that's Codegen.h) -- these are free functions and
// globals that used to be `static`/file-scope in the monolith but are used
// from more than one of the new files, so they need real external linkage
// now. Mirrors ParserInternal.h/ResolverInternal.h/TypeCheckerInternal.h's
// role for the earlier split passes.
//
// Every `NodeType::codegen()`/`codegenLValue()` method itself needs NO entry
// here -- each is already declared as part of its AST node class in AST.h,
// so it's visible everywhere via `#include "AST.h"` regardless of which of
// the 7 new files actually defines it.

#include "AST.h"     // ExprAST
#include "Codegen.h" // TheContext/Builder/TheModule/CurrentFunction/... + getLLVMType/CreateEntryBlockAlloca (already public, listed here only for reference)
#include "Errors.h"  // SourceLocation
#include "ResolverSymbolTable.h" // ResolverConstantSymbol
#include "Types.h"

#include "llvm/IR/Value.h"

#include <map>
#include <string>
#include <vector>

// ── Shared global state ──────────────────────────────────────────────────
// Defined in CodegenCore.cpp; FuncDefExprAST/ExternFuncDeclExprAST/
// FuncCallExprAST (CodegenFunc.cpp) populate/query them, and
// getExprType()/CastToType() (CodegenCore.cpp itself) also consult them for
// a call expression's inferred return type.

/// Tracks full parameter types for every user-defined function (keyed by
/// mangled name), so a later call site can type-check its arguments.
extern std::map<std::string, std::vector<TurfTypePtr>> FuncParamTypes;
/// Tracks the declared return type for every user-defined function.
extern std::map<std::string, TurfTypePtr> FuncReturnTypes;

/// Type cache for Monomorphization: maps a mangled type name to the LLVM
/// type already generated for it. CodegenStruct.cpp (StructDeclExprAST)
/// pre-populates an entry directly (not just through getLLVMType()) before
/// resolving field types, so a self-referential struct's own field types
/// see it already registered.
extern std::map<std::string, llvm::Type *> LLVMTypeCache;

// ── Shared helpers ────────────────────────────────────────────────────────
// All defined in CodegenCore.cpp.

/// Unboxes a boxed i64-slot value (used by the `!T` fallible-value
/// representation) back to its real, unboxed LLVM value of type @p T.
/// Needed by CodegenFunc.cpp (a fallible function's return path).
llvm::Value *UnboxFromI64Slot(llvm::Value *Slot, const TurfType *T);

/// Builds the LLVM constant tag identifying a specific declared error
/// variant (used by the `!T` fallible-value representation).
/// Needed by CodegenStruct.cpp (ScopeAccessExprAST's error-variant case)
/// and (transitively, via GetErrorTagValue()) CodegenExpr.cpp.
llvm::Value *BuildErrorTag(const std::string &ErrorTypeName,
                           const std::string &VariantName);

/// Declares (once) or looks up the C `strcmp` used to compare two error tag
/// strings. Needed by CodegenExpr.cpp (BinaryExprAST's `==`/`!=` on an
/// error-tag operand).
llvm::Function *getOrCreateStrcmpFn();

/// True if @p E is an error-tag-shaped operand for `==`/`!=`. Needed by
/// CodegenExpr.cpp (BinaryExprAST).
bool IsErrorTagExpr(const ExprAST *E);

/// Returns the tag string (i8*) for an error-tag-shaped `==`/`!=` operand.
/// Needed by CodegenExpr.cpp (BinaryExprAST).
llvm::Value *GetErrorTagValue(const ExprAST *E);

/// True if @p E is exactly the `ErrorType::Variant` reference shape. Needed
/// by CodegenControlFlow.cpp (ReturnExprAST, to decide whether a returned
/// value is an error tag or a genuine success value).
bool IsErrorConstructionExpr(const ExprAST *E);

/// Wraps a raw success value (or, if IsError, an error payload i8*) into a
/// `!T` FallibleValue for a `return` inside a `!T`-declared function.
/// Needed by CodegenControlFlow.cpp (ReturnExprAST).
llvm::Value *WrapFallibleValue(llvm::Value *RawVal, bool IsError);

/// Best-effort reconstruction of a TurfType from a raw LLVM type -- used
/// wherever the AST-level type isn't directly available and Codegen has to
/// infer one from what LLVM already generated. Needed across nearly every
/// category (ControlFlow/Expr/Func/Var), not just Core.
TurfTypePtr getTurfTypeFromLLVM(llvm::Type *Ty);

/// The common type two operand types promote to for a binary operation
/// (e.g. int + double -> double). Needed by CodegenControlFlow.cpp (loop
/// bound normalization) and CodegenExpr.cpp (BinaryExprAST).
TurfTypePtr getCommonType(TurfTypePtr A, TurfTypePtr B);

/// Best-effort static type of an arbitrary expression node, consulting
/// NamedValues/FuncReturnTypes/the AST's own resolved-symbol fields as
/// needed. The single most widely used helper in Codegen -- needed by
/// every one of the 6 non-Core categories.
const TurfType *getExprType(const ExprAST *E);

/// Casts @p Val to @p DestType, inserting whatever LLVM conversion
/// instruction is needed (sext/trunc/fp-to-int/bitcast/...). The single
/// most widely used helper in Codegen after getExprType() -- needed by
/// every one of the 6 non-Core categories.
llvm::Value *CastToType(llvm::Value *Val, const TurfType *DestType,
                        const std::string &Name, SourceLocation Loc = {0, 0});
llvm::Value *CastToType(llvm::Value *Val, TurfTypePtr DestType,
                        const std::string &Name, SourceLocation Loc = {0, 0});

/// The default (zero) value for a given type -- 0 for int, 0.0 for double,
/// false for bool, a null pointer for pointer types, a recursively
/// zero-initialized constant struct for struct types, etc. Needed by
/// CodegenFunc.cpp (`T::default()`'s DefaultConstruct branch, and implicit
/// struct field defaults) and CodegenVar.cpp (VarDeclExprAST with no
/// initializer -- storage is still always zero-filled here regardless of
/// type, per F-INIT-2/FREEZE.md §6.4, even though SEM-004 removed the
/// separate DefaultInitExprAST node this used to also flow through for a
/// no-initializer struct/char/int32/array/generic declaration).
llvm::Constant *GetDefaultValue(const TurfType *T);

/// Emits the LLVM constant for an already-resolved `export immut` value.
/// Needed by CodegenStruct.cpp (ScopeAccessExprAST's `Module::CONST` case)
/// and CodegenVar.cpp (VariableExprAST's self-reference-to-a-constant case).
llvm::Value *CodegenConstantValue(const ResolverConstantSymbol &C);

/// Clears main's argv/argc plumbing globals (CodegenFunc.cpp's
/// ArgcStorageGV/ArgvStorageGV) so a fresh InitializeModule() doesn't reuse
/// GlobalVariable* pointers that belong to a torn-down previous TheModule.
/// Called from InitializeModule() itself (CodegenCore.cpp); a no-op the
/// first time, since those statics start out null.
void ResetArgvHelpers();

// ── SEM-002: runtime signed-integer-overflow trapping ────────────────────
// All defined in CodegenCore.cpp. FREEZE.md §6.2 row 7: runtime signed
// integer overflow traps (panics), never wraps, never UB. Needed by
// CodegenExpr.cpp (BinaryExprAST '+'/'-'/'*'/'<<'/'/, UnaryExprAST '-') and
// CodegenVar.cpp (CompoundAssignmentExprAST, PostfixExprAST -- neither
// reuses BinaryExprAST::codegen(), so each needs the same helpers called
// directly; see CodegenCore.cpp's own comment on EmitCheckedIntArith).

/// Checked '+'/'-'/'*' on two i64 values -- traps via CodegenPanic() on
/// overflow, otherwise returns the (in-range) result.
llvm::Value *EmitCheckedIntArith(char Op, llvm::Value *L, llvm::Value *R,
                                 SourceLocation Loc);

/// Checked unary negation -- traps only for the single input (INT64_MIN)
/// whose negation has no representable int64 result.
llvm::Value *EmitCheckedIntNegate(llvm::Value *V, SourceLocation Loc);

/// Checked '<<' -- traps if the shift amount is outside [0, 64) or if any
/// set bit was shifted out past the sign (LLVM has no
/// llvm.shl.with.overflow intrinsic, unlike +/-/*, so this is checked
/// directly rather than via an intrinsic call).
llvm::Value *EmitCheckedShl(llvm::Value *L, llvm::Value *R, SourceLocation Loc);

/// Traps if computing `L / R` (or `L % R`) would be INT64_MIN / -1 -- the
/// one signed-division overflow case (ARCHITECTURE_OVERHAUL.md §10.2).
/// Call after the existing divide-by-zero check (so R != 0 is already
/// established) and before emitting the actual CreateSDiv/CreateSRem.
void EmitSignedDivOverflowGuard(llvm::Value *L, llvm::Value *R, SourceLocation Loc);

// ── TOOL-001: `-g` debug info helpers ────────────────────────────────────
// All defined in CodegenCore.cpp. Every one of these is safe to call
// unconditionally, `-g` or not -- each checks g_EmitDebugInfo/DBuilder
// itself and no-ops (or returns nullptr) when debug info isn't being
// emitted, so call sites in the other 6 Codegen*.cpp files never need their
// own "if (g_EmitDebugInfo)" guard.

/// Looks up (or lazily creates) the llvm::DIFile for a SourceLocation's own
/// Filename -- cached by path, same idea as LLVMTypeCache. Needed by
/// CodegenFunc.cpp (a function's own declaring file, which may differ from
/// DebugCU's main file for a stdlib/imported-module function -- see the
/// "double-parse wrinkle"/AllSourceLines note in docs/architecture/
/// compiler_pipeline.md §0 and ARCHITECTURE_OVERHAUL.md §8 TOOL-001's own
/// "two TurfLang-specific wrinkles"). Returns DebugCU's own file for an
/// empty/synthesized location, and nullptr when `-g` is off.
llvm::DIFile *getOrCreateDebugFile(const std::string &Filename);

/// Best-effort DWARF DIType for a TurfType -- DIBasicType for primitives,
/// DIDerivedType for pointers/string, DICompositeType for fixed-size arrays
/// and structs (lazily built from the already-registered TurfStructType +
/// the already-`setBody()`'d llvm::StructType's real DataLayout offsets, so
/// it never needs its own struct-declaration-time hook), a typedef-to-
/// underlying-int for enums, and DBuilder->createUnspecifiedType() for
/// anything this doesn't specifically recognize (generics/erased type
/// parameters, function values, error unions -- richness beyond "some named
/// type with a plausible size" isn't needed for TOOL-001's own completion
/// criteria: a breakpoint, a backtrace, and printing a local). Cached by
/// mangled name, mirroring getLLVMType()'s own LLVMTypeCache. Returns
/// nullptr when `-g` is off.
llvm::DIType *getDebugType(const TurfType *T);

/// Best-effort SourceLocation for an arbitrary AST node -- same dispatch
/// shape as getExprType() (CodegenCore.cpp), dynamic_cast across every node
/// kind that carries a real getLoc(). Returns a default-constructed (Line
/// == 0) SourceLocation for a node with no location of its own (BlockExprAST
/// itself, a handful of bare literals) or an unrecognized node -- SetDebugLoc()
/// below treats Line == 0 as "keep whatever debug location is already set"
/// rather than emitting a location that would point gdb at line 0.
SourceLocation GetNodeDebugLoc(const ExprAST *E);

/// Sets Builder's current debug location from @p Loc, scoped to
/// CurrentDebugScope -- every subsequent llvm::IRBuilder Create*() call
/// attaches this location automatically until the next SetDebugLoc() call,
/// which is what gives every instruction a DILocation without every single
/// codegen() needing to build one by hand (see CodegenControlFlow.cpp's
/// BlockExprAST::codegen() loop, the one call site that alone covers every
/// ordinary statement inside a `{ }` body). No-ops when `-g` is off,
/// CurrentDebugScope is null (top level), or Loc.Line <= 0 (synthesized
/// node -- keeping the previous real location is more useful to a debugger
/// than pointing it at line 0).
void SetDebugLoc(const SourceLocation &Loc);

#endif
