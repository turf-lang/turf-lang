# Package graph fixtures

What a consumer's tests read (IF-2, IF-5). `graph.json` is a package graph as
`turf test` and `turf graph` write it (with the root's `tests`), `build-graph.json`
as `turf build` writes it, and `report.json` is the shape of `turf graph
--unsafe`. Paths are relative to this directory; the `app` and `shapes`
packages they name are here. `scripts/check/diagnostics_json_check.py` checks
each against its schema and compiles and tests `app` through the graph.
