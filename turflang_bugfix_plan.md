# Turflang Bugfix Implementation Plan

## Summary:
The overarching strategy is to tackle the issues in order of their impact on compiler stability and architectural foundations:

**Crashes → Resolver architecture → Module-dependent bugs → Type system → Polish**

The biggest leverage point is Phase 2. A clean namespace-aware resolver will likely make several other bugs either self-resolve or become much simpler to diagnose.

---

## Phase 1 — Stop the crashes first (blocking everything)
> [!IMPORTANT]
> These are outright compiler crashes from missing bounds checks — a single `Args.size()` guard fixes them. Quick wins, high impact.

### Note 10: `typeof()` crash on empty args
`typeof()` has started to give out the type of multiple inputs for some reason.

    io.printline(typeof(5, 10))

This prints

    int

Looking at LLVM IR, this is for sure:

`@typename = private unnamed_addr constant [4 x i8] c"int\00"`

Here, except for the first argument, everything else is just ignored silently. Can be ignored by adding a rule in the Parser when it comes to `typeof()` something like this in the grammar:

`TYPEOF_EXPR: typeof '(' expression ')'`

Confirmed first argument by looking at the IR:

`@typename = private unnamed_addr constant [7 x i8] c"double\00", align 1`

For the expression:

    io.printline(typeof(5.0, 10, 11))

Now, when looking at another line:

    io.printline(typeof())

It crashes due to a compiler bug.

`/bits/stl_vector.h:1253: reference std::vector<llvm::Value *>::operator[]: Assertion '__n < this->size()' failed.`

Somewhere in the compiler:

`args[0]`

happens.

Predicted crash chain:

```
typeof()
   └─→ parser accepts it (no arity check)
          └─→ semantic analysis passes it through
                 └─→ codegen does: llvm::Value* val = args[0]  ← BOOM
                                                            ^^^
                              empty vector, index 0 doesn't exist
```

### Note 13: Math intrinsic arity checks
Math intrinsics are beyond cooked. hell nah:

The power codegen is even more dangerous than abs.

For a case like `math_power_error_int_string.tr`:

`math.power(2, "hello")`

This happens:

```cpp
Value *B = Args[1];           // B is a ptr (string)
bool BIsDouble = B->getType()->isDoubleTy();  // false
bool BIsInt = B->getType()->isIntegerTy(64);  // false
```

It's trying to do `sitofp` on a ptr type, which LLVM rejects at IR validation, causing the internal error that then triggers your cross-namespace fallback lookup.

```
> ./turf tests/custom/math_power_error_int_string.tr
[Main] GlobalCFGs size before CallGraph: 0
[CallGraph] Functions: 0
── error at line 4, col 19

  Oops! I can't find a function named 'math_power'.
  Hint: Double-check the spelling against the built-in functions like 'printline' or 'lengthof'.

  4 │     io.printline(math.power(2, "hello"))
    │                   ^ Here!
```

1. `Args.size()` check  → before ANY `Args[n]` access
2. Type whitelist     → explicit `isIntegerTy(64) || isDoubleTy()`
                        never a catch-all `else` that accepts `ptr`/`i1`

Another example in `math_power_error_three_args.tr`

```turflang
import math

fn int main() {
    io.printline(math.power(2, 3, 4)) // Can't match declaration thus cooked and started to find something else

    return 0
}
```

What in the living hell:

    io.printline(math.power(2))

gives: 2 🥀💔

no way.

The IR speaks for itself:

  `%calltmp = call double @turf_math_floor_double(double 2.000000e+00)`

`math.power(2)` became `math.floor(2)` and printed 2.

```
math.power(2)
  └─→ Args.size() == 1, needs 2
        └─→ no arity check exists
              └─→ Args[1] is uninitialized/garbage
                    └─→ LLVM IR builder gets a bad Value*
                          └─→ falls through to next intrinsic handler
                                └─→ math.floor(Args[0]) ← WRONG FUNCTION ENTIRELY
                                      └─→ floor(2.0) = 2
```

Intrinsic dispatch is falling through to the next if block when `Args[1]` fails, instead of erroring. It's essentially:

