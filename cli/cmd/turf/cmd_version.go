package main

import (
	"fmt"
	"os"

	"github.com/turf-lang/turf-lang/cli/internal/driver"
	"github.com/turf-lang/turf-lang/cli/internal/version"
)

// cmdVersion prints turf's own version first, unconditionally, so it is
// available even when the compiler cannot be found or does not match: those
// are exactly the situations in which someone runs `turf version`.
func cmdVersion([]string) int {
	fmt.Printf("turf %s\n", version.Version)
	compiler, err := driver.Locate(version.Version)
	if err != nil {
		fmt.Fprintf(os.Stderr, "turf: %v\n", err)
		return exitFailure
	}
	line, err := driver.VersionLine(compiler)
	if err != nil {
		return failf("cannot run %s --version: %v", compiler, err)
	}
	fmt.Printf("%s  (%s)\n", line, compiler)
	return 0
}

const helpVersion = `turf version - print the toolchain version

Usage:
  turf version
  turf --version

Prints turf's version, then the discovered compiler's version and path. The
two must be equal: turf refuses to drive a turfc from a different build.
`
