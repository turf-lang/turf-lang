#!/bin/bash
#
# project_smoke.sh -- LEC-001 stage 5: `turf` reads projects. It finds a
# turf.toml, resolves path dependencies into a package graph and drives
# `turfc --package-graph` (docs/development/FREEZE.md 6.16, F-PKG-1..4; the
# graph format is include/PackageGraph.h).
#
# The Go unit tests (cd cli && go test ./...) cover the manifest rules, graph
# resolution and the text edits in isolation. What only a real build shows is
# the whole path: `turf new` -> `turf add` -> `turf build`/`run`/`check` against
# the real compiler, with the diagnostics passed through unchanged, and the
# manifest edits leaving a hand-written file alone. package_graph_smoke.sh
# already proves the compiler half; this proves the CLI half feeds it correctly.
# Every case checks the exit status and the message or error code it is about,
# never just "it failed" (AGENTS.md: a failure for the wrong reason is not a
# pass).
#
# Usage: scripts/project_smoke.sh     (after building build/turf and build/turfc)
# Exit:  0 = all checks pass, or build/turf is absent and CI is not set (Go is
#            optional for a local build: a SKIP, never a silent pass in CI);
#        1 = a check failed (each failure is printed).

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
TURF="$ROOT/build/turf"
TURFC="$ROOT/build/turfc"

if [ ! -x "$TURF" ]; then
  if [ -n "${CI:-}" ]; then
    echo "FAIL: build/turf is missing in CI (was Go installed before the CMake build?)"
    exit 1
  fi
  echo "SKIP (no build/turf: Go not installed?)"
  exit 0
fi
[ -x "$TURFC" ] || { echo "FAIL: $TURFC not built"; exit 1; }
# Which compiler and which stdlib run must follow from the binaries' own
# location, not from a developer's shell (TURF_HOME often points at another
# checkout).
unset TURF_HOME TURF_COMPILER

V=$(head -1 VERSION)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILED=0
fail() { echo "FAIL: $1"; FAILED=$((FAILED + 1)); }

# t <name> <dir> <turf args...>: run turf in <dir>; output and status are kept
# under $WORK/_<name>.{out,rc}.
t() {
  local n=$1 d=$2
  shift 2
  (cd "$d" && "$TURF" "$@") > "$WORK/_$n.out" 2>&1
  echo $? > "$WORK/_$n.rc"
}
strip() { sed 's/\x1b\[[0-9;]*m//g' "$1"; }
expect_rc() { [ "$(cat "$WORK/_$2.rc")" = "$3" ] || fail "$1: exit $(cat "$WORK/_$2.rc"), expected $3 (got: $(strip "$WORK/_$2.out" | head -c 300))"; }
expect_out() {
  strip "$WORK/_$2.out" | grep -qF -- "$3" ||
    fail "$1: output lacks '$3' (got: $(strip "$WORK/_$2.out" | head -c 400))"
}
expect_no_out() {
  if strip "$WORK/_$2.out" | grep -qF -- "$3"; then fail "$1: output must not contain '$3'"; fi
}
same() { cmp -s "$2" "$3" || fail "$1: $2 and $3 differ"; }

# lib <dir> <name> <root module source>: a library package by hand, so the
# module bodies are ours; the manifest goes through `turf new` where it can.
mklib() {
  local dir=$1 name=$2
  (cd "$dir" && "$TURF" new "$name" --lib) > /dev/null 2>&1 || fail "setup: turf new $name --lib"
  printf '%s\n' "$3" > "$dir/$name/src/$name.tr"
}

# --- new, run, build ----------------------------------------------------------
P1=$WORK/p1
mkdir -p "$P1"
t new_app "$P1" new app
expect_rc "turf new app" new_app 0
[ -f "$P1/app/turf.toml" ] && [ -f "$P1/app/src/main.tr" ] && [ -f "$P1/app/.gitignore" ] ||
  fail "turf new app: turf.toml, src/main.tr and .gitignore should all exist"
grep -qx '/build/' "$P1/app/.gitignore" || fail "turf new app: .gitignore lacks /build/"
grep -q "^turf = \"$(echo "$V" | cut -d. -f1,2)\"$" "$P1/app/turf.toml" ||
  fail "turf new app: turf key should be the compiler's major.minor ($V)"

