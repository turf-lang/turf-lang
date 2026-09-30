package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/driver"
	"github.com/turf-lang/turf-lang/cli/internal/manifest"
	"github.com/turf-lang/turf-lang/cli/internal/project"
	"github.com/turf-lang/turf-lang/cli/internal/stdlib"
)

// fallbackTurfRequirement is written into a new manifest's `turf` key when the
// compiler cannot be asked. Better a working pin than a project that refuses
// to be created because turfc is not installed yet.
const fallbackTurfRequirement = "2.0"

// compilerRequirement is the `turf = "M.m"` pin for a new manifest: the
// compiler's own major.minor, so a project starts out saying "this compiler or
// later in this major", which is exactly what it was made with.
func compilerRequirement() string {
	compiler, err := driver.Discover()
	if err == nil {
		var info driver.CompilerInfo
		if info, err = driver.Probe(compiler); err == nil {
			var major, minor int
			if major, minor, err = project.ReleaseMajorMinor(info.Release); err == nil {
				return fmt.Sprintf("%d.%d", major, minor)
			}
		}
	}
	fmt.Fprintf(os.Stderr, "turf: note: cannot determine the compiler version (%s); writing turf = %q, edit it if that is wrong\n",
		firstLine(err.Error()), fallbackTurfRequirement)
	return fallbackTurfRequirement
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}

// stdlibModules lists the stdlib module names, or returns nil when there is no
// stdlib to read (no compiler and no $TURF_HOME). Callers skip the check on nil:
// turfc rejects the same names itself when it builds.
func stdlibModules() map[string]bool {
	compiler, err := driver.Discover()
	if err != nil {
		compiler = ""
	}
	if compiler == "" && os.Getenv("TURF_HOME") == "" {
		return nil
	}
	std, err := stdlib.Modules(stdlib.InstallRoot(compiler))
	if err != nil {
		return nil
	}
	return std
}

// checkNewName applies the manifest's name rules, plus F-PKG-2's: a library
// named like a stdlib module could never be imported, because `import <name>;`
// would name the stdlib module (or, worse, be an ambiguity error at every use).
func checkNewName(name string, lib bool) error {
	if msg := manifest.CheckName(name); msg != "" {
		return errors.New(msg)
	}
	if lib {
		if std := stdlibModules(); std[name] {
			return fmt.Errorf("'%s' is the name of a standard-library module, so a library called that could never be imported (`import %s;` would name the stdlib module)\nhint: pick another name", name, name)
		}
	}
	return nil
}

func manifestText(name, turfReq string) string {
	return fmt.Sprintf("[package]\nname = %q\nversion = \"0.1.0\"\nturf = %q\n\n[dependencies]\n", name, turfReq)
}

// mainTemplate is src/main.tr for a new package. Every new package gets one,
// a library too, so `turf run` works straight after `turf new`: the first
// thing anyone does with a new project is run it. In a library it also shows
// how the package's own root module is imported.
func mainTemplate(name string, lib bool) string {
	head := "// src/main.tr: this package's entry point. `turf run` builds and runs it.\n"
	if !lib {
		return head + fmt.Sprintf("import io;\n\nfn main(): int {\n    io::printline(\"Hello, TurfLang! This is %s.\");\n    return 0;\n}\n", name)
	}
	return head + fmt.Sprintf("// The library itself is src/%s.tr; other packages import that, never this.\n"+
		"import io;\nimport %s;\n\nfn main(): int {\n"+
		"    io::printline(\"Hello, TurfLang! %s::answer() is \" + (%s::answer() to string) + \".\");\n"+
		"    return 0;\n}\n", name, name, name, name)
}

// indent prefixes every non-empty line of s, for quoting a file in a message.
func indent(s, prefix string) string {
	lines := strings.Split(strings.TrimRight(s, "\n"), "\n")
	for i, l := range lines {
		if l != "" {
			lines[i] = prefix + l
		}
	}
	return strings.Join(lines, "\n")
}

