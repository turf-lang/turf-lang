#!/bin/bash
# SLM_IMPROVEMENT_PLAN workstream C1/C2: the docs the model is shown may not
# teach invalid Turf.
#
# Everything under docs/slm/ is pasted into the model's prompt, and a 1.5B
# model copies what it sees. A page whose CORRECT example does not compile
# trains it on exactly the syntax it then writes into a fix (live evidence,
# issue #267: E138's page showed `fn<T> Box.get(): T { ... }`, and `{ ... }` is
# not Turf). So every CORRECT block is wrapped into a minimal program and run
# through the real compiler, and a page with none fails.
#
# What is checked:
#   docs/slm/codes/Exxx.txt   every `CORRECT...:` / `VALID...:` block; a page
#                             that is not retired must have at least one.
#   docs/slm/*.txt            the category docs, the same blocks (they have
#                             fewer: those that have none are not an error).
#   docs/slm/syntax_card.txt  the always-included syntax card: EVERY indented
#                             block in it must compile (it has no INCORRECT
#                             examples, so nothing in it may fail).
#
# A block is the text indented 4+ columns under its heading (relative to the
# heading), dedented, then made into a program:
#   * declarations at column 0 (`fn`, `struct`, `enum`, `error`, `machine`,
#     `extern`, `operator`, `import`, `export`, decorators) stay at the top
#     level, in order; everything else at column 0 is a statement and goes into
#     `fn main(): int { ...; return 0; }`;
#   * a block with only declarations is checked with `--module` (no `main`
#     needed), one that declares `main` is checked as it stands;
#   * `import <m>;` is added for any stdlib module the block uses as `m::`
#     and does not import itself, the one thing a fragment legitimately
#     leaves out;
#   * a block with `// name.tr` lines is several files (an import across two
#     files): each section is written as name.tr in a folder of its own, and
#     the one with `main` (else the last) is the entry point;
#   * a block whose heading names `turf.toml` is a manifest line (E091's fix),
#     not Turf source: it counts as the page's CORRECT block and is not compiled.
# Nothing else is invented: a block that uses a variable it never declares, or
# `{ ... }` for a body, fails here and the page is what gets fixed. A heading
# is `CORRECT`/`VALID` plus optional words, then `:` (no other colon in it).
#
# Run from anywhere. Exit 0 = every block compiles; 1 = at least one did not
# (or a page lacks a CORRECT block); 2 = build/turfc is missing.
# Usage: scripts/slm/slm_doc_examples_check.sh [--keep DIR] [page.txt ...]
#   --keep DIR   leave the generated programs in DIR (to read what was compiled)
set -u
cd "$(dirname "$0")/../.." || exit 2

TURFC="${TURF_COMPILER:-./build/turfc}"
[ -x "$TURFC" ] || { echo "slm_doc_examples_check: $TURFC is missing -- build first"; exit 2; }

KEEP=""
if [ "${1:-}" = "--keep" ]; then
    KEEP="$2"
    shift 2
    mkdir -p "$KEEP"
fi

if [ $# -gt 0 ]; then
    FILES=("$@")
else
    FILES=(docs/slm/codes/E*.txt)
    for f in docs/slm/*.txt; do
        [ "$(basename "$f")" = "syntax_card.txt" ] || FILES+=("$f")
    done
    [ -f docs/slm/syntax_card.txt ] && FILES+=(docs/slm/syntax_card.txt)
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
OUT="${KEEP:-$TMP/progs}"
mkdir -p "$OUT"

# Stdlib module names (every lib/std/**/<name>.tr), for the auto-import.
MODULES=$(find lib/std -name '*.tr' | sed -E 's#.*/([^/]+)\.tr$#\1#' | sort -u | tr '\n' ' ')

