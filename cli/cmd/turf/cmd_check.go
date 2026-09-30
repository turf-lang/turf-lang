package main

import "strings"

// cmdCheck is `turfc --check`: every diagnostic a real compile would print,
// and no output file. It is a separate compiler flag rather than the CLI
// stopping early because only the compiler knows the point at which all its
// diagnostics have been raised.
//
// With no .tr argument it checks the project instead, the same two compiler
// runs as `turf build` and each with --check. Whether an argument is a file is
// decided by its .tr suffix alone, the same test `turf run` uses, so turf never
// has to look inside anything to choose a mode.
func cmdCheck(args []string) int {
	for _, a := range args {
		if strings.HasSuffix(a, ".tr") {
			return forward(append([]string{"--check"}, args...))
		}
	}
	return projectCheck(args)
}

const helpCheck = `turf check - report every diagnostic without producing a file

Usage:
  turf check <file.tr> [flags]
  turf check --module <file.tr> [flags]
  turf check [flags]                       (inside a project)

Runs turfc --check: the same diagnostics, in the same words, as a normal
compile, and the same exit status (0 clean, 1 on any error), but nothing is
written. Other flags are passed to turfc unchanged.

The file is checked as a program, so a missing main function is an error,
exactly as it is for turf compile. Pass --module to check a file that is
only ever imported: the main requirement is skipped.

With no .tr argument, run anywhere inside a project: the package graph is
written to build/package-graph.json and src/main.tr and/or src/<name>.tr are
checked with it, exactly as 'turf build' compiles them, but no executable is
produced. --offline and --locked apply as they do to 'turf build' (see
'turf help build'); turf.lock is kept up to date the same way.
`
