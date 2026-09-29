// Command turf is the TurfLang toolchain's front end. It drives the `turfc`
// compiler; it never parses TurfLang source and never rewrites a compiler
// diagnostic (docs/development/FREEZE.md 6.16, F-PKG-1). This file is only
// the dispatcher: one file per command sits beside it.
package main

import (
	"fmt"
	"os"
	"strings"
)

func main() {
	os.Exit(dispatch(os.Args[1:]))
}

type routeKind int

const (
	routeUsage       routeKind = iota // no arguments
	routeCommand                      // a subcommand, including help and version
	routeInstall                      // the retired pre-2.0 command
	routePassthrough                  // `turf <file.tr | -flags>` is `turfc` unchanged
	routeUnknown                      // a bare word that is none of the above
)

type route struct {
	kind routeKind
	name string   // the command, for routeCommand; the word, for routeUnknown
	args []string // the arguments after the command name
}

// classify decides what argv[1:] means, exactly as FREEZE.md 6.16
// "Passthrough" specifies. The order matters:
//
//   - `--version` and `--help`/`-h` are the CLI's own, so they are decided
//     before the "starts with -" passthrough rule swallows them;
//   - a known command name wins over the file rules;
//   - a bare word that neither starts with `-` nor ends in `.tr` is an unknown
//     command, never handed to the compiler, because "no such file: biuld" from
//     turfc is a worse answer than "did you mean 'build'?".
func classify(argv []string) route {
	if len(argv) == 0 {
		return route{kind: routeUsage}
	}
	first, rest := argv[0], argv[1:]
	switch first {
	case "--version":
		return route{kind: routeCommand, name: "version", args: rest}
	case "--help", "-h":
		return route{kind: routeCommand, name: "help", args: nil}
	case "install":
		return route{kind: routeInstall}
	}
	if find(first) != nil {
		return route{kind: routeCommand, name: first, args: rest}
	}
	if strings.HasPrefix(first, "-") || strings.HasSuffix(first, ".tr") {
		return route{kind: routePassthrough, args: argv}
	}
	return route{kind: routeUnknown, name: first}
}

func dispatch(argv []string) int {
	r := classify(argv)
	switch r.kind {
	case routeUsage:
		fmt.Fprint(os.Stderr, overallHelp())
		return 2
	case routeInstall:
		fmt.Fprintln(os.Stderr, "turf: `turf install` is now `turf fetch` (LEC-001 stage 6)")
		return 2
	case routePassthrough:
		return forward(r.args)
	case routeUnknown:
		return unknownCommand(r.name)
	}
	cmd := find(r.name)
	if wantsHelp(r.args) {
		fmt.Print(cmd.help)
		return 0
	}
	return cmd.run(r.args)
}

// wantsHelp reports whether -h/--help appears before any `--`. After `--` the
// arguments belong to the program `turf run` starts, and `turf run p.tr --
// --help` must reach it.
func wantsHelp(args []string) bool {
	for _, a := range args {
		if a == "--" {
			return false
		}
		if a == "-h" || a == "--help" {
			return true
		}
	}
	return false
}

func unknownCommand(name string) int {
	fmt.Fprintf(os.Stderr, "turf: unknown command '%s'\n", name)
	if s, ok := suggest(name, commandNames()); ok {
		fmt.Fprintf(os.Stderr, "  did you mean '%s'?\n", s)
	}
	fmt.Fprintln(os.Stderr, "run `turf help` for the list of commands")
	return 2
}

// suggest returns the candidate nearest to name by edit distance, if it is
// within 2: close enough to be a typo (a transposition such as biuld is
// distance 2), far enough that unrelated words get no guess.
func suggest(name string, candidates []string) (string, bool) {
	best, bestD := "", 3
	for _, c := range candidates {
		if d := editDistance(name, c); d < bestD {
			best, bestD = c, d
		}
	}
	return best, best != ""
}

func editDistance(a, b string) int {
	ra, rb := []rune(a), []rune(b)
	prev := make([]int, len(rb)+1)
	cur := make([]int, len(rb)+1)
	for j := range prev {
		prev[j] = j
	}
	for i := 1; i <= len(ra); i++ {
		cur[0] = i
		for j := 1; j <= len(rb); j++ {
			cost := 1
			if ra[i-1] == rb[j-1] {
				cost = 0
			}
			cur[j] = min(prev[j]+1, cur[j-1]+1, prev[j-1]+cost)
		}
		prev, cur = cur, prev
	}
	return prev[len(rb)]
}
