#!/bin/bash
#
# package_graph_smoke.sh -- LEC-001 stage 4: `turfc --package-graph <file>`,
# the compiler half of package builds (docs/development/FREEZE.md 6.16,
# F-PKG-1/2/4; format in src/tooling/PackageGraph.h).
#
# scripts/test_runner.sh is single-file only -- it cannot hand the compiler a
# package graph -- so this builds small multi-package trees in a temp
# directory and checks each rule end to end: resolution against the graph
# (own modules, direct dependencies, stdlib), module identity per
# (package, path) so two packages' `util` modules and their types coexist,
# E090/E091, graph validation, and the per-package unsafe bit. Each case
# checks the exit status and the error code or message it is about, never
# just "it failed" (AGENTS.md: a failure for the wrong reason is not a pass).
#
# Usage: scripts/smoke/package_graph_smoke.sh     (after building build/turfc)
# Exit:  0 = all checks pass; 1 = a check failed (each failure is printed).

set -u
cd "$(dirname "$0")/../.." || exit 1
TURFC="$(pwd)/build/turfc"
[ -x "$TURFC" ] || { echo "FAIL: $TURFC not built"; exit 1; }
# Resolution must depend on the graph and the binary's own install root only.
unset TURF_HOME TURF_COMPILER

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1
FAILED=0
fail() { echo "FAIL: $1"; FAILED=$((FAILED + 1)); }

# run <name> <args...>: turfc with output under $WORK/<name>.{out,rc}.
run() {
  local n=$1
  shift
  "$TURFC" "$@" > "$WORK/$n.out" 2>&1
  echo $? > "$WORK/$n.rc"
}
expect_rc() { [ "$(cat "$WORK/$2.rc")" = "$3" ] || fail "$1: exit $(cat "$WORK/$2.rc"), expected $3"; }
expect_out() {
  sed 's/\x1b\[[0-9;]*m//g' "$WORK/$2.out" | grep -qF -- "$3" ||
    fail "$1: output lacks '$3' (got: $(sed 's/\x1b\[[0-9;]*m//g' "$WORK/$2.out" | head -c 400))"
}
graph() { # graph <file> <json>
  printf '%s\n' "$2" > "$1"
}

# --- the tree -----------------------------------------------------------------
# myapp -> geometry. Both packages have an internal `util` declaring `Node`:
# the collision module identity (package, path) exists to allow.
mkdir -p myapp/src geometry/src plotter/src
cat > geometry/src/util.tr <<'EOF'
pub struct Node {
    pub v: int
}
pub fn g(): int {
    return 5;
}
EOF
cat > geometry/src/shapes.tr <<'EOF'
pub fn sides(): int {
    return 4;
}
EOF
cat > geometry/src/geometry.tr <<'EOF'
import util;
import shapes;
import list;
pub fn area(): int {
    mut n: util::Node = util::Node::default();
    n.v = util::g();
    return n.v * shapes::sides();
}
fn hidden(): int {
    return 1;
}
EOF
cat > myapp/src/util.tr <<'EOF'
pub struct Node {
    pub v: int
}
pub fn f(): int {
    return 1;
}
EOF
cat > myapp/src/main.tr <<'EOF'
import geometry;
import util;
import list;
fn main(): int {
    mut n: util::Node = util::Node::default();
    n.v = util::f();
    return geometry::area() + n.v;
}
EOF
cat > plotter/src/plotter.tr <<'EOF'
import geometry;
pub fn plot(): int {
    return geometry::area() * 2;
}
EOF
graph g.json '{"format": 1, "root": "myapp", "packages": {
  "myapp":    {"src": "myapp/src",    "deps": ["geometry"], "allow_unsafe": true},
  "geometry": {"src": "geometry/src", "deps": []}}}'

# --- builds that must work ------------------------------------------------------
# 5 * 4 + 1: geometry's util::Node and myapp's util::Node are two types, and
# both packages share the one stdlib `list` without registering it twice.
run build --package-graph g.json myapp/src/main.tr -o app
expect_rc "two packages, two util modules" build 0
[ -x app ] && { ./app; r=$?; [ $r = 21 ] || fail "two packages: program exited $r, expected 21"; }

# A package has one program entry point, the root's src/main.tr, so each case
# below that needs a different program writes it there (as_main) and puts the
# original back afterwards (restore_main). The case sources live outside src/,
# where the package build would otherwise see them as modules.
mkdir -p cases
cp myapp/src/main.tr cases/main.orig.tr
as_main() { cp "cases/$1" myapp/src/main.tr; }
restore_main() { cp cases/main.orig.tr myapp/src/main.tr; }

