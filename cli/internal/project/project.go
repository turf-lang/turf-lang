// Package project turns a root turf.toml into the package graph `turfc
// --package-graph` consumes (src/tooling/PackageGraph.h). It is the CLI's half of
// the seam in docs/development/FREEZE.md 6.16 (F-PKG-1): it owns files and
// manifests, and never looks inside a .tr file. Which module an `import` names
// stays the compiler's decision, made from the graph this package writes.
package project

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"github.com/turf-lang/turf-cli/internal/gitdep"
	"github.com/turf-lang/turf-cli/internal/lockfile"
	"github.com/turf-lang/turf-cli/internal/manifest"
)

// ManifestFile is the project marker.
const ManifestFile = "turf.toml"

// ErrNoProject is returned by FindRoot when no turf.toml is found.
var ErrNoProject = errors.New("not inside a TurfLang project")

// FindRoot walks up from start to the first directory holding turf.toml. The
// first one wins, so a package nested in another project's tree is its own
// project, the way a nested manifest is in every other build tool.
func FindRoot(start string) (string, error) {
	dir, err := filepath.Abs(start)
	if err != nil {
		return "", err
	}
	for {
		if st, err := os.Stat(filepath.Join(dir, ManifestFile)); err == nil && !st.IsDir() {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", ErrNoProject
		}
		dir = parent
	}
}

// Package is one node of the graph.
type Package struct {
	Name        string
	Dir         string // canonical directory holding the package's turf.toml
	Manifest    *manifest.Manifest
	Deps        []string // direct dependencies, in the order the manifest declares them
	AllowUnsafe bool     // whether the compiler honors `unsafe module;` here

	// Git is set for a git dependency: what turf.lock pins for it.
	Git *GitPin
	// TreeRoot is the extracted git tree this package lives in, for the git
	// package itself and for every path dependency reached through it; empty
	// for a package on disk in the project. Such packages are covered by their
	// git ancestor's tree hash, so they get no lock entry of their own, and a
	// path dependency may not leave the tree.
	TreeRoot string
}

// GitPin is the pinned identity of a git dependency.
type GitPin struct {
	URL, Ref, Commit, Hash string
}

// Src is the package's source root.
func (p *Package) Src() string { return filepath.Join(p.Dir, "src") }

// Tests is tests/: the package's test programs, run by `turf test`.
func (p *Package) Tests() string { return filepath.Join(p.Dir, "tests") }

// RootModule is src/<name>.tr: the file that makes a package a library and the
// only module another package can import.
func (p *Package) RootModule() string { return filepath.Join(p.Src(), p.Name+".tr") }

// MainFile is src/main.tr: its presence makes the package build an executable.
func (p *Package) MainFile() string { return filepath.Join(p.Src(), "main.tr") }

func exists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

// IsLibrary reports whether the package has a root module.
func (p *Package) IsLibrary() bool { return exists(p.RootModule()) }

// HasMain reports whether the package has a program entry point.
func (p *Package) HasMain() bool { return exists(p.MainFile()) }

// Graph is the resolved package graph of a build.
type Graph struct {
	Root     *Package
	Packages map[string]*Package
}

// Names returns the package names sorted, for deterministic iteration.
func (g *Graph) Names() []string {
	names := make([]string, 0, len(g.Packages))
	for n := range g.Packages {
		names = append(names, n)
	}
	sort.Strings(names)
	return names
}

func canonical(path string) (string, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	return filepath.EvalSymlinks(abs)
}

// Resolve resolves root's path dependencies, depth first, into one package graph.
//
// A path dependency is relative to the directory of the turf.toml that declares
// it, not to the root's, so a package works the same whichever project pulls it
// in. Directories are canonicalised so that the same package reached through a
// symlink or a `..` detour is one package (a diamond that agrees), while two
// different directories with the same package name are the error F-PKG-3
// forbids: one version per name.
//
// Resolve knows only path dependencies; a git dependency is an error, because
// nothing here can fetch one. ResolveWith is the same walk with a git source.
func Resolve(root *manifest.Manifest) (*Graph, error) { return ResolveWith(root, nil) }

