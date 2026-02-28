# Control Flow Graph (CFG) Implementation

## Overview

The Turf compiler now includes a Control Flow Graph (CFG) implementation that analyzes the control flow structure of functions to detect potential issues and enable better diagnostics.

## Architecture

### Core Components

#### 1. **TurfBasicBlock** (`include/CFG.h`)

Represents a single basic block in the control flow graph:

- **Single entry point** at the beginning
- **Single exit point** at the end
- **No branches** except at the end

**Key members:**

- `BlockID ID` - Unique identifier
- `std::string Name` - Human-readable name (e.g., "entry", "if.then")
- `std::vector<ExprAST*> Statements` - Statements in this block
- `std::vector<TurfBasicBlock*> Predecessors/Successors` - Control flow edges
- `TerminatorKind Terminator` - How the block ends (return, break, branch, etc.)
- `bool IsReachable` - Whether reachable from function entry

#### 2. **CFG** (`include/CFG.h`)

Container for all basic blocks in a function:

- Maintains entry and exit blocks
- Manages block creation and edge linking
- Provides analysis passes (reachability, return path checking)
- Reports flow-based diagnostics

**Key methods:**

- `createBlock()` - Create a new basic block
- `addEdge()` - Link two blocks (control flow edge)
- `computeReachability()` - Mark reachable blocks from entry
- `reportFlowDiagnostics()` - Report control flow issues

#### 3. **CFGBuilder** (`src/CFG.cpp`)

Constructs CFGs from AST by visiting expressions:

- Traverses function bodies
- Creates basic blocks at control flow boundaries
- Links blocks based on control flow constructs
- Handles loops, conditionals, returns, break, continue

## Terminator Types

```cpp
enum class TerminatorKind {
  None,          // No explicit terminator (fallthrough)
  Return,        // return statement
  Break,         // break statement
  Continue,      // continue statement
  Branch,        // Conditional branch (if)
  Unconditional  // Unconditional jump (end of then/else, loop body)
};
```

## Control Flow Constructs

### If-Else Blocks

```
Entry Block
    ├─> Then Block ─┐
    └─> Else Block ─┤
                    └─> Merge Block
```

### While Loops

```
Entry Block
    └─> Condition Block
            ├─> Body Block ─┐ (loops back)
            |               |
            |       <───────┘
            └─> Exit Block
```

## Diagnostics Enabled

### 1. **Unreachable Code Detection**

Identifies basic blocks that cannot be reached from function entry:

```turf
fn int example() {
    return 5
    int x = 10  // Unreachable!
}
```

### 2. **Missing Return Paths**

Warns when not all control flow paths end with a return:

```turf
fn int example(int x) {
    if (x > 0) {
        return x
    }
    // Missing return for x <= 0 case
}
```

### 3. **Dead Branches**

Detects conditional branches that are never taken.

### 4. **Statements After Terminators**

Built on existing semantic checks to catch statements after:

- `return`
- `break`
- `continue`

## Integration

CFG construction happens during function definition code generation:

```cpp
// In FuncDefExprAST::codegen() (src/Codegen.cpp)
Body->codegen();  // Generate LLVM IR

// Build and analyze CFG
CFGBuilder Builder;
auto FuncCFG = Builder.buildCFG(Name, Body.get());
FuncCFG->reportFlowDiagnostics();  // Report issues
GlobalCFGs.push_back(std::move(FuncCFG));  // Store for later use
```

## Current Limitations

1. **AST-Level Analysis**: CFG is built from AST nodes before full LLVM IR generation. This means it operates on high-level constructs rather than low-level instructions.

2. **Block Granularity**: The current implementation creates blocks at major control flow boundaries (if/else, loops) but treats sequences of statements as single units.

3. **No Data Flow**: The CFG tracks control flow but not data flow. Variable liveness and reaching definitions are not analyzed.

4. **No Optimization**: The CFG is purely for diagnostics, not optimization passes.

## Future Enhancements

### Phase 3 Possibilities:

- **Context-Aware Diagnostics**: Use CFG to provide better error messages with full control flow context
- **Initialization Tracking**: Detect use of variables that may not be initialized on all paths
- **Loop Analysis**: Detect infinite loops or loops that never execute
- **Dead Code Elimination**: Use reachability information for optimization
- **SSA Form**: Convert to Static Single Assignment for advanced analyses

## Example Output

```bash
$ ./turf tests/cfg_test_unreachable.tr
Warning: Unreachable code in block 'unreachable' of function 'test_function'
Error at 7:8: Unreachable declaration after return statement
```

## Testing

Run the CFG test suite:

```bash
bash ./scripts/test_cfg.sh
```

Test files in `tests/cfg_test_*.tr` cover:

- Unreachable code detection
- Missing return paths
- Break/continue unreachable code
- Valid control flow
- Complex nested structures

## Implementation Files

- `include/CFG.h` - CFG data structures and interfaces
- `src/CFG.cpp` - CFG construction and analysis implementation
- `include/Errors.h` - CFG-specific error types added
- `src/Codegen.cpp` - CFG integration into compilation pipeline

## References

- **Basic Block**: Maximal straight-line code sequence with no branches except at the end
- **Control Flow Graph**: Directed graph representing all paths that might be traversed during program execution
- **Reachability Analysis**: Determining which blocks can be reached from the entry point
