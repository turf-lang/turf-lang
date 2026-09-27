#!/bin/bash
#
# check_boundary.sh -- STD-001's own regression coverage
# (docs/planning/ARCHITECTURE_OVERHAUL.md §8 STD-001, "boundary
# formalization"). Mechanically checks the three claims that section makes
# rather than leaving them as prose someone has to re-verify by hand:
#
#   1. No path-based privilege remains -- AGENTS.md's "No compiler
#      special-casing; ordinary .tr files" claim, and F-STD-1 (FREEZE.md
#      §6.8): a raw-pointer capability is DECLARED IN SOURCE
#      (`unsafe module;`, LANG-002), never granted by a file's location on
#      disk. `IsPathUnderLibStd()` was the mechanism this replaced; its
#      name is checked for directly, since its reappearance under any name
#      is exactly the regression this exists to catch.
#   2. The prelude's membership is specified, not just a hardcoded list --
#      `PreludeModuleNames` (src/ParserModule.cpp) must name exactly the
#      `.tr` files under `lib/std/core/`, no more, no fewer. See
#      docs/architecture/boundary.md's "Prelude module specification"
#      section for why core/ is the specification rather than an
#      incidental coincidence.
#   3. The closed intrinsic list (include/Intrinsics.h, F-STD-2) actually
#      covers every bare compiler-known name reachable from Turf source --
#      every `RegisterBuiltin(...)` name in src/Builtins.cpp, and the
#      `sizeof`/`typeof` lexer-level ones, must appear in Intrinsics.h's
#      text. This catches a FUTURE addition to g_builtins that forgets to
#      also document itself -- it does not, and cannot, catch every
#      possible name-based special case in the compiler (see
#      docs/architecture/boundary.md's "One documented exception" section
#      for the one known, pre-existing, out-of-scope exception this script
#      deliberately does not flag).
#   4. OVH-002 (docs/development/changes/OVH-002.md): no Codegen call
#      site reaches a bare `malloc` under the name `getOrCreateMallocFn()`
#      any more -- both of its former callers now GC-heap-allocate via
#      `turf_rt_alloc` with a real descriptor instead (F-MEM-1's precise
#      tracing depends on every GC-heap-bound allocation going through it).
#   5. OVH-002, `docs/architecture/boundary.md` §5: a best-effort scan for
#      `mem::alloc(sizeof(SomeType))` in `lib/std`/`showcase` where
#      `SomeType` has a `string`/collection field -- exactly the "hide a
#      GC pointer inside untraced manual memory" shape this group fixed at
#      its two known stdlib/showcase sites (json.tr, kv_store_server.tr).
#      Best-effort, not exhaustive: it only recognizes the textual
#      `mem::alloc(sizeof(...))` shape and a same-repo `struct Name { ... }`
#      declaration to check fields against -- see boundary.md for why a
#      fully general check isn't attempted.
#   6. SMP-002 / INV-GEN-2 (docs/development/changes/SMP-002.md,
#      docs/development/INVARIANTS.md): every generic-parameter query --
#      does a type contain/wrap an open `TurfGenericParamType`, substitute
#      it, unify it against a concrete type -- goes through the one shared
#      walker, include/TypeWalk.h. A bare `dynamic_cast<const
#      TurfGenericParamType` anywhere else in src/ or include/ is a hand-
#      rolled re-derivation of exactly the pattern SMP-002 unified (three of
#      the eight original instances were independently, silently
#      incomplete -- see the change record). Two kinds of exception are
#      excluded deliberately, not silently: the INV-GEN-1 assertion sites
#      (`assert(!dynamic_cast<...>)` -- a different invariant, checking that
#      NONE reaches Codegen, not deriving anything from one) and
#      `CodegenCore.cpp`, whose own separate `getExprType()` type-inference
#      engine independently reimplements this same pattern at several more
#      sites using raw, non-owning `const TurfType*` throughout (not
#      `TurfTypePtr`) -- deliberately left unmigrated this session,
#      `ARCHITECTURE_OVERHAUL.md` §25 item 4's caution against casually
#      touching that second engine. See INVARIANTS.md's INV-GEN-2 row for
#      the full account of what this does and does not cover.
#
# Usage: scripts/check_boundary.sh
# Exit:  0 = every check passed; 1 = at least one failed.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

FAILED=0
pass() { echo -e "   ${GREEN}✓${RESET} $1"; }
fail() { echo -e "   ${RED}✗${RESET} $1"; FAILED=$((FAILED + 1)); }

echo -e "${BOLD}check_boundary.sh — STD-001${RESET}\n"

# ── 1. No path-based privilege remains ──────────────────────────────────────
# A handful of doc comments still NAME IsPathUnderLibStd() -- deliberately:
# they explain what LANG-002 replaced it with and why (this project's own
# "doc comments explain WHY" convention, AGENTS.md). Only a LIVE definition
# or call site is the regression this check exists to catch, so comment-only
# lines (anything after `//` on its own line) are excluded.
echo -e "${BOLD}1. No path-based privilege${RESET}"
HITS=$(grep -rn "IsPathUnderLibStd" src/ include/ 2>/dev/null | grep -vE ':\s*(//|\*)')
if [ -n "$HITS" ]; then
  fail "IsPathUnderLibStd (the old path-based privilege check) is still LIVE code:"
  echo "$HITS" | sed 's/^/       /'
else
  pass "IsPathUnderLibStd is not live code anywhere in src/ or include/ (doc-comment mentions only)"
fi
if grep -q "bool HasRawPointerCapability" include/Parser.h 2>/dev/null; then
  pass "HasRawPointerCapability() (the declared-capability replacement) is exported"
else
  fail "HasRawPointerCapability() is not declared in include/Parser.h"
fi

