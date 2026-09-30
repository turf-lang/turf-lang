package project

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/turf-lang/turf-lang/cli/internal/manifest"
)

// pkgSpec describes one package to lay out under the temp root.
type pkgSpec struct {
	dir  string // relative to the temp root
	name string
	deps string // raw lines for the [dependencies] table
	tail string // raw text appended after it (e.g. an [unsafe] table)
	lib  bool   // create src/<name>.tr
	main bool   // create src/main.tr
}

func write(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func layout(t *testing.T, specs ...pkgSpec) string {
	t.Helper()
	// EvalSymlinks: on macOS the temp dir is itself behind a symlink, and the
	// graph reports canonical paths.
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	for _, s := range specs {
		dir := filepath.Join(root, s.dir)
		write(t, filepath.Join(dir, "turf.toml"),
			"[package]\nname = \""+s.name+"\"\nversion = \"0.1.0\"\n\n[dependencies]\n"+s.deps+"\n"+s.tail)
		if s.lib {
			write(t, filepath.Join(dir, "src", s.name+".tr"), "pub fn f(): int { return 1; }\n")
		}
		if s.main {
			write(t, filepath.Join(dir, "src", "main.tr"), "fn main(): int { return 0; }\n")
		}
	}
	return root
}

func build(t *testing.T, root, rootDir string) (*Graph, error) {
	t.Helper()
	m, err := manifest.Load(filepath.Join(root, rootDir, "turf.toml"))
	if err != nil {
		t.Fatalf("root manifest: %v", err)
	}
	return Resolve(m)
}

func dep(path string) string { return "{ path = \"" + path + "\" }" }

func mustFail(t *testing.T, g *Graph, err error, frags ...string) {
	t.Helper()
	if err == nil {
		t.Fatalf("expected an error containing %q, got a graph", frags)
	}
	for _, f := range frags {
		if !strings.Contains(err.Error(), f) {
			t.Errorf("error %q lacks %q", err.Error(), f)
		}
	}
}

func TestDiamondThatAgrees(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + dep("../a") + "\nb = " + dep("../b")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "c = " + dep("../c")},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "c = " + dep("../c")},
		pkgSpec{dir: "c", name: "c", lib: true},
	)
	g, err := build(t, root, "app")
	if err != nil {
		t.Fatal(err)
	}
	if got := strings.Join(g.Names(), ","); got != "a,app,b,c" {
		t.Fatalf("packages = %s", got)
	}
	if got := strings.Join(g.Packages["a"].Deps, ","); got != "c" {
		t.Errorf("a deps = %s", got)
	}
	if got := strings.Join(g.Root.Deps, ","); got != "a,b" {
		t.Errorf("root deps = %s, want declaration order a,b", got)
	}
	if !g.Root.AllowUnsafe || g.Packages["c"].AllowUnsafe {
		t.Errorf("only the root is unsafe by default: root=%v c=%v", g.Root.AllowUnsafe, g.Packages["c"].AllowUnsafe)
	}
}

func TestSamePackageThroughASymlinkIsOnePackage(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + dep("../a") + "\nb = " + dep("../b")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "c = " + dep("../c")},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "c = " + dep("../c-link")},
		pkgSpec{dir: "c", name: "c", lib: true},
	)
	if err := os.Symlink(filepath.Join(root, "c"), filepath.Join(root, "c-link")); err != nil {
		t.Skip("symlinks unavailable: ", err)
	}
	if _, err := build(t, root, "app"); err != nil {
		t.Fatalf("the same directory reached twice must be fine: %v", err)
	}
}

func TestNameConflict(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + dep("../a") + "\nb = " + dep("../b")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "c = " + dep("../c1")},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "c = " + dep("../c2")},
		pkgSpec{dir: "c1", name: "c", lib: true},
		pkgSpec{dir: "c2", name: "c", lib: true},
	)
	g, err := build(t, root, "app")
	mustFail(t, g, err, "package 'c' is required from two different directories",
		"package 'a'", "package 'b'", filepath.Join(root, "c1"), filepath.Join(root, "c2"))
}

func TestCycle(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + dep("../a")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "b = " + dep("../b")},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "a = " + dep("../a")},
	)
	g, err := build(t, root, "app")
	mustFail(t, g, err, "dependency cycle", "a -> b -> a")
}

func TestCycleBackToTheRoot(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", lib: true, main: true, deps: "a = " + dep("../a")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "app = " + dep("../app")},
	)
	g, err := build(t, root, "app")
	mustFail(t, g, err, "dependency cycle", "app -> a -> app")
}

func TestKeyNameMismatch(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "geometry = " + dep("../geo")},
		pkgSpec{dir: "geo", name: "geo", lib: true},
	)
	g, err := build(t, root, "app")
	mustFail(t, g, err, "dependency 'geometry' (../geo)", "is the package 'geo'", "the key must be its package name")
}

