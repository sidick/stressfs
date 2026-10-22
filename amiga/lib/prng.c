/* amiga/lib/prng.c — xoshiro128** with a SplitMix32 seeding rule.
 *
 * Portable C, no Amiga dependencies (see prng.h's header comment and
 * docs/implementation-plan.md section 1): this file compiles and is
 * unit-tested on the host, and cross-compiles unchanged for m68k.
 *
 * xoshiro128**: David Blackman and Sebastiano Vigna, public domain
 * (https://prng.di.unimi.it/). SplitMix32 is the 32-bit analogue of the
 * splitmix64 mixer the same authors suggest for seeding xoshiro/
 * xoroshiro generators from a smaller seed.
 */

#include "stressfs/prng.h"

static uint32_t rotl(uint32_t x, int k)
{
    return (x << k) | (x >> (32 - k));
}

/* SplitMix32: a simple, well-mixed counter-based generator used only to
 * expand the caller's 32-bit seed into xoshiro128**'s four-word state.
 * `state` is both the running counter and is updated in place. */
static uint32_t splitmix32_next(uint32_t *state)
{
    uint32_t z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

void stressfs_prng_seed(stressfs_prng_t *prng, uint32_t seed)
{
    uint32_t sm_state = seed;
    int i;

    for (i = 0; i < 4; i++) {
        prng->s[i] = splitmix32_next(&sm_state);
    }
}

uint32_t stressfs_prng_next(stressfs_prng_t *prng)
{
    uint32_t *s = prng->s;
    uint32_t const result = rotl(s[1] * 5u, 7) * 9u;
    uint32_t const t = s[1] << 9;

    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];

    s[2] ^= t;

    s[3] = rotl(s[3], 11);

    return result;
}

uint32_t stressfs_prng_below(stressfs_prng_t *prng, uint32_t bound)
{
    /* Lemire's method (debiased modulo via multiplication): reject and
     * redraw the low tail that would otherwise bias the result towards
     * small values when bound does not divide 2^32 evenly. */
    uint64_t m;
    uint32_t l;

    if (bound == 0) {
        return 0;
    }

    for (;;) {
        uint32_t const x = stressfs_prng_next(prng);
        m = (uint64_t)x * (uint64_t)bound;
        l = (uint32_t)m;
        if (l >= bound) {
            return (uint32_t)(m >> 32);
        }
        {
            uint32_t const threshold = (uint32_t)(-(int32_t)bound) % bound;
            if (l >= threshold) {
                return (uint32_t)(m >> 32);
            }
        }
    }
}
