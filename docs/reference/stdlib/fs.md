# Files and directories: `fs`

Path strings and directory operations. Not prelude: it needs `import fs;`.

```turflang
import fs;
```

`fs` is about **paths and directories**. Reading and writing the contents of a
file, and asking whether a path exists, are in [`io`](io_and_net.md#files-fileerror-read_to_string-read_lines-write_string-exists).

The module has two halves:

- **Path functions** (`join`, `dirname`, `basename`, `extension`, `is_absolute`)
  work on strings alone. They never touch the disk, never fail, and the path
  does not need to exist. They understand `/` as the separator and nothing else.
- **Directory functions** (`mkdir`, `remove_file`, `remove_dir`, `is_dir`) act
  on the file system. The three that change it return `!void` and fail with an
  `FsError` that carries the path; `is_dir` is a question, so it returns a `bool`.

---

## Path functions

| Function | Result | Examples |
|---|---|---|
| `join(a: string, b: string): string` | `a` and `b` with exactly one `/` between them | `join("a", "b")` is `a/b`; `join("a/", "b")` is `a/b`; `join("a", "")` is `a/` |
| `dirname(path: string): string` | everything before the last `/` | `dirname("a/b/c.txt")` is `a/b`; `dirname("c.txt")` is `.`; `dirname("/c.txt")` is `/`; `dirname("a/b/")` is `a/b` |
| `basename(path: string): string` | everything after the last `/`, or `path` itself if there is none | `basename("a/b/c.txt")` is `c.txt`; `basename("c.txt")` is `c.txt`; `basename("a/b/")` is `""` |
| `extension(path: string): string` | the text after the last `.` of the basename, or `""` | `extension("a/b/c.txt")` is `txt`; `extension("a.tar.gz")` is `gz`; `extension("noext")` is `""` |
| `is_absolute(path: string): bool` | whether `path` starts with `/` | `is_absolute("/usr")` is true; `is_absolute("usr")` is false |

Details worth knowing:

- `join` only adds a separator; it does not clean up. `join("a", "/b")` is `a//b`,
  and `join("", "b")` is `/b`. It joins two paths, so build a longer one with
  nested calls: `fs::join(fs::join("out", "logs"), "today.txt")`.
- `dirname` and `basename` treat a trailing `/` as part of the name, so a path
  that ends in `/` has an empty basename. Strip the slash first if you may be
  given one.
- `extension` ignores a dot that begins the basename: `extension(".gitignore")`
  is `""`. It looks only at the basename, so `extension("my.dir/file")` is `""`.
  A basename that ends in a dot (`a.`) has the empty extension.

```turflang
import fs;

fn main(): int {
    immut path: string = fs::join("reports/2026", "summary.txt");
    printline(path);                     // reports/2026/summary.txt
    printline(fs::dirname(path));        // reports/2026
    printline(fs::basename(path));       // summary.txt
    printline(fs::extension(path));      // txt
    printline(fs::is_absolute(path));    // false
    printline(fs::is_absolute("/tmp"));  // true
    return 0;
}
```

### Platform note

The path functions split on `/` only, on every platform. On Windows, `/` is a
valid separator for the system calls, so `fs::join("data", "x.txt")` works there,
but a path with backslashes is not split: `dirname("a\\b\\c")` is `.`, and
`is_absolute("C:\\x")` is false. Use `/` in paths you build or pass to these
functions.

---

## Directory functions

### `error FsError`

```text
error FsError {
    MkdirFailed(path: string),
    RemoveFileFailed(path: string),
    RemoveDirFailed(path: string),
}
```

One variant per operation, each carrying the path it failed on, like
[`io::FileError`](io_and_net.md#files-fileerror-read_to_string-read_lines-write_string-exists).
`err == fs::FsError::MkdirFailed` compares the variant alone; a pattern inside a
`compare` reads the path:

```turflang
import fs;

fn main(): int {
    fs::mkdir("data") recover run {
        printline("could not create data");
    };
    fs::mkdir("data") recover as err run {
        compare err {
            matches fs::FsError::MkdirFailed(path):
            printline("mkdir failed for " + path);
            done
            otherwise:
            printline("other failure");
        }
    };
    fs::remove_dir("data") recover run {
        printline("could not remove data");
    };
    return 0;
}
```

```text
mkdir failed for data
```

(The first `mkdir` succeeds, so only the second prints; the directory is
removed at the end.)

### `mkdir(path: string): !void`

Creates one directory with the permissions `rwxr-xr-x` (octal `0755`, reduced by
the process's umask on systems that have one). It fails with
`FsError::MkdirFailed(path)` if the parent does not exist (`mkdir("a/b/c")` does
not create `a` and `a/b`), if `path` already exists, or if the process may not
create it. Create each level in turn to make a nested path.

### `remove_file(path: string): !void`

Deletes the file at `path`. It fails with `FsError::RemoveFileFailed(path)` if
there is no such file or the process may not delete it, and for a directory that
has contents. On Linux it also deletes an **empty** directory; use `remove_dir`
for directories so the intent is plain.

### `remove_dir(path: string): !void`

Removes an **empty** directory. It fails with `FsError::RemoveDirFailed(path)` if
the directory does not exist, is not empty, or is not a directory. It never
deletes contents: remove the files first.

### `is_dir(path: string): bool`

Whether `path` names a directory that can be opened. It is `false` for a file,
for a path that does not exist, and for a directory the process may not read.

### Not in this module

`fs` has no listing of a directory's contents, no file sizes or times, and no
recursive create or delete. To test that a file exists, use `io::exists`
(which is also true for a directory on Linux, so combine it with `is_dir`).

---

## Example: a working directory for a program's output

This creates an output directory if it is not there, writes a report into it
with `io::write_string`, and removes everything again. Each step that can fail
returns a `!T`, so each has a `recover`.

```turflang
import fs;

fn main(): int {
    immut dir: string = "build_out";
    if !fs::is_dir(dir) {
        fs::mkdir(dir) recover run {
            eprintline("cannot create " + dir);
            return 1;
        };
    }

    immut report: string = fs::join(dir, "report.txt");
    io::write_string(report, "all done\n") recover run {
        eprintline("cannot write " + report);
        return 1;
    };
    printline(fs::basename(report) + " is in " + fs::dirname(report));

    fs::remove_file(report) recover run {
        eprintline("cannot delete " + report);
        return 1;
    };
    fs::remove_dir(dir) recover run {
        eprintline("cannot remove " + dir);
        return 1;
    };
    return 0;
}
```

```text
report.txt is in build_out
```