func TestNotALibrary(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "tool = " + dep("../tool")},
		pkgSpec{dir: "tool", name: "tool", main: true},
	)
	g, err := build(t, root, "app")
	mustFail(t, g, err, "'tool'", "is not a library package: it has no src/tool.tr")
}

// Resolve, with no git source, cannot fetch anything, and says which entry
// asked for it.
func TestGitDependencyWithoutASource(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "net = { git = \"https://x/net\", tag = \"v1\" }"},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "deep = { git = \"u\", rev = \"abcd\" }"},
	)
	g, err := build(t, root, "app")
	mustFail(t, g, err, "dependency 'net' in ", "is a git dependency", filepath.Join(root, "app", "turf.toml"))
}

func TestMissingDirectoryAndManifest(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "gone = " + dep("../gone")},
	)
	g, err := build(t, root, "app")
	mustFail(t, g, err, "dependency 'gone'", "cannot use path \"../gone\"")

	root = layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "bare = " + dep("../bare")},
	)
	if err := os.MkdirAll(filepath.Join(root, "bare"), 0o755); err != nil {
		t.Fatal(err)
	}
	g, err = build(t, root, "app")
	mustFail(t, g, err, "dependency 'bare'", "has no turf.toml")
}

func TestUnsafe(t *testing.T) {
	specs := func(tail string) []pkgSpec {
		return []pkgSpec{
			{dir: "app", name: "app", main: true, deps: "a = " + dep("../a"), tail: tail},
			{dir: "a", name: "a", lib: true, deps: "c = " + dep("../c"), tail: "[unsafe]\nallow = [\"c\"]\n"},
			{dir: "c", name: "c", lib: true},
		}
	}
	t.Run("allow grants exactly the listed package", func(t *testing.T) {
		root := layout(t, specs("[unsafe]\nallow = [\"c\"]\n")...)
		g, err := build(t, root, "app")
		if err != nil {
			t.Fatal(err)
		}
		if !g.Packages["c"].AllowUnsafe || g.Packages["a"].AllowUnsafe || !g.Root.AllowUnsafe {
			t.Errorf("app=%v a=%v c=%v", g.Root.AllowUnsafe, g.Packages["a"].AllowUnsafe, g.Packages["c"].AllowUnsafe)
		}
	})
	t.Run("a dependency's own [unsafe] is ignored", func(t *testing.T) {
		root := layout(t, specs("")...)
		g, err := build(t, root, "app")
		if err != nil {
			t.Fatal(err)
		}
		if g.Packages["c"].AllowUnsafe {
			t.Error("a's [unsafe] allow must not grant c: only the root manifest speaks for the build")
		}
	})
	t.Run("an unknown name", func(t *testing.T) {
		root := layout(t, specs("[unsafe]\nallow = [\"nope\"]\n")...)
		g, err := build(t, root, "app")
		mustFail(t, g, err, "[unsafe] allow names 'nope'", "not a package in this project's dependency graph")
	})
	t.Run("the root listed", func(t *testing.T) {
		root := layout(t, specs("[unsafe]\nallow = [\"app\"]\n")...)
		g, err := build(t, root, "app")
		mustFail(t, g, err, "lists the root package 'app'", "its own modules need no listing")
	})
}

func TestRelativePathsFromANestedManifest(t *testing.T) {
	// a's dependency path is relative to a's turf.toml, not to the root's.
	root := layout(t,
		pkgSpec{dir: "ws/app", name: "app", main: true, deps: "a = " + dep("../../libs/a")},
		pkgSpec{dir: "libs/a", name: "a", lib: true, deps: "b = " + dep("../b")},
		pkgSpec{dir: "libs/b", name: "b", lib: true},
	)
	g, err := build(t, root, "ws/app")
	if err != nil {
		t.Fatal(err)
	}
	if got := g.Packages["b"].Dir; got != filepath.Join(root, "libs", "b") {
		t.Errorf("b resolved to %s", got)
	}
}

