// Package stdlib lists the standard-library module names by looking at the
// install tree. It is filesystem layout, not TurfLang parsing: F-PKG-2 needs
// turf to refuse a package named like a stdlib module (`import list;` could
// never reach it), and the only way to know the names without a hard-coded list
// that rots is to read lib/std the way the compiler does.
package stdlib

import (
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// InstallRoot is the root the compiler finds lib/std under: $TURF_HOME if set,
// else the directory above the one holding turfc, with symlinks resolved. It
// mirrors TurfInstallRoot() in src/frontend/ParserModule.cpp; if the two disagree,
// `turf` would check names against a different stdlib than the one `turfc`
// resolves imports against.
func InstallRoot(compiler string) string {
	if home := os.Getenv("TURF_HOME"); home != "" {
		return home
	}
	if resolved, err := filepath.EvalSymlinks(compiler); err == nil {
		compiler = resolved
	}
	if abs, err := filepath.Abs(compiler); err == nil {
		compiler = abs
	}
	return filepath.Join(filepath.Dir(compiler), "..")
}

// Modules returns the module names under <installRoot>/lib/std: the stem of
// every .tr file, at any depth, because a module's subfolder is invisible to
// `import`.
func Modules(installRoot string) (map[string]bool, error) {
	dir := filepath.Join(installRoot, "lib", "std")
	if st, err := os.Stat(dir); err != nil || !st.IsDir() {
		return nil, errors.New("no standard library at " + dir)
	}
	names := map[string]bool{}
	err := filepath.WalkDir(dir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.IsDir() && strings.HasSuffix(d.Name(), ".tr") {
			names[strings.TrimSuffix(d.Name(), ".tr")] = true
		}
		return nil
	})
	return names, err
}
