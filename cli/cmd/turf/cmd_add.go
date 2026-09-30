package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/manifest"
	"github.com/turf-lang/turf-lang/cli/internal/project"
)

// openManifest finds the project from the cwd and reads its turf.toml as text
// and as a validated manifest. Both are returned because add and remove edit
// the text and use the manifest only to decide whether to.
func openManifest() (path string, data []byte, m *manifest.Manifest, code int) {
	cwd, err := os.Getwd()
	if err != nil {
		return "", nil, nil, failf("cannot determine the current directory: %v", err)
	}
	dir, err := project.FindRoot(cwd)
	if errors.Is(err, project.ErrNoProject) {
		return "", nil, nil, usagef("%s", noProjectMsg)
	} else if err != nil {
		return "", nil, nil, failf("%v", err)
	}
	path = filepath.Join(dir, project.ManifestFile)
	if data, err = os.ReadFile(path); err != nil {
		return "", nil, nil, failf("%v", err)
	}
	if m, err = manifest.Parse(path, data); err != nil {
		return "", nil, nil, failf("%v", err)
	}
	return path, data, m, 0
}

func cmdAdd(args []string) int {
	var name, dirArg string
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--path":
			if i+1 >= len(args) {
				return usagef("--path needs a directory (usage: turf add <name> --path <dir>)")
			}
			i++
			dirArg = args[i]
		case strings.HasPrefix(a, "--path="):
			dirArg = strings.TrimPrefix(a, "--path=")
		case a == "--git" || strings.HasPrefix(a, "--git=") || a == "--tag" || a == "--rev" || a == "--branch":
			return usagef("git dependencies arrive in LEC-001 stage 6; use --path <dir> for now")
		case strings.HasPrefix(a, "-"):
			return usagef("unknown flag '%s' (usage: turf add <name> --path <dir>)", a)
		case name != "":
			return usagef("unexpected argument '%s' (usage: turf add <name> --path <dir>)", a)
		default:
			name = a
		}
	}
	if name == "" || dirArg == "" {
		return usagef("usage: turf add <name> --path <dir>")
	}

	tomlPath, data, m, code := openManifest()
	if code != 0 {
		return code
	}
	if _, dup := m.Deps[name]; dup {
		return failf("dependency '%s' is already in [dependencies] of %s", name, tomlPath)
	}

	// The directory is what the user typed in their shell, so it is relative to
	// the cwd; it is stored relative to the project, where the manifest that
	// declares it lives, in forward slashes so the file is the same on every OS.
	abs, err := filepath.Abs(dirArg)
	if err != nil {
		return failf("%v", err)
	}
	stored := filepath.ToSlash(abs)
	if rel, err := filepath.Rel(filepath.Dir(tomlPath), abs); err == nil {
		stored = filepath.ToSlash(rel)
	}

	edited, err := manifest.AddDependency(string(data), name, stored)
	if err != nil {
		return failf("%s: %v", tomlPath, err)
	}
	em, err := manifest.Parse(tomlPath, []byte(edited))
	if err != nil {
		return failf("adding '%s' would make turf.toml invalid, so nothing was written:\n%v", name, err)
	}
	// Resolve the edited manifest exactly as `turf build` will: that is what
	// guarantees `add` never writes a dependency `build` then rejects (a key
	// that is not the package's name, no root module, a cycle, a second
	// directory for a name already in the graph).
	g, _, err := resolveGraph(em, netFlags{}, nil)
	if err != nil {
		return failf("cannot add '%s' (nothing was written): %v", name, err)
	}
	if std := stdlibModules(); std != nil {
		if err := g.CheckStdlibNames(std); err != nil {
			return failf("cannot add '%s' (nothing was written): %v", name, err)
		}
	}
	if err := manifest.WriteFileAtomic(tomlPath, []byte(edited)); err != nil {
		return failf("cannot write %s: %v", tomlPath, err)
	}
	fmt.Printf("Added dependency '%s' (%s) to %s\n", name, stored, tomlPath)
	return 0
}

func cmdRemove(args []string) int {
	if len(args) != 1 || strings.HasPrefix(args[0], "-") {
		return usagef("usage: turf remove <name>")
	}
	name := args[0]

	tomlPath, data, m, code := openManifest()
	if code != 0 {
		return code
	}
	if _, ok := m.Deps[name]; !ok {
		return failf("dependency '%s' is not in [dependencies] of %s", name, tomlPath)
	}
	edited, err := manifest.RemoveDependency(string(data), name)
	if err != nil {
		return failf("%s: %v", tomlPath, err)
	}
	if _, err := manifest.Parse(tomlPath, []byte(edited)); err != nil {
		return failf("removing '%s' would make turf.toml invalid, so nothing was written:\n%v", name, err)
	}
	if err := manifest.WriteFileAtomic(tomlPath, []byte(edited)); err != nil {
		return failf("cannot write %s: %v", tomlPath, err)
	}
	fmt.Printf("Removed dependency '%s' from %s\n", name, tomlPath)
	if slices.Contains(m.UnsafeAllow, name) {
		// The line is the user's to change: turf edits exactly one line, and a
		// stale name is caught by the next build (it is no longer in the graph).
		fmt.Fprintf(os.Stderr, "turf: note: '%s' is still listed in [unsafe] allow in %s; the next build will reject it until you remove it there\n", name, tomlPath)
	}
	return 0
}

const helpAdd = `turf add - add a path dependency

Usage:
  turf add <name> --path <dir>

Adds  <name> = { path = "<dir relative to the project>" }  to [dependencies] in
the project's turf.toml. <dir> is relative to where you run the command. It
must hold a turf.toml whose package is called <name> and a src/<name>.tr root
module: the same checks 'turf build' makes, so 'add' never writes a dependency
that 'build' then rejects. Nothing is written if any check fails, and only one
line of the file changes: comments, blank lines and key order are kept.

--git arrives with the lockfile and the cache in LEC-001 stage 6.
`

const helpRemove = `turf remove - remove a dependency

Usage:
  turf remove <name>

Deletes the line declaring <name> in [dependencies] and nothing else. A
dependency written over several lines (or as a [dependencies.<name>] table) is
refused: edit turf.toml by hand. If <name> is also listed in [unsafe] allow, a
note says so; that line is left for you to change.
`
