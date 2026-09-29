// Pre-2.0 registry-model command; not dispatched. Reworked by LEC-001 stage 5/6 (docs/development/changes/LEC-001.md).

package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/turf-lang/turf-lang/cli/internal/archive"
	"github.com/turf-lang/turf-lang/cli/internal/cache"
	"github.com/turf-lang/turf-lang/cli/internal/config"
	"github.com/turf-lang/turf-lang/cli/internal/manifest"
)

func runRemove(args []string) error {
	if len(args) != 1 {
		return fmt.Errorf("usage: turf remove <package>")
	}

	name := args[0]

	m, err := manifest.Read("turf.toml")
	if err != nil {
		return fmt.Errorf("reading turf.toml: %w", err)
	}

	if _, ok := m.Dependencies[name]; !ok {
		return fmt.Errorf("%q is not a dependency", name)
	}

	if err := os.RemoveAll(filepath.Join("turf_modules", name)); err != nil {
		return fmt.Errorf("removing turf_modules/%s: %w", name, err)
	}

	delete(m.Dependencies, name)

	if err := manifest.Write("turf.toml", m); err != nil {
		return fmt.Errorf("updating turf.toml: %w", err)
	}

	fmt.Printf("removed %s\n", name)

	if err := removeOrphans(m); err != nil {
		return fmt.Errorf("cleaning orphans: %w", err)
	}

	return nil
}

func removeOrphans(m *manifest.Manifest) error {
	needed := map[string]bool{}

	cfg, err := config.Load()
	if err != nil {
		return err
	}

	for dep, constraint := range m.Dependencies {
		if err := collectNeeded(dep, constraint, cfg.Registry, needed); err != nil {
			return err
		}
	}

	entries, err := os.ReadDir("turf_modules")
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}

	for _, entry := range entries {
		if !needed[entry.Name()] {
			fmt.Printf("removing orphan %s\n", entry.Name())
			os.RemoveAll(filepath.Join("turf_modules", entry.Name()))
		}
	}

	return nil
}

func collectNeeded(name, constraint, registry string, needed map[string]bool) error {
	if needed[name] {
		return nil
	}

	needed[name] = true

	meta, err := fetchMeta(name, registry)
	if err != nil {
		return err
	}

	version, _, err := bestVersion(meta, constraint)
	if err != nil {
		return err
	}

	archivePath, err := cache.Path(name, version)
	if err != nil {
		return err
	}

	tmpDir, err := os.MkdirTemp("", "turf-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmpDir)

	if err := archive.Unpack(archivePath, tmpDir); err != nil {
		return err
	}

	m, err := manifest.Read(filepath.Join(tmpDir, "turf.toml"))
	if err != nil {
		return err
	}

	for dep, depConstraint := range m.Dependencies {
		if err := collectNeeded(dep, depConstraint, registry, needed); err != nil {
			return err
		}
	}

	return nil
}
