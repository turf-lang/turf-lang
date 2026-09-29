package main

// cmdCompile is `turfc <args...>` spelled out. It exists so the compiler's
// whole surface has a named home under turf, while `turf file.tr` (the
// passthrough) keeps working exactly as before the split.
func cmdCompile(args []string) int {
	return forward(args)
}

// Drift: this flag list is copied by hand from the compiler's parseArgs()
// (src/main.cpp) and its usage text, and will go stale the day a flag is
// added there. REL-001's `turfc --help` is meant to replace it, with
// `turf compile --help` then forwarding to the compiler instead of printing
// this.
const helpCompile = `turf compile - compile a TurfLang file (same as: turfc <file.tr> [flags])

Usage:
  turf compile <file.tr> [flags]
  turf <file.tr> [flags]              the same thing, without the word "compile"

Every argument is passed to turfc unchanged, including flags this list does
not know about, so a new compiler flag works through turf the day it lands.

Output:
  -o <file>                 name of the executable (default: the input's name)
  --emit-llvm               write LLVM IR instead of an executable
  -O0 | -O1 | -O2 | -O3 | -Os
                            optimization level (default: -O2)
  -g                        emit DWARF debug info
  --target=<triple>         cross-compile for this target triple
  -mcpu=<cpu>               target CPU (default: generic)
  --link <lib>              link the extra library lib (-l<lib>)

Checking and inspection:
  --check                   report every diagnostic and write no file
  --resolve                 run and report the name-resolution pass
  --typecheck               run and report the type-checking pass
  --dump-ast                print the parsed AST
  --dump-resolver-symbols   print the symbol table after resolution
  --dump-resolved           print the AST annotated with resolved symbols
  --dump-typechecked        print the AST annotated with inferred types
  --dump-modules            print every module the program imports
  --dump-instantiations     print the generic-instantiation census

Diagnostics and safety:
  --smart                   on error, ask the local model for a validated fix
  --smart-write-fix=<path>  write the first validated --smart fix here
  --stdlib-warnings         show warnings anchored inside the standard library
  --allow-unsafe-modules    honor 'unsafe module;' outside the install root

Other compiler modes (also available as their own commands):
  --format [--write] <file>       turf fmt
  --migrate=pub [--write] <file>  turf migrate pub
  --explain E0NN                  turf explain
  --version                       turf version
`
