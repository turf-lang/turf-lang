// Pre-2.0 registry-model command; not dispatched. Reworked by LEC-001 stage 5/6 (docs/development/changes/LEC-001.md).

package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/archive"
	"github.com/turf-lang/turf-lang/cli/internal/cache"
	"github.com/turf-lang/turf-lang/cli/internal/checksum"
	"github.com/turf-lang/turf-lang/cli/internal/config"
	manifest "github.com/turf-lang/turf-lang/cli/internal/legacymanifest"
	"github.com/turf-lang/turf-lang/cli/internal/lockfile"
	"github.com/turf-lang/turf-lang/cli/internal/resolver"
	"github.com/turf-lang/turf-lang/cli/internal/semver"
)

func runInstall(args []string) error {
	if len(args) == 0 {
		return installAll()
	}

	src := args[0]

	if isLocalPath(src) {
		return installLocal(src)
	}

	return installFromRegistry(src)
}

func isLocalPath(s string) bool {
	return strings.HasPrefix(s, "./") ||
		strings.HasPrefix(s, "../") ||
		strings.HasPrefix(s, "/")
}

func copyDir(src, dst string) error {
	return filepath.Walk(src, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}

		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}

		target := filepath.Join(dst, rel)

		if info.IsDir() {
			return os.MkdirAll(target, info.Mode())
		}

		return copyFile(path, target, info.Mode())
	})
}

func copyFile(src, dst string, mode os.FileMode) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	out, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, mode)
	if err != nil {
		return err
	}
	defer out.Close()

	_, err = io.Copy(out, in)
	return err
}

func installLocal(src string) error {
	src = filepath.Clean(src)

	visited := map[string]*resolver.Node{}
	root, err := resolver.Build(src, visited)
	if err != nil {
		return fmt.Errorf("resolving dependencies: %w", err)
	}

	order := []*resolver.Node{}
	resolver.Resolve(root, map[string]bool{}, &order)

	for _, node := range order {
		dest := filepath.Join("turf_modules", node.Manifest.Name)

		if err := os.MkdirAll(dest, 0o755); err != nil {
			return fmt.Errorf("creating turf_modules/: %w", err)
		}

		pkgSrc := filepath.Join(filepath.Dir(src), node.Manifest.Name)
		if node.Manifest.Name == root.Manifest.Name {
			pkgSrc = src
		}

		if err := copyDir(pkgSrc, dest); err != nil {
			return fmt.Errorf("installing %s: %w", node.Manifest.Name, err)
		}

		fmt.Printf("installed %s@%s  →  turf_modules/%s\n", node.Manifest.Name, node.Manifest.Version, node.Manifest.Name)
	}

	lf := &lockfile.Lockfile{}
	for _, node := range order {
		lf.Packages = append(lf.Packages, lockfile.Entry{
			Name:    node.Manifest.Name,
			Version: node.Manifest.Version,
		})
	}

	if err := lockfile.Write("turf.lock", lf); err != nil {
		return fmt.Errorf("writing lockfile: %w", err)
	}

	fmt.Println("wrote turf.lock")
	return nil
}

func installAll() error {
	m, err := manifest.Read("turf.toml")
	if err != nil {
		return fmt.Errorf("reading turf.toml: %w", err)
	}

	if len(m.Dependencies) == 0 {
		fmt.Println("no dependencies to install")
		return nil
	}

	cfg, err := config.Load()
	if err != nil {
		return fmt.Errorf("loading config: %w", err)
	}

	for dep, constraint := range m.Dependencies {
		if err := downloadAndInstall(dep, "", "", constraint, cfg.Registry); err != nil {
			return err
		}
	}

	return nil
}

func installFromRegistry(name string) error {
	cfg, err := config.Load()
	if err != nil {
		return fmt.Errorf("loading config: %w", err)
	}

	return downloadAndInstall(name, "", "", "*", cfg.Registry)
}

