# Examples

Multi-package TurfLang projects, built with `turf`. They are the walkthrough
of `docs/planning/ROADMAP_v2.0.0.md` Part IX as files you can run, and the
reference for how a project is laid out. The full manual is
[`docs/reference/projects_and_packages.md`](../docs/reference/projects_and_packages.md).

You need `turf` and `turfc` built (`./scripts/cmake_build.sh --release`, then
`./scripts/dev_install.sh`, or run `build/turf` directly). Each project builds
into its own `build/` directory, which is git-ignored.

| Project | Kind | What it shows |
|---|---|---|
| [`geometry/`](geometry/) | library | A package's root module (`src/geometry.tr`, the only module another package can import) and an internal module (`src/vecmath.tr`, which stays private to the package). One `pub` generic function. |
| [`plotter/`](plotter/) | program | A path dependency (`geometry = { path = "../geometry" }`), and a generic function of `geometry` called with a struct that only `plotter` declares: generics are compiled from source across a package boundary. |

## Running them

```sh
cd examples/plotter
turf run
```

prints

```
distance squared: 25
manhattan: 7
picked the sprite drawn as *
a Sprite is 32 bytes
an int is 8 bytes
```

`turf build` in `plotter/` leaves the executable at `build/plotter`.
`turf check` in `geometry/` checks the library without producing anything;
there is nothing to run in a library, and `turf run` there says so.

Both directories hold a `turf.lock`, which `turf` generates and which is meant
to be committed. It has no git entries here, because both packages are `path`
dependencies, which are live source rather than something to pin.

## A git dependency

`plotter` uses a `path` dependency so it builds with no network. The same
dependency from a git repository is written

```toml
[dependencies]
geometry = { git = "https://github.com/someone/geometry", tag = "v0.1.0" }
```

with exactly one of `tag`, `rev` or `branch`, and `turf build` then fetches it
into the cache and pins the commit and a hash of the tree in `turf.lock`. This
directory does not carry that form because building it needs a repository to
fetch from. It is exercised end to end, against local bare repositories and no
network, by [`scripts/git_deps_smoke.sh`](../scripts/git_deps_smoke.sh).
