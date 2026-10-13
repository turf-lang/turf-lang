package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"github.com/turf-lang/turf-cli/internal/manifest"
	"github.com/turf-lang/turf-cli/internal/project"
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
	const usage = "usage: turf add <name> --path <dir>\n       turf add <name> --git <url> (--tag <tag> | --branch <branch> | --rev <commit>)"
	var name, dirArg, gitURL string
	refs := map[string]string{} // "tag" | "branch" | "rev" -> value
	valueFlags := map[string]func(string) string{
		"--path": func(v string) string { dirArg = v; return "" },
		"--git":  func(v string) string { gitURL = v; return "" },
	}
	for _, kind := range []string{"tag", "branch", "rev"} {
		valueFlags["--"+kind] = func(v string) string {
			if _, twice := refs[kind]; twice {
				return fmt.Sprintf("--%s is given twice", kind)
			}
			refs[kind] = v
			return ""
		}
	}
	for i := 0; i < len(args); i++ {
		a := args[i]
		if a == "--offline" || a == "--locked" {
			return usagef("'add' does not take %s: adding a git dependency has to fetch it", a)
		}
		flag, val, hasEq := strings.Cut(a, "=")
		if set, ok := valueFlags[flag]; ok {
			if !hasEq {
				if i+1 >= len(args) {
					return usagef("%s needs a value\n%s", flag, usage)
				}
				i++
				val = args[i]
			}
			if msg := set(val); msg != "" {
				return usagef("%s", msg)
			}
			continue
		}
		switch {
		case strings.HasPrefix(a, "-"):
			return usagef("unknown flag '%s'\n%s", a, usage)
		case name != "":
			return usagef("unexpected argument '%s'\n%s", a, usage)
		default:
			name = a
		}
	}
	switch {
	case name == "":
		return usagef("%s", usage)
	case gitURL != "" && dirArg != "":
		return usagef("--path and --git are alternatives: a dependency is one or the other")
	case gitURL == "" && len(refs) > 0:
		return usagef("--tag, --branch and --rev only apply with --git")
	case gitURL == "" && dirArg == "":
		return usagef("%s", usage)
	case gitURL != "" && len(refs) != 1:
		return usagef("a git dependency needs exactly one of --tag, --branch or --rev, and got %d\nhint: pin a release with --tag, or a commit with --rev", len(refs))
	}
	for kind, v := range refs {
		if v == "" {
			return usagef("--%s needs a value", kind)
		}
	}

	tomlPath, data, m, code := openManifest()
	if code != 0 {
		return code
	}
	if _, dup := m.Deps[name]; dup {
		return failf("dependency '%s' is already in [dependencies] of %s", name, tomlPath)
	}

	var edited, what string
	var err error
	if gitURL != "" {
		var kind, ref string
		for kind, ref = range refs { // exactly one, checked above
		}
		edited, err = manifest.AddGitDependency(string(data), name, gitURL, kind, ref)
		what = fmt.Sprintf("git %s, %s=%s", gitURL, kind, ref)
	} else {
		// The directory is what the user typed in their shell, so it is relative to
		// the cwd; it is stored relative to the project, where the manifest that
		// declares it lives, in forward slashes so the file is the same on every OS.
		abs, aerr := filepath.Abs(dirArg)
		if aerr != nil {
			return failf("%v", aerr)
		}
		stored := filepath.ToSlash(abs)
		if rel, rerr := filepath.Rel(filepath.Dir(tomlPath), abs); rerr == nil {
			stored = filepath.ToSlash(rel)
		}
		edited, err = manifest.AddDependency(string(data), name, stored)
		what = stored
	}
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
	// directory for a name already in the graph). For a git dependency this is
	// also what fetches and extracts it first, and checks its package name and
	// root module, before a line is written.
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
	if p := g.Packages[name]; p != nil && p.Git != nil {
		what += ", commit " + p.Git.Commit[:12]
	}
	// turf.lock is not touched: the next build pins what this resolved.
	fmt.Printf("Added dependency '%s' (%s) to %s\n", name, what, tomlPath)
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

const helpAdd = `turf add - add a dependency

Usage:
  turf add <name> --path <dir>
  turf add <name> --git <url> (--tag <tag> | --branch <branch> | --rev <commit>)

Adds  <name> = { path = "<dir relative to the project>" }  or
<name> = { git = "<url>", tag = "<tag>" }  to [dependencies] in the project's
turf.toml. For --path, <dir> is relative to where you run the command. It must
hold a turf.toml whose package is called <name> and a src/<name>.tr root
module: the same checks 'turf build' makes, so 'add' never writes a dependency
that 'build' then rejects. Nothing is written if any check fails, and only one
line of the file changes: comments, blank lines and key order are kept.

--git needs exactly one of --tag, --branch and --rev. The repository is fetched
and extracted into the cache first, and its root turf.toml and root module are
checked, so the network is needed; turf.lock is updated by the next build (or
'turf fetch'), which pins the commit. --offline and --locked are not accepted.
`

const helpRemove = `turf remove - remove a dependency

Usage:
  turf remove <name>

Deletes the line declaring <name> in [dependencies] and nothing else. A
dependency written over several lines (or as a [dependencies.<name>] table) is
refused: edit turf.toml by hand. If <name> is also listed in [unsafe] allow, a
note says so; that line is left for you to change.
`
