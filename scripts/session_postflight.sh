#!/bin/bash
#
# session_postflight.sh <FEATURE-ID> -- run this BEFORE declaring a feature
# group done. Mechanically checks docs/development/SESSION_PROTOCOL.md's
# completion checklist and exits non-zero on any miss.
#
# The check that matters most is #4: that the change record is not a stub.
# `docs/planning/ARCHITECTURE_OVERHAUL.md` §5 C-8 is what a stub record costs
# -- 43 source comments citing a design document nobody ever wrote down the
# replacement for. *Removed assumptions* and *Known pitfalls* are singled out
# because they are precisely the two things a future reader cannot
# reconstruct from the diff.
#
# Usage: scripts/session_postflight.sh ARCH-001 [--no-tests]
# Exit:  0 = every mechanical item passes; 1 = at least one does not.

set -u
cd "$(dirname "$0")/.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

FEATURE="${1:-}"
if [ -z "$FEATURE" ]; then
  echo "Usage: $0 <FEATURE-ID> [--no-tests]"
  exit 1
fi
shift
DO_TESTS=1
for a in "$@"; do
  case "$a" in
    --no-tests) DO_TESTS=0 ;;
    *) echo "Unknown option: $a"; exit 1 ;;
  esac
done

RECORD="docs/development/changes/$FEATURE.md"
FAILED=0
pass() { echo -e "   ${GREEN}✓${RESET} $1"; }
fail() { echo -e "   ${RED}✗${RESET} $1"; FAILED=$((FAILED + 1)); }
note() { echo -e "   ${YELLOW}!${RESET} $1"; }

echo -e "${BOLD}Session postflight — $FEATURE${RESET}\n"

# ── 1. The suite ────────────────────────────────────────────────────────────
echo -e "${BOLD}1. Test suite${RESET}"
if [ $DO_TESTS -eq 0 ]; then
  note "skipped (--no-tests) — the suite still has to be green before you are done"
elif [ ! -x build/turfc ]; then
  fail "build/turfc missing — build before running the postflight"
elif ./scripts/test_runner.sh > /tmp/turf_postflight_tests.log 2>&1; then
  pass "$(grep -oE 'Total Tests: [0-9]+' /tmp/turf_postflight_tests.log | head -1) — all passing"
else
  fail "suite is RED — see /tmp/turf_postflight_tests.log"
  grep -E 'FAIL' /tmp/turf_postflight_tests.log | head -10 | sed 's/^/       /'
fi
# SMP-001: the tool API's own unit tests -- a C++ binary, not .tr files, so
# test_runner.sh never sees them.
if [ $DO_TESTS -eq 1 ]; then
  if [ ! -x build/turf-api-tests ]; then
    fail "build/turf-api-tests missing — build before running the postflight"
  elif ./build/turf-api-tests > /tmp/turf_postflight_api.log 2>&1; then
    pass "tool API tests (build/turf-api-tests) passing"
  else
    fail "tool API tests are RED — see /tmp/turf_postflight_api.log"
    grep -E 'FAIL' /tmp/turf_postflight_api.log | head -10 | sed 's/^/       /'
  fi
fi

# ── 2. lib/std and showcase, compiled directly ──────────────────────────────
#
# test_runner.sh covers tests/ ONLY. A stdlib or showcase file left on removed
# syntax sits broken indefinitely with the full suite green -- AGENTS.md
# records a confirmed instance. Every lib/std module must resolve with no
# error at all (other than the expected "No entry point found" a pure
# library file always reports). Until OVH-011
# (docs/development/changes/OVH-011.md), list.tr and mem.tr could not
# clear this bar: the prelude module `strings` does `import list::List`, so
# compiling list.tr (or mem.tr, which list.tr imports) as an entry point
# re-declared its own type through the transitively-reimported prelude
# chain and raised E034 -- ClaimEntryFileImportSlot() (ParserModule.cpp)
# closes that gap by recognizing when a transitive import resolves back to
# the entry file's own path, so no per-file exemption belongs here any more
# (a REGRESSION of OVH-011 must fail this check like any other).
echo -e "\n${BOLD}2. lib/std and showcase compiled directly${RESET}"
if [ ! -x build/turfc ]; then
  fail "build/turfc missing — cannot compile-check lib/std or showcase"
