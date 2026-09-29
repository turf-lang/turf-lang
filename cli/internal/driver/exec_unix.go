//go:build unix

package driver

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
)

// Exec replaces the turf process with path, and only returns if that failed.
// Replacing (rather than spawning and forwarding) is what makes passthrough
// byte-identical to running turfc directly: stdout, stderr, the exit status
// and signal handling are the compiler's by construction, with nothing in
// between to buffer, reorder or translate them.
//
// argv[0] is the resolved absolute path, not the name turf was found under:
// the compiler derives its install root (lib/std, libturfrt) from its own
// location, and on some platforms that comes from argv[0].
func Exec(path string, args []string) int {
	if abs, err := filepath.Abs(path); err == nil {
		path = abs
	}
	argv := append([]string{path}, args...)
	err := syscall.Exec(path, argv, os.Environ())
	fmt.Fprintf(os.Stderr, "turf: cannot execute %s: %v\n", path, err)
	return 1
}

func exitStatus(ee *exec.ExitError) int {
	if ws, ok := ee.Sys().(syscall.WaitStatus); ok && ws.Signaled() {
		return 128 + int(ws.Signal())
	}
	return ee.ExitCode()
}
