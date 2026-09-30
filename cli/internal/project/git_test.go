package project

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/turf-lang/turf-lang/cli/internal/gitdep"
	"github.com/turf-lang/turf-lang/cli/internal/lockfile"
	"github.com/turf-lang/turf-lang/cli/internal/manifest"
)

// fakeGit stands in for the fetcher: it maps "url|ref" to a directory already
// laid out under the test's temp root, with a fixed commit. It records the
// requests it was asked to materialize, which is what the diamond tests are
// about (one fetch per package, however many requirers).
type fakeGit struct {
	trees   map[string]string
	commits map[string]string
	fetched []string
	resolve []string
}

func commit(n string) string { return strings.Repeat(n, 40)[:40] }

func (f *fakeGit) Materialize(r gitdep.Request) (*gitdep.Tree, error) {
	k := r.URL + "|" + r.Ref
	f.fetched = append(f.fetched, k)
	dir, ok := f.trees[k]
	if !ok {
		return nil, &fakeErr{"no such ref " + k}
	}
	return &gitdep.Tree{Dir: dir, Commit: f.commits[k], Hash: "sha256:" + strings.Repeat("0", 64)}, nil
}

func (f *fakeGit) ResolveCommit(r gitdep.Request) (string, error) {
	k := r.URL + "|" + r.Ref
	f.resolve = append(f.resolve, k)
	c, ok := f.commits[k]
	if !ok {
		return "", &fakeErr{"no such ref " + k}
	}
	return c, nil
}

type fakeErr struct{ s string }

func (e *fakeErr) Error() string { return e.s }

func buildGit(t *testing.T, root, rootDir string, git gitdep.Source) (*Graph, error) {
	t.Helper()
	m, err := manifest.Load(filepath.Join(root, rootDir, "turf.toml"))
	if err != nil {
		t.Fatalf("root manifest: %v", err)
	}
	return ResolveWith(m, git)
}

func gitDepLine(url, ref string) string {
	return "{ git = \"" + url + "\", " + ref + " }"
}

func TestGitDependencyIsAPackageAtItsTree(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "jsonx = " + gitDepLine("u/jsonx", `tag = "v1"`) + "\ngeo = " + dep("../geo")},
		pkgSpec{dir: "geo", name: "geo", lib: true},
		pkgSpec{dir: "cache/jsonx-1", name: "jsonx", lib: true},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/jsonx|tag=v1": filepath.Join(root, "cache/jsonx-1")},
		commits: map[string]string{"u/jsonx|tag=v1": commit("a")},
	}
	g, err := buildGit(t, root, "app", git)
	if err != nil {
		t.Fatal(err)
	}
	p := g.Packages["jsonx"]
	if p == nil || p.Dir != filepath.Join(root, "cache/jsonx-1") || p.Git == nil || p.Git.Commit != commit("a") {
		t.Fatalf("jsonx: %+v", p)
	}
	if got := strings.Join(g.Root.Deps, ","); got != "jsonx,geo" {
		t.Errorf("root deps %q", got)
	}
	// The graph JSON needs nothing special: a git package is just a src root.
	data, err := g.JSON()
	if err != nil || !strings.Contains(string(data), filepath.Join(root, "cache/jsonx-1", "src")) {
		t.Fatalf("graph JSON does not point at the extracted tree: %v\n%s", err, data)
	}

	l := g.Lock()
	want := []lockfile.Package{
		{Name: "geo", Source: "path+../geo"},
		{Name: "jsonx", Source: "git+u/jsonx", Ref: "tag=v1", Commit: commit("a"), Hash: "sha256:" + strings.Repeat("0", 64)},
	}
	if len(l.Packages) != 2 || l.Packages[0] != want[0] || l.Packages[1] != want[1] {
		t.Fatalf("Lock() = %+v, want %+v", l.Packages, want)
	}
}

