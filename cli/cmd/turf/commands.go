package main

import (
	"fmt"
	"os"
	"sort"

	"github.com/turf-lang/turf-lang/cli/internal/driver"
)

// Exit codes for the CLI's own failures: 2 for usage, 1 for everything else.
// Provisional; REL-001 decides the real table and may renumber.
const (
	exitFailure = 1
	exitUsage   = 2
)

const (
	groupAny     = "Any file"
	groupProject = "Project (not yet implemented)"
	groupOther   = "Other"
)

type command struct {
	name    string
	group   string
	summary string
	help    string
	run     func(args []string) int
}

var commands []command

// The registry is filled in init() rather than a var initialiser because the
// `help` command reads it: a package-level initialiser would be an
// initialisation cycle.
func init() {
	commands = []command{
		{"compile", groupAny, "compile a file to an executable", helpCompile, cmdCompile},
		{"run", groupAny, "compile to a temporary executable and run it", helpRun, cmdRun},
		{"check", groupAny, "report every diagnostic, write no file", helpCheck, cmdCheck},
		{"fmt", groupAny, "format source files", helpFmt, cmdFmt},
		{"explain", groupAny, "explain an error code", helpExplain, cmdExplain},
		{"migrate", groupAny, "migrate source across a language change", helpMigrate, cmdMigrate},
		{"dump", groupAny, "print an intermediate form (ast, resolved, ...)", helpDump, cmdDump},

		{"new", groupProject, "create a project", helpNew, cmdNew},
		{"init", groupProject, "make the current directory a project", helpInit, cmdInit},
		{"build", groupProject, "build the project", helpBuild, cmdBuild},
		{"test", groupProject, "run the project's tests", stubHelp("test"), stubCmd("test")},
		{"clean", groupProject, "remove build output", helpClean, cmdClean},
		{"fetch", groupProject, "download dependencies", stubHelp("fetch"), stubCmd("fetch")},
		{"add", groupProject, "add a dependency", helpAdd, cmdAdd},
		{"remove", groupProject, "remove a dependency", helpRemove, cmdRemove},
		{"update", groupProject, "update dependencies", stubHelp("update"), stubCmd("update")},

		{"publish", groupOther, "reserved: there is no registry in 2.0", helpPublish, cmdPublish},
		{"version", groupOther, "print the turf and turfc versions", helpVersion, cmdVersion},
		{"help", groupOther, "show help: turf help <command>", helpHelp, cmdHelp},
	}
}

func find(name string) *command {
	for i := range commands {
		if commands[i].name == name {
			return &commands[i]
		}
	}
	return nil
}

func commandNames() []string {
	names := make([]string, 0, len(commands))
	for _, c := range commands {
		names = append(names, c.name)
	}
	// Sorted so a tie in suggest() resolves the same way every run.
	sort.Strings(names)
	return names
}

func usagef(format string, a ...any) int {
	fmt.Fprintf(os.Stderr, "turf: "+format+"\n", a...)
	return exitUsage
}

func failf(format string, a ...any) int {
	fmt.Fprintf(os.Stderr, "turf: "+format+"\n", a...)
	return exitFailure
}

// withCompiler finds turfc and runs the interface handshake before fn drives it.
// Every command that runs the compiler goes through here; a command that runs
// nothing (a stub, `publish`) deliberately does not, since demanding a
// compiler for a command that will not use it only hides the real message.
func withCompiler(fn func(compiler string) int) int {
	c, err := driver.Locate()
	if err != nil {
		fmt.Fprintf(os.Stderr, "turf: %v\n", err)
		return exitFailure
	}
	return fn(c)
}

// forward replaces this process with `turfc args...`. Nothing about args is
// validated: the compiler owns its flags, so a flag added tomorrow works
// through turf the same day (FREEZE.md 6.16).
func forward(args []string) int {
	return withCompiler(func(c string) int { return driver.Exec(c, args) })
}
