package main

// cmdCheck is `turfc --check`: every diagnostic a real compile would print,
// and no output file. It is a separate compiler flag rather than the CLI
// stopping early because only the compiler knows the point at which all its
// diagnostics have been raised.
func cmdCheck(args []string) int {
	return forward(append([]string{"--check"}, args...))
}

const helpCheck = `turf check - report every diagnostic without producing a file

Usage:
  turf check <file.tr> [flags]
  turf check --module <file.tr> [flags]

Runs turfc --check: the same diagnostics, in the same words, as a normal
compile, and the same exit status (0 clean, 1 on any error), but nothing is
written. Other flags are passed to turfc unchanged.

The file is checked as a program, so a missing main function is an error,
exactly as it is for turf compile. Pass --module to check a file that is
only ever imported: the main requirement is skipped.
`