// ResolveWith resolves root's dependencies, fetching git dependencies through
// git. A git dependency's package directory is its extracted cache tree, and
// from there the walk treats it like any other directory (its turf.toml is
// loaded and the same name and library rules apply), so the "one directory per
// name" rule that catches two path versions of a package also catches two
// commits of a git one.
func ResolveWith(root *manifest.Manifest, git gitdep.Source) (*Graph, error) {
	dir, err := canonical(filepath.Dir(root.Path))
	if err != nil {
		return nil, err
	}
	rp := &Package{Name: root.Package.Name, Dir: dir, Manifest: root, AllowUnsafe: true}
	g := &Graph{Root: rp, Packages: map[string]*Package{rp.Name: rp}}
	w := &walker{g: g, git: git, byDir: map[string]*Package{dir: rp}, requirer: map[string]requirer{rp.Name: {who: "the root package", where: dir}}}
	if err := w.visit(rp); err != nil {
		return nil, err
	}
	if err := g.applyUnsafe(); err != nil {
		return nil, err
	}
	return g, nil
}

// requirer records who first asked for a package and where it is, so the
// message for a second, different request can name both.
type requirer struct {
	who   string // "package 'a' (/p/a/turf.toml)"
	where string // a directory, or "git <url> (<ref>, commit <12 hex>)"
}

type walker struct {
	g        *Graph
	git      gitdep.Source // nil: git dependencies cannot be resolved
	byDir    map[string]*Package
	requirer map[string]requirer
	stack    []*Package
}

// found is one dependency edge's target, before the checks every edge shares.
type found struct {
	pkg   *Package
	seen  bool   // already in the graph
	where string // for the conflict message
	spec  string // how the requirer spelled it, for the key/name message
}

func (w *walker) conflict(name string, who, where string) error {
	other := w.requirer[name]
	return fmt.Errorf("package '%s' is required from two different directories, and a build has one package per name:\n  %s needs it at %s\n  %s needs it at %s",
		name, other.who, other.where, who, where)
}

// pathDep resolves a `path` dependency of p. A path dependency of a package
// inside a git tree is resolved against that tree and must stay inside it: the
// tree hash pins the tree, so a path that escaped it would be an unpinned
// input to the build.
func (w *walker) pathDep(p *Package, name string, d manifest.Dep) (found, error) {
	m := p.Manifest
	declared := d.Path
	if !filepath.IsAbs(declared) {
		declared = filepath.Join(p.Dir, filepath.FromSlash(declared))
	}
	escapes := func(at string) error {
		return fmt.Errorf("dependency '%s' in %s: a git dependency's path dependencies must stay inside its repository (\"%s\" is %s, outside %s)", name, m.Path, d.Path, at, p.TreeRoot)
	}
	// Checked twice. Lexically first, so `../x` is reported as leaving the
	// repository even when nothing exists there (the usual case: the directory
	// only exists on the author's machine), and again after symlinks are
	// resolved, because a link inside the tree can point out of it.
	if p.TreeRoot != "" && !within(p.TreeRoot, filepath.Clean(declared)) {
		return found{}, escapes(filepath.Clean(declared))
	}
	dir, err := canonical(declared)
	if err != nil {
		return found{}, fmt.Errorf("dependency '%s' in %s: cannot use path \"%s\": %v", name, m.Path, d.Path, pathErr(err))
	}
	if p.TreeRoot != "" && !within(p.TreeRoot, dir) {
		return found{}, escapes(dir)
	}
	dep, seen := w.byDir[dir]
	if !seen {
		dm, err := manifest.Load(filepath.Join(dir, ManifestFile))
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				return found{}, fmt.Errorf("dependency '%s' in %s: %s has no %s, so it is not a TurfLang package", name, m.Path, dir, ManifestFile)
			}
			return found{}, err
		}
		dep = &Package{Name: dm.Package.Name, Dir: dir, Manifest: dm, TreeRoot: p.TreeRoot}
	}
	return found{pkg: dep, seen: seen, where: dir, spec: d.Path}, nil
}

