package main

import (
	"reflect"
	"testing"
)

// One case per row of the dispatch table in docs/development/changes/LEC-001.md
// (FREEZE.md 6.16 "Passthrough"), plus the ordering traps between rows.
func TestClassify(t *testing.T) {
	tests := []struct {
		name string
		argv []string
		want route
	}{
		{"no arguments", nil, route{kind: routeUsage}},
		{"--version", []string{"--version"}, route{kind: routeCommand, name: "version", args: []string{}}},
		{"version", []string{"version"}, route{kind: routeCommand, name: "version", args: []string{}}},
		{"--help", []string{"--help"}, route{kind: routeCommand, name: "help"}},
		{"-h", []string{"-h"}, route{kind: routeCommand, name: "help"}},
		{"help", []string{"help"}, route{kind: routeCommand, name: "help", args: []string{}}},
		{"help cmd", []string{"help", "run"}, route{kind: routeCommand, name: "help", args: []string{"run"}}},
		{"known command", []string{"check", "a.tr"}, route{kind: routeCommand, name: "check", args: []string{"a.tr"}}},
		{"known command, project", []string{"build"}, route{kind: routeCommand, name: "build", args: []string{}}},
		{"install", []string{"install", "x"}, route{kind: routeInstall}},
		{"file", []string{"t.tr", "-o", "a"}, route{kind: routePassthrough, args: []string{"t.tr", "-o", "a"}}},
		{"flag first", []string{"-O2", "t.tr"}, route{kind: routePassthrough, args: []string{"-O2", "t.tr"}}},
		{"explain flag is passthrough", []string{"--explain", "E001"}, route{kind: routePassthrough, args: []string{"--explain", "E001"}}},
		{"unknown bare word", []string{"biuld"}, route{kind: routeUnknown, name: "biuld"}},
		{"a file without .tr is not passed on", []string{"main"}, route{kind: routeUnknown, name: "main"}},
		{"empty word", []string{""}, route{kind: routeUnknown, name: ""}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := classify(tc.argv)
			// nil and empty argument lists mean the same thing.
			if len(got.args) == 0 {
				got.args = tc.want.args
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("classify(%q) = %+v, want %+v", tc.argv, got, tc.want)
			}
		})
	}
}

func TestSuggest(t *testing.T) {
	names := commandNames()
	tests := []struct {
		in   string
		want string
		ok   bool
	}{
		{"biuld", "build", true}, // transposition, distance 2
		{"buil", "build", true},  // deletion
		{"chek", "check", true},  // deletion
		{"complie", "compile", true},
		{"formt", "fmt", true}, // distance 2: still a hit
		{"format", "", false},  // distance 3 from fmt: no guess beats a wrong one
		{"xyzzy", "", false},
		{"", "", false},
	}
	for _, tc := range tests {
		got, ok := suggest(tc.in, names)
		if got != tc.want || ok != tc.ok {
			t.Errorf("suggest(%q) = %q, %v; want %q, %v", tc.in, got, ok, tc.want, tc.ok)
		}
	}
}

func TestEditDistance(t *testing.T) {
	tests := []struct {
		a, b string
		want int
	}{
		{"", "", 0},
		{"abc", "abc", 0},
		{"abc", "", 3},
		{"", "abc", 3},
		{"kitten", "sitting", 3},
		{"biuld", "build", 2},
		{"flaw", "lawn", 2},
	}
	for _, tc := range tests {
		if got := editDistance(tc.a, tc.b); got != tc.want {
			t.Errorf("editDistance(%q, %q) = %d, want %d", tc.a, tc.b, got, tc.want)
		}
	}
}

// -h/--help shows a command's help only before a `--`; after it the arguments
// belong to the program `turf run` starts.
func TestWantsHelp(t *testing.T) {
	tests := []struct {
		args []string
		want bool
	}{
		{nil, false},
		{[]string{"-h"}, true},
		{[]string{"--help"}, true},
		{[]string{"a.tr", "--help"}, true},
		{[]string{"a.tr", "--", "--help"}, false},
		{[]string{"--", "-h"}, false},
		{[]string{"a.tr", "-O2"}, false},
	}
	for _, tc := range tests {
		if got := wantsHelp(tc.args); got != tc.want {
			t.Errorf("wantsHelp(%q) = %v, want %v", tc.args, got, tc.want)
		}
	}
}

func TestSplitAtDashDash(t *testing.T) {
	before, after := splitAtDashDash([]string{"a.tr", "-O2", "--", "x", "--", "y"})
	if !reflect.DeepEqual(before, []string{"a.tr", "-O2"}) || !reflect.DeepEqual(after, []string{"x", "--", "y"}) {
		t.Fatalf("got %q / %q", before, after)
	}
	before, after = splitAtDashDash([]string{"a.tr"})
	if !reflect.DeepEqual(before, []string{"a.tr"}) || after != nil {
		t.Fatalf("no separator: got %q / %q", before, after)
	}
}

// Every command in the registry has help text and a runner, and every name is
// unique: the help listing and the dispatcher both read the same table.
func TestRegistry(t *testing.T) {
	seen := map[string]bool{}
	for _, c := range commands {
		if seen[c.name] {
			t.Errorf("duplicate command %q", c.name)
		}
		seen[c.name] = true
		if c.help == "" || c.run == nil || c.summary == "" {
			t.Errorf("command %q is missing help, summary or runner", c.name)
		}
	}
	for _, want := range []string{
		"compile", "check", "dump", "explain", "fmt", "migrate", "run",
		"new", "init", "build", "test", "clean", "fetch", "add", "remove", "update",
		"publish", "version", "help",
	} {
		if !seen[want] {
			t.Errorf("command %q is missing from the registry", want)
		}
	}
	if seen["install"] {
		t.Error("`install` must not be a command: it is retired (FREEZE.md 6.16)")
	}
}

func TestDumpFlag(t *testing.T) {
	want := map[string]string{
		"ast":            "--dump-ast",
		"resolved":       "--dump-resolved",
		"typechecked":    "--dump-typechecked",
		"modules":        "--dump-modules",
		"symbols":        "--dump-resolver-symbols",
		"instantiations": "--dump-instantiations",
	}
	if len(dumpKinds) != len(want) {
		t.Fatalf("dumpKinds has %d entries, want %d", len(dumpKinds), len(want))
	}
	for what, flag := range want {
		if got, ok := dumpFlag(what); !ok || got != flag {
			t.Errorf("dumpFlag(%q) = %q, %v; want %q", what, got, ok, flag)
		}
	}
	if _, ok := dumpFlag("nope"); ok {
		t.Error("dumpFlag accepted an unknown name")
	}
}
