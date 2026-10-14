# Time: `time`

The wall clock, a CPU stopwatch, and sleeping. Not prelude: it needs
`import time;`.

```turflang
import time;
```

There are two different clocks, and the module's functions are not
interchangeable between them:

| Clock | Functions | Resolution | Advances while the program sleeps or waits |
|---|---|---|---|
| **Wall clock**: the calendar time of day | `now_seconds` | one second | yes |
| **CPU time**: how long this process has been computing | `clock_ticks`, `elapsed_ms`, `elapsed_seconds` | microseconds | **no** |

So `elapsed_ms` measures how much processor time your code used, not how long a
user waited. After `time::sleep_ms(300)` it has advanced by about 0, because a
sleeping program uses no CPU. There is no high-resolution wall-clock stopwatch;
for a duration of seconds, subtract two `now_seconds()` readings.

## Wall clock

### `now_seconds(): int`

The current time as whole seconds since the Unix epoch (1970-01-01 00:00:00
UTC). The value is the same on every platform. It is the only wall-clock
reading, so two calls less than a second apart can return the same number.

## CPU time

### `clock_ticks(): int`

The CPU time this process has consumed so far, in **microseconds**, on every
platform. It is a reading to subtract from a later one, not a timestamp: its
zero point means nothing.

### `elapsed_ms(start_clock_ticks: int): int`

The CPU milliseconds used since `start_clock_ticks`, a value that
`clock_ticks()` returned earlier. It rounds down.

### `elapsed_seconds(start_clock_ticks: int): int`

The same in whole CPU seconds, rounded down.

```turflang
import time;

fn sum_of_squares(n: int): int {
    mut total: int = 0;
    mut i: int = 0;
    while i < n {
        total = total + (i % 1000) * (i % 1000);
        i = i + 1;
    }
    return total;
}

fn main(): int {
    immut started_at: int = time::now_seconds();
    immut start: int = time::clock_ticks();

    immut result: int = sum_of_squares(200000000);

    printline("result: " + (result to string));
    printline("cpu: " + (time::elapsed_ms(start) to string) + " ms");
    printline("wall: " + ((time::now_seconds() - started_at) to string) + " s");
    return 0;
}
```

The program prints the result, then two durations whose values depend on the
machine.

## Sleeping

### `sleep_seconds(n: int): void`

Suspends the program for `n` wall-clock seconds. `n` of zero or less returns at
once.

### `sleep_ms(n: int): void`

Suspends the program for `n` wall-clock milliseconds, with the same behaviour for
zero or less. Waits longer than a second are done in one-second steps, so a large
`n` is safe.

```turflang
import time;

fn main(): int {
    immut before: int = time::now_seconds();
    immut cpu: int = time::clock_ticks();

    time::sleep_ms(1500);

    printline("slept about " + ((time::now_seconds() - before) to string) + " s of wall time");
    printline("used " + (time::elapsed_ms(cpu) to string) + " ms of CPU");
    return 0;
}
```

```text
slept about 1 s of wall time
used 0 ms of CPU
```

The wall figure is 1 or 2, depending on where in the second the sleep began,
because `now_seconds` counts whole seconds.

## Constants

### `CLOCKS_PER_SEC: int`

`1000000`, the number of `clock_ticks` in one second.

### `MAX_USLEEP_USEC: int`

`1000000`, the longest single `usleep` call the module makes (one second); the
sleep functions split longer waits into steps of this size.

## The C bindings

Three raw C functions are public as they are, with the C library's own
behaviour:

| Binding | Signature |
|---|---|
| `time::time` | `time(t: ptr): int`: the epoch seconds, as `now_seconds` does; pass `null` for `t` |
| `time::clock` | `clock(): int`: the C library's CPU clock |
| `time::usleep` | `usleep(usec: int32): int32`: sleep for `usec` microseconds; the C library leaves a value of a second or more undefined |

Prefer `now_seconds`, `clock_ticks` and the `sleep_*` functions. In particular
`clock()` counts ticks of a platform-specific size (and on Windows it is a
wall-clock reading), so only `clock_ticks` and `CLOCKS_PER_SEC` agree with each
other everywhere.

## Failure

None of these functions fail or panic. Sleeping and reading a clock have no
error case, and a negative duration is treated as zero.