```cpp
if (IntrinsicName == "power") {
    Value *A = Args[0];
    Value *B = Args[1];  // ← garbage/null when only 1 arg passed
    // ... B fails somewhere silently
    // ... falls through or jumps to next handler
}
// next handler down:
if (IntrinsicName == "floor") {  // ← this fires with Args[0] = 2
```

Expected arity should be defined in the intrinsic which should be stored somewhere in either the SymbolTable or the Builtins, but need to be stored. Like @intrinsic("abs", 2). LLVM intrinsics might already have arity inside them, but idk.
Arity is very necessary for anything requiring a fixed number of arguments.

Kind of a table like this needs to be made:

```cpp
static const std::unordered_map<std::string, int> IntrinsicArity = {
    {"abs",   1},
    {"sqrt",  1},
    {"sin",   1},
    {"cos",   1},
    {"floor", 1},
    {"ceil",  1},
    {"round", 1},
    {"power", 2},
    {"pow",   2},
};
```

---

## Phase 2 — The architectural root cause
> [!NOTE]
> Note 12 and Note 2 are really the same problem — the resolver doing global string-prefix matching instead of namespace-aware lookup. This is the root cause behind the cross-namespace misrouting in Notes 12 and 13. Fix this before anything else that touches module resolution, because a lot of other bugs will either disappear or become much clearer once the hashmap-based symbol table is in place.

### Note 2: Resolver typo suggestions
After implementing modules and stuff, there is another weird issue.

When I make a typo in a method from another module, say from io.

`io.pritn()`

When it runs and can't find the symbol, it should suggest a near defined symbol from the symbol table. But it does not suggest something even near to that. Instead it suggests an error message expected from the current module implementation:

```
> ./turf tests/suggestions/builtin_typo.tr
[Main] GlobalCFGs size before CallGraph: 0
[CallGraph] Functions: 0
── error at line 4, col 3

  Oops! Something doesn't add up:
  Could not determine type of method call base

  4 │   io.pritn("hello")
    │   ^ Here!
```

Although the error is correct, it is not very helpful.

On the other hand, when a typo in a identifier name is there, for example, from a variable, it suggests the near variable.

```
> ./turf tests/suggestions/long_name_suggest.tr
[Main] GlobalCFGs size before CallGraph: 0
[CallGraph] Functions: 0
── error at line 5, col 16

  Hold on! You're trying to use 'idenfier_longname' before telling me what it is.
  Did you mean: 'identifier_longname'?

  5 │     io.printline(idenfier_longname)
    │                ^ Here!
```

That means when it parses a function name from other module, the prefix added in the function name must have caused this.

So, right now, the best fix which I can think of is maintaining a Hashmap of symbols from modules when importing.

When user imports io, all the identifiers are added to a central hashmap for the current context, with distinctions.

```
global:
    Keywords: ... (from Lexer)
current_file:
    Functions: ...
    Identifiers: ...
io: 
    Functions: print, printline, input, ...
    Identifiers: newline, ...
math:
    Functions: sin, cos, tan, abs, pow, sqrt, ...
    Identifiers: pi, e, ...
```

This would then help in a better error suggestion.

So, now when a user types `io.pritn()`, it tries to match both the module if there is one and then tries to find the matches from both the functions and the identifiers.
If there is no module, then it must be from the current file, but can also be from other files, so two suggestions can be there ranked:
1. From the current file.
2. From the imported files.

Suggested as:
```
---
Error: Symbol not found

It may be from the current file: (Damereau-Levenshtein matches from current file)
Or, could be one of these from the imported files: print() found from io module.
```

Errors.h would also need a refactor then, with things like editing "double check against built-in functions" with "double check against existing functions".

### Note 12: Global string-prefix matching issues
Resolver is really bad. We can use the dictionary proposal to find and store functions and definitions, making SymbolTable kind of the brain of the Compiler, by deloading the parser altogether.

```cpp
// The resolver should do:
Module* mod = resolveModule("io");       // find the io module
Function* fn = mod->lookup("printline"); // look up within it
// NOT: globalLookup("io_printline")
```

A really funny thing is that in the program:

```turflang
import math

fn int main() {
    math.abs(true)
    io.printline(42)

    return 0
}
```

The emitted LLVM IR becomes this:

