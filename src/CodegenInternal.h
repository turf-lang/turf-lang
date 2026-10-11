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

/// Declares (once) or looks up `libturfrt`'s `turf_rt_alloc` -- see this
/// function's own definition (CodegenCore.cpp) for the OVH-002 reasoning.
/// Needed by CodegenExpr.cpp (`int`/`double` -> `string` cast, which built
/// its result buffer with a bare `malloc` before OVH-002 -- a `string`
/// value produced by any operation belongs on the GC heap, FREEZE.md §6.5).
llvm::Function *getOrCreateTurfRtAllocFn();

/// FDN-001 (docs/planning/ROADMAP_v2.0.0.md, DEC-5): the codegen for the
/// `heap(expr): &T` bare intrinsic. Allocates `sizeof(Val's LLVM type)`
/// bytes on the GC heap with that type's REAL TurfTypeDesc (a named
/// struct's own whole-instance descriptor via
/// getOrForwardDeclareTurfTypeDescGlobal(), or
/// getOrCreateElementTurfTypeDesc()'s general per-LLVM-type descriptor for
/// anything else -- primitive or fixed array), stores @p Val's already-
/// evaluated bits into the fresh block, and returns its address. Reuses the
/// exact size/descriptor-selection shape BoxToI64Slot()'s aggregate branch
/// already established (CodegenCore.cpp) for the identical reason
/// (`!T`'s oversized success-value boxing) -- the only difference is the
/// return value: heap() hands back the real pointer, never an i64-boxed
/// slot. Needed by CodegenFunc.cpp (FuncCallExprAST::codegen()'s `heap`
/// dispatch branch).
llvm::Value *HeapAllocateValue(llvm::Value *Val);

/// STD-002 (ARCHITECTURE_OVERHAUL.md §8/§15.6(a)): the value a
/// `@caller_location` function's hidden extra argument gets, either at a
/// literal expression-level read (CallerLocationExprAST::codegen()) or at
/// a call site that needs to auto-supply the hidden argument
/// (FuncCallExprAST::codegen(), CodegenFunc.cpp). If NamedValues currently
/// has a "caller_location" entry (i.e. Codegen is inside the body of a
/// function ALSO declared `@caller_location`), forwards that function's
/// own inherited value unchanged -- this is the whole propagation
/// mechanism, with no separate bookkeeping needed (NamedValues is already
/// saved/restored per function by FuncDefExprAST::codegen()). Otherwise
/// (ordinary code, or a non-`@caller_location` function), stamps a
/// compile-time string constant built from @p StampLoc -- the call
/// expression's own location, so the ROOT of a propagation chain always
/// names the true, textual call site, never a placeholder.
llvm::Value *BuildCallerLocationValue(const SourceLocation &StampLoc);

/// Calls a resolved `operator` overload @p F with @p Args, appending the
/// hidden caller-location argument when @p Target is `@caller_location`
/// (stamped from @p Loc, the operator expression). Every operator dispatch
/// site goes through here, so a `@caller_location` operator never receives
/// one argument too few.
llvm::Value *CallOperator(llvm::Function *F, const ResolverFunctionSymbol &Target,
                          std::vector<llvm::Value *> Args, const SourceLocation &Loc,
                          const llvm::Twine &Name = "");

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

/// The declared payload field types of variant @p VariantName of the error
/// type @p ErrorTypeName (an identity), in declaration order -- empty for a
/// variant with none, or an unknown name. LEC-004. Needed by CodegenFunc.cpp
/// (casting each constructor argument) and CodegenPattern.cpp.
std::vector<TurfTypePtr> ErrorVariantFieldTypes(const std::string &ErrorTypeName,
                                                const std::string &VariantName);

/// The LLVM struct a variant's payload is stored as -- one named struct per
/// variant with fields (`errpayload.<Error>.<Variant>`), built on first use
/// and cached, so EmitAllTurfTypeDescs() gives it the descriptor the collector
/// traces its `string`/collection fields through (LEC-004). nullptr for a
/// variant with no fields. Needed by CodegenPattern.cpp (reading a payload).
llvm::StructType *getErrorPayloadStructType(const std::string &ErrorTypeName,
                                            const std::string &VariantName);

/// The tag string (i8*) naming one error variant -- exactly what an error
/// value's tag field holds and what `==` compares. Needed by CodegenPattern.cpp.
llvm::Value *BuildErrorVariantTag(const std::string &ErrorTypeName,
                                  const std::string &VariantName);

