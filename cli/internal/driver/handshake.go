package driver

import (
	"fmt"
	"os"
	"os/exec"
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/version"
)

// Handshake checks that compiler is the turfc built alongside this turf, by
// comparing line 1 of `turfc --version` with "turfc <turfVersion>". A
// hand-assembled pair from different builds is refused, not tolerated:
// the two share one version because they are built from one tree, and a
// mismatch is how a stale binary left behind by a partial rebuild shows up.
//
// TURF_COMPILER does not bypass this (FREEZE.md 6.16), and neither does an
// unstamped turf: with no version of its own there is nothing to compare, and
// guessing would defeat the check.
//
// Only line 1 is read. The compiler may print more lines later (LLVM version,
// runtime ABI, install root) without breaking older front ends.
func Handshake(compiler, turfPath, turfVersion string) error {
	if turfVersion == version.Unstamped {
		return &Error{
			Msg: "turf was built without a version stamp; build it with 'cmake --build build --target turf-cli'",
		}
	}

	out, err := exec.Command(compiler, "--version").Output()
	if err != nil {
		return &Error{
			Msg:  fmt.Sprintf("cannot run %s --version: %v", compiler, err),
			Hint: envHint,
		}
	}
	line := strings.TrimRight(strings.SplitN(string(out), "\n", 2)[0], "\r")
	if line == "" {
		return &Error{
			Msg:  fmt.Sprintf("%s printed nothing for --version, so it does not look like turfc", compiler),
			Hint: envHint,
		}
	}
	got, ok := strings.CutPrefix(line, "turfc ")
	if !ok {
		return &Error{
			Msg:  fmt.Sprintf("%s does not look like turfc: --version printed %q", compiler, line),
			Hint: envHint,
		}
	}
	if got == turfVersion {
		return nil
	}
	return &Error{
		Msg: fmt.Sprintf("compiler version mismatch\n  turf  %s  (%s)\n  turfc %s  (%s)",
			turfVersion, turfPath, got, compiler),
		Hint: "turf and turfc must come from the same build; rebuild with `cmake --build build`, or point TURF_COMPILER at the matching turfc",
	}
}

// Locate finds turfc and runs the handshake, the two steps every command that
// drives the compiler starts with.
func Locate(turfVersion string) (string, error) {
	compiler, err := Discover()
	if err != nil {
		return "", err
	}
	turfPath, _ := os.Executable()
	if err := Handshake(compiler, turfPath, turfVersion); err != nil {
		return "", err
	}
	return compiler, nil
}

// VersionLine returns line 1 of `compiler --version`, for `turf version`.
func VersionLine(compiler string) (string, error) {
	out, err := exec.Command(compiler, "--version").Output()
	if err != nil {
		return "", err
	}
	return strings.TrimRight(strings.SplitN(string(out), "\n", 2)[0], "\r"), nil
}