# Any other file is refused as the program, and accepted as a module.
run notmain --package-graph g.json myapp/src/util.tr -o notmain
expect_rc "a non-main.tr entry point" notmain 1
expect_out "a non-main.tr entry point" notmain "the program's entry point is the root package's src/main.tr"
run notmain_mod --check --module --package-graph g.json myapp/src/util.tr
expect_rc "the same file checked as a module" notmain_mod 0

cat > cases/flat.tr <<'EOF'
import geometry::area;
fn main(): int {
    return area();
}
EOF
as_main flat.tr
run flat --package-graph g.json myapp/src/main.tr -o flat
restore_main
expect_rc "flat import from a dependency" flat 0
[ -x flat ] && { ./flat; r=$?; [ $r = 20 ] || fail "flat import: exited $r, expected 20"; }

# A diamond: myapp -> plotter -> geometry and myapp -> geometry. geometry is
# one package with one identity, loaded once.
cat > cases/diamond.tr <<'EOF'
import geometry;
import plotter;
fn main(): int {
    return plotter::plot() - geometry::area();
}
EOF
graph gd.json '{"format": 1, "root": "myapp", "packages": {
  "myapp":    {"src": "myapp/src",    "deps": ["geometry", "plotter"]},
  "plotter":  {"src": "plotter/src",  "deps": ["geometry"]},
  "geometry": {"src": "geometry/src", "deps": []}}}'
as_main diamond.tr
run diamond --package-graph gd.json myapp/src/main.tr -o diamond
restore_main
expect_rc "diamond dependency" diamond 0
[ -x diamond ] && { ./diamond; r=$?; [ $r = 20 ] || fail "diamond: exited $r, expected 20"; }

# A library package checked on its own: its root module is a module, not a
# program.
graph gl.json '{"format": 1, "root": "geometry", "packages": {
  "geometry": {"src": "geometry/src", "deps": []}}}'
run lib --check --module --package-graph gl.json geometry/src/geometry.tr
expect_rc "library root checked with --module" lib 0

# --- resolution errors ----------------------------------------------------------
graph gt.json '{"format": 1, "root": "myapp", "packages": {
  "myapp":    {"src": "myapp/src",    "deps": ["plotter"]},
  "plotter":  {"src": "plotter/src",  "deps": ["geometry"]},
  "geometry": {"src": "geometry/src", "deps": []}}}'
run transitive --check --package-graph gt.json myapp/src/main.tr
expect_rc "transitive dependency" transitive 1
expect_out "transitive dependency" transitive "[E091"
expect_out "transitive dependency" transitive "'plotter' depends on it"
expect_out "transitive dependency" transitive "Add 'geometry' to [dependencies]"

printf 'import shapes;\nfn main(): int {\n    return 0;\n}\n' > cases/internal.tr
as_main internal.tr
run internal --check --package-graph g.json myapp/src/main.tr
restore_main
expect_rc "another package's internal module" internal 1
expect_out "another package's internal module" internal "[E091"
expect_out "another package's internal module" internal "internal to package 'geometry'"

cp myapp/src/util.tr myapp/src/geometry.tr
run owndep --check --package-graph g.json myapp/src/main.tr
expect_rc "own module named like a dependency" owndep 1
expect_out "own module named like a dependency" owndep "[E090"
expect_out "own module named like a dependency" owndep "dependency 'geometry'"
expect_out "own module named like a dependency" owndep "may not share its name with a dependency"
rm myapp/src/geometry.tr

# Not searched in a package build: the cwd. A stray module there must not be
# found (single-file mode would find it).
printf 'pub fn s(): int {\n    return 1;\n}\n' > stray.tr
printf 'import stray;\nfn main(): int {\n    return stray::s();\n}\n' > cases/stray_user.tr
as_main stray_user.tr
run stray --check --package-graph g.json myapp/src/main.tr
restore_main
expect_rc "cwd is not searched" stray 1
expect_out "cwd is not searched" stray "[E027"
# ...and the hint names the places a package build does look, not the cwd.
expect_out "cwd is not searched" stray "In a project, \`import stray;\` names one of three things"

# A dependency's private function: E088 names the package, not its identity.
printf 'import geometry;\nfn main(): int {\n    return geometry::hidden();\n}\n' > cases/priv.tr
as_main priv.tr
run priv --check --package-graph g.json myapp/src/main.tr
restore_main
expect_rc "private function of a dependency" priv 1
expect_out "private function of a dependency" priv "[E088"
expect_out "private function of a dependency" priv "is private to module 'geometry'"
expect_out "private function of a dependency" priv "mark it \`pub\` in geometry.tr"

