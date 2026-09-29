package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/turf-lang/turf-cli/internal/manifest"
)

func runInit(args []string) error {
	name := filepath.Base(mustCwd())
	if len(args) >= 1 {
		name = args[0]
	}

	if _, err := os.Stat("turf.toml"); err == nil {
		return fmt.Errorf("turf.toml already exists")
	}

	m := &manifest.Manifest{
		Name:    name,
		Version: "0.1.0",
	}

	if err := manifest.Write("turf.toml", m); err != nil {
		return fmt.Errorf("writing turf.toml: %w", err)
	}

	if err := os.MkdirAll("src", 0o755); err != nil {
		return fmt.Errorf("creating src/: %w", err)
	}

	fmt.Printf("initialized %q v%s\n", name, m.Version)
	return nil
}

func mustCwd() string {
	wd, err := os.Getwd()
	if err != nil {
		return "project"
	}
	return wd
}
