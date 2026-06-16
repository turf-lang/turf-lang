/**
 * @file codegen_stubs.cpp
 * @brief Stub codegen() implementations for test_type_checker.
 *
 * The vtable for each ExprAST subclass is anchored in the translation unit
 * that defines the first non-inline virtual function — which is codegen().
 * In the full build this is Codegen.cpp.  The test binary cannot link
 * Codegen.cpp (it pulls in the entire LLVM IR builder chain).
 *
 * This file provides no-op stubs so the linker is satisfied.  These stubs
 * are NEVER called at test time because TypeChecker only calls accept() and
 * reads metadata — it never calls codegen().
 *
 * Do NOT include this file in the main turf executable.
 */

#include "AST.h"

// Make the LLVM headers happy without initialising any real LLVM context.
// We just need the vtable entries to exist; they are never invoked in tests.

llvm::Value *NumberExprAST::codegen()    { return nullptr; }
llvm::Value *BoolExprAST::codegen()     { return nullptr; }
llvm::Value *StringExprAST::codegen()   { return nullptr; }
llvm::Value *NullExprAST::codegen()     { return nullptr; }
llvm::Value *BinaryExprAST::codegen()   { return nullptr; }
llvm::Value *UnaryExprAST::codegen()    { return nullptr; }
llvm::Value *AssignmentExprAST::codegen(){ return nullptr; }
llvm::Value *VarDeclExprAST::codegen()  { return nullptr; }
llvm::Value *CastExprAST::codegen()     { return nullptr; }
llvm::Value *BlockExprAST::codegen()    { return nullptr; }
llvm::Value *IfExprAST::codegen()       { return nullptr; }
llvm::Value *WhileExprAST::codegen()    { return nullptr; }
llvm::Value *ForExprAST::codegen()      { return nullptr; }
llvm::Value *BreakExprAST::codegen()    { return nullptr; }
llvm::Value *ContinueExprAST::codegen() { return nullptr; }
llvm::Value *ReturnExprAST::codegen()   { return nullptr; }

llvm::Value *FuncDefExprAST::codegen()  { return nullptr; }
llvm::Value *FuncCallExprAST::codegen() { return nullptr; }
llvm::Value *MethodCallExprAST::codegen(){ return nullptr; }
llvm::Value *FieldAccessExprAST::codegen(){ return nullptr; }
llvm::Value *FieldAccessExprAST::codegenLValue(){ return nullptr; }
llvm::Value *StructDeclExprAST::codegen(){ return nullptr; }
llvm::Value *StructInitExprAST::codegen(){ return nullptr; }
llvm::Value *ArrayDeclExprAST::codegen(){ return nullptr; }
llvm::Value *ArrayAccessExprAST::codegen(){ return nullptr; }
llvm::Value *ArrayAccessExprAST::codegenLValue(){ return nullptr; }
llvm::Value *ArrayAssignExprAST::codegen(){ return nullptr; }
llvm::Value *ArrayLengthExprAST::codegen(){ return nullptr; }
llvm::Value *ModuleImportExprAST::codegen(){ return nullptr; }
llvm::Value *SizeOfExprAST::codegen()   { return nullptr; }
llvm::Value *VariableExprAST::codegen() { return nullptr; }
llvm::Value *VariableExprAST::codegenLValue(){ return nullptr; }
