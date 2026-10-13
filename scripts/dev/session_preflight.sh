#!/bin/bash
#
# session_preflight.sh <FEATURE-ID> -- run this BEFORE changing anything.
#
# Prints the reading list for one overhaul feature group, then proves the
# starting state is clean. The second half is the point: a session that starts
# work on a tree that was already failing will spend hours attributing a
# pre-existing failure to its own change, and -- worse -- may "fix" something
# that was never broken by it.
#
# Usage: scripts/dev/session_preflight.sh ARCH-001 [--no-build] [--no-tests]
# Exit:  0 = safe to start; 1 = the starting state is not clean.

set -u
cd "$(dirname "$0")/../.." || exit 1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
CYAN="\033[0;36m"; RESET="\033[0m"

FEATURE="${1:-}"
if [ -z "$FEATURE" ]; then
  echo "Usage: $0 <FEATURE-ID> [--no-build] [--no-tests]"
  echo "Feature IDs: ARCHITECTURE_OVERHAUL.md Appendix B; FDN/LEC/REL/SMP-NNN in ROADMAP_v2.0.0.md (all docs/planning/). OVH-NNN (the Overhaul closure items) lived in the now-deleted OVERHAUL_REMAINING_WORK.md -- see docs/development/changes/OVH-CLOSE.md, all landed."
  exit 1
fi
shift
DO_BUILD=1; DO_TESTS=1
for a in "$@"; do
  case "$a" in
    --no-build) DO_BUILD=0 ;;
    --no-tests) DO_TESTS=0 ;;
    *) echo "Unknown option: $a"; exit 1 ;;
  esac
done

# The spec a feature ID lives in. Overhaul groups (ARCH-001 ... SLM-001) are
# in ARCHITECTURE_OVERHAUL.md §8; the 2.0 milestones (FDN-/LEC-/REL-/SMP-NNN)
# are in ROADMAP_v2.0.0.md. The overhaul-closure fixes (OVH-NNN) used to live
# in the temporary handoff OVERHAUL_REMAINING_WORK.md -- OVH-015 deleted that
# file on closure (docs/development/changes/OVH-CLOSE.md); an OVH-NNN ID now
# only resolves via that item's own change record under docs/development/changes/.
# First file with a matching "### <ID> — " heading wins; the overhaul
# blueprint stays the fallback so an unknown ID still fails against the
# familiar file.
BLUEPRINT="docs/archive/ARCHITECTURE_OVERHAUL.md"
for CANDIDATE in docs/archive/ARCHITECTURE_OVERHAUL.md \
                 docs/planning/ROADMAP_v2.0.0.md; do
  if [ -f "$CANDIDATE" ] && grep -q "^### $FEATURE — " "$CANDIDATE"; then
    BLUEPRINT="$CANDIDATE"
    break
  fi
done
FAILED=0

echo -e "${BOLD}Session preflight — $FEATURE${RESET}\n"

# ── Reading list ────────────────────────────────────────────────────────────
echo -e "${BOLD}1. Read these, in this order, before opening any source file:${RESET}"
echo -e "   ${CYAN}AGENTS.md${RESET}                        build/test conventions — in full, first"
echo -e "   ${CYAN}docs/development/FREEZE.md${RESET}       the frozen decisions — settled, not reinterpretable"
echo -e "   ${CYAN}docs/development/SESSION_PROTOCOL.md${RESET}  this protocol, in full"
echo -e "   ${CYAN}$BLUEPRINT${RESET}"

