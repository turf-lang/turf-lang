// Package driver finds the `turfc` compiler, checks that it is the same build
// as the running `turf`, and runs it. It knows nothing about TurfLang source:
// FREEZE.md 6.16 (F-PKG-1) keeps every question that depends on source with
// the compiler and every question about files, network and reproducibility
// with the CLI.
package driver

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// CompilerFile is the compiler's file name on this platform.
var CompilerFile = "turfc" + exeSuffix()

func exeSuffix() string {
	if runtime.GOOS == "windows" {
		return ".exe"
	}
	return ""
}

// Error is a failure the user can act on: a message, and optionally a hint
// line telling them how. Every driver failure is one, so main prints them all
// the same way.
type Error struct {
	Msg  string
	Hint string
}

func (e *Error) Error() string {
	if e.Hint == "" {
		return e.Msg
	}
	return e.Msg + "\nhint: " + e.Hint
}

const envHint = "set TURF_COMPILER to the turfc binary, or build it with `cmake --build build`"

// Discover finds turfc for the running executable. See DiscoverFrom.
func Discover() (string, error) {
	exe, err := os.Executable()
	if err != nil {
		exe = ""
	}
	return DiscoverFrom(exe)
}

// DiscoverFrom returns the path to turfc, first match wins (FREEZE.md 6.16):
//
//  1. $TURF_COMPILER, if set and non-empty. It is authoritative: naming a file
//     that does not exist is an error, not a reason to fall through, so a typo
//     in the override can never silently run some other compiler.
//  2. turfc in the directory of exe with symlinks resolved. This is both the
//     installed layout and the dev layout (build/turf beside build/turfc), and
//     resolving symlinks is what lets `ln -s .../build/turf ~/bin/turf` work.
//  3. $TURF_HOME/bin/turfc.
//  4. turfc on PATH.
//
// exe is a parameter, not read here, so tests can stand in for the running
// binary.
func DiscoverFrom(exe string) (string, error) {
	if v := os.Getenv("TURF_COMPILER"); v != "" {
		if !isFile(v) {
			return "", &Error{
				Msg:  fmt.Sprintf("TURF_COMPILER is set to %q, which is not an existing file", v),
				Hint: "point TURF_COMPILER at the turfc binary, or unset it to use the compiler installed beside turf",
			}
		}
		return v, nil
	}

	var tried []string
	tried = append(tried, "$TURF_COMPILER (not set)")

	if exe != "" {
		if resolved, err := filepath.EvalSymlinks(exe); err == nil {
			exe = resolved
		}
		cand := filepath.Join(filepath.Dir(exe), CompilerFile)
		if isFile(cand) {
			return cand, nil
		}
		tried = append(tried, cand)
	} else {
		tried = append(tried, "beside the turf executable (its path could not be determined)")
	}

	if home := os.Getenv("TURF_HOME"); home != "" {
		cand := filepath.Join(home, "bin", CompilerFile)
		if isFile(cand) {
			return cand, nil
		}
		tried = append(tried, cand)
	} else {
		tried = append(tried, "$TURF_HOME/bin/"+CompilerFile+" (TURF_HOME not set)")
	}

	if p, err := exec.LookPath("turfc"); err == nil {
		return p, nil
	} else if !errors.Is(err, exec.ErrNotFound) {
		// A PATH entry named turfc that is not runnable is worth showing.
		tried = append(tried, "turfc on PATH ("+err.Error()+")")
	} else {
		tried = append(tried, "turfc on PATH")
	}

	return "", &Error{
		Msg:  "cannot find the compiler (turfc)\n  looked in:\n    " + strings.Join(tried, "\n    "),
		Hint: envHint,
	}
}

func isFile(p string) bool {
	st, err := os.Stat(p)
	return err == nil && !st.IsDir()
}