func TestDiamondOnOneGitRefFetchesOnce(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + dep("../a") + "\nb = " + dep("../b")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "c = " + gitDepLine("u/c", `tag = "v1"`)},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "c = " + gitDepLine("u/c", `tag = "v1"`)},
		pkgSpec{dir: "cache/c-1", name: "c", lib: true},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/c|tag=v1": filepath.Join(root, "cache/c-1")},
		commits: map[string]string{"u/c|tag=v1": commit("a")},
	}
	g, err := buildGit(t, root, "app", git)
	if err != nil {
		t.Fatal(err)
	}
	if len(git.fetched) != 1 || len(git.resolve) != 0 {
		t.Errorf("one package, one fetch and no extra resolution; fetched %v resolved %v", git.fetched, git.resolve)
	}
	if g.Packages["a"].Deps[0] != "c" || g.Packages["b"].Deps[0] != "c" {
		t.Error("both requirers must depend on c")
	}
}

// Two spellings of one commit are one package; two commits are the conflict.
func TestDiamondOnTwoRefsOfOneCommitAgrees(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + dep("../a") + "\nb = " + dep("../b")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "c = " + gitDepLine("u/c", `tag = "v1"`)},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "c = " + gitDepLine("u/c", `rev = "aaaa"`)},
		pkgSpec{dir: "cache/c-1", name: "c", lib: true},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/c|tag=v1": filepath.Join(root, "cache/c-1")},
		commits: map[string]string{"u/c|tag=v1": commit("a"), "u/c|rev=aaaa": commit("a")},
	}
	if _, err := buildGit(t, root, "app", git); err != nil {
		t.Fatalf("two refs of one commit must agree: %v", err)
	}
}

func TestDiamondOnTwoCommitsIsAConflictNamingBothRefs(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + dep("../a") + "\nb = " + dep("../b")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "c = " + gitDepLine("u/c", `tag = "v1"`)},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "c = " + gitDepLine("u/c", `tag = "v2"`)},
		pkgSpec{dir: "cache/c-1", name: "c", lib: true},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/c|tag=v1": filepath.Join(root, "cache/c-1")},
		commits: map[string]string{"u/c|tag=v1": commit("a"), "u/c|tag=v2": commit("b")},
	}
	g, err := buildGit(t, root, "app", git)
	mustFail(t, g, err, "package 'c' is required from two different directories",
		"package 'a'", "package 'b'", "tag=v1", "tag=v2", commit("a")[:12], commit("b")[:12])
}

func TestGitAndPathWithOneNameConflict(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + dep("../a") + "\nb = " + dep("../b")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "c = " + gitDepLine("u/c", `tag = "v1"`)},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "c = " + dep("../c")},
		pkgSpec{dir: "c", name: "c", lib: true},
		pkgSpec{dir: "cache/c-1", name: "c", lib: true},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/c|tag=v1": filepath.Join(root, "cache/c-1")},
		commits: map[string]string{"u/c|tag=v1": commit("a")},
	}
	// Git first, path second, and the other way round: both orders name both.
	g, err := buildGit(t, root, "app", git)
	mustFail(t, g, err, "package 'c' is required from two different directories", "git u/c (tag=v1, commit "+commit("a")[:12], filepath.Join(root, "c"))

	root2 := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "b = " + dep("../b") + "\na = " + dep("../a")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "c = " + gitDepLine("u/c", `tag = "v1"`)},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "c = " + dep("../c")},
		pkgSpec{dir: "c", name: "c", lib: true},
	)
	g, err = buildGit(t, root2, "app", &fakeGit{})
	mustFail(t, g, err, "package 'c' is required from two different directories", "git u/c (tag=v1)", filepath.Join(root2, "c"))
}

func TestSameNameFromTwoURLsConflicts(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + dep("../a") + "\nb = " + dep("../b")},
		pkgSpec{dir: "a", name: "a", lib: true, deps: "c = " + gitDepLine("u/one", `tag = "v1"`)},
		pkgSpec{dir: "b", name: "b", lib: true, deps: "c = " + gitDepLine("u/two", `tag = "v1"`)},
		pkgSpec{dir: "cache/c-1", name: "c", lib: true},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/one|tag=v1": filepath.Join(root, "cache/c-1")},
		commits: map[string]string{"u/one|tag=v1": commit("a"), "u/two|tag=v1": commit("a")},
	}
	g, err := buildGit(t, root, "app", git)
	mustFail(t, g, err, "package 'c' is required from two different directories", "git u/one", "git u/two")
}

