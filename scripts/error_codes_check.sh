#!/bin/bash
# Issue #263: the error-code registry may not lie.
#
# `turf --explain Exxx` is a promise to a beginner. Three ways the registry
# (include/ErrorCodes.h) used to break it without anything noticing, each
# found by hand:
#   1. a code with no docs/errors/codes/Exxx.txt -- `--explain` silently fell
#      back to a generic category page that never mentioned the mistake;
#   2. a code (and a documentation page) for a class that NOTHING raises --
#      ArgumentTypeError, ReturnTypeMismatchError, ... -- so the diagnostic the
#      page describes was reported under the catch-all E066 instead;
#   3. an ErrCode:: constant (the raw LogErrorAt() path's codes) that no call
#      site uses.
# This script fails on any of them. A code that is deliberately no longer
# produced is marked "(retired" in its table row's description: it keeps its
# number (a code is never reassigned, ErrorCodes.h) and its page, which then
# says it is retired and points at what replaced it.
#
# Run from the repository root. Exit 0 = the registry is consistent.
set -u
cd "$(dirname "$0")/.." || exit 2

HDR=include/ErrorCodes.h
fail=0
say() { echo "error_codes_check: $*"; fail=1; }

# Rows: `{"E001", "ClassName", "Description", ...}` or `{ErrCode::Name, ...}`.
# ErrCode::Name resolves to its "Exxx" through the constant block.
declare -A CONST_CODE
while IFS= read -r line; do
    name=$(sed -E 's/.*\*([A-Za-z0-9_]+) = .*/\1/' <<< "$line")
    code=$(grep -oE '"E[0-9]{3}"' <<< "$line" | tr -d '"')
    CONST_CODE[$name]=$code
done < <(grep -E 'inline constexpr const char \*[A-Za-z0-9_]+ = "E[0-9]{3}"' "$HDR")

rows=$(grep -E '^[[:space:]]*\{(ErrCode::[A-Za-z0-9_]+|"E[0-9]{3}"),' "$HDR")
[ -n "$rows" ] || { say "found no registry rows in $HDR"; exit 1; }

while IFS= read -r row; do
    first=$(sed -E 's/^[[:space:]]*\{([^,]+),.*/\1/' <<< "$row")
    cls=$(sed -E 's/^[^,]+,[[:space:]]*"([^"]+)".*/\1/' <<< "$row")
    desc=$(sed -E 's/^[^,]+,[^,]+,[[:space:]]*"([^"]*)".*/\1/' <<< "$row")
    if [[ "$first" == ErrCode::* ]]; then
        code="${CONST_CODE[${first#ErrCode::}]:-}"
    else
        code=$(tr -d '"' <<< "$first")
    fi
    [ -n "$code" ] || { say "cannot resolve the code of row: $first"; continue; }

    # 1. every code has its own page.
    [ -f "docs/errors/codes/$code.txt" ] || \
        say "$code ($cls) has no docs/errors/codes/$code.txt -- --explain would fall back to a generic page"

    # 1b. ...and its machine-facing twin, which the SLM grounds on. Same rule:
    #    no page means a silent fallback to a coarse category doc.
    [ -f "docs/slm/codes/$code.txt" ] || \
        say "$code ($cls) has no docs/slm/codes/$code.txt -- the SLM would fall back to a generic category doc"

    retired=0; [[ "$desc" == *"(retired"* ]] && retired=1

    # 2. every code is raised by something. A class is raised if its name
    #    appears in a file other than the one defining it; a raw-path family
    #    (an ErrCode:: constant, no class) is raised if the constant appears in
    #    src/ -- checked in 3.
    if [[ "$first" != ErrCode::* && $retired -eq 0 ]]; then
        def=$(grep -lE "^class $cls\b" include/*.h 2>/dev/null | head -1)
        refs=$(grep -rlw "$cls" src include tests/api --include='*.cpp' --include='*.h' 2>/dev/null \
                   | grep -v "^$HDR$" | grep -vx "${def:-/nonexistent}" | wc -l)
        [ "$refs" -gt 0 ] || \
            say "$code $cls is registered and documented but nothing raises it -- route a diagnostic to it, or mark its row \"(retired\""
    fi
done <<< "$rows"

# 3. every ErrCode:: constant is used by a call site (not just declared), and
#    names a row of the table.
for name in "${!CONST_CODE[@]}"; do
    code="${CONST_CODE[$name]}"
    uses=$(grep -rn "ErrCode::$name\b" src include --include='*.cpp' --include='*.h' | grep -v "^$HDR:" | wc -l)
    rowuse=$(grep -cE "^[[:space:]]*\{ErrCode::$name," "$HDR")
    [ "$uses" -gt 0 ] || say "ErrCode::$name ($code) is declared but no call site uses it"
    # Reused pre-existing codes (E021, E025, ...) are rows of their own under a
    # literal "Exxx"; the family constants must be rows via ErrCode::.
    grep -qE "\{(ErrCode::$name|\"$code\")," "$HDR" || say "ErrCode::$name ($code) is not a row of AllErrorCodes"
done

# 4. no code is the first column of two rows (resolved, so an ErrCode:: row and
#    a literal row of the same number are caught too).
dups=$(while IFS= read -r row; do
           first=$(sed -E 's/^[[:space:]]*\{([^,]+),.*/\1/' <<< "$row")
           if [[ "$first" == ErrCode::* ]]; then echo "${CONST_CODE[${first#ErrCode::}]:-}"
           else tr -d '"' <<< "$first"; fi
       done <<< "$rows" | sort | uniq -d | tr '\n' ' ')
[ -z "$dups" ] || say "more than one registry row for: $dups"

if [ $fail -eq 0 ]; then
    echo "error_codes_check: OK ($(wc -l <<< "$rows") registered codes, every one documented (human + SLM) and raised)"
fi
exit $fail
