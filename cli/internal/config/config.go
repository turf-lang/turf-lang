package config

import (
	"bufio"
	"os"
	"path/filepath"
	"strings"
)

type Config struct {
	Registry string
	Token    string
}

func Load() (*Config, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return nil, err
	}

	path := filepath.Join(home, ".turf", "config.toml")

	c := &Config{
		Registry: "http://localhost:8080",
	}

	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return c, nil
		}
		return nil, err
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		key, val, ok := parsePair(line)
		if !ok {
			continue
		}
		switch key {
		case "registry":
			c.Registry = val
		case "token":
			c.Token = val
		}
	}

	return c, scanner.Err()
}

func parsePair(line string) (key, val string, ok bool) {
	parts := strings.SplitN(line, "=", 2)
	if len(parts) != 2 {
		return "", "", false
	}
	key = strings.TrimSpace(parts[0])
	val = strings.Trim(strings.TrimSpace(parts[1]), `"`)
	return key, val, true
}