func TestGitPackageWithoutAManifest(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "c = " + gitDepLine("u/c", `tag = "v1"`)},
	)
	write(t, filepath.Join(root, "cache/c-1/src/c.tr"), "pub fn f(): int { return 1; }\n")
	git := &fakeGit{
		trees:   map[string]string{"u/c|tag=v1": filepath.Join(root, "cache/c-1")},
		commits: map[string]string{"u/c|tag=v1": commit("a")},
	}
	g, err := buildGit(t, root, "app", git)
	mustFail(t, g, err, "dependency 'c' in ", "u/c (tag=v1) has no turf.toml at its root")
}

func TestGitPackageKeyMustBeItsName(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "wrong = " + gitDepLine("u/c", `tag = "v1"`)},
		pkgSpec{dir: "cache/c-1", name: "c", lib: true},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/c|tag=v1": filepath.Join(root, "cache/c-1")},
		commits: map[string]string{"u/c|tag=v1": commit("a")},
	}
	g, err := buildGit(t, root, "app", git)
	mustFail(t, g, err, "dependency 'wrong' (u/c, tag=v1)", "is the package 'c'")
}

func TestGitDependenciesOfGitDependenciesAreFetched(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + gitDepLine("u/a", `tag = "v1"`)},
		pkgSpec{dir: "cache/a-1", name: "a", lib: true, deps: "b = " + gitDepLine("u/b", `branch = "main"`)},
		pkgSpec{dir: "cache/b-1", name: "b", lib: true},
	)
	git := &fakeGit{
		trees: map[string]string{
			"u/a|tag=v1":      filepath.Join(root, "cache/a-1"),
			"u/b|branch=main": filepath.Join(root, "cache/b-1"),
		},
		commits: map[string]string{"u/a|tag=v1": commit("a"), "u/b|branch=main": commit("b")},
	}
	g, err := buildGit(t, root, "app", git)
	if err != nil {
		t.Fatal(err)
	}
	if got := strings.Join(g.Names(), ","); got != "a,app,b" {
		t.Fatalf("packages %q", got)
	}
	if got := g.Lock().Packages[1].Ref; got != "branch=main" {
		t.Errorf("b's ref in the lock is %q", got)
	}
}

// A monorepo sibling reached by path from inside a git tree is covered by that
// tree's hash, so it has no lock entry of its own; going outside the tree is
// the error the spec asks for.
func TestPathDependencyInsideAGitTree(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + gitDepLine("u/mono", `tag = "v1"`)},
		pkgSpec{dir: "cache/mono-1", name: "a", lib: true, deps: "sib = " + dep("sibs/sib")},
		pkgSpec{dir: "cache/mono-1/sibs/sib", name: "sib", lib: true},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/mono|tag=v1": filepath.Join(root, "cache/mono-1")},
		commits: map[string]string{"u/mono|tag=v1": commit("a")},
	}
	g, err := buildGit(t, root, "app", git)
	if err != nil {
		t.Fatal(err)
	}
	if p := g.Packages["sib"]; p == nil || p.TreeRoot == "" {
		t.Fatalf("sib should be a package inside the git tree: %+v", p)
	}
	l := g.Lock()
	if len(l.Packages) != 1 || l.Packages[0].Name != "a" {
		t.Fatalf("only the git package belongs in the lock, got %+v", l.Packages)
	}
}