# ── 2. Prelude membership is specified ──────────────────────────────────────
echo -e "\n${BOLD}2. Prelude module list matches lib/std/core/${RESET}"
CORE_FILES=$(find lib/std/core -maxdepth 1 -name '*.tr' 2>/dev/null \
             | xargs -n1 basename 2>/dev/null | sed 's/\.tr$//' | sort)
DECLARED=$(awk '/PreludeModuleNames\[\] = \{/{f=1} f{print} f && /};/{exit}' \
           src/ParserModule.cpp \
           | grep -oE '"[a-zA-Z_]+"' | tr -d '"' | sort)
if [ -z "$CORE_FILES" ]; then
  fail "lib/std/core/ has no .tr files -- cannot cross-check (did the layout move?)"
elif [ -z "$DECLARED" ]; then
  fail "could not find PreludeModuleNames[] in src/ParserModule.cpp"
elif [ "$CORE_FILES" = "$DECLARED" ]; then
  pass "PreludeModuleNames == lib/std/core/*.tr exactly ($(echo "$CORE_FILES" | wc -l) modules)"
else
  fail "PreludeModuleNames and lib/std/core/*.tr have drifted apart:"
  diff <(echo "$CORE_FILES") <(echo "$DECLARED") | sed 's/^/       /'
fi

# ── 3. Closed intrinsic list covers every bare builtin name ─────────────────
echo -e "\n${BOLD}3. Closed intrinsic list covers every registered builtin${RESET}"
BUILTIN_NAMES=$(grep -oE 'RegisterBuiltin\("[a-zA-Z_]+"' src/Builtins.cpp \
                 | grep -oE '"[a-zA-Z_]+"' | tr -d '"' | sort -u)
MISSING=""
for n in $BUILTIN_NAMES sizeof typeof; do
  grep -q -- "$n" include/Intrinsics.h || MISSING="$MISSING $n"
done
if [ -n "$MISSING" ]; then
  fail "name(s) registered as a bare intrinsic but undocumented in include/Intrinsics.h:$MISSING"
else
  pass "every RegisterBuiltin() name, plus sizeof/typeof, is documented in include/Intrinsics.h"
fi

# ── 4. getOrCreateMallocFn() has no remaining Codegen caller ────────────────
echo -e "\n${BOLD}4. No Codegen call site reaches a bare malloc via getOrCreateMallocFn()${RESET}"
MALLOC_HITS=$(grep -rn "getOrCreateMallocFn" src/Codegen*.cpp 2>/dev/null)
if [ -n "$MALLOC_HITS" ]; then
  fail "getOrCreateMallocFn() is referenced again in Codegen -- a GC-heap allocation may be silently bypassing turf_rt_alloc:"
  echo "$MALLOC_HITS" | sed 's/^/       /'
else
  pass "getOrCreateMallocFn() has no remaining definition or caller in src/Codegen*.cpp"
fi

# ── 5. mem::alloc(sizeof(SomeType)) never boxes a GC-pointer-holding type ──
echo -e "\n${BOLD}5. mem::alloc(sizeof(...)) in lib/std/showcase never boxes a string/collection field${RESET}"
BOXED_TYPES=$(grep -rohE 'mem::alloc\(sizeof\([A-Za-z_][A-Za-z0-9_]*\)\)' lib/std showcase 2>/dev/null \
              | grep -oE '\(sizeof\([A-Za-z_][A-Za-z0-9_]*\)\)' \
              | grep -oE '[A-Za-z_][A-Za-z0-9_]*' | grep -v '^sizeof$' | sort -u)
BOUNDARY5_FAILED=0
for t in $BOXED_TYPES; do
  FIELDS=$(grep -rhozE "struct[[:space:]]+$t([[:space:]]*<[^>]*>)?[[:space:]]*\{[^}]*\}" lib/std showcase 2>/dev/null \
           | tr -d '\0')
  if echo "$FIELDS" | grep -qE ':[[:space:]]*(string|List<|Map<|Set<|Stack<|Queue<)'; then
    fail "mem::alloc(sizeof($t)) boxes '$t', which has a string/collection field:"
    echo "$FIELDS" | sed 's/^/       /'
    BOUNDARY5_FAILED=1
  fi
done
if [ "$BOUNDARY5_FAILED" -eq 0 ]; then
  pass "no mem::alloc(sizeof(...)) site boxes a type with a string/collection field"
fi

# ── 6. INV-GEN-2: every generic-parameter query goes through TypeWalk ──────
echo -e "\n${BOLD}6. dynamic_cast<TurfGenericParamType> only in TypeWalk (INV-GEN-2)${RESET}"
GEN_HITS=$(grep -rn "dynamic_cast<const TurfGenericParamType" src/ include/ 2>/dev/null \
           | grep -vE ':\s*(//|\*)' \
           | grep -v "^src/TypeWalk\.cpp:" | grep -v "^include/TypeWalk\.h:" \
           | grep -v "^src/CodegenCore\.cpp:" \
           | grep -v "assert(!dynamic_cast<const TurfGenericParamType")
if [ -n "$GEN_HITS" ]; then
  fail "dynamic_cast<const TurfGenericParamType found outside TypeWalk/CodegenCore/an INV-GEN-1 assertion -- route it through TypeWalk::containsOpenParam() instead:"
  echo "$GEN_HITS" | sed 's/^/       /'
else
  pass "every dynamic_cast<const TurfGenericParamType is in TypeWalk, an INV-GEN-1 assertion, or the documented CodegenCore.cpp exception"
fi

echo
if [ $FAILED -eq 0 ]; then
  echo -e "${GREEN}${BOLD}Boundary checks clean.${RESET}"
  exit 0
fi
echo -e "${RED}${BOLD}$FAILED boundary check(s) failed.${RESET}"
exit 1
