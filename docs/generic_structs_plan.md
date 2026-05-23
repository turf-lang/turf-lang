# Goal Description
The goal is to implement Generic Structs (Monomorphization) directly in the Turf-Lang C++ compiler architecture. This will finally allow users to build generic, native data structures like `Stack<T>` and `Queue<T>` in Turf-Lang natively without modifying the internal C++ source code going forward.

## Proposed Changes

We will introduce Monomorphization into the parsing and code generation pipeline. 

### 1. Type System Extensions (`include/Types.h`)
- **[NEW]** Add `TurfTypeVariable` inheriting from `TurfType` to represent an unbound template parameter (like `T` or `K`).
- **[NEW]** Add `GenericStructTemplate` struct to hold a generic definition (`Name`, `TypeParams`, `Fields`) before it is instantiated.
- **[NEW]** Add a recursive `TurfTypePtr substituteType(TurfTypePtr type, const std::map<std::string, TurfTypePtr>& mapping)` function to clone types replacing variables (e.g., `T -> int`).

### 2. Symbol Table Management (`include/SymbolTable.h`, `src/SymbolTable.cpp`)
- **[NEW]** Add a registry for generic templates: `std::map<std::string, std::shared_ptr<GenericStructTemplate>> StructTemplates`.
- **[MODIFY]** Add methods to Register and Lookup struct templates separately from concrete types.

### 3. Parser Enhancements (`src/Parser.cpp`)
- **[MODIFY] `ParseStructDecl`**:
  - Check for optional `<T1, T2>` syntax right after the struct name.
  - If type parameters exist, push a temporary scope and register them as `TurfTypeVariable`s so `ParseType()` can successfully parse fields containing `T`.
  - Save the struct as a `GenericStructTemplate` instead of immediately instantiating a concrete `TurfStructType`.
- **[MODIFY] `ParseType`**:
  - When parsing a `Name<Arg1, Arg2>` block, check if `Name` exists in the `StructTemplates` registry.
  - If it does, dynamically generate a new monomorphized type (e.g., `Name_Arg1_Arg2`), substitute all inner fields (`T -> Arg1`), register it in the `GlobalSymbolTable` as a standard `TurfStructType`, and return it.

### 4. Code Generation Updates (`src/Codegen.cpp`)
- **[MODIFY] `StructDeclExprAST::codegen`**:
  - If the struct is generic (i.e., has `TypeParams`), exit early and do not generate an LLVM struct block. LLVM structs will only be generated when concretized.
- **[MODIFY] `getLLVMType`**:
  - When resolving a `TurfStructType`, if the LLVM struct doesn't exist in the cache, lazily create it via `llvm::StructType::create`, translate the fields recursively via `setBody`, and cache it. This handles the dynamic instantiation of `Stack_int` when it's first referenced in LLVM.

## User Review Required
> [!IMPORTANT]
> The architectural leap you are proposing (Monomorphization) is advanced and touches multiple critical compiler components. I have laid out exactly how I plan to parse `<T>` tags, store templates, substitute types, and dynamically generate LLVM structs on demand.
> 
> Regarding your other question: `raise Exception("...")` throws a structured struct payload with a specific type so your `onError(err)` can handle different errors intelligently, whereas `raise "..."` simply throws a raw string.
>
> Does the Generic Structs plan look good to you? If so, I will execute these changes immediately!
