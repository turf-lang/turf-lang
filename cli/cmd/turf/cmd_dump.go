package main

import (
	"fmt"
	"os"
	"strings"
)

// dumpKinds maps the word after `turf dump` to the compiler flag. The order is
// the order the compiler's pipeline produces them, which is also the order
// they are listed in errors and help.
var dumpKinds = []struct{ what, flag string }{
	{"ast", "--dump-ast"},
	{"resolved", "--dump-resolved"},
	{"typechecked", "--dump-typechecked"},
	{"modules", "--dump-modules"},
	{"symbols", "--dump-resolver-symbols"},
	{"instantiations", "--dump-instantiations"},
}

func dumpFlag(what string) (string, bool) {
	for _, k := range dumpKinds {
		if k.what == what {
			return k.flag, true
		}
	}
	return "", false
}

func dumpNames() []string {
	names := make([]string, len(dumpKinds))
	for i, k := range dumpKinds {
		names[i] = k.what
	}
	return names
}

// cmdDump is `turfc --check --dump-<what>`. --check is added because the raw
// --dump-* flags continue into codegen and link an executable; a dump is an
// inspection and must not leave a binary behind.
func cmdDump(args []string) int {
	if len(args) == 0 {
		return dumpUsage("dump: missing what to dump")
	}
	flag, ok := dumpFlag(args[0])
	if !ok {
		msg := fmt.Sprintf("dump: unknown thing to dump '%s'", args[0])
		if s, found := suggest(args[0], dumpNames()); found {
			msg += fmt.Sprintf(" (did you mean '%s'?)", s)
		}
		return dumpUsage(msg)
	}
	return forward(append([]string{"--check", flag}, args[1:]...))
}

func dumpUsage(msg string) int {
	fmt.Fprintf(os.Stderr, "turf: %s\n  usage: turf dump <what> <file.tr> [flags]\n  <what> is one of: %s\n",
		msg, strings.Join(dumpNames(), ", "))
	return exitUsage
}

const helpDump = `turf dump - print an intermediate form of a program

Usage:
  turf dump <what> <file.tr> [flags]

<what> is one of:
  ast              the parsed AST                        (--dump-ast)
  resolved         the AST with resolved symbols         (--dump-resolved)
  typechecked      the AST with inferred types           (--dump-typechecked)
  modules          every imported module and its imports (--dump-modules)
  symbols          the symbol table after resolution     (--dump-resolver-symbols)
  instantiations   the generic-instantiation census      (--dump-instantiations)

Runs turfc --check --dump-<what>, so no executable is produced.
`
