package main

import "testing"

// Every usage error is decided before a manifest is opened, so these run from
// any directory.
func TestAddUsageErrors(t *testing.T) {
	for name, args := range map[string][]string{
		"no name":                 {"--git", "u", "--tag", "v"},
		"git with no ref":         {"x", "--git", "u"},
		"git with two refs":       {"x", "--git", "u", "--tag", "v", "--branch", "main"},
		"git with three refs":     {"x", "--git", "u", "--tag", "v", "--branch", "b", "--rev", "abcd"},
		"a ref with no git":       {"x", "--tag", "v"},
		"a rev with path":         {"x", "--path", "../x", "--rev", "abcd"},
		"path and git":            {"x", "--path", "../x", "--git", "u", "--tag", "v"},
		"neither path nor git":    {"x"},
		"--git without a value":   {"x", "--git"},
		"--tag without a value":   {"x", "--git", "u", "--tag"},
		"an empty tag":            {"x", "--git", "u", "--tag="},
		"the same ref twice":      {"x", "--git", "u", "--tag", "a", "--tag", "b"},
		"an unknown flag":         {"x", "--git", "u", "--tag", "v", "--frobnicate"},
		"a second name":           {"x", "y", "--git", "u", "--tag", "v"},
		"--offline with add":      {"x", "--git", "u", "--tag", "v", "--offline"},
		"--locked with add":       {"x", "--git", "u", "--tag", "v", "--locked"},
		"--git=... without a ref": {"x", "--git=u"},
	} {
		if got := cmdAdd(args); got != exitUsage {
			t.Errorf("%s: exit %d, want %d", name, got, exitUsage)
		}
	}
}

func TestUpdateRejectsFlags(t *testing.T) {
	if got := cmdUpdate([]string{"--offline"}); got != exitUsage {
		t.Errorf("update --offline: exit %d, want %d", got, exitUsage)
	}
	if got := cmdUpdate([]string{"--frobnicate"}); got != exitUsage {
		t.Errorf("update --frobnicate: exit %d, want %d", got, exitUsage)
	}
}

func TestFetchTakesOnlyItsFlags(t *testing.T) {
	if got := cmdFetch([]string{"jsonx"}); got != exitUsage {
		t.Errorf("fetch jsonx: exit %d, want %d", got, exitUsage)
	}
}
