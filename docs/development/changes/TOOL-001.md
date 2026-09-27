# TOOL-001 — Debug information (`-g` / DWARF)

| | |
|---|---|
| **Status** | landed |
| **Dates** | started 2026-09-18 · landed 2026-09-18 |
| **Commits** | `280bb0f` DIBuilder + `-g` flag + module/parameter/local debug info + `scripts/debuginfo_smoke.sh` · this record |
| **Sessions** | 1 of 1 |
| **Depends on** | ARCH-001, OPT-001 |
| **Frozen decisions relied on** | FREEZE.md §6.10 ("optimization levels ... never change observable behaviour" — this group never threads `opts.OptLevel` into any debug-info decision, see CodegenCore.cpp's `isOptimized=false` comment) |

## Why

"Can you debug the binary?" was no — there was no `DIBuilder` anywhere in
Codegen and no `-g` flag at all. A crash in a compiled Turf program gave you
an address, not a TurfLang line, function name, or the value of a local.
This also de-risks the eventual GC (MEM-002): a collector bug is far easier
to chase with frame information available.

## Language semantics

### Old behaviour

`-g` was not a recognized flag at all — `turf prog.tr -g` failed with
`Unknown option: -g`.

### New behaviour

`turf prog.tr -O0 -g -o prog` emits a binary whose object file carries
DWARF4 debug information: a compile unit for the entry file, a `DIFile`
per distinct source file actually involved (main file, and any
`lib/std/**`/user module a function was declared in), a `DISubprogram` per
function with the correct declaring file/line, `DILocalVariable` +
`llvm.dbg.declare` for every parameter and body-declared local, a
`DIBasicType`/`DIDerivedType`/`DICompositeType` for every Turf type a local
or parameter can have, and a `DILocation` on every instruction inside a
function that has debug info (via `Builder`'s "current debug location",
set once per statement — see Compiler architecture below). `gdb` can set a
breakpoint by TurfLang line, print a local by name with the correct value,
and show a backtrace with real TurfLang function names (not the internal
`turf_<module>_<name>_<paramtypes>` mangled symbol — see Known pitfalls).

Verified live (`scripts/debuginfo_smoke.sh`, and manually against
`showcase/*.tr` + `tests/08_e2e_stress/*.tr`):

```
$ gdb --batch -ex "break fixture.tr:3" -ex run -ex "print sum" -ex backtrace ./fixture
Breakpoint 1, add (a=5, b=10) at fixture.tr:3
3       return sum;
$1 = 15
#0  add (a=5, b=10) at fixture.tr:3
#1  0x0000... in main () at fixture.tr:8
```

### Is this breaking?

No. `-g` is opt-in (default off); every code path it touches
(`g_EmitDebugInfo`/`DBuilder`/`CurrentDebugScope`) is a guarded no-op when
the flag isn't passed, and `-g`'s own output was verified byte-for-byte
observably-identical to a non-`-g` build across `tests/08_e2e_stress/*.tr`
(exit code + stdout, `-O0`) — debug info never changes what a program does,
only what a debugger can see about it.

## Compiler architecture

- **Codegen (`CodegenCore.cpp`)** — new module-level state:
  `g_EmitDebugInfo`/`DBuilder`/`DebugCU`/`CurrentDebugScope` (declared in
  `Codegen.h`, defined here). `InitializeModule()` builds the `DIBuilder`,
  the two required module flags (`"Debug Info Version"`,
  `"Dwarf Version"` = **4**, not 5 — see Known pitfalls), and one
  `DICompileUnit` anchored at `MainSourceFilename`, when `g_EmitDebugInfo`
  is set. Four new shared helpers (declared in `CodegenInternal.h`, mirroring
  the existing `getExprType()`/`getLLVMType()` "one central dispatch
  function" pattern rather than a virtual per-AST-node method):
  `getOrCreateDebugFile()` (per-file `DIFile` cache, keyed like
  `AllSourceLines`), `getDebugType()` (`TurfType` → `DIType`, cached by
  mangled name, mirrors `getLLVMType()`'s own `LLVMTypeCache` idiom
  including the "register a forward-declared stub before recursing into
  fields" trick for a pointer-mediated struct cycle), `GetNodeDebugLoc()`
  (best-effort `SourceLocation` for an arbitrary `ExprAST*`, a
  `dynamic_cast` dispatch chain over every node kind that has a real
  `getLoc()`), and `SetDebugLoc()` (sets `Builder`'s current debug location
  from a `SourceLocation`, scoped to `CurrentDebugScope` — every instruction
  `Builder` creates afterward inherits it automatically until the next
  call).
- **Codegen (`CodegenFunc.cpp`)** — `FuncDefExprAST::codegen()`: builds the
  `DISubprogram` once, inside the existing `if (!TheFunc)` prototype-creation
  branch (this runs exactly once per function — see the file's own
  "double-parse wrinkle" doc comment — so there's no duplicate-subprogram
  risk across the pre-pass's two `codegen()` calls for the same function).
  `TheFunc->getSubprogram()` is read back out at body-codegen time
  regardless of which of the two calls actually built it.
  `CurrentDebugScope`/`Builder`'s debug location are saved/restored the same
  way `CurrentFunction`/`CurrentFuncReturnType` already are. Each parameter
  gets a `DILocalVariable` + `llvm.dbg.declare` right after its alloca.
  `DBuilder->finalizeSubprogram()` runs once the function's body is fully
  codegenned (resolves any outstanding forward-declared debug type a local's
  `dbg.declare` referenced).
- **Codegen (`CodegenVar.cpp`)** — `VarDeclExprAST::codegen()`: a
  `DILocalVariable` + `llvm.dbg.declare` right after the local's alloca,
  using `NameLoc` (the variable name token, not the `mutable`/`immutable`
  keyword) for the declared line.
- **Codegen (`CodegenControlFlow.cpp`)** — `BlockExprAST::codegen()`'s
  per-statement loop calls `SetDebugLoc(GetNodeDebugLoc(...))` once per
  child, right before that child's own `codegen()` — this single call site
  alone gives every *ordinary* statement inside any `{ }` body (a function
  body, an `if`/`while`/`for` body, ...) a correct line, because every
  statement-shaped node is a direct `BlockExprAST` child somewhere. The
  handful of control-flow *headers* evaluated outside a block (an
  `if`/`while`/`do-while`/`for`/`for-each`'s own condition) each set their
  own location directly at the top of their `codegen()`.
- **`main.cpp`** — `-g` flag → `CompilerOptions::DebugInfo` →
  `g_EmitDebugInfo` (set before `InitializeModule()`, which is the one call
  site that decides whether `DBuilder`/`DebugCU` get built at all).
  `DBuilder->finalize()` runs right before `verifyModule()` — the one point
  guaranteed to be after every function's codegen (pre-pass *and* main
  pass) has already run, resolving any temporary forward-declared struct
  debug type still outstanding.

Nothing in Resolver, TypeChecker, or the parser changed — this is Codegen +
driver only, as scoped.

## Runtime changes

None. `libturfrt` doesn't exist yet (RT-001 is later); this group's own
scope note ("`libturfrt` should be built with `-g` too") is recorded here
as a reminder for whichever session lands RT-001, not something to act on
now.

## Standard-library changes

None. `lib/std/**` compiles unchanged, and was swept directly (see Tests).

## New invariants

Added to `docs/development/INVARIANTS.md`:

- **INV-DEBUG-1** — `-g` never changes a program's observable behaviour
  (stdout, exit code) at any optimization level. Enforced by: Codegen only
  ever *reads* `SourceLocation`/`TurfType` data that already existed for
  other reasons (diagnostics, `getLLVMType()`) to build debug metadata —
  it never branches program logic on `g_EmitDebugInfo` — plus
  `scripts/debuginfo_smoke.sh` and a manual `-g`-vs-no-`-g` diff across
  `tests/08_e2e_stress/*.tr` (recorded below). `holds`.
- **INV-DEBUG-2** — Every object file `turf` emits with `-g` passes
  `llvm-dwarfdump --verify` with no errors. Enforced by:
  `DBuilder->finalize()` running before `verifyModule()` (resolves
  outstanding forward-declared struct types) + `"Dwarf Version"` pinned to
  **4**, not LLVM's newer DWARF5 default (see Known pitfalls for why 5
  fails on this toolchain). `holds` — verified against every
  `showcase/*.tr` program and `tests/08_e2e_stress/*.tr`.

## Removed assumptions

- **A `TurfType*` no longer has "no debug-visible representation."**
  `getDebugType()` is now a second type-mapping function alongside
  `getLLVMType()`, and the two must be kept in sync BY HAND — there's no
  structural check tying them together. Adding a new `TurfType` subclass
  (or a new primitive) without a matching `getDebugType()` branch doesn't
  fail to compile; it silently falls through to the generic
  `createUnspecifiedType(T->getName())` catch-all, which is a *plausible*,
  not obviously wrong, debug type (a debugger prints "some named thing" for
  it rather than erroring) — easy to miss in review.
- **`DW_AT_linkage_name` is not safe to set to Turf's real mangled symbol
  name.** It's tempting (Turf functions are always mangled, so it looks
  like exactly what the field is for) — don't. See Known pitfalls.
- **The compile unit's `isOptimized` flag being wired to `opts.OptLevel`
  is not a hypothetical someone might reasonably want "for accuracy" — it
  would be a real, if narrow, violation of INV-OPT-1** (Codegen must never
  see the opt level). It's hardcoded `false` on purpose; see
  `CodegenCore.cpp`'s own comment on that line before "fixing" it.
- **A file compiled with `-g` on THIS toolchain (GNU ld 2.47, `gdb` 17.2,
  glibc's own unstripped-by-distro startup objects) is not safe to assume
  DWARF5-clean.** `"Dwarf Version"` must stay 4 here — see Known pitfalls.
  This is a statement about the LINKER, not about LLVM's DWARF5 *emission*
  being wrong; a different linker (`lld`, a newer `ld.bfd`) might handle it
  fine, but this project links via whatever `cc`/`ld` the host resolves to
  (`main.cpp`'s `linkObject()`), not a pinned one.

## Tests

- **Added** `scripts/debuginfo_smoke.sh` — TOOL-001's own regression
  coverage (not a `.tr` test; `test_runner.sh` has no way to drive a
  debugger). Compiles a small fixture (`add`/`main`, a real call so a
  backtrace has two genuine frames) at `-O0 -g`, asserts
  `llvm-dwarfdump --verify` is clean, then drives `gdb --batch` asserting:
  a breakpoint set by TurfLang line actually stops there; `print <local>`
  shows the correct value; the backtrace shows the TurfLang function name
  (not the mangled symbol) with correct parameter values; the caller frame
  shows the correct TurfLang line. Gated on `gdb` being on `PATH` (exits 0,
  not 1, when absent — a CI image without a debugger installed skips this,
  it doesn't fail the whole suite over an unrelated missing tool).
- **Verified, not added as `.tr` tests** (per TOOL-001's own "Testing
  scope" — none of this is `.tr`-test-shaped):
  - `./scripts/test_runner.sh` — 624/624, unchanged, both before and after
    (default `-g`-off build; `-g` adds no new code path anything in
    `tests/` exercises).
  - Every `showcase/*.tr` program compiled at `-O0 -g` and passed
    `llvm-dwarfdump --verify` cleanly, including
    `llvm_ffi_selfhost_demo.tr` (the most FFI-dense program in the tree —
    the same one OPT-001's own record singles out as the highest-risk case
    for a different reason) and the two server/client pairs.
  - `lib/std/**/*.tr` swept directly via `--typecheck <file> -O0 -g` for
    every file — no new diagnostic anywhere. Two pre-existing failures
    (`lib/std/collections/list.tr`, `lib/std/mem/mem.tr`, both `E034`
    "struct already declared") were independently confirmed, by reverting
    to `c47044c` and re-running the identical command, to be **pre-existing
    baseline behaviour with `-g` played no part in** (a direct-compile-only
    artifact of the double-parse pre-pass re-registering a struct-declaring
    file's own top-level struct twice within one `GlobalSymbolTable`
    session — the same class of hazard the six *prelude* modules were
    fixed for, just never extended to non-prelude struct-declaring
    modules). Out of scope for TOOL-001 (§25 item 3's spirit: not
    something to fix opportunistically while implementing something else)
    — worth a `27.2`/`D`-style entry in a future planning pass, not fixed
    here.
  - `tests/08_e2e_stress/*.tr` compiled and run at `-O0`/`-O0 -g` in pairs;
    stdout and exit code diffed byte-for-byte identical for every program
    (INV-DEBUG-1's own live evidence).
  - `-g` combined with every `-O` level (`-O0`..`-O3`/`-Os`) against
    `tests/08_e2e_stress/01_ackermann.tr`: compiles, runs (exit 0), and
    `llvm-dwarfdump --verify` stays clean at every level.
  - `--emit-llvm -g` confirmed to still work (produces `.ll` text
    containing real `DISubprogram`/`!dbg` metadata) — this mode skips
    `emitObjectFile()` entirely, so it's a genuinely separate code path
    from the `llvm-dwarfdump`-verified object-emission path above.
  - The two "TurfLang-specific wrinkles" §8 TOOL-001 named up front, both
    confirmed live rather than assumed: (1) the double-parse pre-pass is
    where every function BODY actually gets codegenned (main pass only
    codegens non-function top-level statements), so that's the only place
    debug metadata for a body ever gets attached — confirmed correct
    (parameter values/locals print correctly) since there is no other
    code path that could have produced them; (2) an imported module's own
    function (`io::printline`, declared in `lib/std/io/io.tr`) carries
    `DW_AT_decl_file` pointing at `lib/std/io/io.tr`, not the entry file —
    confirmed via `llvm-dwarfdump --debug-info` on the compiled binary.

## Known pitfalls

- **`"Dwarf Version"` must stay 4, not LLVM's own DWARF5 default, on this
  toolchain.** Confirmed live: DWARF5 (`addModuleFlag(..., "Dwarf Version",
  5)`) produces an object file that itself looks fine, but the FINAL LINKED
  BINARY fails `llvm-dwarfdump --verify` with ~30 `.debug_str_offsets`/
  `.debug_names` "invalid string offset" errors — reproduced on a
  single-function, zero-import fixture (rules out "many files/subprograms"
  as the cause), and the errors vanish completely switching to
  `"Dwarf Version"` 4 with no other change. This is a `GNU ld` (2.47 here)
  DWARF5 string-offsets-table linking limitation, not an LLVM emission bug
  or a TurfLang Codegen bug — `llvm-dwarfdump --verify` on the pre-link
  `.o` was not checked directly (the `.o` is deleted after a successful
  link, see `main.cpp`'s `std::remove(ObjFile...)`), but the fully
  reproducible "5 always fails, 4 always passes, nothing else changed"
  result is conclusive enough on its own. **Before ever raising this back
  to 5** (a real future temptation — DWARF5 has genuine advantages), test
  against whatever linker CI/the target platform actually uses; this may
  be fixed in a newer `ld.bfd`, or a non-issue under `lld`/`gold`/macOS's
  linker entirely.
- **Do not set `DW_AT_linkage_name` to Turf's real mangled symbol.**
  Confirmed live: `gdb` 17.2 prefers `DW_AT_linkage_name` over `DW_AT_name`
  for frame/breakpoint/backtrace display whenever both are present on a
  `DW_LANG_C99` subprogram — passing `MangledName` (e.g.
  `"turf_add_int_int"`) as `createFunction()`'s `LinkageName` argument
  made every breakpoint hit and backtrace line show the raw mangled
  symbol instead of the TurfLang name (`add`), exactly backwards from this
  group's own "show a backtrace with TurfLang function names" completion
  criterion. Fixed by passing an empty `LinkageName` — the low_pc/high_pc
  PC range LLVM emits for the subprogram already comes directly from
  `TheFunc` itself (`TheFunc->setSubprogram(SP)`), so a distinct linkage
  name buys nothing for breakpoint/backtrace correctness on this language
  (no real C++-style mangling scheme for `gdb` to demangle) and actively
  hurts it. If a future session ever wants `break turf_add_int_int`-by-
  mangled-name support specifically, that's a real, separate, deliberate
  trade-off to make — not a default to fall back into.
- **`TypeSize`, not `uint64_t`, is what `DataLayout::getTypeSizeInBits()`/
  `StructLayout::getElementOffsetInBits()`/`getSizeInBits()` actually
  return** in this LLVM version — an implicit `operator ScalarTy()`
  conversion exists but is explicitly discouraged in the header's own
  comments (and will `reportFatalInternalError()` at runtime if ever asked
  to implicitly convert a *scalable* size, which none of these calls can
  produce today, but a future SIMD/vector Turf type could). `getDebugType()`
  calls `.getFixedValue()` explicitly everywhere rather than relying on the
  implicit conversion.
- **`getDebugType()`'s struct branch is lazy and lives in `CodegenCore.cpp`,
  not `CodegenStruct.cpp`'s `StructDeclExprAST::codegen()`** — a deliberate
  deviation from §8 TOOL-001's own "Relevant files" list, which names
  `CodegenStruct.cpp` for composite types. Building it lazily, from the
  already-registered `TurfStructType` + the already-`setBody()`'d
  `llvm::StructType`'s real `DataLayout` offsets, needed zero new hook in
  struct-declaration codegen and mirrors `getLLVMType()`'s own
  "register-the-stub-before-recursing" idiom exactly (same cycle-breaking
  trick, same file). If a future session moves it, the cycle-breaking
  stub-registration order (`DebugTypeCache[Key] = Stub;` BEFORE resolving
  any field's own debug type) is the one invariant that must survive the
  move.
- **Local-variable debug scoping is flat, not block-nested.** Every local
  in a function uses `CurrentDebugScope` == that function's own
  `DISubprogram` directly — there is no `DILexicalBlock` per `{ }`. Correct
  for this group's own completion criteria (breakpoint, backtrace, print a
  local by name), but means two same-named locals in sibling blocks that
  shadow each other will both show up attached to the same scope in the
  debug info (gdb still resolves `print x` to whichever `NamedValues`/
  `GlobalSymbolTable` entry Codegen itself would use at that point, since
  that's a separate, already-correct mechanism — this is purely about what
  a debugger's own *static* scope view would show if it walked the DWARF
  tree directly, e.g. `info scope`). Fixing this is a real, scoped,
  separate change (a `DILexicalBlock` push/pop around `BlockExprAST`'s own
  scope enter/exit, which already exists at the `GlobalSymbolTable` level
  and would just need a debug-info-side mirror) — not done here.
- **Only `int`/`int32`/`double`/`bool`/`char`/`string`/pointers/fixed
  arrays/structs/enums have a real, specific `DIType`.** A generic type
  parameter, a first-class function value, and an `error`/`!T` value all
  fall through to `createUnspecifiedType(T->getName())` — a debugger can
  still print SOME representation (the raw pointer/i64 bits), but not a
  richly-typed one. Not needed for this group's own completion criteria;
  worth revisiting after TYPE-002 (monomorphization) removes type erasure,
  since a monomorphized generic instantiation would have a REAL concrete
  type to describe instead of an erased pointer.

## Handbook impact

| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
| `docs/reference/cli_and_diagnostics.md` | No `-g` flag documented at all (the reference is paused — see `docs/development/handbook-sync/PENDING.md`, new TOOL-001 row added there rather than edited in directly). | Add `-g` next to OPT-001's own still-pending `-O` row: what it does, that it's off by default, the `-O0 -g` pairing recommendation, and that `-g` never changes program behavior (INV-DEBUG-1) — only debuggability. |

### Examples that became invalid

None — no existing reference example used `-g` or referenced its absence.

### Examples that should be added

A `gdb`/breakpoint walkthrough using a fixture close to
`scripts/debuginfo_smoke.sh`'s own (a two-function call, a local, a
parameter) once `cli_and_diagnostics.md` resumes.

### Sections that must NOT change

Every existing CLI flag's own documentation (`--emit-llvm`, `--smart`,
`--dump-*`, `-O*`, `--target`/`-mcpu`) is unaffected — `-g` composes with
all of them (verified live for `--emit-llvm -g` and every `-O` level; the
`--dump-*`/`--resolve`/`--typecheck` family don't reach Codegen at all, so
they're trivially unaffected).

## Unresolved questions

None raised this session — every frozen decision this group touched
(FREEZE.md §6.10) was followed as written, with no interpretation dispute.

## Related records

- OPT-001 — prerequisite (`-O0` existing is what makes `-g` useful; this
  group's own smoke test and manual sweep both exercise every `-O` level
  `-g` combines with).
- ARCH-001 — prerequisite (this record itself, and the session protocol it
  was written under).
