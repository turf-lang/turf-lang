package main

import (
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/driver"
)

// cmdFmt drives `turfc --format`, which takes exactly one file per run.
//
// Without --write the formatted text goes to stdout, so several files would
// concatenate into output nobody could tell apart; that is refused instead of
// guessed at. With --write each file is rewritten in place, so any number is
// fine: every file is attempted even after one fails (one bad file must not
// leave the rest unformatted), and the first failing status is the result.
// Project-wide fmt is REL-002.
func cmdFmt(args []string) int {
	write := false
	var files []string
	for _, a := range args {
		switch {
		case a == "--write":
			write = true
		case strings.HasPrefix(a, "-"):
			return usagef("fmt: unknown flag '%s' (usage: turf fmt [--write] <files...>)", a)
		default:
			files = append(files, a)
		}
	}
	if len(files) == 0 {
		return usagef("fmt: no files given (usage: turf fmt [--write] <files...>)")
	}
	if !write && len(files) > 1 {
		return usagef("fmt: without --write the formatted text goes to stdout, so exactly one file is accepted; pass --write to format several in place")
	}

	flags := []string{"--format"}
	if write {
		flags = append(flags, "--write")
	}
	return withCompiler(func(c string) int {
		if len(files) == 1 {
			return driver.Exec(c, append(flags, files[0]))
		}
		status := 0
		for _, f := range files {
			argv := append(append([]string{}, flags...), f)
			if s := driver.Run(c, argv); s != 0 && status == 0 {
				status = s
			}
		}
		return status
	})
}

const helpFmt = `turf fmt - format TurfLang source

Usage:
  turf fmt <file.tr>              print the formatted file to stdout
  turf fmt --write <file.tr>...   rewrite the files in place

Without --write exactly one file is accepted, because the output goes to
stdout. With --write any number are formatted, each attempted even if an
earlier one fails; the exit status is the first failure's. Runs turfc --format.
`
