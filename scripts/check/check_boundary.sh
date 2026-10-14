#!/bin/bash
#
# check_boundary.sh -- mechanical checks of the line between the compiler,
# the runtime and the standard library (docs/architecture/boundary.md), so
# those rules do not rest on prose someone has to re-verify by hand:
#
#   1. No privilege comes from a file's location. The raw-pointer capability
#      is declared in source (`unsafe module;`, HasRawPointerCapability()),
#      never granted for being under lib/std (F-STD-1, FREEZE.md §6.8). A
#      live IsPathUnderLibStd(), the path-based check that rule rules out, is
#      looked for by name.
#   2. The prelude is specified by lib/std/core/: `PreludeModuleNames`
#      (src/frontend/ParserModule.cpp) names exactly the `.tr` files there
#      (boundary.md, "Prelude module specification").
#   3. The closed intrinsic list (src/sema/Intrinsics.h, F-STD-2) mentions
#      every name registered in src/sema/Builtins.cpp, plus the lexer-level
#      `sizeof`/`typeof`. It catches a builtin added without documenting
#      itself; it cannot catch every name-based special case in the compiler
#      (boundary.md, "One documented exception").
#   4. Codegen allocates GC-heap values through `turf_rt_alloc` with a type
#      descriptor, which precise tracing depends on (F-MEM-1): the old bare
#      `malloc` helper, getOrCreateMallocFn(), has no definition or caller in
#      src/codegen/.
#   5. A best-effort scan of lib/std and showcase for
#      `mem::alloc(sizeof(SomeType))` where `SomeType` has a string or
#      collection field: a GC pointer hidden in manual memory the collector
#      never scans (boundary.md §5). It knows only that textual shape and
#      same-repo `struct` declarations; boundary.md says why a general check
#      is not attempted.
#   6. Every generic-parameter query (does a type contain an open
#      TurfGenericParamType, substitute it, unify it) goes through the shared
#      walker in src/sema/TypeWalk.h (INV-GEN-2). A bare
#      `dynamic_cast<const TurfGenericParamType` elsewhere in src/ or include/
#      is a hand-rolled copy of it. Excluded: the INV-GEN-1 assertions
#      (`assert(!dynamic_cast<...>)`, which check that none reaches Codegen)
#      and CodegenCore.cpp, whose getExprType() has its own type inference
#      over raw `const TurfType*` (INVARIANTS.md, INV-GEN-2).
#   7. The front end's state save/restore hooks are called only by the query
#      facade, src/tooling/AnalysisFacade.cpp (INV-API-1).
#
# Usage: scripts/check/check_boundary.sh
# Exit:  0 = every check passed; 1 = at least one failed.

set -u
cd "$(dirname "$0")/../.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

FAILED=0
pass() { echo -e "   ${GREEN}✓${RESET} $1"; }
fail() { echo -e "   ${RED}✗${RESET} $1"; FAILED=$((FAILED + 1)); }

echo -e "${BOLD}check_boundary.sh — STD-001${RESET}\n"

# ── 1. No path-based privilege remains ──────────────────────────────────────
# Comments may name IsPathUnderLibStd() to explain the rule; only a live
# definition or call is the regression, so comment lines are excluded.
echo -e "${BOLD}1. No path-based privilege${RESET}"
HITS=$(grep -rn "IsPathUnderLibStd" src/ include/ 2>/dev/null | grep -vE ':\s*(//|\*)')
if [ -n "$HITS" ]; then
  fail "IsPathUnderLibStd (the old path-based privilege check) is still LIVE code:"
  echo "$HITS" | sed 's/^/       /'
else
  pass "IsPathUnderLibStd is not live code anywhere in src/ or include/ (doc-comment mentions only)"
fi
if grep -q "bool HasRawPointerCapability" src/frontend/Parser.h 2>/dev/null; then
  pass "HasRawPointerCapability() (the declared-capability replacement) is exported"
else
  fail "HasRawPointerCapability() is not declared in src/frontend/Parser.h"
fi

# ── 2. Prelude membership is specified ──────────────────────────────────────
echo -e "\n${BOLD}2. Prelude module list matches lib/std/core/${RESET}"
CORE_FILES=$(find lib/std/core -maxdepth 1 -name '*.tr' 2>/dev/null \
             | xargs -n1 basename 2>/dev/null | sed 's/\.tr$//' | sort)
DECLARED=$(awk '/PreludeModuleNames\[\] = \{/{f=1} f{print} f && /};/{exit}' \
           src/frontend/ParserModule.cpp \
           | grep -oE '"[a-zA-Z_]+"' | tr -d '"' | sort)
