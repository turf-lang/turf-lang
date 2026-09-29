// Package project turns a root turf.toml into the package graph `turfc
// --package-graph` consumes (include/PackageGraph.h). It is the CLI's half of
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

	"github.com/turf-lang/turf-lang/cli/internal/manifest"
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
}

// Src is the package's source root.
func (p *Package) Src() string { return filepath.Join(p.Dir, "src") }

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
func Resolve(root *manifest.Manifest) (*Graph, error) {
	dir, err := canonical(filepath.Dir(root.Path))
	if err != nil {
		return nil, err
	}
	rp := &Package{Name: root.Package.Name, Dir: dir, Manifest: root, AllowUnsafe: true}
	g := &Graph{Root: rp, Packages: map[string]*Package{rp.Name: rp}}
	w := &walker{g: g, byDir: map[string]*Package{dir: rp}, requirer: map[string]string{rp.Name: "the root package"}}
	if err := w.visit(rp); err != nil {
		return nil, err
	}
	if err := g.applyUnsafe(); err != nil {
		return nil, err
	}
	return g, nil
}

type walker struct {
	g        *Graph
	byDir    map[string]*Package
	requirer map[string]string // package name -> who first asked for it, for the conflict message
	stack    []*Package
}

func (w *walker) visit(p *Package) error {
	w.stack = append(w.stack, p)
	defer func() { w.stack = w.stack[:len(w.stack)-1] }()

	m := p.Manifest
	for _, name := range m.DepOrder {
		d := m.Deps[name]
		if d.Git != "" {
			return fmt.Errorf("git dependencies arrive in LEC-001 stage 6 ('%s' in %s)", name, m.Path)
		}
		declared := d.Path
		if !filepath.IsAbs(declared) {
			declared = filepath.Join(p.Dir, filepath.FromSlash(declared))
		}
		dir, err := canonical(declared)
		if err != nil {
			return fmt.Errorf("dependency '%s' in %s: cannot use path \"%s\": %v", name, m.Path, d.Path, pathErr(err))
		}
		who := fmt.Sprintf("package '%s' (%s)", p.Name, m.Path)

		dep, seen := w.byDir[dir]
		if !seen {
			mp := filepath.Join(dir, ManifestFile)
			dm, err := manifest.Load(mp)
			if err != nil {
				if errors.Is(err, os.ErrNotExist) {
					return fmt.Errorf("dependency '%s' in %s: %s has no %s, so it is not a TurfLang package", name, m.Path, dir, ManifestFile)
				}
				return err
			}
			dep = &Package{Name: dm.Package.Name, Dir: dir, Manifest: dm}
		}
		if dep.Name != name {
			return fmt.Errorf("dependency '%s' (%s) in %s is the package '%s'; the key must be its package name", name, d.Path, m.Path, dep.Name)
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
			return fmt.Errorf("package '%s' is required from two different directories, and a build has one package per name:\n  %s needs it at %s\n  %s needs it at %s",
				dep.Name, w.requirer[dep.Name], other.Dir, who, dep.Dir)
		}
		p.Deps = append(p.Deps, name)
		if seen {
			continue
		}
		w.g.Packages[dep.Name] = dep
		w.byDir[dir] = dep
		w.requirer[dep.Name] = who
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

// JSON renders the graph in the format documented in include/PackageGraph.h,
// with absolute src paths. The output is a pure function of the manifests:
// package keys are sorted (encoding/json sorts map keys) and each dependency
// list keeps the manifest's declaration order, so rebuilding writes identical
// bytes and the file is safe to diff and to commit to a cache key.
func (g *Graph) JSON() ([]byte, error) {
	type pkg struct {
		Src         string   `json:"src"`
		Deps        []string `json:"deps"`
		AllowUnsafe bool     `json:"allow_unsafe"`
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
