package main

// cmdExplain is `turfc --explain <code>`. The explanation text, and its
// colours, belong to the compiler: turf only spells the flag.
func cmdExplain(args []string) int {
	if len(args) != 1 {
		return usagef("explain: expected exactly one error code, for example: turf explain E001")
	}
	return forward([]string{"--explain", args[0]})
}

const helpExplain = `turf explain - explain an error code

Usage:
  turf explain <code>

For example: turf explain E001. The code is the [E0NN] shown in an error's
header. Runs turfc --explain.
`