t run_app "$P1/app" run
expect_rc "turf run in a new project" run_app 0
expect_out "turf run in a new project" run_app "Hello, app!"
[ -x "$P1/app/build/app" ] || fail "turf run in a new project: build/app was not left behind"
[ -f "$P1/app/build/package-graph.json" ] || fail "turf run: build/package-graph.json was not written"
# From a subdirectory: found by walking up, and diagnostics still say src/main.tr
# because turfc always runs from the project root.
t run_sub "$P1/app/src" run
expect_rc "turf run from src/" run_sub 0

t new_dir_exists "$P1" new app
expect_rc "turf new over an existing directory" new_dir_exists 1
expect_out "turf new over an existing directory" new_dir_exists "already exists"

t new_geo "$P1" new geometry --lib
expect_rc "turf new geometry --lib" new_geo 0
[ -f "$P1/geometry/src/geometry.tr" ] && [ ! -f "$P1/geometry/src/main.tr" ] ||
  fail "turf new --lib: only src/geometry.tr should exist"
t build_geo "$P1/geometry" build
expect_rc "turf build in a library" build_geo 0
[ ! -e "$P1/geometry/build/geometry" ] || fail "a library must not produce an executable"
[ -f "$P1/geometry/build/package-graph.json" ] || fail "library build: no graph file"
t run_geo "$P1/geometry" run
expect_rc "turf run in a library" run_geo 1
expect_out "turf run in a library" run_geo "is a library; there is nothing to run"

# A library with an error: checked even though nothing imports it.
printf 'pub fn broken(): int {\n    return nope;\n}\n' > "$P1/geometry/src/geometry.tr"
t build_geo_bad "$P1/geometry" build
expect_rc "library errors surface" build_geo_bad 1
expect_out "library errors surface" build_geo_bad "nope"
expect_out "library errors keep their code" build_geo_bad "[E040]"
printf 'pub fn answer(): int {\n    return 42;\n}\n' > "$P1/geometry/src/geometry.tr"

# --- add, and the library's value through the CLI ------------------------------
t add_geo "$P1/app" add geometry --path ../geometry
expect_rc "turf add" add_geo 0
grep -qxF 'geometry = { path = "../geometry" }' "$P1/app/turf.toml" || fail "turf add: dependency line not written"
printf 'import geometry;\n\nfn main(): int {\n    return geometry::answer();\n}\n' > "$P1/app/src/main.tr"
t run_dep "$P1/app" run
expect_rc "a dependency's function through turf run" run_dep 42

t build_flag_o "$P1/app" build -o x
expect_rc "turf build -o" build_flag_o 2
expect_out "turf build -o" build_flag_o "-o is not accepted"
t build_flag_g "$P1/app" build --package-graph x.json
expect_rc "turf build --package-graph" build_flag_g 2
expect_out "turf build --package-graph" build_flag_g "--package-graph is not accepted"
t run_flag_o "$P1/app" run -o x
expect_rc "turf run -o in a project" run_flag_o 2

# check: no executable, graph only
t clean1 "$P1/app" clean
expect_rc "turf clean" clean1 0
[ ! -e "$P1/app/build" ] || fail "turf clean: build/ still exists"
t clean2 "$P1/app" clean
expect_rc "turf clean twice" clean2 0
t check_ok "$P1/app" check
expect_rc "turf check in a project" check_ok 0
[ ! -e "$P1/app/build/app" ] && [ -f "$P1/app/build/package-graph.json" ] ||
  fail "turf check: should write only the graph file"
# The graph is a pure function of the manifests: a rebuild writes the same bytes.
cp "$P1/app/build/package-graph.json" "$WORK/graph1.json"
t build_again "$P1/app" build
expect_rc "rebuild" build_again 0
same "graph is byte-identical on rebuild" "$WORK/graph1.json" "$P1/app/build/package-graph.json"
grep -q '"format": 1' "$P1/app/build/package-graph.json" || fail "graph lacks format 1"
grep -q "\"src\": \"$P1/app/src\"" "$P1/app/build/package-graph.json" ||
  grep -q "\"src\": \"$(cd "$P1/app" && pwd -P)/src\"" "$P1/app/build/package-graph.json" ||
  fail "graph src paths should be absolute"

# A type error is reported by turfc, unchanged, with its code.
cp "$P1/app/src/main.tr" "$WORK/main_good.tr"
printf 'import geometry;\n\nfn main(): int {\n    return geometry::answer() + missing;\n}\n' > "$P1/app/src/main.tr"
t check_bad "$P1/app" check
expect_rc "turf check reports a compile error" check_bad 1
expect_out "turf check reports a compile error" check_bad "missing"
cp "$WORK/main_good.tr" "$P1/app/src/main.tr"

