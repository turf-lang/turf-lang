package manifest

import (
	"bufio"
	"fmt"
	"os"
	"strings"
)

type Manifest struct {
	Name         string
	Version      string
	Dependencies map[string]string
}

func Read(path string) (*Manifest, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	m := &Manifest{
		Dependencies: make(map[string]string),
	}

	scanner := bufio.NewScanner(f)

	inDeps := false

	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())

		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		if strings.HasPrefix(line, "[") {
			inDeps = strings.Trim(line, "[]") == "dependencies"
			continue
		}

		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}

		key := strings.TrimSpace(parts[0])
		val := strings.Trim(strings.TrimSpace(parts[1]), `"`)

		if inDeps {
			m.Dependencies[key] = val
		} else {
			switch key {
			case "name":
				m.Name = val
			case "version":
				m.Version = val
			}
		}
	}

	if err := scanner.Err(); err != nil {
		return nil, err
	}

	return m, nil
}

func Write(path string, m *Manifest) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()

	fmt.Fprintf(f, "name    = %q\n", m.Name)
	fmt.Fprintf(f, "version = %q\n", m.Version)

	if len(m.Dependencies) > 0 {
		fmt.Fprintln(f, "")
		fmt.Fprintln(f, "[dependencies]")
		for pkg, constraint := range m.Dependencies {
			fmt.Fprintf(f, "%-16s = %q\n", pkg, constraint)
		}
	}

	return nil
}
