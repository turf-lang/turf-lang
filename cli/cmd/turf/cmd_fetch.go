package main

import (
	"fmt"
)

// cmdFetch does everything `turf build` does up to and including writing
// turf.lock (resolve, fetch, extract, verify) and then stops. It needs no
// compiler, so a CI step or a Docker layer can warm the cache before a compiler
// is even installed. What it skips is exactly what only the compiler can check
// (its version, the stdlib names), which the build still does.
func cmdFetch(args []string) int {
	nf, rest := splitNetFlags(args)
	if len(rest) > 0 {
		return usagef("'fetch' takes no arguments besides --offline and --locked (got '%s')", rest[0])
	}
	m, code := loadRootManifest()
	if code != 0 {
		return code
	}
	g, old, err := resolveGraph(m, nf, nil)
	if err != nil {
		return failf("%v", err)
	}
	if err := commitLock(m, g, old, nf); err != nil {
		return failf("%v", err)
	}
	for _, n := range g.Names() {
		if p := g.Packages[n]; p.Git != nil {
			fmt.Printf("%s %s (%s)\n", n, p.Git.Commit[:12], p.Git.Ref)
		}
	}
	return 0
}

const helpFetch = `turf fetch - download the project's dependencies

Usage:
  turf fetch [--offline] [--locked]

Does everything 'turf build' does up to writing turf.lock, and does not
compile: each git dependency is fetched into the cache, extracted, hashed, and
checked against turf.lock (or pinned in it, if it is new). Prints one line per
git dependency: its name, the first 12 digits of its commit, and its ref. It
needs no compiler.

  --offline   run no git network command; a dependency that is not both locked
              and cached is an error naming it
  --locked    turf.lock must already be exactly what the manifests need; any
              difference, or no turf.lock, is an error and nothing is written
`
