package main

import (
	"fmt"
	"net/http"
	"os"

	"github.com/turf-lang/turf-cli/registry/handlers"
)

func main() {
	if err := os.MkdirAll("registry-data", 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	http.HandleFunc("/publish", handlers.Publish)
	http.HandleFunc("/package/", handlers.Fetch)

	fmt.Println("registry listening on :8080")
	if err := http.ListenAndServe(":8080", nil); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}
