package main

import (
	"errors"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/driver"
	"github.com/turf-lang/turf-lang/cli/internal/manifest"
	"github.com/turf-lang/turf-lang/cli/internal/project"
	"github.com/turf-lang/turf-lang/cli/internal/stdlib"
)

const noProjectMsg = "not inside a TurfLang project (no turf.toml here or in any parent directory); " +
	"use 'turf new <name>', or 'turf compile <file.tr>' for a single file"

// buildDirName is where a project's build output goes. The graph file lives
// here too, so `turf clean` removing one directory removes everything turf
// wrote.
const buildDirName = "build"

// projectBuild is a project ready to hand to turfc: its manifest read, its
// graph resolved and checked, and the graph file written.
type projectBuild struct {
	dir      string // the project root, canonical
	graph    *project.Graph
	compiler string // absolute, so it still resolves when the child runs in dir
}

func (p *projectBuild) name() string { return p.graph.Root.Name }

// graphArg and the paths below are relative to dir: turfc runs there, so its
// diagnostics spell a file `src/main.tr` however deep the user's shell is.
func (p *projectBuild) graphArg() string { return buildDirName + "/package-graph.json" }

func (p *projectBuild) exeArg() string {
	e := buildDirName + "/" + p.name()
	if runtime.GOOS == "windows" {
		e += ".exe"
	}
	return e
}

func (p *projectBuild) exePath() string { return filepath.Join(p.dir, filepath.FromSlash(p.exeArg())) }

// checkProjectFlags refuses the two flags turf owns in a project build. The
// executable's name and place are fixed (build/<name>), and a hand-passed
// graph would be silently replaced by, or fight with, the one turf writes.
func checkProjectFlags(flags []string) int {
	for _, f := range flags {
		switch {
		case f == "-o":
			return usagef("a project build writes build/<name> itself, so -o is not accepted; use 'turf compile -o …' to name the executable")
		case f == "--package-graph" || strings.HasPrefix(f, "--package-graph="):
			return usagef("turf writes the package graph itself (build/package-graph.json); --package-graph is not accepted here")
		}
	}
	return 0
}

// withProject does everything a project command needs before it can call the
// compiler, in the order that gives the most useful first error: find the
// project and read its manifest (no compiler needed to say "no turf.toml"),
// then locate the compiler, then resolve and check the graph, then write it.
func withProject(fn func(p *projectBuild) int) int {
	cwd, err := os.Getwd()
	if err != nil {
		return failf("cannot determine the current directory: %v", err)
	}
	dir, err := project.FindRoot(cwd)
	if errors.Is(err, project.ErrNoProject) {
		return usagef("%s", noProjectMsg)
	} else if err != nil {
		return failf("%v", err)
	}
	m, err := manifest.Load(filepath.Join(dir, project.ManifestFile))
	if err != nil {
		return failf("%v", err)
	}

	return withCompiler(func(compiler string) int {
		if abs, err := filepath.Abs(compiler); err == nil {
			compiler = abs
		}
		g, err := project.Resolve(m)
		if err != nil {
			return failf("%v", err)
		}
		info, err := driver.Probe(compiler)
		if err != nil {
			return failf("%v", err)
		}
		if err := g.CheckCompilerVersion(info.Release, compiler); err != nil {
			return failf("%v", err)
		}
		// Without a readable stdlib the check is skipped, not failed: turfc
		// rejects the same names itself and says so with its own message.
		if std, err := stdlib.Modules(stdlib.InstallRoot(compiler)); err == nil {
			if err := g.CheckStdlibNames(std); err != nil {
				return failf("%v", err)
			}
		}
		root := g.Root
		if !root.HasMain() && !root.IsLibrary() {
			return failf("package '%s' has neither src/main.tr (a program) nor src/%s.tr (a library)", root.Name, root.Name)
		}

		data, err := g.JSON()
		if err != nil {
			return failf("cannot render the package graph: %v", err)
		}
		p := &projectBuild{dir: root.Dir, graph: g, compiler: compiler}
		if err := os.MkdirAll(filepath.Join(p.dir, buildDirName), 0o755); err != nil {
			return failf("cannot create %s: %v", filepath.Join(p.dir, buildDirName), err)
		}
		if err := os.WriteFile(filepath.Join(p.dir, filepath.FromSlash(p.graphArg())), data, 0o644); err != nil {
			return failf("cannot write the package graph: %v", err)
		}
		return fn(p)
	})
}