```llvm
define i64 @main() {
entry:
  call void @turf_io_print_bool(i1 true)    ; ← math.abs(true) became this!
  call void @turf_io_printline_int(i64 42)  ; ← io.printline(42)
  ret i64 0
}
```

Which is extremely weird.

`math.abs(true)` is being silently rewritten into `turf_io_print_bool(true)` — and since `print_bool` uses `%d` format (no newline), it prints 1 immediately before 42, producing 142 with no spaces.

```
output: "142"
         │└─── io.printline(42) → "42\n"
         └──── math.abs(true) → silently became print_bool(true) → "1"
```

Guessing the fail chain:

```
math.abs(true)
  └─→ no math.abs_bool overload found
        └─→ fallback/default handler triggered
              └─→ searches ALL registered functions accepting i1
                    └─→ finds turf_io_print_bool(i1) ← wrong namespace!
                          └─→ emits that call silently, no error
```

1. `math.abs` accepts bool with no type error
2. Overload resolution falls through to wrong namespace : `math.abs` → `io.print_bool`
3. No "no matching overload" error raised

A cross namespace resolution should not be allowed. This is clearly an ambiguity. Thus, hashmap based approach is much cleaner right now.

Confirmed codegen issue:

```cpp
Value *Val = Args[0];  // ← No arity check, crashes if Args is empty
if (Val->getType()->isDoubleTy()) {
    // double path
} else {
    // ← bool falls here! No type guard, accepts ANY non-double
    Value *Zero = ConstantInt::get(Type::getInt64Ty(*TheContext), 0);
    Value *Neg = Builder->CreateNeg(Val, "absneg");  // operates on i1 directly
    Value *Cmp = Builder->CreateICmpSLT(Val, Zero, "abscmp");
    return Builder->CreateSelect(Cmp, Neg, Val, "abs");
}
```

`else` branch explains all three bugs at once:

- `math.abs()` : `Args[0]` → out of bounds → falls to error handler → cross-namespace lookup
- `math.abs(true)` : `i1` is not `doubleTy` → enters `else` → operates on `i1` directly → misrouted
- `math.abs(5)` : Works correctly

AI is very careless 💔🥀
Wrong type misroutes, no argument leads to cross namespace lookup.

Need to always measure argument size as well in all the functions.

```
math.abs(1, 2)
         │  └─ silently ignored
         └─── abs(1) = 1 → printed
```

---

## Phase 3 — Bugs that depend on Phase 2 being clean
> [!NOTE]
> Note 5 (array param mangling), Note 1 (inconsistent prelude scoping), and Note 6 (module search order) all touch module/symbol resolution. Fixing them before Phase 2 risks patching over symptoms rather than the actual cause.

### Note 1: Inconsistent prelude scoping
After I edited the Prelude, it starts to give the expected error.

The issue was if I call the `io.printline()` in a function other than the `main()` function without the `io` namespace, it was not giving an error.
But the moment I edited the prelude and removed the math import and mem import, it was giving the expected error.

[BEFORE]

`prelude.tr`
```turflang
import io
import mem
import math
```

`test.tr`
```turflang
fn void function() {
    printline("HELLO")
}

fn int main() {
    function()

    return 0
}
```

This was not giving an error. The correct syntax would have been `io.printline("HELLO")`.

But in the same `prelude.tr`, if I do something like this in my `main()` function:

```turflang
fn int main() {
    printline("HELLO")

    return 0
}
```

This was giving an error that `printline()` is an unknown symbol. This is very inconsistent but very weird as well.

[AFTER]

`prelude.tr`
```turflang
import io
```

`test.tr`
```turflang
fn void function() {
    printline("HELLO")
}

fn int main() {
    function()

    return 0
}
```

This started to give an error.

### Note 5: Array param mangling issue
Extremely weird issue in `array_param_func.turf` file. Both the functions:

```turflang
fn int sum_array(int[5] data) {
    int total = 0
    io.printline("Function sum_array called")
    for i in 0..5 step i++ {
        io.printline(total)
        total = total + data[i]
    }
    return total
}

fn int get_first(int[10] arr) {
    io.printline("Function get_first called")
    return arr[0]
}
```

dispatch to: `turf_get_first_int_arr` in the emitted LLVM IR.

