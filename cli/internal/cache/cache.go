package cache

import (
	"os"
	"path/filepath"
)

func dir() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(home, ".turf", "cache"), nil
}

func Path(name, version string) (string, error) {
	d, err := dir()
	if err != nil {
		return "", err
	}
	return filepath.Join(d, name+"-"+version+".tar.gz"), nil
}

func Has(name, version string) bool {
	p, err := Path(name, version)
	if err != nil {
		return false
	}
	_, err = os.Stat(p)
	return err == nil
}

func Store(name, version, srcArchive string) error {
	d, err := dir()
	if err != nil {
		return err
	}

	if err := os.MkdirAll(d, 0o755); err != nil {
		return err
	}

	dest, err := Path(name, version)
	if err != nil {
		return err
	}

	data, err := os.ReadFile(srcArchive)
	if err != nil {
		return err
	}

	return os.WriteFile(dest, data, 0o644)
}