# --- the unsafe bit (F-PKG-4) ---------------------------------------------------
printf 'unsafe module;\npub fn raw(): int {\n    mut p: int* = null;\n    return 1;\n}\n' > geometry/src/rawmod.tr
printf 'import rawmod;\n' | cat - geometry/src/geometry.tr > t && mv t geometry/src/geometry.tr
run unsafe_denied --check --package-graph g.json myapp/src/main.tr
expect_rc "dependency's unsafe module, not allowed" unsafe_denied 1
expect_out "dependency's unsafe module, not allowed" unsafe_denied "[E130"
# The global flag does not grant it in a package build: consent is per package.
run unsafe_flag --check --allow-unsafe-modules --package-graph g.json myapp/src/main.tr
expect_rc "--allow-unsafe-modules does not override the graph" unsafe_flag 1
graph gu.json '{"format": 1, "root": "myapp", "packages": {
  "myapp":    {"src": "myapp/src",    "deps": ["geometry"]},
  "geometry": {"src": "geometry/src", "deps": [], "allow_unsafe": true}}}'
run unsafe_ok --check --package-graph gu.json myapp/src/main.tr
expect_rc "dependency's unsafe module, allowed" unsafe_ok 0

# --- graph validation -----------------------------------------------------------
bad() { # bad <label> <json> <expected fragment>
  graph bad.json "$2"
  run bad --check --package-graph bad.json myapp/src/main.tr
  expect_rc "graph: $1" bad 1
  expect_out "graph: $1" bad "$3"
}
bad "not JSON" 'nope' "not valid JSON"
bad "unknown format" '{"format": 2}' '"format" must be 1'
bad "unknown key" '{"format":1,"root":"myapp","packages":{"myapp":{"src":"myapp/src","depz":[]}}}' 'unknown key "depz"'
bad "missing src dir" '{"format":1,"root":"myapp","packages":{"myapp":{"src":"nosuch"}}}' "does not exist"
bad "unknown dependency" '{"format":1,"root":"myapp","packages":{"myapp":{"src":"myapp/src","deps":["nope"]}}}' "which is not in"
bad "dependency named like stdlib" '{"format":1,"root":"myapp","packages":{"myapp":{"src":"myapp/src","deps":["list"]},"list":{"src":"plotter/src"}}}' "name of a standard-library module"
bad "dependency without a root module" '{"format":1,"root":"myapp","packages":{"myapp":{"src":"myapp/src","deps":["plotter"]},"plotter":{"src":"geometry/src"}}}' "has no root module"
bad "root not in packages" '{"format":1,"root":"x","packages":{"myapp":{"src":"myapp/src"}}}' "not in \"packages\""
run outside --check --package-graph g.json plotter/src/plotter.tr
expect_rc "entry file outside the root package" outside 1
expect_out "entry file outside the root package" outside "is not inside the root package"

# --- the "tests" key (REL-002, driver interface 1.3) ----------------------------
# A graph that names the root package's tests/ lets any file there be the entry
# (each test is its own program), and makes it the root package's own code: it
# imports the package's src/ modules, its direct dependencies and the helper
# modules (`*_module.tr`) beside it -- and is held to the same E091 as src/ is.
# Another file in tests/ is a test, never a helper.
mkdir -p myapp/tests
printf 'pub fn twice(x: int): int {\n    return x * 2;\n}\n' > myapp/src/doubling.tr
printf 'pub fn three(): int {\n    return 3;\n}\n' > myapp/tests/support_module.tr
cat > myapp/tests/t.tr <<'EOF'
import geometry;
import doubling;
import support_module;
fn main(): int {
    return doubling::twice(support_module::three()) - geometry::area() + 14;
}
EOF
printf 'import shapes;\nfn main(): int {\n    return 0;\n}\n' > myapp/tests/internals.tr
GT='{"format":1,"root":"myapp","packages":{
  "myapp":    {"src": "myapp/src",    "deps": ["geometry"], "allow_unsafe": true, "tests": "myapp/tests"},
  "geometry": {"src": "geometry/src", "deps": [], "allow_unsafe": true}}}'
