package main

import (
	"fmt"
	"strings"
)

// overallHelp lists the commands by group. It is generated from the registry
// so a command added there cannot be forgotten here.
func overallHelp() string {
	var b strings.Builder
	b.WriteString("turf - the TurfLang toolchain front end\n\n")
	b.WriteString("Usage:\n")
	b.WriteString("  turf <command> [args]\n")
	b.WriteString("  turf <file.tr> [flags]      same as: turfc <file.tr> [flags]  (flags forwarded unchanged)\n\n")
	for _, g := range []string{groupAny, groupProject, groupOther} {
		fmt.Fprintf(&b, "%s:\n", g)
		for _, c := range commands {
			if c.group == g {
				fmt.Fprintf(&b, "  %-9s %s\n", c.name, c.summary)
			}
		}
		b.WriteString("\n")
	}
	b.WriteString("Run 'turf help <command>' for a command's options.\n")
	return b.String()
}

func cmdHelp(args []string) int {
	if len(args) == 0 {
		fmt.Print(overallHelp())
		printPathHint()
		return 0
	}
	c := find(args[0])
	if c == nil {
		return unknownCommand(args[0])
	}
	fmt.Print(c.help)
	return 0
}

const helpHelp = `turf help - show help

Usage:
  turf help              list the commands
  turf help <command>    show one command's options
  turf <command> --help  the same
`
