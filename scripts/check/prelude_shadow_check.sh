#!/usr/bin/env bash
# prelude_shadow_check.sh -- V-3 of docs/planning/ROADMAP_v3.0.0.md: before a
# new name is added to a bare-callable prelude module, list every program that
# already declares a function of that name.
#
# `io`, `math` and `sys` are bare-callable (FREEZE.md 6.25, F-PRE-2): every
# `pub` free function in them can be called unqualified in every program. The
# prelude is the last resort of a bare call (F-PRE-3), so a program that
# declares its own `f` hides the prelude's `f`, and a call shaped for the
# prelude's `f` is then checked against the program's alone. A new name added
# to one of those modules therefore changes what a call to it means in every
# program that already uses that name. 5.2 of the roadmap lets a minor add to
# the stdlib only if no program that compiled stops compiling, and this is the
# check that finds the programs to look at before adding it.
#
# For each NAME it searches showcase/, examples/, tests/ and lib/std/ for a
# free function declaration `fn NAME(` or `fn<T> NAME(`, in any `.tr` file:
# `pub` or private, `extern`, nested or not. A method (`fn Type.NAME(`) is never
# a bare name (F-PRE-2), so it is not a hit. The prelude modules' own files
# (lib/std/core/<bare module>.tr) are skipped: a name declared there is the
# prelude, not a user of it; it is reported as "already bare-callable".
#
# The bare modules are read from `PreludeBareModuleNames` in
# src/frontend/ParserModule.cpp, so the list cannot drift from the compiler.
#
# Usage: scripts/check/prelude_shadow_check.sh NAME [NAME...]
#        scripts/check/prelude_shadow_check.sh --list-bare
#   NAME         a function name proposed for io, math or sys
#   --list-bare  print the bare-callable modules and exit 0
# Exit:  0 = no program declares any NAME (safe to add on this evidence);
#        1 = at least one does, each listed as path:line: declaration (review
#            each before adding the name; a hit is not automatically a break);
#        2 = usage error, or the bare-module list could not be read.
set -u
cd "$(dirname "$0")/../.." || exit 2

SEARCH_ROOTS=(showcase examples tests lib/std)
PARSER_MODULE=src/frontend/ParserModule.cpp

usage() {
  sed -n '/^# Usage:/,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'
}

# The quoted names inside `PreludeBareModuleNames[] = {"io", "math", "sys"};`.
bare_modules() {
  sed -n 's/.*PreludeBareModuleNames\[\] *= *{\(.*\)}.*/\1/p' "$PARSER_MODULE" |
    tr -d '" ' | tr ',' '\n' | sed '/^$/d'
}

BARE=$(bare_modules 2>/dev/null)
if [ -z "$BARE" ]; then
  echo "prelude_shadow_check: cannot read PreludeBareModuleNames from $PARSER_MODULE" >&2
  exit 2
fi

case "${1:-}" in
  -h | --help) usage; exit 0 ;;
  --list-bare) printf '%s\n' "$BARE"; exit 0 ;;
  "") usage >&2; exit 2 ;;
esac

# `fn`, optional generics, then the name directly followed by `(`. `Type.name`
# has a `.` before the paren, so a method never matches. The optional `pub`,
# `extern` and `from "lib"` prefixes are the forms the declaration can take.
decl_regex() { # name
  printf '^[[:space:]]*(pub[[:space:]]+)?(extern[[:space:]]+)?(from[[:space:]]+"[a-z]+"[[:space:]]+)?fn(<[^>]*>)?[[:space:]]+%s[[:space:]]*\\(' "$1"
}

hits_total=0
names_with_hits=0

for name in "$@"; do
  # The name goes into a regex: accept an identifier and nothing else.
  if ! [[ "$name" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
    echo "prelude_shadow_check: '$name' is not an identifier" >&2
    exit 2
  fi
  re=$(decl_regex "$name")

  skip=()
  for m in $BARE; do
    f=lib/std/core/$m.tr
    [ -f "$f" ] || continue
    if grep -Eq "$re" "$f"; then
      echo "note: '$name' is already bare-callable (declared in $f); a new overload adds no new name"
    fi
    skip+=("$f")
  done

  roots=()
  for r in "${SEARCH_ROOTS[@]}"; do [ -d "$r" ] && roots+=("$r"); done

  # An empty skip list must not become a `:` pattern that drops every line.
  skip_pat=$(printf '%s:\n' "${skip[@]+"${skip[@]}"}")
  found=$(grep -rEn --include='*.tr' "$re" "${roots[@]}" 2>/dev/null |
    { if [ ${#skip[@]} -gt 0 ]; then grep -vF "$skip_pat"; else cat; fi; } |
    LC_ALL=C sort)

  if [ -z "$found" ]; then
    echo "ok: no program declares '$name'"
    continue
  fi
  count=$(printf '%s\n' "$found" | wc -l | tr -d ' ')
  files=$(printf '%s\n' "$found" | cut -d: -f1 | sort -u | wc -l | tr -d ' ')
  echo "SHADOWED: '$name' is declared $count time(s) in $files file(s):"
  printf '%s\n' "$found" | sed 's/^/  /'
  hits_total=$((hits_total + count))
  names_with_hits=$((names_with_hits + 1))
done

echo "prelude shadow: $# name(s) checked, $names_with_hits declared by existing programs ($hits_total declaration(s))"
[ "$hits_total" -eq 0 ]