# The single-file path inside a project: E027 with the `turf build` hint.
t compile_in_proj "$P1/app" compile src/main.tr
expect_rc "turf compile of a file that imports a dependency" compile_in_proj 1
expect_out "turf compile of a file that imports a dependency" compile_in_proj "[E027]"
expect_out "turf compile of a file that imports a dependency" compile_in_proj "only visible to \`turf build\`"

# --- manifest edits leave a hand-written file alone ------------------------------
P2=$WORK/p2
mkdir -p "$P2"
t new_app2 "$P2" new app
mklib "$P2" geometry 'pub fn answer(): int { return 7; }'
{ printf '# my own comment, kept\n'; cat "$P2/app/turf.toml"; } > "$WORK/toml.new"
cp "$WORK/toml.new" "$P2/app/turf.toml"
cp "$P2/app/turf.toml" "$WORK/toml.before"
t add2 "$P2/app" add geometry --path ../geometry
expect_rc "turf add into a commented manifest" add2 0
grep -qxF '# my own comment, kept' "$P2/app/turf.toml" || fail "turf add dropped a comment"
[ "$(($(wc -l < "$P2/app/turf.toml") - $(wc -l < "$WORK/toml.before")))" = 1 ] || fail "turf add changed more than one line"
t dup "$P2/app" add geometry --path ../geometry
expect_rc "turf add of a dependency already present" dup 1
expect_out "turf add of a dependency already present" dup "already in [dependencies]"
t remove2 "$P2/app" remove geometry
expect_rc "turf remove" remove2 0
same "add then remove is byte-identical" "$WORK/toml.before" "$P2/app/turf.toml"
t remove_absent "$P2/app" remove geometry
expect_rc "turf remove of an absent dependency" remove_absent 1
expect_out "turf remove of an absent dependency" remove_absent "is not in [dependencies]"

# add refuses what build would reject, and writes nothing.
mklib "$P2" other 'pub fn f(): int { return 1; }'
cp "$P2/app/turf.toml" "$WORK/toml.before2"
t add_wrongkey "$P2/app" add geo --path ../other
expect_rc "turf add with a key that is not the package name" add_wrongkey 1
expect_out "turf add with a key that is not the package name" add_wrongkey "the key must be its package name"
t add_nopkg "$P2/app" add ghost --path ../ghost
expect_rc "turf add of a missing directory" add_nopkg 1
t add_git "$P2/app" add net --git https://example.invalid/net --tag v1
expect_rc "turf add --git" add_git 2
expect_out "turf add --git" add_git "stage 6"
same "a refused add writes nothing" "$WORK/toml.before2" "$P2/app/turf.toml"

# A dependency spread over several lines is refused, not half-deleted.
cat > "$P2/app/turf.toml" <<'EOF'
[package]
name = "app"
version = "0.1.0"

[dependencies]
geometry = {
    path = "../geometry",
}
EOF
cp "$P2/app/turf.toml" "$WORK/toml.multi"
t remove_multi "$P2/app" remove geometry
expect_rc "turf remove of a multi-line dependency" remove_multi 1
expect_out "turf remove of a multi-line dependency" remove_multi "edit turf.toml by hand"
same "a refused remove writes nothing" "$WORK/toml.multi" "$P2/app/turf.toml"

# --- packages with the same internal module name ------------------------------
# The stage 4 identity case, now through the CLI: both packages have a
# util::Node, and they are two types.
P3=$WORK/p3
mkdir -p "$P3"
t new_app3 "$P3" new app
mklib "$P3" shapes 'import util;
pub fn area(): int {
    mut n: util::Node = util::Node::default();
    n.v = util::g();
    return n.v * 4;
}'
printf 'pub struct Node {\n    pub v: int\n}\npub fn g(): int {\n    return 5;\n}\n' > "$P3/shapes/src/util.tr"
printf 'pub struct Node {\n    pub v: int\n}\npub fn f(): int {\n    return 1;\n}\n' > "$P3/app/src/util.tr"
printf 'import shapes;\nimport util;\n\nfn main(): int {\n    mut n: util::Node = util::Node::default();\n    n.v = util::f();\n    return shapes::area() + n.v;\n}\n' > "$P3/app/src/main.tr"
t add3 "$P3/app" add shapes --path ../shapes
expect_rc "setup: add shapes" add3 0
t run3 "$P3/app" run
expect_rc "two packages each with util::Node" run3 21

