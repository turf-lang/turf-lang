#!/usr/bin/env bash
# stdlib_surface_check.sh -- LEC-007's completeness tripwire for the API lock.
#
# tests/46_stdlib_surface/ compiles and runs programs that use every `pub` item
# of lib/std/**, so deleting or renaming one in 2.x breaks a test. That only
# holds if the tests actually cover every item, and nothing else notices when a
# NEW `pub` item is added without being locked. This script closes that: it
# extracts every public declaration from the library and fails if
# tests/46_stdlib_surface/*.tr does not mention it.
#
# "Mentions" is deliberately a source-text check, not a proof the item is
# exercised -- the compile is the proof. The check is strict where names are
# unique and necessarily loose where they are not (`get`, `length`, `close`
# exist on many types): there it only requires the method-call spelling to
# appear somewhere. The locking test files are written to reference each item
# by its full spelling anyway (`module::name(...)`, `Type.method`).
#
# Usage: scripts/check/stdlib_surface_check.sh [--list]
#   --list  print every public item found (one per line) and exit 0
set -u
cd "$(dirname "$0")/../.."

LOCK_DIR=tests/46_stdlib_surface
LIST_ONLY=0
[ "${1:-}" = "--list" ] && LIST_ONLY=1

# Comment-stripped concatenation of the lock tests (a mention inside a `//`
# comment does not count).
LOCK_SRC=$(cat "$LOCK_DIR"/*.tr 2>/dev/null | sed 's://.*$::')

missing=0
badnames=0
total=0

check() { # kind, module, name, regex
  local kind=$1 mod=$2 name=$3 re=$4
  total=$((total + 1))
  if [ "$LIST_ONLY" = 1 ]; then
    echo "$mod $kind $name"
    return
  fi
  # F-API-2 (FREEZE.md 6.20): public functions, methods and fields are
  # snake_case. Types are PascalCase and constants UPPER_CASE by the same rule,
  # but those were never in question; the camelCase that existed was all in
  # function and method names.
  local last=${name##*.}
  case $kind in
    fn|method|field)
      if ! [[ "$last" =~ ^[a-z][a-z0-9_]*$ ]]; then
        echo "  NOT snake_case: $mod $kind $name"
        badnames=$((badnames + 1))
      fi ;;
  esac
  if ! printf '%s\n' "$LOCK_SRC" | grep -Eq "$re"; then
    echo "  NOT LOCKED: $mod $kind $name"
    missing=$((missing + 1))
  fi
}

while IFS= read -r file; do
  mod=$(basename "$file" .tr)
  inside_struct=""
  while IFS= read -r line; do
    # Track struct bodies so `pub field:` lines are attributed to their struct.
    # Only a `pub struct` counts: a pub field of a private struct (json's
    # Parser) is not reachable from another module, so it is not API.
    if [[ "$line" =~ ^pub[[:space:]]+struct[[:space:]]+([A-Za-z_0-9]+) ]]; then
      inside_struct=${BASH_REMATCH[1]}
    elif [[ "$line" =~ ^struct[[:space:]] ]]; then
      inside_struct=""
    elif [[ "$line" =~ ^\} ]]; then
      inside_struct=""
    fi

    if [[ "$line" =~ ^pub[[:space:]]+(extern[[:space:]]+)?(from[[:space:]]+\"[a-z]+\"[[:space:]]+)?fn(\<[A-Za-z,\ ]+\>)?[[:space:]]+([A-Za-z_0-9]+)\.([a-z_0-9]+)\( ]]; then
      check method "$mod" "${BASH_REMATCH[4]}.${BASH_REMATCH[5]}" "[.:]${BASH_REMATCH[5]}\\("
    elif [[ "$line" =~ ^pub[[:space:]]+(extern[[:space:]]+)?(from[[:space:]]+\"[a-z]+\"[[:space:]]+)?fn(\<[A-Za-z,\ ]+\>)?[[:space:]]+([a-z_0-9A-Z]+)\( ]]; then
      n=${BASH_REMATCH[4]}
      # A free function is locked by `module::name` or, for UFCS-callable
      # prelude helpers, by `.name(`.
      check fn "$mod" "$n" "(\\b${mod}::${n}\\b|\\.${n}\\()"
    elif [[ "$line" =~ ^pub[[:space:]]+(struct|enum|error)[[:space:]]+([A-Za-z_0-9]+) ]]; then
      n=${BASH_REMATCH[2]}
      check "${BASH_REMATCH[1]}" "$mod" "$n" "(\\b${mod}::${n}\\b|\\b${n}\\b)"
    elif [[ "$line" =~ ^export[[:space:]]+immut[[:space:]]+([A-Za-z_0-9]+) ]]; then
      n=${BASH_REMATCH[1]}
      check const "$mod" "$n" "\\b${mod}::${n}\\b"
    elif [ -n "$inside_struct" ] && [[ "$line" =~ ^[[:space:]]+pub[[:space:]]+([a-z_0-9]+): ]]; then
      n=${BASH_REMATCH[1]}
      check field "$mod" "$inside_struct.$n" "\\.${n}\\b"
    fi
  done < "$file"
done < <(find lib/std -name '*.tr' | sort)

if [ "$LIST_ONLY" = 1 ]; then
  exit 0
fi
echo "stdlib surface: $total public items, $missing not locked by $LOCK_DIR, $badnames not snake_case"
[ "$missing" -eq 0 ] && [ "$badnames" -eq 0 ]