// within reports whether path is root or below it.
func within(root, path string) bool {
	rel, err := filepath.Rel(root, path)
	return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}

func pinWhere(url, ref, commit string) string {
	return fmt.Sprintf("git %s (%s, commit %s)", url, ref, commit[:12])
}

// gitDep resolves a `git` dependency of p.
//
// A package that is already in the graph is not fetched again. From the same
// URL and the same ref string it is the very same request. From the same URL
// with a different ref string (a tag and a rev, or two tags) the new ref is
// resolved and compared: two spellings of one commit agree, two commits are
// the one-version-per-name conflict. Any other source for the name is a
// conflict outright.
func (w *walker) gitDep(p *Package, name string, d manifest.Dep, who string) (found, error) {
	m := p.Manifest
	if w.git == nil {
		return found{}, fmt.Errorf("dependency '%s' in %s is a git dependency, and this resolution has no git source to fetch it with", name, m.Path)
	}
	req := gitdep.Request{Name: name, URL: d.Git, Ref: d.RefSpec(), File: m.Path}
	spec := d.Git + ", " + req.Ref
	if existing, ok := w.g.Packages[name]; ok {
		pin := existing.Git
		if pin == nil || pin.URL != d.Git {
			return found{}, w.conflict(name, who, fmt.Sprintf("git %s (%s)", d.Git, req.Ref))
		}
		if pin.Ref == req.Ref {
			return found{pkg: existing, seen: true, where: pinWhere(pin.URL, pin.Ref, pin.Commit), spec: spec}, nil
		}
		commit, err := w.git.ResolveCommit(req)
		if err != nil {
			return found{}, err
		}
		if commit != pin.Commit {
			return found{}, w.conflict(name, who, pinWhere(d.Git, req.Ref, commit))
		}
		return found{pkg: existing, seen: true, where: pinWhere(d.Git, req.Ref, commit), spec: spec}, nil
	}
	tree, err := w.git.Materialize(req)
	if err != nil {
		return found{}, err
	}
	dir, err := canonical(tree.Dir)
	if err != nil {
		return found{}, err
	}
	dm, err := manifest.Load(filepath.Join(dir, ManifestFile))
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return found{}, fmt.Errorf("dependency '%s' in %s: %s (%s) has no %s at its root, so it is not a TurfLang package", name, m.Path, d.Git, req.Ref, ManifestFile)
		}
		return found{}, err
	}
	dep := &Package{Name: dm.Package.Name, Dir: dir, Manifest: dm, TreeRoot: dir,
		Git: &GitPin{URL: d.Git, Ref: req.Ref, Commit: tree.Commit, Hash: tree.Hash}}
	return found{pkg: dep, where: pinWhere(d.Git, req.Ref, tree.Commit), spec: spec}, nil
}

func (w *walker) visit(p *Package) error {
	w.stack = append(w.stack, p)
	defer func() { w.stack = w.stack[:len(w.stack)-1] }()

	m := p.Manifest
	for _, name := range m.DepOrder {
		d := m.Deps[name]
		who := fmt.Sprintf("package '%s' (%s)", p.Name, m.Path)
		var (
			f   found
			err error
		)
		if d.Git != "" {
			f, err = w.gitDep(p, name, d, who)
		} else {
			f, err = w.pathDep(p, name, d)
		}
		if err != nil {
			return err
		}
		dep, seen := f.pkg, f.seen
		if dep.Name != name {
			return fmt.Errorf("dependency '%s' (%s) in %s is the package '%s'; the key must be its package name", name, f.spec, m.Path, dep.Name)
		}
		if !dep.IsLibrary() {
			return fmt.Errorf("dependency '%s' in %s is not a library package: it has no src/%s.tr", name, m.Path, name)
		}
		// A cycle is checked before "already loaded" so that a package that is
		// merely in progress is not mistaken for a finished diamond arm.
		for i, s := range w.stack {
			if s == dep {
				cycle := make([]string, 0, len(w.stack)-i+1)
				for _, c := range w.stack[i:] {
					cycle = append(cycle, c.Name)
				}
				cycle = append(cycle, dep.Name)
				return fmt.Errorf("dependency cycle between packages: %s", strings.Join(cycle, " -> "))
			}
		}
		if other, ok := w.g.Packages[dep.Name]; ok && other.Dir != dep.Dir {
			return w.conflict(dep.Name, who, f.where)
		}
		p.Deps = append(p.Deps, name)
		if seen {
			continue
		}
		w.g.Packages[dep.Name] = dep
		w.byDir[dep.Dir] = dep
		w.requirer[dep.Name] = requirer{who: who, where: f.where}
		if err := w.visit(dep); err != nil {
			return err
		}
	}
	return nil
}

