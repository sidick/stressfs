/* amiga/tests/prng_test.c — host unit test for stressfs_prng (M0 exit
 * criterion: "host ctest green on a first lib/ unit test (the PRNG
 * with known vectors)").
 *
 * The vectors below were computed independently from the documented
 * algorithm (SplitMix32 seeding + xoshiro128**, see prng.c) with a
 * short reference script, not copied from the implementation under
 * test, so this test can actually catch a regression in either stage
 * of the generator rather than just re-deriving the same numbers the
 * implementation produces.
 */

#include "stressfs/prng.h"

#include <stdio.h>
#include <stdlib.h>

struct vector {
    uint32_t seed;
    uint32_t expect[8];
};

static const struct vector VECTORS[] = {
    {1u, {0x9190299eu, 0xc1017b27u, 0xe3af522fu, 0x7d71fb05u,
          0x787816c2u, 0xfbbe0c00u, 0x5a5c175au, 0x89063de1u}},
    {42u, {0xa91e1cacu, 0x207b36e9u, 0x1c987ffau, 0xd09fde9eu,
           0x14404494u, 0x06bee32au, 0x03e4c0b2u, 0x424bc820u}},
    {0xdeadbeefu, {0x897ad89du, 0x998f30dfu, 0x4a8bba06u, 0xa0e20ce0u,
                   0x8a40cc10u, 0xbdc3ff9fu, 0x2a8da947u, 0xdc58ee47u}},
};

static int failures = 0;

static void check_vector(const struct vector *v)
{
    stressfs_prng_t prng;
    int i;

    stressfs_prng_seed(&prng, v->seed);
    for (i = 0; i < 8; i++) {
        uint32_t got = stressfs_prng_next(&prng);
        if (got != v->expect[i]) {
            fprintf(stderr,
                    "FAIL: seed=0x%08lx output[%d]: got 0x%08lx, want 0x%08lx\n",
                    (unsigned long)v->seed, i,
                    (unsigned long)got, (unsigned long)v->expect[i]);
            failures++;
        }
    }
}

static void check_reproducible(void)
{
    stressfs_prng_t a, b;
    int i;

    stressfs_prng_seed(&a, 0x1234u);
    stressfs_prng_seed(&b, 0x1234u);

    for (i = 0; i < 16; i++) {
        uint32_t ga = stressfs_prng_next(&a);
        uint32_t gb = stressfs_prng_next(&b);
        if (ga != gb) {
            fprintf(stderr,
                    "FAIL: same seed diverged at step %d: 0x%08lx vs 0x%08lx\n",
                    i, (unsigned long)ga, (unsigned long)gb);
            failures++;
        }
    }
}

static void check_below_in_range(void)
{
    stressfs_prng_t prng;
    int i;
    const uint32_t bound = 7u;

    stressfs_prng_seed(&prng, 99u);
    for (i = 0; i < 1000; i++) {
        uint32_t v = stressfs_prng_below(&prng, bound);
        if (v >= bound) {
            fprintf(stderr, "FAIL: stressfs_prng_below(%lu) returned %lu\n",
                    (unsigned long)bound, (unsigned long)v);
            failures++;
        }
    }
}

int main(void)
{
    size_t i;

    for (i = 0; i < sizeof(VECTORS) / sizeof(VECTORS[0]); i++) {
        check_vector(&VECTORS[i]);
    }
    check_reproducible();
    check_below_in_range();

    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    printf("prng_test: OK\n");
    return 0;
}