# --- graph shapes -------------------------------------------------------------
# pa -> pc and pb -> pc: a diamond that agrees.
P4=$WORK/p4
mkdir -p "$P4"
t new_app4 "$P4" new app
mklib "$P4" pc 'pub fn one(): int { return 1; }'
mklib "$P4" pa 'import pc;
pub fn fa(): int { return pc::one() + 10; }'
mklib "$P4" pb 'import pc;
pub fn fb(): int { return pc::one() + 20; }'
t add_pc_a "$P4/pa" add pc --path ../pc
t add_pc_b "$P4/pb" add pc --path ../pc
t add_pa "$P4/app" add pa --path ../pa
t add_pb "$P4/app" add pb --path ../pb
expect_rc "setup: diamond adds" add_pb 0
printf 'import pa;\nimport pb;\n\nfn main(): int {\n    return pa::fa() + pb::fb();\n}\n' > "$P4/app/src/main.tr"
t run4 "$P4/app" run
expect_rc "a diamond that agrees" run4 32

# Transitive import: pc is reachable only through pa and pb, so app cannot import it.
cp "$P4/app/src/main.tr" "$WORK/main4.tr"
printf 'import pa;\nimport pc;\n\nfn main(): int {\n    return pa::fa() + pc::one();\n}\n' > "$P4/app/src/main.tr"
t trans "$P4/app" build
expect_rc "importing a transitive dependency" trans 1
expect_out "importing a transitive dependency" trans "[E091]"
cp "$WORK/main4.tr" "$P4/app/src/main.tr"

# Name conflict: two different directories both holding a package called pc.
P5=$WORK/p5
mkdir -p "$P5"
t new_app5 "$P5" new app
mklib "$P5" pa 'pub fn fa(): int { return 1; }'
mklib "$P5" pb 'pub fn fb(): int { return 2; }'
mkdir -p "$P5/one" "$P5/two"
mklib "$P5/one" pc 'pub fn one(): int { return 1; }'
mklib "$P5/two" pc 'pub fn one(): int { return 2; }'
t c1 "$P5/pa" add pc --path ../one/pc
t c2 "$P5/pb" add pc --path ../two/pc
t c3 "$P5/app" add pa --path ../pa
expect_rc "setup: add pa" c3 0
# `turf add` resolves what it writes, so it refuses the second one...
cp "$P5/app/turf.toml" "$WORK/toml5"
t c4 "$P5/app" add pb --path ../pb
expect_rc "turf add that would create a name conflict" c4 1
expect_out "turf add that would create a name conflict" c4 "package 'pc' is required from two different directories"
same "the refused add wrote nothing" "$WORK/toml5" "$P5/app/turf.toml"
# ...so the conflict is written by hand to prove `build` refuses it too.
printf 'pb = { path = "../pb" }\n' >> "$P5/app/turf.toml"
t conflict "$P5/app" build
expect_rc "one package name at two directories" conflict 1
expect_out "one package name at two directories" conflict "package 'pc' is required from two different directories"
expect_out "one package name at two directories" conflict "package 'pa'"
expect_out "one package name at two directories" conflict "package 'pb'"

# Cycle: written by hand, because `turf add` would refuse to create it.
P6=$WORK/p6
mkdir -p "$P6"
mklib "$P6" pa 'pub fn fa(): int { return 1; }'
mklib "$P6" pb 'pub fn fb(): int { return 2; }'
printf 'pb = { path = "../pb" }\n' >> "$P6/pa/turf.toml"
printf 'pa = { path = "../pa" }\n' >> "$P6/pb/turf.toml"
t cycle "$P6/pa" build
expect_rc "a dependency cycle" cycle 1
expect_out "a dependency cycle" cycle "dependency cycle"
expect_out "a dependency cycle" cycle "pa -> pb -> pa"
# `add` resolves what it writes, so it refuses to close the cycle.
mklib "$P6" pc 'pub fn fc(): int { return 3; }'
mklib "$P6" pd 'pub fn fd(): int { return 4; }'
t addc1 "$P6/pc" add pd --path ../pd
t addc2 "$P6/pd" add pc --path ../pc
expect_rc "turf add that would close a cycle" addc2 1
expect_out "turf add that would close a cycle" addc2 "dependency cycle"