// readmeTemplate is the README.md of a new package: the handful of commands
// someone needs in a TurfLang project, where they will look first.
func readmeTemplate(name string, lib bool) string {
	var b strings.Builder
	fmt.Fprintf(&b, "# %s\n\nA TurfLang package, created with `turf new`.\n\n", name)
	b.WriteString("## Everyday commands\n\n")
	b.WriteString("| Command | What it does |\n|---|---|\n")
	b.WriteString("| `turf run` | build and run `src/main.tr` |\n")
	fmt.Fprintf(&b, "| `turf build` | build without running; the program is `build/%s` |\n", name)
	b.WriteString("| `turf check` | report every error and warning, without building |\n")
	b.WriteString("| `turf add <dep> --path ../<dep>` | depend on a package on disk |\n")
	b.WriteString("| `turf add <dep> --git <url> --tag <tag>` | depend on a package in a git repository |\n")
	b.WriteString("| `turf remove <dep>` | stop depending on it |\n")
	b.WriteString("| `turf update` | move git dependencies to the newest commit their tag or branch names |\n")
	b.WriteString("| `turf clean` | delete `build/` |\n")
	b.WriteString("| `turf fmt --write src/main.tr` | format a file in place |\n")
	b.WriteString("| `turf explain E090` | explain an error code |\n")
	b.WriteString("| `turf help` | every command |\n\n")
	b.WriteString("## Layout\n\n")
	b.WriteString("- `turf.toml`: the package's name, version and dependencies\n")
	b.WriteString("- `turf.lock`: pins every dependency; written by the first build, commit it\n")
	b.WriteString("- `src/main.tr`: the entry point, what `turf run` runs\n")
	if lib {
		fmt.Fprintf(&b, "- `src/%s.tr`: the library's root module, the only module other packages can `import %s;`\n", name, name)
	}
	b.WriteString("- `build/`: build output, ignored by git\n")
	return b.String()
}

func libTemplate(name string) string {
	return fmt.Sprintf("// The root module of the package '%s': the only module another package can\n"+
		"// `import %s;`. Everything here is private unless it is marked `pub`.\n\n"+
		"pub fn answer(): int {\n    return 42;\n}\n", name, name)
}

// parseScaffoldArgs reads `[--lib] [name]`. Anything else that looks like a
// flag is a usage error rather than a name.
func parseScaffoldArgs(args []string) (name string, lib bool, err error) {
	for _, a := range args {
		switch {
		case a == "--lib":
			lib = true
		case strings.HasPrefix(a, "-"):
			return "", false, fmt.Errorf("unknown flag '%s'", a)
		case name != "":
			return "", false, fmt.Errorf("unexpected argument '%s'", a)
		default:
			name = a
		}
	}
	return name, lib, nil
}

func cmdNew(args []string) int {
	name, lib, err := parseScaffoldArgs(args)
	if err != nil {
		return usagef("%v (usage: turf new <name> [--lib])", err)
	}
	if name == "" {
		return usagef("usage: turf new <name> [--lib]")
	}
	if err := checkNewName(name, lib); err != nil {
		return failf("%v", err)
	}
	if _, err := os.Lstat(name); err == nil {
		return failf("cannot create '%s': it already exists", name)
	}
	created, err := scaffold(name, name, lib)
	if err != nil {
		// Do not leave a half-made project behind; the directory did not exist
		// before this command, so removing it is safe.
		os.RemoveAll(name)
		return failf("%v", err)
	}
	kind := "program"
	if lib {
		kind = "library"
	}
	fmt.Printf("Created %s package '%s' in %s/\n", kind, name, name)
	for _, c := range created {
		fmt.Println("  " + c)
	}
	return 0
}

func cmdInit(args []string) int {
	name, lib, err := parseScaffoldArgs(args)
	if err != nil {
		return usagef("%v (usage: turf init [--lib] [name])", err)
	}
	cwd, err := os.Getwd()
	if err != nil {
		return failf("cannot determine the current directory: %v", err)
	}
	if name == "" {
		name = filepath.Base(cwd)
	}
	if err := checkNewName(name, lib); err != nil {
		return failf("%v\nhint: pass a package name explicitly: turf init [--lib] <name>", err)
	}
	if _, err := os.Lstat(project.ManifestFile); err == nil {
		return failf("%s already exists here; this directory is already a project", project.ManifestFile)
	}
	created, err := scaffold(".", name, lib)
	if err != nil {
		return failf("%v", err)
	}
	fmt.Printf("Initialized package '%s' in %s\n", name, cwd)
	for _, c := range created {
		fmt.Println("  " + c)
	}
	return 0
}

