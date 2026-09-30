package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/gitdep"
	"github.com/turf-lang/turf-lang/cli/internal/lockfile"
	"github.com/turf-lang/turf-lang/cli/internal/manifest"
	"github.com/turf-lang/turf-lang/cli/internal/project"
)

// netFlags are turf's own flags for the commands that resolve dependencies
// (build, run, check, fetch). They are never forwarded to turfc: the compiler
// never touches the network or the lock (FREEZE.md 6.16, F-PKG-1).
type netFlags struct {
	offline bool // run no network git command
	locked  bool // turf.lock must already be exactly right; write nothing
}

// splitNetFlags takes --offline and --locked out of args and returns the rest,
// which for build/run/check are the compiler flags.
func splitNetFlags(args []string) (netFlags, []string) {
	var f netFlags
	var rest []string
	for _, a := range args {
		switch a {
		case "--offline":
			f.offline = true
		case "--locked":
			f.locked = true
		default:
			rest = append(rest, a)
		}
	}
	return f, rest
}

func lockPath(m *manifest.Manifest) string {
	return filepath.Join(filepath.Dir(m.Path), lockfile.FileName)
}

// readLock reads the project's turf.lock. A missing file is (nil, nil): every
// build without a lock is a first build. A malformed one is an error even
// when the build would not have used it, because a lock that is quietly
// ignored is a lock that quietly stops pinning.
func readLock(m *manifest.Manifest) (*lockfile.Lock, error) {
	l, err := lockfile.Read(lockPath(m))
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	return l, err
}

// resolveGraph resolves m's dependency graph, fetching what the lock does not
// already pin, and returns the graph with the lock that was on disk (nil if
// none). update names the packages whose pin `turf update` is moving.
func resolveGraph(m *manifest.Manifest, nf netFlags, update func(string) bool) (*project.Graph, *lockfile.Lock, error) {
	old, err := readLock(m)
	if err != nil {
		return nil, nil, err
	}
	if nf.locked && old == nil {
		return nil, nil, fmt.Errorf("--locked needs %s, and there is none at %s\nhint: run `turf build` or `turf fetch` once without --locked and commit turf.lock", lockfile.FileName, lockPath(m))
	}
	f := gitdep.NewFetcher(old, nf.offline)
	f.Log = os.Stderr
	f.Update = update
	g, err := project.ResolveWith(m, f)
	if err != nil {
		return nil, nil, err
	}
	return g, old, nil
}

// commitLock is the last step of resolution: with --locked it proves the lock
// on disk is what this graph needs and writes nothing; otherwise it writes the
// lock, unless the bytes are already right (an unchanged file is an untouched
// mtime and a clean `git status`).
//
// It runs after every other check a command makes, so a build that fails on
// something else does not leave a half-updated lock behind.
func commitLock(m *manifest.Manifest, g *project.Graph, old *lockfile.Lock, nf netFlags) error {
	next := g.Lock()
	if nf.locked {
		if diff := lockfile.Diff(old, next); len(diff) > 0 {
			return fmt.Errorf("%s is out of date, and --locked forbids changing it (nothing was written):\n  %s\nhint: run `turf build` or `turf fetch` without --locked, and commit the new %s",
				lockfile.FileName, strings.Join(diff, "\n  "), lockfile.FileName)
		}
		return nil
	}
	data := next.Marshal()
	path := lockPath(m)
	if cur, err := os.ReadFile(path); err == nil && bytes.Equal(cur, data) {
		return nil
	}
	if err := manifest.WriteFileAtomic(path, data); err != nil {
		return fmt.Errorf("cannot write %s: %v", path, err)
	}
	return nil
}