```llvm
%calltmp = call i64 @turf_get_first_int_arr(ptr nonnull %nums)  ; should be sum_array!
...
%calltmp18 = call i64 @turf_get_first_int_arr(ptr nonnull %big)
```

The issue is the order of function registration. `get_first` is defined after `sum_array`, so when the fallback loop runs:

```cpp
for (const auto &[Name, ParamTypes] : FuncParamTypes) {
    std::string Prefix = "turf_" + CalleeName;
    if (Name == Prefix ||
        (Name.size() > Prefix.size() && Name[Prefix.size()] == '_')) {
```

Since the program compiles, the fallback is finding something. The issue is likely that `getExprType()` on the variable returns the array type, but then this branch runs:

```cpp
if (const TurfType *ExprTy = getExprType(Arg.get())) {
    if (auto *Ptr = dynamic_cast<const TurfPointerType*>(ExprTy)) { ... }
    else if (ExprTy->isNamed("void*")) { ... }
    else if (ExprTy->isNamed("int")) ActTy = TurfInt();
    // ... no array case! Falls through to:
    else ActTy = getTurfTypeFromLLVM(V->getType());  // → TurfString
```

So, add an array case in the arg type collection block:

```cpp
if (const TurfType *ExprTy = getExprType(Arg.get())) {
    if (isArrayType(ExprTy)) {
        // Preserve the array type so mangling produces _int_arr, not _string
        ActTy = std::make_shared<ArrayType>(
            dynamic_cast<const ::ArrayType*>(ExprTy)->getElementTypePtr(),
            -1  // size irrelevant for mangling
        );
    } else if (auto *Ptr = dynamic_cast<const TurfPointerType*>(ExprTy)) {
        // ... existing code
    }
    // ... rest of existing cases
}
```

This ensures `sum_array(nums)` mangles to `turf_sum_array_int_arr`, finds the function directly without needing the fallback at all, and dispatches correctly.

### Note 6: Module search order
An extra issue is module imports were added, but they should look like this: first search in the standard library, then search in the current directory.

---

## Phase 4 — Type system correctness
> [!NOTE]
> Note 7 (typeof on conditional expressions), Note 14 (typeof on List types), and Note 3 (null values) belong here. These require the type system to be reasonably stable first, otherwise you're building on shifting ground.

### Note 3: Null values
There should be a null value in the language after so much has happened. Null can be only stored in certain types, like pointers or its derivatives.

### Note 7: typeof on conditional expressions
`typeof()` is not working on the return type of inline conditional blocks.

```turflang
// Test 2: typeof() with conditional expressions
io.printline("=== Conditional Expression Tests ===")
int x = 5
io.print("typeof(if x > 3 { 10 } else { 20 }): ")
io.printline(typeof(if x > 3 { 10 } else { 20 }))

io.print("typeof(if true { 1.5 } else { 2.5 }): ")
io.printline(typeof(if true { 1.5 } else { 2.5 }))

io.print("typeof(if false { true } else { false }): ")
io.printline(typeof(if false { true } else { false }))
```

This outputs:

```
=== Conditional Expression Tests ===
typeof(if x > 3 { 10 } else { 20 }): int
typeof(if true { 1.5 } else { 2.5 }): int
typeof(if false { true } else { false }): int
```

Which is wrong, most probably it is taking if statement itself as integer which might be the return type when it is codegenned as a 64-bit unsigned integer.

### Note 14: typeof on List types
`nested_list_typeof.tr` gives an unkown type when being asked a perfect type, should return `List<int>` or something like a "List of integer objects".

File content:

```turflang
import list

fn int main() {
    List<int> inner = List<int>()
    io.printline(typeof(inner))

    return 0
}
```

```
> ./turf tests/custom/edgy/nested_list_typeof.tr
[AliasAnalysis] running on function: List.push
[AliasAnalysis] running on function: List.pop
[AliasAnalysis] running on function: List.length
[AliasAnalysis] running on function: main
[Main] GlobalCFGs size before CallGraph: 4
[CallGraph] Functions: 0
Compiled to program.o
Successfully compiled to program

> ./program
unknown
```

The compiler might need a heavy refactoring to acheive something like this cuz of the else part of the typeof() intrinsic.

