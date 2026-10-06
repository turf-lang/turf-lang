// Package version holds the `turf` front end's own release version and the
// compiler driver interface it needs.
package version

// Version is turf's release version. It is independent of the compiler's:
// turf and turfc are released on their own schedules (FREEZE.md 6.16,
// F-PKG-1), and `turf version` prints both. It is a plain constant, so a
// plain `go build` produces a correctly versioned binary.
const Version = "1.0.0-dev"

// Interface is a turfc driver-interface version, `major.minor`, as printed on
// line 2 of `turfc --version`.
type Interface struct {
	Major, Minor int
}

// Required is the driver interface this turf speaks. A compiler is usable if
// it has the same Major and at least this Minor (turfc bumps Minor for
// additive changes and Major for anything removed or changed). Raise Minor
// here when turf starts passing something a newer turfc added; raise Major
// only together with a turfc that raised its own. 1.1 is the package graph
// (`--package-graph`, LEC-001 stage 4), which project builds pass.
var Required = Interface{Major: 1, Minor: 1}

// RequiredForTest is what `turf test` additionally needs: `turfc --test` and the
// package graph's "tests" key (REL-002) arrived in interface 1.3. It is checked
// by `turf test` alone, not by the handshake every command runs, so a turfc
// that predates it still serves build, run and check.
var RequiredForTest = Interface{Major: 1, Minor: 3}

// RequiredForFmt is what `turf fmt` needs: the batch form of `turfc --format`
// (several files, `--check`, a verified result), interface 1.4 (REL-002 stage
// 2) and `--skip-unlexable`, which it passes for the files it found under a
// directory: interface 1.5. Like RequiredForTest it is checked by the one
// command, not by the handshake every command runs.
var RequiredForFmt = Interface{Major: 1, Minor: 5}