func TestPathDependencyThatEscapesAGitTree(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + gitDepLine("u/mono", `tag = "v1"`)},
		pkgSpec{dir: "cache/mono-1", name: "a", lib: true, deps: "out = " + dep("../outside")},
		pkgSpec{dir: "cache/outside", name: "out", lib: true},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/mono|tag=v1": filepath.Join(root, "cache/mono-1")},
		commits: map[string]string{"u/mono|tag=v1": commit("a")},
	}
	g, err := buildGit(t, root, "app", git)
	mustFail(t, g, err, "a git dependency's path dependencies must stay inside its repository",
		"dependency 'out' in ", filepath.Join(root, "cache/mono-1", "turf.toml"))

	// An absolute path is outside too, however plausible it looks.
	elsewhere := layout(t, pkgSpec{dir: "x", name: "out", lib: true})
	root = layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + gitDepLine("u/mono", `tag = "v1"`)},
		pkgSpec{dir: "cache/mono-1", name: "a", lib: true, deps: "out = " + dep(filepath.ToSlash(filepath.Join(elsewhere, "x")))},
	)
	git = &fakeGit{
		trees:   map[string]string{"u/mono|tag=v1": filepath.Join(root, "cache/mono-1")},
		commits: map[string]string{"u/mono|tag=v1": commit("a")},
	}
	g, err = buildGit(t, root, "app", git)
	mustFail(t, g, err, "a git dependency's path dependencies must stay inside its repository")
}

// The usual real case: the sibling exists only on the author's machine, so
// nothing is at the path in the cache. It must still be reported as leaving the
// repository, not as a missing directory.
func TestEscapingPathThatDoesNotExistIsStillAnEscape(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + gitDepLine("u/mono", `tag = "v1"`)},
		pkgSpec{dir: "cache/mono-1", name: "a", lib: true, deps: "sib = " + dep("../sib")},
	)
	git := &fakeGit{
		trees:   map[string]string{"u/mono|tag=v1": filepath.Join(root, "cache/mono-1")},
		commits: map[string]string{"u/mono|tag=v1": commit("a")},
	}
	g, err := buildGit(t, root, "app", git)
	mustFail(t, g, err, "must stay inside its repository", "\"../sib\"")
}

func TestSymlinkInAGitTreeCannotLeadOut(t *testing.T) {
	elsewhere := layout(t, pkgSpec{dir: "x", name: "out", lib: true})
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "a = " + gitDepLine("u/mono", `tag = "v1"`)},
		pkgSpec{dir: "cache/mono-1", name: "a", lib: true, deps: "out = " + dep("vendor/out")},
	)
	if err := os.MkdirAll(filepath.Join(root, "cache/mono-1/vendor"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(filepath.Join(elsewhere, "x"), filepath.Join(root, "cache/mono-1/vendor/out")); err != nil {
		t.Skip("symlinks unavailable: ", err)
	}
	git := &fakeGit{
		trees:   map[string]string{"u/mono|tag=v1": filepath.Join(root, "cache/mono-1")},
		commits: map[string]string{"u/mono|tag=v1": commit("a")},
	}
	g, err := buildGit(t, root, "app", git)
	mustFail(t, g, err, "must stay inside its repository")
}

func TestLockOfAPathOnlyProject(t *testing.T) {
	root := layout(t,
		pkgSpec{dir: "app", name: "app", main: true, deps: "zed = " + dep("../zed") + "\nalpha = " + dep("../alpha")},
		pkgSpec{dir: "zed", name: "zed", lib: true},
		pkgSpec{dir: "alpha", name: "alpha", lib: true},
	)
	g, err := build(t, root, "app")
	if err != nil {
		t.Fatal(err)
	}
	got := string(g.Lock().Marshal())
	want := "version = 1\n\n[[package]]\nname = \"alpha\"\nsource = \"path+../alpha\"\n\n[[package]]\nname = \"zed\"\nsource = \"path+../zed\"\n"
	if !strings.HasSuffix(got, want) {
		t.Fatalf("lock:\n%s", got)
	}
	if empty := (&Graph{Root: &Package{}, Packages: map[string]*Package{}}).Lock(); len(empty.Packages) != 0 {
		t.Fatal("no packages, no entries")
	}
}