func TestJSONIsDeterministicAndShaped(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "zed = " + dep("../zed") + "\nalpha = " + dep("../alpha"),
			tail: "[unsafe]\nallow = [\"alpha\"]\n"},
		pkgSpec{dir: "zed", name: "zed", lib: true},
		pkgSpec{dir: "alpha", name: "alpha", lib: true},
	)
	var first []byte
	for i := 0; i < 3; i++ {
		g, err := build(t, root, "app")
		if err != nil {
			t.Fatal(err)
		}
		b, err := g.JSON()
		if err != nil {
			t.Fatal(err)
		}
		if first == nil {
			first = b
		} else if !bytes.Equal(first, b) {
			t.Fatalf("rebuild %d wrote different bytes:\n%s\nvs\n%s", i, first, b)
		}
	}
	var got struct {
		Format   int
		Root     string
		Packages map[string]struct {
			Src         string
			Deps        []string
			AllowUnsafe bool `json:"allow_unsafe"`
		}
	}
	if err := json.Unmarshal(first, &got); err != nil {
		t.Fatalf("not JSON: %v\n%s", err, first)
	}
	if got.Format != 1 || got.Root != "app" {
		t.Errorf("format/root = %d/%s", got.Format, got.Root)
	}
	app := got.Packages["app"]
	if strings.Join(app.Deps, ",") != "zed,alpha" {
		t.Errorf("app deps = %v, want the declared order zed,alpha", app.Deps)
	}
	if !filepath.IsAbs(app.Src) || app.Src != filepath.Join(root, "app", "src") {
		t.Errorf("app src = %s", app.Src)
	}
	if got.Packages["zed"].Deps == nil {
		t.Errorf("a package without dependencies must serialise deps as [], not null")
	}
	if !got.Packages["alpha"].AllowUnsafe || got.Packages["zed"].AllowUnsafe || !app.AllowUnsafe {
		t.Errorf("allow_unsafe wrong: %+v", got.Packages)
	}
}

func TestCheckStdlibNames(t *testing.T) {
	std := map[string]bool{"list": true, "io": true}
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "list = " + dep("../list")},
		pkgSpec{dir: "list", name: "list", lib: true},
	)
	g, err := build(t, root, "app")
	if err != nil {
		t.Fatal(err)
	}
	mustFail(t, g, g.CheckStdlibNames(std), "dependency 'list'", "standard-library module 'list'")

	root = layout(t, pkgSpec{dir: "io", name: "io", lib: true, main: true})
	g, err = build(t, root, "io")
	if err != nil {
		t.Fatal(err)
	}
	mustFail(t, g, g.CheckStdlibNames(std), "package 'io'", "a library named like the standard-library module 'io'")

	// A program that merely shares a stdlib name is not importable by anyone,
	// so the name costs nothing.
	root = layout(t, pkgSpec{dir: "io", name: "io", main: true})
	g, err = build(t, root, "io")
	if err != nil {
		t.Fatal(err)
	}
	if err := g.CheckStdlibNames(std); err != nil {
		t.Errorf("a program-only package named like a stdlib module is fine: %v", err)
	}
}

func TestCheckCompilerVersion(t *testing.T) {
	mk := func(turf string) *Graph {
		root := layout(t, pkgSpec{dir: "app", name: "app", main: true})
		m, err := manifest.Load(filepath.Join(root, "app", "turf.toml"))
		if err != nil {
			t.Fatal(err)
		}
		m.Package.Turf = turf
		g, err := Resolve(m)
		if err != nil {
			t.Fatal(err)
		}
		return g
	}
	tests := []struct {
		req, release string
		frag         string // "" means accepted
	}{
		{"", "2.0.0-dev", ""},
		{"2.0", "2.0.0-dev", ""},
		{"2.0", "2.3.1", ""},
		{"2.1", "2.0.0", "which is older"},
		{"9.0", "2.0.0-dev", "a different major version"},
		{"1.0", "2.0.0", "a different major version"},
	}
	for _, tc := range tests {
		err := mk(tc.req).CheckCompilerVersion(tc.release, "/x/turfc")
		if tc.frag == "" {
			if err != nil {
				t.Errorf("req %q vs %s: unexpected %v", tc.req, tc.release, err)
			}
			continue
		}
		if err == nil || !strings.Contains(err.Error(), tc.frag) ||
			!strings.Contains(err.Error(), tc.req) || !strings.Contains(err.Error(), tc.release) {
			t.Errorf("req %q vs %s: want an error naming both versions and %q, got %v", tc.req, tc.release, tc.frag, err)
		}
	}
}

func TestFindRoot(t *testing.T) {
	root := layout(t, pkgSpec{dir: "app", name: "app", main: true})
	deep := filepath.Join(root, "app", "src", "a", "b")
	if err := os.MkdirAll(deep, 0o755); err != nil {
		t.Fatal(err)
	}
	got, err := FindRoot(deep)
	if err != nil || got != filepath.Join(root, "app") {
		t.Fatalf("FindRoot = %q, %v", got, err)
	}
	// The nearest manifest wins.
	write(t, filepath.Join(root, "app", "src", "a", "turf.toml"), "")
	got, _ = FindRoot(deep)
	if got != filepath.Join(root, "app", "src", "a") {
		t.Fatalf("nearest turf.toml should win, got %q", got)
	}
	if _, err := FindRoot(t.TempDir()); !errors.Is(err, ErrNoProject) {
		// A stray turf.toml above the system temp dir would make this fail
		// for a reason unrelated to the code; say so.
		t.Skipf("no project expected above the temp dir, got %v", err)
	}
}
