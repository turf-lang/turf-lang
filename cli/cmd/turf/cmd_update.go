package main

import (
	"fmt"
	"sort"
	"strings"
)

// cmdUpdate is the only command that moves a pinned ref (FREEZE.md 6.16,
// F-PKG-3): a re-pointed tag or a branch that moved is invisible to build, and
// this re-resolves it, then writes the new commit and tree hash to turf.lock.
func cmdUpdate(args []string) int {
	var names []string
	for _, a := range args {
		switch {
		case a == "--offline":
			return usagef("'update' has to reach the network to see what moved, so --offline is not accepted")
		case strings.HasPrefix(a, "-"):
			return usagef("unknown flag '%s' (usage: turf update [name...])", a)
		default:
			names = append(names, a)
		}
	}
	m, code := loadRootManifest()
	if code != 0 {
		return code
	}

	// First resolve the graph as a build would, only to learn which packages
	// are git dependencies: a name that is not one is refused before any
	// network operation whose only purpose is an update.
	g0, old, err := resolveGraph(m, netFlags{}, nil)
	if err != nil {
		return failf("%v", err)
	}
	set := map[string]bool{}
	if len(names) == 0 {
		for _, n := range g0.Names() {
			if g0.Packages[n].Git != nil {
				set[n] = true
			}
		}
	}
	for _, n := range names {
		p := g0.Packages[n]
		switch {
		case p == nil:
			return failf("'%s' is not a package in this project's dependency graph", n)
		case p == g0.Root:
			return failf("'%s' is the project's own package, not a dependency", n)
		case p.Git == nil:
			return failf("'%s' is a path dependency; it is always used as it is on disk", n)
		}
		set[n] = true
	}

	g, _, err := resolveGraph(m, netFlags{}, func(n string) bool { return set[n] })
	if err != nil {
		return failf("%v", err)
	}
	if err := commitLock(m, g, old, netFlags{}); err != nil {
		return failf("%v", err)
	}

	next := g.Lock()
	var order []string
	for n := range set {
		order = append(order, n)
	}
	sort.Strings(order)
	if len(order) == 0 {
		fmt.Println("no git dependencies to update")
	}
	for _, n := range order {
		was, now := old.Find(n), next.Find(n)
		switch {
		case now == nil:
			// Only possible if the name left the graph between the two
			// resolutions, which the manifests cannot cause.
			continue
		case was == nil:
			fmt.Printf("%s (not locked) -> %s\n", n, now.Commit[:12])
		case was.Commit != now.Commit:
			fmt.Printf("%s %s -> %s\n", n, was.Commit[:12], now.Commit[:12])
		default:
			fmt.Printf("%s unchanged\n", n)
		}
	}
	return 0
}

const helpUpdate = `turf update - move git dependencies to what their refs point at now

Usage:
  turf update [name...]

Refreshes the cached mirror of each named git dependency (all of them with no
names), re-resolves its ref, extracts and hashes the tree, and writes the new
commit to turf.lock. Nothing else moves a pinned commit: 'turf build' keeps
using the locked one even if the tag was re-pointed or the branch has new
commits.

  a tag       is resolved again, so a moved tag moves
  a branch    moves to its tip
  a rev       resolves to the same commit: updating it changes nothing

Prints '<name> <old commit> -> <new commit>' (12 digits each) for a change and
'<name> unchanged' otherwise. A path dependency is refused (it is always used
as it is on disk), and so is a name that is not in the dependency graph.
--offline is not accepted: seeing what moved needs the network.
`
