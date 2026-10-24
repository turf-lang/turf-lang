/* NM-1: particles.tr in C, built with `clang -O3` by scripts/dev/bench.py so the
 * Turf/C ratio tracks #257's figure (checked arithmetic vs auto-vectorization).
 * Keep the two programs computing the same thing: the checksums must match. */
#include <stdint.h>
#include <stdio.h>

typedef struct {
    int64_t x, y, vx, vy;
} Particle;

int main(void) {
    enum { COUNT = 2000 };
    const int64_t steps = 20000;
    static Particle ps[COUNT];
    for (int64_t i = 0; i < COUNT; i++) {
        ps[i].x = i;
        ps[i].y = COUNT - i;
        ps[i].vx = (i % 7) - 3;
        ps[i].vy = 3 - (i % 5);
    }
    for (int64_t step = 0; step < steps; step++) {
        for (int64_t i = 0; i < COUNT; i++) {
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
