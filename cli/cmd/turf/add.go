// Pre-2.0 registry-model command; not dispatched. Reworked by LEC-001 stage 5/6 (docs/development/changes/LEC-001.md).

package main

import (
	"fmt"

	"github.com/turf-lang/turf-lang/cli/internal/config"
	"github.com/turf-lang/turf-lang/cli/internal/manifest"
)

func runAdd(args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: turf add <package>")
	}

	cfg, err := config.Load()
	if err != nil {
		return fmt.Errorf("loading config: %w", err)
	}

	name := args[0]

	meta, err := fetchMeta(name, cfg.Registry)
	if err != nil {
		return err
	}

	version, _, err := bestVersion(meta, "*")
	if err != nil {
		return err
	}

	m, err := manifest.Read("turf.toml")
	if err != nil {
		return fmt.Errorf("reading turf.toml: %w", err)
	}

	constraint := "^" + version
	if m.Dependencies == nil {
		m.Dependencies = map[string]string{}
	}

	m.Dependencies[name] = constraint

	if err := manifest.Write("turf.toml", m); err != nil {
		return fmt.Errorf("updating turf.toml: %w", err)
	}

	fmt.Printf("added %s@%s to turf.toml\n", name, version)

	if err := downloadAndInstall(name, "", "", constraint, cfg.Registry); err != nil {
		return fmt.Errorf("installing %s: %w", name, err)
	}

	return nil
}