if [ -z "$CORE_FILES" ]; then
  fail "lib/std/core/ has no .tr files -- cannot cross-check (did the layout move?)"
elif [ -z "$DECLARED" ]; then
  fail "could not find PreludeModuleNames[] in src/frontend/ParserModule.cpp"
elif [ "$CORE_FILES" = "$DECLARED" ]; then
  pass "PreludeModuleNames == lib/std/core/*.tr exactly ($(echo "$CORE_FILES" | wc -l) modules)"
else
  fail "PreludeModuleNames and lib/std/core/*.tr have drifted apart:"
  diff <(echo "$CORE_FILES") <(echo "$DECLARED") | sed 's/^/       /'
fi

# ── 3. Closed intrinsic list covers every bare builtin name ─────────────────
echo -e "\n${BOLD}3. Closed intrinsic list covers every registered builtin${RESET}"
BUILTIN_NAMES=$(grep -oE 'RegisterBuiltin\("[a-zA-Z_]+"' src/sema/Builtins.cpp \
                 | grep -oE '"[a-zA-Z_]+"' | tr -d '"' | sort -u)
MISSING=""
for n in $BUILTIN_NAMES sizeof typeof; do
  grep -q -- "$n" src/sema/Intrinsics.h || MISSING="$MISSING $n"
done
if [ -n "$MISSING" ]; then
  fail "name(s) registered as a bare intrinsic but undocumented in src/sema/Intrinsics.h:$MISSING"
else
  pass "every RegisterBuiltin() name, plus sizeof/typeof, is documented in src/sema/Intrinsics.h"
fi

# ── 4. getOrCreateMallocFn() has no remaining Codegen caller ────────────────
echo -e "\n${BOLD}4. No Codegen call site reaches a bare malloc via getOrCreateMallocFn()${RESET}"
MALLOC_HITS=$(grep -rn "getOrCreateMallocFn" src/codegen/Codegen*.cpp 2>/dev/null)
if [ -n "$MALLOC_HITS" ]; then
  fail "getOrCreateMallocFn() is referenced again in Codegen -- a GC-heap allocation may be silently bypassing turf_rt_alloc:"
  echo "$MALLOC_HITS" | sed 's/^/       /'
else
  pass "getOrCreateMallocFn() has no remaining definition or caller in src/codegen/Codegen*.cpp"
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
           | grep -v "^src/sema/TypeWalk\.cpp:" | grep -v "^src/sema/TypeWalk\.h:" \
           | grep -v "^src/codegen/CodegenCore\.cpp:" \
           | grep -v "assert(!dynamic_cast<const TurfGenericParamType")
if [ -n "$GEN_HITS" ]; then
  fail "dynamic_cast<const TurfGenericParamType found outside TypeWalk/CodegenCore/an INV-GEN-1 assertion -- route it through TypeWalk::containsOpenParam() instead:"
  echo "$GEN_HITS" | sed 's/^/       /'
else
  pass "every dynamic_cast<const TurfGenericParamType is in TypeWalk, an INV-GEN-1 assertion, or the documented CodegenCore.cpp exception"
fi

# ── 7. INV-API-1: only the query facade isolates a compile ─────────────────
# The per-subsystem save/restore hooks exist for GlobalStateScope
# (src/tooling/AnalysisFacade.cpp) alone. Any other caller would have to list
# every piece of front-end global state itself, and would break silently the
# first time the pipeline gained one it did not know about.
echo -e "\n${BOLD}7. Front-end state save/restore only in the query facade (INV-API-1)${RESET}"
API_HITS=$(grep -rnE "\b(Push|Pop)(Parser|Resolver)CompileState\(|DiagnosticEngine::(saveState|restoreState)\(|\bSetSourceOverlay\(|\bSetInstallRootOverride\(" src/ 2>/dev/null \
           | grep -vE ':\s*(//|\*)' \
           | grep -v "^src/tooling/AnalysisFacade\.cpp:" \
           | grep -vE ":(void|DiagnosticEngine::State) [A-Za-z:]*(CompileState|saveState|restoreState|SetSourceOverlay|SetInstallRootOverride)\(")
if [ -n "$API_HITS" ]; then
  fail "front-end state save/restore called outside src/tooling/AnalysisFacade.cpp -- run the compile through turf::analyze() (include/turf/Analysis.h) instead:"
  echo "$API_HITS" | sed 's/^/       /'
else
  pass "every front-end state save/restore hook is called only from the query facade"
fi

echo
if [ $FAILED -eq 0 ]; then
  echo -e "${GREEN}${BOLD}Boundary checks clean.${RESET}"
  exit 0
fi
echo -e "${RED}${BOLD}$FAILED boundary check(s) failed.${RESET}"
exit 1
