package driver

import (
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"

	"github.com/turf-lang/turf-lang/cli/internal/version"
)

// CompilerInfo is what `turfc --version` reports: line 1 is
// `turfc <release version>`, line 2 is `interface <major>.<minor>`.
type CompilerInfo struct {
	Release   string
	Interface version.Interface
	// Extra holds the later `<key> <value>` lines (llvm, runtime, root), in the
	// compiler's order, for `turf version` to show. Nothing decides anything
	// from them: only lines 1 and 2 are a contract.
	Extra []string
}

// Probe runs `compiler --version` and parses its first two lines. Later lines
// are kept verbatim in Extra but never interpreted, so the compiler may add
// more without breaking this turf.
func Probe(compiler string) (CompilerInfo, error) {
	out, err := exec.Command(compiler, "--version").Output()
	if err != nil {
		return CompilerInfo{}, &Error{
			Msg:  fmt.Sprintf("cannot run %s --version: %v", compiler, err),
			Hint: envHint,
		}
	}
	lines := strings.Split(strings.ReplaceAll(string(out), "\r", ""), "\n")
	if lines[0] == "" {
		return CompilerInfo{}, &Error{
			Msg:  fmt.Sprintf("%s printed nothing for --version, so it does not look like turfc", compiler),
			Hint: envHint,
		}
	}
	release, ok := strings.CutPrefix(lines[0], "turfc ")
	if !ok {
		return CompilerInfo{}, &Error{
			Msg:  fmt.Sprintf("%s does not look like turfc: --version printed %q", compiler, lines[0]),
			Hint: envHint,
		}
	}
	info := CompilerInfo{Release: release}
	if len(lines) < 2 || !parseInterface(lines[1], &info.Interface) {
		return info, &Error{
			Msg: fmt.Sprintf("turfc %s (%s) reports no driver interface, so it predates this turf "+
				"(turf %s needs interface %d.%d or a later %d.x)",
				release, compiler, version.Version, version.Required.Major,
				version.Required.Minor, version.Required.Major),
			Hint: "upgrade turfc, or point TURF_COMPILER at a newer one",
		}
	}
	for _, l := range lines[2:] {
		if l != "" {
			info.Extra = append(info.Extra, l)
		}
	}
	return info, nil
}

func parseInterface(line string, into *version.Interface) bool {
	rest, ok := strings.CutPrefix(line, "interface ")
	if !ok {
		return false
	}
	maj, min, ok := strings.Cut(rest, ".")
	if !ok {
		return false
	}
	var err1, err2 error
	into.Major, err1 = strconv.Atoi(maj)
	into.Minor, err2 = strconv.Atoi(min)
	return err1 == nil && err2 == nil
}

// Handshake checks that compiler speaks the driver interface this turf needs:
// the same major, and at least the required minor (FREEZE.md 6.16, F-PKG-1).
// Release versions are deliberately not compared -- turf and turfc are
// released independently, and a TurfLang 2.0.0 compiler driven by CLI 1.0.0
// is a supported pairing. TURF_COMPILER does not bypass the check.
func Handshake(compiler, turfPath string) error {
	info, err := Probe(compiler)
	if err != nil {
		return err
	}
	need, got := version.Required, info.Interface
	if got.Major == need.Major && got.Minor >= need.Minor {
		return nil
	}
	// Which side to upgrade follows from which one is behind.
	hint := "upgrade turfc, or point TURF_COMPILER at one that speaks the interface above"
	if got.Major > need.Major {
		hint = "upgrade turf: this compiler's interface is newer than this turf understands"
	}
	return &Error{
		Msg: fmt.Sprintf("this compiler does not speak the interface this turf needs\n"+
			"  turf  %s  needs interface %d.%d or a later %d.x  (%s)\n"+
			"  turfc %s  speaks interface %d.%d  (%s)",
			version.Version, need.Major, need.Minor, need.Major, turfPath,
			info.Release, got.Major, got.Minor, compiler),
		Hint: hint,
	}
}

// Locate finds turfc and runs the handshake, the two steps every command that
// drives the compiler starts with.
func Locate() (string, error) {
	compiler, err := Discover()
	if err != nil {
		return "", err
	}
	turfPath, _ := os.Executable()
	if err := Handshake(compiler, turfPath); err != nil {
		return "", err
	}
	return compiler, nil
}
