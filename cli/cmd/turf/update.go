// Pre-2.0 registry-model command; not dispatched. Reworked by LEC-001 stage 5/6 (docs/development/changes/LEC-001.md).

package main

import (
	"fmt"
	"os"

	manifest "github.com/turf-lang/turf-lang/cli/internal/legacymanifest"
)

func runUpdate(args []string) error {
	if len(args) > 0 {
		return fmt.Errorf("usage: turf update")
	}

	m, err := manifest.Read("turf.toml")
	if err != nil {
		return fmt.Errorf("reading turf.toml: %w", err)
	}

	if len(m.Dependencies) == 0 {
		fmt.Println("no dependencies to update")
		return nil
	}

	if err := os.RemoveAll("turf_modules"); err != nil {
		return fmt.Errorf("removing turf_modules/: %w", err)
	}

	return installAll()
}