// pathErr trims the noise Go puts around a filesystem error.
func pathErr(err error) string {
	var pe *os.PathError
	if errors.As(err, &pe) {
		return pe.Err.Error()
	}
	return err.Error()
}

// applyUnsafe sets the graph's allow_unsafe bits from the ROOT manifest only
// (F-PKG-4): a dependency's own [unsafe] table is deliberately never read, so
// a package cannot grant itself, or anything below it, the capability. The
// root's own modules need no listing.
func (g *Graph) applyUnsafe() error {
	root := g.Root.Manifest
	for _, n := range root.UnsafeAllow {
		if n == g.Root.Name {
			return fmt.Errorf("%s: [unsafe] allow lists the root package '%s': its own modules need no listing", root.Path, n)
		}
		p, ok := g.Packages[n]
		if !ok {
			return fmt.Errorf("%s: [unsafe] allow names '%s', which is not a package in this project's dependency graph\nhint: list only packages the build actually depends on, directly or transitively", root.Path, n)
		}
		p.AllowUnsafe = true
	}
	return nil
}

// Lock is what turf.lock must say for this graph: one entry per package but
// the root, path packages by their directory relative to the root and git
// packages by URL, ref, commit and tree hash.
//
// A package inside a git tree (a monorepo sibling the git package depends on
// by path) gets no entry: its bytes are already pinned by the tree hash of the
// git package that contains it, and the only path it could be named by is one
// inside the cache, which differs on every machine.
func (g *Graph) Lock() *lockfile.Lock {
	l := &lockfile.Lock{}
	for _, n := range g.Names() {
		p := g.Packages[n]
		switch {
		case p == g.Root:
		case p.Git != nil:
			l.Packages = append(l.Packages, lockfile.Package{
				Name: n, Source: lockfile.GitSource(p.Git.URL), Ref: p.Git.Ref, Commit: p.Git.Commit, Hash: p.Git.Hash,
			})
		case p.TreeRoot != "":
		default:
			rel := filepath.ToSlash(p.Dir)
			if r, err := filepath.Rel(g.Root.Dir, p.Dir); err == nil {
				rel = filepath.ToSlash(r)
			}
			l.Packages = append(l.Packages, lockfile.Package{Name: n, Source: lockfile.PathSource(rel)})
		}
	}
	return l
}

// CheckStdlibNames rejects a package that could never be imported because its
// name is a stdlib module's (F-PKG-2): a dependency named like one, or a
// library whose own name is. turfc rejects the dependency case too, but from
// the graph file, where the message cannot name the turf.toml that caused it.
func (g *Graph) CheckStdlibNames(std map[string]bool) error {
	for _, n := range g.Names() {
		p := g.Packages[n]
		if !std[n] || !p.IsLibrary() {
			continue
		}
		if p == g.Root {
			return fmt.Errorf("package '%s' (%s) is a library named like the standard-library module '%s', so `import %s;` could never reach it\nhint: rename the package", n, p.Manifest.Path, n, n)
		}
		return fmt.Errorf("dependency '%s' (%s) has the name of the standard-library module '%s', so `import %s;` could never reach it\nhint: rename the package", n, p.Manifest.Path, n, n)
	}
	return nil
}

