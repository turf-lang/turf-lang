package main

import (
	"fmt"
	"os"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: turf <command>")
		os.Exit(1)
	}

	var err error

	switch os.Args[1] {
	case "init":
		err = runInit(os.Args[2:])
	case "install":
		err = runInstall(os.Args[2:])
	case "publish":
		err = runPublish(os.Args[2:])
	case "add":
		err = runAdd(os.Args[2:])
	case "remove":
		err = runRemove(os.Args[2:])
	case "update":
		err = runUpdate(os.Args[2:])
	default:
		fmt.Fprintf(os.Stderr, "unknown command: %s\n", os.Args[1])
		os.Exit(1)
	}

	if err != nil {
		fmt.Fprintf(os.Stderr, "turf: %v\n", err)
		os.Exit(1)
	}
}
