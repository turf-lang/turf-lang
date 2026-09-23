# LANG-001 — Operator system completion

| | |
|---|---|
| **Status** | landed |
| **Dates** | started 2026-09-23 · landed 2026-09-23 |
| **Commits** | `00bd327` (compiler core), `16f9b28` (stdlib), `243f426` (tests) |
| **Sessions** | 1 of 1 (S15) |
| **Depends on** | TYPE-002 (landed 2026-09-22) |
| **Frozen decisions relied on** | F-OP-1, F-OP-2, F-OP-3, F-OP-4 |

## Why

Before this session, exactly ten binary operators were overloadable
(`+ - * / == != < > <= >=`), unary operator overloading was explicitly
rejected ("Unary operator overloading is not supported yet"), `[]`/`[]=`
had no overload form at all, and compound assignment (`+=` etc.) did not
consult operator overloads — deliberately deferred by the original
STRING_UFCS_OPERATOR_CHAR_RFC.md build order, now unblocked by TYPE-002's
monomorphization (a monomorphized `Map<string,int>` gives `K` the concrete
type `string`, so exact-match operator resolution actually has something to
match against inside a generic body — see §13.7). This session closes that
gap: the binary set gains `% ** & | ^ << >>`; unary `- ! ~` become
overloadable; `operator[]`/`operator[]=` are added; compound assignment and
postfix `++`/`--` are wired into operator resolution; `List<int>`/
`Map<string, int>`/`Set<int>` gain `operator[]`(`=`) as the better form of
`LIMITATIONS.md` §4's `get`/`set` request.

## Language semantics

### Old behaviour

```turflang
operator &(a: Vec2, b: Vec2): Vec2 { ... }      // parse error: not in the binary whitelist
operator -(a: Vec2): Vec2 { ... }               // parse error: "Unary operator overloading is not supported yet"
operator [](a: List<int>, i: int): int { ... }  // no such syntax at all
mut s: string = "a"; s += "b";                  // compile error (fail_string_compound_assign.tr)
```

### New behaviour

- Binary: `+ - * / % ** & | ^ << >> == != < > <= >=` (was: `+ - * / == != < > <= >=`).
- Unary: `- ! ~` (arity, not a separate spelling, distinguishes `-`'s unary
  form from its binary form — `operatorFunctionName()` returns `"operator-"`
  for both; the ordinary arity-aware overload map does the rest).
