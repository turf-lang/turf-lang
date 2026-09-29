//go:build !unix

package driver

import "os/exec"

// Exec runs path as a child and returns its status. Windows has no
// process-replacing exec, so passthrough here is spawn-and-forward with
// inherited stdio and the child's exit code, which is as close as the platform
// allows.
func Exec(path string, args []string) int {
	return Run(path, args)
}

func exitStatus(ee *exec.ExitError) int {
	return ee.ExitCode()
}