# Key/name mismatch.
P7=$WORK/p7
mkdir -p "$P7"
t new_app7 "$P7" new app
mklib "$P7" geo 'pub fn f(): int { return 1; }'
printf 'geometry = { path = "../geo" }\n' >> "$P7/app/turf.toml"
t mismatch "$P7/app" build
expect_rc "dependency key differs from the package name" mismatch 1
expect_out "dependency key differs from the package name" mismatch "the key must be its package name"
# A dependency with no root module.
(cd "$P7" && "$TURF" new tool) > /dev/null 2>&1 || fail "setup: turf new tool"
printf '[package]\nname = "app"\nversion = "0.1.0"\n\n[dependencies]\ntool = { path = "../tool" }\n' > "$P7/app/turf.toml"
t notlib "$P7/app" build
expect_rc "a dependency that is not a library" notlib 1
expect_out "a dependency that is not a library" notlib "is not a library package: it has no src/tool.tr"

# --- manifest rules, through the CLI --------------------------------------------
manifest_case() { # manifest_case <label> <expected rc> <fragment> <turf.toml text>
  local d=$WORK/m_$RANDOM
  mkdir -p "$d/src"
  printf 'fn main(): int {\n    return 0;\n}\n' > "$d/src/main.tr"
  printf '%s\n' "$4" > "$d/turf.toml"
  t manifest "$d" build
  expect_rc "manifest: $1" manifest "$2"
  expect_out "manifest: $1" manifest "$3"
}
manifest_case "unknown key in [package]" 1 "unknown key \`authors\` in [package]" '[package]
name = "app"
version = "0.1.0"
authors = ["me"]'
manifest_case "unknown table" 1 "unknown key \`build\` at the top level" '[package]
name = "app"
version = "0.1.0"
[build]
x = 1'
manifest_case "registry-style dependency" 1 "registry versions are not part of TurfLang 2.0" '[package]
name = "app"
version = "0.1.0"
[dependencies]
math = "^1.2.0"'
manifest_case "path and git together" 1 "has both \`path\` and \`git\`" '[package]
name = "app"
version = "0.1.0"
[dependencies]
m = { path = "../m", git = "https://example.invalid/m", tag = "v1" }'
manifest_case "a git dependency" 1 "git dependencies arrive in LEC-001 stage 6 ('m' in" '[package]
name = "app"
version = "0.1.0"
[dependencies]
m = { git = "https://example.invalid/m", tag = "v1" }'
manifest_case "reserved word as the package name" 1 "'fn' is a reserved word" '[package]
name = "fn"
version = "0.1.0"'
manifest_case "no name" 1 "missing the required key \`name\`" '[package]
version = "0.1.0"'
mkdir -p "$WORK/empty"
printf '[package]\nname = "empty"\nversion = "0.1.0"\n' > "$WORK/empty/turf.toml"
t neither "$WORK/empty" build
expect_rc "a package with neither main.tr nor a root module" neither 1
expect_out "a package with neither main.tr nor a root module" neither "has neither src/main.tr (a program) nor src/empty.tr (a library)"
manifest_case "turf pin, wrong major" 1 "requires turf \"9.0\"" '[package]
name = "app"
version = "0.1.0"
turf = "9.0"'
expect_out "turf pin names the compiler version too" manifest "turfc $V"
manifest_case "turf pin, newer minor" 1 "which is older" '[package]
name = "app"
version = "0.1.0"
turf = "2.99"'

# --- [unsafe] allow (F-PKG-4) ---------------------------------------------------
P8=$WORK/p8
mkdir -p "$P8"
t new_app8 "$P8" new app
mklib "$P8" ffi 'import rawmod;
pub fn go(): int { return rawmod::raw(); }'
printf 'unsafe module;\npub fn raw(): int {\n    mut p: int* = null;\n    return 1;\n}\n' > "$P8/ffi/src/rawmod.tr"
t add8 "$P8/app" add ffi --path ../ffi
printf 'import ffi;\n\nfn main(): int {\n    return ffi::go();\n}\n' > "$P8/app/src/main.tr"
cp "$P8/app/turf.toml" "$WORK/toml8"
t unsafe_denied "$P8/app" build
expect_rc "a dependency's unsafe module, not allowed" unsafe_denied 1
expect_out "a dependency's unsafe module, not allowed" unsafe_denied "[E066]"
unsafe_case() { # unsafe_case <label> <rc> <fragment> <allow list>
  { cat "$WORK/toml8"; printf '\n[unsafe]\nallow = [%s]\n' "$4"; } > "$P8/app/turf.toml"
  t unsafe_case "$P8/app" build
  expect_rc "unsafe: $1" unsafe_case "$2"
  [ -z "$3" ] || expect_out "unsafe: $1" unsafe_case "$3"
}
unsafe_case "allowed" 0 "" '"ffi"'
unsafe_case "an unknown package" 1 "which is not a package in this project's dependency graph" '"nope"'
unsafe_case "the root itself" 1 "its own modules need no listing" '"app"'
# The dependency's own [unsafe] table grants nothing.
cp "$WORK/toml8" "$P8/app/turf.toml"
printf '\n[unsafe]\nallow = ["ffi"]\n' >> "$P8/ffi/turf.toml"
t unsafe_self "$P8/app" build
expect_rc "a dependency cannot allow itself" unsafe_self 1
expect_out "a dependency cannot allow itself" unsafe_self "[E066]"

