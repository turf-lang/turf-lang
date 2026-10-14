#!/bin/bash
#
# check_doc_links.sh -- every `docs/....md` path cited from src/, include/,
# scripts/, docs/ or a top-level Markdown file must name a file that exists.
#
# Why this exists: 43 source comments across 22 files spent months citing a
# planning document (`Errors.md`) as the design authority for the `error`/`!T`/
# `recover` feature after that file had been deleted, and nothing noticed
# (ARCHITECTURE_OVERHAUL.md §5 C-8). A citation to a file that is not there is
# worse than no citation -- it reads as authoritative and sends the reader
# looking for something they cannot find. This check is the structural
# prevention for that whole class.
#
# Deliberate exceptions live in scripts/check/doc_links_allowlist.txt, which is
# referrer-scoped on purpose: allowlisting a retired document for the one doc
# that *narrates its removal* must not also silence a brand-new source comment
# that starts citing it again.
#
# Usage: scripts/check/check_doc_links.sh [--quiet]
# Exit:  0 = every citation resolves; 1 = at least one dangling or stale entry.

set -u

cd "$(dirname "$0")/../.." || exit 1

ALLOWLIST="scripts/check/doc_links_allowlist.txt"
QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1

BOLD="\033[1m"; GREEN="\033[0;32m"; RED="\033[0;31m"; YELLOW="\033[0;33m"
RESET="\033[0m"

# Files that may cite a doc. `docs/` itself is included because a dangling
# cross-reference between two docs is the same failure as one from source.
mapfile -t FILES < <(
  find src include scripts docs -type f \
       \( -name '*.cpp' -o -name '*.h' -o -name '*.sh' -o -name '*.md' \) \
       -not -path 'scripts/check/doc_links_allowlist.txt' | sort
  ls -1 ./*.md 2>/dev/null | sed 's|^\./||'
)

FAILED=0
CITATIONS=0
declare -A ALLOW_USED=()

is_allowed() {  # $1 = cited path, $2 = citing file
  local path="$1" referrer="$2" line ap ar rest
  [ -f "$ALLOWLIST" ] || return 1
  while IFS= read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    ap=${line%%::*}; rest=${line#*::}; ar=${rest%%::*}
    ap=$(echo "$ap" | xargs); ar=$(echo "$ar" | xargs)
    if [ "$ap" = "$path" ] && [[ "$referrer" == $ar ]]; then
      ALLOW_USED["$ap::$ar"]=1
      return 0
    fi
  done < "$ALLOWLIST"
  return 1
}

for f in "${FILES[@]}"; do
  # One citation per (file, path, line) -- a path cited twice on one line is
  # still one thing to fix.
  while IFS=: read -r lineno path; do
    [ -z "$path" ] && continue
    CITATIONS=$((CITATIONS + 1))
    [ -e "$path" ] && continue
    if is_allowed "$path" "$f"; then
      [ $QUIET -eq 0 ] && echo -e "${YELLOW}allowed${RESET} $f:$lineno -> $path"
      continue
    fi
    echo -e "${RED}DANGLING${RESET} $f:$lineno cites $path (no such file)"
    FAILED=$((FAILED + 1))
  done < <(grep -noE 'docs/[A-Za-z0-9_][A-Za-z0-9_./+-]*\.md' "$f" | sort -u -t: -k1,1n -k2,2)
done

# A stale allowlist is the same disease one step later: an entry for a file
# that now exists, or for a citation nobody makes any more, silently widens
# the exception. Both are failures, and both are fixed by deleting a line.
if [ -f "$ALLOWLIST" ]; then
  while IFS= read -r line; do
    case "$line" in ''|\#*) continue ;; esac
    ap=${line%%::*}; rest=${line#*::}; ar=${rest%%::*}
    ap=$(echo "$ap" | xargs); ar=$(echo "$ar" | xargs)
    if [ -e "$ap" ]; then
      echo -e "${RED}STALE ALLOWLIST${RESET} $ap now exists -- delete its line from $ALLOWLIST"
      FAILED=$((FAILED + 1))
    elif [ -z "${ALLOW_USED[$ap::$ar]:-}" ]; then
      echo -e "${RED}STALE ALLOWLIST${RESET} nothing under '$ar' cites $ap any more -- delete its line from $ALLOWLIST"
      FAILED=$((FAILED + 1))
    fi
  done < "$ALLOWLIST"
fi

if [ $FAILED -eq 0 ]; then
  [ $QUIET -eq 0 ] && echo -e "${GREEN}${BOLD}OK${RESET} $CITATIONS docs/*.md citations across ${#FILES[@]} files, all resolve."
  exit 0
fi
echo -e "\n${RED}${BOLD}$FAILED dangling or stale documentation reference(s).${RESET}"
echo "Fix by re-pointing the citation at the live document, rewording it so it"
echo "does not name a path, or -- only for a deliberate historical mention --"
echo "adding a referrer-scoped line to $ALLOWLIST."
exit 1
