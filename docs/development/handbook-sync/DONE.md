# DONE.md — handbook edits completed

Rows moved here from `PENDING.md`, each with the commit that did the work.
The handbook itself is still paused; rows land here as non-handbook items
in the queue are done.

| Feature | Chapter / file | What was done | Commit |
|---|---|---|---|
| LEC-001 | `docs/reference/cli_and_diagnostics.md` and the new `docs/reference/projects_and_packages.md` (the reference half of the LEC-001 `cli_and_diagnostics.md` row) | Command-line sections rewritten around `turf` and `turfc` (commands, passthrough, handshake, discovery, single-file commands, a "for tool authors" section); error-code count (68 → 91) and Important list corrected, E090/E091 added; new reference page for projects, packages, dependencies, `turf.lock` and the cache; both linked from `docs/reference/README.md`. The OPT-001 and TOOL-001 rows for this file stay open: the `-O*`, `-g`, `--target` and `-mcpu` flags are now in the table, but the F-OPT-1 contract paragraph and the `gdb` walkthrough are not written. | `docs(reference): projects_and_packages.md (@a2ys)` and `docs(reference): cli_and_diagnostics.md around turf and turfc (@a2ys)` (LEC-001 stage 8, 2026-09-30) |
| FDN-005 | `docs/development/INVARIANTS.md` `INV-RT-4` | Added the SysV64 execution evidence (`TURF_GC_VALIDATE`'s register cross-check and full-stack mark-set comparison, zero misses) and recorded the no-`/proc` fallback that did silently under-scan, now fixed. | `docs(development): record the FDN-005 follow-up` (the FDN-005 follow-up, 2026-09-29) |
