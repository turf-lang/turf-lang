package main

import (
	"fmt"
	"os"
)

// cmdPublish is reserved, not implemented: the package registry is FUTURE
// F-7c, and 2.0 distributes a library by pushing a git tag. The name is kept
// (FREEZE.md 6.16) so the command surface does not change when it arrives.
func cmdPublish([]string) int {
	fmt.Fprintln(os.Stderr, "turf: the package registry is not part of TurfLang 2.0; publish a library by pushing a git tag (FUTURE F-7c)")
	return exitFailure
}

const helpPublish = `turf publish - reserved

There is no package registry in TurfLang 2.0. To publish a library, push a git
tag and let dependents name it with a git dependency. A registry is FUTURE F-7c.
`
