package main

import "fmt"

// stubWhen names the stage that implements each project command, per the
// stage table in docs/development/changes/LEC-001.md. `test` is REL-002's, not
// LEC-001's. None of these runs the compiler, so none runs the handshake.
var stubWhen = map[string]string{
	"test": "REL-002",
}

// stubCmd is a command whose name is frozen (FREEZE.md 6.16) but whose
// behaviour is not built yet. It says so and exits 2, rather than being left
// out (which would make the name look free to reuse) or half-working.
func stubCmd(name string) func([]string) int {
	return func([]string) int {
		return usagef("'%s' is not implemented yet (%s)", name, stubWhen[name])
	}
}

func stubHelp(name string) string {
	return fmt.Sprintf("turf %s - not implemented yet (%s)\n\nThe command name is reserved by the toolchain design\n(docs/development/FREEZE.md 6.16) but it does nothing yet.\n", name, stubWhen[name])
}
