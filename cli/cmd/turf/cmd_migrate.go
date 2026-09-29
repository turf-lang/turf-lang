package main

import "strings"

// cmdMigrate is `turfc --migrate=<name> args...`. Which migrations exist is
// the compiler's knowledge, so an unknown name is left for turfc to reject in
// its own words.
func cmdMigrate(args []string) int {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return usagef("migrate: missing migration name (usage: turf migrate <name> [--write] <files...>; the name today is: pub)")
	}
	return forward(append([]string{"--migrate=" + args[0]}, args[1:]...))
}

const helpMigrate = `turf migrate - migrate source across a language change

Usage:
  turf migrate <name> [--write] <files...>

Names:
  pub    add 'pub' to every top-level declaration and struct field, keeping the
         pre-2.0 behaviour where everything a module declared was importable

Without --write it only reports what it would change. Runs
turfc --migrate=<name>.
`
