// Package version holds the toolchain version the `turf` front end reports and
// checks the compiler against.
package version

// Version is stamped at build time, never edited here:
//
//	-ldflags "-X github.com/turf-lang/turf-lang/cli/internal/version.Version=<v>"
//
// CMake passes the contents of the repo-root VERSION file, the same value it
// compiles into `turfc`, so the two binaries cannot disagree by construction.
// The "unknown" default is deliberate: a plain `go build` has no stamp, and a
// front end that guessed a version could pass the compiler handshake against
// the wrong compiler. driver.Handshake refuses to run while it is "unknown".
var Version = Unstamped

// Unstamped is the value Version has when nothing set it.
const Unstamped = "unknown"
