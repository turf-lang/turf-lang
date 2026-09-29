package driver

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
)

// Run starts path with args, stdio inherited, waits for it, and returns the
// status `turf` should exit with. It is the portable path, used where the
// process must outlive the child (several files in a row, `turf run`'s
// cleanup); commands that only forward use Exec, which is byte-exact.
func Run(path string, args []string) int {
	return ExitCode(Command(path, args).Run())
}

// RunIn is Run with the child's working directory set to dir. A project build
// runs turfc from the project root so diagnostics name files as src/main.tr
// however deep in the tree the user typed `turf build`, and the same file is
// always spelled the same way.
func RunIn(dir, path string, args []string) int {
	cmd := Command(path, args)
	cmd.Dir = dir
	return ExitCode(cmd.Run())
}

// Command builds a child with stdio inherited, so its output reaches the
// user's terminal (or pipe) directly and is never re-rendered by turf.
func Command(path string, args []string) *exec.Cmd {
	cmd := exec.Command(path, args...)
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd
}

// ExitCode converts the result of running a child into an exit status: 0 on
// success, the child's own status if it exited non-zero, and (on Unix)
// 128+signal if a signal killed it, the shell convention, so `turf run`'s
// status is the same one the program would have given when launched directly.
// A child that could not be started at all is turf's own failure: status 1,
// and the reason printed.
func ExitCode(err error) int {
	if err == nil {
		return 0
	}
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		return exitStatus(ee)
	}
	fmt.Fprintf(os.Stderr, "turf: %v\n", err)
	return 1
}
