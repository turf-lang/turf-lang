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
/// CodegenExpr.cpp (DefaultInitExprAST), CodegenFunc.cpp (implicit struct
/// field defaults), and CodegenVar.cpp (VarDeclExprAST with no initializer).
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

#endif
