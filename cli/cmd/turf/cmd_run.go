package main

import (
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/driver"
)

// cmdRun compiles one file to a temporary executable, runs it, and cleans up.
//
//	turf run <file.tr> [compiler flags...] [-- program args...]
//
// Project mode (`turf run` with no file) is LEC-001 stage 5.
func cmdRun(args []string) int {
	before, after := splitAtDashDash(args)

	file := ""
	var flags []string
	for _, a := range before {
		if file == "" && strings.HasSuffix(a, ".tr") {
			file = a
			continue
		}
		flags = append(flags, a)
	}
	if file == "" {
		return usagef("project builds arrive in LEC-001 stage 5; use 'turf run <file.tr>'")
	}
	for _, f := range flags {
		if f == "-o" {
			// The executable lives in a directory turf deletes on the way out,
			// so an -o the user gave would be built and then thrown away.
			fmt.Fprintln(os.Stderr, "turf run builds to a temporary file; use 'turf compile -o …'")
			return exitUsage
		}
	}

	return withCompiler(func(compiler string) int {
		tmp, err := os.MkdirTemp("", "turf-run-")
		if err != nil {
			return failf("cannot create a temporary directory: %v", err)
		}
		// A deferred RemoveAll, and this function returning a status instead of
		// calling os.Exit, is what guarantees the directory is gone on every
		// path out, including a failed compile and a program killed by a signal.
		defer os.RemoveAll(tmp)

		// Ctrl-C must reach the child and turf must still clean up. signal.Notify
		// (a handler that discards the signal) achieves both; signal.Ignore does
		// not, because an ignored disposition is inherited across exec and the
		// program would then ignore Ctrl-C too. A caught signal is reset to the
		// default in a child.
		sigs := make(chan os.Signal, 1)
		signal.Notify(sigs, os.Interrupt)
		defer signal.Stop(sigs)

		stem := strings.TrimSuffix(filepath.Base(file), ".tr")
		if stem == "" {
			stem = "a"
		}
		exe := filepath.Join(tmp, stem)
		if runtime.GOOS == "windows" {
			exe += ".exe"
		}

		compileArgs := append([]string{file}, flags...)
		compileArgs = append(compileArgs, "-o", exe)
		if status := driver.Run(compiler, compileArgs); status != 0 {
			return status
		}
		if st, err := os.Stat(exe); err != nil || st.IsDir() {
			// e.g. --emit-llvm: the compiler succeeded but there is nothing to run.
			return failf("the compiler produced no executable")
		}

		return driver.Run(exe, after)
	})
}

func splitAtDashDash(args []string) (before, after []string) {
	for i, a := range args {
		if a == "--" {
			return args[:i], args[i+1:]
		}
	}
	return args, nil
}

const helpRun = `turf run - compile a file and run it

Usage:
  turf run <file.tr> [compiler flags...] [-- program args...]

Compiles to a temporary executable (turfc <file.tr> <flags> -o <temp>), runs
it with the arguments after --, and deletes it. The exit status is the
program's, or 128+N if signal N killed it. A failed compile exits with the
compiler's status. Ctrl-C goes to the program.

-o is refused: the executable is deleted afterwards. Use 'turf compile -o'.
Project mode ('turf run' with no file) arrives in LEC-001 stage 5.
`
