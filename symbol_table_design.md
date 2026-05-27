# Turflang Symbol Table & Resolver Architecture

To handle modules, current file scope, function overloading, structs, and variables cleanly, you need a hierarchical data structure. In C++, this is typically implemented using a combination of `unordered_map`s inside nested `Scope` objects.

Here is the industry-standard blueprint for how to structure this in C++.

## 1. The Core Symbol Types

First, define what exactly you are storing in the HashMaps. You don't just store LLVM values; you store "Symbols" which contain all semantic information.

```cpp
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

// Forward declarations
class Type; 

// Represents a local variable or global variable
struct VariableSymbol {
    std::string Name;
    std::shared_ptr<Type> VarType;
    bool IsMutable;
    // llvm::Value* LLVMValue; // Attached later during codegen
};

// Represents a single function signature (one of potentially many overloads)
struct FunctionSymbol {
    std::string BaseName;       // "printline"
    std::string MangledName;    // "turf_io_printline_int"
    std::vector<std::shared_ptr<Type>> ParamTypes;
    std::shared_ptr<Type> ReturnType;
    bool IsIntrinsic;
    int Arity;                  // For arity checks!
};

// Represents a Struct/Class definition
struct StructSymbol {
    std::string Name;
    std::unordered_map<std::string, std::shared_ptr<Type>> Fields;
    // If structs have methods, you can put a HashMap of FunctionSymbols here too!
};
```

## 2. The Scope (The Lexical Environment)

A `Scope` represents a block of code `{ ... }` or a file level. Scopes are nested. When looking up a variable, you check the current scope, and if it's not there, you check the parent scope.

```cpp
class Scope {
public:
    Scope* Parent; // Null if this is the root scope of a module

    // HashMaps for different types of symbols
    std::unordered_map<std::string, VariableSymbol> Variables;
    std::unordered_map<std::string, StructSymbol> Structs;
    
    // NOTE: The value is a VECTOR of FunctionSymbols to support overloading!
    std::unordered_map<std::string, std::vector<FunctionSymbol>> Functions;

    Scope(Scope* parent = nullptr) : Parent(parent) {}

    // Example resolution method for variables
    VariableSymbol* lookupVariable(const std::string& name) {
        auto it = Variables.find(name);
        if (it != Variables.end()) {
            return &it->second;
        }
        if (Parent) {
            return Parent->lookupVariable(name);
        }
        return nullptr;
    }
};
```

## 3. The Module (Namespaces)

A `Module` represents an entire file or a standard library namespace like `io` or `math`. It owns a root scope.

```cpp
class Module {
public:
    std::string Name; // "io", "math", or "main" (current file)
    std::shared_ptr<Scope> RootScope;

    Module(const std::string& name) : Name(name) {
        RootScope = std::make_shared<Scope>();
    }

    // Lookup a specific function overload in this module
    FunctionSymbol* lookupFunction(const std::string& name, const std::vector<std::shared_ptr<Type>>& argTypes) {
        auto it = RootScope->Functions.find(name);
        if (it == RootScope->Functions.end()) return nullptr;

        // Overload resolution: iterate through the vector of signatures
        for (auto& overload : it->second) {
            if (overload.ParamTypes.size() != argTypes.size()) continue;
            
            bool match = true;
            for (size_t i = 0; i < argTypes.size(); ++i) {
                if (!argTypes[i]->isEqual(overload.ParamTypes[i].get())) {
                    match = false;
                    break;
                }
            }
            if (match) return &overload;
        }
        return nullptr; // Function exists, but no matching overload!
    }
};
```

## 4. The Global Symbol Table (The Brain)

This is the master class that the Semantic Analyzer uses to keep track of everything across all files.

```cpp
class SymbolTable {
public:
    // Maps module names like "io" or "math" to their Module object
    std::unordered_map<std::string, std::shared_ptr<Module>> Modules;
    
    // The module currently being compiled (e.g., "main")
    std::shared_ptr<Module> CurrentModule;
    
    // The specific block we are inside (e.g., inside an if-statement)
    std::shared_ptr<Scope> CurrentScope;

    SymbolTable() {
        CurrentModule = std::make_shared<Module>("main");
        CurrentScope = CurrentModule->RootScope;
    }

    // Call this when you encounter `{`
    void enterScope() {
        CurrentScope = std::make_shared<Scope>(CurrentScope.get());
    }

    // Call this when you encounter `}`
    void exitScope() {
        if (CurrentScope->Parent) {
            CurrentScope = std::shared_ptr<Scope>(CurrentScope->Parent);
        }
    }

    // Resolving a cross-namespace call like `io.printline(42)`
    FunctionSymbol* resolveModuleFunction(const std::string& moduleName, 
                                          const std::string& funcName, 
                                          const std::vector<std::shared_ptr<Type>>& argTypes) {
        auto modIt = Modules.find(moduleName);
        if (modIt == Modules.end()) {
            // Throw error: "Module 'moduleName' not found"
            return nullptr; 
        }

        FunctionSymbol* func = modIt->second->lookupFunction(funcName, argTypes);
        if (!func) {
            // Throw error: "Function 'funcName' with given arguments not found in module 'moduleName'"
            // HERE is where you can do Levenshtein distance on modIt->second->RootScope->Functions.keys()
            // to suggest "Did you mean: printline?"
        }
        return func;
    }
};
```

## How this solves your problems

1. **`io.printline(42)`**: The parser gives you `NamespaceAccess(module: "io", member: "printline", args: [42])`. The Semantic Analyzer calls `SymbolTable.resolveModuleFunction("io", "printline", [IntType])`. It returns the exact `FunctionSymbol` containing `turf_io_printline_int`. No string math required.
2. **Local Variables hiding Globals**: Because `CurrentScope` falls back to `Parent` scope, local variables perfectly shadow outer variables.
3. **Module Isolation**: `math.abs` and `my_module.abs` live in completely different `Module` objects inside the `Modules` hashmap. They can never accidentally bleed into each other.
4. **Typo Suggestions**: If a user types `io.pritn()`, the lookup fails, but you already have a pointer to the `io` Module. You simply iterate over the keys in `io->RootScope->Functions` (which is small) to find the closest match.
