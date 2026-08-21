# `time`

Wall-clock time, CPU-time measurement, and sleeping. Not prelude — requires
an explicit `import time;`.

```turflang
import time;
```

### `now_seconds(): int`

Seconds since the Unix epoch (wraps libc `time(NULL)`) — real wall-clock
time.

```turflang
test::assert(time::now_seconds() > 1735689600, "after 2025-01-01");
```

### `clock_ticks(): int` / `CLOCKS_PER_SEC: int = 1000000`

Wraps libc `clock()` — **CPU time consumed by this process, not wall-clock
time.** Divide a difference in ticks by `CLOCKS_PER_SEC` to get seconds of
CPU time. `elapsed_ms`/`elapsed_seconds` below do this division for you.

> **Gotcha: this is not what you want for timing a `sleep`.** Sleeping
> consumes essentially no CPU time, so `elapsed_ms` measured across a sleep
> stays near zero — this is correct behavior for a CPU-time clock, not a
> bug, but it surprises people expecting wall-clock elapsed time. Use
> `now_seconds()` (before/after, both in seconds) if you need to measure
> real elapsed time across a sleep or I/O wait; use `clock_ticks()` if you
> need to measure how much CPU work something actually did.

### `elapsed_ms(start_clock_ticks: int): int` / `elapsed_seconds(start_clock_ticks: int): int`

Takes a `clock_ticks()` value captured earlier and returns CPU-time elapsed
since then, in milliseconds or seconds.

```turflang
immut start: int = time::clock_ticks();
// ... CPU-bound work ...
io::printline((time::elapsed_ms(start) to string) + "ms of CPU time");
```

### `sleep_seconds(n: int): void` / `sleep_ms(n: int): void`

Wall-clock sleeps, built on `usleep`. Both loop internally in ≤1-second
chunks (`MAX_USLEEP_USEC: int = 1000000`) rather than one large
multiplied call, to stay under `usleep`'s practical per-call ceiling and
avoid `int32` overflow on a large request.

```turflang
time::sleep_ms(50);
time::sleep_seconds(1);
```