```cpp
else {
    TypeName = "unknown";
}
```

An AI given suggestion is this: `ensure_builtins_registered` Is Never Called from `LookupBuiltin`. Could be fixed in a single pass.

---

## Phase 5 — Language consistency and polish
> [!NOTE]
> Note 4 (default variable values), Note 8 (inconsistent empty-else warnings), Note 11 (better error messages), and Note 9 (the type mismatch crash prevention message) are last — they're important for usability but none of them are blocking correctness.

### Note 4: Default variable values
Allow default values in variables. Right now, parsers only allow assignment of operators, and nothing else. Allow default assignment as well for consistency.

An array is assignable, or default assigned as the default value of the type it has. A multi-dimensional array must also have default values inside.

For a value, say integer, it can have 0, double will have 0.0, and so on.

This would prevent segmentation faults and also a consistent design.

### Note 8: Inconsistent empty-else warnings
Another interesting catch I got is this:

When executing `while_break.tr` which has the contents:

```turflang
fn int main() {
    int i = 0
    int sum = 0

    while i < 100 {
        if i >= 5 {
            break
        } else {}
        sum = sum + i
        i = i + 1
    }

    io.printline(sum)
    io.printline(i)

    return 0
}
```

It generates the correct warning:

```
> ./turf tests/loops/while_break.tr
[AliasAnalysis] running on function: main
[Main] GlobalCFGs size before CallGraph: 1
[CallGraph] Functions: 0
── warning at line 8, col 11

  Warning: The 'else' branch is empty — it doesn't do anything!
  Hint: If you haven't written the code yet, add a comment like '// TODO'. Otherwise, you may be able to simplify this 'if' statement.

  8 │         } else {}
    │           ^ Here!

Compiled to program.o
Successfully compiled to program
```

But doing the same, having an empty else branch on `nested_loops.tr` with the contents:

```turflang
fn int main() {
    int total = 0
    for i in 1..5 step i += 1 {
        int j = 0
        while j < 5 {
            j = j + 1
            if j == 3 {
                continue
            } else {}
            if i == 4 {
                break
            } else {}
            total = total + (i * j)
        }
    }
    io.printline(total)

    return 0
}
```

It does not give a warning:

```
> ./turf tests/loops/nested_loops.tr
[AliasAnalysis] running on function: main
[Main] GlobalCFGs size before CallGraph: 1
[CallGraph] Functions: 0
Compiled to program.o
Successfully compiled to program
```

This could be an issue in the CFG file, or somewhere which checks for empty else branches.

### Note 9: Type mismatch crash prevention message
Another interesting issue in `verify_crash_prevention.turf`:

```
./turf tests/custom/verify_crash_prevention.turf
[Main] GlobalCFGs size before CallGraph: 0
[CallGraph] Functions: 0
── error at line 22, col 0

  Type mismatch!
  Cannot cast between string and non-string types
  Hint: Check that you're not mixing incompatible types (like assigning a string to an int variable).
```

Could be somewhere in the new update of the compiler where the user needs to declare the main() entry point and not the program.

Can't really have an error message which is good for this case. Contents:

```turflang
// This file intentionally contains semantic errors.
// Before Phase 4, compiling this would cause the compiler itself to segmentation fault.
// Now, it should cleanly report errors and exit gracefully.

fn int main() {
    // Error 1: Type mismatch
    int a = 5
    string b = "hello"
    int c = a + b 

    // Error 2: Missing return in non-void function
    fn int missing_return() {
        int x = 10
    }

    // Error 3: Array bounds error caught at compile time
    int[5] arr = [1,2,3,4,5]
    io.printline(arr[10])

    return 0
}
```

Could really also happen due to the function registering pre-pass and thus AST walk skipped every function later. Not really sure.

### Note 11: Better error messages
Need a better error message for unitialized lists.

```
> ./turf tests/custom/struct_list.tr
Error: Statements are not allowed outside of a function. Please define 'fn int main()' and move this code inside.
── error at line 7, col 12

  Expected a variable name after the type keyword.
  Hint: Variable declarations look like: int x = 5

  7 │     list<User> users
    │            ^ Here!
```

This is more robotic and less informative, could be better.