// JSON renders the graph in the format documented in src/tooling/PackageGraph.h,
// with absolute src paths. The output is a pure function of the manifests:
// package keys are sorted (encoding/json sorts map keys) and each dependency
// list keeps the manifest's declaration order, so rebuilding writes identical
// bytes and the file is safe to diff and to commit to a cache key.
func (g *Graph) JSON() ([]byte, error) { return g.json(false) }

// JSONWithTests is JSON plus the root package's tests/ directory, which is what
// `turfc --test` needs and no other compiler run should be given: a turfc older
// than driver interface 1.3 rejects the key as unknown, so `turf build` must
// keep writing the graph without it.
func (g *Graph) JSONWithTests() ([]byte, error) { return g.json(true) }

func (g *Graph) json(withTests bool) ([]byte, error) {
	type pkg struct {
		Src         string   `json:"src"`
		Deps        []string `json:"deps"`
		AllowUnsafe bool     `json:"allow_unsafe"`
		Tests       string   `json:"tests,omitempty"`
	}
	out := struct {
		Format   int            `json:"format"`
		Root     string         `json:"root"`
		Packages map[string]pkg `json:"packages"`
	}{Format: 1, Root: g.Root.Name, Packages: map[string]pkg{}}
	for name, p := range g.Packages {
		deps := p.Deps
		if deps == nil {
			deps = []string{}
		}
		out.Packages[name] = pkg{Src: p.Src(), Deps: deps, AllowUnsafe: p.AllowUnsafe}
		if withTests && p == g.Root {
			e := out.Packages[name]
			e.Tests = p.Tests()
			out.Packages[name] = e
		}
	}
	b, err := json.MarshalIndent(out, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(b, '\n'), nil
}

// CheckCompilerVersion enforces every package's `turf = "M.m"` against the
// compiler's release version: the major must match and the compiler must be at
// least M.m. Any `-suffix` on the release ("2.0.0-dev") is ignored, so a
// development build satisfies the requirement of the version it is heading for.
// Dependencies are checked too: a library that needs a newer compiler than the
// root does still cannot be built by an older one.
func (g *Graph) CheckCompilerVersion(release, compiler string) error {
	major, minor, err := parseRelease(release)
	if err != nil {
		return fmt.Errorf("cannot read the compiler's release version %q (%s): %v", release, compiler, err)
	}
	for _, n := range g.Names() {
		p := g.Packages[n]
		req := p.Manifest.Package.Turf
		if req == "" {
			continue
		}
		rmaj, rmin, err := parseRelease(req)
		if err != nil {
			return fmt.Errorf("%s: `turf = \"%s\"`: %v", p.Manifest.Path, req, err)
		}
		if rmaj != major {
			return fmt.Errorf("package '%s' (%s) requires turf \"%s\", but the compiler is turfc %s (%s): a different major version\nhint: use a turfc %d.x, or change `turf` in that manifest", n, p.Manifest.Path, req, release, compiler, rmaj)
		}
		if rmin > minor {
			return fmt.Errorf("package '%s' (%s) requires turf \"%s\", but the compiler is turfc %s (%s), which is older\nhint: upgrade turfc, or lower `turf` in that manifest", n, p.Manifest.Path, req, release, compiler)
		}
	}
	return nil
}

// ReleaseMajorMinor reads the "major.minor" out of a release version such as
// "2.0.0-dev", for `turf new` to write the `turf = "M.m"` pin.
func ReleaseMajorMinor(release string) (major, minor int, err error) {
	return parseRelease(release)
}

func parseRelease(s string) (major, minor int, err error) {
	if i := strings.IndexAny(s, "-+"); i >= 0 {
		s = s[:i]
	}
	parts := strings.Split(s, ".")
	if len(parts) < 2 {
		return 0, 0, errors.New("expected <major>.<minor>")
	}
	if major, err = strconv.Atoi(parts[0]); err != nil {
		return 0, 0, errors.New("the major version is not a number")
	}
	if minor, err = strconv.Atoi(parts[1]); err != nil {
		return 0, 0, errors.New("the minor version is not a number")
	}
	return major, minor, nil
}