// compile runs turfc over the project. A program (src/main.tr) is compiled, or
// only checked; a library (src/<name>.tr) is always only checked, with --module
// because its root module has no main. Checking the library even when a
// program is present is what surfaces a library's errors when the program does
// not happen to import it. The first failing status is the result and later
// steps are skipped: when the program fails to compile, a second run over a
// library it imports would only repeat the same diagnostics.
func (p *projectBuild) compile(flags []string, checkOnly bool) int {
	root := p.graph.Root
	run := func(args []string) int {
		return driver.RunIn(p.dir, p.compiler, append(args, flags...))
	}
	if root.HasMain() {
		args := []string{"--package-graph", p.graphArg(), "src/main.tr"}
		if checkOnly {
			args = append([]string{"--check"}, args...)
		} else {
			args = append(args, "-o", p.exeArg())
		}
		if st := run(args); st != 0 {
			return st
		}
	}
	if root.IsLibrary() {
		args := []string{"--check", "--module", "--package-graph", p.graphArg(), "src/" + root.Name + ".tr"}
		if st := run(args); st != 0 {
			return st
		}
	}
	return 0
}

// catchInterrupt makes turf survive Ctrl-C so it can still return the child's
// status. A handler that discards the signal (signal.Notify) is used rather
// than signal.Ignore: an ignored disposition is inherited across exec and the
// program would then ignore Ctrl-C too, whereas a caught signal is reset to the
// default in a child. Call the returned function to restore.
func catchInterrupt() (stop func()) {
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, os.Interrupt)
	return func() { signal.Stop(sigs) }
}

func projectRun(flags, programArgs []string) int {
	if code := checkProjectFlags(flags); code != 0 {
		return code
	}
	return withProject(func(p *projectBuild) int {
		if !p.graph.Root.HasMain() {
			return failf("'%s' is a library; there is nothing to run", p.name())
		}
		defer catchInterrupt()()
		if st := p.compile(flags, false); st != 0 {
			return st
		}
		if st, err := os.Stat(p.exePath()); err != nil || st.IsDir() {
			// e.g. --emit-llvm: the compiler succeeded but there is nothing to run.
			return failf("the compiler produced no executable (%s)", p.exeArg())
		}
		return driver.Run(p.exePath(), programArgs)
	})
}

func cmdBuild(args []string) int {
	if code := checkProjectFlags(args); code != 0 {
		return code
	}
	return withProject(func(p *projectBuild) int { return p.compile(args, false) })
}

func projectCheck(flags []string) int {
	if code := checkProjectFlags(flags); code != 0 {
		return code
	}
	return withProject(func(p *projectBuild) int { return p.compile(flags, true) })
}

func cmdClean(args []string) int {
	if len(args) > 0 {
		return usagef("'clean' takes no arguments (got '%s')", args[0])
	}
	cwd, err := os.Getwd()
	if err != nil {
		return failf("cannot determine the current directory: %v", err)
	}
	dir, err := project.FindRoot(cwd)
	if errors.Is(err, project.ErrNoProject) {
		return usagef("%s", noProjectMsg)
	} else if err != nil {
		return failf("%v", err)
	}
	// Nothing to remove is success: clean is meant to be safely repeatable.
	if err := os.RemoveAll(filepath.Join(dir, buildDirName)); err != nil {
		return failf("cannot remove %s: %v", filepath.Join(dir, buildDirName), err)
	}
	return 0
}

const helpBuild = `turf build - build the project

Usage:
  turf build [compiler flags...]

Run anywhere inside a project (a directory tree with a turf.toml). Reads the
manifests, resolves the path dependencies into a package graph, writes it to
build/package-graph.json, and drives the compiler with it:

  src/main.tr present      turfc --package-graph ... src/main.tr -o build/<name>
  src/<name>.tr present    turfc --check --module --package-graph ... src/<name>.tr

so a library's errors show even when no program imports it. Compiler flags such
as -O2 or -g are passed through. -o and --package-graph are refused: turf owns
both. The exit status is the first failing compiler status.

Dependencies are 'path' dependencies for now; git dependencies and the lockfile
arrive in LEC-001 stage 6.
`

const helpClean = `turf clean - remove the project's build output

Usage:
  turf clean

Deletes <project>/build/, which holds the executable and the package graph.
Nothing to delete is not an error.
`