/// Builds an error value (`ErrorType::Variant(fields..., [message])`):
/// GC-allocates the payload struct (if the variant has fields) and the
/// `TurfErrorValue` pointing at it. @p FieldVals are already cast to the
/// declared field types; @p MessageVal is null when no message was written.
/// Needed by CodegenFunc.cpp (FuncCallExprAST::codegen()'s
/// ErrorVariantConstruct branch).
llvm::Value *CodegenErrorVariantConstruct(const std::string &ErrorTypeName,
                                          const std::string &VariantName,
                                          const std::vector<llvm::Value *> &FieldVals,
                                          llvm::Value *MessageVal);

/// Reads field @p FieldIdx (0 = tag, 1 = message, 2 = payload) out of a
/// runtime `TurfErrorValue*` value. Needed by CodegenStruct.cpp
/// (FieldAccessExprAST::codegen()'s `err.message` special case, field 1) and
/// CodegenPattern.cpp (tag and payload).
llvm::Value *LoadErrorValueField(llvm::Value *ErrValPtr, unsigned FieldIdx,
                                 const char *Name);

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

/// Checked '>>' (arithmetic) -- traps if the shift amount is outside
/// [0, 64), the same rule and panic text as EmitCheckedShl().
llvm::Value *EmitCheckedShr(llvm::Value *L, llvm::Value *R, SourceLocation Loc);

/// Traps if computing `L / R` would be INT64_MIN / -1 -- the one
/// signed-division overflow case (ARCHITECTURE_OVERHAUL.md §10.2).
/// Call after the existing divide-by-zero check (so R != 0 is already
/// established) and before emitting the actual CreateSDiv.
void EmitSignedDivOverflowGuard(llvm::Value *L, llvm::Value *R, SourceLocation Loc);
/// `L % R` for signed integers, after the zero check: INT_MIN % -1 is 0
/// (representable, so no trap), never `srem`'s poison.
llvm::Value *EmitSafeSRem(llvm::Value *L, llvm::Value *R, const llvm::Twine &Name);

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

// ── MEM-002: the collector ───────────────────────────────────────────────
// Defined in CodegenStruct.cpp (alongside RT-001's struct-instance
// EmitAllTurfTypeDescs()/emitTurfTypeDesc(), which this shares its
// pointer-word-bitmap-building logic with).

/// Returns (memoized, by raw llvm::Type* identity) a per-ELEMENT
/// TurfTypeDesc constant global describing one element of @p ElemTy --
/// runtime/turfrt.h's TurfTypeDesc doc comment explains how the collector
/// repeats this across a block's real length. Needed by CodegenExpr.cpp's
/// CastExprAST::codegen(), which recognizes a `gc_alloc(...)`/
/// `gc_alloc_zeroed(...)`/`gc_realloc(...)` operand being cast `to T*` and
/// injects this descriptor in place of MEM-001's literal NULL.
llvm::GlobalVariable *getOrCreateElementTurfTypeDesc(llvm::Type *ElemTy);

/// OVH-002: returns (forward-declaring if necessary) the
/// "__turf_typedesc_<Name>" global holding @p ST's own whole-struct
/// TurfTypeDesc -- the SAME descriptor `EmitAllTurfTypeDescs()` emits for
/// every named struct type in the module, just referenceable from a call
/// site that runs before that late pass does. Needed by CodegenCore.cpp's
/// `turf_rt_alloc` call sites (a message-carrying error value, an oversized
/// `!T` box) so they can pass a real descriptor instead of `NULL`, for a
/// struct type that is ALREADY fully built (INV-RT-3's own guarantee) by
/// the time any function body codegens -- see this function's own
/// definition (CodegenStruct.cpp) for why `emitTurfTypeDesc()` must fill in
/// the SAME global object this returns, not a fresh one.
llvm::GlobalVariable *getOrForwardDeclareTurfTypeDescGlobal(
    llvm::StructType *ST);

/// Set by CastExprAST::codegen() immediately before it calls codegen() on a
/// `gc_alloc`/`gc_alloc_zeroed`/`gc_realloc` operand it recognizes as
/// `to T*`-cast, and consumed (read once, then reset to nullptr) by
/// FuncCallExprAST::codegen()'s own dispatch for those three names
/// (CodegenFunc.cpp) in place of the literal null pointer MEM-001 always
/// passed. nullptr in every other context (an ordinary `gc_alloc(n) to
/// string` string-buffer site, or any call not immediately wrapped in a
/// recognized cast) -- MEM-002 defines a NULL desc as "opaque, no pointer
/// words traced", which is correct, not merely a fallback, for those sites
/// (see turfrt.h's turf_rt_alloc doc comment). A single file-scope Value*
/// rather than a parameter threaded through codegen()'s fixed virtual
/// signature -- the same "can't change a virtual codegen() signature for
/// one caller" constraint SetDebugLoc()'s CurrentDebugScope above already
/// works around the identical way.
extern llvm::Value *PendingGCAllocDescOverride;

#endif