type registryMeta struct {
	Name     string            `json:"name"`
	Versions map[string]string `json:"versions"`
}

func fetchMeta(name, registry string) (*registryMeta, error) {
	resp, err := http.Get(registry + "/package/" + name)
	if err != nil {
		return nil, fmt.Errorf("contacting registry: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("package %q not found in registry", name)
	}

	var meta registryMeta
	if err := json.NewDecoder(resp.Body).Decode(&meta); err != nil {
		return nil, fmt.Errorf("parsing registry response: %w", err)
	}

	return &meta, nil
}

func downloadAndInstall(name, version, expectedHash, constraint, registry string) error {
	if version == "" {
		meta, err := fetchMeta(name, registry)
		if err != nil {
			return err
		}

		if constraint == "" {
			constraint = "*"
		}

		var err2 error
		version, expectedHash, err2 = bestVersion(meta, constraint)
		if err2 != nil {
			return err2
		}
	}

	if cache.Has(name, version) {
		fmt.Printf("cached  %s@%s  →  turf_modules/%s\n", name, version, name)

		archivePath, err := cache.Path(name, version)
		if err != nil {
			return err
		}

		dest := filepath.Join("turf_modules", name)
		if err := os.MkdirAll(dest, 0o755); err != nil {
			return err
		}

		if err := archive.Unpack(archivePath, dest); err != nil {
			return fmt.Errorf("extracting %s from cache: %w", name, err)
		}

		m, err := manifest.Read(filepath.Join(dest, "turf.toml"))
		if err != nil {
			return fmt.Errorf("reading installed manifest: %w", err)
		}

		for dep, depConstraint := range m.Dependencies {
			if err := downloadAndInstall(dep, "", "", depConstraint, registry); err != nil {
				return err
			}
		}

		return nil
	}

	url := fmt.Sprintf("http://localhost:8080/package/%s/%s/download", name, version)

	resp, err := http.Get(url)
	if err != nil {
		return fmt.Errorf("downloading %s@%s: %w", name, version, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("registry returned %d for %s@%s", resp.StatusCode, name, version)
	}

	archivePath := fmt.Sprintf("/tmp/%s-%s.tar.gz", name, version)

	out, err := os.Create(archivePath)
	if err != nil {
		return err
	}

	if _, err := io.Copy(out, resp.Body); err != nil {
		return fmt.Errorf("saving archive: %w", err)
	}
	out.Close()

	hash, err := checksum.Hash(archivePath)
	if err != nil {
		return err
	}

	if hash != expectedHash {
		return fmt.Errorf("checksum mismatch for %s@%s", name, version)
	}

	dest := filepath.Join("turf_modules", name)
	if err := os.MkdirAll(dest, 0o755); err != nil {
		return err
	}

	if err := archive.Unpack(archivePath, dest); err != nil {
		return fmt.Errorf("extracting %s: %w", name, err)
	}

	if err := cache.Store(name, version, archivePath); err != nil {
		fmt.Printf("warning: could not cache %s@%s: %v\n", name, version, err)
	}

	fmt.Printf("installed %s@%s  →  turf_modules/%s\n", name, version, name)

	m, err := manifest.Read(filepath.Join(dest, "turf.toml"))
	if err != nil {
		return fmt.Errorf("reading installed manifest: %w", err)
	}

	for dep, depConstraint := range m.Dependencies {
		if err := downloadAndInstall(dep, "", "", depConstraint, registry); err != nil {
			return err
		}
	}

	return nil
}

func bestVersion(meta *registryMeta, constraint string) (version, hash string, err error) {
	available := make([]string, 0, len(meta.Versions))
	for v := range meta.Versions {
		available = append(available, v)
	}

	version, err = semver.Resolve(constraint, available)
	if err != nil {
		return "", "", fmt.Errorf("no version of %q satisfies %q", meta.Name, constraint)
	}

	hash = meta.Versions[version]
	return version, hash, nil
}