# --- refusals -------------------------------------------------------------------
LOOSE=$WORK/loose
mkdir -p "$LOOSE"
t outside "$LOOSE" build
expect_rc "turf build outside a project" outside 2
expect_out "turf build outside a project" outside "turf new"
t outside_run "$LOOSE" run
expect_rc "turf run with no file outside a project" outside_run 2
t outside_add "$LOOSE" add x --path ../x
expect_rc "turf add outside a project" outside_add 2

t new_stdlib "$LOOSE" new list --lib
expect_rc "turf new of a library named like a stdlib module" new_stdlib 1
expect_out "turf new of a library named like a stdlib module" new_stdlib "standard-library module"
[ ! -e "$LOOSE/list" ] || fail "a refused turf new must not create the directory"
t new_reserved "$LOOSE" new fn
expect_rc "turf new with a reserved word" new_reserved 1
expect_out "turf new with a reserved word" new_reserved "reserved word"
t new_dash "$LOOSE" new my-app
expect_rc "turf new with an invalid name" new_dash 1

# A program named like a stdlib module is fine: nothing can import it.
t new_prog_std "$LOOSE" new json
expect_rc "turf new of a program named like a stdlib module" new_prog_std 0

# --- init -----------------------------------------------------------------------
mkdir -p "$LOOSE/myinit/src"
printf 'fn main(): int {\n    return 3;\n}\n' > "$LOOSE/myinit/src/main.tr"
printf '*.o\n' > "$LOOSE/myinit/.gitignore"
t init1 "$LOOSE/myinit" init
expect_rc "turf init in an existing directory" init1 0
grep -q 'return 3;' "$LOOSE/myinit/src/main.tr" || fail "turf init overwrote src/main.tr"
grep -qx '/build/' "$LOOSE/myinit/.gitignore" && grep -qx '\*.o' "$LOOSE/myinit/.gitignore" ||
  fail "turf init should append /build/ and keep the existing .gitignore lines"
grep -q '^name = "myinit"$' "$LOOSE/myinit/turf.toml" || fail "turf init should default the name to the directory's"
t init2 "$LOOSE/myinit" init
expect_rc "turf init twice" init2 1
expect_out "turf init twice" init2 "already exists"
t init_run "$LOOSE/myinit" run
expect_rc "the initialised project runs" init_run 3
mkdir -p "$LOOSE/bad-name"
t init_bad "$LOOSE/bad-name" init
expect_rc "turf init in a directory whose name is not a package name" init_bad 1
expect_out "turf init in a directory whose name is not a package name" init_bad "not a valid package name"

# --- drift: the reserved words the CLI carries vs the lexer's --------------------
sed -n '/^std::map<std::string, int> Keywords/,/;$/p' src/Lexer.cpp |
  grep -o '{"[a-z_0-9]*", TOK' | sed 's/{"\(.*\)", TOK/\1/' | sort > "$WORK/kw_lexer"
sed -n '/^var reserved = /,/^}/p' cli/internal/manifest/manifest.go |
  grep -o '"[a-z_0-9]*"' | tr -d '"' | sort > "$WORK/kw_cli"
[ "$(wc -l < "$WORK/kw_lexer")" -gt 40 ] || fail "drift: could not extract the lexer's keyword table"
if ! diff "$WORK/kw_lexer" "$WORK/kw_cli" > "$WORK/kw_diff"; then
  fail "drift: cli/internal/manifest/manifest.go's reserved list differs from src/Lexer.cpp's Keywords ($(tr '\n' ' ' < "$WORK/kw_diff"))"
fi

if [ "$FAILED" -ne 0 ]; then
  echo "project_smoke: $FAILED check(s) failed"
  exit 1
fi
echo "project_smoke: all checks passed"