SECTION_LINE=$(grep -n "^### $FEATURE — " "$BLUEPRINT" | head -1 | cut -d: -f1)
if [ -n "$SECTION_LINE" ]; then
  echo -e "      └─ the ${BOLD}$FEATURE${RESET} section, at line ${BOLD}$SECTION_LINE${RESET}, including its \"Must NOT reinterpret\" list"
  # The architecture docs that section actually names, rather than a guess.
  END_LINE=$(awk -v s="$SECTION_LINE" 'NR>s && /^### [A-Z]+-[0-9]+ — / {print NR; exit}' "$BLUEPRINT")
  [ -z "$END_LINE" ] && END_LINE=$(wc -l < "$BLUEPRINT")
  mapfile -t ARCH_DOCS < <(sed -n "${SECTION_LINE},${END_LINE}p" "$BLUEPRINT" \
      | grep -oE 'docs/architecture/[A-Za-z0-9_-]+\.md' | sort -u)
  if [ ${#ARCH_DOCS[@]} -gt 0 ]; then
    echo -e "   ${BOLD}Architecture docs this section names:${RESET}"
    for d in "${ARCH_DOCS[@]}"; do echo -e "   ${CYAN}$d${RESET}"; done
  else
    echo -e "   ${YELLOW}This section names no docs/architecture/*.md file.${RESET}"
    echo -e "   Pick the relevant one from SESSION_PROTOCOL.md's Step 4 table if you touch a subsystem."
  fi
else
  echo -e "   ${RED}No \"### $FEATURE — \" section found in $BLUEPRINT.${RESET}"
  echo -e "   Check the ID against the index of the file it should live in — a typo here means you read the wrong spec."
  FAILED=$((FAILED + 1))
fi
echo
echo -e "   Also ${CYAN}docs/archive/ARCHITECTURE_OVERHAUL.md${RESET} §25 — the changes that look"
echo -e "   tempting from inside a feature group and are wrong."
echo

# ── Starting state ──────────────────────────────────────────────────────────
echo -e "${BOLD}2. Starting state${RESET}"

if [ -n "$(git status --porcelain)" ]; then
  echo -e "   ${RED}✗${RESET} Worktree is dirty. Commit or stash first — otherwise this session's"
  echo -e "     diff is not this session's work, and the postflight cannot tell them apart."
  git status --short | sed 's/^/       /'
  FAILED=$((FAILED + 1))
else
  echo -e "   ${GREEN}✓${RESET} Worktree clean ($(git rev-parse --abbrev-ref HEAD) @ $(git rev-parse --short HEAD))"
fi

if [ $DO_BUILD -eq 1 ]; then
  if [ -d build ]; then
    if cmake --build build -j"$(nproc 2>/dev/null || echo 4)" > /tmp/turf_preflight_build.log 2>&1; then
      echo -e "   ${GREEN}✓${RESET} Build succeeds (cmake --build build)"
    else
      echo -e "   ${RED}✗${RESET} Build FAILS before you have changed anything. See /tmp/turf_preflight_build.log"
      tail -15 /tmp/turf_preflight_build.log | sed 's/^/       /'
      FAILED=$((FAILED + 1))
    fi
  else
    echo -e "   ${YELLOW}!${RESET} No build/ directory — run ./scripts/cmake_build.sh --release first"
    FAILED=$((FAILED + 1))
  fi
else
  echo -e "   ${YELLOW}-${RESET} Build check skipped (--no-build)"
fi

if [ $DO_TESTS -eq 1 ]; then
  if [ -x build/turfc ]; then
    if ./scripts/test_runner.sh > /tmp/turf_preflight_tests.log 2>&1; then
      echo -e "   ${GREEN}✓${RESET} $(grep -oE 'Total Tests: [0-9]+' /tmp/turf_preflight_tests.log | head -1) — all passing"
    else
      echo -e "   ${RED}✗${RESET} Suite is RED before you have changed anything. See /tmp/turf_preflight_tests.log"
      grep -E 'FAIL' /tmp/turf_preflight_tests.log | head -10 | sed 's/^/       /'
      FAILED=$((FAILED + 1))
    fi
    # SMP-001: the tool API's unit tests (a C++ binary test_runner.sh never sees).
    if [ -x build/turf-api-tests ]; then
      if ./build/turf-api-tests > /tmp/turf_preflight_api.log 2>&1; then
        echo -e "   ${GREEN}✓${RESET} Tool API tests (build/turf-api-tests) passing"
      else
        echo -e "   ${RED}✗${RESET} Tool API tests are RED before you have changed anything. See /tmp/turf_preflight_api.log"
        FAILED=$((FAILED + 1))
      fi
    fi
  else
    echo -e "   ${RED}✗${RESET} build/turfc missing — cannot run the suite"
    FAILED=$((FAILED + 1))
  fi
else
  echo -e "   ${YELLOW}-${RESET} Test run skipped (--no-tests)"
fi

if ./scripts/check/check_doc_links.sh --quiet > /dev/null 2>&1; then
  echo -e "   ${GREEN}✓${RESET} Documentation cross-references all resolve"
else
  echo -e "   ${RED}✗${RESET} scripts/check/check_doc_links.sh already fails on an unmodified tree"
  FAILED=$((FAILED + 1))
fi

echo
if [ $FAILED -eq 0 ]; then
  echo -e "${GREEN}${BOLD}Preflight clean.${RESET} Record the baseline in your change record, then start."
  echo "When you think you are done: scripts/dev/session_postflight.sh $FEATURE"
  exit 0
fi
echo -e "${RED}${BOLD}Preflight found $FAILED problem(s) in the STARTING state.${RESET}"
echo "Fix these before writing any code — none of them are yours yet, and after"
echo "your first edit it becomes much harder to tell that."
exit 1