// scaffold writes a project into dir. For `turf init` the directory already
// has a life of its own, so nothing that exists is overwritten (a source file
// is skipped, .gitignore is appended to); `turf new` passes a directory that
// did not exist, so those branches never fire there. Only turf.toml is written
// unconditionally, its absence having been checked by the caller. The returned
// lines say what happened to each file.
func scaffold(dir, name string, lib bool) ([]string, error) {
	req := compilerRequirement()
	var out []string
	pre := ""
	if dir != "." {
		pre = dir + "/"
	}
	if err := os.MkdirAll(filepath.Join(dir, "src"), 0o755); err != nil {
		return nil, err
	}

	// O_EXCL: `turf init` checked for turf.toml first, but two of them racing,
	// or a manifest appearing meanwhile, must not be clobbered.
	f, err := os.OpenFile(filepath.Join(dir, project.ManifestFile), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		return nil, err
	}
	_, err = f.WriteString(manifestText(name, req))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return nil, err
	}
	out = append(out, pre+project.ManifestFile)

	// Every package gets src/main.tr so `turf run` works at once; a library
	// also gets its root module. Neither overwrites a file `turf init` finds.
	files := []struct{ rel, content string }{{"src/main.tr", mainTemplate(name, lib)}}
	if lib {
		files = append(files, struct{ rel, content string }{"src/" + name + ".tr", libTemplate(name)})
	}
	files = append(files, struct{ rel, content string }{"README.md", readmeTemplate(name, lib)})
	for _, f := range files {
		path := filepath.Join(dir, filepath.FromSlash(f.rel))
		if _, err := os.Lstat(path); err == nil {
			out = append(out, pre+f.rel+" (already there, left as it is)")
			continue
		}
		if err := os.WriteFile(path, []byte(f.content), 0o644); err != nil {
			return nil, err
		}
		out = append(out, pre+f.rel)
	}

	note, err := ensureGitignore(filepath.Join(dir, ".gitignore"))
	if err != nil {
		return nil, err
	}
	out = append(out, pre+".gitignore"+note)
	return out, nil
}

// ensureGitignore makes sure /build/ is ignored without disturbing what a
// user already ignores: an existing file is appended to, only if the line is
// not already there in any of its usual spellings.
func ensureGitignore(path string) (note string, err error) {
	const line = "/build/"
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return "", os.WriteFile(path, []byte(line+"\n"), 0o644)
	}
	if err != nil {
		return "", err
	}
	sc := bufio.NewScanner(strings.NewReader(string(data)))
	for sc.Scan() {
		switch strings.TrimSpace(sc.Text()) {
		case "/build/", "/build", "build/", "build":
			return " (already ignores build/)", nil
		}
	}
	text := string(data)
	if text != "" && !strings.HasSuffix(text, "\n") {
		text += "\n"
	}
	return " (added /build/)", os.WriteFile(path, []byte(text+line+"\n"), 0o644)
}

const helpNew = `turf new - create a project

Usage:
  turf new <name> [--lib]

Creates <name>/ (which must not exist) holding:

  turf.toml       [package] name/version/turf, and an empty [dependencies]
  src/main.tr     the entry point, a hello-world program: 'turf run' works at once
  README.md       the everyday turf commands and the project's layout
  .gitignore      /build/

With --lib it also creates src/<name>.tr, the package's root module: the
only module another package can import. src/main.tr then imports it, so the
library can be run and tried out on its own. <name> must be a valid
identifier and not a reserved word, because it is spelled in 'import'. A
library named like a standard-library module is refused, since
'import <name>;' could never reach it.

The turf key is written from the compiler's own version (major.minor), or
"2.0" with a note if the compiler cannot be found.
`

const helpInit = `turf init - make the current directory a project

Usage:
  turf init [--lib] [name]

Like 'turf new', in the current directory. The name defaults to the
directory's name and must be a valid package name. A turf.toml already there
is refused. An existing src/main.tr, src/<name>.tr (with --lib) or README.md
is never overwritten, and /build/ is appended to an existing .gitignore only
if it is not already ignored.
`
