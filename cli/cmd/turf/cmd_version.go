package main

import (
	"fmt"
	"os"

	"github.com/turf-lang/turf-cli/internal/driver"
	"github.com/turf-lang/turf-cli/internal/version"
)

// cmdVersion prints turf's own version first, unconditionally, so it is
// available even when the compiler cannot be found or does not fit: those
// are exactly the situations in which someone runs `turf version`. The two
// release versions are independent (FREEZE.md 6.16, F-PKG-1), so both are
// shown, each with the driver interface that decides whether they work
// together.
func cmdVersion([]string) int {
	fmt.Printf("turf %s  (needs turfc interface %d.%d+)\n",
		version.Version, version.Required.Major, version.Required.Minor)
	// Before anything can fail: `turf version` is what someone runs first,
	// typically as ./build/turf, and this is where they learn how to make
	// plain `turf` work.
	printPathHint()
	compiler, err := driver.Discover()
	if err != nil {
		fmt.Fprintf(os.Stderr, "turf: %v\n", err)
		return exitFailure
	}
	info, err := driver.Probe(compiler)
	if err != nil {
		fmt.Fprintf(os.Stderr, "turf: %v\n", err)
		return exitFailure
	}
	fmt.Printf("turfc %s  (interface %d.%d, %s)\n",
		info.Release, info.Interface.Major, info.Interface.Minor, compiler)
	// llvm / runtime / root, as the compiler reports them (REL-001): what a bug
	// report needs, shown indented so they read as belonging to turfc's line.
	for _, l := range info.Extra {
		fmt.Printf("  %s\n", l)
	}
	turfPath, _ := os.Executable()
	if err := driver.Handshake(compiler, turfPath); err != nil {
		fmt.Fprintf(os.Stderr, "turf: %v\n", err)
		return exitFailure
	}
	return 0
}

const helpVersion = `turf version - print the toolchain versions

Usage:
  turf version
  turf --version

Prints turf's version and the discovered compiler's version and path, then the
compiler's LLVM version, runtime ABI and install root. The two
are released independently and need not match; what must fit is the driver
interface: turf needs a turfc with the same interface major and at least the
minor it names. A compiler that does not fit is reported, and turf refuses to
drive it.
`