graph gt.json "$GT"
run tests_entry --check --package-graph gt.json myapp/tests/t.tr
expect_rc "a test file as the entry, importing src/, a dependency and a helper" tests_entry 0
run tests_entry_internals --check --package-graph gt.json myapp/tests/internals.tr
expect_rc "a test cannot import a dependency's internal module" tests_entry_internals 1
expect_out "a test cannot import a dependency's internal module" tests_entry_internals "[E091"
# The helper beside a test is the tests' own: a src/ module never sees it.
printf 'import support;\npub fn bad(): int {\n    return support::three();\n}\n' > myapp/src/leaky.tr
printf 'import leaky;\nfn main(): int {\n    return leaky::bad();\n}\n' > myapp/src/leakmain.tr
run tests_helper_from_src --check --module --package-graph gt.json myapp/src/leaky.tr
expect_rc "a src/ module does not see tests/ helpers" tests_helper_from_src 1
expect_out "a src/ module does not see tests/ helpers" tests_helper_from_src "support"
# Without the key a tests/ file is outside the package, as before.
run tests_without_key --check --package-graph g.json myapp/tests/t.tr
expect_rc "a tests/ file with no \"tests\" key" tests_without_key 1
expect_out "a tests/ file with no \"tests\" key" tests_without_key "is not inside the root package"
# Naming tests/ does not loosen src/: the one program there is still main.tr.
run tests_src_not_main --check --package-graph gt.json myapp/src/doubling.tr
expect_rc "src/ still has one entry point when tests/ is named" tests_src_not_main 1
expect_out "src/ still has one entry point when tests/ is named" tests_src_not_main "entry point is the root package's src/main.tr"
bad "tests on a dependency" '{"format":1,"root":"myapp","packages":{"myapp":{"src":"myapp/src","deps":["geometry"]},"geometry":{"src":"geometry/src","tests":"myapp/tests"}}}' "only the root package can have \"tests\""
bad "tests directory that does not exist" '{"format":1,"root":"myapp","packages":{"myapp":{"src":"myapp/src","tests":"nosuch"}}}' "tests directory"
bad "tests that is not a path" '{"format":1,"root":"myapp","packages":{"myapp":{"src":"myapp/src","tests":3}}}' '"tests" must be a directory path'

# --- a root module exposing an internal module's type is E089 -------------------
# Other packages import only a library's root module, so a `pub` type of an
# internal module is one they cannot name. Its root module may not put it in a
# `pub` signature; a private function may use it, and so may the internal
# module's own `pub` items (only the package itself imports those).
mkdir -p leak/src leakapp/src
cat > leak/src/inner.tr <<'EOF'
pub struct Thing {
    pub v: int
}
pub fn make(): Thing {
    return Thing::default();
}
EOF
cat > leak/src/leak.tr <<'EOF'
import inner;
fn private_use(): inner::Thing {
    return inner::make();
}
pub fn value(): int {
    return private_use().v;
}
EOF
printf 'import leak;\nfn main(): int {\n    return leak::value();\n}\n' > leak/src/main.tr
printf 'import leak;\nfn main(): int {\n    return leak::value();\n}\n' > leakapp/src/main.tr
GLL='{"format":1,"root":"leak","packages":{"leak":{"src":"leak/src","deps":[]}}}'
GLA='{"format":1,"root":"leakapp","packages":{
  "leakapp": {"src": "leakapp/src", "deps": ["leak"]},
  "leak":    {"src": "leak/src",    "deps": []}}}'
graph gll.json "$GLL"
graph gla.json "$GLA"
run leak_ok --check --package-graph gll.json leak/src/main.tr
expect_rc "an internal type in a private function of the root module" leak_ok 0
printf 'import inner;\npub fn get(): inner::Thing {\n    return inner::make();\n}\n' > leak/src/leak.tr
printf 'import leak;\nfn main(): int {\n    return leak::get().v;\n}\n' > leak/src/main.tr
cp leak/src/main.tr leakapp/src/main.tr
run leak_lib --check --package-graph gll.json leak/src/main.tr
expect_rc "a root module's pub fn returning an internal type" leak_lib 1
expect_out "a root module's pub fn returning an internal type" leak_lib "[E089"
expect_out "a root module's pub fn returning an internal type" leak_lib "Public function 'get' of package 'leak' exposes 'Thing', a type of its internal module 'inner'."
run leak_dep --check --package-graph gla.json leakapp/src/main.tr
expect_rc "the same, with the library as a dependency" leak_dep 1
expect_out "the same, with the library as a dependency" leak_dep "[E089"
printf 'import inner;\npub struct Box {\n    pub t: inner::Thing\n}\npub fn one(): int {\n    return 1;\n}\n' > leak/src/leak.tr
printf 'import leak;\nfn main(): int {\n    return leak::one();\n}\n' > leak/src/main.tr
run leak_field --check --package-graph gll.json leak/src/main.tr
expect_rc "a root module's pub field of an internal type" leak_field 1
expect_out "a root module's pub field of an internal type" leak_field "Public field 'Box.t' of package 'leak'"

if [ "$FAILED" -ne 0 ]; then
  echo "package_graph_smoke: $FAILED check(s) failed"
  exit 1
fi
echo "package_graph_smoke: all checks passed"