# One awk pass per file: writes <OUT>/<base>_<n>.tr for each block (or
# <base>_<n>.mod.tr when it declares no `main` and has no statements), and
# prints "<program>\t<heading line>" per block. Also prints "NOBLOCK" when a
# page has none.
read -r -d '' EXTRACT <<'AWK'
function indent(s,   m) { match(s, /^ */); return RLENGTH }
function blank(s) { return s ~ /^[ \t]*$/ }
# Strip string literals and // comments so braces in them don't count.
function code_only(s) {
    gsub(/"([^"\\]|\\.)*"/, "", s)
    gsub(/'([^'\\]|\\.)*'/, "", s)
    sub(/\/\/.*/, "", s)
    return s
}
function braces(s,   t, o, c) {
    t = code_only(s)
    o = gsub(/\{/, "", t); c = gsub(/\}/, "", t)
    return o - c
}
function is_decl(s) {
    return s ~ /^(@[A-Za-z_]+|(pub |public )?(fn|struct|enum|error|machine|extern|operator|import|export|impl|static)[ <(])/ \
        || s ~ /^(pub |public )?(fn|struct|enum|error|machine)$/
}
# Builds one program from blk[from..to]; sets prog_text, prog_has_main and
# prog_is_module (no `main` and no statements: checked with --module).
function build(from, to,   i, decls, stmts, text, mods, all, m, name, imports, has_stmt, last, line, depth, indecl) {
    # Split into declarations (hoisted) and statements.
    decls = ""; stmts = ""; prog_has_main = 0; has_stmt = 0; depth = 0; indecl = 0; last = ""
    for (i = from; i <= to; i++) {
        line = blk[i]
        if (!indecl && depth == 0 && indent(line) == 0 && is_decl(line)) {
            indecl = 1
        }
        if (indecl) {
            decls = decls line "\n"
            if (line ~ /^(pub |public )?fn main[ (]/) prog_has_main = 1
            depth += braces(line)
            if (depth <= 0) { indecl = 0; depth = 0 }
        } else {
            stmts = stmts "    " line "\n"
            if (!blank(line) && line !~ /^[ \t]*\/\//) has_stmt = 1
            if (indent(line) == 0 && !blank(line) && line !~ /^\/\//) last = line
            depth += braces(line)
            if (depth < 0) depth = 0
        }
    }
    all = decls stmts
    # Auto-import any stdlib module used as `m::` that the block did not import.
    split(MODULES, mods, " ")
    imports = ""
    for (m in mods) {
        name = mods[m]
        if (index(all, name "::") == 0) continue
        if (all ~ ("(^|[^A-Za-z0-9_])" name "::")) {
            if (all !~ ("import " name "[;:]") && all !~ ("import " name "$"))
                imports = imports "import " name ";\n"
        }
    }
    text = imports decls
    if (prog_has_main && !has_stmt) {
        # as it stands
    } else if (has_stmt) {
        text = text "\nfn main(): int {\n" stmts
        if (last !~ /^return[ ;]/) text = text "    return 0;\n"
        text = text "}\n"
        prog_has_main = 1
    }
    prog_is_module = (!prog_has_main) ? 1 : 0
    prog_text = text
}
# A block is one program, or -- when it has `// name.tr` lines -- several files
# (the pages that show an import across two files), each section written as
# <name>.tr in a directory of its own so `import name;` finds its sibling. The
# entry point is the section with `main`, else the last one, and a module-only
# block is checked with --module.
function flush(   i, n, out, starts, names, ns, s, to, entry, dir, flag, f) {
    if (nblock == 0) return
    nprog++
    # A block whose heading says it is a turf.toml is a manifest, not Turf
    # source (E091's fix is a [dependencies] line): it counts as the page's
    # CORRECT block but there is nothing to compile.
    if (blockhead ~ /turf\.toml/) { nblock = 0; return }
    ns = 0
    for (i = 1; i <= nblock; i++)
        if (blk[i] ~ /^\/\/ [A-Za-z0-9_]+\.tr:?[ \t]*$/) {
            ns++; starts[ns] = i
            names[ns] = blk[i]; sub(/^\/\/ /, "", names[ns]); sub(/:?[ \t]*$/, "", names[ns])
        }
    if (ns == 0) {
        build(1, nblock)
        out = OUT "/" BASE "_" nprog (prog_is_module ? ".mod" : "") ".tr"
        printf "%s", prog_text > out
        close(out)
        print out "\t" blockline "\t" (prog_is_module ? "module" : "")
    } else {
        dir = OUT "/" BASE "_" nprog
        system("mkdir -p '" dir "'")
        entry = ""; flag = ""
        for (s = 1; s <= ns; s++) {
            to = (s < ns) ? starts[s + 1] - 1 : nblock
            build(starts[s], to)
            f = dir "/" names[s]
            printf "%s", prog_text > f
            close(f)
            if (prog_has_main || s == ns) { if (!entry || prog_has_main) { entry = f; flag = prog_is_module ? "module" : "" } }
        }
        print entry "\t" blockline "\t" flag
    }
    nblock = 0
}
BEGIN { nprog = 0; nblock = 0; inblk = 0; hindent = 0; pending_blank = 0; nlines = 0 }
{
    raw = $0
    if (MODE == "all") {
        # Every run of lines indented 4+ columns is a block.
        if (indent(raw) >= 4 && !blank(raw)) {
            if (!inblk) { inblk = 1; blockline = FNR; nblock = 0; strip = indent(raw) }
            blk[++nblock] = substr(raw, 5)
            pending_blank = 0
            next
        }
        if (inblk && blank(raw)) { next }   # blanks inside or after a block
        if (inblk) { flush(); inblk = 0 }
        next
    }
    if (inblk) {
        if (blank(raw)) { pending_blank++; next }
        if (indent(raw) >= hindent + 4) {
            for (b = 0; b < pending_blank; b++) blk[++nblock] = ""
            pending_blank = 0
            blk[++nblock] = substr(raw, hindent + 5)
            next
        }
        flush(); inblk = 0; pending_blank = 0
    }
    if (raw ~ /^[ \t]*(CORRECT|VALID)[^:]*:[ \t]*$/) {
        inblk = 1; hindent = indent(raw); blockline = FNR; nblock = 0; pending_blank = 0
        blockhead = raw
    }
}
END {
    if (inblk) flush()
    if (nprog == 0 && MODE != "all") print "NOBLOCK"
}
AWK

fail=0
nblocks=0
npages=0
declare -a MISSING=()

for f in "${FILES[@]}"; do
    [ -f "$f" ] || { echo "slm_doc_examples_check: no such file: $f"; fail=1; continue; }
    npages=$((npages + 1))
    base=$(basename "$f" .txt)
    mode=headings
    [ "$base" = "syntax_card" ] && mode=all
    # The blocks of one page, as "<program>\t<line>" rows.
    rows=$(awk -v OUT="$OUT" -v BASE="$base" -v MODE="$mode" -v MODULES="$MODULES" "$EXTRACT" "$f")
    if [ "$rows" = "NOBLOCK" ]; then
        case "$f" in
        docs/slm/codes/*)
            # A retired code is never raised, so there is nothing to teach.
            if head -1 "$f" | grep -q "RETIRED"; then continue; fi
            MISSING+=("$f") ;;
        docs/slm/syntax_card.txt)
            MISSING+=("$f") ;;
        esac
        continue
    fi
    while IFS=$'\t' read -r prog line module; do
        [ -n "$prog" ] || continue
        nblocks=$((nblocks + 1))
        flags=(--check)
        [ "$module" = "module" ] && flags+=(--module)
        out=$(timeout 60 "$TURFC" "${flags[@]}" "$prog" 2>&1)
        rc=$?
        if [ $rc -ne 0 ]; then
            fail=1
            # The first error only: its code, the compiler's one-line message
            # and the generated line it points at, so the page's author sees
            # what was compiled without opening the program.
            plain=$(sed -E 's/\x1b\[[0-9;]*m//g' <<<"$out")
            head=$(grep -m1 -E '^── error' <<<"$plain")
            code=$(grep -oE '\[E[0-9]{3}' <<<"$head" | head -1)
            at=$(grep -oE 'line [0-9]+' <<<"$head" | head -1)
            msg=$(grep -A3 -m1 -E '^── error' <<<"$plain" | grep -v -E '^── error|^\s*$' | head -1 | sed -E 's/^ +//')
            src=$(grep -m1 -E '^ +[0-9]+ │' <<<"$plain" | sed -E 's/^ +//')
            echo "FAIL $f:$line (block starting at that line) ${code:-[no code]}${at:+, $at of $(basename "$prog")}"
            echo "     ${msg:-$(grep -v '^$' <<<"$plain" | head -2 | tr '\n' ' ')}"
            [ -n "$src" ] && echo "     $src"
        fi
    done <<<"$rows"
done

for f in "${MISSING[@]:-}"; do
    [ -n "$f" ] || continue
    fail=1
    echo "FAIL $f  has no CORRECT block (every page the model can be given needs one, with the full compilable syntax)"
done

if [ $fail -eq 0 ]; then
    echo "slm_doc_examples_check: OK ($nblocks blocks from $npages files, every one compiles)"
    exit 0
fi
echo "slm_doc_examples_check: FAILED -- a doc under docs/slm/ teaches the model Turf the compiler rejects (or lacks an example)"
exit 1
