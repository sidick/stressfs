/* stressfs/prng.h
 *
 * A fixed, documented, host-portable PRNG (docs/implementation-plan.md
 * section 1 and milestone M0). This is deliberately NOT "whatever libc
 * provides": the seed/PRNG lives in amiga/lib precisely because the
 * Amiga binary, the host ctest build and (eventually) phase 4's Rust
 * stressfs-drive must all reproduce the exact same operation stream
 * from the same seed. No Amiga dependencies, no floating point, no
 * platform-provided rand() — just portable C using fixed-width integer
 * types.
 *
 * Algorithm: xoshiro128** (Blackman & Vigna, public domain), the 32-bit
 * member of the xoshiro/xoroshiro family, chosen because its full
 * 32-bit state fits comfortably in the "4 MB comfortable target"
 * working-set budget the proposal sets for layer 2, and 32-bit output
 * is a natural fit for the offsets/sizes/choices this harness mostly
 * needs.
 *
 * Seeding rule (fixed, documented, and NOT allowed to change silently —
 * every tool reproducing a seed depends on it being exactly this):
 * xoshiro128**'s own authors warn its state must never be all-zero and
 * recommend seeding it from a different, well-mixed generator rather
 * than writing the seed bits directly into the four words. This
 * implementation seeds via SplitMix32 (the standard small-state mixer
 * used for the same purpose when seeding 64-bit xoshiro generators from
 * a 64-bit splitmix64): the 32-bit seed is fed into a SplitMix32
 * counter, and four successive SplitMix32 outputs become the initial
 * state words s[0]..s[3], in order. See prng.c for both algorithms in
 * full and known test vectors in amiga/tests/prng_test.c.
 */
#ifndef STRESSFS_PRNG_H
#define STRESSFS_PRNG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t s[4];
} stressfs_prng_t;

/* Seed a generator. Any 32-bit seed value is valid input, including 0
 * — the SplitMix32 expansion step guarantees the resulting four-word
 * state is never the all-zero state xoshiro128** must avoid. */
void stressfs_prng_seed(stressfs_prng_t *prng, uint32_t seed);

/* Return the next 32-bit output and advance the generator's state. */
uint32_t stressfs_prng_next(stressfs_prng_t *prng);

/* Return a value uniformly distributed in [0, bound), bound > 0, using
 * Lemire's rejection method so the result is not biased towards low
 * values the way a plain modulo would be for a bound that does not
 * evenly divide 2^32. */
uint32_t stressfs_prng_below(stressfs_prng_t *prng, uint32_t bound);

/* M4 (docs/implementation-plan.md): a direct, O(1), stateless hash of
 * three 32-bit inputs, built from the same SplitMix32 finalizer
 * prng.c's seeding step already uses. This is NOT the stream generator
 * above -- stressfs_prng_next() only runs forward one step at a time,
 * so it cannot answer "what is byte N of this file's content" without
 * replaying N steps, which is exactly the "stored" cost
 * docs/proposal.md's layer 2 rules out ("content generated from the
 * seed and offset rather than stored"). stressfs_prng_hash3() answers
 * that in one call: `random`'s model keys it with (run seed, a file's
 * stable id, byte offset / 4) to get that file's expected content at
 * any offset without ever holding the bytes in memory. Reusing the
 * existing mixing math here (rather than adding a second algorithm) is
 * a deliberate M4 decision -- see amiga/include/stressfs/random_model.h. */
uint32_t stressfs_prng_hash3(uint32_t a, uint32_t b, uint32_t c);

#ifdef __cplusplus
}
#endif

#endif /* STRESSFS_PRNG_H */