else
  STD_BAD=0
  while IFS= read -r f; do
    out=$(./build/turfc --resolve "$f" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    errs=$(echo "$out" | grep -E '^── error|^Error:' | grep -v 'No entry point found')
    [ -z "$errs" ] && continue
    fail "$f does not resolve:"
    echo "$errs" | head -3 | sed 's/^/       /'
    STD_BAD=$((STD_BAD + 1))
  done < <(find lib/std -name '*.tr' | sort)
  [ $STD_BAD -eq 0 ] && pass "every lib/std module resolves ($(find lib/std -name '*.tr' | wc -l) files)"

  # The six prelude modules, compiled directly: the ONLY regression coverage
  # for the prelude-self-import bug class (AGENTS.md). A regression shows up
  # as "Ambiguous ... call" on a module's own internal UFCS self-call.
  PRELUDE_BAD=0
  for m in int double bool char strings array; do
    f="lib/std/core/$m.tr"
    out=$(./build/turfc --resolve "$f" 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    if echo "$out" | grep -qi 'ambiguous'; then
      fail "prelude self-import regression in $f (ambiguous call — see AGENTS.md)"
      PRELUDE_BAD=$((PRELUDE_BAD + 1))
    fi
  done
  [ $PRELUDE_BAD -eq 0 ] && pass "all six prelude modules compile directly with no ambiguity (the self-import check)"

  SHOW_BAD=0
  while IFS= read -r f; do
    if ! ./build/turfc "$f" -o /tmp/turf_postflight_showcase.out > /tmp/turf_postflight_showcase.log 2>&1; then
      fail "$f does not compile:"
      sed 's/\x1b\[[0-9;]*m//g' /tmp/turf_postflight_showcase.log | grep -E '^── error|^Error:' | head -3 | sed 's/^/       /'
      SHOW_BAD=$((SHOW_BAD + 1))
    fi
  done < <(find showcase -name '*.tr' | sort)
  rm -f /tmp/turf_postflight_showcase.out
  [ $SHOW_BAD -eq 0 ] && pass "every showcase program compiles ($(find showcase -name '*.tr' | wc -l) files)"

  # LEC-007: every `pub` item in lib/std is referenced by
  # tests/46_stdlib_surface/, so a removal fails a compile and a NEW public
  # item that nobody locked fails here.
  if SURFACE_OUT=$(./scripts/stdlib_surface_check.sh 2>&1); then
    pass "$(echo "$SURFACE_OUT" | tail -1)"
  else
    fail "stdlib public surface is not fully locked by tests/46_stdlib_surface:"
    echo "$SURFACE_OUT" | head -8 | sed 's/^/       /'
  fi

  # OVH-012: a plain 'import io; import json; import http;' ("hello world")
  # must print NO warning by default. A warning anchored inside lib/std is a
  # "system-module warning" -- recorded but not printed unless
  # --stdlib-warnings is passed (DiagnosticEngine::flushAll(), Lexer.cpp). A
  # failure here means either a lib/std source change reintroduced a warning
  # class OVH-012 fixed (NeverReturnsErrorWarning/UnusedVariableWarning/
  # DeadStoreWarning blind to `recover` blocks) or the print-time suppression
  # itself regressed.
  HELLO_SRC="/tmp/turf_postflight_hello.tr"
  printf 'import io;\nimport json;\nimport http;\nfn main(): int { return 0; }\n' > "$HELLO_SRC"
  HELLO_OUT=$(./build/turfc --resolve "$HELLO_SRC" -o /tmp/turf_postflight_hello.out 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
  rm -f "$HELLO_SRC" /tmp/turf_postflight_hello.out
  if echo "$HELLO_OUT" | grep -qi 'warning'; then
    fail "'import io; import json; import http;' prints a warning by default (OVH-012 regression):"
    echo "$HELLO_OUT" | grep -i 'warning' | head -3 | sed 's/^/       /'
  else
    pass "hello-world stdlib imports (io/json/http) produce no warning by default"
  fi
fi

# ── 3. The _module.tr naming sweep, both directions ─────────────────────────
#
# SESSION_PROTOCOL.md item 3 scopes this to test files THIS session added or
# renamed, and the scoping is deliberate rather than lenient: the sweep also
# turns up pre-existing hits that predate the session by years, and a check
# that is red on arrival is a check people learn to ignore. So: anything this
# session touched is a hard failure; anything already in the tree is reported
# for the record and named, so it cannot be "discovered" again in a year.
echo -e "\n${BOLD}3. Test-file naming sweep${RESET}"
has_main() { grep -qE '\bfn\s+(int\s+)?main\s*\(' "$1" 2>/dev/null; }

# Files this session added or renamed under tests/ (staged, unstaged, or new).
mapfile -t TOUCHED < <(
  { git diff --name-only --diff-filter=AR -- tests/
    git diff --cached --name-only --diff-filter=AR -- tests/
    git ls-files --others --exclude-standard -- tests/
  } 2>/dev/null | sort -u
)

sweep_one() {  # $1 = path; echoes a reason if the name is wrong, else nothing
  local f="$1" base; base=$(basename "$f")
  case "$f" in tests/99_known_bugs/*|tests/custom/*) return ;; esac
  if [[ "$base" == *_module.tr* ]]; then
    has_main "$f" && echo "has a main() but is silently EXCLUDED by the runner's '_module.tr' substring filter"
  else
    case "$base" in fail_*) return ;; esac
    has_main "$f" || echo "has no main() but IS run as a standalone test (it will fail with 'No entry point found')"
  fi
}

TOUCHED_BAD=0
for f in "${TOUCHED[@]:-}"; do
  [ -z "$f" ] && continue
  case "$f" in *.tr) ;; *) continue ;; esac
  [ -f "$f" ] || continue
  reason=$(sweep_one "$f")
  if [ -n "$reason" ]; then
    fail "$f $reason"
    TOUCHED_BAD=$((TOUCHED_BAD + 1))
  fi
done
if [ ${#TOUCHED[@]} -eq 0 ] || [ -z "${TOUCHED[0]:-}" ]; then
  pass "no test file added or renamed by this session — sweep not required"
elif [ $TOUCHED_BAD -eq 0 ]; then
  pass "${#TOUCHED[@]} test file(s) added/renamed by this session, all correctly named"
fi

# Whole-tree state, informational. The long-standing pre-existing hit here
# (tests/07_functions_modules/export_qualified_cross_module.tr, ending in the
# substring `_module.tr` though it has a main()) was renamed to
# export_qualified_across_modules.tr by OVH-015 (2026-09-27); this sweep is
# now expected to be clean on a healthy tree.
PRE_SKIPPED=$(find tests -name "*_module.tr" -not -path 'tests/99_known_bugs/*' -not -path 'tests/custom/*' \
              | xargs grep -lE '\bfn\s+(int\s+)?main\s*\(' 2>/dev/null)
PRE_INCLUDED=$(find tests -name "*.tr" -not -name "*_module.tr" -not -name "fail_*" \
               -not -path 'tests/99_known_bugs/*' -not -path 'tests/custom/*' \
               | xargs grep -LE '\bfn\s+(int\s+)?main\s*\(' 2>/dev/null)
if [ -n "$PRE_SKIPPED$PRE_INCLUDED" ]; then
  note "whole-tree sweep (informational, not this session's doing):"
  [ -n "$PRE_SKIPPED" ] && echo "$PRE_SKIPPED" | sed 's/^/       skipped by the filter, but has a main(): /'
  [ -n "$PRE_INCLUDED" ] && echo "$PRE_INCLUDED" | sed 's/^/       run as a test, but has no main(): /'
else
  pass "whole-tree sweep clean in both directions"
fi

# ── 4. The change record, and that it is not a stub ─────────────────────────
echo -e "\n${BOLD}4. Change record${RESET}"
if [ ! -f "$RECORD" ]; then
  fail "$RECORD does not exist — copy docs/development/TEMPLATE.md to it"
else
  pass "$RECORD exists"
  # A section is "filled in" when it has real prose under it: at least two
  # non-empty, non-heading lines. One line is a placeholder; a heading
  # followed by the next heading is a stub.
  section_body_lines() {
    awk -v want="$1" '
      /^## / { inside = ($0 == want) ? 1 : 0; next }
      inside && NF && $0 !~ /^#/ { n++ }
      END { print n + 0 }
    ' "$RECORD"
  }
  for sec in "## Removed assumptions" "## Known pitfalls"; do
    n=$(section_body_lines "$sec")
    if [ "$n" -lt 2 ]; then
      fail "'$sec' is a stub ($n line(s) of content). This is the section a future reader cannot reconstruct from the diff — write it."
    else
      pass "'$sec' filled in ($n lines)"
    fi
  done
  for sec in "## Why" "## Tests" "## Handbook impact"; do
    n=$(section_body_lines "$sec")
    [ "$n" -lt 1 ] && fail "'$sec' is empty — write 'None' if that is the honest answer" || pass "'$sec' filled in"
  done
fi

# ── 5. INDEX, handbook-sync, INVARIANTS ─────────────────────────────────────
echo -e "\n${BOLD}5. Index, handbook queue, invariants${RESET}"
if grep -q "| $FEATURE |" docs/development/INDEX.md 2>/dev/null; then
  pass "INDEX.md has a row for $FEATURE"
else
  fail "INDEX.md has no row for $FEATURE"
fi
if grep -q "$FEATURE" docs/development/handbook-sync/PENDING.md 2>/dev/null; then
  pass "handbook-sync/PENDING.md has an entry for $FEATURE"
else
  fail "handbook-sync/PENDING.md has no entry for $FEATURE (write the row even if what is owed is 'nothing')"
fi
if [ -f "$RECORD" ]; then
  mapfile -t NEW_INVS < <(grep -oE 'INV-[A-Z]+-[0-9]+' "$RECORD" | sort -u)
  MISSING=""
  for inv in "${NEW_INVS[@]:-}"; do
    [ -z "$inv" ] && continue
    grep -q "$inv" docs/development/INVARIANTS.md || MISSING="$MISSING $inv"
  done
  if [ -n "$MISSING" ]; then
    fail "the record names invariant(s) that INVARIANTS.md does not:$MISSING"
  else
    pass "every INV-* named in the record is in INVARIANTS.md"
  fi
fi

# ── 6. Doc links ────────────────────────────────────────────────────────────
echo -e "\n${BOLD}6. Documentation cross-references${RESET}"
if ./scripts/check_doc_links.sh --quiet > /tmp/turf_postflight_links.log 2>&1; then
  pass "scripts/check_doc_links.sh passes"
else
  fail "scripts/check_doc_links.sh fails:"
  head -8 /tmp/turf_postflight_links.log | sed 's/^/       /'
fi

# ── 7. The part no script can check ─────────────────────────────────────────
echo -e "\n${BOLD}7. Completion criteria (manual)${RESET}"
note "§8's numbered 'Completion criteria' for $FEATURE must each be DEMONSTRATED,"
note "not asserted: a command whose output you actually looked at. No script can"
note "check this for you, and it is the item most often skipped."

echo
if [ $FAILED -eq 0 ]; then
  echo -e "${GREEN}${BOLD}Postflight clean.${RESET} Every mechanical item passes."
  exit 0
fi
echo -e "${RED}${BOLD}Postflight found $FAILED problem(s). Not done yet.${RESET}"
exit 1