- `operator[](self_ty, index_ty): ElemType` (read) and
  `operator[]=(self_ty, index_ty, value_ty): void` (write) — dedicated
  syntax, not spelling-based (no single Lexer token to switch `[`/`]`/`=`
  on the way the other operators' single tokens work).
- The self operand of `operator[]`/`operator[]=` (and ONLY that operand, of
  that operator) may be declared `&T` and still match a bare `T` call site
  — the sole relaxation of F-OP-1's exact-match rule anywhere in the system
  (`operatorSelfOperandMatches()`, `TypeCheckerCall.cpp`). Needed because
  `operator[]=` is useless as a mutation mechanism if self is passed by
  value; a `.`-method's own implicit self gets an equivalent auto-address-of
  exemption for free (§6.6), but `operator` declarations are never
  `.`-methods (see `ParseOperatorDecl()`'s own doc comment), so they don't
  inherit it automatically.
- Compound assignment desugars for real now (F-OP-2): `a op= b` picks up
  `operator op` via exact match against `(LHSType, RHSType)`, still
  evaluating the LHS location exactly once (no literal `a = a op b` AST
  rewrite — see Compiler architecture below for why).
- Postfix `++`/`--` (D21's decision, now made and recorded): picks up
  `operator+`/`operator-` matched against `(OperandType, int)`, same as
  prefix's own implicit `+ 1`/`- 1`. Prefix itself needed no new code — it
  already desugars to `x = x + 1` at parse time and so already inherited
  any newly-added `operator+` automatically.
- New orphan rule (§13.4): at least one operand must be a struct/enum
  declared in the SAME module as the operator. `OperatorOrphanRuleError`
  (E080).
- `operator!` (unary) takes priority over the `!x` → `x != true` builtin
  desugaring for any type that declares it — see Compiler architecture
  below for the mechanism, which deliberately does NOT touch the
  desugaring's existing runtime semantics for `bool`/numeric operands.
- `List<int>` gains `operator[]`/`operator[]=`; `Map<string, int>` gains
  `operator[]`/`operator[]=` (self by reference — `put()` can reallocate);
  `Set<int>` gains READ-ONLY `operator[]` only (no `operator[]=` — would let
  a caller silently violate the set's uniqueness invariant). All three are
  concrete instantiations, not genuinely generic over their type
  parameter(s) — see Removed assumptions.
- `List<T>`/`Map<K,V>`/`Set<T>`'s public `data`/`keys`/`values`/`items`
  fields are UNCHANGED (kept, per the group's own scope) — `operator[]` is
  additive, not a replacement.

### Is this breaking?

No. Purely additive: every previously-legal program still compiles and
behaves identically (verified — full suite green before and after, byte-
identical pass/fail set except the three tests that were EXPECTED to flip,
see Tests below). The orphan rule could in principle reject a pre-existing
declaration; none exists in `lib/std` (checked directly — the only existing
operator overloads anywhere in the tree are `strings.tr`'s `string`-only
ones, which have no struct/enum operand at all and so are unaffected by the
orphan rule by construction, see Known pitfalls).

## Compiler architecture

- **Lexer**: no changes — every needed token (`%`, `TOK_POWER`, `&`, `|`,
  `^`, `TOK_LSHIFT`, `TOK_RSHIFT`, `!`, `~`) already existed for the
  built-in operators; operator declarations just needed to accept tokens
  they'd previously rejected.
- **Parser** (`ParserDecl.cpp`, `ParseOperatorDecl()`): dispatches on `[`
  (for `operator[]`/`operator[]=`, disambiguated by whether `]` is followed
  by `TOK_ASSIGN` — NOT the ASCII `'='`, a real bug hit and fixed live
  during this session, see Known pitfalls) or on
  `isOverloadableBinaryOp(CurTok) || isOverloadableUnaryOp(CurTok)`
  otherwise. Arity validation after the parameter list is now
  operator-shape-dependent (2 for index-get, 3 for index-set, 1-or-2 for a
  spelling capable of both forms, 1 or 2 exclusively otherwise) instead of
  always-exactly-2, with a specific message naming which arities that
  symbol actually supports. The old blanket "Unary operator overloading is
  not supported yet" message is deleted, not merely unreachable.
- **AST** (`ASTExpr.h`, `ASTAccess.h`): `UnaryExprAST` gains
  `ResolvedOperatorCandidates`/`ResolvedOperatorTarget` (mirrors
  `BinaryExprAST`'s existing fields). `PostfixExprAST` and
  `CompoundAssignmentExprAST` gain the same pair, independently (each needs
  its own candidate set, keyed differently — postfix always against
  `operator+`/`operator-`, compound assignment against
  `operatorFunctionName(Opcode)`). `ArrayAccessExprAST` gains TWO pairs,
  `ResolvedIndexGetCandidates/Target` and `ResolvedIndexSetCandidates/
  Target` — both gathered unconditionally by the Resolver regardless of
  whether a given access turns out to be a read or a write (that's decided
  later, by whichever TypeChecker visitor actually consults them).
  `BinaryExprAST` gains a THIRD, separate pair,
  `ResolvedNotOperatorCandidates/Target`, used ONLY when `IsDesugaredNot` is
  true — see the `operator!` bullet below for why this is kept apart from
  the node's ordinary (binary `operator!=`) candidates.
- **Resolver** (`ResolverExpr.cpp`, `ResolverDecl.cpp`, `ResolverCore.cpp`):
  `visitUnary`/`visitPostfixExpr`/`visitCompoundAssignment`/
  `visitArrayAccess` all gather candidates the same way `visitBinary`
  already did (self-file, then every other known module — Option A
  visibility, unchanged). `validateOperatorDecl()` is generalized from
  "always exactly 2 params, both-builtin check only" to: collect the
  relevant operand(s) based on shape (self only, for index ops; all
  params, otherwise), run the all-primitive rejection (skipped for index
  ops — no second operand to pair against), then the new orphan rule.
- **TypeChecker** (`TypeCheckerExpr.cpp`, `TypeCheckerCall.cpp`): each of
  the five sites above gets an overload-resolution block that mirrors
  `visitBinary`'s existing one (exact match, ambiguity check, early return
  on a hit). Three NEW scope-boundary diagnostics were added, all raised
  as clean type errors rather than reaching Codegen: (1) `operator[]=` not
  found for a struct/enum-shaped base (`visitAssignment`); (2) compound
  assignment through `operator[]` (`visitCompoundAssignment`); (3) postfix
  `++`/`--` through `operator[]` (`visitPostfixExpr`) — see Known pitfalls
  for the real bug these three caught mid-session
  (`GenericType`, not `TurfStructType`/`TurfEnumType`, is what a
  monomorphized `List<int>`/`Set<int>` actually IS, so the first attempt at
  these three checks silently missed every stdlib collection type).
- **Codegen** (`CodegenExpr.cpp`, `CodegenArray.cpp`, `CodegenVar.cpp`,
  `CodegenCore.cpp`): each resolved target becomes a direct
  `Builder->CreateCall()` to its mangled name, mirroring
  `BinaryExprAST::codegen()`'s existing dispatch. `operator[]`
  (read)/`operator[]=` (write) each check whether the matched overload's
  self parameter is a reference and pass `codegenLValue()` (an address) or
  `codegen()` (a value) accordingly. `getExprType()` (`CodegenCore.cpp`,
  the SEPARATE, LLVM-type-driven inference engine Codegen still uses in
  several places, §9.7/INV-TYPE-2's own documented duality) gained an
  `operator[]`-aware branch too — needed because `AssignmentExprAST::
  codegen()` calls it directly and it previously had no idea a struct base
  could have a type at all.
- **`operator!` mechanism** (deliberately NOT a new `UnaryExprAST('!', ...)`
  node): `!x` in source has ALWAYS desugared to `BinaryExprAST(TOK_NEQ, x,
  true)` at parse time (`ParserCore.cpp`), marked `IsDesugaredNot` so
  `Lint.cpp` can tell it apart from a literal `x != true`. Switching `!` to
  parse as a real unary node was considered and rejected: the parser has no
  type information yet, so it cannot decide at parse time whether a given
  `!x` will want `operator!` or the builtin comparison, and reimplementing
  the builtin comparison's exact runtime semantics (via
  `CommonType`/`CastBoth`/`ICmpNE`, all inside `BinaryExprAST::codegen()`)
  from scratch for a new node risked a silent behavior change for every
  existing `!` in the language. Instead, unary `operator!` resolution is
  layered ONTO the existing desugared shape: `Resolver::visitBinary()`
  additionally gathers `operator!` candidates (arity 1) against the LHS
  whenever `IsDesugaredNot` is true; `TypeChecker::visitBinary()` checks
  those FIRST, before its ordinary `operator!=`/builtin path; `BinaryExprAST
  ::codegen()` dispatches to a 1-argument call (passing only `L`, never the
  synthetic `true` literal) when that target is set. A type that never
  declares `operator!` (i.e. everything in `lib/std` and every existing
  test) takes the exact same code path it always did.

## Runtime changes
None.

## Standard-library changes

- `lib/std/collections/list.tr`: `operator[]`/`operator[]=` for `List<int>`
  (bounds-checked, unlike the raw `self.data[i]` reads elsewhere in this
  same file — `l[i]` reads like array indexing to a caller, and array
  indexing IS bounds-checked everywhere else in the language). Self by
  value for the write form (sufficient — `data` is a heap pointer, a
  by-value copy still shares the backing buffer, same reasoning
  `list_sort()`'s own doc comment already gives for taking `List<int>` by
  value).
- `lib/std/collections/map.tr`: `operator[]`/`operator[]=` for
  `Map<string, int>`, wrapping `get()`/`put()`. Self BY REFERENCE
  (`&Map<string, int>`) for the write form — `put()` can reallocate/grow
  the map's own fields, which a by-value copy would silently lose.
- `lib/std/collections/set.tr`: READ-ONLY `operator[]` for `Set<int>`
  (positional, matching `add()`'s slot order until a `remove()` reorders
  it via swap-with-last — same caveat `Set.remove()`'s own doc comment
  already documents). Deliberately no `operator[]=`.
- All three specialize on ONE concrete instantiation rather than being
  genuinely generic (`List<T>`/`Map<K,V>`/`Set<T>`) — see Removed
  assumptions.

## New invariants

Added to `INVARIANTS.md`:
- **INV-OP-1**: operator overload resolution is exact-match only, with
  exactly one documented, narrowly-scoped exception (`operator[]`/
  `operator[]=`'s self operand may match by reference).
- **INV-OP-2**: an operator declaration must own at least one of its
  operands (the orphan rule).

## Removed assumptions

- **"An `operator` parameter's type is always the resolved
  `TurfStructType`/`TurfEnumType` by the time `validateOperatorDecl()` runs"
  — false.** `registerFunctionSignature()` (this function's only caller)
  runs during Pass 1 HOISTING, before type resolution — a struct/enum
  operand is still a bare `NamedType("A")` at this point, and `NamedType`
  overrides neither `isStruct()` nor `isEnum()`. The orphan rule's first
  implementation checked `T->isStruct() || T->isEnum()` directly and
  silently let a genuine cross-module orphan declaration through (found via
  a deliberately-constructed regression test, not by inspection). Fixed by
  checking the type registry BY NAME (`isKnownStruct(Name) ||
  isEnumName(Name)`) in addition to the resolved-class check.
- **"A struct-shaped `ArrayAccessExprAST` base is always a
  `TurfStructType`/enum, so `T->isStruct() || T->isEnum()` is the right
  gate for 'this access can only ever go through operator[]'" — also
  false, independently, for a DIFFERENT reason.** A monomorphized generic
  instantiation (`List<int>`, `Map<string,int>`, `Set<int>`) is a
  `GenericType` (`Types.h`), a SIBLING of `TurfStructType`, not a subclass
  — `isStruct()` is unconditionally false for one. All three new
  `operator[]`-scope-boundary diagnostics (operator[]= not found, compound
  assignment through operator[], postfix through operator[]) originally
  used this same narrow check and silently missed every stdlib collection
  type, falling through to Codegen's "Internal error: Base is not an array
  or pointer" ICE instead of the clean diagnostic they exist to produce —
  caught live by `fail_stdlib_set_no_index_set.tr`. Fixed by replacing the
  check with `needsOperatorIndex()`: true for anything that is NOT one of
  the three builtin-indexable kinds (array/pointer/string), regardless of
  which specific non-indexable class it is.
- **"`self` is available as an ordinary parameter name in a plain
  function"** — false. `self` is a reserved keyword (`TOK_SELF`), used
  automatically for a real `.`-method's implicit receiver; an `operator`
  declaration's self/index/value parameters need a different spelling. Not
  a bug, just a naming trap the first draft of every `operator[]`/
  `operator[]=` test in this session hit (see Known pitfalls).
- **"Any struct declaring `operator[]=` can pass its self parameter by
  value and still have the write persist"** — only true when the struct's
  mutation happens entirely through an ALREADY-heap-allocated field it
  merely writes into (`List<int>`'s `data`), never when the write needs to
  update the struct's OWN fields (`Map`'s `len`/`cap`/`keys`/`values`,
  which `put()` can reallocate). `operatorSelfOperandMatches()`'s `&T`
  relaxation exists specifically so the second case has a correct option.

## Tests

`tests/18_operator_overload/` — 21 new/changed files:
- **Removed** (behavior they documented as rejected is now accepted, so
  they would otherwise silently start passing as an unintended false
  negative): `fail_operator_bitwise.tr`, `fail_operator_unary.tr`,
  `fail_string_compound_assign.tr`.
- **Added, positive**: `06_operator_bitwise.tr` (`& | ^` on a struct),
  `07_operator_shift_power_percent.tr` (`<< >> ** %`),
  `08_operator_unary.tr` (unary `- ! ~`, including `-`'s unary-vs-binary
  coexistence and `operator!` taking priority over the builtin
  desugaring), `09_string_compound_assign.tr` (`s += t` now calling
  `operator+`), `10_operator_index_get_set.tr` (`operator[]`/`[]=` on a
  plain struct, both self-by-value and self-by-reference), `11_postfix_
  and_compound_assign.tr` (`+= -=`, prefix, and postfix all reaching the
  same `operator+`/`operator-`), `12_stdlib_list_index.tr`,
  `13_stdlib_map_index.tr` (including growing a map FROM EMPTY purely
  through `[]=`, proving the by-reference self actually propagates
  reallocation), `14_stdlib_set_index.tr`.
- **Added, negative**: `fail_operator_builtin_pair_bitwise.tr`,
  `fail_operator_unary_arity_mismatch.tr`, `fail_operator_orphan_rule.tr`
  (plus its two companion modules, `orphan_a_module.tr`/
  `orphan_b_module.tr`), `fail_operator_index_not_declared.tr`,
  `fail_operator_compound_assign_via_index.tr`,
  `fail_operator_postfix_via_index.tr`, `fail_stdlib_set_no_index_set.tr`.

Full suite: 702 → 715 (702 baseline, −3 removed, +12 net after the 3
flipped fail tests' replacements and 9 genuinely new files), all green.
`lib/std/**` and `showcase/**` swept by direct compile — `map.tr`/`set.tr`
and all 6 prelude modules clean; `list.tr`/`mem.tr` hit a PRE-EXISTING
(confirmed via `git stash`, unrelated to this session) self-compile
duplicate-`struct` failure from D16's documented circular-import
limitation — not a regression, and both are exercised correctly via the
normal `import list::List;` path (verified live, see
`12_stdlib_list_index.tr`). `*_module.tr` naming sweep run in both
directions — the only hit (`export_qualified_cross_module.tr`) is
pre-existing and untouched by this session.

## Known pitfalls

- **`operator[]=`'s `]` `=` sequence is TWO tokens, and `=` is
  `TOK_ASSIGN` (-4), not the ASCII `'='` (61).** The first implementation
  of the `[`/`]`/`=` parser branch checked `CurTok == '='` directly and
  silently always took the index-GET path, even when the source said
  `operator[]=`, producing a confusing cascading "Expected '(' after
  'operator[]'" error at the `(` that followed the `=`. Any future
  addition to `ParseOperatorDecl()` involving a bare `=` needs
  `TOK_ASSIGN`, not the character literal.
- **`self` cannot be a plain parameter's name.** It's a keyword
  (`TOK_SELF`). Every `operator[]`/`operator[]=` example in this session
  (and this record) uses a different name for the receiver parameter for
  exactly this reason — writing `operator [](self: T, i: int): E` fails
  to parse with "Expected a parameter name."
- **`isStruct()`/`isEnum()` are the wrong gate for "this is a user-defined
  aggregate type" in TWO different, independent places** (both found live
  during this session, both described fully under Removed assumptions
  above): a still-unresolved Pass-1 operand (`NamedType`, in the Resolver)
  and a monomorphized generic instantiation (`GenericType`, in the
  TypeChecker). Neither predicate covers those. Check the type registry by
  name for the former; use `needsOperatorIndex()` (or equivalent
  "not one of the builtin-indexable kinds") for the latter.
- **Compound assignment and postfix `++`/`--` THROUGH `operator[]`
  (`box[i] += 1`, `box[i]++`) are explicitly NOT implemented** — this is a
  deliberate scope boundary, not an oversight, but it is easy for a future
  session to assume it "just works" since plain `+=`/postfix on an
  ordinary struct DOES now route through operator overloads. It would need
  a read (`operator[]`) then write (`operator[]=`) chained together, a
  genuinely separate feature. Both shapes are caught with a clean
  diagnostic (see Tests) rather than reaching Codegen's
  `ArrayAccessExprAST::codegenLValue()` "Internal error: Base is not an
  array or pointer" ICE — but a future session extending operator[] should
  know this gap exists and is intentional, not silently reintroduce it by
  "fixing" one of the two guard checks without also building the
  read-modify-write chain.
- **Prefix `++`/`--` only accepts a bare identifier operand
  (`++box[0]` is a pre-existing "Expected identifier after prefix ++ or
  --" PARSE error, for a builtin array too, not just an `operator[]`-based
  one)** — confirmed pre-existing (unrelated to this session — the same
  restriction applies to `++arr[0]` on a plain `int[3]`) via direct
  testing, not fixed here (out of scope; this session's postfix/compound-
  assignment work is about OPERATOR RESOLUTION, not about widening what
  prefix's own parser accepts as an operand).
- **A Lint false positive ("assigned but the value is never read (dead
  store)") fires on a plain `mut x: int = 5; x += 1;` with NO operators
  involved at all** — confirmed pre-existing (reproduces without any
  `operator` declaration in scope), not introduced or worsened by this
  session's compound-assignment changes. Left as-is; a Lint.cpp fix is a
  separate concern.
- **`getExprType()` (`CodegenCore.cpp`) is a SEPARATE type-inference engine
  from the TypeChecker's `NodeTypes_`** (§9.7/INV-TYPE-2's documented
  duality) and needed its own, independent `operator[]`-awareness fix
  (`AssignmentExprAST::codegen()` calls it directly, not `getType()`) — a
  future operator-related Codegen change should check whether this
  function needs a parallel update, the same way this session's
  `visitArrayAccess()`/`getExprType()` pair did.

## Handbook impact

| File / chapter | What is now wrong | Suggested replacement |
|---|---|---|
| `docs/reference/generics_and_operators.md` §"Operator Overloading" | "Only ten binary symbols", the "no indexing"/"no unary" `[!NOTE]`, the "compound assignment isn't routed through operators" gotcha | Queued in `handbook-sync/PENDING.md` (not edited directly — `docs/reference/` is paused, per that file's own header and the unbroken precedent of every prior landed session) |
| `docs/reference/operators.md` §"Logical", §"Compound assignment", §"Postfix" | `!x` always desugars; bitwise compound forms are unconditionally int-only; postfix is never desugared/never overload-aware | Queued in `handbook-sync/PENDING.md`, same reasoning |
| Handbook ch12 (operator overloading section) | Presumably mirrors the same three claims | Queued in `handbook-sync/PENDING.md` — not independently verified which subsection, handbook is paused |

### Examples that became invalid
None — every existing worked example in `docs/reference/generics_and_operators.md`/`operators.md` remains correct; only the ENUMERATED sets and a few standalone notes/gotchas are now wrong (see PENDING.md rows for the precise diff).

### Examples that should be added
See the `handbook-sync/PENDING.md` rows — each names specific new examples owed (unary/index/orphan-rule/postfix worked examples), with `tests/18_operator_overload/06`–`14` as ready-to-adapt source material.

### Sections that must NOT change
`docs/reference/generics_and_operators.md`'s "Generics" half (everything before "## Operator Overloading") — untouched by this group, still accurate.

## Unresolved questions
None raised against a frozen decision this session.

## Related records
[TYPE-002](TYPE-002.md) (prerequisite — monomorphization is why this group could sequence after it, §13's own "Why it must happen here").
