/* REL-005: structs.tr in C, built with `clang -O3` by scripts/bench.py so the
 * Turf/C ratio tracks F-24 (checked arithmetic vs auto-vectorization). Keep the
 * two programs computing the same thing: the checksums must match. */
#include <stdint.h>
#include <stdio.h>

typedef struct {
    int64_t x, y, vx, vy;
} Particle;

int main(void) {
    enum { COUNT = 1024 };
    const int64_t steps = 500000;
    static Particle ps[COUNT];
    for (int64_t i = 0; i < COUNT; i++) {
        ps[i].x = i;
        ps[i].y = COUNT - i;
        ps[i].vx = 0;
        ps[i].vy = 0;
    }
    for (int64_t step = 0; step < steps; step++) {
        for (int64_t i = 0; i < COUNT; i++) {
            const int64_t d = (i % 5) - 2;
            ps[i].vx += d;
            ps[i].vy -= d;
            ps[i].x += ps[i].vx;
            ps[i].y += ps[i].vy;
        }
    }
    int64_t sum = 0;
    for (int64_t i = 0; i < COUNT; i++)
        sum += ps[i].x - ps[i].y;
    printf("%lld\n", (long long)sum);
    return 0;
}
