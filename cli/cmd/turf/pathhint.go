package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// pathHint returns a note for someone running this turf by its full path
// (`./build/turf`, `C:\...\turf.exe`) while typing `turf` would not reach it,
// or "" when it would. It gives the exact command that fixes that for their
// shell, because "add it to your PATH" alone assumes the reader knows which
// file to edit and what to write.
//
// FOR NOW ONLY. It is shown by `turf version` and `turf help`, the commands
// someone runs first, and it only ever prints: turf does not edit shell
// start-up files or the Windows registry behind the user's back. Real PATH
// management on every OS (bash/zsh/fish, macOS login shells, the Windows user
// PATH) is the release installers' job (ROADMAP_v2.0.0.md REL-008 stage 7,
// install.sh / install.ps1). When those land, revisit this and the matching
// note in scripts/dev_install.sh.
func pathHint() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	if real, err := filepath.EvalSymlinks(exe); err == nil {
		exe = real
	}
	onPath, lookErr := exec.LookPath("turf")
	if lookErr == nil {
		if real, err := filepath.EvalSymlinks(onPath); err == nil && real == exe {
			return ""
		}
	}

	dir := filepath.Dir(exe)
	var b strings.Builder
	if lookErr == nil {
		fmt.Fprintf(&b, "note: 'turf' on your PATH is %s, not this one (%s).\n", onPath, exe)
	} else {
		fmt.Fprintf(&b, "note: this turf (%s) is not on your PATH, so typing 'turf' will not find it.\n", exe)
	}

	// A source checkout has a script that links turf into ~/.local/bin (only
	// turf, not turfc); prefer it over putting the whole build/ directory on
	// PATH.
	script := filepath.Join(filepath.Dir(dir), "scripts", "dev_install.sh")
	if st, err := os.Stat(script); err == nil && !st.IsDir() && runtime.GOOS != "windows" {
		fmt.Fprintf(&b, "      To run it from anywhere:  %s   (links it into ~/.local/bin)\n", script)
		b.WriteString("      and follow the line it prints if ~/.local/bin is not on PATH either.\n")
		return b.String()
	}

	b.WriteString("      To run it from anywhere, " + addToPathCommand(dir) + "\n")
	return b.String()
}

// addToPathCommand is the command that puts dir on PATH for the user's shell.
func addToPathCommand(dir string) string {
	if runtime.GOOS == "windows" {
		// The user PATH, which new terminals pick up; no admin rights needed.
		return fmt.Sprintf("run in PowerShell, then open a new terminal:\n"+
			"        [Environment]::SetEnvironmentVariable(\"Path\", \"%s;\" + [Environment]::GetEnvironmentVariable(\"Path\", \"User\"), \"User\")", dir)
	}
	export := fmt.Sprintf(`export PATH="%s:$PATH"`, dir)
	switch filepath.Base(os.Getenv("SHELL")) {
	case "zsh":
		return fmt.Sprintf("run (zsh), then open a new terminal:\n        echo '%s' >> ~/.zshrc", export)
	case "bash":
		rc := "~/.bashrc"
		if runtime.GOOS == "darwin" {
			// macOS starts bash as a login shell, which reads ~/.bash_profile.
			rc = "~/.bash_profile"
		}
		return fmt.Sprintf("run (bash), then open a new terminal:\n        echo '%s' >> %s", export, rc)
	case "fish":
		return fmt.Sprintf("run (fish):\n        fish_add_path %s", dir)
	default:
		return fmt.Sprintf("add this line to your shell's start-up file:\n        %s", export)
	}
}

// printPathHint writes pathHint to stderr, so it never mixes into output a
// script reads (`turf version` on stdout is parsed by people and tools).
func printPathHint() {
	if h := pathHint(); h != "" {
		fmt.Fprint(os.Stderr, h)
	}
}
